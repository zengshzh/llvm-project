#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "vminterpreter.h"

#define VM_REGS 4096

// Saved register values from VMSaveReg, consumed by VMExecute
static uintptr_t gpr[8];

void VMSaveReg(void *r0, void *r1, void *r2, void *r3,
               void *r4, void *r5, void *r6, void *r7) {
    gpr[0] = (uintptr_t)r0;
    gpr[1] = (uintptr_t)r1;
    gpr[2] = (uintptr_t)r2;
    gpr[3] = (uintptr_t)r3;
    gpr[4] = (uintptr_t)r4;
    gpr[5] = (uintptr_t)r5;
    gpr[6] = (uintptr_t)r6;
    gpr[7] = (uintptr_t)r7;
    printf("[VMSaveReg] r0=%p r1=%p r2=%p r3=%p "
           "r4=%p r5=%p r6=%p r7=%p\n",
           (void *)gpr[0], (void *)gpr[1], (void *)gpr[2], (void *)gpr[3],
           (void *)gpr[4], (void *)gpr[5], (void *)gpr[6], (void *)gpr[7]);
}

// ---- VM execution context ----
typedef struct {
    uintptr_t r[VM_REGS];
    uint8_t  *m;
    size_t    mcap;
    uint32_t  vm_sp;
} VMContext;

// ---- instruction printer ----
static void print_insn(const uint8_t *bc, uint32_t off) {
    uint8_t  op   = bc[off];
    uint8_t  flg  = bc[off + 1];
    uint16_t dst  = bc[off + 2] | (uint16_t)bc[off + 3] << 8;
    uint16_t src1 = bc[off + 4] | (uint16_t)bc[off + 5] << 8;
    uint16_t src2 = bc[off + 6] | (uint16_t)bc[off + 7] << 8;
    const char *nat = (flg & 2) ? ".nat" : "";

    switch (op) {
    case VM_ALLOCA:
        if (flg & 1)
            printf("  0x%04X: ALLOCA r%u, r%u\n",      off, dst, src1);
        else
            printf("  0x%04X: ALLOCA r%u, #%u\n",      off, dst, src2);
        break;
    case VM_LOAD:  printf("  0x%04X: LOAD%s r%u, r%u\n",      off, nat, dst, src1); break;
    case VM_STORE: printf("  0x%04X: STORE%s r%u, r%u\n",      off, nat, src1, dst); break;
    case VM_LI:    printf("  0x%04X: LI     r%u, #%u\n",      off, dst, src2); break;
    case VM_ADD:   printf("  0x%04X: ADD    r%u, r%u, r%u\n", off, dst, src1, src2); break;
    case VM_SUB:   printf("  0x%04X: SUB    r%u, r%u, r%u\n", off, dst, src1, src2); break;
    case VM_MUL:   printf("  0x%04X: MUL    r%u, r%u, r%u\n", off, dst, src1, src2); break;
    case VM_UDIV:  printf("  0x%04X: DIV    r%u, r%u, r%u\n", off, dst, src1, src2); break;
    case VM_RET:   printf("  0x%04X: RET    r%u\n",           off, dst); break;
    default:       printf("  0x%04X: ???    (op=%02X)\n",      off, op); break;
    }
}

// ---- debug print (hex + disassemble) ----
// static void disassemble(const uint8_t *bc, uint32_t size) {
//     printf("[disassemble]\n");
//     for (uint32_t off = 0; off + 8 <= size; off += 8)
//         print_insn(bc, off);
// }

static void hexdump(const uint8_t *bc, uint32_t size) {
    printf("[hexdump]\n");
    for (uint32_t off = 0; off + 8 <= size; off += 8)
        printf("  0x%04X: %02X %02X %02X %02X  %02X %02X %02X %02X\n",
               off, bc[off], bc[off+1], bc[off+2], bc[off+3],
               bc[off+4], bc[off+5], bc[off+6], bc[off+7]);
}

// ---- execution engine ----
void *VMExecute(const uint8_t *bc, uint32_t size) {
    hexdump(bc, size);
    printf("=== VM bytecode (%u bytes) ===\n", size);

    VMContext ctx;
    memset(&ctx, 0, sizeof(ctx));

    // Restore argument registers saved by VMSaveReg
    for (int i = 0; i < 8; i++)
        ctx.r[i] = gpr[i];

    void *retval = NULL;

    for (uint32_t pc = 0; pc + 8 <= size; pc += 8) {
        uint8_t  op   = bc[pc];
        uint8_t  flg  = bc[pc + 1];
        uint16_t dst  = bc[pc + 2] | (uint16_t)bc[pc + 3] << 8;
        uint16_t src1 = bc[pc + 4] | (uint16_t)bc[pc + 5] << 8;
        uint16_t src2 = bc[pc + 6] | (uint16_t)bc[pc + 7] << 8;

        print_insn(bc, pc);

        if (dst >= VM_REGS || src1 >= VM_REGS) {
            fprintf(stderr, "[VM] reg bounds at 0x%04X\n", pc);
            goto cleanup;
        }

        unsigned mod_dst = UINT32_MAX;
        uintptr_t mod_val = 0;

        switch (op) {
        case VM_ALLOCA: {
            uint32_t alloc_size = (flg & 1) ? (uint32_t)ctx.r[src1] : src2;
            if (ctx.vm_sp + alloc_size > ctx.mcap) {
                size_t newcap = ctx.mcap ? ctx.mcap * 2 : 4096;
                while (ctx.vm_sp + alloc_size > newcap)
                    newcap *= 2;
                ctx.m = realloc(ctx.m, newcap);
                memset(ctx.m + ctx.mcap, 0, newcap - ctx.mcap);
                ctx.mcap = newcap;
            }
            ctx.r[dst] = ctx.vm_sp;
            ctx.vm_sp += alloc_size;
            mod_dst = dst; mod_val = ctx.r[dst];
            break;
        }
        case VM_LOAD: {
            size_t load_size = (flg & 1) ? sizeof(uintptr_t) : 4;
            if (flg & 2) {
                uintptr_t load_addr = ctx.r[src1];
                if (!load_addr) { fprintf(stderr, "[VM] load null at 0x%04X\n", pc); goto cleanup; }
                memcpy(&ctx.r[dst], (void *)load_addr, load_size);
            } else {
                uintptr_t load_addr = ctx.r[src1];
                if (load_addr + load_size > ctx.mcap) { fprintf(stderr, "[VM] load bounds at 0x%04X\n", pc); goto cleanup; }
                memcpy(&ctx.r[dst], ctx.m + load_addr, load_size);
            }
            mod_dst = dst; mod_val = ctx.r[dst];
            break;
        }
        case VM_STORE: {
            size_t store_size = (flg & 1) ? sizeof(uintptr_t) : 4;
            uintptr_t store_addr = ctx.r[dst];
            if (flg & 2) {
                if (!store_addr) { fprintf(stderr, "[VM] store null at 0x%04X\n", pc); goto cleanup; }
                memcpy((void *)store_addr, &ctx.r[src1], store_size);
            } else {
                if (store_addr + store_size > ctx.mcap) { fprintf(stderr, "[VM] store bounds at 0x%04X\n", pc); goto cleanup; }
                memcpy(ctx.m + store_addr, &ctx.r[src1], store_size);
            }
            printf("  => mem[%u] = %u (0x%X)\n",
                   (uint32_t)store_addr, (uint32_t)ctx.r[src1], (uint32_t)ctx.r[src1]);
            break;
        }
        case VM_LI:
            ctx.r[dst] = src2;
            mod_dst = dst; mod_val = ctx.r[dst];
            break;
        case VM_ADD:
            if (src2 >= VM_REGS) { fprintf(stderr, "[VM] src2 bounds at 0x%04X\n", pc); goto cleanup; }
            ctx.r[dst] = (uint32_t)(ctx.r[src1] + ctx.r[src2]);
            mod_dst = dst; mod_val = ctx.r[dst];
            break;
        case VM_SUB:
            if (src2 >= VM_REGS) { fprintf(stderr, "[VM] src2 bounds at 0x%04X\n", pc); goto cleanup; }
            ctx.r[dst] = (uint32_t)(ctx.r[src1] - ctx.r[src2]);
            mod_dst = dst; mod_val = ctx.r[dst];
            break;
        case VM_MUL:
            if (src2 >= VM_REGS) { fprintf(stderr, "[VM] src2 bounds at 0x%04X\n", pc); goto cleanup; }
            ctx.r[dst] = (uint32_t)(ctx.r[src1] * ctx.r[src2]);
            mod_dst = dst; mod_val = ctx.r[dst];
            break;
        case VM_UDIV:
            if (src2 >= VM_REGS) { fprintf(stderr, "[VM] src2 bounds at 0x%04X\n", pc); goto cleanup; }
            ctx.r[dst] = ctx.r[src2] ? (uint32_t)(ctx.r[src1] / ctx.r[src2]) : 0;
            mod_dst = dst; mod_val = ctx.r[dst];
            break;
        case VM_RET:
            printf("[VM] return: %u (0x%08X)\n", (uint32_t)ctx.r[dst], (uint32_t)ctx.r[dst]);
            retval = (void *)(uintptr_t)ctx.r[dst];
            goto cleanup;
        default:
            fprintf(stderr, "[VM] bad op 0x%02X at 0x%04X\n", op, pc);
            goto cleanup;
        }
        if (mod_dst != UINT32_MAX)
            printf("  => r%u = %u (0x%X)\n", mod_dst, (uint32_t)mod_val, (uint32_t)mod_val);
    }
    fprintf(stderr, "[VM] no RET found\n");

cleanup:
    free(ctx.m);
    return retval;
}
