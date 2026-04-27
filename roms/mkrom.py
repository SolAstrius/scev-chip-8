#!/usr/bin/env python3
"""Pad a raw CHIP-8 program to 512-byte sector boundaries.

RVVM's ATA emulation reports IDENTIFY capacity = file_size >> 9, so a
file smaller than one sector reports cap=0 and our driver skips it.
RVVM's READ_SECTORS for past-EOF also fails with STATUS.ERR mid-stream,
so we want at least full-sector files.

Usage: python3 mkrom.py <in.ch8> [<out.ch8>]
"""
import sys, os

if len(sys.argv) < 2:
    print(__doc__)
    sys.exit(1)

src = sys.argv[1]
dst = sys.argv[2] if len(sys.argv) > 2 else src.rsplit('.', 1)[0] + '.padded.ch8'

data = open(src, 'rb').read()
pad = (-len(data)) % 512
out = data + bytes(pad)
open(dst, 'wb').write(out)
print(f'{src}: {len(data)} -> {len(out)} bytes ({len(out)//512} sectors) -> {dst}')
