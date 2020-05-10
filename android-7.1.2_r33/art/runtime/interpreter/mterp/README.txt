rt "mterp" README

NOTE: Find rebuilding instructions at the bottom of this file.


==== Overview ====

Every configuration has a "config-*" file that controls how the sources
are generated.  The sources are written into the "out" directory, where
they are picked up by the Android build system.

The best way to become familiar with the interpreter is to look at the
generated files in the "out" directory.


==== Config file format ====

The config files are parsed from top to bottom.  Each line in the file
may be blank, hold a comment (line starts with '#'), or be a command.

The commands are:

  handler-style <computed-goto|jump-table>

    Specify which style of interpreter to generate.  In computed-goto,
    each handler is allocated a fixed region, allowing transitions to
    be done via table-start-address + (opcode * handler-size). With
    jump-table style, handlers may be of any length, and the generated
    table is an array of pointers to the handlers.  This command is required,
    and must be the first command in the config file.

  handler-size <bytes>

    Specify the size of the fixed region, in bytes.  On most platforms
    this will need to be a power of 2.  For jump-table implementations,
    this command is ignored.

  import <filename>

    The specified file is included immediately, in its entirety.  No
    substitutions are performed.  ".cpp" and ".h" files are copied to the
    C output, ".S" files are copied to the asm output.

  asm-alt-stub <filename>

    When present, this command will cause the generation of an alternate
    set of entry points (for computed-goto interpreters) or an alternate
    jump table (for jump-table interpreters).

  fallback-stub <filename>

    Specifies a file to be used for the special FALLBACK tag on the "op"
    command below.  Intended to be used to transfer control to an alternate
    interpreter to single-step a not-yet-implemented opcode.  Note: should
    note be used on RETURN-class instructions.

  op-start <directory>

    Indicates the start of the opcode list.  Must precede any "op"
    commands.  The specified directory is the default location to pull
    instruction files from.

  op <opcode> <directory>|FALLBACK

    Can only appear after "op-start" and before "op-end".  Overrides the
    default source file location of the specified opcode.  The opcode
    definition will come from the specified file, e.g. "op OP_NOP arm"
    will load from "arm/OP_NOP.S".  A substitution dictionary will be
    applied (see below).  If the special "FALLBACK" token is used instead of
    a directory name, the source file specified in fallback-stub will instead
    be used for this opcode.

  alt <opcode> <directory>

    Can only appear after "op-start" and before "op-end".  Similar to the
    "op" command above, but denotes a source file to override the entry
    in the alternate handler table.  The opcode definition will come from
    the specified file, e.g. "alt OP_NOP arm" will load from
    "arm/ALT_OP_NOP.S".  A substitution dictionary will be applied
    (see below).

  op-end

    Indicates the end of the opcode list.  All kNumPackedOpcodes
    opcodes are emitted when this is seen, followed by any code that
    didn't fit inside the fixed-size instruction handler space.

The order of "op" and "alt" directives are not significant; the generation
tool will extract ordering info from the VM sources.

Typically the form in which most opcodes currently exist is used in
the "op-start" directive.

==== Instruction file format ====

The assembly instruction files are simply fragments of assembly sources.
The starting label will be provided by the generation tool, as will
declarations for the segment type and alignment.  The expected target
assembler is GNU "as", but others will work (may require fiddling with
some of the pseudo-ops emitted by the generation tool).

A substitution dictionary is applied to all opcode fragments as they are
appended to the output.  Substitutions can look like "$value" or "${value}".

The dictionary always includes:

  $opcode - opcode name, e.g. "OP_NOP"
  $opnum - opcode number, e.g. 0 for OP_NOP
  $handler_size_bytes - max size of an instruction handler, in bytes
  $handler_size_bits - max size of an instruction handler, log 2

Both C and assembly sources will be passed through the C pre-processor,
so you can take advantage of C-style comments and preprocessor directives
like "#define".

Some generator operations are available.

  %include "filename" [subst-dict]

    Includes the file, which should look like "arm/OP_NOP.S".  You can
    specify values for the substitution dictionary, using standard Python
    syntax.  For example, this:
      %include "arm/unop.S" {"result":"r1"}
    would insert "arm/unop.S" at the current file position, replacing
    occurrences of "$result" with "r1".

  %default <subst-dict>

    Specify default substitution dictionary values, using standard Python
    syntax.  Useful if you want to have a "base" version and variants.

  %break

    Identifies the split between the main portion of the instruction
    handler (which must fit in "handler-size" bytes) and the "sister"
    code, which is appended to the end of the instruction handler block.
    In jump table implementations, %break is ignored.

The generation tool does *not* print a warning if your instructions
exceed "handler-size", but the VM will abort on startup if it detects an
oversized handler.  On architectures with fixed-width instructions this
is easy to work with, on others this you will need to count bytes.


==== Using C constants from assembly sources ====

The file "art/runtime/asm_support.h" has some definitions for constant
values, structure sizes, and struct member offsets.  The format is fairly
restricted, as simple macros are used to massage it for use with both C
(where it is verified) and assembly (where the definitions are used).

If a constant in the file becomes out of sync, the VM will log an error
message and abort during startup.


==== Development tips ====

If you need to debug the initial piece of an opcode handler, and your
debug code expands it beyond the handler size limit, you can insert a
generic header at the top:

    b       ${opcode}_start
%break
${opcode}_start:

If you already have a %break, it's okay to leave it in place -- the second
%break is ignored.


==== Rebuilding ====

If you change any of the source file fragments, you need to rebuild the
combined source files in the "out" directory.  Make sure the files in
"out" are editable, then:

    $ cd mterp
    $ ./rebuild.sh

The ultimate goal is to have the build system generate the necessary
output files without requiring this separate step, but we're not yet
ready to require Python in the build.

==== Interpreter Control ====

The mterp fast interpreter achieves much of its performance advantage
over the C++ interpreter through its efficient mechanism of
transitioning from one Dalvik bytecode to the next.  Mterp for ARM targets
uses a computed-goto mechanism, in which the handler entrypoints are
located at the base of the handler table + (opcode * 128).

In normal operation, the dedicated register rIBASE
(r8 for ARM, edx for x86) holds a mainHandlerTable.  If we need to switch
to a mode that requires inter-instruction checking, rIBASE is changed
to altHandlerTable.  Note that this change is not immediate.  What is actually
changed is the value of curHandlerTable - which is part of the interpBreak
structure.  Rather than explicitly check for changes, each thread will
blindly refresh rIBASE at backward branches, exception throws and returns.
