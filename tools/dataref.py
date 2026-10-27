"""Find code that touches a range of global data via RIP-relative addressing.

  python dataref.py GAME.exe START_RVA END_RVA

Decodes every instruction with capstone (linear sweep per .pdata function) and
lists those whose RIP-relative target falls in [START, END). Needs capstone.
"""
import sys

import capstone
from capstone import x86

from disasm import Image


def main():
    image = Image(sys.argv[1])
    start, end = int(sys.argv[2], 0), int(sys.argv[3], 0)
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.detail = True
    hits = []
    for begin, finish in image.functions:
        try:
            code = image.data[image.offset(begin):image.offset(begin) + (finish - begin)]
        except ValueError:
            continue
        for insn in md.disasm(code, begin):
            for op in insn.operands:
                if op.type == x86.X86_OP_MEM and op.mem.base == x86.X86_REG_RIP:
                    target = insn.address + insn.size + op.mem.disp
                    if start <= target < end:
                        hits.append((insn.address, target, f"{insn.mnemonic} {insn.op_str}", begin))
    for address, target, text, function in sorted(hits):
        print(f"game+0x{address:x}  (fn 0x{function:x})  -> 0x{target:x}  {text}")
    print(f"{len(hits)} reference(s)")


if __name__ == "__main__":
    main()
