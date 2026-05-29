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
  VM_RET    = 0x20,
};

void vmprint(void);
void VMExecute(const uint8_t *bytecode, uint32_t size);
void VMSaveReg(void *r0, void *r1, void *r2, void *r3,
               void *r4, void *r5, void *r6, void *r7);

#endif
