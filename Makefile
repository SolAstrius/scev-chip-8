# scev-cores/chip-8 — bare-metal CHIP-8 firmware for RVVM
#
# Build: `make`        produces firmware.bin (flat binary, mtd-physmap loadable)
# Run:   `make run`    boots under RVVM
# Clean: `make clean`

TARGET   := riscv64-freestanding-none
CC       := zig cc -target $(TARGET)
LD       := zig cc -target $(TARGET)
OBJCOPY  := llvm-objcopy

CFLAGS   := -Os -ffreestanding -fno-stack-protector -fno-pie \
            -mcmodel=medany -nostdlib \
            -Wall -Wextra -Wno-unused-parameter \
            -Isrc

LDFLAGS  := -nostdlib -static -Wl,-T,link.ld

OBJS     := build/start.o build/main.o build/uart.o build/chip8.o \
            build/pci.o build/bochs.o

all: firmware.bin

build/%.o: src/%.c
	@mkdir -p build
	$(CC) $(CFLAGS) -c -o $@ $<

build/%.o: src/%.S
	@mkdir -p build
	$(CC) $(CFLAGS) -c -o $@ $<

firmware.elf: $(OBJS) link.ld
	$(LD) $(LDFLAGS) -o $@ $(OBJS)

firmware.bin: firmware.elf
	$(OBJCOPY) -O binary $< $@
	@printf '\nBuilt %s (%s bytes)\n' "$@" "$$(stat -c %s $@)"

run: firmware.bin
	rvvm -m 64M -mtd firmware.bin

clean:
	rm -rf build firmware.elf firmware.bin

.PHONY: all run clean
