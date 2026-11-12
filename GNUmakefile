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
MUSL_NET_TEST_ELF := $(BUILD)/musl-net-test.elf
INITRAMFS := $(BUILD)/initramfs.tar
# Notepad is an ordinary SDK program (sdk/examples/notepad.c) shipped in
# the initramfs as /bin/notepad.
SDK_NOTEPAD := sdk/build/notepad
SDK_FETCH := sdk/build/fetch
SDK_UPDATE := sdk/build/update
SDK_PKG := sdk/build/pkg
SDK_PLAY := sdk/build/play
SDK_FONTTEST := sdk/build/fonttest
SDK_BROWSER := sdk/build/browser
FONTS := $(wildcard data/fonts/*.ttf data/fonts/OFL.txt)
SDK_SOURCES := $(wildcard sdk/include/*.h sdk/lib/*.c sdk/src/*.S) sdk/opos.ld sdk/Makefile sdk/examples/notepad.c sdk/examples/fetch.c sdk/examples/update.c sdk/examples/pkg.c sdk/examples/play.c sdk/lib/image.c sdk/stb/stb_truetype.h sdk/examples/fonttest.c $(wildcard sdk/browser/*.c sdk/browser/*.h) sdk/include/opos_tls.h sdk/include/opos_image.h
# Bundled backgrounds (rendered by tools/mkwallpapers.py), shipped in the
# initramfs as /share/wallpapers.
WALLPAPERS := $(wildcard data/wallpapers/*.png)
CA_BUNDLE := data/ssl/ca-certificates.crt
INSTALL_TAR := $(BUILD)/install.tar
BIOS_BOOT := $(BUILD)/bios-boot.bin
TEST_DISK := $(BUILD)/fat16-test.img
# The OPOS version (VERSION file); updates must have a higher one.
OPOS_VERSION := $(shell cat VERSION)
RELEASE := $(BUILD)/release
UPDATE_KEY ?= $(HOME)/.opos-signing/update-key.pem
# The OPOS Store (store/README.md): packages built from the SDK examples.
STORE := $(BUILD)/store
STORE_KEY ?= $(HOME)/.opos-signing/store-key.pem
STORE_PACKAGES := sdk/examples/hello.pkg sdk/examples/sketch.pkg
# Linux programs built from pinned sources by store/linux/<name>/build.sh
# (docs/linux-programs.md). Sources are downloaded once into
# STORE_SOURCES. LINUX_CC is a static musl compiler: the musl files in
# build/musl-local with build/musl-local.specs when present, else the
# distribution's musl-gcc (Ubuntu/Debian: apt install musl-tools).
STORE_LINUX ?= lua sqlite
STORE_SOURCES ?= $(BUILD)/store_sources
MUSL_SPECS := $(BUILD)/musl-local.specs
LINUX_CC ?= $(if $(wildcard $(MUSL_SPECS)),gcc -specs $(abspath $(MUSL_SPECS)),musl-gcc)
MUSL_LICENSE ?= $(firstword $(wildcard $(BUILD)/musl-local/usr/share/doc/musl/copyright /usr/share/doc/musl/copyright))
# musl's shared libc, which is also its dynamic linker. OPOS ships it as
# /lib/ld-musl-x86_64.so.1 so dynamically linked musl programs run
# (docs/linux-programs.md, step 6). Without it the image still builds.
MUSL_LIBC_SO ?= $(firstword $(wildcard $(BUILD)/musl-local/lib/x86_64-linux-musl/libc.so /usr/lib/x86_64-linux-musl/libc.so /usr/lib/musl/lib/libc.so))
# BearSSL files the kernel uses to check update signatures (ECDSA P-256).
KERNEL_BEARSSL_SOURCES := $(addprefix sdk/bearssl/src/,codec/ccopy.c \
	ec/ec_p256_m31.c ec/ec_secp256r1.c ec/ec_secp384r1.c ec/ec_secp521r1.c \
	ec/ecdsa_atr.c ec/ecdsa_i31_bits.c ec/ecdsa_i31_vrfy_asn1.c \
	ec/ecdsa_i31_vrfy_raw.c int/i31_add.c int/i31_bitlen.c int/i31_decmod.c \
	int/i31_decode.c int/i31_encode.c int/i31_fmont.c int/i31_iszero.c \
	int/i31_modpow.c int/i31_montmul.c int/i31_muladd.c int/i31_ninv31.c \
	int/i31_rshift.c int/i31_sub.c int/i31_tmont.c int/i32_div32.c)
KERNEL_BEARSSL_OBJECTS := $(patsubst sdk/bearssl/src/%.c,$(BUILD)/kbearssl/%.o,$(KERNEL_BEARSSL_SOURCES))

CFLAGS := -std=gnu11 -O2 -Wall -Wextra \
	-ffreestanding -fno-builtin -fno-stack-protector \
	-fno-pic -fno-pie -m64 -mno-red-zone -mgeneral-regs-only -mcmodel=kernel
LDFLAGS := -nostdlib -static \
	-Wl,-z,max-page-size=0x1000 \
	-Wl,-z,noexecstack \
	-Wl,--build-id=none \
	-Wl,-T,kernel/linker.ld

.PHONY: all run test-disk persistence-test sound-test permissions-test package-test release store clean

all: $(ISO)

run: $(ISO)
	qemu-system-x86_64 -cdrom $(ISO) -display none -serial stdio -monitor none -no-reboot

test-disk: $(TEST_DISK)

persistence-test: $(ISO) $(TEST_DISK)
	$(BUILD)/mkfat16_test $(TEST_DISK)
	sh tools/test_fat16_persistence.sh $(ISO) $(TEST_DISK)

sound-test: $(ISO)
	sh tools/test_sound.sh $(ISO)

permissions-test:
	mkdir -p $(BUILD)
	$(CC) -std=gnu11 -O2 -Wall -Wextra -Ikernel tools/test_app_permissions.c kernel/permissions.c kernel/package.c kernel/sha256.c -o $(BUILD)/test_app_permissions
	$(BUILD)/test_app_permissions
	$(CC) -std=gnu11 -O2 -Wall -Wextra -Ikernel tools/test_permission_grants.c kernel/permission_grants.c kernel/permissions.c kernel/package.c kernel/manifest.c kernel/sha256.c -o $(BUILD)/test_permission_grants
	$(BUILD)/test_permission_grants
	$(CC) -std=gnu11 -O2 -Wall -Wextra -Ikernel -Isdk/include tools/test_manifest.c kernel/manifest.c -o $(BUILD)/test_manifest
	$(BUILD)/test_manifest
	$(CC) -std=gnu11 -O2 -Wall -Wextra -ffunction-sections -fdata-sections -Wl,--gc-sections -Ikernel tools/test_vfs_root_guard.c -o $(BUILD)/test_vfs_root_guard
	$(BUILD)/test_vfs_root_guard

package-test:
	mkdir -p $(BUILD)
	python3 sdk/tools/test_mkopk.py
	$(CC) -std=gnu11 -O2 -Wall -Wextra -Ikernel tools/test_package.c kernel/package.c -o $(BUILD)/test_package
	$(BUILD)/test_package
	python3 tools/test_package_parity.py $(BUILD)/test_package

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

$(MUSL_NET_TEST_ELF): kernel/musl_net_test.c
	mkdir -p $(BUILD)
	$(MUSL_CC) -static -Os -s -fno-stack-protector -Wl,--build-id=none $< -o $@

$(SDK_NOTEPAD): $(SDK_SOURCES)
	$(MAKE) -C sdk build/notepad

$(SDK_FETCH): $(SDK_SOURCES)
	$(MAKE) -C sdk build/fetch

$(SDK_FONTTEST): $(SDK_SOURCES)
	$(MAKE) -C sdk build/fonttest

$(SDK_BROWSER): $(SDK_SOURCES)
	$(MAKE) -C sdk build/browser

$(SDK_UPDATE): $(SDK_SOURCES)
	$(MAKE) -C sdk build/update

$(SDK_PKG): $(SDK_SOURCES)
	$(MAKE) -C sdk build/pkg

$(SDK_PLAY): $(SDK_SOURCES)
	$(MAKE) -C sdk build/play

# The SDK is self-contained, so it carries copies of the kernel's image
# decoder; these rules keep them identical.
sdk/include/opos_image.h: kernel/image.h
	cp $< $@

sdk/lib/image.c: kernel/image.c
	sed 's|#include "image.h"|#include "opos_image.h"|' $< > $@

$(INITRAMFS): $(WALLPAPERS) $(FONTS) $(SDK_FONTTEST) $(SDK_BROWSER) $(CA_BUNDLE) $(SDK_NOTEPAD) $(SDK_FETCH) $(SDK_UPDATE) $(SDK_PKG) $(SDK_PLAY) $(USER_ELF) $(EXEC_TEST_ELF) $(DYNAMIC_MAIN_ELF) $(DYNAMIC_PIE_MAIN_ELF) $(DYNAMIC_INTERPRETER_ELF) $(DYNAMIC_LIBRARY_ELF) $(DYNAMIC_BASE_LIBRARY_ELF) $(MUSL_TEST_ELF) $(MUSL_PTHREAD_TEST_ELF) $(MUSL_SIGNAL_TEST_ELF) $(MUSL_NET_TEST_ELF) $(MUSL_LIBC_SO)
	rm -rf $(BUILD)/initramfs_root
	mkdir -p $(BUILD)/initramfs_root/bin $(BUILD)/initramfs_root/lib $(BUILD)/initramfs_root/tests
	cp $(USER_ELF) $(BUILD)/initramfs_root/tests/elf-test
	cp $(EXEC_TEST_ELF) $(BUILD)/initramfs_root/tests/exec-test
	cp $(DYNAMIC_MAIN_ELF) $(BUILD)/initramfs_root/tests/dynamic-test
	cp $(DYNAMIC_PIE_MAIN_ELF) $(BUILD)/initramfs_root/tests/pie-test
	cp $(DYNAMIC_INTERPRETER_ELF) $(BUILD)/initramfs_root/lib/opos-ld.so
	cp $(DYNAMIC_LIBRARY_ELF) $(BUILD)/initramfs_root/lib/libopos-test.so
	cp $(DYNAMIC_BASE_LIBRARY_ELF) $(BUILD)/initramfs_root/lib/libopos-base.so
	cp $(MUSL_TEST_ELF) $(BUILD)/initramfs_root/tests/musl-test
	cp $(MUSL_PTHREAD_TEST_ELF) $(BUILD)/initramfs_root/tests/musl-pthread-test
	cp $(MUSL_NET_TEST_ELF) $(BUILD)/initramfs_root/tests/musl-net-test
	cp $(MUSL_SIGNAL_TEST_ELF) $(BUILD)/initramfs_root/tests/musl-signal-test
	cp $(SDK_NOTEPAD) $(BUILD)/initramfs_root/bin/notepad
	cp $(SDK_FETCH) $(BUILD)/initramfs_root/bin/fetch
	cp $(SDK_UPDATE) $(BUILD)/initramfs_root/bin/update
	cp $(SDK_PKG) $(BUILD)/initramfs_root/bin/pkg
	cp $(SDK_PLAY) $(BUILD)/initramfs_root/bin/play
	cp $(SDK_BROWSER) $(BUILD)/initramfs_root/bin/browser
	cp $(SDK_FONTTEST) $(BUILD)/initramfs_root/tests/fonttest
	mkdir -p $(BUILD)/initramfs_root/share/fonts
	cp $(FONTS) $(BUILD)/initramfs_root/share/fonts/
	mkdir -p $(BUILD)/initramfs_root/share/wallpapers
	cp $(WALLPAPERS) $(BUILD)/initramfs_root/share/wallpapers/
	if [ -n "$(MUSL_LIBC_SO)" ]; then 		cp $(MUSL_LIBC_SO) $(BUILD)/initramfs_root/lib/ld-musl-x86_64.so.1; 		mkdir -p $(BUILD)/initramfs_root/share/licenses; 		cp $(MUSL_LICENSE) $(BUILD)/initramfs_root/share/licenses/musl.txt; 	fi
	mkdir -p $(BUILD)/initramfs_root/etc/ssl/certs
	cp $(CA_BUNDLE) $(BUILD)/initramfs_root/etc/ssl/certs/
	tar --format=ustar -C $(BUILD)/initramfs_root -cf $@ bin etc lib share tests

$(KERNEL): kernel/main.c kernel/interrupts.c kernel/interrupts.h kernel/interrupts.S kernel/io.h kernel/pci.c kernel/pci.h kernel/ahci.c kernel/ahci.h kernel/ata.c kernel/ata.h kernel/nvme.c kernel/nvme.h kernel/pvscsi.c kernel/pvscsi.h kernel/mpt.c kernel/mpt.h kernel/buslogic.c kernel/buslogic.h kernel/scsi.c kernel/scsi.h kernel/partition.c kernel/partition.h kernel/block.c kernel/block.h kernel/fat16.c kernel/fat16.h kernel/storage.c kernel/storage.h kernel/optical.c kernel/optical.h kernel/iso9660.c kernel/iso9660.h kernel/xhci.c kernel/xhci.h kernel/usb_storage.c kernel/usb_storage.h kernel/vfs.c kernel/vfs.h kernel/keyboard.c kernel/keyboard.h kernel/mouse.c kernel/mouse.h kernel/desktop.c kernel/desktop.h kernel/cmos.c kernel/cmos.h kernel/memory.c kernel/memory.h kernel/paging.c kernel/paging.h kernel/power.c kernel/power.h kernel/scheduler.c kernel/scheduler.h kernel/shell.c kernel/shell.h kernel/timer.c kernel/timer.h kernel/user.c kernel/user.h kernel/user.S kernel/elf.c kernel/elf.h kernel/initramfs.c kernel/initramfs.h kernel/console.h kernel/font.c kernel/font.h kernel/svga.c kernel/svga.h kernel/vmware.c kernel/vmware.h kernel/installer.c kernel/installer.h kernel/system_root.c kernel/system_root.h kernel/accounts.c kernel/accounts.h kernel/sha256.c kernel/sha256.h kernel/permissions.c kernel/permissions.h kernel/permission_grants.c kernel/permission_grants.h kernel/manifest.c kernel/manifest.h kernel/package.c kernel/package.h kernel/package_install.c kernel/image.c kernel/image.h kernel/appearance.c kernel/appearance.h kernel/boot_display.c kernel/boot_display.h kernel/sdhci.c kernel/sdhci.h kernel/acpi_dump.c kernel/acpi_dump.h kernel/i2c_hid.c kernel/i2c_hid.h kernel/touchpad_gesture.c kernel/touchpad_gesture.h kernel/e1000.c kernel/e1000.h kernel/hda.c kernel/hda.h kernel/sound.c kernel/sound.h kernel/vmxnet3.c kernel/vmxnet3.h kernel/pcnet.c kernel/pcnet.h kernel/nic.c kernel/nic.h kernel/net.c kernel/net.h kernel/update.c kernel/update.h kernel/update_key.h kernel/signature.c kernel/signature.h kernel/store_key.h VERSION $(KERNEL_BEARSSL_OBJECTS) kernel/vbrstub.S kernel/linker.ld $(LIMINE_HEADER)
	$(CC) $(CFLAGS) -DOPOS_VERSION='"$(OPOS_VERSION)"' -Ikernel -Isdk/bearssl/inc -I$(dir $(LIMINE_HEADER)) $(LDFLAGS) -o $@ kernel/main.c kernel/interrupts.c kernel/interrupts.S kernel/pci.c kernel/ahci.c kernel/ata.c kernel/nvme.c kernel/pvscsi.c kernel/mpt.c kernel/buslogic.c kernel/scsi.c kernel/partition.c kernel/block.c kernel/fat16.c kernel/storage.c kernel/optical.c kernel/iso9660.c kernel/xhci.c kernel/usb_storage.c kernel/vfs.c kernel/keyboard.c kernel/mouse.c kernel/desktop.c kernel/cmos.c kernel/memory.c kernel/paging.c kernel/power.c kernel/scheduler.c kernel/shell.c kernel/timer.c kernel/user.c kernel/elf.c kernel/initramfs.c kernel/font.c kernel/svga.c kernel/vmware.c kernel/installer.c kernel/system_root.c kernel/accounts.c kernel/sha256.c kernel/permissions.c kernel/permission_grants.c kernel/manifest.c kernel/package.c kernel/package_install.c kernel/image.c kernel/appearance.c kernel/boot_display.c kernel/sdhci.c kernel/acpi_dump.c kernel/i2c_hid.c kernel/touchpad_gesture.c kernel/hda.c kernel/sound.c kernel/e1000.c kernel/vmxnet3.c kernel/pcnet.c kernel/nic.c kernel/net.c kernel/update.c kernel/signature.c kernel/user.S kernel/vbrstub.S $(KERNEL_BEARSSL_OBJECTS)

$(BUILD)/kbearssl/%.o: sdk/bearssl/src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(filter-out -Wall -Wextra,$(CFLAGS)) -U_FORTIFY_SOURCE -Isdk/bearssl/inc -Isdk/bearssl/src \
		-DBR_USE_UNIX_TIME=0 -DBR_USE_URANDOM=0 -DBR_USE_GETENTROPY=0 \
		-DBR_USE_WIN32_RAND=0 -DBR_USE_WIN32_TIME=0 -DBR_RDRAND=0 \
		-DBR_AES_X86NI=0 -DBR_SSE2=0 -DBR_POWER8=0 -c $< -o $@

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

# make_iso OUTPUT,EXTRA: builds a boot ISO; files in the folder EXTRA (if
# given) go into its /boot as well.
define make_iso
	rm -rf $(BUILD)/iso_root
	mkdir -p $(BUILD)/iso_root/boot/limine $(BUILD)/iso_root/EFI/BOOT
	cp $(KERNEL) $(BUILD)/iso_root/boot/kernel.elf
	cp $(INITRAMFS) $(BUILD)/iso_root/boot/initramfs.tar
	cp $(INSTALL_TAR) $(BUILD)/iso_root/boot/install.tar
	$(if $(2),cp $(2)/* $(BUILD)/iso_root/boot/)
	cp limine.conf $(BUILD)/iso_root/boot/limine/
	cp $(LIMINE_DIR)/limine-uefi-cd.bin $(BUILD)/iso_root/boot/limine/
	cp $(LIMINE_DIR)/limine-bios.sys $(LIMINE_DIR)/limine-bios-cd.bin $(BUILD)/iso_root/boot/limine/
	cp $(LIMINE_DIR)/BOOTX64.EFI $(LIMINE_DIR)/BOOTIA32.EFI $(BUILD)/iso_root/EFI/BOOT/
	xorriso -as mkisofs -R -r -J \
		-b boot/limine/limine-bios-cd.bin -no-emul-boot -boot-load-size 4 -boot-info-table \
		-hfsplus -apm-block-size 2048 \
		--efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		$(BUILD)/iso_root -o $(1)
	$(LIMINE_DIR)/limine bios-install $(1)
	rm -rf $(BUILD)/iso_root
endef

$(ISO): $(LIMINE_DIR)/limine $(KERNEL) $(USER_ELF) $(INITRAMFS) $(INSTALL_TAR) limine.conf
	$(call make_iso,$@,)

# Update files for a GitHub Release (see docs/updates.md), signed with
# UPDATE_KEY. Upload everything in build/release/ to the release.
# release-notes.txt is shown in Settings > UPDATES: edit it per release.
# The release ISO also carries the signed manifest in /boot, so attaching
# it to a VM (or burning it) works as update media too.
release: $(ISO)
	python3 tools/mkrelease.py --version $(OPOS_VERSION) --key $(UPDATE_KEY) \
		--output $(RELEASE) --notes release-notes.txt \
		$(INITRAMFS)=boot/initramfs.tar $(KERNEL)=boot/kernel.elf
	rm -rf $(BUILD)/iso_extra
	mkdir -p $(BUILD)/iso_extra
	cp $(RELEASE)/opos-update.manifest $(RELEASE)/opos-update.sig \
		$(RELEASE)/opos-release-notes.txt $(BUILD)/iso_extra/
	$(call make_iso,$(RELEASE)/opos-$(OPOS_VERSION).iso,$(BUILD)/iso_extra)
	rm -rf $(BUILD)/iso_extra

# Store packages and their signed index in build/store/. Upload them as a
# release of PorkifyNET/opos-packages (store/README.md).
store:
	$(MAKE) -C sdk all
	rm -rf $(STORE) $(BUILD)/store_opk
	mkdir -p $(BUILD)/store_opk
	for package in $(STORE_PACKAGES); do \
		python3 sdk/tools/mkopk.py build $$package -o $(BUILD)/store_opk || exit 1; \
	done
	@test -n "$(MUSL_LICENSE)" || { echo "store: musl's copyright file not found; set MUSL_LICENSE"; exit 1; }
	for program in $(STORE_LINUX); do \
		LINUX_CC="$(LINUX_CC)" MUSL_LICENSE="$(abspath $(MUSL_LICENSE))" \
		MKOPK="$(abspath sdk/tools/mkopk.py)" \
		sh store/linux/$$program/build.sh $(abspath $(BUILD)/store_opk) \
			$(abspath $(STORE_SOURCES)) || exit 1; \
	done
	rm -rf $(BUILD)/store_opk/work-*
	python3 tools/mkindex.py --output $(STORE) --key $(STORE_KEY) \
		--descriptions store/descriptions.txt $(BUILD)/store_opk
	rm -rf $(BUILD)/store_opk

clean:
	rm -rf $(BUILD)
	$(MAKE) -C sdk clean
