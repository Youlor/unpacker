/* must be "mul w0, w1, w0" -- "w0, w0, w1" is illegal */
%include "arm64/binopLit16.S" {"instr":"mul     w0, w1, w0"}
