.SUFFIXES:
.DELETE_ON_ERROR:

CC := gcc
MUSL_CC ?= x86_64-linux-musl-gcc
BUILD := build
LIMINE_VERSION := 12.9.6
LIMINE_SHA256 := b38aabb042b0c0f9f562b7741be3c1204838d7bbacfa78bb411c674bd5b984ee
LIMINE_PROTOCOL_COMMIT := 6e85ddd6f928b5b512015c4d47d1cfce2fd28079
LIMINE_DIR := $(BUILD)/limine-binary
LIMINE_HEADER := $(BUILD)/limine-protocol/include/limine.h
KERNEL := $(BUILD)/kernel.elf
ISO := $(BUILD)/opos.iso
USER_ELF := $(BUILD)/user-program.elf
EXEC_TEST_ELF := $(BUILD)/exec-test.elf
DYNAMIC_MAIN_ELF := $(BUILD)/dynamic-test.elf
DYNAMIC_PIE_MAIN_ELF := $(BUILD)/pie-test.elf
DYNAMIC_INTERPRETER_ELF := $(BUILD)/opos-ld.so
DYNAMIC_LIBRARY_ELF := $(BUILD)/libopos-test.so
DYNAMIC_BASE_LIBRARY_ELF := $(BUILD)/libopos-base.so
MUSL_TEST_ELF := $(BUILD)/musl-test.elf
MUSL_PTHREAD_TEST_ELF := $(BUILD)/musl-pthread-test.elf
MUSL_SIGNAL_TEST_ELF := $(BUILD)/musl-signal-test.elf
INITRAMFS := $(BUILD)/initramfs.tar
# Notepad is an ordinary SDK program (sdk/examples/notepad.c) shipped in
# the initramfs as /bin/notepad.
SDK_NOTEPAD := sdk/build/notepad
SDK_SOURCES := $(wildcard sdk/include/*.h sdk/lib/*.c sdk/src/*.S) sdk/opos.ld sdk/Makefile sdk/examples/notepad.c
INSTALL_TAR := $(BUILD)/install.tar
BIOS_BOOT := $(BUILD)/bios-boot.bin
TEST_DISK := $(BUILD)/fat16-test.img

CFLAGS := -std=gnu11 -O2 -Wall -Wextra \
	-ffreestanding -fno-builtin -fno-stack-protector \
	-fno-pic -fno-pie -m64 -mno-red-zone -mgeneral-regs-only -mcmodel=kernel
LDFLAGS := -nostdlib -static \
	-Wl,-z,max-page-size=0x1000 \
	-Wl,-z,noexecstack \
	-Wl,--build-id=none \
	-Wl,-T,kernel/linker.ld

.PHONY: all run test-disk persistence-test clean

all: $(ISO)

run: $(ISO)
	qemu-system-x86_64 -cdrom $(ISO) -display none -serial stdio -monitor none -no-reboot

test-disk: $(TEST_DISK)

persistence-test: $(ISO) $(TEST_DISK)
	$(BUILD)/mkfat16_test $(TEST_DISK)
	sh tools/test_fat16_persistence.sh $(ISO) $(TEST_DISK)

$(BUILD)/mkfat16_test: tools/mkfat16_test.c
	mkdir -p $(BUILD)
	$(CC) -std=c11 -O2 -Wall -Wextra $< -o $@

$(TEST_DISK): $(BUILD)/mkfat16_test
	$(BUILD)/mkfat16_test $@

$(BUILD)/limine-binary.tar.gz:
	mkdir -p $(BUILD)
	curl -fL "https://github.com/Limine-Bootloader/Limine/releases/download/v$(LIMINE_VERSION)/limine-binary.tar.gz" -o $@
	echo "$(LIMINE_SHA256)  $@" | sha256sum -c -

$(LIMINE_DIR)/limine.c: $(BUILD)/limine-binary.tar.gz
	tar -xzf $< -C $(BUILD)

$(LIMINE_DIR)/limine: $(LIMINE_DIR)/limine.c
	$(MAKE) -C $(LIMINE_DIR)

$(LIMINE_HEADER):
	mkdir -p $(dir $@)
	curl -fL "https://raw.githubusercontent.com/Limine-Bootloader/limine-protocol/$(LIMINE_PROTOCOL_COMMIT)/include/limine.h" -o $@

$(BUILD)/user-program.o: kernel/user_elf.S
	mkdir -p $(BUILD)
	$(CC) -m64 -ffreestanding -fno-pic -fno-pie -c $< -o $@

$(USER_ELF): $(BUILD)/user-program.o kernel/user_elf.ld
	ld -nostdlib -static -z max-page-size=0x1000 --build-id=none -T kernel/user_elf.ld -o $@ $(BUILD)/user-program.o

$(BUILD)/exec-test.o: kernel/user_exec_test.S
	mkdir -p $(BUILD)
	$(CC) -m64 -ffreestanding -fno-pic -fno-pie -c $< -o $@

$(EXEC_TEST_ELF): $(BUILD)/exec-test.o kernel/user_elf.ld
	ld -nostdlib -static -z max-page-size=0x1000 --build-id=none -T kernel/user_elf.ld -o $@ $(BUILD)/exec-test.o

$(BUILD)/dynamic-main.o: kernel/dynamic_main.S
	mkdir -p $(BUILD)
	$(CC) -m64 -ffreestanding -fno-pic -fno-pie -c $< -o $@

$(BUILD)/dynamic-library.o: kernel/dynamic_library.S
	mkdir -p $(BUILD)
	$(CC) -m64 -ffreestanding -fPIC -c $< -o $@

$(BUILD)/dynamic-tls.o: kernel/dynamic_tls.c
	mkdir -p $(BUILD)
	$(CC) -std=gnu11 -O2 -Wall -Wextra -ffreestanding -fno-builtin -fno-stack-protector -fPIC -m64 -mno-red-zone -mgeneral-regs-only -c $< -o $@

$(BUILD)/dynamic-tlsdesc.o: kernel/dynamic_tlsdesc.c
	mkdir -p $(BUILD)
	$(CC) -std=gnu11 -O2 -Wall -Wextra -ffreestanding -fno-builtin -fno-stack-protector -fPIC -m64 -mno-red-zone -mgeneral-regs-only -mtls-dialect=gnu2 -c $< -o $@

$(BUILD)/dynamic-base-library.o: kernel/dynamic_base_library.S
	mkdir -p $(BUILD)
	$(CC) -m64 -ffreestanding -fPIC -c $< -o $@

$(DYNAMIC_BASE_LIBRARY_ELF): $(BUILD)/dynamic-base-library.o kernel/dynamic_base_library.ld kernel/dynamic_base_versions.map
	ld -nostdlib -shared -Bsymbolic --hash-style=gnu --version-script=kernel/dynamic_base_versions.map -z max-page-size=0x1000 --build-id=none -T kernel/dynamic_base_library.ld -soname libopos-base.so -o $@ $(BUILD)/dynamic-base-library.o

$(DYNAMIC_LIBRARY_ELF): $(BUILD)/dynamic-library.o $(BUILD)/dynamic-tls.o $(BUILD)/dynamic-tlsdesc.o $(DYNAMIC_BASE_LIBRARY_ELF) kernel/dynamic_library.ld
	ld -nostdlib -shared -Bsymbolic --hash-style=sysv -z relro -z now -z max-page-size=0x1000 --build-id=none -T kernel/dynamic_library.ld -soname libopos-test.so -L$(BUILD) -o $@ $(BUILD)/dynamic-library.o $(BUILD)/dynamic-tls.o $(BUILD)/dynamic-tlsdesc.o -lopos-base

$(DYNAMIC_MAIN_ELF): $(BUILD)/dynamic-main.o $(DYNAMIC_LIBRARY_ELF) kernel/dynamic_main.ld
	ld -nostdlib -E --allow-shlib-undefined --hash-style=sysv -z max-page-size=0x1000 --build-id=none -dynamic-linker /lib/opos-ld.so -T kernel/dynamic_main.ld -L$(BUILD) -rpath-link $(BUILD) -o $@ $(BUILD)/dynamic-main.o -lopos-test

$(BUILD)/dynamic-pie-main.o: kernel/dynamic_pie_main.S
	mkdir -p $(BUILD)
	$(CC) -m64 -ffreestanding -fPIC -c $< -o $@

$(DYNAMIC_PIE_MAIN_ELF): $(BUILD)/dynamic-pie-main.o $(DYNAMIC_LIBRARY_ELF) kernel/dynamic_pie_main.ld
	ld -nostdlib -pie --allow-shlib-undefined --hash-style=sysv -z max-page-size=0x1000 --build-id=none -dynamic-linker /lib/opos-ld.so -T kernel/dynamic_pie_main.ld -L$(BUILD) -rpath-link $(BUILD) -o $@ $(BUILD)/dynamic-pie-main.o -lopos-test

$(BUILD)/dynamic-interpreter.o: kernel/dynamic_interpreter.S
	mkdir -p $(BUILD)
	$(CC) -m64 -ffreestanding -fPIC -c $< -o $@

$(BUILD)/dynamic-linker.o: kernel/dynamic_linker.c
	mkdir -p $(BUILD)
	$(CC) -std=gnu11 -O2 -Wall -Wextra -ffreestanding -fno-builtin -fno-stack-protector -fPIC -m64 -mno-red-zone -mgeneral-regs-only -c $< -o $@

$(DYNAMIC_INTERPRETER_ELF): $(BUILD)/dynamic-interpreter.o $(BUILD)/dynamic-linker.o kernel/dynamic_interpreter.ld
	ld -nostdlib -shared -Bsymbolic --hash-style=sysv -z max-page-size=0x1000 --build-id=none -T kernel/dynamic_interpreter.ld -o $@ $(BUILD)/dynamic-interpreter.o $(BUILD)/dynamic-linker.o

$(MUSL_TEST_ELF): kernel/musl_test.c
	mkdir -p $(BUILD)
	$(MUSL_CC) -static -Os -s -fno-stack-protector -Wl,--build-id=none $< -o $@

$(MUSL_PTHREAD_TEST_ELF): kernel/musl_pthread_test.c
	mkdir -p $(BUILD)
	$(MUSL_CC) -static -Os -s -fno-stack-protector -Wl,--build-id=none -pthread $< -o $@

$(MUSL_SIGNAL_TEST_ELF): kernel/musl_signal_test.c
	mkdir -p $(BUILD)
	$(MUSL_CC) -static -Os -s -fno-stack-protector -Wl,--build-id=none $< -o $@

$(SDK_NOTEPAD): $(SDK_SOURCES)
	$(MAKE) -C sdk build/notepad

$(INITRAMFS): $(SDK_NOTEPAD) $(USER_ELF) $(EXEC_TEST_ELF) $(DYNAMIC_MAIN_ELF) $(DYNAMIC_PIE_MAIN_ELF) $(DYNAMIC_INTERPRETER_ELF) $(DYNAMIC_LIBRARY_ELF) $(DYNAMIC_BASE_LIBRARY_ELF) $(MUSL_TEST_ELF) $(MUSL_PTHREAD_TEST_ELF) $(MUSL_SIGNAL_TEST_ELF)
	rm -rf $(BUILD)/initramfs_root
	mkdir -p $(BUILD)/initramfs_root/bin $(BUILD)/initramfs_root/lib
	cp $(USER_ELF) $(BUILD)/initramfs_root/bin/elf-test
	cp $(EXEC_TEST_ELF) $(BUILD)/initramfs_root/bin/exec-test
	cp $(DYNAMIC_MAIN_ELF) $(BUILD)/initramfs_root/bin/dynamic-test
	cp $(DYNAMIC_PIE_MAIN_ELF) $(BUILD)/initramfs_root/bin/pie-test
	cp $(DYNAMIC_INTERPRETER_ELF) $(BUILD)/initramfs_root/lib/opos-ld.so
	cp $(DYNAMIC_LIBRARY_ELF) $(BUILD)/initramfs_root/lib/libopos-test.so
	cp $(DYNAMIC_BASE_LIBRARY_ELF) $(BUILD)/initramfs_root/lib/libopos-base.so
	cp $(MUSL_TEST_ELF) $(BUILD)/initramfs_root/bin/musl-test
	cp $(MUSL_PTHREAD_TEST_ELF) $(BUILD)/initramfs_root/bin/musl-pthread-test
	cp $(MUSL_SIGNAL_TEST_ELF) $(BUILD)/initramfs_root/bin/musl-signal-test
	cp $(SDK_NOTEPAD) $(BUILD)/initramfs_root/bin/notepad
	tar --format=ustar -C $(BUILD)/initramfs_root -cf $@ bin lib

$(KERNEL): kernel/main.c kernel/interrupts.c kernel/interrupts.h kernel/interrupts.S kernel/io.h kernel/pci.c kernel/pci.h kernel/ahci.c kernel/ahci.h kernel/ata.c kernel/ata.h kernel/nvme.c kernel/nvme.h kernel/pvscsi.c kernel/pvscsi.h kernel/mpt.c kernel/mpt.h kernel/buslogic.c kernel/buslogic.h kernel/scsi.c kernel/scsi.h kernel/partition.c kernel/partition.h kernel/block.c kernel/block.h kernel/fat16.c kernel/fat16.h kernel/vfs.c kernel/vfs.h kernel/keyboard.c kernel/keyboard.h kernel/mouse.c kernel/mouse.h kernel/desktop.c kernel/desktop.h kernel/cmos.c kernel/cmos.h kernel/memory.c kernel/memory.h kernel/paging.c kernel/paging.h kernel/power.c kernel/power.h kernel/scheduler.c kernel/scheduler.h kernel/shell.c kernel/shell.h kernel/timer.c kernel/timer.h kernel/user.c kernel/user.h kernel/user.S kernel/elf.c kernel/elf.h kernel/initramfs.c kernel/initramfs.h kernel/console.h kernel/font.c kernel/font.h kernel/svga.c kernel/svga.h kernel/vmware.c kernel/vmware.h kernel/installer.c kernel/installer.h kernel/accounts.c kernel/accounts.h kernel/sha256.c kernel/sha256.h kernel/permissions.c kernel/permissions.h kernel/vbrstub.S kernel/linker.ld $(LIMINE_HEADER)
	$(CC) $(CFLAGS) -Ikernel -I$(dir $(LIMINE_HEADER)) $(LDFLAGS) -o $@ kernel/main.c kernel/interrupts.c kernel/interrupts.S kernel/pci.c kernel/ahci.c kernel/ata.c kernel/nvme.c kernel/pvscsi.c kernel/mpt.c kernel/buslogic.c kernel/scsi.c kernel/partition.c kernel/block.c kernel/fat16.c kernel/vfs.c kernel/keyboard.c kernel/mouse.c kernel/desktop.c kernel/cmos.c kernel/memory.c kernel/paging.c kernel/power.c kernel/scheduler.c kernel/shell.c kernel/timer.c kernel/user.c kernel/elf.c kernel/initramfs.c kernel/font.c kernel/svga.c kernel/vmware.c kernel/installer.c kernel/accounts.c kernel/sha256.c kernel/permissions.c kernel/user.S kernel/vbrstub.S

# BIOS boot sector + stage 2 for installed disks, as `limine bios-install`
# would lay them out on an MBR disk (see tools/mkbiosboot.c).
$(BUILD)/mkbiosboot: tools/mkbiosboot.c $(LIMINE_DIR)/limine
	$(CC) -std=c11 -O2 -Wall -Wextra -I$(LIMINE_DIR) $< -o $@

$(BIOS_BOOT): $(BUILD)/mkbiosboot
	$(BUILD)/mkbiosboot $(LIMINE_DIR)/limine-bios-hdd.h $@

# Files the installer copies besides the kernel and initramfs. Only the ISO
# carries this module; the kernel treats its presence as "live media".
$(INSTALL_TAR): $(BIOS_BOOT) $(LIMINE_DIR)/limine limine.conf
	rm -rf $(BUILD)/install_root
	mkdir -p $(BUILD)/install_root
	cp $(BIOS_BOOT) $(LIMINE_DIR)/limine-bios.sys $(LIMINE_DIR)/BOOTX64.EFI limine.conf $(BUILD)/install_root/
	tar --format=ustar -C $(BUILD)/install_root -cf $@ bios-boot.bin limine-bios.sys BOOTX64.EFI limine.conf
	rm -rf $(BUILD)/install_root

$(ISO): $(LIMINE_DIR)/limine $(KERNEL) $(USER_ELF) $(INITRAMFS) $(INSTALL_TAR) limine.conf
	rm -rf $(BUILD)/iso_root
	mkdir -p $(BUILD)/iso_root/boot/limine $(BUILD)/iso_root/EFI/BOOT
	cp $(KERNEL) $(BUILD)/iso_root/boot/kernel.elf
	cp $(INITRAMFS) $(BUILD)/iso_root/boot/initramfs.tar
	cp $(INSTALL_TAR) $(BUILD)/iso_root/boot/install.tar
	cp limine.conf $(BUILD)/iso_root/boot/limine/
	cp $(LIMINE_DIR)/limine-uefi-cd.bin $(BUILD)/iso_root/boot/limine/
	cp $(LIMINE_DIR)/limine-bios.sys $(LIMINE_DIR)/limine-bios-cd.bin $(BUILD)/iso_root/boot/limine/
	cp $(LIMINE_DIR)/BOOTX64.EFI $(LIMINE_DIR)/BOOTIA32.EFI $(BUILD)/iso_root/EFI/BOOT/
	xorriso -as mkisofs -R -r -J \
		-b boot/limine/limine-bios-cd.bin -no-emul-boot -boot-load-size 4 -boot-info-table \
		-hfsplus -apm-block-size 2048 \
		--efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		$(BUILD)/iso_root -o $@
	$(LIMINE_DIR)/limine bios-install $@
	rm -rf $(BUILD)/iso_root

clean:
	rm -rf $(BUILD)
	$(MAKE) -C sdk clean
