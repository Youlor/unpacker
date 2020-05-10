#ifdef MIPS32REVGE6
%include "mips/binopLit8.S" {"instr":"mod a0, a0, a1", "chkzero":"1"}
#else
%include "mips/binopLit8.S" {"preinstr":"div zero, a0, a1", "instr":"mfhi a0", "chkzero":"1"}
#endif
