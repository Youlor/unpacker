/*
 * Copyright (C) 2012 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "asm_support_mips.S"

    .set noreorder
    .balign 4

    /*
     * Jni dlsym lookup stub.
     */
    .extern artFindNativeMethod
ENTRY art_jni_dlsym_lookup_stub
    addiu $sp, $sp, -48         # leave room for $f12, $f13, $f14, $f15, $a0, $a1, $a2, $a3, and $ra
    .cfi_adjust_cfa_offset 48
    sw    $ra, 32($sp)
    .cfi_rel_offset 31, 32
    SDu   $f14, $f15, 24, $sp, $t0
    SDu   $f12, $f13, 16, $sp, $t0
    sw    $a3, 12($sp)
    .cfi_rel_offset 7, 12
    sw    $a2, 8($sp)
    .cfi_rel_offset 6, 8
    sw    $a1, 4($sp)
    .cfi_rel_offset 5, 4
    sw    $a0, 0($sp)
    .cfi_rel_offset 4, 0
    la    $t9, artFindNativeMethod
    jalr  $t9                   # (Thread*)
    move  $a0, $s1              # pass Thread::Current()
    lw    $a0, 0($sp)           # restore registers from stack
    lw    $a1, 4($sp)
    lw    $a2, 8($sp)
    lw    $a3, 12($sp)
    LDu   $f12, $f13, 16, $sp, $t0
    LDu   $f14, $f15, 24, $sp, $t0
    lw    $ra, 32($sp)
    beq   $v0, $zero, .Lno_native_code_found
    addiu $sp, $sp, 48          # restore the stack
    .cfi_adjust_cfa_offset -48
    move  $t9, $v0              # put method code result in $t9
    jalr  $zero, $t9            # leaf call to method's code
    nop
.Lno_native_code_found:
    jalr  $zero, $ra
    nop
END art_jni_dlsym_lookup_stub
