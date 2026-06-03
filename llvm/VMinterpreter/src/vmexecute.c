#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stddef.h>
#include <math.h>
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
    print_vmsave(gpr);
}

// Overflow block for VLA (variable-length ALLOCA)
typedef struct VMMemBlock {
    struct VMMemBlock *next;
    uint8_t           *mem;
    size_t             size;
} VMMemBlock;

// ---- VM execution context ----
typedef struct {
    uintptr_t *r;    // dynamically allocated, nregs elements
    uint32_t   nregs;
    uint8_t   *m;           // main block: pre-allocated from bytecode scan
    size_t     mcap;        // main block capacity
    VMMemBlock *blocks;     // linked list of VLA overflow blocks
    uint32_t   vm_sp;       // bump offset in main block
    void      *call_args[8];      // integer/pointer arg slots
    double     call_args_fp[8];   // float/double arg slots
    double     ret_fp;            // float/double return value
} VMContext;

// ---- helper: resolve src2 (register or immediate) �?returns 0 on success ----
static inline int vm_src2(VMContext *ctx, uint8_t flg, uint16_t src2, uint32_t pc, uintptr_t *val) {
    if (flg & VM_FLAG_IMM) { *val = src2; return 0; }
    if (src2 >= ctx->nregs) {
        fprintf(stderr, "[VM] src2 bounds at 0x%04X\n", pc);
        return -1;
    }
    *val = ctx->r[src2];
    return 0;
}

// 4 universal call types for dispatch without assembly
// Type 1: int args → GP regs, int ret ← RAX
typedef void *(*VMCallFn)(void*, void*, void*, void*,
                          void*, void*, void*, void*);
// Type 2: int args → GP regs, fp ret ← XMM0
typedef double (*DblRetCallFn)(void*, void*, void*, void*,
                               void*, void*, void*, void*);
// Type 3: fp args → XMM regs, int ret ← RAX (rare, truncate)
// Type 4: fp args → XMM regs, fp ret ← XMM0
typedef double (*FPVMCallFn)(double, double, double, double,
                             double, double, double, double);

#define ARGS8(a)  a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]
// Assembly trampoline for mixed int/fp arguments
struct vmcall_result {
    uintptr_t int_ret;
    double    fp_ret;
};
extern void vm_call_trampoline(void *func, void **int_args,
                               double *fp_args,
                               struct vmcall_result *result);

// ---- execution engine ----
void *VMExecute(const uint8_t *bc, uint32_t size, uint32_t nregs,
                void (**func_table)(void), uint32_t func_count,
                const uintptr_t *global_init, uint32_t num_globals) {
    hexdump(bc, size);
    print_vm_header(size, nregs);

    VMContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.nregs = nregs;
    ctx.r = calloc(nregs, sizeof(uintptr_t));

    // Pre-scan bytecode for fixed-size ALLOCAs, sum them up so we allocate
    // exactly once — no realloc needed (it would invalidate returned pointers).
    // VLA (variable-length) ALLOCAs fall back to separate overflow blocks.
    {
      size_t need = 0;
      for (uint32_t pc = 0; pc + 8 <= size; pc += 8) {
        if (bc[pc] == VM_ALLOCA && !(bc[pc + 1] & 1)) {
          uint16_t sz = bc[pc + 6] | (uint16_t)bc[pc + 7] << 8;
          need += sz;
        }
      }
      if (need) {
        ctx.mcap = need;
        ctx.m = calloc(1, need);
      }
    }

    // Restore argument registers saved by VMSaveReg
    for (int i = 0; i < 8 && i < (int)nregs; i++)
        ctx.r[i] = gpr[i];

    // Load global variable addresses from {reg, value} pair table.
    // global_init alternates: [reg0, val0, reg1, val1, ...]
    for (uint32_t i = 0; i + 1 < num_globals * 2 && i + 1 < (uint32_t)nregs * 2; i += 2) {
        unsigned r = (unsigned)global_init[i];
        if (r < (uint32_t)nregs)
            ctx.r[r] = global_init[i + 1];
    }

    void *retval = NULL;

    for (uint32_t pc = 0; pc + 8 <= size; ) {
        uint8_t  op   = bc[pc];
        uint8_t  flg  = bc[pc + 1];
        uint16_t dst  = bc[pc + 2] | (uint16_t)bc[pc + 3] << 8;
        uint16_t src1 = bc[pc + 4] | (uint16_t)bc[pc + 5] << 8;
        uint16_t src2 = bc[pc + 6] | (uint16_t)bc[pc + 7] << 8;

        print_insn(bc, pc);

        // Special instructions that don't use standard register fields
        if (op == VM_JMP || op == VM_BR || op == VM_SETARG || op == VM_CALL) {
            // Bounds check only the register-referencing fields
            if (op == VM_SETARG && src1 > 0 && src1 >= ctx.nregs) {
                fprintf(stderr, "[VM] src1 bounds at 0x%04X\n", pc); goto cleanup;
            }
            if (op == VM_CALL && dst > 0 && dst >= ctx.nregs) {
                fprintf(stderr, "[VM] dst bounds at 0x%04X\n", pc); goto cleanup;
            }
        } else {
            if (dst >= ctx.nregs || src1 >= ctx.nregs) {
                fprintf(stderr, "[VM] reg bounds at 0x%04X\n", pc);
                goto cleanup;
            }
        }

// Integer binary op: resolves src2, evaluates expr, prints result
#define INT_BINOP(expr) do { \
    uintptr_t _v2_ = 0; \
    if (vm_src2(&ctx, flg, src2, pc, &_v2_)) goto cleanup; \
    ctx.r[dst] = (expr); \
    print_reg_result(dst, ctx.r[dst]); \
} while(0)

// Float binary op: bit0=0 → 32-bit float, bit0=1 → 64-bit double
#define FLOAT_BINOP(op) do { \
    uintptr_t _v2_ = 0; \
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
            if (flg & 1) {
              // VLA: allocate an overflow block (can't live in main bump arena)
              VMMemBlock *blk = calloc(1, sizeof(VMMemBlock));
              blk->mem = calloc(1, alloc_size);
              blk->size = alloc_size;
              blk->next = ctx.blocks;
              ctx.blocks = blk;
              ctx.r[dst] = (uintptr_t)blk->mem;
            } else {
              if (ctx.vm_sp + alloc_size > ctx.mcap) {
                fprintf(stderr, "[VM] ALLOCA oom at 0x%04X (need %zu, cap %zu)\n",
                        pc, ctx.vm_sp + (size_t)alloc_size, ctx.mcap);
                goto cleanup;
              }
              ctx.r[dst] = (uintptr_t)(ctx.m + ctx.vm_sp);
              ctx.vm_sp += alloc_size;
            }
            print_reg_result(dst, ctx.r[dst]);
            break;
        }
        case VM_LOAD: {
            uintptr_t load_addr = ctx.r[src1];
            if (!load_addr) { fprintf(stderr, "[VM] load null at 0x%04X\n", pc); goto cleanup; }
            unsigned load_size = (flg & 0x0F) + 1;          // bits 0-3: size-1
            if (load_size > sizeof(uintptr_t)) load_size = sizeof(uintptr_t);

            // Zero-extend: copy bytes into zeroed temp
            uintptr_t tmp = 0;
            memcpy(&tmp, (void *)load_addr, load_size);

            // Sign-extend for 4-byte integer loads (i32 → intptr), so that
            // negative i32 values (e.g. higher bit set) are correctly preserved
            // in subsequent signed arithmetic (ADD, ASHR, etc.).
            if (load_size == 4 && !(flg & VM_FLAG_FLOAT)) {
                int32_t sx = (int32_t)tmp;
                ctx.r[dst] = (uintptr_t)sx;
            } else {
                ctx.r[dst] = tmp;
            }
            print_reg_result(dst, ctx.r[dst]);
            break;
        }
        case VM_STORE: {
            unsigned store_size = (flg & 0x0F) + 1;         // bits 0-3: size-1
            if (store_size > sizeof(uintptr_t)) store_size = sizeof(uintptr_t);
            uintptr_t store_addr = ctx.r[dst];
            if (!store_addr) { fprintf(stderr, "[VM] store null at 0x%04X\n", pc); goto cleanup; }
            memcpy((void *)store_addr, &ctx.r[src1], store_size);
            print_store_mem(store_addr, ctx.r[src1]);
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
        case VM_SEXT: {
            int32_t tmp; memcpy(&tmp, &ctx.r[src1], 4);
            ctx.r[dst] = (intptr_t)tmp;
            print_reg_result(dst, ctx.r[dst]);
            break;
        }
        case VM_ZEXT: {
            uint32_t tmp; memcpy(&tmp, &ctx.r[src1], 4);
            ctx.r[dst] = tmp;
            print_reg_result(dst, ctx.r[dst]);
            break;
        }
        case VM_TRUNC: {
            ctx.r[dst] = (uint32_t)ctx.r[src1];
            print_reg_result(dst, ctx.r[dst]);
            break;
        }
        case VM_UITOFP: {
            uint32_t tmp; memcpy(&tmp, &ctx.r[src1], 4);
            if (flg & 1) {
                double d = (double)tmp; memcpy(&ctx.r[dst], &d, 8);
            } else {
                float f = (float)tmp; memcpy(&ctx.r[dst], &f, 4);
            }
            print_reg_result(dst, ctx.r[dst]);
            break;
        }
        case VM_FPTOUI: {
            uint32_t ui;
            if (flg & 1) {
                double d; memcpy(&d, &ctx.r[src1], 8); ui = (uint32_t)d;
            } else {
                float f; memcpy(&f, &ctx.r[src1], 4); ui = (uint32_t)f;
            }
            ctx.r[dst] = ui;
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
        case VM_MOV:
            ctx.r[dst] = ctx.r[src1];
            print_reg_result(dst, ctx.r[dst]);
            break;
        case VM_CMP: {
            if (src2 >= ctx.nregs) {
                fprintf(stderr, "[VM] src2 bounds at 0x%04X\n", pc);
                goto cleanup;
            }
            uintptr_t a = ctx.r[src1], b = ctx.r[src2];
            uint8_t pred = flg & 0x0F;
            uintptr_t r = 0;
            switch (pred) {
            case 0:  r = (a == b) ? 1 : 0; break;                       // EQ
            case 1:  r = (a != b) ? 1 : 0; break;                       // NE
            case 2:  r = (a >  b) ? 1 : 0; break;                       // UGT
            case 3:  r = (a >= b) ? 1 : 0; break;                       // UGE
            case 4:  r = (a <  b) ? 1 : 0; break;                       // ULT
            case 5:  r = (a <= b) ? 1 : 0; break;                       // ULE
            case 6:  r = ((intptr_t)a >  (intptr_t)b) ? 1 : 0; break;  // SGT
            case 7:  r = ((intptr_t)a >= (intptr_t)b) ? 1 : 0; break;  // SGE
            case 8:  r = ((intptr_t)a <  (intptr_t)b) ? 1 : 0; break;  // SLT
            case 9:  r = ((intptr_t)a <= (intptr_t)b) ? 1 : 0; break;  // SLE
            default:
                fprintf(stderr, "[VM] bad cmp pred %u at 0x%04X\n", pred, pc);
                goto cleanup;
            }
            ctx.r[dst] = r;
            print_reg_result(dst, r);
            break;
        }
        case VM_FCMP: {
#define FCMP_BODY(T, sz) do { T a,b; memcpy(&a,&ctx.r[src1],sz); memcpy(&b,&ctx.r[src2],sz); \
    uint8_t _p=flg&0x0F; uintptr_t _r=0; \
    switch(_p) { \
    case 0:_r=(a==b)?1:0;break; case 1:_r=(a>b)?1:0;break; case 2:_r=(a>=b)?1:0;break; \
    case 3:_r=(a<b)?1:0;break; case 4:_r=(a<=b)?1:0;break; case 5:_r=(a!=b)?1:0;break; \
    case 6:_r=(!isnan(a)&&!isnan(b))?1:0;break; case 7:_r=(isnan(a)||isnan(b))?1:0;break; \
    case 8:_r=(a==b||isnan(a)||isnan(b))?1:0;break; case 9:_r=(a>b||isnan(a)||isnan(b))?1:0;break; \
    case 10:_r=(a>=b||isnan(a)||isnan(b))?1:0;break; case 11:_r=(a<b||isnan(a)||isnan(b))?1:0;break; \
    case 12:_r=(a<=b||isnan(a)||isnan(b))?1:0;break; case 13:_r=(a!=b||isnan(a)||isnan(b))?1:0;break; \
    default:fprintf(stderr,"[VM] bad fcmp pred %u\n",_p);goto cleanup; \
    } ctx.r[dst]=_r; \
} while(0)
            if (flg & 0x10) FCMP_BODY(double, 8);
            else             FCMP_BODY(float, 4);
            print_reg_result(dst, ctx.r[dst]);
            break;
#undef FCMP_BODY
        }
        case VM_JMP: {
            int32_t rel = (int32_t)(int16_t)src1;
            pc = pc + 8 + rel;
            continue;
        }
        case VM_BR: {
            if (src1 >= ctx.nregs) {
                fprintf(stderr, "[VM] src1 bounds at 0x%04X\n", pc);
                goto cleanup;
            }
            int32_t rel = (int32_t)(int16_t)src2;
            int cond = (ctx.r[src1] != 0);
            if (flg & VM_FLAG_BR_NT) cond = !cond;
            if (cond)
                pc = pc + 8 + rel;
            else
                pc = pc + 8;
            continue;
        }
        case VM_SETARG:
            if (dst < 8) {
                if (flg & 1)  // float/double arg
                    memcpy(&ctx.call_args_fp[dst], &ctx.r[src1], sizeof(double));
                else          // integer/pointer arg
                    ctx.call_args[dst] = (void *)ctx.r[src1];
            }
            break;
        case VM_CALL: {
            if (src1 >= func_count || !func_table) {
                fprintf(stderr, "[VM] bad func idx %u at 0x%04X\n", src1, pc);
                goto cleanup;
            }
            void *func = func_table[src1];

            if (flg & VM_CALL_ARG_MIX) {
                // ── Mixed int+fp args: use assembly trampoline (case 5-6) ──
                struct vmcall_result res;
                vm_call_trampoline(func, ctx.call_args, ctx.call_args_fp, &res);
                if (dst) {
                    if (flg & VM_CALL_RET_FP) memcpy(&ctx.r[dst], &res.fp_ret, 8);
                    else                      ctx.r[dst] = res.int_ret;
                }
            } else if (flg & VM_CALL_ARG_FP) {
                // ── Pure fp args: use FPVMCallFn (case 3-4) ──
                FPVMCallFn fn = (FPVMCallFn)func;
                double r = fn(ARGS8(ctx.call_args_fp));
                if (dst) {
                    if (flg & VM_CALL_RET_FP)
                        memcpy(&ctx.r[dst], &r, 8);
                    else
                        ctx.r[dst] = (uintptr_t)(intptr_t)(int32_t)r;  // truncate double→int
                }
            } else {
                // ── Pure int args: use VMCallFn or DblRetCallFn (case 1-2) ──
                if (flg & VM_CALL_RET_FP) {
                    // Case 2: int args + fp ret → DblRetCallFn (reads XMM0)
                    DblRetCallFn fn = (DblRetCallFn)func;
                    double r = fn(ARGS8(ctx.call_args));
                    if (dst) memcpy(&ctx.r[dst], &r, 8);
                } else {
                    // Case 1: int args + int ret → VMCallFn (reads RAX)
                    VMCallFn fn = (VMCallFn)func;
                    void *r = fn(ARGS8(ctx.call_args));
                    if (dst) ctx.r[dst] = (uintptr_t)r;
                }
            }
            memset(ctx.call_args, 0, sizeof(ctx.call_args));
            memset(ctx.call_args_fp, 0, sizeof(ctx.call_args_fp));
            if (dst) print_reg_result(dst, ctx.r[dst]);
            break;
        }
        case VM_RET:
            print_vm_ret(ctx.r[dst]);
            retval = (void *)(uintptr_t)ctx.r[dst];
            goto cleanup;
        default:
            fprintf(stderr, "[VM] bad op 0x%02X at 0x%04X\n", op, pc);
            goto cleanup;
        }
        pc += 8;
    }
    fprintf(stderr, "[VM] no RET found\n");
cleanup:
    {
        VMMemBlock *blk = ctx.blocks;
        while (blk) {
            VMMemBlock *next = blk->next;
            free(blk->mem);
            free(blk);
            blk = next;
        }
    }
    free(ctx.r);
    free(ctx.m);
    return retval;
}
