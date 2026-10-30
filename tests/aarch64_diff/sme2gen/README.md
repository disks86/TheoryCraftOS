The tables in src/Emulation/AArch64Sme2*Forms.inc are generated from LLVM's encodings (llvm-mc with all features) by these scripts, so that the SME2 decoder
needs no hand-written bit positions:

    python3 mkinc.py    # ZA array vector-group instructions  -> AArch64Sme2Forms.inc
    python3 mkvec.py    # multi-vector arithmetic (min/max, clamp, ...) -> AArch64Sme2VecForms.inc
    python3 mkmisc.py   # zip/uzp, unpack, conversions, narrowing shifts, sel -> AArch64Sme2MiscForms.inc
    python3 mkmop.py    # MOP4 outer products -> AArch64Sme2Mop4Forms.inc

`forms.py` assembles each template with random operands and keeps the bits that never change (mask and value), and the bit positions of each operand.
