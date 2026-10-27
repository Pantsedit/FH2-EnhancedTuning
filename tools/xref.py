"""Find x64 instructions in the game EXE that address [reg + OFFSET].

  python xref.py GAME.exe 0x1390 [0x2048 ...] [--context 48]

Looks for a ModRM byte with a 32-bit displacement (mod=10) followed by the
offset, preceded by a common load/store/SSE opcode. Crude, but it finds the
code that touches a struct field without needing a disassembler. Prints the
RVA (matches Extra Options' game+0x... addresses) and the surrounding bytes.
Standard library only.
"""
import argparse
import struct

# Opcode bytes that commonly come right before a ModRM byte.
OPCODES = {
    0x8b: "mov r,[m]", 0x89: "mov [m],r", 0x8d: "lea",
    0x10: "movss/ups load", 0x11: "movss/ups store", 0x28: "movaps load", 0x29: "movaps store",
    0x58: "add", 0x59: "mul", 0x5c: "sub", 0x5e: "div", 0x2e: "ucomiss", 0x2f: "comiss",
    0x51: "sqrt", 0x5d: "min", 0x5f: "max", 0xc7: "mov [m],imm", 0x3b: "cmp", 0x39: "cmp",
}


def sections(data):
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    count = struct.unpack_from("<H", data, pe + 6)[0]
    opt_size = struct.unpack_from("<H", data, pe + 20)[0]
    at = pe + 24 + opt_size
    for _ in range(count):
        name = data[at:at + 8].rstrip(b"\0").decode(errors="replace")
        vsize, va, rsize, raw = struct.unpack_from("<IIII", data, at + 8)
        flags = struct.unpack_from("<I", data, at + 36)[0]
        yield name, va, raw, rsize, flags
        at += 40


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("exe")
    p.add_argument("offsets", nargs="+")
    p.add_argument("--context", type=int, default=24)
    args = p.parse_args()
    data = open(args.exe, "rb").read()
    code = [(n, va, raw, size) for n, va, raw, size, flags in sections(data) if flags & 0x20000000]
    for text in args.offsets:
        offset = int(text, 0)
        needle = struct.pack("<I", offset)
        hits = []
        for name, va, raw, size in code:
            blob = data[raw:raw + size]
            at = blob.find(needle)
            while at != -1:
                modrm = blob[at - 1] if at >= 1 else 0
                opcode = blob[at - 2] if at >= 2 else 0
                if modrm & 0xC0 == 0x80 and modrm & 7 != 4 and opcode in OPCODES:
                    hits.append((va + at - 2, OPCODES[opcode], (modrm >> 3) & 7, modrm & 7,
                                 blob[max(0, at - args.context):at + 4 + args.context]))
                at = blob.find(needle, at + 1)
        print(f"== [reg+0x{offset:x}]: {len(hits)} instruction(s)")
        for rva, kind, reg, base, ctx in hits:
            print(f"  game+0x{rva:x}  {kind:16} reg={reg} base={base}  {ctx.hex(' ')}")


if __name__ == "__main__":
    main()
