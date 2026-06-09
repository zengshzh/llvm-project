#include <stdio.h>
#include <stdarg.h>
#include "vminterpreter.h"

// Define VM_SILENT before including this file to disable ALL output
#define VM_SILENT
#ifdef VM_SILENT
#define PRINTF(...) ((void)0)
#else
#define PRINTF(...) printf(__VA_ARGS__)
#endif

// ---- instruction printer ----
void print_insn(const uint8_t *bc, uint32_t off) {
    uint8_t  op   = bc[off];
    uint8_t  flg  = bc[off + 1];
    uint16_t dst  = bc[off + 2] | (uint16_t)bc[off + 3] << 8;
    uint16_t src1 = bc[off + 4] | (uint16_t)bc[off + 5] << 8;
    uint16_t src2 = bc[off + 6] | (uint16_t)bc[off + 7] << 8;
    (void)dst; (void)src1; (void)src2;
    switch (op) {
    case VM_ALLOCA:
        if (flg & 1)
            PRINTF("  0x%04X: ALLOCA r%u, r%u  ",      off, dst, src1);
        else
            PRINTF("  0x%04X: ALLOCA r%u, #%u  ",      off, dst, src2);
        break;
    case VM_LOAD:  {
        unsigned sz = (flg & 0x0F) + 1;
        (void)sz;
        PRINTF("  0x%04X: LOAD.%u r%u, r%u",  off, sz, dst, src1);
        break;
    }
    case VM_STORE: {
        unsigned sz = (flg & 0x0F) + 1;
        (void)sz;
        PRINTF("  0x%04X: STORE.%u r%u, r%u",   off, sz, src1, dst);
        break;
    }
    case VM_LI:    PRINTF("  0x%04X: LI     r%u, #%u  ",      off, dst, src2); break;
    case VM_LI32:  PRINTF("  0x%04X: LI32   r%u, #%u  ",      off, dst, (uint32_t)src1 | ((uint32_t)src2 << 16)); break;
    case VM_SITOFP: PRINTF("  0x%04X: SITOFP r%u, r%u  ",     off, dst, src1); break;
    case VM_FPTOSI: PRINTF("  0x%04X: FPTOSI r%u, r%u  ",     off, dst, src1); break;
    case VM_FPTRUNC: PRINTF("  0x%04X: FPTRUNC r%u, r%u  ",   off, dst, src1); break;
    case VM_FPEXT:  PRINTF("  0x%04X: FPEXT  r%u, r%u  ",     off, dst, src1); break;
    case VM_SEXT:   PRINTF("  0x%04X: SEXT   r%u, r%u  ",     off, dst, src1); break;
    case VM_ZEXT:   PRINTF("  0x%04X: ZEXT   r%u, r%u  ",     off, dst, src1); break;
    case VM_TRUNC:  PRINTF("  0x%04X: TRUNC  r%u, r%u  ",     off, dst, src1); break;
    case VM_UITOFP: PRINTF("  0x%04X: UITOFP r%u, r%u  ",     off, dst, src1); break;
    case VM_FPTOUI: PRINTF("  0x%04X: FPTOUI r%u, r%u  ",     off, dst, src1); break;
    case VM_FADD:  PRINTF("  0x%04X: FADD   r%u, r%u, %s%u  ", off, dst, src1, (flg & VM_FLAG_IMM)?"#":"r", src2); break;
    case VM_FSUB:  PRINTF("  0x%04X: FSUB   r%u, r%u, %s%u  ", off, dst, src1, (flg & VM_FLAG_IMM)?"#":"r", src2); break;
    case VM_FMUL:  PRINTF("  0x%04X: FMUL   r%u, r%u, %s%u  ", off, dst, src1, (flg & VM_FLAG_IMM)?"#":"r", src2); break;
    case VM_FDIV:  PRINTF("  0x%04X: FDIV   r%u, r%u, %s%u  ", off, dst, src1, (flg & VM_FLAG_IMM)?"#":"r", src2); break;
    case VM_ADD:   PRINTF("  0x%04X: ADD    r%u, r%u, %s%u  ", off, dst, src1, (flg & VM_FLAG_IMM)?"#":"r", src2); break;
    case VM_SUB:   PRINTF("  0x%04X: SUB    r%u, r%u, %s%u  ", off, dst, src1, (flg & VM_FLAG_IMM)?"#":"r", src2); break;
    case VM_MUL:   PRINTF("  0x%04X: MUL    r%u, r%u, %s%u  ", off, dst, src1, (flg & VM_FLAG_IMM)?"#":"r", src2); break;
    case VM_UDIV:  PRINTF("  0x%04X: UDIV   r%u, r%u, %s%u  ", off, dst, src1, (flg & VM_FLAG_IMM)?"#":"r", src2); break;
    case VM_SDIV:  PRINTF("  0x%04X: SDIV   r%u, r%u, %s%u  ", off, dst, src1, (flg & VM_FLAG_IMM)?"#":"r", src2); break;
    case VM_UREM:  PRINTF("  0x%04X: UREM   r%u, r%u, %s%u  ", off, dst, src1, (flg & VM_FLAG_IMM)?"#":"r", src2); break;
    case VM_SREM:  PRINTF("  0x%04X: SREM   r%u, r%u, %s%u  ", off, dst, src1, (flg & VM_FLAG_IMM)?"#":"r", src2); break;
    case VM_SHL:   PRINTF("  0x%04X: SHL    r%u, r%u, %s%u  ", off, dst, src1, (flg & VM_FLAG_IMM)?"#":"r", src2); break;
    case VM_LSHR:  PRINTF("  0x%04X: LSHR   r%u, r%u, %s%u  ", off, dst, src1, (flg & VM_FLAG_IMM)?"#":"r", src2); break;
    case VM_ASHR:  PRINTF("  0x%04X: ASHR   r%u, r%u, %s%u  ", off, dst, src1, (flg & VM_FLAG_IMM)?"#":"r", src2); break;
    case VM_AND:   PRINTF("  0x%04X: AND    r%u, r%u, %s%u  ", off, dst, src1, (flg & VM_FLAG_IMM)?"#":"r", src2); break;
    case VM_OR:    PRINTF("  0x%04X: OR     r%u, r%u, %s%u  ", off, dst, src1, (flg & VM_FLAG_IMM)?"#":"r", src2); break;
    case VM_XOR:   PRINTF("  0x%04X: XOR    r%u, r%u, %s%u  ", off, dst, src1, (flg & VM_FLAG_IMM)?"#":"r", src2); break;
    case VM_MOV:   PRINTF("  0x%04X: MOV    r%u, r%u\n",       off, dst, src1); break;
    case VM_CMP:   PRINTF("  0x%04X: CMP    r%u, r%u, r%u  ", off, dst, src1, src2);
                   PRINTF("(pred=%u)", flg & 0x0F); break;
    case VM_FCMP:  PRINTF("  0x%04X: FCMP   r%u, r%u, r%u  ", off, dst, src1, src2);
                   PRINTF("(pred=%u%s)", flg & 0x0F, (flg & 0x10)?" d":""); break;
    case VM_JMP:   PRINTF("  0x%04X: JMP    #%+d\n",          off, (int16_t)src1); break;
    case VM_BR:    PRINTF("  0x%04X: BR%s    r%u, #%+d\n",    off, (flg & VM_FLAG_BR_NT)?"!":"", src1, (int16_t)src2); break;
    case VM_SETARG: PRINTF("  0x%04X: SETARG%s r%u, r%u\n", off, (flg & 1)?"fp ":"   ", dst, src1); break;
    case VM_CALL:  PRINTF("  0x%04X: CALL%s%s r%u, [%u]\n", off,
        (flg & VM_CALL_ARG_MIX)?"mx":(flg & VM_CALL_ARG_FP)?"fp":"  ",
        (flg & VM_CALL_RET_FP)?"r":" ", dst, src1); break;
    case VM_RET:   PRINTF("  0x%04X: RET    r%u\n",           off, dst); break;
    default:       PRINTF("  0x%04X: ???    (op=%02X)\n",      off, op); break;
    }
}

void hexdump(const uint8_t *bc, uint32_t size) {
    PRINTF("=== [hexdump] ===\n");
    for (uint32_t off = 0; off + 8 <= size; off += 8)
        PRINTF("  0x%04X: %02X %02X %02X %02X  %02X %02X %02X %02X\n",
               off, bc[off], bc[off+1], bc[off+2], bc[off+3],
               bc[off+4], bc[off+5], bc[off+6], bc[off+7]);
    PRINTF("=== [VM ASM START] ===\n");
    for (uint32_t pc = 0; pc + 8 <= size; pc+=8){
        print_insn(bc, pc);
        PRINTF("\n");
    }
    PRINTF("=== [VM ASM END] ===\n"); 
}

void print_reg_result(unsigned reg, uintptr_t val) {
    PRINTF("  => r%u = %zu (0x%zX)\n", reg, val, val);
}

void print_vm_header(uint32_t size, uint32_t nregs) {
    PRINTF("=== VM bytecode (%u bytes, %u regs) ===\n", size, nregs);
}

void print_vmsave(const uintptr_t gpr[8]) {
    PRINTF("[VMSaveReg] r0=%p r1=%p r2=%p r3=%p "
           "r4=%p r5=%p r6=%p r7=%p\n",
           (void *)gpr[0], (void *)gpr[1], (void *)gpr[2], (void *)gpr[3],
           (void *)gpr[4], (void *)gpr[5], (void *)gpr[6], (void *)gpr[7]);
}

void print_store_mem(uintptr_t addr, uintptr_t val) {
    PRINTF("  => mem[%zu] = %zu (0x%zX)\n", addr, val, val);
}

void print_vm_ret(uintptr_t val) {
    PRINTF("[VM] return: %zu (0x%zX)\n", val, val);
}

void print_vm_error(const char *fmt, ...) {
#ifdef VM_SILENT
    (void)fmt;
#else
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
#endif
}
