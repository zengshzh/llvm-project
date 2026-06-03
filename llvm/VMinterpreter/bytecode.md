# VM Bytecode Specification

## Instruction Encoding

每条指令固定 **8 字节**，小端序：

```
[opcode(1)][flags(1)][dst(2)][src1(2)][src2(2)]
```

| Offset | Size | Field  | Description            |
|--------|------|--------|------------------------|
| 0      | 1    | opcode | 指令操作码              |
| 1      | 1    | flags  | 标志位（见各指令的 flags 说明） |
| 2-3    | 2    | dst    | 目标寄存器编号           |
| 4-5    | 2    | src1   | 源操作数 1              |
| 6-7    | 2    | src2   | 源操作数 2 / 立即数      |

> **注意：** flags 字节的每个 bit 含义**因指令而异**。例如 bit 0 在 LOAD/STORE 中表示宽度，在 BR 中表示条件取反，在算术指令中无意义。以下各指令详情中均有 flags 说明。

## Register Model

- **r0 – r7**: 函数参数寄存器，由 `VMSaveReg` 在函数入口捕获
- **r8+**: 通用虚拟寄存器，由 bytecode 生成器动态分配和回收

> 寄存器值宽度为 `uintptr_t`（32 位平台 4 字节，64 位平台 8 字节）。
> ALLOCA 返回真实内存地址（`ctx.m + 偏移`），LOAD/STORE 统一通过真实地址访问内存。
> 编码字段 16 位（支持 0–65535）。

### 动态寄存器分配算法

`VMCodeGen.cpp` 中的 `RegisterAllocator` 使用三阶段分配：

**Phase 0 — 数据流分析**
遍历所有 IR 指令的操作数，统计每个 SSA 值被引用的次数（`UseCount`）。

跳过 AllocaInst 结果和 void 指令（不占用虚拟寄存器）。

**Phase 1 — 参数寄存器映射**
将函数的前 8 个参数固定映射到 `r0–r7`（Map 表）。不会进入回收流程。

**Phase 2 — 指令翻译 + 寄存器分配**
顺序翻译每条 IR 指令，同时维护寄存器状态：

| 操作 | 说明 |
|------|------|
| `alloc(V)` | 为 IR 值 `V` 分配寄存器：优先从 `FreeList` 栈顶取用，否则 `NextReg++` |
| `allocRaw()` | 分配临时寄存器（不存入 Map，如 LI 的立即数目标） |
| `consume(V)` | 标记 `V` 被使用了一次：`UseCount[V]--`，如果归零且 `DefBB[V] == 当前 BB`，则回收寄存器到 `FreeList` |
| `freeReg(r)` | 将寄存器号 `r` 推回 `FreeList` 供后续分配 |
| `lookupReg(V)` | 查 Map 获取 `V` 的寄存器号（不改变引用计数） |
| `aliasValue(Alias, Target)` | `Alias`（零偏移 GEP 结果）与 `Target`（基指针）共享同一寄存器，转移 UseCount |

**寄存器回收条件**（必须同时满足）：
1. `UseCount[V]` 归零（所有引用已消耗）
2. `V` 不是 PHINode（循环穿越的值不能回收）
3. **定义 BB == 当前 BB**（跨基本块定义的值可能被循环回边复用，永不回收）

**MaxReg 计算**
`allocRaw()` 每次分配时更新 `MaxReg = max(MaxReg, r + 1)`。初始值 8（含 r0–r7）。
最终 `MaxReg` 作为 `nregs` 参数传入 `VMExecute`，VM 入口按需分配 `calloc(nregs, 8)` 字节。

### 寄存器冲突避免

**GEP 非零偏移**使用 `ADD rdst, rbase, #imm` 单指令（`VM_FLAG_IMM`），不经过 LI 临时寄存器，消除 `rbase` 与临时寄存器碰撞的风险。

**变量偏移 GEP**（如 `arr[i]`，其中 i 为运行时的值）：
1. 扫描 GEP 的每个索引，对每个索引计算 `idx * elem_size`
   - 常量索引：直接发射 `ADD rdst, rbase, #offset`（`VM_FLAG_IMM`）
   - 变量索引：`elem_size == 1` 时发射 `ADD rdst, rbase, ridx`；`elem_size <= 0xFFFF` 时先 `LI tmp, #size` + `MUL tmp, ridx, tmp` 再 `ADD rdst, rdst, tmp`
2. 依次累加各维度的偏移量到临时累加器寄存器
3. 最后将累加器写入 GEP 目标寄存器

示例：`input[i]`（`getelementptr i8, i8* %input, i64 %i`）翻译为：
```
ADD  rtmp, r_input, r_i    ; rtmp = input + i * 1
```

**Phi 节点**的寄存器永不进入 FreeList，确保循环穿越的值在多轮迭代中不被回收。

## Opcode Table

| Opcode | Mnemonic | Format & flags           | Description                          |
|--------|----------|--------------------------|--------------------------------------|
|        | **内存** (0x00–0x0F) | | |
| 0x00   | ALLOCA   | `ALLOCA rdst, #size`     | flags bit0=0: src2 为立即数大小，bit0=1: src1 为存放大小的寄存器 |
| 0x01   | LOAD     | `LOAD rdst, raddr`       | bits0-3=size-1(0→1B…15→16B)，零扩展到 uintptr_t。例: LOAD.1=1B, LOAD.4=4B |
| 0x02   | STORE    | `STORE rval, raddr`      | bits0-3=size-1(同LOAD); bit4忽略 |
| 0x03   | LI       | `LI rdst, #imm`          | 加载 16 位立即数到目标寄存器，flags 不适用 |
| 0x04   | LI32     | `LI32 rdst, #imm32`      | src1\|(src2<<16) → rdst，flags 不适用 |
| 0x05   | MOV      | `MOV rdst, rsrc`         | rdst = rsrc，flags 不适用            |
| 0x06   | CMP      | `CMP rdst, rsrc1, rsrc2` | flags 低4位编码比较谓词(0=EQ…9=SLE)，结果0/1写入rdst |
| 0x07   | FCMP     | `FCMP rdst, rsrc1, rsrc2`| flags 低4位编码比较谓词；bit4=1→double, 0→float |
| 0x08   | JMP      | `JMP #offset`            | pc += (int16_t)src1，flags 不适用    |
| 0x09   | BR       | `BR rcond, #offset`      | rcond≠0 时跳转；bit0=1 → 条件取反(rcond==0跳) |
|        | **函数调用** (0x0A-0x0B) | | |
| 0x0A   | SETARG   | `SETARG slot, rsrc`      | bit0=1→浮点写入`call_args_fp`, 0→整数写入`call_args` |
| 0x0B   | CALL     | `CALL rdst, [func_idx]`  | bit0=1→读XMM0返回(浮点), 0→读RAX；bit1=1→全浮点参数；bit2=1→混合参数 |
|        | **整数算术** (0x10–0x1F) | | |
| 0x10   | ADD      | `ADD rdst, rsrc1, rsrc2` | bit2=1 → src2 为16位立即数，0 → src2 为寄存器 |
| 0x11–0x1C | SUB … XOR | (同 ADD) | 所有整数算术指令复用相同的 flags 编码：bit2=1 为立即数模式 |
|        | **浮点算术** (0x20–0x2F) | | |
| 0x20   | FADD     | `FADD rdst, rsrc1, rsrc2`| bit0=1→double, 0→float；bit2=1→src2为立即数 |
| 0x21–0x23 | FSUB … FDIV | (同 FADD) | 浮点算术复用相同 flags |
|        | **类型转换** (0x30–0x3F) | | |
| 0x30   | SITOFP   | `SITOFP rdst, rsrc`      | bit0=1→double, 0→float              |
| 0x31   | FPTOSI   | `FPTOSI rdst, rsrc`      | bit0=1→double输入, 0→float输入      |
| 0x32   | FPTRUNC  | `FPTRUNC rdst, rsrc`     | double→float，flags 不适用           |
| 0x33   | FPEXT    | `FPEXT rdst, rsrc`       | float→double，flags 不适用           |
| 0x34   | SEXT     | `SEXT rdst, rsrc`        | 符号扩展 i32→intptr，flags 不适用    |
| 0x35   | ZEXT     | `ZEXT rdst, rsrc`        | 零扩展 i32→uintptr，flags 不适用    |
| 0x36   | TRUNC    | `TRUNC rdst, rsrc`       | 截断至 i32，flags 不适用             |
| 0x37   | UITOFP   | `UITOFP rdst, rsrc`      | uint→float/double, bit0=1→double    |
| 0x38   | FPTOUI   | `FPTOUI rdst, rsrc`      | float/double→uint, bit0=1→double输入|
|        | **特殊** | | |
| 0xFF   | RET      | `RET rval`               | 返回 rval，flags 不适用              |

### Instruction Details

**ALLOCA** (0x00)
- 编码: `[0x00][flags(1)][dst(2)][src1(2)][src2(2)]`
- VM 内部维护栈指针（从 0 向上增长），每次 ALLOCA 把当前栈顶地址赋给 `rdst`，栈指针增加分配大小
- **flags bit 0 == 0**: `ALLOCA rdst, #imm` — src2 为立即数大小（编译时已知）
  - 例如：`ALLOCA r8, #256` → r8 = 当前栈顶, 栈指针 += 256
- **flags bit 0 == 1**: `ALLOCA rdst, rsize` — src1 为存放大小的寄存器（运行时 VLA）
  - 例如：`ALLOCA r8, r1` → r8 = 当前栈顶, 栈指针 += r[1]

**LOAD** (0x01)
- 编码: `[0x01][flags(1)][dst(2)][raddr(2)][0]`
- 从 `raddr` 指向的真实地址读取数据，存入 `rdst`
- **bits 0-3**: `size = (flags & 0x0F) + 1`，取值范围 1–16 字节。超过 `sizeof(uintptr_t)` 时截断。
- 宽度 1/2 字节：**零扩展**到 `uintptr_t`。宽度 4 字节：**符号扩展** int32→intptr（对 float 零扩展）。
- 示例：`LOAD.1`=1 字节零扩展，`LOAD.4`=4 字节符号扩展

**STORE** (0x02)
- 编码: `[0x02][flags(1)][raddr(2)][rval(2)][0]`
- 将 `rval` 的值的低 N 字节写入 `raddr` 指向的真实地址
- 注意: dst 字段在此指令中表示地址，src1 表示值
- **bits 0-3**: `size = (flags & 0x0F) + 1`，同 LOAD。bit 4 忽略。

**MOV** (0x05)
- 编码: `[0x05][0][dst(2)][src(2)][0]`
- `rdst = rsrc`，纯寄存器复制，用于 PHI 节点降级

**CMP** (0x06)
- 编码: `[0x06][pred(1)][dst(2)][src1(2)][src2(2)]`
- 比较 `rsrc1` 与 `rsrc2`，结果（0 或 1）写入 `rdst`
- flags 低 4 位编码比较谓词（predicate）：

| 编码 | 谓词  | 条件 |
|------|-------|------|
| 0    | EQ    | `rsrc1 == rsrc2` |
| 1    | NE    | `rsrc1 != rsrc2` |
| 2    | UGT   | `rsrc1 > rsrc2`（无符号） |
| 3    | UGE   | `rsrc1 >= rsrc2`（无符号） |
| 4    | ULT   | `rsrc1 < rsrc2`（无符号） |
| 5    | ULE   | `rsrc1 <= rsrc2`（无符号） |
| 6    | SGT   | `rsrc1 > rsrc2`（有符号） |
| 7    | SGE   | `rsrc1 >= rsrc2`（有符号） |
| 8    | SLT   | `rsrc1 < rsrc2`（有符号） |
| 9    | SLE   | `rsrc1 <= rsrc2`（有符号） |

- src2 始终为寄存器，不支持立即数（避免与 predicate flags 冲突）

**FCMP** (0x07)
- 编码: `[0x07][flags(1)][dst(2)][src1(2)][src2(2)]`
- 比较 `r[src1]` 与 `r[src2]` 的浮点值，结果写入 `rdst`
- flags 低 4 位编码比较谓词，与 CMP 相同（0=EQ … 5=NE, 6=ORD, 7=UNO）
- **flags bit 4** = 1 时比较 `double`（64 位），否则比较 `float`（32 位）
- NaN 处理：ordered 谓词（OEQ/OGT/OGE/OLT/OLE/ONE）在任一操作数为 NaN 时返回 false；unordered 谓词（UEQ/UGT/UGE/ULT/ULE/UNE）在任一操作数为 NaN 时返回 true

**JMP** (0x08)
- 编码: `[0x08][0][0][offset_lo(2)][0]`
- `pc = pc + 8 + (int16_t)src1`，src1 为有符号 16 位相对偏移（相对于下一条指令）
- dst/src2 字段保留（编码为 0）

**BR** (0x09)
- 编码: `[0x09][flags][0][rcond(2)][offset(2)]`
- 若 `r[rcond] != 0` 则 `pc = pc + 8 + (int16_t)src2`，否则 `pc = pc + 8`
- **flags bit 0 (VM_FLAG_BR_NT)**：条件取反。置位时，`r[rcond] == 0` 触发跳转，`r[rcond] != 0` 时继续执行
- dst 字段保留（编码为 0）
- 在 CodeGen 中，条件分支翻译为 **BR + JMP** 两条指令：BR 跳转到真目标，紧接的 JMP 跳转到假目标（避免依赖基本块布局顺序）
- **优化**：当其中一个目标恰好在基本块布局中紧随当前块时，可省略 JMP：
  - 假目标为下一块：仅 BR（条件成立时跳真目标，否则 fall through 到假目标）
  - 真目标为下一块：使用 `BR!`（置位 VM_FLAG_BR_NT），条件成立时不跳转（fall through 到真目标），否则跳假目标
  - 以上两情形均可消除冗余 JMP 指令

**SETARG** (0x0A)
- 编码: `[0x0A][flags(1)][slot(2)][src_reg(2)][0]`
- 设置调用参数槽：`ctx.call_args[dst] = ctx.r[src1]`
- `slot`（dst 字段）取值 0-7，对应 `call_args[slot]`
- **flags bit 0**：参数类型。0 = 整数/指针 → 写入 `call_args[slot]`（通用寄存器），1 = 浮点 → 写入 `call_args_fp[slot]`（XMM 寄存器）
- 对可变参数函数（如 `printf`），浮点参数会额外写入 `call_args[slot]`（满足 Win64 变参 shadow 要求）

**CALL** (0x0B)
- 编码: `[0x0B][flags(1)][ret_reg(2)][func_idx(2)][0]`
- 调用 `func_table[func_idx]`，返回值存入 `ctx.r[ret_reg]`（`ret_reg = 0` 时忽略）
- **flags** 使用 2 个独立位控制参数传递方式和返回类型：

  | bit | 常量 | 含义 |
  |-----|------|------|
  | 0 | `VM_CALL_RET_FP` | 返回类型：0=RAX（整数/指针），1=XMM0（浮点） |
  | 1 | `VM_CALL_ARG_FP` | 参数类型：0=整数/指针，1=浮点（仅当全部参数类型一致时设置） |
  | 2 | `VM_CALL_ARG_MIX` | 参数混合：1=同时存在整数和浮点参数，需用汇编跳板 |

- **6 种分派路径**（4 种函数指针 + 汇编跳板）：

  | flags | 参数 | 返回 | 路径 | 函数指针类型 |
  |-------|------|------|------|------------|
  | 0 | 纯整数 | RAX | **VMCallFn** | `void* (*)(void*×8)` |
  | 1 | 纯整数 | XMM0 | **DblRetCallFn** ← 新！ | `double (*)(void*×8)` |
  | 2 | 纯浮点 | RAX | FPVMCallFn + truncate | `double (*)(double×8)` |
  | 3 | 纯浮点 | XMM0 | **FPVMCallFn** | `double (*)(double×8)` |
  | 4 | 混合 | RAX | 汇编跳板 | `vm_call_trampoline` |
  | 5 | 混合 | XMM0 | 汇编跳板 | `vm_call_trampoline` |

  各路径说明：
  - **VMCallFn**：`void*` 参数全部走通用寄存器，`void*` 返回值读 RAX。纯整数参数 + 整数返回的标准路径。
  - **DblRetCallFn**：`void*` 参数走通用寄存器，但返回值读 **XMM0**。解决 `sin(int)→double` 这类参数是整数但返回值是浮点的场景。
  - **FPVMCallFn**：`double` 参数走 XMM 寄存器，返回值读 XMM0。纯浮点参数的标准路径。返回 RAX 时截断 `double` 到 `int`。
  - **汇编跳板**：当参数同时包含整数和浮点类型时，需要同时设置 GP 和 XMM 寄存器，使用 `vm_call_trampoline` 函数。

  参数通过 `SETARG` 预先设置到 `call_args[0..7]`（整数槽 GP 寄存器）和 `call_args_fp[0..7]`（浮点槽 XMM 寄存器）。CodeGen 根据所有参数类型和返回类型自动选择最佳路径。

### 参数传递机制

`VM_CALL` 使用平台相关的汇编跳板实现正确的硬件调用约定。当前支持三个平台：

| 平台 | 汇编文件 | 整数寄存器 | 浮点寄存器 | 栈参数 |
|------|---------|-----------|-----------|--------|
| Windows x64 | `vmcall_win64.S` | RCX, RDX, R8, R9 | XMM0-XMM3 | 第 5-8 参数 |
| Linux x64 | `vmcall_linux64.S` | RDI, RSI, RDX, RCX, R8, R9 | XMM0-XMM7 | 第 7-8 参数 |
| Linux ARM64 | `vmcall_linux_arm64.S` | X0-X7 | D0-D7 | 第 9+ 参数 |

#### Windows x64 (Microsoft x64 calling convention)

```
C 调用:
  vm_call_trampoline(func, int_args, fp_args, &result)

汇编跳板:
  ┌─ 保存参数（func, int_args, fp_args, result）到栈上
  ├─ 从 int_args[0..3] 加载 RCX, RDX, R8, R9    ← 整数/指针参数
  ├─ 从 int_args[4..7] 拷贝到 [RSP+32..63]        ← 第 5-8 个整数参数（栈上）
  ├─ 从 fp_args[0..3] 加载 XMM0 ~ XMM3          ← 浮点参数
  ├─ call func                                   ← 统一调用
  └─ 存储 RAX → result->int_ret, XMM0 → result->fp_ret
```

寄存器分配：

| 寄存器 | 用途 | 来源 |
|--------|------|------|
| RCX | 第 1 个整数/指针参数 | `call_args[0]` |
| RDX | 第 2 个整数/指针参数 | `call_args[1]` |
| R8 | 第 3 个整数/指针参数 | `call_args[2]` |
| R9 | 第 4 个整数/指针参数 | `call_args[3]` |
| [RSP+32..63] | 第 5-8 个整数/指针参数 | `call_args[4..7]` |
| XMM0 | 第 1 个浮点参数 | `call_args_fp[0]` |
| XMM1 | 第 2 个浮点参数 | `call_args_fp[1]` |
| XMM2 | 第 3 个浮点参数 | `call_args_fp[2]` |
| XMM3 | 第 4 个浮点参数 | `call_args_fp[3]` |
| RAX (返回值) | 整数/指针返回值 | `result->int_ret` |
| XMM0 (返回值) | 浮点返回值 | `result->fp_ret` |

> 对于混合参数（如 `int func(double, int)`），第 1 个浮点参数通过 XMM0 传递，XMM0 对应的 RCX 槽位在 Win64 中未定义。跳板同时设置两种寄存器，函数按类型选择正确的来源。

#### Linux x86-64 (System V AMD64)

| 寄存器 | 用途 | 来源 |
|--------|------|------|
| RDI | 第 1 个整数/指针参数 | `call_args[0]` |
| RSI | 第 2 个整数/指针参数 | `call_args[1]` |
| RDX | 第 3 个整数/指针参数 | `call_args[2]` |
| RCX | 第 4 个整数/指针参数 | `call_args[3]` |
| R8 | 第 5 个整数/指针参数 | `call_args[4]` |
| R9 | 第 6 个整数/指针参数 | `call_args[5]` |
| [RSP+0..15] | 第 7-8 个整数/指针参数 | `call_args[6..7]` |
| XMM0-XMM7 | 第 1-8 个浮点参数 | `call_args_fp[0..7]` |
| RAX (返回值) | 整数/指针返回值 | `result->int_ret` |
| XMM0 (返回值) | 浮点返回值 | `result->fp_ret` |

#### Linux ARM64 (AArch64)

| 寄存器 | 用途 | 来源 |
|--------|------|------|
| X0-X7 | 第 1-8 个整数/指针参数 | `call_args[0..7]` |
| D0-D7 | 第 1-8 个浮点参数 | `call_args_fp[0..7]` |
| X0 (返回值) | 整数/指针返回值 | `result->int_ret` |
| D0 (返回值) | 浮点返回值 | `result->fp_ret` |

**RET** (0xFF)
- 编码: `[0xFF][flags(0)][rval(2)][0][0]`
- 返回 `rval` 中的值

**LI** (0x03)
- 编码: `[0x03][flags(0)][dst(2)][0][immediate(2)]`
- 将 16 位立即数零扩展后加载到 `rdst`

**LI32** (0x04)
- 编码: `[0x04][flags(0)][dst(2)][value_lo(2)][value_hi(2)]`
- 将 32 位立即数加载到 `rdst`，src1 和 src2 拼接为值：`val = src1 | (src2 << 16)`
- 用于 float 常量（位模式直接加载）和 > 16 位的整数常量


## IR → Bytecode Mapping

| LLVM IR         | VM Bytecode   |
|-----------------|---------------|
| `alloca i32`    | `ALLOCA rdst, #size` |
| `load i8, ptr`   | `LOAD.1 rdst, raddr` (size=1, bit4=0 零扩展) |
| `load i32, ptr` | `LOAD.4 rdst, raddr` (size=4, bit4=1 符号扩展) |
| `load float, ptr` | `LOAD.4 rdst, raddr` (size=4, bit4=0 直接复制) |
| `load ptr, ptr` | `LOAD.8 rdst, raddr` (size=8 全宽复制) |
| `store val, ptr`| `STORE rval, raddr` |
| `add` / `sub` / … | `ADD` / `SUB` / … (bit2=1 则 src2 为立即数) |
| `fadd` / …     | `FADD` / … (bit0=1→double, bit2=1→立即数) |
| `ret val`       | `RET rval` |
| `sitofp` / `fptosi` / `uitofp` / `fptoui` | 对应指令 (bit0=1→double) |
| `fptrunc` / `fpext` / `sext` / `zext` / `trunc` | 对应指令，flags 不适用 |
| `ptrtoint` / `inttoptr` / `bitcast` | `MOV rdst, rsrc`（VM 中寄存器不变，无操作） |
| `const int`     | `LI rdst, #imm` |
| `const float`   | `LI32 rdst, #imm32` |
| `icmp`          | `CMP rdst, rsrc1, rsrc2` (pred 编码在 flags 低4位) |
| `fcmp`          | `FCMP rdst, rsrc1, rsrc2` (pred 同 CMP, bit4=1→double) |
| `br label`      | `JMP #offset` |
| `br i1 cond, label, label` | `BR rcond, #offset` (true → BR, false → JMP，相邻目标时优化 JMP)` |
| `phi`           | 降级为 `MOV` 指令，在前驱块末尾插入，当前块跳过生成 |
| `switch val, default, [c1→bb1, ...]` | `LI #c; CMP; BR` 链 + phi 降级 |
| `getelementptr` | 常量偏移 → `ADD rdst, rbase, #imm` (`VM_FLAG_IMM`)；变量偏移 → `MOV rtmp, rbase` + `ADD rtmp, rtmp, ridx`(×elem_size) + `MOV rdst, rtmp` |

**总进度： 9/10 类指令已支持（`█████████░`）**

### 分类进度

| 类别 | 进度 | 说明 |
|------|------|------|
| 内存分配/访问 | ✅ ✅ ✅ ✅ | alloca, load, store, GEP |
| 整数算术 | ✅ ✅ ✅ ✅ ✅ ✅ ✅ | add, sub, mul, udiv, sdiv, urem, srem |
| 整数移位 | ✅ ✅ ✅ | shl, lshr, ashr |
| 整数位运算 | ✅ ✅ ✅ | and, or, xor |
| 浮点算术 | ✅ ✅ ✅ ✅ ❌ | fadd, fsub, fmul, fdiv 使用专用浮点 ALU；frem 暂不支持 |
| 控制流 | ✅ ✅ ❌ ❌ ❌ | br (无条件/条件), phi (MOV 降级) ✅ / switch, select, indirectbr 等 ✗ |
| 比较 | ✅ ✅ | icmp, fcmp |
| 类型转换 | ✅ ✅ ✅ ✅ ✅ ✅ ✅ ✅ ✅ | sitofp, fptosi, fptrunc, fpext, sext, zext, trunc, uitofp, fptoui 全部支持 |
| 聚合操作 | ❌ ❌ ❌ ❌ ❌ | extractvalue, insertvalue, 向量操作 |
| 函数调用 | ✅ ✅ | call (SETARG+CALL 汇编跳板，支持整数/浮点参数及返回值) |

## 未支持的 IR 指令

当前 `VMCodeGen.cpp` 遇到以下指令会触发 `report_fatal_error` 直接终止编译。

### 控制流

| IR 指令 | 说明 | 依赖 |
|---------|------|------|
| `select` | 条件选择 | 待实现 |
| `indirectbr` | 间接跳转 | 较少见 |
| `invoke` / `resume` / `landingpad` | 异常处理 | 复杂，暂不考虑 |

### 类型转换

| IR 指令 | 说明 |
|---------|------|
| `addrspacecast` | ❌ 地址空间转换 |

### 浮点运算

| IR 指令 | 说明 |
|---------|------|
| `frem` | ❌ 浮点取余 |

### 内存内联函数

| IR 指令 | 说明 |
|---------|------|
| `memcpy` / `memmove` | 内存拷贝（`@llvm.memcpy.*`） |
| `memset` | 内存设置（`@llvm.memset.*`） |

### 聚合操作

| IR 指令 | 说明 |
|---------|------|
| `extractvalue` | 从聚合类型取值 |
| `insertvalue` | 设置聚合类型字段 |
| `extractelement` | 向量取值 |
| `insertelement` | 向量设值 |
| `shufflevector` | 向量重排 |

## Runtime Interface

在 VMP 注解函数的入口，LLVM pass 依次插入两个调用：

```
VMSaveReg(r0, r1, ..., r7);           // 捕获 8 个函数参数作为 VM 寄存器初始值
VMExecute(bytecode, size, nregs);     // 执行 bytecode，返回 void*
```

- `VMSaveReg` 将函数参数的运行时值存入 VM 寄存器供 bytecode 使用
- `VMExecute` 开始解释执行 bytecode，第三个参数 `nregs` 指示 VM 上下文需要分配的寄存器数量（**r0–r(nregs-1)**，由 CodeGen 的 Use-count 回收算法计算的最大并发寄存器数）
- 返回值通过 `(rettype)VMExecute(...)` 转换

## 未支持的特性

* 全局变量
* 虚拟内存和真实内存地址的转换（函数传参时触发崩溃）

## Example

源 C 代码：
```c
__attribute__((annotate("VMP")))
int test(int a, int b) {
    int x = a;
    int y = b;
    int s = x + y;
    int p = s * x;
    int q = p / y;
    return q;
}
```

生成的 bytecode：
```
  0x0000: ALLOCA r8, #4        ; x = alloca i32
  0x0008: ALLOCA r9, #4        ; y = alloca i32
  0x0010: ALLOCA r10, #4       ; s = alloca i32
  0x0018: ALLOCA r11, #4       ; p = alloca i32
  0x0020: ALLOCA r12, #4       ; q = alloca i32
  0x0028: STORE  r0, r8        ; x = a (r0)
  0x0030: STORE  r1, r9        ; y = b (r1)
  0x0038: LOAD   r13, r8       ; load x
  0x0040: LOAD   r14, r9       ; load y
  0x0048: ADD    r15, r13, r14 ; s = x + y
  0x0050: STORE  r15, r10      ; save s
  0x0058: LOAD   r16, r8       ; load x
  0x0060: MUL    r17, r15, r16 ; p = s * x
  0x0068: STORE  r17, r11      ; save p
  0x0070: LOAD   r18, r11      ; load p
  0x0078: LOAD   r19, r9       ; load y
  0x0080: DIV    r20, r18, r19 ; q = p / y
  0x0088: STORE  r20, r12      ; save q
  0x0090: LOAD   r21, r12      ; load q
  0x0098: RET    r21
```

引用参数示例（ALLOCA 返回真实地址，LOAD/STORE 统一走原生内存）：
```c
__attribute__((annotate("VMP")))
void test(uint32_t &a, uint32_t &b) {
    int c = (a + b) * a / b;
    a = c;
}
```

生成的 bytecode：
```
  0x0000: ALLOCA r37, #8       ; alloca ptr (reference a)
  0x0008: ALLOCA r38, #8       ; alloca ptr (reference b)
  0x0010: ALLOCA r39, #4       ; c = alloca i32
  0x0018: STORE  r0, r37       ; store reference ptr a to alloca
  0x0020: STORE  r1, r38       ; store reference ptr b to alloca
  0x0028: LOAD   r8, r37       ; r8 = *a (LOAD 统一走真实地址)
  0x0030: LOAD   r9, r38       ; r9 = *b
  0x0038: ADD    r10, r8, r9   ; c = a + b
  ...
  0x0050: STORE  r10, r37      ; *a = c (STORE 统一走真实地址)
```
