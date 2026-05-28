#ifndef VMINTERPRETER_H
#define VMINTERPRETER_H

#include <stdint.h>
#include <stddef.h>

void vmprint(void);
void VMExecute(const uint8_t *bytecode, uint32_t size);
void VMSaveReg(void *r0, void *r1, void *r2, void *r3,
               void *r4, void *r5, void *r6, void *r7);

#endif
