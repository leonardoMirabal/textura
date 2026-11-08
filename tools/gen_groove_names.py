#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""GROOVE's grid names packed in 5-bit codes (engines/tx_groove.c GR_GN): prints the C array to paste there.

The codes, low bits first: 0 the end of a name, 1 .. 26 A .. Z, 27 ' ', 28 '-', 29 '+', 30 '/', 31 a digit (its value
the next code). The names in the grids' order (TYPE 0, then 33 .. 72), the SWINGs of 8 steps left out (tx_groove_name
names them by their swing)."""

NAMES = ("LOOSE 16|STRAIGHT 8|SWING 16|SHUFFLE|TRIPLETS|DOTTED|HALF-TIME|CLAVE 3-2|CLAVE 2-3|RUMBA 3-2|RUMBA 2-3|"
         "BOSSA|TRESILLO|CINQUILLO|SAMBA|BAIAO|12/8 BELL|BEMBE|ONE DROP|DEMBOW|OFFBEAT|BREAKS|GAHU|SHIKO|SOUKOUS|"
         "SON MONTUNO|PARTIDO|NANIGO|KPANLOGO|FUME FUME|SEVEN 2+2+3|FIVE 3+2|WALTZ|JIG|SLOW 4|")


def codes(text):
    for ch in text:
        if ch == "|":
            yield 0
        elif "A" <= ch <= "Z":
            yield ord(ch) - 64
        elif ch.isdigit():
            yield 31
            yield int(ch)
        else:
            yield 27 + " -+/".index(ch)


def main():
    bits = n = 0
    out = []
    for c in codes(NAMES):
        bits |= c << n
        n += 5
        while n >= 8:
            out.append(bits & 255)
            bits >>= 8
            n -= 8
    out.append(bits & 255)
    out.append(0)                                # (the decoder reads a byte past the last code)
    print(f"static const uint8_t GR_GN[{len(out)}] = {{")
    for i in range(0, len(out), 19):
        print("    " + " ".join(f"{b:#04x}," for b in out[i:i + 19]))
    print("};")


if __name__ == "__main__":
    main()
