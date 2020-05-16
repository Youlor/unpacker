%include "arm64/binopWide2addr.S" {"preinstr":"sdiv x3, x0, x1", "instr":"msub x0, x3, x1, x0", "chkzero":"1"}
