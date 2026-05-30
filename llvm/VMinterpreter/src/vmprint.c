#include <stdio.h>
#include "vminterpreter.h"

// ---- instruction printer ----
void print_insn(const uint8_t *bc, uint32_t off) {
    uint8_t  op   = bc[off];
    uint8_t  flg  = bc[off + 1];
    uint16_t dst  = bc[off + 2] | (uint16_t)bc[off + 3] << 8;
    uint16_t src1 = bc[off + 4] | (uint16_t)bc[off + 5] << 8;
    uint16_t src2 = bc[off + 6] | (uint16_t)bc[off + 7] << 8;
    const char *nat = (flg & 2) ? ".nat" : "";

    switch (op) {
    case VM_ALLOCA:
        if (flg & 1)
            printf("  0x%04X: ALLOCA r%u, r%u  ",      off, dst, src1);
        else
            printf("  0x%04X: ALLOCA r%u, #%u  ",      off, dst, src2);
        break;
    case VM_LOAD:  printf("  0x%04X: LOAD%s r%u, r%u  ",      off, nat, dst, src1); break;
    case VM_STORE: printf("  0x%04X: STORE%s r%u, r%u  ",      off, nat, src1, dst); break;
    case VM_LI:    printf("  0x%04X: LI     r%u, #%u  ",      off, dst, src2); break;
    case VM_ADD:   printf("  0x%04X: ADD    r%u, r%u, %s%u  ", off, dst, src1, (flg & VM_FLAG_IMM)?"#":"r", src2); break;
    case VM_SUB:   printf("  0x%04X: SUB    r%u, r%u, %s%u  ", off, dst, src1, (flg & VM_FLAG_IMM)?"#":"r", src2); break;
    case VM_MUL:   printf("  0x%04X: MUL    r%u, r%u, %s%u  ", off, dst, src1, (flg & VM_FLAG_IMM)?"#":"r", src2); break;
    case VM_UDIV:  printf("  0x%04X: UDIV   r%u, r%u, %s%u  ", off, dst, src1, (flg & VM_FLAG_IMM)?"#":"r", src2); break;
    case VM_SDIV:  printf("  0x%04X: SDIV   r%u, r%u, %s%u  ", off, dst, src1, (flg & VM_FLAG_IMM)?"#":"r", src2); break;
    case VM_UREM:  printf("  0x%04X: UREM   r%u, r%u, %s%u  ", off, dst, src1, (flg & VM_FLAG_IMM)?"#":"r", src2); break;
    case VM_SREM:  printf("  0x%04X: SREM   r%u, r%u, %s%u  ", off, dst, src1, (flg & VM_FLAG_IMM)?"#":"r", src2); break;
    case VM_SHL:   printf("  0x%04X: SHL    r%u, r%u, %s%u  ", off, dst, src1, (flg & VM_FLAG_IMM)?"#":"r", src2); break;
    case VM_LSHR:  printf("  0x%04X: LSHR   r%u, r%u, %s%u  ", off, dst, src1, (flg & VM_FLAG_IMM)?"#":"r", src2); break;
    case VM_ASHR:  printf("  0x%04X: ASHR   r%u, r%u, %s%u  ", off, dst, src1, (flg & VM_FLAG_IMM)?"#":"r", src2); break;
    case VM_AND:   printf("  0x%04X: AND    r%u, r%u, %s%u  ", off, dst, src1, (flg & VM_FLAG_IMM)?"#":"r", src2); break;
    case VM_OR:    printf("  0x%04X: OR     r%u, r%u, %s%u  ", off, dst, src1, (flg & VM_FLAG_IMM)?"#":"r", src2); break;
    case VM_XOR:   printf("  0x%04X: XOR    r%u, r%u, %s%u  ", off, dst, src1, (flg & VM_FLAG_IMM)?"#":"r", src2); break;
    case VM_RET:   printf("  0x%04X: RET    r%u\n",           off, dst); break;
    default:       printf("  0x%04X: ???    (op=%02X)\n",      off, op); break;
    }
}

void hexdump(const uint8_t *bc, uint32_t size) {
    printf("[hexdump]\n");
    for (uint32_t off = 0; off + 8 <= size; off += 8)
        printf("  0x%04X: %02X %02X %02X %02X  %02X %02X %02X %02X\n",
               off, bc[off], bc[off+1], bc[off+2], bc[off+3],
               bc[off+4], bc[off+5], bc[off+6], bc[off+7]);
}
