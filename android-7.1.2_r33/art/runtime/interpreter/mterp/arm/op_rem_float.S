/* EABI doesn't define a float remainder function, but libm does */
%include "arm/binop.S" {"instr":"bl      fmodf"}
