// SPDX-License-Identifier: GPL-3.0-or-later
// No game input: a cleanup frame under a catch-all caller, with real CFI/LSDA.
.text
.global _start
_start:
    b caller
    .word mod0
    .space 8
mod0:
    .ascii "MOD0"
    .word 0, 0, 0
    .word __eh_frame_hdr - mod0
    .word 0, 0

.macro frame name, lsda
.p2align 2
.global \name
.type \name, %function
\name:
    .cfi_startproc
    .cfi_personality 0x1b, personality
    .cfi_lsda 0x1b, \lsda
    stp x29, x30, [sp, #-32]!
    .cfi_def_cfa_offset 32
    .cfi_offset 29, -32
    .cfi_offset 30, -24
    mov x29, sp
    .cfi_def_cfa_register 29
    str x19, [sp, #16]
    .cfi_offset 19, -16
.endm

.macro epilogue
    ldr x19, [sp, #16]
    ldp x29, x30, [sp], #32
    ret
.endm

frame caller, caller_lsda
    mov x19, #0x7777
caller_call:
    bl cleanup
caller_call_end:
    bl unexpected_return
    b caller_return
caller_pad:
    bl begin_catch
    bl caught
    bl end_catch
    mov x0, #42
caller_return:
    epilogue
    .cfi_endproc
.size caller, .-caller

frame cleanup, cleanup_lsda
    mov x19, #0x8888
cleanup_call:
    bl throw_exception
cleanup_call_end:
    bl unexpected_return
    epilogue
cleanup_pad:
    mov x19, x0
    bl cleaned
    mov x0, x19
    bl resume_exception
    brk #0
    .cfi_endproc
.size cleanup, .-cleanup

.macro plt name, slot
.p2align 2
.global \name
.type \name, %function
\name:
    adrp x16, \slot
    ldr x17, [x16, :lo12:\slot]
    add x16, x16, :lo12:\slot
    br x17
.size \name, .-\name
.endm

plt throw_exception, got_throw
plt begin_catch, got_begin
plt end_catch, got_end
plt resume_exception, got_resume
plt cleaned, got_cleaned
plt caught, got_caught
plt unexpected_return, got_unexpected
.global personality
.type personality, %function
personality:
    ret
.size personality, .-personality

.section .got,"aw",%progbits
.p2align 3
got_throw: .quad 0
got_begin: .quad 0
got_end: .quad 0
got_resume: .quad 0
got_cleaned: .quad 0
got_caught: .quad 0
got_unexpected: .quad 0

.section .gcc_except_table,"a",%progbits
caller_lsda:
    .byte 0xff, 0x1b
    .uleb128 caller_types - 1f
1:  .byte 1
    .uleb128 caller_cs_end - caller_cs
caller_cs:
    .uleb128 caller_call - caller
    .uleb128 caller_call_end - caller_call
    .uleb128 caller_pad - caller
    .uleb128 1
caller_cs_end:
    .sleb128 1 // positive type filter: catch (...), null type_info below
    .sleb128 0
    .p2align 2
    .word 0
caller_types:

cleanup_lsda:
    .byte 0xff, 0xff, 1
    .uleb128 cleanup_cs_end - cleanup_cs
cleanup_cs:
    .uleb128 cleanup_call - cleanup
    .uleb128 cleanup_call_end - cleanup_call
    .uleb128 cleanup_pad - cleanup
    .uleb128 0 // cleanup, no handler action
cleanup_cs_end:
