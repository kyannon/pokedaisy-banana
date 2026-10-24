#!/usr/bin/env python3
"""Fills a fixture ROM's cartridge header (gbafix's job in the 3DS build):
title, game code, maker, the fixed 0x96, revision and the header checksum.
No logo - see crt0.s.

    mkheader.py ROM GAMECODE REVISION
"""
import sys


def main():
    path, code, rev = sys.argv[1], sys.argv[2].encode("ascii"), int(sys.argv[3])
    rom = bytearray(open(path, "rb").read())
    if len(rom) < 0xC0 or len(code) != 4:
        sys.exit("not a ROM, or a game code that isn't 4 letters")
    rom[0xA0:0xAC] = b"POKEDAISYTST"
    rom[0xAC:0xB0] = code
    rom[0xB0:0xB2] = b"01"
    rom[0xB2] = 0x96
    rom[0xB3] = 0
    rom[0xBC] = rev
    rom[0xBD] = (-(sum(rom[0xA0:0xBD]) + 0x19)) & 0xFF
    # a whole number of 256 KB, like a cartridge
    pad = (-len(rom)) % 0x40000
    rom += b"\xff" * pad
    open(path, "wb").write(rom)


if __name__ == "__main__":
    main()
