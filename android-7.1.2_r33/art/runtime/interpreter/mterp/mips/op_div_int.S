#ifdef MIPS32REVGE6
%include "mips/binop.S" {"instr":"div a0, a0, a1", "chkzero":"1"}
#else
%include "mips/binop.S" {"preinstr":"div zero, a0, a1", "instr":"mflo a0", "chkzero":"1"}
#endif
