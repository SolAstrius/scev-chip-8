# scev-cores/chip-8 — bare-metal CHIP-8 firmware for RVVM.
#
# Consumes rvvm-hal (vendor/rvvm-hal as a git submodule) for the
# device drivers; only chip-8-specific code lives here.
#
# Build: `make`        produces firmware.bin
# Run:   `make run`    boots under RVVM with -bochs_display
# Clean: `make clean`

HAL      := vendor/rvvm-hal
TARGET   := riscv64-freestanding-none
CC       := zig cc -target $(TARGET)
OBJCOPY  := llvm-objcopy

# Path to the RVVM binary. Override on the command line:
#   make run RVVM=/path/to/rvvm_x86_64
# Default tries `rvvm` on PATH first, then known build location.
RVVM     ?= $(shell command -v rvvm 2>/dev/null || \
                    echo /home/sol/repos/RVVM/release.linux.x86_64/rvvm_x86_64)

CFLAGS   := -Os -ffreestanding -fno-stack-protector -fno-pie \
            -mcmodel=medany -nostdlib \
            -Wall -Wextra -Wno-unused-parameter \
            -Isrc -I$(HAL)/include

LDFLAGS  := -nostdlib -static -Wl,-T,$(HAL)/link.ld

OBJS     := build/main.o build/chip8.o

# We need -hda_test on the rvvm command line to attach the HDA device.

all: firmware.bin

build/%.o: src/%.c
	@mkdir -p build
	$(CC) $(CFLAGS) -c -o $@ $<

$(HAL)/libhal.a:
	$(MAKE) -C $(HAL)

firmware.elf: $(OBJS) $(HAL)/libhal.a
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(HAL)/libhal.a

firmware.bin: firmware.elf
	$(OBJCOPY) -O binary $< $@
	@printf '\nBuilt %s (%s bytes)\n' "$@" "$$(stat -c %s $@)"

run: firmware.bin
	$(RVVM) firmware.bin -bochs_display -nonet -hda_test

run-headless: firmware.bin
	$(RVVM) firmware.bin -nogui -nonet -hda_test

run-rom: firmware.bin
	@test -n "$(ROM)" || { echo "usage: make run-rom ROM=roms/foo.padded.ch8"; exit 1; }
	$(RVVM) firmware.bin -bochs_display -nonet -hda_test -ata $(ROM)

clean:
	rm -rf build firmware.elf firmware.bin
	$(MAKE) -C $(HAL) clean

.PHONY: all run run-headless run-rom clean
