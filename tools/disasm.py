"""Disassemble the whole game function containing an address.

  python disasm.py GAME.exe 0x1375ad6 [more RVAs] [--mark 0x1390,0x1360] [-o out.txt]

Function bounds come from the EXE's .pdata table, so the output starts at the
function's first instruction. Lines touching a --mark offset get a '>>'.
Needs: pip install capstone
"""
import argparse
import bisect
import struct

import capstone


class Image:
    def __init__(self, path):
        self.data = open(path, "rb").read()
        d = self.data
        pe = struct.unpack_from("<I", d, 0x3C)[0]
        count = struct.unpack_from("<H", d, pe + 6)[0]
        opt = pe + 24
        opt_size = struct.unpack_from("<H", d, pe + 20)[0]
        self.base = struct.unpack_from("<Q", d, opt + 24)[0]
        pdata_rva, pdata_size = struct.unpack_from("<II", d, opt + 112 + 3 * 8)
        self.sections = []
        at = opt + opt_size
        for _ in range(count):
            vsize, va, rsize, raw = struct.unpack_from("<IIII", d, at + 8)
            self.sections.append((va, max(vsize, rsize), raw))
            at += 40
        start = self.offset(pdata_rva)
        self.functions = sorted(struct.unpack_from("<II", d, start + i * 12)
                                for i in range(pdata_size // 12))
        self.starts = [f[0] for f in self.functions]

    def offset(self, rva):
        for va, size, raw in self.sections:
            if va <= rva < va + size:
                return rva - va + raw
        raise ValueError(f"RVA 0x{rva:x} is outside the image")

    def function_at(self, rva):
        i = bisect.bisect_right(self.starts, rva) - 1
        if i < 0 or not (self.functions[i][0] <= rva < self.functions[i][1]):
            return None
        # Chained unwind entries split one function into pieces; walk back to the
        # first piece that ends where the next begins.
        begin, end = self.functions[i]
        while i > 0 and self.functions[i - 1][1] == begin:
            i -= 1
            begin = self.functions[i][0]
        return begin, end


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("exe")
    p.add_argument("rvas", nargs="+")
    p.add_argument("--mark", default="")
    p.add_argument("-o", "--output")
    args = p.parse_args()
    image = Image(args.exe)
    marks = [int(m, 0) for m in args.mark.split(",") if m]
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    lines, done = [], set()
    for text in args.rvas:
        rva = int(text, 0)
        bounds = image.function_at(rva)
        if not bounds:
            lines.append(f"; 0x{rva:x}: no .pdata function")
            continue
        if bounds in done:
            continue
        done.add(bounds)
        begin, end = bounds
        lines.append(f"\n; ===== function game+0x{begin:x} .. 0x{end:x} ({end - begin} bytes), contains 0x{rva:x}")
        code = image.data[image.offset(begin):image.offset(begin) + (end - begin)]
        for insn in md.disasm(code, begin):
            text_ops = f"{insn.mnemonic} {insn.op_str}"
            hit = any(f"0x{m:x}]" in insn.op_str for m in marks)
            lines.append(f"{'>>' if hit else '  '} {insn.address:08x}  {text_ops}")
    out = "\n".join(lines)
    if args.output:
        open(args.output, "w").write(out)
        print(f"wrote {len(lines)} lines to {args.output}")
    else:
        print(out)


if __name__ == "__main__":
    main()
