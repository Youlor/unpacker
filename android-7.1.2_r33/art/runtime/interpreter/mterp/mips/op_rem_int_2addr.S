#ifdef MIPS32REVGE6
%include "mips/binop2addr.S" {"instr":"mod a0, a0, a1", "chkzero":"1"}
#else
%include "mips/binop2addr.S" {"preinstr":"div zero, a0, a1", "instr":"mfhi a0", "chkzero":"1"}
#endif
