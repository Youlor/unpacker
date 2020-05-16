    /*
     * Conversion from or to floating-point happens in a floating-point register.
     * Therefore we load the input and store the output into or from a
     * floating-point register irrespective of the type.
     */
%include "mips64/fcvtHeader.S" { "suffix":"_DOUBLE", "valreg":"f0" }
    cvt.d.l f0, f0
%include "mips64/fcvtFooter.S" { "suffix":"_DOUBLE", "valreg":"f0" }
