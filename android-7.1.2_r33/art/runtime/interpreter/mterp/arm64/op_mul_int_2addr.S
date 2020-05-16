/* must be "mul w0, w1, w0" -- "w0, w0, w1" is illegal */
%include "arm64/binop2addr.S" {"instr":"mul     w0, w1, w0"}
