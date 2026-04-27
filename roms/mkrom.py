#!/usr/bin/env python3
"""Build a fat-ROM (.fat.ch8) from a raw CHIP-8 program.

Output layout (header v2):
  16-byte header  "SCEVCH8\\2" + tickrate(u16 LE) + flags(u8) + 5 reserved
  raw .ch8 bytes
  pad to 512-byte sector boundary

Header is *optional* — the firmware also accepts headerless raw ROMs and
v1 headers (u8 tickrate), falling back to compiled-in defaults
(30 cycles/frame, vblank-wait off) when no header is present.

Usage:
  mkrom.py in.ch8                              # raw, defaults
  mkrom.py in.ch8 --tickrate=200               # tank-style
  mkrom.py in.ch8 --tickrate=20 --vblank       # COSMAC-VIP era games
  mkrom.py in.ch8 --from-archive=DIR/programs.json --name=outlaw
                                               # auto-lookup chip8Archive
                                               # metadata for the named ROM
"""
import argparse, json, os, sys, struct

MAGIC_V2 = b"SCEVCH8\x02"
HEADER_LEN = 16
FLAG_VBLANK = 0x01

def build_header(tickrate=0, vblank=False):
    """v2 header: u16 LE tickrate at off 8..9, flags at off 10."""
    flags = FLAG_VBLANK if vblank else 0
    return MAGIC_V2 + struct.pack("<HB5x", tickrate & 0xFFFF, flags)

def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input", help="raw .ch8 program")
    ap.add_argument("output", nargs="?",
                    help="output (.padded.ch8 if absent)")
    ap.add_argument("--tickrate", type=int, default=0,
                    help="cycles per 60Hz frame (1-255). 0 = use firmware default.")
    ap.add_argument("--vblank", action="store_true",
                    help="enable COSMAC display-wait quirk")
    ap.add_argument("--no-header", action="store_true",
                    help="emit raw + padding only (legacy v0 ROMs)")
    ap.add_argument("--from-archive",
                    help="path to chip8Archive's programs.json — auto-set "
                         "tickrate from the entry named via --name")
    ap.add_argument("--name", help="entry name to look up in --from-archive")
    args = ap.parse_args()

    out_path = args.output or args.input.rsplit(".", 1)[0] + ".padded.ch8"

    if args.from_archive:
        if not args.name:
            sys.exit("--from-archive requires --name")
        meta = json.load(open(args.from_archive))
        if args.name not in meta:
            sys.exit(f"no entry '{args.name}' in {args.from_archive}")
        opts = meta[args.name].get("options", {})
        if "tickrate" in opts and not args.tickrate:
            args.tickrate = int(opts["tickrate"])
        if opts.get("vBlankQuirks"):
            args.vblank = True

    if args.tickrate > 65535:
        print(f"warning: clamping tickrate {args.tickrate} → 65535 "
              f"(header field is u16).", file=sys.stderr)
        args.tickrate = 65535

    raw = open(args.input, "rb").read()
    body = raw if args.no_header else build_header(args.tickrate, args.vblank) + raw

    pad = (-len(body)) % 512
    out = body + b"\x00" * pad
    open(out_path, "wb").write(out)

    h = "raw" if args.no_header else \
        f"hdr(tickrate={args.tickrate} vblank={int(args.vblank)})"
    print(f"{args.input}: {len(raw)} → {out_path}  ({h}, {len(out)} bytes / "
          f"{len(out)//512} sectors)")

if __name__ == "__main__":
    main()
