#ifndef VMINTERPRETER_H
#define VMINTERPRETER_H

#include <stdint.h>
#include <stddef.h>

// VM bytecode opcodes
enum VM_Opcode {
  VM_ALLOCA = 0x00,
  VM_LOAD   = 0x01,
  VM_STORE  = 0x02,
  VM_LI     = 0x03,
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
  VM_RET    = 0x20,
};

// Flags
#define VM_FLAG_IMM  4   // bit 2: src2 是 16 位立即数（用于算术/逻辑运算）

void print_insn(const uint8_t *bc, uint32_t off);
void hexdump(const uint8_t *bc, uint32_t size);
void *VMExecute(const uint8_t *bytecode, uint32_t size);
void VMSaveReg(void *r0, void *r1, void *r2, void *r3,
               void *r4, void *r5, void *r6, void *r7);

#endif
