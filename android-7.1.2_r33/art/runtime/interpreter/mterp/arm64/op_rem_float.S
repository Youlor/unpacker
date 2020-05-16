/* EABI doesn't define a float remainder function, but libm does */
%include "arm64/fbinop.S" {"instr":"bl      fmodf"}
