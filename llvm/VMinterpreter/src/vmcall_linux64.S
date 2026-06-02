/* VM CALL trampoline — Linux x86-64 (System V AMD64)
   Sets up GP registers (RDI,RSI,RDX,RCX,R8,R9) and XMM registers (XMM0-XMM7)
   from int_args and fp_args arrays, then calls the function.
   Stores results to result->int_ret (RAX) and result->fp_ret (XMM0).

   void vm_call_trampoline(void *func, void **int_args, double *fp_args,
                           struct vmcall_result *result);
     RDI = func      RSI = int_args    RDX = fp_args    RCX = result

   System V AMD64 calling convention:
     Integer args 1-6: RDI, RSI, RDX, RCX, R8, R9
     Float   args 1-8: XMM0 - XMM7
     Extra args on stack (starting at RSP at call time)
     RAX = integer return, XMM0 = float/double return
     RSP must be 16-byte aligned before call
*/
.text
.intel_syntax noprefix
.globl  vm_call_trampoline
vm_call_trampoline:
    push    rbx
    push    rbp
    mov     rbp, rsp
    sub     rsp, 40                 // align to 16: total = 8+8+8+40 = 64

    // Save function parameters (use rbp-relative addressing)
    mov     [rbp-24], rdi           // func
    mov     [rbp-32], rsi           // int_args
    mov     [rbp-40], rdx           // fp_args
    mov     [rbp-48], rcx           // result

    // Load 6 integer args from int_args
    mov     r10, [rbp-32]
    mov     rdi, [r10]              // arg0
    mov     rsi, [r10+8]            // arg1
    mov     rdx, [r10+16]           // arg2
    mov     rcx, [r10+24]           // arg3
    mov     r8,  [r10+32]           // arg4
    mov     r9,  [r10+40]           // arg5

    // Extra stack args 6-7 (if nargs > 6)
    mov     rax, [r10+48]
    mov     [rsp], rax
    mov     rax, [r10+56]
    mov     [rsp+8], rax

    // Load 8 float args from fp_args into XMM0-XMM7
    mov     r10, [rbp-40]
    movsd   xmm0, [r10]
    movsd   xmm1, [r10+8]
    movsd   xmm2, [r10+16]
    movsd   xmm3, [r10+24]
    movsd   xmm4, [r10+32]
    movsd   xmm5, [r10+40]
    movsd   xmm6, [r10+48]
    movsd   xmm7, [r10+56]

    // Call (RSP is 16-byte aligned: 8 call + 8 push + 8 push + 40 sub = 64)
    mov     rax, [rbp-24]
    call    rax

    // Store results
    mov     r10, [rbp-48]
    mov     [r10], rax
    movsd   [r10+8], xmm0

    mov     rsp, rbp
    pop     rbp
    pop     rbx
    ret
