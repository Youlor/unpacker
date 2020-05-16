/* EABI doesn't define a double remainder function, but libm does */
%include "arm/binopWide2addr.S" {"instr":"bl      fmod"}
