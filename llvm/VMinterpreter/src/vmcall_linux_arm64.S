/* VM CALL trampoline — Linux ARM64 (AArch64)
   Sets up GP registers (X0-X7) and FP/SIMD registers (D0-D7) from
   int_args and fp_args arrays, then calls the function.
   Stores results to result->int_ret (X0) and result->fp_ret (D0).

   void vm_call_trampoline(void *func, void **int_args, double *fp_args,
                           struct vmcall_result *result);
     X0 = func      X1 = int_args    X2 = fp_args    X3 = result

   AArch64 calling convention:
     Integer args 1-8: X0 - X7
     Float   args 1-8: D0 - D7 (lower 64 bits of V0-V7)
     Extra args on stack (starting at SP at call time)
     X0 = integer return, D0 = float/double return
     SP must be 16-byte aligned before call
*/
.text
.globl  vm_call_trampoline
vm_call_trampoline:
    // Save frame and allocate stack: 8*4 (regs) + 8*8 (fp) = 96, align to 16
    stp     x29, x30, [sp, #-16]!
    mov     x29, sp
    sub     sp, sp, #80

    // Save input parameters (they overlap with call argument registers)
    str     x0, [x29, #-8]           // func
    str     x1, [x29, #-16]          // int_args
    str     x2, [x29, #-24]          // fp_args
    str     x3, [x29, #-32]          // result

    // Load 8 integer args from int_args into X0-X7
    ldr     x10, [x29, #-16]         // int_args
    ldp     x0, x1, [x10]
    ldp     x2, x3, [x10, #16]
    ldp     x4, x5, [x10, #32]
    ldp     x6, x7, [x10, #48]

    // Extra stack args (if nargs > 8) — ARM64 passes 9th+ on stack
    // Currently unused, stack space reserved if needed

    // Load 8 float args from fp_args into D0-D7
    ldr     x10, [x29, #-24]         // fp_args
    ldp     d0, d1, [x10]
    ldp     d2, d3, [x10, #16]
    ldp     d4, d5, [x10, #32]
    ldp     d6, d7, [x10, #48]

    // Call (SP is 16-byte aligned: 16 for stp + 80 sub = 96 = 6*16)
    ldr     x10, [x29, #-8]          // func
    blr     x10

    // Store results
    ldr     x10, [x29, #-32]         // result
    str     x0, [x10]                // result->int_ret = X0
    str     d0, [x10, #8]            // result->fp_ret = D0

    add     sp, sp, #80
    ldp     x29, x30, [sp], #16
    ret
