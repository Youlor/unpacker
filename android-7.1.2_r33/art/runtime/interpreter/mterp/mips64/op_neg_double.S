%include "mips64/fcvtHeader.S" { "suffix":"_DOUBLE", "valreg":"f0" }
    neg.d   f0, f0
%include "mips64/fcvtFooter.S" { "suffix":"_DOUBLE", "valreg":"f0" }
