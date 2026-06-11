#ifndef VMINTERPRETER_H
#define VMINTERPRETER_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// VM bytecode opcodes
// Memory:         0x00–0x0F
// Integer arith:  0x10–0x1F
// Float arith:    0x20–0x2F
// Type convert:   0x30–0x3F
// Special:        0xFF
enum VM_Opcode {
  // ── Memory ──
  VM_ALLOCA = 0x00,
  VM_LOAD   = 0x01,
  VM_STORE  = 0x02,
  VM_LI     = 0x03,
  VM_LI32   = 0x04,
  VM_MOV    = 0x05,   // rdst = rsrc1
  VM_CMP    = 0x06,   // int compare: rdst = (rsrc1 pred rsrc2) ? 1 : 0
  VM_FCMP   = 0x07,   // float compare: rdst = (rsrc1 pred rsrc2) ? 1 : 0
  VM_JMP    = 0x08,   // pc += (int16_t)src1
  VM_BR     = 0x09,   // if rsrc1 != 0 then pc += (int16_t)src2
  VM_SETARG = 0x0A,   // ctx.call_args[dst] = ctx.r[src1]
  VM_CALL   = 0x0B,   // call func_table[src1], ret → rdst
  VM_INVOKE_PREP = 0x0C, // set unwind_pc = dst | (src1 << 16) for next CALL
  VM_LPAD   = 0x0D,   // landingpad: ctx.r[dst] = exception handle
  VM_RESUME = 0x0E,   // rethrow exception from ctx.r[src1]

  // ── Integer arithmetic ──
  VM_ADD    = 0x10,
  VM_SUB    = 0x11,
  VM_MUL    = 0x12,
  VM_UDIV   = 0x13,
  VM_SDIV   = 0x14,
  VM_UREM   = 0x15,
  VM_SREM   = 0x16,
  VM_SHL    = 0x17,
  VM_LSHR   = 0x18,
  VM_ASHR   = 0x19,
  VM_AND    = 0x1A,
  VM_OR     = 0x1B,
  VM_XOR    = 0x1C,

  // ── Float arithmetic ──
  VM_FADD   = 0x20,
  VM_FSUB   = 0x21,
  VM_FMUL   = 0x22,
  VM_FDIV   = 0x23,

  // ── Type conversion ──
  VM_SITOFP = 0x30,
  VM_FPTOSI = 0x31,
  VM_FPTRUNC = 0x32,
  VM_FPEXT  = 0x33,
  VM_SEXT   = 0x34,  // sign-extend lower 32 bits → full uintptr_t
  VM_ZEXT   = 0x35,  // zero-extend lower 32 bits → full uintptr_t
  VM_TRUNC  = 0x36,  // truncate to lower 32 bits (zero upper)
  VM_UITOFP = 0x37,  // unsigned int → float/double
  VM_FPTOUI = 0x38,  // float/double → unsigned int

  // ── Special ──
  VM_RET    = 0xFF,
};

// Flags
#define VM_FLAG_IMM   4   // bit 2: src2 是 16 位立即数（用于算术/逻辑运算）
#define VM_FLAG_FLOAT 8   // bit 3: 通用浮点标志（类型转换双精度等）
#define VM_FLAG_SEXT  0x10 // bit 4: LOAD/STORE 符号扩展（否则零扩展）
#define VM_FLAG_BR_NT 1   // bit 0: BR 条件取反（条件为 0 时跳转）

// CALL 标志位（与通用 flags 共用字节）
#define VM_CALL_RET_FP   1   // bit 0: 返回值在 XMM0（浮点），否则 RAX（整数）
#define VM_CALL_ARG_FP   2   // bit 1: 参数全为浮点 → FPVMCallFn
#define VM_CALL_ARG_MIX  4   // bit 2: 参数混合整数+浮点 → libffi
#define VM_CALL_INVOKE   8   // bit 3: 此 CALL 来自 invoke（try/catch 包裹）

void print_insn(const uint8_t *bc, uint32_t off);
void hexdump(const uint8_t *bc, uint32_t size);
void print_reg_result(unsigned reg, uintptr_t val);
void print_vm_header(uint32_t size, uint32_t nregs);
void print_vmsave(const uintptr_t gpr[8]);
void print_store_mem(uintptr_t addr, uintptr_t val);
void print_vm_ret(uintptr_t val);
void print_vm_error(const char *fmt, ...);
uintptr_t VMExecute(const uint8_t *bytecode, uint32_t size, uint32_t nregs,
                void (**func_table)(void), uint32_t func_count,
                const uintptr_t *global_init, uint32_t num_globals);
void VMSaveReg(uintptr_t r0, uintptr_t r1, uintptr_t r2, uintptr_t r3,
               uintptr_t r4, uintptr_t r5, uintptr_t r6, uintptr_t r7);

#ifdef __cplusplus
}
#endif

#endif
