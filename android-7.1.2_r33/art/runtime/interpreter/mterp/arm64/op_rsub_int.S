/* this op is "rsub-int", but can be thought of as "rsub-int/lit16" */
%include "arm64/binopLit16.S" {"instr":"sub     w0, w1, w0"}
