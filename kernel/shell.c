#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "console.h"
#include "desktop.h"
#include "accounts.h"
#include "acpi_dump.h"
#include "i2c_hid.h"
#include "ahci.h"
#include "block.h"
#include "fat16.h"
#include "initramfs.h"
#include "installer.h"
#include "nvme.h"
#include "mouse.h"
#include "power.h"
#include "scheduler.h"
#include "storage.h"
#include "pci.h"
#include "shell.h"
#include "timer.h"
#include "user.h"
#include "vfs.h"
#include "keyboard.h"
#include "net.h"

#define SHELL_LINE_CAPACITY 2048
#define FAT16_DISPLAY_CAPACITY 1024
#define FAT16_EXECUTABLE_CAPACITY (1024 * 1024)
#define TIMER_TICKS_PER_SECOND 100

static char input_line[SHELL_LINE_CAPACITY];
static size_t input_length;
static bool input_overflow;
static uint8_t fat16_executable[FAT16_EXECUTABLE_CAPACITY];

static char ascii_lowercase(char character) {
    if (character >= 'A' && character <= 'Z') {
        return character + ('a' - 'A');
    }
    return character;
}

static bool command_matches(const char *command, size_t length, const char *expected) {
    size_t i = 0;
    while (i < length && expected[i] != '\0') {
        if (ascii_lowercase(command[i]) != expected[i]) {
            return false;
        }
        i++;
    }
    return i == length && expected[i] == '\0';
}

/*
 * The kernel shell can read and write any sector, so it is for
 * administrators only. With accounts on the disk it asks for an
 * administrator's name and password first, unless one is logged in.
 */
enum shell_lock {
    SHELL_OPEN,
    SHELL_ASK_NAME,
    SHELL_ASK_PASSWORD,
};

static enum shell_lock shell_lock;
static size_t shell_login_account;

static void print_prompt(void) {
    if (shell_lock == SHELL_ASK_NAME) {
        console_write_string("Administrator account: ");
        console_write_bytes(input_line, input_length);
        return;
    }
    if (shell_lock == SHELL_ASK_PASSWORD) {
        console_write_string("Password: ");
        for (size_t i = 0; i < input_length; i++) {
            console_write_string("*");
        }
        return;
    }
    console_write_string("opos> ");
    console_write_bytes(input_line, input_length);
}

static void shell_lock_update(void) {
    if (accounts_current() == ACCOUNT_NONE) {
        accounts_load();
    }
    size_t current = accounts_current();
    bool admin_present = current != ACCOUNT_NONE && accounts_is_admin(current);
    shell_lock = accounts_count() == 0 || admin_present
        ? SHELL_OPEN : SHELL_ASK_NAME;
    if (shell_lock != SHELL_OPEN) {
        console_write_string(
            "This computer has accounts. Log in as an administrator.\n");
    }
}

static void shell_login_line(void) {
    if (shell_lock == SHELL_ASK_NAME) {
        shell_login_account = accounts_find(input_line);
        shell_lock = SHELL_ASK_PASSWORD;
        return;
    }
    size_t account = shell_login_account;
    bool ok = account != ACCOUNT_NONE && accounts_is_admin(account)
        && accounts_login(account, input_line);
    if (ok) {
        shell_lock = SHELL_OPEN;
        console_write_string("OPOS shell ready. Type help for commands.\n");
        return;
    }
    /* Same reply for a wrong name, a standard account or a wrong
       password. */
    console_write_error("Login failed.\n");
    shell_lock = SHELL_ASK_NAME;
}

static void write_decimal(uint64_t value) {
    char digits[20];
    size_t length = 0;
    do {
        digits[length++] = '0' + value % 10;
        value /= 10;
    } while (value != 0);

    while (length != 0) {
        console_write_bytes(&digits[--length], 1);
    }
}

static bool parse_sector_number(const char *text, size_t length,
                                uint32_t *sector) {
    if (length == 0 || sector == NULL) {
        return false;
    }

    uint32_t value = 0;
    for (size_t i = 0; i < length; i++) {
        if (text[i] < '0' || text[i] > '9') {
            return false;
        }
        uint32_t digit = (uint32_t)(text[i] - '0');
        if (value > (UINT32_MAX - digit) / 10) {
            return false;
        }
        value = value * 10 + digit;
    }
    *sector = value;
    return true;
}

static bool disk_path(const char *name, size_t name_length,
                      char *path, size_t path_capacity, size_t *path_length) {
    static const char prefix[] = "/disk/";
    if (name == NULL || path == NULL || path_length == NULL
        || name_length == 0
        || sizeof(prefix) - 1 + name_length > path_capacity) {
        return false;
    }
    /* An absolute path ("/disk2/A.TXT") is used as it is. */
    if (name[0] == '/') {
        for (size_t i = 0; i < name_length; i++) {
            path[i] = name[i];
        }
        *path_length = name_length;
        return true;
    }
    for (size_t i = 0; i < sizeof(prefix) - 1; i++) {
        path[i] = prefix[i];
    }
    for (size_t i = 0; i < name_length; i++) {
        path[sizeof(prefix) - 1 + i] = name[i];
    }
    *path_length = sizeof(prefix) - 1 + name_length;
    return true;
}

static void write_hex_byte(uint8_t value) {
    static const char digits[] = "0123456789abcdef";
    char output[2] = {digits[value >> 4], digits[value & 0x0f]};
    console_write_bytes(output, sizeof(output));
}

static void disk_run_executable(const char *text, size_t length) {
    char path[32];
    size_t path_length;
    size_t file_size = 0;
    if (!disk_path(text, length, path, sizeof(path), &path_length)) {
        console_write_warning("Usage: disk run NAME.ELF\n");
        return;
    }
    if (!vfs_read_file(path, path_length, fat16_executable,
                       sizeof(fat16_executable), &file_size)) {
        if (file_size > sizeof(fat16_executable)) {
            console_write_string("Executable exceeds the 1 MiB load limit.\n");
        } else {
            console_write_error("FAT16 executable could not be read.\n");
        }
        return;
    }

    uint64_t process_id;
    uint64_t exit_status;
    if (!user_process_run_elf_path(fat16_executable, file_size,
                                   path, path_length,
                                   &process_id, &exit_status)) {
        console_write_error("File is not a supported executable or failed to start.\n");
        return;
    }
    console_write_string("Disk application process ");
    write_decimal(process_id);
    console_write_string(" exited with status ");
    write_decimal(exit_status);
    console_write_string("\n");
}

static void disk_list_directory(const char *text, size_t length) {
    char path[32];
    size_t path_length;
    if (length == 0) {
        static const char root[] = "/disk";
        for (size_t i = 0; i < sizeof(root) - 1; i++) {
            path[i] = root[i];
        }
        path_length = sizeof(root) - 1;
    } else if (!disk_path(text, length, path, sizeof(path), &path_length)) {
        console_write_warning("Usage: disk ls [PATH]\n");
        return;
    }

    size_t count;
    if (!vfs_directory_count(path, path_length, &count)) {
        console_write_error("FAT16 directory could not be read.\n");
        return;
    }
    for (size_t i = 0; i < count; i++) {
        struct vfs_directory_entry entry;
        if (!vfs_directory_entry_at(path, path_length, i, &entry)) {
            console_write_error("FAT16 directory read failed.\n");
            return;
        }
        console_write_bytes(entry.name, entry.name_length);
        if (entry.type == VFS_NODE_DIRECTORY) {
            console_write_string("/\n");
        } else {
            console_write_string(" (");
            write_decimal(entry.size);
            console_write_string(" bytes)\n");
        }
    }
}

static void write_disk_size(uint64_t sectors) {
    if (sectors >= 2ULL * 1024 * 1024 * 10) {
        write_decimal(sectors / (2ULL * 1024 * 1024));
        console_write_string(" GiB");
    } else {
        write_decimal(sectors / 2048);
        console_write_string(" MiB");
    }
}

static const char *fat_type_name(enum fat_volume_type type) {
    return type == FAT_VOLUME_FAT32 ? "FAT32" : "FAT16";
}

static void report_disk(void) {
    uint64_t sectors = block_sector_count();
    if (sectors == 0) {
        console_write_error("No readable disk found.\n");
        return;
    }
    console_write_string("Disk: ");
    console_write_string(block_device_name(block_active_device()));
    console_write_string(", ");
    write_decimal(sectors);
    console_write_string(" sectors (");
    write_disk_size(sectors);
    console_write_string(")\n");
    if (fat16_system_mounted()) {
        console_write_string("Filesystem: ");
        console_write_string(fat_type_name(fat16_volume_type()));
        console_write_string(" volume of ");
        write_disk_size(fat16_volume_sectors());
        console_write_string(" mounted at /disk\n");
        return;
    }
    console_write_string("Filesystem: none - ");
    console_write_string(fat16_disk_state_description(fat16_disk_state()));
    console_write_string("\nType 'disk format' to create a partition and FAT filesystem.\n");
}

/* 'disk format' only describes what would happen. Formatting a blank disk
   needs 'disk format yes'; a disk holding any data needs the longer
   'disk format erase-all-data', so a stray confirmation cannot wipe it. */
static void disk_format(const char *confirmation, size_t length) {
    uint64_t sectors = block_sector_count();
    enum fat_volume_type type;
    uint32_t partition_sectors;
    if (sectors == 0) {
        console_write_error("No readable disk found.\n");
        return;
    }
    if (!fat16_format_plan(sectors, &type, &partition_sectors)) {
        console_write_error("Disk is too small to format (minimum 9 MiB).\n");
        return;
    }
    bool blank = !fat16_system_mounted() && fat16_disk_state() == FAT_DISK_BLANK;
    const char *required = blank ? "yes" : "erase-all-data";
    if (length == 0) {
        console_write_string("disk format writes a new MBR partition table with one ");
        write_disk_size(partition_sectors);
        console_write_string(" ");
        console_write_string(fat_type_name(type));
        console_write_string(" partition to the ");
        write_disk_size(sectors);
        console_write_string(" disk (");
        console_write_string(block_device_name(block_active_device()));
        console_write_string(").\n");
        if (!blank) {
            console_write_warning("WARNING: the disk is not blank; ALL data on it will be erased.\n");
        }
        console_write_string("To continue, type: disk format ");
        console_write_string(required);
        console_write_string("\n");
        return;
    }
    if (!command_matches(confirmation, length, required)) {
        console_write_error("Confirmation does not match; type 'disk format' for instructions.\n");
        return;
    }
    if (vfs_disk_objects_open()) {
        console_write_string("Files on /disk are open; close them before formatting.\n");
        return;
    }
    console_write_string("Formatting disk...\n");
    fat16_unmount_others(BLOCK_NO_DISK);
    bool formatted = fat16_format_disk();
    fat16_mount_others();
    if (!formatted) {
        console_write_error("Format failed; the disk may be unwritable.\n");
        return;
    }
    /* The new volume has no accounts; end any session from the old one. */
    accounts_load();
    console_write_string("Disk formatted as ");
    console_write_string(fat_type_name(fat16_volume_type()));
    console_write_string(" and mounted at /disk.\n");
}

static void shell_install_progress(const char *step, unsigned percent) {
    console_write_string("  ");
    write_decimal(percent);
    console_write_string("% ");
    console_write_string(step);
    console_write_string("\n");
}

static void write_install_disk(size_t index) {
    struct block_disk_info info;
    if (!block_disk_info(index, &info)) {
        return;
    }
    console_write_string("  ");
    write_decimal(index + 1);
    console_write_string(": ");
    write_disk_size(info.sectors);
    console_write_string(" ");
    console_write_string(info.location);
    if (info.model[0] != '\0') {
        console_write_string(" (");
        console_write_string(info.model);
        console_write_string(")");
    }
    struct installer_disk_summary summary;
    if (!installer_disk_summary(index, &summary)) {
        console_write_string(", unreadable");
    } else if (summary.blank) {
        console_write_string(", empty");
    } else if (summary.gpt) {
        console_write_string(", GPT with ");
        write_decimal(summary.partition_count);
        console_write_string(" partition(s)");
    } else if (summary.whole_disk_fat) {
        console_write_string(", FAT filesystem");
    } else if (summary.partition_count != 0) {
        console_write_string(", MBR with ");
        write_decimal(summary.partition_count);
        console_write_string(" partition(s)");
    } else {
        console_write_string(", unknown data");
    }
    const char *unusable = installer_disk_unusable(index);
    if (unusable != NULL) {
        console_write_warning(" - cannot install: ");
        console_write_warning(unusable);
    }
    console_write_string("\n");
}

/* install: copies OPOS from the live media onto a chosen disk.
   'install' lists the disks; 'install N erase-all-data' installs onto
   disk N. With one usable disk, 'install erase-all-data' also works. */
static void shell_install(const char *arguments, size_t length) {
    if (!installer_available()) {
        console_write_error("OPOS is not running from installation media.\n");
        return;
    }
    size_t usable = 0;
    size_t only_usable = 0;
    for (size_t i = 0; i < block_disk_count(); i++) {
        if (installer_disk_unusable(i) == NULL) {
            usable++;
            only_usable = i;
        }
    }
    if (block_disk_count() == 0) {
        console_write_error("No readable disk found.\n");
        return;
    }
    if (length == 0) {
        console_write_string("Disks:\n");
        for (size_t i = 0; i < block_disk_count(); i++) {
            write_install_disk(i);
        }
        console_write_warning("WARNING: ALL data on the chosen disk will be erased.\n");
        console_write_string("To continue, type: install N erase-all-data\n");
        return;
    }

    size_t disk = BLOCK_NO_DISK;
    const char *confirmation = arguments;
    size_t confirmation_length = length;
    if (arguments[0] >= '0' && arguments[0] <= '9') {
        size_t number = 0;
        size_t i = 0;
        while (i < length && arguments[i] >= '0' && arguments[i] <= '9'
               && number < 100) {
            number = number * 10 + (size_t)(arguments[i] - '0');
            i++;
        }
        if (i >= length || arguments[i] != ' ' || number == 0
            || number > block_disk_count()) {
            console_write_error("Unknown disk number; type 'install' for the list.\n");
            return;
        }
        disk = number - 1;
        confirmation = arguments + i + 1;
        confirmation_length = length - i - 1;
    } else if (usable == 1) {
        disk = only_usable;
    } else {
        console_write_error("More than one disk: type 'install' and choose a disk number.\n");
        return;
    }
    if (!command_matches(confirmation, confirmation_length, "erase-all-data")) {
        console_write_error("Confirmation does not match; type 'install' for instructions.\n");
        return;
    }
    const char *error = "";
    console_write_string("Installing OPOS on disk ");
    write_decimal(disk + 1);
    console_write_string("...\n");
    if (!installer_install(disk, shell_install_progress, &error)) {
        console_write_error("Installation failed: ");
        console_write_error(error);
        console_write_error("\n");
        return;
    }
    /* /disk is the new installation, which has no accounts yet. */
    accounts_load();
    console_write_string("OPOS is installed. Remove the installation media and reboot.\n");
}

static void shell_print_line(const char *line) {
    console_write_string(line);
    console_write_string("\n");
}

/* Prints a ping or lookup as it runs; Ctrl+C or Esc stops it. */
static void shell_net_wait(bool started) {
    char line[128];
    while (started && net_session_active()) {
        while (net_take_line(line, sizeof(line))) {
            shell_print_line(line);
        }
        char character;
        while (keyboard_read_character(&character)) {
            if (character == KEY_ESCAPE
                || (keyboard_last_ctrl()
                    && (character == 'c' || character == 'C'))) {
                console_write_string("^C\n");
                net_session_stop();
            }
        }
        console_cursor_refresh();
        __asm__ volatile ("hlt" : : : "memory");
        net_poll();
    }
    while (net_take_line(line, sizeof(line))) {
        shell_print_line(line);
    }
}

/* Whether /bin/NAME exists, for a NAME of letters, digits, '-' and '_'. */
static bool bin_program_exists(const char *name, size_t length) {
    char path[48] = "bin/";
    if (length == 0 || length + 5 > sizeof(path)) {
        return false;
    }
    for (size_t i = 0; i < length; i++) {
        char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
              || (c >= '0' && c <= '9') || c == '-' || c == '_')) {
            return false;
        }
        path[4 + i] = c;
    }
    path[4 + length] = '\0';
    struct initramfs_file file;
    return initramfs_find_file(path, &file);
}

struct shell_help_entry {
    const char *name;
    const char *usage;
    const char *description;
};

static const struct shell_help_entry shell_help_entries[] = {
    {"help", "help [COMMAND]", "Lists commands or explains one command."},
    {"echo", "echo [TEXT]", "Prints text to the console."},
    {"uptime", "uptime", "Shows time since OPOS started."},
    {"clear", "clear", "Clears the console."},
    {"mouse", "mouse", "Shows PS/2 pointer and ACPI input diagnostics."},
    {"touchpad", "touchpad [mouse|multitouch]",
     "Shows touchpad diagnostics or changes its mode until restart."},
    {"pci", "pci", "Lists detected PCI devices."},
    {"desktop", "desktop", "Starts the graphical desktop."},
    {"storage", "storage", "Shows storage-controller diagnostics."},
    {"drives", "drives", "Lists mounted and detected storage volumes."},
    {"disk", "disk [format|read LBA|ls [PATH]|mkdir PATH|cat PATH|write PATH TEXT|run PATH]",
     "Inspects or changes the primary FAT disk."},
    {"ls", "ls [PATH]", "Lists initramfs files or a VFS directory."},
    {"sum", "sum PATH", "Shows file size, bytes read and an FNV-1a hash."},
    {"install", "install [DISK] erase-all-data",
     "Lists install targets or installs OPOS from live media."},
    {"reboot", "reboot", "Restarts the computer."},
    {"shutdown", "shutdown", "Turns off the computer through ACPI."},
    {"run", "run PROGRAM [ARGUMENTS]",
     "Runs a built-in test or an ELF program by path."},
    {"net", "net", "Shows network-interface and address status."},
    {"dhcp", "dhcp", "Restarts DHCP address configuration."},
    {"ping", "ping HOST", "Sends ICMP echo requests to a host."},
    {"lookup", "lookup NAME", "Resolves a DNS name."},
    {"http", "http HOST[/PATH]", "Fetches an HTTP resource for diagnostics."},
    {"schedtest", "schedtest", "Runs the scheduler self-test."},
    {"proctest", "proctest", "Runs process isolation and reuse tests."},
    {"multitest", "multitest", "Runs concurrent-process tests."},
    {"inputwaittest", "inputwaittest", "Tests blocking program input."},
    {"update", "update [check|install] [OPTIONS]",
     "Checks for or installs OPOS updates using /bin/update."},
    {"fetch", "fetch URL", "Downloads a URL using /bin/fetch."},
};

static void shell_command_help(const char *name, size_t length) {
    for (size_t i = 0;
         i < sizeof(shell_help_entries) / sizeof(shell_help_entries[0]); i++) {
        const struct shell_help_entry *entry = &shell_help_entries[i];
        if (!command_matches(name, length, entry->name)) {
            continue;
        }
        console_write_string("Usage: ");
        console_write_string(entry->usage);
        console_write_string("\n");
        console_write_string(entry->description);
        console_write_string("\n");
        return;
    }
    if (bin_program_exists(name, length)) {
        console_write_string("Usage: ");
        console_write_bytes(name, length);
        console_write_string(" [ARGUMENTS]\nRuns /bin/");
        console_write_bytes(name, length);
        console_write_string("; no detailed help is registered.\n");
        return;
    }
    console_write_warning("No help available for '");
    console_write_bytes(name, length);
    console_write_warning("'. Type help to list commands.\n");
}

static void execute_command(void) {
    /* USB sticks may have come or gone since the last command. */
    (void)storage_poll();
    char *command = input_line;
    size_t command_length = 0;
    while (command[command_length] == ' ') {
        command++;
    }
    while (command[command_length] != '\0' && command[command_length] != ' ') {
        command_length++;
    }

    char *arguments = command + command_length;
    while (*arguments == ' ') {
        arguments++;
    }
    size_t arguments_length = 0;
    while (arguments[arguments_length] != '\0') {
        arguments_length++;
    }
    if (command_length == 0) {
        return;
    }
    if (command_matches(command, command_length, "help")) {
        if (arguments_length != 0) {
            shell_command_help(arguments, arguments_length);
        } else {
            console_write_string("Commands: help, echo, uptime, clear, mouse, touchpad, pci, desktop, disk, "
                                 "storage, drives, ls, sum, install, reboot, shutdown, run, "
                                 "schedtest, proctest, multitest, inputwaittest, net, dhcp, "
                                 "ping, lookup, http, update, fetch (any program in /bin runs by name)\n"
                                 "Type help COMMAND for usage and an explanation.\n");
        }
    } else if (command_matches(command, command_length, "echo")) {
        console_write_string(arguments);
        console_write_string("\n");
    } else if (command_matches(command, command_length, "uptime")) {
        uint64_t ticks = timer_ticks();
        console_write_string("Uptime: ");
        write_decimal(ticks / TIMER_TICKS_PER_SECOND);
        console_write_string(" seconds (");
        console_write_hex(ticks);
        console_write_string(" ticks)\n");
    } else if (command_matches(command, command_length, "clear")) {
        console_clear();
    } else if (command_matches(command, command_length, "schedtest")) {
        console_write_string("Running two preempted kernel tasks...\n");
        console_write_string(scheduler_self_test()
                                 ? "Scheduler test passed.\n"
                                 : "Scheduler test failed.\n");
    } else if (command_matches(command, command_length, "proctest")) {
        console_write_string("Testing PCB fault isolation and resource reuse...\n");
        console_write_string("This test faults a process on purpose; one isolated page fault report is expected.\n");
        console_write_string(user_process_self_test()
                                 ? "Process control block test passed.\n"
                                 : "Process control block test failed.\n");
    } else if (command_matches(command, command_length, "multitest")) {
        console_write_string("Running two concurrent ring-3 processes...\n");
        console_write_string(user_process_concurrent_self_test()
                                 ? "Concurrent process test passed.\n"
                                 : "Concurrent process test failed.\n");
    } else if (command_matches(command, command_length, "inputwaittest")) {
        console_write_string("Type a line after the ring-3 input prompt.\n");
        console_write_string(user_process_input_wait_self_test()
                                 ? "Input wait-queue test passed.\n"
                                 : "Input wait-queue test failed.\n");
    } else if (command_matches(command, command_length, "mouse")) {
        struct mouse_state state;
        if (!mouse_read_state(&state)) {
            console_write_warning("PS/2 mouse unavailable: ");
            console_write_warning(mouse_failure_reason());
            console_write_warning("\n");
        } else {
            console_write_string("Pointer: x=");
            write_decimal((uint64_t)state.x);
            console_write_string(" y=");
            write_decimal((uint64_t)state.y);
            console_write_string(" buttons=");
            write_decimal(state.buttons);
            console_write_string("\n");
        }
        uint8_t device_id;
        if (mouse_reset_info(&device_id)) {
            console_write_string("PS/2 device ID: ");
            console_write_hex(device_id);
            console_write_string("\n");
        } else {
            console_write_string("PS/2 device did not answer a reset.\n");
        }
        /* What the firmware says is there, to tell a PS/2 touchpad from
           an I2C one that OPOS has no driver for yet. */
        struct power_input_report input;
        power_scan_input_devices(&input);
        console_write_string("ACPI tables scanned: ");
        write_decimal(input.tables_scanned);
        console_write_string("\n8042 controller flag: ");
        console_write_string(!input.boot_flags_valid ? "not reported"
                             : input.has_8042 ? "yes" : "no");
        console_write_string("\nACPI PS/2 keyboard entries: ");
        write_decimal(input.ps2_keyboard);
        console_write_string("\nACPI PS/2 mouse entries: ");
        write_decimal(input.ps2_mouse);
        console_write_string("\nACPI I2C HID entries: ");
        write_decimal(input.i2c_hid);
        console_write_string("\nTouchpad IDs:");
        if (input.touchpad_hid_count == 0) {
            console_write_string(" none");
        }
        for (size_t i = 0; i < input.touchpad_hid_count; i++) {
            console_write_string(" ");
            console_write_string(input.touchpad_hids[i]);
        }
        console_write_string("\n");
    } else if (command_matches(command, command_length, "touchpad")) {
        if (arguments_length == 0) {
            acpi_dump_touchpad();
            i2c_hid_write_status();
        } else if (command_matches(arguments, arguments_length, "mouse")
                   || command_matches(arguments, arguments_length,
                                      "multitouch")) {
            /* Until the next boot: plain mouse mode, or multi-touch. */
            bool multitouch = arguments_length == sizeof("multitouch") - 1;
            if (i2c_hid_set_multitouch(multitouch)) {
                console_write_string(multitouch
                                     ? "Touchpad: multi-touch mode.\n"
                                     : "Touchpad: mouse mode.\n");
            } else {
                console_write_warning(
                    "The touchpad cannot switch to that mode.\n");
            }
        } else {
            console_write_warning("Usage: touchpad [mouse|multitouch]\n");
        }
    } else if (command_matches(command, command_length, "net")) {
        net_describe(shell_print_line);
    } else if (command_matches(command, command_length, "dhcp")) {
        net_dhcp_restart();
        console_write_string("Asking the DHCP server for an address...\n");
    } else if (command_matches(command, command_length, "http")) {
        shell_net_wait(net_http_start(arguments, arguments_length));
    } else if (command_matches(command, command_length, "ping")
               || command_matches(command, command_length, "lookup")) {
        bool started = command_length == 4
                           ? net_ping_start(arguments, arguments_length)
                           : net_lookup_start(arguments, arguments_length);
        shell_net_wait(started);
    } else if (command_matches(command, command_length, "pci")) {
        acpi_dump_pci();
    } else if (command_matches(command, command_length, "desktop")) {
        desktop_run();
    } else if (command_matches(command, command_length, "drives")) {
        static struct storage_volume drives[STORAGE_MAX_VOLUMES];
        size_t count = storage_list(drives, STORAGE_MAX_VOLUMES);
        if (count == 0) {
            console_write_warning("No disks found.\n");
        }
        for (size_t i = 0; i < count; i++) {
            char size[16];
            storage_format_size(drives[i].sectors, size, sizeof(size));
            console_write_string(drives[i].optical ? "cd/dvd " : "disk ");
            write_decimal(drives[i].disk + 1);
            if (!drives[i].optical) {
                console_write_string(" part ");
                write_decimal(drives[i].partition);
            }
            if (drives[i].removable) {
                console_write_string(drives[i].sd_card ? " (sd card)" : " (usb)");
            }
            console_write_string(": ");
            console_write_string(storage_filesystem_name(drives[i].filesystem));
            console_write_string(", ");
            console_write_string(size);
            if (drives[i].label[0] != '\0') {
                console_write_string(", label '");
                console_write_string(drives[i].label);
                console_write_string("'");
            }
            if (drives[i].path[0] != 0) {
                console_write_string(", at ");
                console_write_string(drives[i].path);
            }
            console_write_string("\n");
        }
    } else if (command_matches(command, command_length, "storage")) {
        struct pci_storage_controllers controllers;
        struct ahci_status ahci;
        pci_get_storage_controllers(&controllers);
        ahci_get_status(&ahci);
        if (controllers.ahci_count == 0 && controllers.nvme_count == 0) {
            console_write_error("No AHCI or NVMe PCI controller found.\n");
        } else {
            if (controllers.ahci_count != 0) {
                console_write_string("AHCI controllers: ");
                write_decimal(controllers.ahci_count);
                console_write_string(" (first at ");
                write_decimal(controllers.ahci_bus);
                console_write_string(":");
                write_decimal(controllers.ahci_device);
                console_write_string(".");
                write_decimal(controllers.ahci_function);
                console_write_string(", BAR5 0x");
                console_write_hex(controllers.ahci_bar5);
                console_write_string(")\n");
                console_write_string("Attached SATA devices: ");
                write_decimal(ahci.sata_devices);
                console_write_string(", ATAPI devices: ");
                write_decimal(ahci.atapi_devices);
                console_write_string("\n");
                console_write_string("Readable AHCI SATA sectors: ");
                write_decimal(ahci_sector_count(0));
                console_write_string("\n");
            }
            if (controllers.nvme_count != 0) {
                console_write_string("NVMe controllers: ");
                write_decimal(controllers.nvme_count);
                console_write_string(" (first at ");
                write_decimal(controllers.nvme_bus);
                console_write_string(":");
                write_decimal(controllers.nvme_device);
                console_write_string(".");
                write_decimal(controllers.nvme_function);
                console_write_string(", BAR0 0x");
                console_write_hex(controllers.nvme_bar0);
                console_write_string(")\n");
            }
            if (controllers.nvme_count != 0) {
                struct nvme_status nvme;
                nvme_get_status(&nvme);
                if (!nvme.controller_ready) {
                    console_write_error("NVMe controller could not be initialized.\n");
                } else {
                    console_write_string("NVMe namespace ");
                    write_decimal(nvme.namespace_id);
                    console_write_string(": ");
                    write_decimal(nvme.namespace_blocks);
                    console_write_string(" blocks of ");
                    write_decimal(nvme.logical_block_size);
                    console_write_string(nvme.logical_block_size == 512
                        ? " bytes\n" : " bytes (unsupported block size)\n");
                }
            }
        }
        if (controllers.unsupported_count != 0) {
            console_write_string("Unsupported storage controllers: ");
            write_decimal(controllers.unsupported_count);
            console_write_string(" (first: PCI class 01 subclass ");
            write_hex_byte(controllers.unsupported_subclass);
            console_write_string(", ID ");
            write_hex_byte((uint8_t)(controllers.unsupported_vendor_id >> 8));
            write_hex_byte((uint8_t)controllers.unsupported_vendor_id);
            console_write_string(":");
            write_hex_byte((uint8_t)(controllers.unsupported_device_id >> 8));
            write_hex_byte((uint8_t)controllers.unsupported_device_id);
            console_write_string(")\n");
            console_write_error("SCSI/SAS disks (e.g. LSI Logic, PVSCSI) are not supported; attach the disk as SATA, NVMe, or IDE.\n");
        }
        report_disk();
    } else if (command_matches(command, command_length, "disk")) {
        uint64_t sectors = block_sector_count();
        if (arguments_length == 0) {
            report_disk();
        } else if (command_matches(arguments, arguments_length, "format")) {
            disk_format(arguments, 0);
        } else if (arguments_length > 7
                   && command_matches(arguments, 7, "format ")) {
            disk_format(arguments + 7, arguments_length - 7);
        } else if (arguments_length >= 5
                   && command_matches(arguments, 5, "read ")) {
            uint32_t sector_number;
            uint8_t data[512];
            if (sectors == 0) {
                console_write_error("No readable disk found.\n");
            } else if (!parse_sector_number(arguments + 5, arguments_length - 5,
                                     &sector_number)
                || !block_read_sectors(sector_number, 1, data)) {
                console_write_error("Disk sector could not be read.\n");
            } else {
                console_write_string("First 16 bytes: ");
                for (size_t i = 0; i < 16; i++) {
                    if (i != 0) {
                        console_write_string(" ");
                    }
                    write_hex_byte(data[i]);
                }
                console_write_string("\n");
            }
        } else if (arguments_length == 2
                   && command_matches(arguments, arguments_length, "ls")) {
            disk_list_directory(0, 0);
        } else if (arguments_length > 3
                   && command_matches(arguments, 3, "ls ")) {
            disk_list_directory(arguments + 3, arguments_length - 3);
        } else if (arguments_length > 6
                   && command_matches(arguments, 6, "mkdir ")) {
            char path[32];
            size_t path_length;
            if (!disk_path(arguments + 6, arguments_length - 6, path,
                           sizeof(path), &path_length)) {
                console_write_warning("Usage: disk mkdir PATH\n");
            } else if (!vfs_create_directory(path, path_length)) {
                console_write_error("Directory could not be created.\n");
            } else {
                console_write_string("Directory created.\n");
            }
        } else if (arguments_length > 6
                   && command_matches(arguments, 6, "write ")) {
            const char *file_and_text = arguments + 6;
            size_t file_and_text_length = arguments_length - 6;
            size_t name_length = 0;
            while (name_length < file_and_text_length
                   && file_and_text[name_length] != ' ') {
                name_length++;
            }
            char path[32];
            size_t path_length;
            const uint8_t *text = (const uint8_t *)file_and_text + name_length;
            size_t text_length = file_and_text_length - name_length;
            while (text_length != 0 && *text == ' ') {
                text++;
                text_length--;
            }
            uint8_t verify[FAT16_DISPLAY_CAPACITY];
            size_t verify_size = 0;
            if (name_length == file_and_text_length
                || !disk_path(file_and_text, name_length, path, sizeof(path),
                              &path_length)
                || text_length > sizeof(verify)) {
                console_write_warning("Usage: disk write NAME.TXT TEXT\n");
            } else if (!vfs_write_file(path, path_length, text, text_length)) {
                console_write_error("File could not be written; the FAT16 volume may be full or damaged.\n");
            } else if (!vfs_read_file(path, path_length, verify,
                                      sizeof(verify), &verify_size)
                       || verify_size != text_length) {
                console_write_error("Disk write verification failed.\n");
            } else {
                bool matches = true;
                for (size_t i = 0; i < text_length; i++) {
                    if (verify[i] != text[i]) {
                        matches = false;
                    }
                }
                console_write_string(matches
                    ? "Disk write verified.\n"
                    : "Disk write verification failed.\n");
            }
        } else if (arguments_length > 4
                   && command_matches(arguments, 4, "run ")) {
            disk_run_executable(arguments + 4, arguments_length - 4);
        } else if (arguments_length > 4
                   && command_matches(arguments, 4, "cat ")) {
            char path[32];
            size_t path_length;
            uint8_t contents[FAT16_DISPLAY_CAPACITY];
            size_t file_size = 0;
            if (!disk_path(arguments + 4, arguments_length - 4, path,
                           sizeof(path), &path_length)) {
                console_write_warning("Usage: disk cat NAME.TXT\n");
            } else if (!vfs_read_file(path, path_length, contents,
                                      sizeof(contents), &file_size)) {
                if (file_size > sizeof(contents)) {
                    console_write_string("File exceeds the 1024-byte display limit.\n");
                } else {
                    console_write_error("FAT16 file could not be read.\n");
                }
            } else {
                for (size_t i = 0; i < file_size; i++) {
                    uint8_t character = contents[i];
                    if (character == '\n' || character == '\r'
                        || character == '\t'
                        || (character >= 0x20 && character <= 0x7e)) {
                        char output = (char)character;
                        console_write_bytes(&output, 1);
                    } else {
                        console_write_string(".");
                    }
                }
                console_write_string("\n");
            }
        } else {
            console_write_warning("Usage: disk | disk format | disk read LBA | disk ls [PATH] | disk mkdir PATH | "
                                 "disk cat PATH | disk write PATH TEXT | disk run PATH\n");
        }
    } else if (command_matches(command, command_length, "sum")) {
        /* sum PATH: size and FNV-1a 64 of any VFS file (test reads). */
        uint64_t hash = 0xcbf29ce484222325ULL;
        size_t offset = 0;
        static uint8_t chunk[4096];
        size_t got = 0;
        while (vfs_read_at(arguments, arguments_length, offset, chunk,
                           sizeof(chunk), &got) && got != 0) {
            for (size_t i = 0; i < got; i++) {
                hash = (hash ^ chunk[i]) * 0x100000001b3ULL;
            }
            offset += got;
        }
        size_t size = 0;
        if (!vfs_file_size(arguments, arguments_length, &size)) {
            console_write_error("File not found.\n");
        } else {
            console_write_string("size ");
            write_decimal(size);
            console_write_string(" read ");
            write_decimal(offset);
            console_write_string(" fnv 0x");
            console_write_hex(hash);
            console_write_string("\n");
        }
    } else if (command_matches(command, command_length, "ls")
               && arguments_length != 0) {
        /* ls PATH: any VFS folder (/disk2, /cdrom, ...). */
        size_t count = 0;
        if (!vfs_directory_count(arguments, arguments_length, &count)) {
            console_write_error("Folder not found.\n");
            return;
        }
        for (size_t i = 0; i < count; i++) {
            struct vfs_directory_entry entry;
            if (!vfs_directory_entry_at(arguments, arguments_length, i,
                                        &entry)) {
                break;
            }
            console_write_string(entry.name);
            if (entry.type == VFS_NODE_DIRECTORY) {
                console_write_string("/\n");
            } else {
                console_write_string(" (");
                write_decimal(entry.size);
                console_write_string(" bytes)\n");
            }
        }
    } else if (command_matches(command, command_length, "ls")) {
        if (arguments_length != 0) {
            console_write_warning("Usage: ls [PATH]\n");
        } else {
            char path[256];
            size_t file_size;
            size_t count = initramfs_file_count();
            for (size_t i = 0; i < count; i++) {
                if (!initramfs_file_at(i, path, sizeof(path), &file_size)) {
                    console_write_error("Initramfs directory entry is invalid.\n");
                    return;
                }
                console_write_string("/");
                console_write_string(path);
                console_write_string(" (");
                write_decimal(file_size);
                console_write_string(" bytes)\n");
            }
        }
    } else if (command_matches(command, command_length, "install")) {
        shell_install(arguments, arguments_length);
    } else if (command_matches(command, command_length, "reboot")) {
        if (arguments_length != 0) {
            console_write_warning("Usage: reboot\n");
        } else {
            console_write_string("Restarting OPOS...\n");
            power_reboot();
        }
    } else if (command_matches(command, command_length, "shutdown")) {
        if (arguments_length != 0) {
            console_write_warning("Usage: shutdown\n");
        } else if (!power_shutdown()) {
            console_write_warning("ACPI shutdown is unavailable on this system.\n");
        }
    } else if (command_matches(command, command_length, "run")) {
        enum user_program program;
        struct initramfs_file file;
        bool run_file = false;
        const char *program_arguments = "";
        size_t program_arguments_length = 0;
        char path[256];
        size_t path_length = 0;
        if (command_matches(arguments, arguments_length, "demo")) {
            program = USER_PROGRAM_DEMO;
        } else if (command_matches(arguments, arguments_length, "fault")) {
            program = USER_PROGRAM_FAULT_TEST;
            console_write_string("Self-test: writing to read-only code. An isolated page fault (vector 0x0E, status 142) is the expected, passing result.\n");
        } else if (command_matches(arguments, arguments_length, "nx")) {
            program = USER_PROGRAM_NX_TEST;
            console_write_string("Self-test: executing from the non-executable stack. An isolated page fault (vector 0x0E, status 142) is the expected, passing result.\n");
        } else if (command_matches(arguments, arguments_length, "input")) {
            program = USER_PROGRAM_INPUT_TEST;
        } else if (command_matches(arguments, arguments_length, "user-shell")) {
            program = USER_PROGRAM_SHELL;
        } else if (command_matches(arguments, arguments_length, "elf")) {
            program = USER_PROGRAM_MODULE_ELF;
        } else if (command_matches(arguments, arguments_length, "linux-read")) {
            program = USER_PROGRAM_LINUX_READ_TEST;
        } else if (command_matches(arguments, arguments_length, "filetest")) {
            program = USER_PROGRAM_FILE_TEST;
        } else if (command_matches(arguments, arguments_length, "spawntest")) {
            program = USER_PROGRAM_SPAWN_TEST;
        } else if (arguments_length != 0) {
            size_t path_start = arguments[0] == '/' ? 1 : 0;
            /* "run PATH ARGS...": the rest of the line is argv[1...]. */
            size_t path_end = path_start;
            while (path_end < arguments_length && arguments[path_end] != ' ') {
                path_end++;
            }
            program_arguments = arguments + path_end;
            program_arguments_length = arguments_length - path_end;
            while (program_arguments_length != 0 && *program_arguments == ' ') {
                program_arguments++;
                program_arguments_length--;
            }
            path_length = path_end - path_start;
            if (path_length == 0) {
                console_write_warning("Usage: run PATH\n");
                return;
            }
            if (path_length > 254) {
                console_write_error("Application path is too long.\n");
                return;
            }
            /* path keeps the absolute form used for argv[0]/AT_EXECFN;
               the initramfs lookup takes it without the leading slash. */
            path[0] = '/';
            for (size_t i = 0; i < path_length; i++) {
                path[1 + i] = arguments[path_start + i];
            }
            path[1 + path_length] = '\0';
            if (!initramfs_find_file(path + 1, &file)) {
                console_write_error("Application file not found.\n");
                return;
            }
            run_file = true;
        } else {
            console_write_warning(
                "Usage: run demo | run fault | run nx | run input | run user-shell | run elf | run PATH | run linux-read | run filetest | run spawntest\n");
            return;
        }

        uint64_t process_id;
        uint64_t exit_status;
        console_write_string("Starting user process...\n");
        bool started = run_file
            ? user_process_run_elf_args(file.data, file.size,
                                        path, path_length + 1,
                                        program_arguments,
                                        program_arguments_length,
                                        &process_id, &exit_status)
            : user_process_run(program, &process_id, &exit_status);
        if (!started) {
            console_write_error("Process could not start or clean up.\n");
        } else {
            /* Non-zero status: yellow, since self-tests expect some. */
            if (exit_status != 0) {
                console_set_text_style(CONSOLE_TEXT_WARNING);
            }
            console_write_string("Process ");
            write_decimal(process_id);
            console_write_string(" exited with status ");
            write_decimal(exit_status);
            console_set_text_style(CONSOLE_TEXT_NORMAL);
            console_write_string("\n");
        }
    } else if (bin_program_exists(command, command_length)
               && 9 + command_length + 1 + arguments_length
                  < SHELL_LINE_CAPACITY) {
        /* "fetch URL" runs /bin/fetch: rewritten to "run /bin/fetch URL". */
        char line[SHELL_LINE_CAPACITY];
        size_t length = 0;
        static const char prefix[] = "run /bin/";
        for (size_t i = 0; prefix[i] != '\0'; i++) {
            line[length++] = prefix[i];
        }
        for (size_t i = 0; i < command_length; i++) {
            line[length++] = command[i];
        }
        line[length++] = ' ';
        for (size_t i = 0; i < arguments_length; i++) {
            line[length++] = arguments[i];
        }
        line[length] = '\0';
        for (size_t i = 0; i <= length; i++) {
            input_line[i] = line[i];
        }
        execute_command();
    } else {
        console_set_text_style(CONSOLE_TEXT_ERROR);
        console_write_string("Unknown command: ");
        console_write_bytes(command, command_length);
        console_set_text_style(CONSOLE_TEXT_NORMAL);
        console_write_string("\nType help for available commands.\n");
    }
}

void shell_initialize(void) {
    input_length = 0;
    input_overflow = false;
    input_line[0] = '\0';
    shell_lock_update();
    if (shell_lock == SHELL_OPEN) {
        console_write_string("OPOS shell ready. Type help for commands.\n");
    }
    print_prompt();
}

void shell_handle_character(char character) {
    if (character == '\n') {
        console_write_string("\n");
        if (input_overflow) {
            console_write_error("Input too long; command not run.\n");
        } else if (shell_lock != SHELL_OPEN) {
            shell_login_line();
        } else {
            bool desktop = command_matches(input_line, input_length,
                                           "desktop");
            execute_command();
            /* The desktop may have logged the administrator out. */
            if (desktop) {
                shell_lock_update();
            }
        }
        /* Do not keep a typed password in memory. */
        for (size_t i = 0; i < input_length; i++) {
            input_line[i] = '\0';
        }
        input_length = 0;
        input_overflow = false;
        input_line[0] = '\0';
        print_prompt();
        return;
    }

    if (character == '\b') {
        if (input_length != 0) {
            input_line[--input_length] = '\0';
            console_write_string("\b");
        }
        return;
    }

    if (character < ' ' || character > '~') {
        return;
    }
    if (input_length + 1 == SHELL_LINE_CAPACITY) {
        input_overflow = true;
        return;
    }

    input_line[input_length++] = character;
    input_line[input_length] = '\0';
    if (shell_lock == SHELL_ASK_PASSWORD) {
        console_write_string("*");
    } else {
        console_write_bytes(&character, 1);
    }
}

void shell_report_dropped_input(uint64_t count) {
    console_write_string("\nOPOS: keyboard input dropped 0x");
    console_write_hex(count);
    console_write_string("\n");
    print_prompt();
}
