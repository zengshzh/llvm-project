#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "vminterpreter.h"

#define VM_REGS 4096
#define VM_MEM  65536

// Saved register values from VMSaveReg, consumed by vmexecute
static uint32_t gpr[8];

void VMSaveReg(void *r0, void *r1, void *r2, void *r3,
               void *r4, void *r5, void *r6, void *r7) {
    gpr[0] = (uint32_t)(uintptr_t)r0;
    gpr[1] = (uint32_t)(uintptr_t)r1;
    gpr[2] = (uint32_t)(uintptr_t)r2;
    gpr[3] = (uint32_t)(uintptr_t)r3;
    gpr[4] = (uint32_t)(uintptr_t)r4;
    gpr[5] = (uint32_t)(uintptr_t)r5;
    gpr[6] = (uint32_t)(uintptr_t)r6;
    gpr[7] = (uint32_t)(uintptr_t)r7;
    printf("[VMSaveReg] r0=%08X r1=%08X r2=%08X r3=%08X "
           "r4=%08X r5=%08X r6=%08X r7=%08X\n",
           gpr[0], gpr[1], gpr[2], gpr[3],
           gpr[4], gpr[5], gpr[6], gpr[7]);
}
// ---- debug print (hex + disassemble) ----
static void disassemble(const uint8_t *bc, uint32_t size) {
    printf("[disassemble]\n");
    for (uint32_t off = 0; off + 8 <= size; off += 8) {
        uint8_t  op   = bc[off];
        //uint8_t flg = bc[off + 1];
        uint16_t dst  = bc[off + 2] | (uint16_t)bc[off + 3] << 8;
        uint16_t src1 = bc[off + 4] | (uint16_t)bc[off + 5] << 8;
        uint16_t src2 = bc[off + 6] | (uint16_t)bc[off + 7] << 8;

        switch (op) {
        case 0x00: printf("  0x%04X: ALLOCA r%u, #%u\n",      off, dst, src2); break;
        case 0x01: printf("  0x%04X: LOAD   r%u, r%u\n",      off, dst, src1); break;
        case 0x02: printf("  0x%04X: STORE  r%u, r%u\n",      off, src1, dst); break;
        case 0x03: printf("  0x%04X: LI     r%u, #%u\n",      off, dst, src2); break;
        case 0x10: printf("  0x%04X: ADD    r%u, r%u, r%u\n", off, dst, src1, src2); break;
        case 0x11: printf("  0x%04X: SUB    r%u, r%u, r%u\n", off, dst, src1, src2); break;
        case 0x12: printf("  0x%04X: MUL    r%u, r%u, r%u\n", off, dst, src1, src2); break;
        case 0x13: printf("  0x%04X: DIV    r%u, r%u, r%u\n", off, dst, src1, src2); break;
        case 0x20: printf("  0x%04X: RET    r%u\n",           off, dst); break;
        default:   printf("  0x%04X: ???    (op=%02X)\n",      off, op); break;
        }
    }
}

static void hexdump(const uint8_t *bc, uint32_t size) {
    printf("[hexdump]\n");
    for (uint32_t off = 0; off + 8 <= size; off += 8)
        printf("  0x%04X: %02X %02X %02X %02X  %02X %02X %02X %02X\n",
               off, bc[off], bc[off+1], bc[off+2], bc[off+3],
               bc[off+4], bc[off+5], bc[off+6], bc[off+7]);
}

void VMPrintBytecode(const uint8_t *bc, uint32_t size) {
    printf("=== VM bytecode (%u bytes) ===\n", size);
    hexdump(bc, size);
    disassemble(bc, size);
    printf("===========================\n");
}
// ---- execution engine ----
void VMExecute(const uint8_t *bc, uint32_t size) {
    VMPrintBytecode(bc, size);
    uint32_t r[VM_REGS];
    uint8_t  m[VM_MEM];
    memset(r, 0, sizeof(r));
    memset(m, 0, sizeof(m));
    // Restore argument registers saved by VMSaveReg
    for (int i = 0; i < 8; i++)
        r[i] = gpr[i];

    for (uint32_t pc = 0; pc + 8 <= size; pc += 8) {
        uint8_t  op   = bc[pc];
        //uint8_t flg = bc[pc + 1];
        uint16_t dst  = bc[pc + 2] | (uint16_t)bc[pc + 3] << 8;
        uint16_t src1 = bc[pc + 4] | (uint16_t)bc[pc + 5] << 8;
        uint16_t src2 = bc[pc + 6] | (uint16_t)bc[pc + 7] << 8;

        if (dst >= VM_REGS || src1 >= VM_REGS || src2 >= VM_REGS) {
            fprintf(stderr, "[VM] reg bounds at 0x%04X\n", pc);
            return;
        }

        switch (op) {
        case 0x00: r[dst] = src2; break;                     // ALLOCA
        case 0x01:                                           // LOAD
            if (r[src1] + 4 > VM_MEM) { fprintf(stderr, "[VM] load bounds at 0x%04X\n", pc); return; }
            memcpy(&r[dst], m + r[src1], 4);
            break;
        case 0x02:                                           // STORE
            if (r[dst] + 4 > VM_MEM) { fprintf(stderr, "[VM] store bounds at 0x%04X\n", pc); return; }
            memcpy(m + r[dst], &r[src1], 4);
            break;
        case 0x03: r[dst] = src2; break;                     // LI
        case 0x10: r[dst] = r[src1] + r[src2]; break;       // ADD
        case 0x11: r[dst] = r[src1] - r[src2]; break;       // SUB
        case 0x12: r[dst] = r[src1] * r[src2]; break;       // MUL
        case 0x13:                                           // DIV
            r[dst] = r[src2] ? r[src1] / r[src2] : 0;
            break;
        case 0x20:                                           // RET
            printf("[VM] return: %u (0x%08X)\n", r[dst], r[dst]);
            return;
        default:
            fprintf(stderr, "[VM] bad op 0x%02X at 0x%04X\n", op, pc);
            return;
        }
    }
    fprintf(stderr, "[VM] no RET found\n");
}

