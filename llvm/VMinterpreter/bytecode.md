# VM Bytecode Specification

## Instruction Encoding

每条指令固定 **8 字节**，小端序：

```
[opcode(1)][flags(1)][dst(2)][src1(2)][src2(2)]
```

| Offset | Size | Field  | Description            |
|--------|------|--------|------------------------|
| 0      | 1    | opcode | 指令操作码              |
| 1      | 1    | flags  | 标志位（见 Flags 定义） |
| 2-3    | 2    | dst    | 目标寄存器编号           |
| 4-5    | 2    | src1   | 源操作数 1              |
| 6-7    | 2    | src2   | 源操作数 2 / 立即数      |

## Flags

| Bit | 用途     | 说明                                       |
|-----|----------|-------------------------------------------|
| 0   | 访问宽度 / BR 条件取反 | 0 = 4 字节（LOAD/STORE），1 = `sizeof(uintptr_t)` 字节（LOAD/STORE）；BR 专用：1 = 条件取反（rcond == 0 时跳转） |
| 1   | 内存空间 | 0 = VM 内部内存，1 = 原生内存（外部指针） |
| 2   | 立即数   | 0 = src2 为寄存器，1 = src2 为 16 位立即数（算术/逻辑运算专用） |
| 3   | 浮点类型 | 0 = 整数，1 = 浮点（仅 LOAD: 不符号扩展保持位模式） |
| 4-7 | 保留     | 当前为 0                                  |

> **CMP** 指令特殊：flags 低 4 位（bits 0–3）编码比较谓词（0=EQ … 9=SLE），不适用上述通用标志位定义。

LOAD 和 STORE 使用 bit 0 + bit 1，ALLOCA 使用 bit 0 区分立即数/寄存器大小。
算术/逻辑运算使用 **bit 2** 指示 src2 是 16 位立即数还是寄存器索引。
LOAD 使用 **bit 3** 指示浮点加载（不符号扩展整数，直接复制位模式）。

## Register Model

- **r0 – r7**: 函数参数寄存器，由 `VMSaveReg` 在函数入口捕获
- **r8+**: 通用虚拟寄存器，由 bytecode 生成器分配

> 寄存器值宽度为 `uintptr_t`（32 位平台 4 字节，64 位平台 8 字节）。
> 编码字段 16 位（支持 0–65535）。

## Opcode Table

| Opcode | Mnemonic | Format                  | Description                          |
|--------|----------|-------------------------|--------------------------------------|
|        | **内存** (0x00–0x0F) | | |
| 0x00   | ALLOCA   | `ALLOCA rdst, #size`    | 在 VM 栈上分配 `size` 字节，返回栈顶地址 |
| 0x01   | LOAD     | `LOAD rdst, raddr`      | 从 `raddr` 指向的地址加载 4/8 字节      |
| 0x02   | STORE    | `STORE rval, raddr`     | 将 `rval` 的值存入 `raddr` 指向的地址 |
| 0x03   | LI       | `LI rdst, #imm`         | 加载 16 位立即数到目标寄存器           |
| 0x04   | LI32     | `LI32 rdst, #imm32`     | 加载 32 位立即数到目标寄存器           |
| 0x05   | MOV      | `MOV rdst, rsrc`        | rdst = rsrc (寄存器复制)              |
| 0x06   | CMP      | `CMP rdst, rsrc1, rsrc2`| 比较 rsrc1 与 rsrc2（谓词在 flags 低 4 位），结果 0/1 写入 rdst |
| 0x07   | JMP      | `JMP #offset`           | 无条件跳转，offset 为有符号 16 位相对偏移 |
| 0x08   | BR       | `BR rcond, #offset`     | 条件跳转，rcond ≠ 0 时跳转 offset 字节 |
|        | **整数算术** (0x10–0x1F) | | |
| 0x10   | ADD      | `ADD rdst, rsrc1, rsrc2`| rdst = rsrc1 + rsrc2 (整数加法)        |
| 0x11   | SUB      | `SUB rdst, rsrc1, rsrc2`| rdst = rsrc1 - rsrc2 (整数减法)        |
| 0x12   | MUL      | `MUL rdst, rsrc1, rsrc2`| rdst = rsrc1 * rsrc2 (整数乘法)        |
| 0x13   | UDIV     | `UDIV rdst, rsrc1, rsrc2`| rdst = rsrc1 / rsrc2 (无符号整数除法)  |
| 0x14   | SDIV     | `SDIV rdst, rsrc1, rsrc2`| rdst = rsrc1 / rsrc2 (有符号整数除法)  |
| 0x15   | UREM     | `UREM rdst, rsrc1, rsrc2`| rdst = rsrc1 % rsrc2 (无符号取余)      |
| 0x16   | SREM     | `SREM rdst, rsrc1, rsrc2`| rdst = rsrc1 % rsrc2 (有符号取余)      |
| 0x17   | SHL      | `SHL rdst, rsrc1, rsrc2` | rdst = rsrc1 << rsrc2 (左移)          |
| 0x18   | LSHR     | `LSHR rdst, rsrc1, rsrc2`| rdst = rsrc1 >> rsrc2 (逻辑右移)      |
| 0x19   | ASHR     | `ASHR rdst, rsrc1, rsrc2`| rdst = rsrc1 >> rsrc2 (算术右移)      |
| 0x1A   | AND      | `AND rdst, rsrc1, rsrc2` | rdst = rsrc1 & rsrc2 (按位与)         |
| 0x1B   | OR       | `OR rdst, rsrc1, rsrc2`  | rdst = rsrc1 \| rsrc2 (按位或)        |
| 0x1C   | XOR      | `XOR rdst, rsrc1, rsrc2` | rdst = rsrc1 ^ rsrc2 (按位异或)       |
|        | **浮点算术** (0x20–0x2F) | | |
| 0x20   | FADD     | `FADD rdst, rsrc1, rsrc2`| rdst = rsrc1 + rsrc2 (浮点加, bit0=1 → double) |
| 0x21   | FSUB     | `FSUB rdst, rsrc1, rsrc2`| rdst = rsrc1 - rsrc2 (浮点减, bit0=1 → double) |
| 0x22   | FMUL     | `FMUL rdst, rsrc1, rsrc2`| rdst = rsrc1 * rsrc2 (浮点乘, bit0=1 → double) |
| 0x23   | FDIV     | `FDIV rdst, rsrc1, rsrc2`| rdst = rsrc1 / rsrc2 (浮点除, bit0=1 → double) |
|        | **类型转换** (0x30–0x3F) | | |
| 0x30   | SITOFP   | `SITOFP rdst, rsrc`     | int32→float/double (bit0=1 → double) |
| 0x31   | FPTOSI   | `FPTOSI rdst, rsrc`     | float/double→int32 (bit0=1 → double) |
| 0x32   | FPTRUNC  | `FPTRUNC rdst, rsrc`    | double→float 截断                     |
| 0x33   | FPEXT    | `FPEXT rdst, rsrc`      | float→double 扩展                     |
|        | **特殊** | | |
| 0xFF   | RET      | `RET rval`              | 返回 `rval` 中的值                   |

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
- 从 `raddr` 指向的地址读取数据，存入 `rdst`
- **flags bit 0**: 0 = 读取 4 字节，1 = 读取 `sizeof(uintptr_t)` 字节
- **flags bit 1**: 0 = 从 VM 内部内存读取（`raddr` 为 ALLOCA 偏移量），1 = 从原生内存读取（`raddr` 为外部指针）

**STORE** (0x02)
- 编码: `[0x02][flags(1)][raddr(2)][rval(2)][0]`
- 将 `rval` 的值写入 `raddr` 指向的地址
- 注意: dst 字段在此指令中表示地址，src1 表示值
- **flags bit 0**: 0 = 写入 4 字节，1 = 写入 `sizeof(uintptr_t)` 字节
- **flags bit 1**: 0 = 写入 VM 内部内存，1 = 写入原生内存

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

**JMP** (0x07)
- 编码: `[0x07][0][0][offset_lo(2)][0]`
- `pc = pc + 8 + (int16_t)src1`，src1 为有符号 16 位相对偏移（相对于下一条指令）
- dst/src2 字段保留（编码为 0）

**BR** (0x08)
- 编码: `[0x08][flags][0][rcond(2)][offset(2)]`
- 若 `r[rcond] != 0` 则 `pc = pc + 8 + (int16_t)src2`，否则 `pc = pc + 8`
- **flags bit 0 (VM_FLAG_BR_NT)**：条件取反。置位时，`r[rcond] == 0` 触发跳转，`r[rcond] != 0` 时继续执行
- dst 字段保留（编码为 0）
- 在 CodeGen 中，条件分支翻译为 **BR + JMP** 两条指令：BR 跳转到真目标，紧接的 JMP 跳转到假目标（避免依赖基本块布局顺序）
- **优化**：当其中一个目标恰好在基本块布局中紧随当前块时，可省略 JMP：
  - 假目标为下一块：仅 BR（条件成立时跳真目标，否则 fall through 到假目标）
  - 真目标为下一块：使用 `BR!`（置位 VM_FLAG_BR_NT），条件成立时不跳转（fall through 到真目标），否则跳假目标
  - 以上两情形均可消除冗余 JMP 指令

**LI** (0x03)
- 编码: `[0x03][flags(0)][dst(2)][0][immediate(2)]`
- 将 16 位立即数零扩展后加载到 `rdst`

**LI32** (0x04)
- 编码: `[0x04][flags(0)][dst(2)][value_lo(2)][value_hi(2)]`
- 将 32 位立即数加载到 `rdst`，src1 和 src2 拼接为值：`val = src1 | (src2 << 16)`
- 用于 float 常量（位模式直接加载）和 > 16 位的整数常量

**RET** (0xFF)
- 编码: `[0xFF][flags(0)][rval(2)][0][0]`
- 返回 `rval` 中的值

## IR → Bytecode Mapping

| LLVM IR         | VM Bytecode   |
|-----------------|---------------|
| `alloca i32`    | `ALLOCA rdst, #size` |
| `load i32, ptr` | `LOAD rdst, raddr` (flags bit 0 = 0) |
| `load float, ptr` | `LOAD rdst, raddr` (flags bit 3 = 1, 不符号扩展) |
| `load ptr, ptr` | `LOAD.nat rdst, raddr` (flags bit 1 = 1) |
| `store val, ptr`| `STORE rval, raddr` |
| `add`           | `ADD rdst, rsrc1, rsrc2` |
| `sub`           | `SUB rdst, rsrc1, rsrc2` |
| `mul`           | `MUL rdst, rsrc1, rsrc2` |
| `udiv`          | `UDIV rdst, rsrc1, rsrc2` |
| `sdiv`          | `SDIV rdst, rsrc1, rsrc2` |
| `urem`          | `UREM rdst, rsrc1, rsrc2` |
| `srem`          | `SREM rdst, rsrc1, rsrc2` |
| `shl`           | `SHL rdst, rsrc1, rsrc2` |
| `lshr`          | `LSHR rdst, rsrc1, rsrc2` |
| `ashr`          | `ASHR rdst, rsrc1, rsrc2` |
| `and`           | `AND rdst, rsrc1, rsrc2` |
| `or`            | `OR rdst, rsrc1, rsrc2` |
| `xor`           | `XOR rdst, rsrc1, rsrc2` |
| `fadd`          | `FADD rdst, rsrc1, rsrc2` |
| `fsub`          | `FSUB rdst, rsrc1, rsrc2` |
| `fmul`          | `FMUL rdst, rsrc1, rsrc2` |
| `fdiv`          | `FDIV rdst, rsrc1, rsrc2` |
| `ret val`       | `RET rval` |
| `sitofp`        | `SITOFP rdst, rsrc` (int→float/double, flags bit0=1 for double) |
| `fptosi`        | `FPTOSI rdst, rsrc` (float/double→int, flags bit0=1 for double) |
| `fptrunc`       | `FPTRUNC rdst, rsrc` (double→float) |
| `fpext`         | `FPEXT rdst, rsrc` (float→double) |
| `const int`     | `LI rdst, #imm` |
| `const float`   | `LI32 rdst, #imm32` (加载浮点位模式) |
| `icmp eq/ne/...`| `CMP rdst, rsrc1, rsrc2` (predicate 编码在 flags 低 4 位) |
| `br label`      | `JMP #offset` |
| `br i1 cond, label, label` | `BR rcond, #offset` (true → BR, false → JMP，相邻目标时优化 JMP)` |
| `phi`           | 降级为 `MOV` 指令，在前驱块末尾插入，当前块跳过生成 |

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
| 比较 | ✅ ❌ | icmp ✅ / fcmp ✗ |
| 类型转换 | ✅ ✅ ✅ ✅ ❌ ❌ ❌ ❌ | sitofp, fptosi, fptrunc, fpext ✓ / trunc, zext, sext 等 ✗ |
| 聚合操作 | ❌ ❌ ❌ ❌ ❌ | extractvalue, insertvalue, 向量操作 |
| 函数调用 | ❌ | call |

## 未支持的 IR 指令

当前 `VMCodeGen.cpp` 遇到以下指令会触发 `report_fatal_error` 直接终止编译。

### 控制流 — 中优先级

| IR 指令 | 说明 | 依赖 |
|---------|------|------|
| `switch` | 多路分支 | 需要 br 支持 |
| `select` | 条件选择 | 需要比较指令 |
| `indirectbr` | 间接跳转 | 较少见 |
| `invoke` / `resume` / `landingpad` | 异常处理 | 复杂，暂不考虑 |

### 类型转换 — 中优先级

| IR 指令 | 说明 |
|---------|------|
| `trunc` | ❌ 整数截断 |
| `zext` | ❌ 零扩展 |
| `sext` | ❌ 符号扩展 |
| `ptrtoint` | ❌ 指针转整数 |
| `inttoptr` | ❌ 整数转指针 |
| `bitcast` | ❌ 位模式重解释 |
| `fptoui` / `uitofp` | ❌ 无符号浮点转换 |
| `addrspacecast` | ❌ 地址空间转换 |

### 浮点运算

| IR 指令 | 说明 |
|---------|------|
| `fcmp` | ❌ 浮点比较 |
| `frem` | ❌ 浮点取余 |

### 内存内联函数 — 低优先级

| IR 指令 | 说明 |
|---------|------|
| `memcpy` / `memmove` | 内存拷贝（`@llvm.memcpy.*`） |
| `memset` | 内存设置（`@llvm.memset.*`） |

### 聚合操作 — 低优先级

| IR 指令 | 说明 |
|---------|------|
| `extractvalue` | 从聚合类型取值 |
| `insertvalue` | 设置聚合类型字段 |
| `extractelement` | 向量取值 |
| `insertelement` | 向量设值 |
| `shufflevector` | 向量重排 |

### 函数调用

| IR 指令 | 说明 |
|---------|------|
| `call` | 调用其他函数（含 `@llvm.*` 内联函数） |

## Runtime Interface

在 VMP 注解函数的入口，LLVM pass 依次插入两个调用：

```
VMSaveReg(r0, r1, ..., r7);           // 捕获 8 个函数参数作为 VM 寄存器初始值
VMExecute(bytecode, size, nregs);     // 执行 bytecode，返回 void*
```

- `VMSaveReg` 将函数参数的运行时值存入 VM 寄存器供 bytecode 使用
- `VMExecute` 开始解释执行 bytecode，第三个参数 `nregs` 指示 VM 上下文需要分配的寄存器数量（**r0–r(nregs-1)**，由 CodeGen 的 Use-count 回收算法计算的最大并发寄存器数）
- 返回值通过 `(rettype)VMExecute(...)` 转换

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

引用参数示例（`LOAD.nat` / `STORE.nat` 访问外部内存）：
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
  0x0028: LOAD.nat r8, r37     ; load a value via native ptr → r8 = *a
  0x0030: LOAD.nat r9, r38     ; load b value via native ptr → r9 = *b
  0x0038: ADD    r10, r8, r9   ; c = a + b
  ...
  0x0050: STORE.nat r10, r37   ; *a = c (write back via native ptr)
```
