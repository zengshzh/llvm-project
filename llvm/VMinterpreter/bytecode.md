# VM Bytecode Specification

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

### compile cmd
```
clang test.cpp -lvminterpreter -lffi -o test
```
clang，libvminterpreter.a是由本项目编译出来的，libffi.a是开源第三方库

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
- 编码: `[0x0B][flags(1)][ret_reg(2)][func_idx(2)][arg_info(1)][type_mask(1)]`
- 调用 `func_table[func_idx]`，返回值存入 `ctx.r[ret_reg]`（`ret_reg = 0` 时忽略）

- **flags** 使用 3 个独立位控制参数传递方式和返回类型：

  | bit | 常量 | 含义 |
  |-----|------|------|
  | 0 | `VM_CALL_RET_FP` | 返回类型：0=RAX（整数/指针），1=XMM0（浮点） |
  | 1 | `VM_CALL_ARG_FP` | 参数全为浮点 → `FPVMCallFn` |
  | 2 | `VM_CALL_ARG_MIX` | 参数混合整数+浮点 → libffi |
  | 3 | `VM_CALL_INVOKE` | 此 CALL 来自 invoke，用 try/catch 包裹以捕获 C++ 异常 |

- **6 种分派路径**（4 种函数指针 + libffi）：

  | flags | 参数 | 返回 | 路径 | 说明 |
  |-------|------|------|------|------|
  | 0 | 纯整数 | RAX | **VMCallFn** | `void* (*)(void*×8)` |
  | 1 | 纯整数 | XMM0 | **DblRetCallFn** | `double (*)(void*×8)` |
  | 2 | 纯浮点 | RAX | FPVMCallFn + truncate | `double (*)(double×8)` |
  | 3 | 纯浮点 | XMM0 | **FPVMCallFn** | `double (*)(double×8)` |
  | 4 | 混合 | RAX | **libffi** | `ffi_call`，非变参用 `ffi_prep_cif`，变参用 `ffi_prep_cif_var` |
  | 5 | 混合 | XMM0 | **libffi** | 同上 |

- **混合模式（flags bit2=1）下 bytes 6-7**：
  - `byte 6` 低 4 位 = 参数总个数（0-8，截断到 8）
  - `byte 6` 高 4 位 = 固定参数个数（可变参数函数的前 N 个命名参数，非可变参数时等于总个数）
  - `byte 7` = 类型掩码（bit i = 1 表示第 i 个参数为浮点，0 为整数/指针）
- **非混合模式下 bytes 6-7** 被忽略

  各路径说明：
  - **VMCallFn**：`void*` 参数全部走通用寄存器，`void*` 返回值读 RAX。纯整数参数 + 整数返回的标准路径。
  - **DblRetCallFn**：`void*` 参数走通用寄存器，但返回值读 **XMM0**。解决 `sin(int)→double` 这类参数是整数但返回值是浮点的场景。
  - **FPVMCallFn**：`double` 参数走 XMM 寄存器，返回值读 XMM0。纯浮点参数的标准路径。返回 RAX 时截断 `double` 到 `int`。
  - **libffi**：混合参数时，根据 byte 6 中的参数信息（总个数 + 固定参数个数 + 类型掩码），非变参调用 `ffi_prep_cif`，变参调用 `ffi_prep_cif_var`，然后通过 `ffi_call` 执行。libffi 自动处理各平台的调用约定（寄存器分配、栈布局等），无需平台相关的汇编代码。

  参数通过 `SETARG` 预先设置到 `call_args[0..7]`（整数槽 GP 寄存器）和 `call_args_fp[0..7]`（浮点槽 XMM 寄存器）。CodeGen 根据所有参数类型和返回类型自动选择最佳路径。

### 参数传递机制

`VM_CALL` 根据参数类型选择不同的调用路径：
- **纯整数/纯浮点参数**：使用 C 函数指针直接调用（`VMCallFn` / `DblRetCallFn` / `FPVMCallFn`），编译器自动生成正确的调用约定。
- **混合参数（整数+浮点）**：使用 **libffi** 库动态处理平台相关的调用约定。

libffi 接管了原来由平台相关汇编跳板（`vmcall_win64.S`、`vmcall_linux64.S`、`vmcall_linux_arm64.S`）处理的寄存器分配和栈布局工作。`libffi` 通过字节码中编码的参数信息（参数个数、固定参数个数、类型掩码），自动为每个目标平台生成正确的调用序列：

- **非可变参数**：调用 `ffi_prep_cif` 描述函数签名，再通过 `ffi_call` 执行
- **可变参数**：调用 `ffi_prep_cif_var`，区分固定参数与可变参数部分，libffi 自动处理各平台的可变参数传递规则（如 Win64 中可变浮点参数走整数寄存器/栈）

参数通过 `SETARG` 预先设置到 `call_args[0..7]`（整数槽）和 `call_args_fp[0..7]`（浮点槽），libffi 从对应的数组中读取每个参数的值。

**INVOKE_PREP** (0x0C)
- 编码: `[0x0C][0][unwind_pc_lo(2)][unwind_pc_hi(2)][0]`
- 设置异常跳转目标：`ctx.unwind_pc = dst | ((uint32_t)src1 << 16)`
- 紧随其后是 `SETARG` + `CALL`（带上 `VM_CALL_INVOKE` 标志），若外部调用抛出 C++ 异常，VM 捕获后将 PC 跳转到 `unwind_pc` 执行 landingpad 清理链
- `dst` + `src1` 组成 32 位绝对字节码偏移量，由 CodeGen Phase 3 回填

**LPAD** (0x0D)
- 编码: `[0x0D][0][dst(2)][0][0]`
- 将 `ctx.exc`（`std::exception_ptr`，被捕获的异常）序列化为 `uintptr_t` 写入 `ctx.r[dst]`
- selector（`{ptr, i32}` 的第二个字段）对于 cleanup-only landingpad 恒为 0，由后续 `extractvalue index=1 → LI 0` 处理

**RESUME** (0x0E)
- 编码: `[0x0E][0][0][exc_reg(2)][0]`
- 从 `ctx.r[src1]` 反序列化 `std::exception_ptr`，调用 `std::rethrow_exception()` 重新抛出异常
- 若异常句柄为空（不应发生），报错并终止 VM

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
| `getelementptr` | 普通基址：常量偏移→`ADD rdst, rbase, #imm`；变量偏移→`MOV rtmp, rbase` + … |
| | 全局变量基址：`MOV rdst, r_global`（被零偏移 GEP 优化掉时通过 CALL 参数直接引用）|
| `invoke` | `INVOKE_PREP + SETARG + CALL` (flags bit3=1 表示 invoke)，正常目标 fall-through 或 `JMP` |
| `landingpad` | `LPAD rdst`（存储序列化的 `std::exception_ptr`） |
| `resume` | `RESUME rexc`（反序列化 `std::exception_ptr` 并 `std::rethrow_exception`） |
| `unreachable` | 无操作（静默跳过，死代码路径在运行时不可达，如 `__cxa_throw` 之后） |
| `extractvalue {ptr,i32}, 0` | `MOV rdst, ragg`（提取异常指针） |
| `extractvalue {ptr,i32}, 1` | `LI rdst, 0`（cleanup-only selector 恒为 0） |
| `insertvalue {ptr,i32}, val, idx` | `MOV rdst, rval`（传播异常句柄；仅支持 cleanup-only 模式） |

**总进度： 10/10 类指令已支持（`██████████`）**

### 分类进度

| 类别 | 进度 | 说明 |
|------|------|------|
| 内存分配/访问 | ✅ ✅ ✅ ✅ | alloca, load, store, GEP |
| 整数算术 | ✅ ✅ ✅ ✅ ✅ ✅ ✅ | add, sub, mul, udiv, sdiv, urem, srem |
| 整数移位 | ✅ ✅ ✅ | shl, lshr, ashr |
| 整数位运算 | ✅ ✅ ✅ | and, or, xor |
| 浮点算术 | ✅ ✅ ✅ ✅ ❌ | fadd, fsub, fmul, fdiv 使用专用浮点 ALU；frem 暂不支持 |
| 控制流 | ✅ ✅ ✅ ❌ ❌ | br (无条件/条件), phi (MOV 降级), invoke ✅ / switch, select, indirectbr 等 ✗ |
| 比较 | ✅ ✅ | icmp, fcmp |
| 类型转换 | ✅ ✅ ✅ ✅ ✅ ✅ ✅ ✅ ✅ | sitofp, fptosi, fptrunc, fpext, sext, zext, trunc, uitofp, fptoui 全部支持 |
| 聚合操作 | 🟡 ❌ ❌ ❌ ❌ | extractvalue/insertvalue (仅 cleanup-only exception 路径) / 向量操作 |
| 函数调用 | ✅ ✅ ✅ | call (SETARG+CALL) + invoke (INVOKE_PREP+SETARG+CALL+try/catch) |

## 未支持的 IR 指令

当前 `VMCodeGen.cpp` 遇到以下指令会触发 `report_fatal_error` 直接终止编译。

### 控制流

| IR 指令 | 说明 | 依赖 |
|---------|------|------|
| `unreachable` | ✅ 已支持 | 静默跳过（死代码路径，如 `__cxa_throw` 之后），不产生字节码 |
| `select` | ❌ 条件选择 | 待实现 |
| `indirectbr` | ❌ 间接跳转 | 较少见 |

### 异常处理（部分支持）

| IR 指令 | 支持状态 | 说明 |
|---------|---------|------|
| `invoke` | ✅ 已支持 | 翻译为 `INVOKE_PREP + SETARG + CALL` (VM_CALL_INVOKE 标志)，VM 用 try/catch 包裹调用。仅支持直接调用路径（纯 int/fp 参数）；libffi 混合参数路径的异常可能无法被捕获。 |
| `landingpad` | 🟡 部分支持 | 仅支持 `cleanup` 类型（无 catch 子句）。`catch` 类型需要集成 `__cxa_begin_catch` / RTTI 类型匹配，暂不支持。 |
| `resume` | ✅ 已支持 | 翻译为 `VM_RESUME`，调用 `std::rethrow_exception()` 重新抛出异常。 |

### 聚合操作（部分支持）

| IR 指令 | 支持状态 | 说明 |
|---------|---------|------|
| `extractvalue` | 🟡 部分支持 | 仅支持来自 landingpad 的 `{ptr, i32}` 聚合值。index 0 → `VM_MOV` 提取异常指针，index 1 → `VM_LI 0`（cleanup-only selector）。 |
| `insertvalue` | 🟡 部分支持 | 仅支持 resume 前的 `{ptr, i32}` 组合，通过 `VM_MOV` 传播异常句柄。 |
| `extractelement` | ❌ | 向量取值 |
| `insertelement` | ❌ | 向量设值 |
| `shufflevector` | ❌ | 向量重排 |

### 类型转换

| IR 指令 | 说明 |
|---------|------|
| `addrspacecast` | ❌ 地址空间转换 |

### 浮点运算

| IR 指令 | 说明 |
|---------|------|
| `frem` | ❌ 浮点取余 |

### 内存内联函数

| IR 指令 | 支持状态 | 说明 |
|---------|---------|------|
| `lifetime.start` / `lifetime.end` | ✅ | 生命周期标记（VM 中为空操作，跳过不处理） |
| `memcpy` / `memmove` | ✅ | `@llvm.memcpy.*` / `@llvm.memmove.*` → 映射为 libc `memcpy` / `memmove`，通过函数表调用 |
| `memset` | ✅ | `@llvm.memset.*` → 映射为 libc `memset`，通过函数表调用 |

> 注：
> - `@llvm.lifetime.start/end` 开启 `-O3` 后由 LLVM 自动插入，CodeGen 直接跳过不生成字节码，也不再加入函数表。
> - 内存内置函数（`@llvm.memcpy/memmove/memset`）无法在运行时通过函数指针直接调用，且其声明被放入函数表后会产生 `ConstantExpr` 引用，导致后续 `PreISelIntrinsicLowering` pass 崩溃。CodeGen 在翻译时将其替换为对应对 libc 函数名，确保函数表中始终使用真实的外部函数。

### 聚合操作

| IR 指令 | 说明 |
|---------|------|
| `extractelement` | 向量取值 |
| `insertelement` | 向量设值 |
| `shufflevector` | 向量重排 |

> 注：`extractvalue` / `insertvalue` 已在异常处理路径中部分支持（见上方[异常处理（部分支持）](#异常处理部分支持)）。

## 异常处理算法

### 概述

VM 支持 C++ 异常处理的基本流程（invoke → landingpad cleanup → resume）。核心思路是将 `invoke` 翻译为带 try/catch 包裹的 `CALL`，异常被 VM 捕获后跳转到 landingpad 块执行清理代码，最后通过 resume 重新抛出。

### 字节码序列

对于一个典型的 `invoke` 调用：

```llvm
invoke void @may_throw_fn(args...) to label %normal unwind label %lpad
```

CodeGen 生成以下字节码序列：

```
INVOKE_PREP <unwind_pc>     ; 设置 ctx.unwind_pc = landingpad 块起始偏移
SETARG r0, arg0             ; 与普通 CALL 相同的参数设置
SETARG r1, arg1
...
CALL [fn_idx] (inv flag=1)  ; CALL 带上 VM_CALL_INVOKE (bit 3) 标志
; -- 正常返回路径 (fall-through 到 normal 块) --
; -- 若 normal 不是下一块，此处插入 JMP --
```

### VM 执行流程

```
1. INVOKE_PREP: ctx.unwind_pc = absolute_offset(lpad_bb)

2. SETARG * N: 设置调用参数（同普通 CALL）

3. CALL (inv flag=1):
   try {
       fn(args...);        // 直接函数指针调用（非 libffi 路径）
       // 正常返回：pc += 8，继续执行 normal 块
   } catch (...) {
       ctx.exc = std::current_exception();  // 保存异常
       pc = ctx.unwind_pc;                  // 跳转到 landingpad
   }

4. LPAD rdst:
   ctx.r[rdst] = serialize(std::exception_ptr → uintptr_t)

5. extractvalue 0 → MOV rdst, ragg:
   提取异常指针（序列化的 std::exception_ptr）到新寄存器

6. extractvalue 1 → LI rdst, 0:
   cleanup-only selector 恒为 0

7. [清理代码]：析构函数调用（普通 CALL 指令）

8. insertvalue {ptr,i32}:
   MOV rdst, rval — 传播异常句柄到 resume 操作数

9. RESUME rexc:
   从 ctx.r[src1] 反序列化 std::exception_ptr
   std::rethrow_exception(ep) — 重新抛出异常到上层调用者
```

### 限制

1. **仅支持 cleanup-only landingpad**：当前实现仅处理 `landingpad {ptr, i32} cleanup` 模式（无 catch 子句）。带有 catch 的 landingpad 需要 RTTI 类型匹配和 `__cxa_begin_catch` / `__cxa_end_catch` 集成，暂不支持。

2. **libffi 混合参数路径**：`VM_CALL_ARG_MIX`（libffi）路径的 try/catch 可能无法捕获 C++ 异常，因为 libffi 的汇编 trampoline 不保证异常传播。直接函数指针调用路径（`VM_CALL_ARG_FP` / 纯 int）可以正常工作。

3. **VM 解释器改用 C++ 编译**：`vmexecute.c` 已改为 `vmexecute.cpp`，需要 C++ 编译器（支持 `<exception>` 和 `std::exception_ptr`）。VMContext 结构体新增 `std::exception_ptr exc` 和 `uint32_t unwind_pc` 字段。

## Runtime Interface

在 VMP 注解函数的入口，LLVM pass 依次插入以下调用：

```
VMSaveReg(r0, r1, ..., r7);           // 捕获 8 个函数参数作为 VM 寄存器初始值
// [可选] 全局变量地址表（函数涉及全局变量时插入）
// VMExecute 根据此表将全局变量地址填入对应的 VM 寄存器
VMExecute(bytecode, size, nregs, func_table, func_count,
          global_init, num_globals);  // 执行 bytecode，返回 uintptr_t
```

- `VMSaveReg` 将函数参数的运行时值存入 VM 寄存器供 bytecode 使用
- `VMExecute` 开始解释执行 bytecode，第三参数 `nregs` 指示 VM 上下文需要分配的寄存器数量（**r0–r(nregs-1)**，由 CodeGen 的 Use-count 回收算法计算的最大并发寄存器数）
- 全局变量通过 `{reg, value}` 对表传递：`global_init` 交替存放 `[reg0, val0, reg1, val1, ...]`，VM 启动时遍历此表将 `value` 写入 `ctx.r[reg]`

### 浮点参数捕获

`VMSaveReg` 的参数类型为 `uintptr_t`（映射到通用寄存器），CodeGen 在生成 LLVM IR 时对不同类型的参数做以下转换以保留其值：

| 参数类型 | 转换方式 |
|---------|---------|
| `int` | `zext → IntPtrTy`（零扩展到指针宽度） |
| `ptr` | `ptrtoint → IntPtrTy` |
| `double` | `bitcast double → IntPtrTy`（保留 IEEE 754 位模式） |
| `float` | `bitcast float → i32` → `zext → IntPtrTy` |

编译器在调用 `VMSaveReg` 时会将浮点值的位模式从 XMM 寄存器搬到 GP 寄存器（`movq %xmmN, %rdi`），作为 `uintptr_t` 参数传入。VM 寄存器宽度为 `uintptr_t`（64 位），足以容纳 `double` 的全部 8 字节。后续字节码通过 `LOAD.8`/`STORE.8` 将位模式写入内存，再通过 `double*` 指针解引用还原为浮点值，或在 `SETARG` 中直接以 `uintptr_t` 形式传递给 libffi。

### 浮点返回值

被加固函数的浮点返回值（`float`/`double`）经过以下链路传递回调用方：

1. **VM 侧**：`RET rval` 直接将 `ctx.r[dst]`（`uintptr_t`，内含 IEEE 754 位模式）返回
2. **LLVM IR 侧**：`insertVmpcall()` 将 `VMExecute` 返回的 `uintptr_t`（IntPtrTy）通过以下步骤还原：
   - 若目标浮点类型宽度 < 64 位（如 `float`），则 `trunc → i32`
   - `bitcast` 回目标浮点类型（`i64 → double` 或 `i32 → float`）
   - `ret double` / `ret float`：LLVM 按 x86-64 ABI 将值放入 XMM0

生成的 LLVM IR 示意（`double` 返回）：
```llvm
%result = call i64 @VMExecute(...)    ; 返回 uintptr_t, 内含 double 位模式
%ret    = bitcast i64 %result to double ; 按位重解释为 double
ret double %ret                       ; → XMM0
```

### 全局变量支持

当 VMP 函数引用全局变量时，VMCodeGen 自动执行以下步骤：

1. **寄存器分配**：`genBytecode()` 中通过 `getGlobalReg(GV)` 为每个唯一 `GlobalVariable` 分配一个 VM 寄存器（`Regs.NextReg++`，位于普通虚拟寄存器之后）
2. **字节码引用**：遇到以下情形时使用全局寄存器而非 `r0`（缺省值）：
   - GEP 的基址是 `GlobalVariable` → `MOV rdst, r_global`
   - LOAD/STORE 的指针操作数 → 直接使用 `r_global`
   - CALL 的参数是 `GlobalVariable`（零偏移 GEP 被优化后） → 同 SETARG 使用 `r_global`
3. **地址注入**：`insertVmpcall()` 生成 LLVM IR 代码，计算每个全局变量的运行时地址（`ptrtoint`），存入 `{reg, value}` 对表，传入 `VMExecute`

VM 启动时遍历该表，将地址填入对应寄存器，后续字节码即可通过全局寄存器访问全局变量。

#### 示例

```c
char teststr[] = "asdadajijiopjq";
int  testint   = 8;

__attribute__((annotate("VMP")))
void test(Results *r) {
    r->global_r = strlen(teststr) + testint;  // teststr、testint 为全局变量
}
```

生成的字节码中，`teststr` 被分配全局寄存器 rN，`testint` 被分配 rN+1。`strlen` 的参数通过 `SETARG r0, rN` 传入，`load i32, i32* @testint` 通过 `LOAD.4 rdst, r(N+1)` 执行。

## 已修复的问题
### `-O3` 生命周期内联函数链接错误 (已修复)
开启优化(`-O3`)后，LLVM 自动插入的 `@llvm.lifetime.start.p0` / `@llvm.lifetime.end.p0` 生命周期标记内联函数会被 CodeGen 当成普通外部函数调用处理，导致链接器报未定义引用。

**修复：** VMCodeGen.cpp 中 CallInst 处理分支跳过这些内联函数（不生成字节码、不加入函数表），同时正确消耗操作数以维持寄存器追踪的正确性。

### `Function*` 常量作为 CALL 参数时段错误 (已修复)
当函数指针常量（如 `@_ZSt3hexRSt8ios_base`）作为另一个函数调用的参数传入时，CodeGen 的 CALL 参数处理只覆盖了 `ConstantInt`、`ConstantFP` 和 `GlobalVariable`，遗漏了 `Function` 类型。该常量落入 `Regs.consume()` 分支，因从未被映射而返回默认值 r0（通常是 sret 指针），导致被调用函数内部尝试跳转到栈地址而触发段错误。

**修复：** 扩展 `VMGlobalRef` 结构体支持 `Function*`，新增 `getFuncPtrReg()` 为函数指针分配专用寄存器，复用 `global_init` 表机制通过 `ptrtoint` 在运行时注入函数地址。

### 窄整数返回值（bool/operator!=）未掩码导致无限循环 (已修复)
C++ 标准库的 `operator!=` 返回 `bool`（i1），但 VM 通过 `IntRetCallFn`（返回 `uintptr_t`）调用它。某些编译单元在返回 `bool` 时未正确零扩展 RAX 寄存器（`mov rax, [rdi]` 加载迭代器指针到高位后，`setne al` 仅设置低字节），导致 VM 读取到的返回值高位包含垃圾地址值。`BR!` 检查 `r24 != 0` 永远为真，循环无法退出。

**修复：** 在 CodeGen 中，CALL 指令发出后，若返回类型为窄整数（i1/i8/i16），自动附加 `VM_AND rdst, rdst, #mask` 指令清除高位垃圾，确保 VM 看到正确的 0/1 值。

### GEP 别名链 UseCount 追踪分裂导致寄存器被提前回收 (已修复)
当存在"GEP 的 GEP"链且两次偏移均为 0 时（如 `GEP(GEP(%this, 0, 0), 0, 0)` — 访问结构体第一个成员数组的首元素），第二个 `aliasValue(%7, %6)` 调用时 `%6` 已经是 `%5` 的别名（UseCount 已被转移给根 `%5`），但 `aliasValue` 未沿别名链追溯到根，导致 `%7` 的 UseCount 被错误积累在中间节点 `%6` 上。`consume` 沿链追溯到根 `%5` 减计数，但中间节点 `%6` 上残留的 UseCount 从未被消耗，最终根 `%5` 的 UseCount 被多减一次提前归零，寄存器被 `LI` 立即数回收覆盖，后续使用该寄存器的 CALL 参数（如 `KeyExpansion(%5)`）读到错误值而崩溃。

**修复：** `aliasValue` 在转移 UseCount 前通过 `while` 循环沿已有 `AliasParent` 链追溯到根节点，确保 UseCount 始终积累在根上，中间别名不再持有独立的 UseCount 条目。

### ADD 负立即数截断导致无符号溢出死循环 (已修复)
LLVM 在 `-O3` 下会将 `sub i32 %x, 1` 优化为 `add nsw i32 %x, -1`。CodeGen 把 `-1` 转换为 16 位立即数 `0xFFFF = 65535`，但 VM 的 `INT_BINOP` 做的是无符号加法，`13 + 65535 = 65548`，后续有符号比较 `CMP pred=6(SGT) 65548 > 0` 永远为真，循环无法退出，最终因循环索引越界访问数组导致崩溃。

**修复：** 在 `case Instruction::Add` 中检查立即数的符号位（`(int16_t)(uint16_t)RSrc2 < 0`），为负时转为 `VM_SUB rdst, rsrc, #abs(val)`，利用 VM 的无符号减法正确计算 `x - (-N) = x + N`。

### `consume` 过早释放寄存器导致碰撞 — GEP 与 BinaryOperator (已修复)
**通用原则：** `consume()` 会将寄存器回收到 FreeList，而后续 `allocRaw()` 可能立即取回同一个寄存器。因此任何在 `emitInsn` 之前调用 `consume` 的模式都存在风险 —— 若 `emitInsn` 之前（或其参数准备过程中）有 `allocRaw`，可能拿到刚释放的寄存器并覆盖其值，导致后续指令读到错误数据。

**修复：** 统一采用延迟消费：先用 `lookupReg` 获取寄存器号，等所有 `emitInsn` 完成后再调用 `consume` 释放。具体涉及两处：

- **GEP 变量偏移：** `consume(j)` 释放 r20 → `allocRaw()` 取回 r20 → `LI r20, #4` 覆盖 `j` → `MUL r20, r20, r20` 计算 `4*4=16` 替代 `j*4`。修复：将 `allocRaw` 移至 `consume` 之前。
- **BinaryOperator 浮点常量：** `consume(%x)` 释放 rM → `allocRaw()`（src2 为 ConstantFP 时的 LI32）取回 rM 覆盖原值 → `FADD rM, rM, rM` 两边操作数变成同一个常量。修复：非立即数操作数一律用 `lookupReg` 取号，`emitInsn` 之后再统一 `consume`。

