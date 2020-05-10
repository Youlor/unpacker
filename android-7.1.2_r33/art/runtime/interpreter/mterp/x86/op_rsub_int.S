/* this op is "rsub-int", but can be thought of as "rsub-int/lit16" */
%include "x86/binopLit16.S" {"instr":"subl    %eax, %ecx","result":"%ecx"}
