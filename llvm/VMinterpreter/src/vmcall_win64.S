/* VM CALL trampoline — Windows x64
   Sets up GP registers (RCX,RDX,R8,R9) and XMM registers (XMM0-XMM3)
   from int_args and fp_args arrays, then calls the function.
   Stores results to result->int_ret and result->fp_ret.

   void vm_call_trampoline(void *func, void **int_args, double *fp_args,
                           struct vmcall_result *result);
     RCX = func      RDX = int_args    R8 = fp_args    R9 = result

   Stack layout (RSP must be 16-byte aligned before call):
     After our prologue + call: RSP = orig - 8 (return address)
     After sub rsp, 104: RSP = orig - 112 (aligned to 16)
     [rsp+0..31]   = shadow space (for callee)
     [rsp+32..63]  = extra stack args (for nargs > 4)
     [rsp+64..95]  = saved parameters
     [rsp+96]      = padding byte
*/
.text
.intel_syntax noprefix
.globl  vm_call_trampoline
vm_call_trampoline:
    sub     rsp, 104

    // Save parameters (above shadow + extra args)
    mov     [rsp+64], rcx           // func
    mov     [rsp+72], rdx           // int_args
    mov     [rsp+80], r8            // fp_args
    mov     [rsp+88], r9            // result

    // Load integer args from int_args[]
    mov     r10, [rsp+72]
    mov     rcx, [r10]              // arg0 → RCX
    mov     rdx, [r10+8]            // arg1 → RDX
    mov     r8,  [r10+16]           // arg2 → R8
    mov     r9,  [r10+24]           // arg3 → R9

    // Extra stack args (4-7) go to [rsp+32..63]
    mov     rax, [r10+32]
    mov     [rsp+32], rax
    mov     rax, [r10+40]
    mov     [rsp+40], rax
    mov     rax, [r10+48]
    mov     [rsp+48], rax
    mov     rax, [r10+56]
    mov     [rsp+56], rax

    // Load float args from fp_args[] into XMM registers (args 1-4)
    mov     r10, [rsp+80]
    movsd   xmm0, [r10]
    movsd   xmm1, [r10+8]
    movsd   xmm2, [r10+16]
    movsd   xmm3, [r10+24]

    // Call (RSP is 16-byte aligned here: sub 104 + return addr = 112)
    mov     rax, [rsp+64]
    call    rax

    // Store results
    mov     r10, [rsp+88]
    mov     [r10], rax              // result->int_ret = RAX
    movsd   [r10+8], xmm0           // result->fp_ret = XMM0

    add     rsp, 104
    ret
