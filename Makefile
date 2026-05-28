CFLAGS := -Wall -Werror -I include -MMD

BIN_FILES = reset_example.bin jtag_example.bin intermezzo.bin boot_bct.bin mem_dumper_usb_server.bin emmc_server.bin

all: shofel2_t124 $(BIN_FILES)

# --------- Host (was x86) ----------

UNAME_S := $(shell uname -s)

# BACKEND selects the USB I/O implementation:
#   sysfs  -> original Linux-only backend (mini_libusb.c)
#   libusb -> portable libusb-1.0 backend (mini_libusb_libusb.c)
# Default: sysfs on Linux (preserves upstream behaviour), libusb on Darwin.
ifeq ($(UNAME_S),Darwin)
  BACKEND ?= libusb
else
  BACKEND ?= sysfs
endif

CC_x86 ?= cc
CFLAGS_x86 := $(CFLAGS)
LDFLAGS_x86 :=

ifeq ($(BACKEND),libusb)
  CFLAGS_x86  += $(shell pkg-config --cflags libusb-1.0)
  LDFLAGS_x86 += $(shell pkg-config --libs libusb-1.0)
  USB_BACKEND_SRC := exploit/mini_libusb_libusb.c
else
  USB_BACKEND_SRC := exploit/mini_libusb.c
endif

# Pick up every exploit/*.c except the backend file we're not using.
C_FILES_x86 := $(filter-out exploit/mini_libusb.c exploit/mini_libusb_libusb.c, $(wildcard exploit/*.c)) $(USB_BACKEND_SRC)
OBJ_FILES_x86 := $(addprefix build/obj_x86/,$(notdir $(C_FILES_x86:.c=.o)))
-include $(OBJ_FILES_x86:.o=.d)

build/obj_x86/%.o: exploit/%.c
	@mkdir -p $(@D)
	$(CC_x86) $(CFLAGS_x86) -c -o $@ $<

shofel2_t124: $(OBJ_FILES_x86)
	$(CC_x86) $(CFLAGS_x86) -o $@ $^ $(LDFLAGS_x86)

# ------------------------


# ----- ARMv4t Thumb -----

TOOLCHAIN_ARM ?= arm-none-eabi-
CC_ARM = $(TOOLCHAIN_ARM)gcc
AS_ARM = $(TOOLCHAIN_ARM)as
OBJCOPY_ARM = $(TOOLCHAIN_ARM)objcopy

CFLAGS_ARM := -Wall -I include -MMD -march=armv4t -mthumb -Os -ffreestanding \
	-fno-common	-fomit-frame-pointer -nostdlib -fno-builtin-printf \
	-fno-asynchronous-unwind-tables -fPIE -fno-builtin -fno-exceptions \
	-Wno-array-bounds -Wno-error \
	-Wl,--no-dynamic-linker,--build-id=none,-T,payloads/payload.ld

C_FILES_ARM := $(wildcard payloads/*.c)
OBJ_FILES_ARM := $(addprefix build/obj_arm/,$(notdir $(C_FILES_ARM:.c=.o)))
-include $(OBJ_FILES_ARM:.o=.d)

build/obj_arm/%.o: payloads/%.c
	@mkdir -p $(@D)
	$(CC_ARM) $(CFLAGS_ARM) -c -o $@ $<

build/reset_example.elf: build/obj_arm/reset_example.o
	$(CC_ARM) $(CFLAGS_ARM) -o $@ $^ -lgcc

build/jtag_example.elf: build/obj_arm/jtag_example.o
	$(CC_ARM) $(CFLAGS_ARM) -o $@ $^ -lgcc

build/boot_bct.elf: build/obj_arm/boot_bct.o
	$(CC_ARM) $(CFLAGS_ARM) -o $@ $^ -lgcc

build/mem_dumper_usb_server.elf: build/obj_arm/mem_dumper_usb_server.o
	$(CC_ARM) $(CFLAGS_ARM) -o $@ $^ -lgcc

build/emmc_server.elf: build/obj_arm/emmc_server.o
	$(CC_ARM) $(CFLAGS_ARM) -o $@ $^ -lgcc

build/intermezzo.elf: build/obj_arm/intermezzo.o
	$(CC_ARM) $(CFLAGS_ARM) -o $@ $^ -lgcc

%.bin: build/%.elf
	$(OBJCOPY_ARM) -O binary $< $@

# ------------------------


clean:
	rm -f $(OBJ_FILES_ARM) $(OBJ_FILES_x86)
	rm -f shofel2_t124 build/*.elf $(BIN_FILES)

cleanall: clean
	rm -f build/obj_arm/*.d build/obj_x86/*.d
