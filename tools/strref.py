"""Find code that loads the address of a string (RIP-relative LEA/MOV) in the game EXE.

  python strref.py GAME.exe TEXT [TEXT ...]

Prints the RVA of every `lea/mov reg, [rip+X]` whose target is the start of
that ASCII string. Standard library only.
"""
import struct
import sys


def sections(data):
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    count = struct.unpack_from("<H", data, pe + 6)[0]
    at = pe + 24 + struct.unpack_from("<H", data, pe + 20)[0]
    for _ in range(count):
        vsize, va, rsize, raw = struct.unpack_from("<IIII", data, at + 8)
        flags = struct.unpack_from("<I", data, at + 36)[0]
        yield va, max(vsize, rsize), raw, rsize, flags
        at += 40


def main():
    data = open(sys.argv[1], "rb").read()
    secs = list(sections(data))

    def rva_of(offset):
        for va, size, raw, rsize, _ in secs:
            if raw <= offset < raw + rsize:
                return va + offset - raw

    targets = {}
    for text in sys.argv[2:]:
        offset = data.find(text.encode() + b"\0")
        while offset != -1 and offset > 0 and data[offset - 1] != 0:
            offset = data.find(text.encode() + b"\0", offset + 1)
        if offset == -1:
            print(f"{text}: not found")
            continue
        targets[rva_of(offset)] = text
    for va, size, raw, rsize, flags in secs:
        if not flags & 0x20000000:
            continue
        code = data[raw:raw + rsize]
        for i in range(len(code) - 7):
            if code[i] in (0x48, 0x4C) and code[i + 1] in (0x8D, 0x8B) and code[i + 2] & 0xC7 == 0x05:
                target = va + i + 7 + struct.unpack_from("<i", code, i + 3)[0]
                if target in targets:
                    print(f"game+0x{va + i:x}  references {targets[target]!r}")


if __name__ == "__main__":
    main()
