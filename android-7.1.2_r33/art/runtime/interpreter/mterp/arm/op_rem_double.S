/* EABI doesn't define a double remainder function, but libm does */
%include "arm/binopWide.S" {"instr":"bl      fmod"}
