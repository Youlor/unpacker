%include "mips64/fcvtHeader.S" { "suffix":"_FLOAT", "valreg":"f0" }
    neg.s   f0, f0
%include "mips64/fcvtFooter.S" { "suffix":"_FLOAT", "valreg":"f0" }
