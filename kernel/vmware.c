#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "console.h"
#include "svga.h"
#include "timer.h"
#include "vmware.h"

/* Constants from open-vm-tools (backdoor_def.h, guest_msg_def.h) and the
   Linux vmmouse driver. */
#define BDOOR_MAGIC 0x564d5868u
#define BDOOR_PORT 0x5658
#define BDOOR_CMD_GETVERSION 10
#define BDOOR_CMD_MESSAGE 30
#define BDOOR_CMD_ABSPOINTER_DATA 39
#define BDOOR_CMD_ABSPOINTER_STATUS 40
#define BDOOR_CMD_ABSPOINTER_COMMAND 41

#define VMMOUSE_CMD_READ_ID 0x45414552u
#define VMMOUSE_CMD_REQUEST_ABSOLUTE 0x53424152u
#define VMMOUSE_VERSION_ID 0x3442554au
#define VMMOUSE_ERROR 0xffff0000u
#define VMMOUSE_RELATIVE_PACKET 0x00010000u
#define VMMOUSE_LEFT_BUTTON 0x20u
#define VMMOUSE_RIGHT_BUTTON 0x10u
#define VMMOUSE_MIDDLE_BUTTON 0x08u

#define MESSAGE_TYPE_OPEN 0
#define MESSAGE_TYPE_SENDSIZE 1
#define MESSAGE_TYPE_SENDPAYLOAD 2
#define MESSAGE_TYPE_RECVSIZE 3
#define MESSAGE_TYPE_RECVPAYLOAD 4
#define MESSAGE_TYPE_RECVSTATUS 5
#define MESSAGE_TYPE_CLOSE 6
#define MESSAGE_STATUS_SUCCESS 0x0001u
#define MESSAGE_STATUS_DORECV 0x0002u
#define GUESTMSG_FLAG_COOKIE 0x80000000u
#define RPCI_PROTOCOL 0x49435052u /* "RPCI": guest -> host requests */
#define TCLO_PROTOCOL 0x4f4c4354u /* "TCLO": host -> guest commands */

#define TCLO_POLL_TICKS 10 /* 100 ms at 100 Hz */
#define MESSAGE_CAPACITY 256

struct backdoor_registers {
    uint32_t eax, ebx, ecx, edx, esi, edi;
};

struct rpc_channel {
    bool open;
    uint16_t id;
    uint32_t cookie_high;
    uint32_t cookie_low;
};

static bool backdoor_present;
static bool mouse_active;
static struct rpc_channel tclo;
static char tclo_reply[MESSAGE_CAPACITY];
static size_t tclo_reply_length;
static uint64_t next_poll_tick;
/* Autofit: follow the VM window size sent by the host. */
static bool autofit_enabled;
static uint32_t host_width;
static uint32_t host_height;

static void backdoor(struct backdoor_registers *r) {
    __asm__ volatile ("inl %%dx, %%eax"
                      : "+a"(r->eax), "+b"(r->ebx), "+c"(r->ecx),
                        "+d"(r->edx), "+S"(r->esi), "+D"(r->edi)
                      : : "memory");
}

static uint32_t backdoor_command(uint32_t command, uint32_t argument,
                                 struct backdoor_registers *out) {
    struct backdoor_registers r = {BDOOR_MAGIC, argument, command,
                                   BDOOR_PORT, 0, 0};
    backdoor(&r);
    if (out != NULL) {
        *out = r;
    }
    return r.eax;
}

static bool running_under_hypervisor(void) {
    uint32_t eax = 1, ebx, ecx = 0, edx;
    __asm__ volatile ("cpuid" : "+a"(eax), "=b"(ebx), "+c"(ecx), "=d"(edx));
    return (ecx & (1u << 31)) != 0;
}

/* ------------------------------------------------------------ vmmouse */

static bool mouse_enable(void) {
    backdoor_command(BDOOR_CMD_ABSPOINTER_COMMAND, VMMOUSE_CMD_READ_ID, NULL);
    uint32_t status = 0;
    for (int i = 0; i < 1000; i++) {
        status = backdoor_command(BDOOR_CMD_ABSPOINTER_STATUS, 0, NULL);
        if ((status & 0xffffu) != 0) {
            break;
        }
    }
    if ((status & VMMOUSE_ERROR) == VMMOUSE_ERROR || (status & 0xffffu) == 0) {
        return false;
    }
    if (backdoor_command(BDOOR_CMD_ABSPOINTER_DATA, 1, NULL)
        != VMMOUSE_VERSION_ID) {
        return false;
    }
    backdoor_command(BDOOR_CMD_ABSPOINTER_COMMAND,
                     VMMOUSE_CMD_REQUEST_ABSOLUTE, NULL);
    return true;
}

bool vmware_mouse_read(uint32_t *x, uint32_t *y, uint8_t *buttons,
                       int32_t *wheel) {
    if (!mouse_active) {
        return false;
    }
    bool got = false;
    for (int packets = 0; packets < 64; packets++) {
        uint32_t status = backdoor_command(BDOOR_CMD_ABSPOINTER_STATUS, 0,
                                           NULL);
        if ((status & VMMOUSE_ERROR) == VMMOUSE_ERROR) {
            /* The host reset the device; ask for absolute mode again. */
            mouse_active = mouse_enable();
            return got;
        }
        if ((status & 0xffffu) < 4) {
            break;
        }
        struct backdoor_registers r;
        uint32_t flags = backdoor_command(BDOOR_CMD_ABSPOINTER_DATA, 4, &r);
        if ((flags & VMMOUSE_RELATIVE_PACKET) != 0) {
            continue;
        }
        /* edx: signed wheel motion, positive toward the user. */
        *wheel -= (int8_t)(uint8_t)r.edx;
        *x = r.ebx & 0xffffu;
        *y = r.ecx & 0xffffu;
        *buttons = (uint8_t)(((flags & VMMOUSE_LEFT_BUTTON) ? 1 : 0)
                             | ((flags & VMMOUSE_RIGHT_BUTTON) ? 2 : 0)
                             | ((flags & VMMOUSE_MIDDLE_BUTTON) ? 4 : 0));
        got = true;
    }
    return got;
}

bool vmware_mouse_active(void) {
    return mouse_active;
}

bool vmware_backdoor_present(void) {
    return backdoor_present;
}

/* ----------------------------------------------------------- GuestRPC */

static struct backdoor_registers message_call(const struct rpc_channel *ch,
                                              uint32_t type,
                                              uint32_t argument) {
    struct backdoor_registers r = {
        BDOOR_MAGIC, argument, (type << 16) | BDOOR_CMD_MESSAGE,
        ((uint32_t)ch->id << 16) | BDOOR_PORT, ch->cookie_high,
        ch->cookie_low,
    };
    backdoor(&r);
    return r;
}

static bool status_ok(const struct backdoor_registers *r) {
    return ((r->ecx >> 16) & MESSAGE_STATUS_SUCCESS) != 0;
}

static bool channel_open(struct rpc_channel *ch, uint32_t protocol) {
    struct backdoor_registers r = {
        BDOOR_MAGIC, protocol | GUESTMSG_FLAG_COOKIE,
        (MESSAGE_TYPE_OPEN << 16) | BDOOR_CMD_MESSAGE, BDOOR_PORT, 0, 0,
    };
    backdoor(&r);
    if (!status_ok(&r)) {
        ch->open = false;
        return false;
    }
    ch->id = (uint16_t)(r.edx >> 16);
    ch->cookie_high = r.esi;
    ch->cookie_low = r.edi;
    ch->open = true;
    return true;
}

static void channel_close(struct rpc_channel *ch) {
    if (ch->open) {
        (void)message_call(ch, MESSAGE_TYPE_CLOSE, 0);
    }
    ch->open = false;
}

static bool channel_send(struct rpc_channel *ch, const char *data,
                         size_t length) {
    struct backdoor_registers r =
        message_call(ch, MESSAGE_TYPE_SENDSIZE, (uint32_t)length);
    if (!status_ok(&r)) {
        return false;
    }
    for (size_t offset = 0; offset < length; offset += 4) {
        uint32_t word = 0;
        for (size_t i = 0; i < 4 && offset + i < length; i++) {
            word |= (uint32_t)(uint8_t)data[offset + i] << (8 * i);
        }
        r = message_call(ch, MESSAGE_TYPE_SENDPAYLOAD, word);
        if (!status_ok(&r)) {
            return false;
        }
    }
    return true;
}

/* Receives one message. *length is 0 if the host had nothing to say. */
static bool channel_receive(struct rpc_channel *ch, char *buffer,
                            size_t capacity, size_t *length) {
    *length = 0;
    struct backdoor_registers r = message_call(ch, MESSAGE_TYPE_RECVSIZE, 0);
    if (!status_ok(&r)) {
        return false;
    }
    if (((r.ecx >> 16) & MESSAGE_STATUS_DORECV) == 0) {
        return true;
    }
    if ((r.edx >> 16) != MESSAGE_TYPE_SENDSIZE) {
        return false;
    }
    size_t size = r.ebx;
    for (size_t offset = 0; offset < size; offset += 4) {
        r = message_call(ch, MESSAGE_TYPE_RECVPAYLOAD, MESSAGE_STATUS_SUCCESS);
        if (!status_ok(&r) || (r.edx >> 16) != MESSAGE_TYPE_SENDPAYLOAD) {
            return false;
        }
        for (size_t i = 0; i < 4 && offset + i < size; i++) {
            if (offset + i < capacity - 1) {
                buffer[offset + i] = (char)(r.ebx >> (8 * i));
            }
        }
    }
    r = message_call(ch, MESSAGE_TYPE_RECVSTATUS, MESSAGE_STATUS_SUCCESS);
    if (!status_ok(&r)) {
        return false;
    }
    *length = size < capacity - 1 ? size : capacity - 1;
    buffer[*length] = '\0';
    return true;
}

static size_t text_length(const char *text) {
    size_t length = 0;
    while (text[length] != '\0') {
        length++;
    }
    return length;
}

/* One guest -> host request on a short-lived RPCI channel. */
static bool rpci(const char *request) {
    struct rpc_channel ch = {0};
    if (!channel_open(&ch, RPCI_PROTOCOL)) {
        return false;
    }
    char reply[MESSAGE_CAPACITY];
    size_t length = 0;
    bool ok = channel_send(&ch, request, text_length(request))
           && channel_receive(&ch, reply, sizeof(reply), &length)
           && length >= 1 && reply[0] == '1';
    channel_close(&ch);
    return ok;
}

static void register_capabilities(void) {
    /* 0x7fffffff marks "unmanaged" tools, as distro open-vm-tools do. */
    (void)rpci("tools.set.version 2147483647");
    if (svga_available()) {
        /* Only advertise resizing while autofit is chosen, so VMware does
           not override a fixed resolution picked in Settings. */
        (void)rpci(autofit_enabled ? "tools.capability.resolution_set 1"
                                   : "tools.capability.resolution_set 0");
        (void)rpci("tools.capability.resolution_server toolbox 1");
        (void)rpci("tools.capability.resolution_min 640 480");
    }
}

static bool starts_with(const char *text, size_t length, const char *prefix) {
    size_t i = 0;
    while (prefix[i] != '\0') {
        if (i >= length || text[i] != prefix[i]) {
            return false;
        }
        i++;
    }
    return true;
}

static const char *parse_number(const char *text, const char *end,
                                uint32_t *value) {
    while (text < end && *text == ' ') {
        text++;
    }
    uint32_t number = 0;
    const char *start = text;
    while (text < end && *text >= '0' && *text <= '9' && number < 100000) {
        number = number * 10 + (uint32_t)(*text - '0');
        text++;
    }
    *value = number;
    return text == start ? NULL : text;
}

static void set_reply(const char *text) {
    size_t length = text_length(text);
    if (length >= sizeof(tclo_reply)) {
        length = sizeof(tclo_reply) - 1;
    }
    for (size_t i = 0; i < length; i++) {
        tclo_reply[i] = text[i];
    }
    tclo_reply[length] = '\0';
    tclo_reply_length = length;
}

static void handle_tclo_command(const char *command, size_t length) {
    if (starts_with(command, length, "reset")) {
        set_reply("OK ATR toolbox");
    } else if (starts_with(command, length, "ping")) {
        set_reply("OK ");
    } else if (starts_with(command, length, "Capabilities_Register")) {
        register_capabilities();
        set_reply("OK ");
    } else if (starts_with(command, length, "Resolution_Set")) {
        const char *end = command + length;
        uint32_t width = 0, height = 0;
        const char *p = parse_number(command + 14, end, &width);
        p = p == NULL ? NULL : parse_number(p, end, &height);
        bool ok = p != NULL && width != 0 && height != 0;
        if (ok) {
            host_width = width;
            host_height = height;
            if (autofit_enabled) {
                ok = svga_set_size(width, height);
            }
        }
        set_reply(ok ? "OK " : "ERROR Invalid resolution");
    } else if (starts_with(command, length, "Set_Option")) {
        set_reply("OK ");
    } else {
        set_reply("ERROR Unknown command");
    }
}

void vmware_tools_poll(void) {
    if (!tclo.open) {
        return;
    }
    uint64_t now = timer_ticks();
    if (now < next_poll_tick) {
        return;
    }
    next_poll_tick = now + TCLO_POLL_TICKS;

    /* Send the previous reply (or an empty message), then take at most a
       few commands per poll so a chatty host cannot stall the UI. */
    for (int round = 0; round < 4; round++) {
        char command[MESSAGE_CAPACITY];
        size_t length = 0;
        if (!channel_send(&tclo, tclo_reply, tclo_reply_length)
            || !channel_receive(&tclo, command, sizeof(command), &length)) {
            /* Channel broke (e.g. VM suspended); reopen next poll. */
            channel_close(&tclo);
            (void)channel_open(&tclo, TCLO_PROTOCOL);
            set_reply("");
            return;
        }
        set_reply("");
        if (length == 0) {
            return;
        }
        handle_tclo_command(command, length);
    }
}

bool vmware_autofit_available(void) {
    return tclo.open && svga_available();
}

bool vmware_autofit_enabled(void) {
    return autofit_enabled;
}

void vmware_set_autofit(bool enabled) {
    autofit_enabled = enabled && vmware_autofit_available();
    if (!tclo.open) {
        return;
    }
    (void)rpci(autofit_enabled ? "tools.capability.resolution_set 1"
                               : "tools.capability.resolution_set 0");
    if (autofit_enabled && host_width != 0 && host_height != 0) {
        (void)svga_set_size(host_width, host_height);
    }
}

void vmware_initialize(void) {
    backdoor_present = false;
    mouse_active = false;
    tclo.open = false;
    if (!running_under_hypervisor()) {
        return;
    }
    struct backdoor_registers r;
    backdoor_command(BDOOR_CMD_GETVERSION, ~BDOOR_MAGIC, &r);
    if (r.ebx != BDOOR_MAGIC) {
        return;
    }
    backdoor_present = true;
    mouse_active = mouse_enable();
    if (channel_open(&tclo, TCLO_PROTOCOL)) {
        register_capabilities();
        set_reply("");
    }
}
