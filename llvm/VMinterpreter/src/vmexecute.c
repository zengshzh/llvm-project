#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "vminterpreter.h"

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
    uintptr_t *r;    // dynamically allocated, nregs elements
    uint32_t   nregs;
    uint8_t   *m;
    size_t     mcap;
    uint32_t   vm_sp;
} VMContext;

// ---- helper: resolve src2 (register or immediate) — returns 0 on success ----
static inline int vm_src2(VMContext *ctx, uint8_t flg, uint16_t src2, uint32_t pc, uintptr_t *val) {
    if (flg & VM_FLAG_IMM) { *val = src2; return 0; }
    if (src2 >= ctx->nregs) {
        fprintf(stderr, "[VM] src2 bounds at 0x%04X\n", pc);
        return -1;
    }
    *val = ctx->r[src2];
    return 0;
}

// ---- execution engine ----
void *VMExecute(const uint8_t *bc, uint32_t size, uint32_t nregs) {
    hexdump(bc, size);
    printf("=== VM bytecode (%u bytes, %u regs) ===\n", size, nregs);

    VMContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.nregs = nregs;
    ctx.r = calloc(nregs, sizeof(uintptr_t));

    // Restore argument registers saved by VMSaveReg
    for (int i = 0; i < 8 && i < (int)nregs; i++)
        ctx.r[i] = gpr[i];

    void *retval = NULL;

    for (uint32_t pc = 0; pc + 8 <= size; pc += 8) {
        uint8_t  op   = bc[pc];
        uint8_t  flg  = bc[pc + 1];
        uint16_t dst  = bc[pc + 2] | (uint16_t)bc[pc + 3] << 8;
        uint16_t src1 = bc[pc + 4] | (uint16_t)bc[pc + 5] << 8;
        uint16_t src2 = bc[pc + 6] | (uint16_t)bc[pc + 7] << 8;

        print_insn(bc, pc);

        if (dst >= ctx.nregs || src1 >= ctx.nregs) {
            fprintf(stderr, "[VM] reg bounds at 0x%04X\n", pc);
            goto cleanup;
        }

// Integer binary op: resolves src2, evaluates expr, prints result
#define INT_BINOP(expr) do { \
    uintptr_t _v2_; \
    if (vm_src2(&ctx, flg, src2, pc, &_v2_)) goto cleanup; \
    ctx.r[dst] = (expr); \
    print_reg_result(dst, ctx.r[dst]); \
} while(0)

// Float binary op: bit0=0 → 32-bit float, bit0=1 → 64-bit double
#define FLOAT_BINOP(op) do { \
    uintptr_t _v2_; \
    if (vm_src2(&ctx, flg, src2, pc, &_v2_)) goto cleanup; \
    if (flg & 1) { \
        double da, db; \
        memcpy(&da, &ctx.r[src1], sizeof(double)); \
        memcpy(&db, &_v2_, sizeof(double)); \
        double dr = da op db; \
        memcpy(&ctx.r[dst], &dr, sizeof(double)); \
        print_reg_result(dst, ctx.r[dst]); \
    } else { \
        float fa, fb; \
        memcpy(&fa, &ctx.r[src1], 4); \
        memcpy(&fb, &_v2_, 4); \
        float fr = fa op fb; \
        memcpy(&ctx.r[dst], &fr, 4); \
        print_reg_result(dst, ctx.r[dst]); \
    } \
} while(0)

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
            print_reg_result(dst, ctx.r[dst]);
            break;
        }
        case VM_LOAD: {
            size_t load_size = (flg & 1) ? sizeof(uintptr_t) : 4;
            void *src_ptr;
            if (flg & 2) {
                uintptr_t load_addr = ctx.r[src1];
                if (!load_addr) { fprintf(stderr, "[VM] load null at 0x%04X\n", pc); goto cleanup; }
                src_ptr = (void *)load_addr;
            } else {
                uintptr_t load_addr = ctx.r[src1];
                if (load_addr + load_size > ctx.mcap) { fprintf(stderr, "[VM] load bounds at 0x%04X\n", pc); goto cleanup; }
                src_ptr = ctx.m + load_addr;
            }
            if (load_size == 4 && !(flg & VM_FLAG_FLOAT)) {
                int32_t tmp;
                memcpy(&tmp, src_ptr, 4);
                ctx.r[dst] = (intptr_t)tmp;  // sign-extend int32→intptr
            } else {
                memcpy(&ctx.r[dst], src_ptr, load_size);  // float or full-width
            }
            print_reg_result(dst, ctx.r[dst]);
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
            printf("  => mem[%zu] = %zu (0x%zX)\n",
                   store_addr, ctx.r[src1], ctx.r[src1]);
            break;
        }
        case VM_LI:
            ctx.r[dst] = src2;
            print_reg_result(dst, ctx.r[dst]);
            break;
        case VM_LI32:
            ctx.r[dst] = (uint32_t)src1 | ((uint32_t)src2 << 16);
            print_reg_result(dst, ctx.r[dst]);
            break;
        case VM_SITOFP: {
            int32_t tmp;
            memcpy(&tmp, &ctx.r[src1], 4);
            if (flg & 1) {
                double d = (double)tmp;
                memcpy(&ctx.r[dst], &d, sizeof(double));
            } else {
                float f = (float)tmp;
                memcpy(&ctx.r[dst], &f, 4);
            }
            print_reg_result(dst, ctx.r[dst]);
            break;
        }
        case VM_FPTOSI: {
            int32_t i;
            if (flg & 1) {
                double d; memcpy(&d, &ctx.r[src1], sizeof(double));
                i = (int32_t)d;
            } else {
                float f; memcpy(&f, &ctx.r[src1], 4);
                i = (int32_t)f;
            }
            ctx.r[dst] = (uintptr_t)(intptr_t)i;
            print_reg_result(dst, ctx.r[dst]);
            break;
        }
        case VM_FPTRUNC: {
            double d; memcpy(&d, &ctx.r[src1], sizeof(double));
            float f = (float)d;
            memcpy(&ctx.r[dst], &f, 4);
            print_reg_result(dst, ctx.r[dst]);
            break;
        }
        case VM_FPEXT: {
            float f; memcpy(&f, &ctx.r[src1], 4);
            double d = (double)f;
            memcpy(&ctx.r[dst], &d, sizeof(double));
            print_reg_result(dst, ctx.r[dst]);
            break;
        }
        case VM_FADD:  { FLOAT_BINOP(+); break; }
        case VM_FSUB:  { FLOAT_BINOP(-); break; }
        case VM_FMUL:  { FLOAT_BINOP(*); break; }
        case VM_FDIV:  { FLOAT_BINOP(/); break; }
        case VM_ADD:   { INT_BINOP(ctx.r[src1] + _v2_); break; }
        case VM_SUB:   { INT_BINOP(ctx.r[src1] - _v2_); break; }
        case VM_MUL:   { INT_BINOP(ctx.r[src1] * _v2_); break; }
        case VM_UDIV:  { INT_BINOP(_v2_ ? (ctx.r[src1] / _v2_) : 0); break; }
        case VM_SDIV:  { INT_BINOP(_v2_ ? (uintptr_t)((intptr_t)ctx.r[src1] / (intptr_t)_v2_) : 0); break; }
        case VM_UREM:  { INT_BINOP(_v2_ ? (ctx.r[src1] % _v2_) : 0); break; }
        case VM_SREM:  { INT_BINOP(_v2_ ? (uintptr_t)((intptr_t)ctx.r[src1] % (intptr_t)_v2_) : 0); break; }
        case VM_SHL:   { INT_BINOP(ctx.r[src1] << (_v2_ & (sizeof(uintptr_t) * 8 - 1))); break; }
        case VM_LSHR:  { INT_BINOP(ctx.r[src1] >> (_v2_ & (sizeof(uintptr_t) * 8 - 1))); break; }
        case VM_ASHR:  { INT_BINOP((uintptr_t)((intptr_t)ctx.r[src1] >> (_v2_ & (sizeof(uintptr_t) * 8 - 1)))); break; }
        case VM_AND:   { INT_BINOP(ctx.r[src1] & _v2_); break; }
        case VM_OR:    { INT_BINOP(ctx.r[src1] | _v2_); break; }
        case VM_XOR:   { INT_BINOP(ctx.r[src1] ^ _v2_); break; }
        case VM_RET:
            printf("[VM] return: %zu (0x%zX)\n", ctx.r[dst], ctx.r[dst]);
            retval = (void *)(uintptr_t)ctx.r[dst];
            goto cleanup;
        default:
            fprintf(stderr, "[VM] bad op 0x%02X at 0x%04X\n", op, pc);
            goto cleanup;
        }
    }
    fprintf(stderr, "[VM] no RET found\n");
cleanup:
    free(ctx.r);
    free(ctx.m);
    return retval;
}
