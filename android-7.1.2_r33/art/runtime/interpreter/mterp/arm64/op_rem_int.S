%include "arm64/binop.S" {"preinstr":"sdiv     w2, w0, w1", "instr":"msub w0, w2, w1, w0", "chkzero":"1"}
