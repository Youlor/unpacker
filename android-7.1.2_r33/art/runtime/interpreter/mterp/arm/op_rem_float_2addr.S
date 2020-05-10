/* EABI doesn't define a float remainder function, but libm does */
%include "arm/binop2addr.S" {"instr":"bl      fmodf"}
