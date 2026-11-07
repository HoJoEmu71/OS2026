# 操作系统实验报告

## 实验基本信息

| 项目 | 内容 |
|------|------|
| **实验名称** | Lab 3: 中断与异常处理（trapentry.S 上下文恢复、trap.c 中断/异常分发） |
| **小组成员** | 2413238-罗清、2412951-申一博、2413191-马云飞 |
| **完成日期** | 2026-11-07 |

### 小组分工

| 成员 | 负责的练习/模块 |
|------|----------------|
| 2413238-罗清 | 功能模块一 `trapentry.S`（RESTORE_ALL / __alltraps）、QEMU 6.2 适配、报告统稿 |
| 2412951-申一博 | 功能模块二 `trap.c` 中断处理（时钟中断重设、中断分类） |
| 2413191-马云飞 | 功能模块二 `trap.c` 异常处理（非法指令/断点 EPC 推进）、prompt 撰写 |

> 注：所有代码均由三人分别审阅、共同调试，测试结果经小组一起复核。

---

## 一、实验目的

1. 理解 RISC-V 中断/异常的硬件触发流程：`stvec` → 保存现场 → 分发处理 → 恢复现场 → `sret`。
2. 掌握 trapframe 的布局（32 个通用寄存器 + `sstatus`/`sepc`/`sbadaddr`/`scause`），以及 `SAVE_ALL` / `RESTORE_ALL` 的对称设计。
3. 实现 `__alltraps`：把 trapframe 指针按调用约定传给 C 函数 `trap()`，处理完后跳转到 `__trapret`。
4. 实现中断分发 `interrupt_handler`：时钟中断必须重设下一次事件（`clock_set_next_event`），否则只触发一次；软件中断、外部中断、未知中断分类输出。
5. 实现异常分发 `exception_handler`：非法指令与断点需要按**指令编码长度**推进 `epc`（低两位 `11` 为 4 字节，否则 2 字节），不能写死 `+4`。

---

## 二、实验环境

**基础环境：**

- 宿主机：Windows 11 + WSL2（Ubuntu 22.04）
- 交叉编译器：`riscv64-unknown-elf-gcc` 10.2.0
- 模拟器：`qemu-system-riscv64` 6.2.0（内置 OpenSBI v0.9）
- 构建：GNU Make；内核通过 `-kernel bin/ucore.img` 加载到 `0x80200000`
- 评分：`make grade`（`tools/inject.py` 注入测试 + `tools/grade.sh` 采集 QEMU 串口输出）


---

## 三、实验整体逻辑分析

### 3.1 本章节的逻辑主线

lab3 的主线是**让内核能“接住”并“返回”中断/异常**：硬件把控制权交给 `stvec` 指向的入口，
入口保存全部寄存器到栈上形成 trapframe，C 代码根据 `scause` 分发处理，最后恢复寄存器、
用 `sret` 回到被中断的指令。

整个实现链：

```
中断/异常发生
  │
  ▼
硬件：scause ← 原因，sepc ← 断点地址，stval ← 附加信息，
     sstatus.SIE ← 0，SPIE ← 旧 SIE，PC ← stvec
  │
  ▼
__alltraps (汇编)
  ├── SAVE_ALL：sp -= 36*REGBYTES，把 32 个 GPR + sstatus/sepc/sbadaddr/scause 压栈
  ├── mv a0, sp         ← trapframe 指针作为第一个参数
  ├── jal trap          ← 调用 C 的 trap()
  │
  ▼
trap() (C, kern/trap/trap.c)
  └── trap_dispatch(tf)
        ├── tf->cause < 0 → interrupt_handler(tf)   ← 中断
        └── 否则          → exception_handler(tf)    ← 异常
  │
  ▼
__trapret (汇编)
  └── RESTORE_ALL：从 trapframe 恢复 4 个 CSR + 32 个 GPR（sp 最后恢复）
  └── sret              ← PC ← sepc，SIE ← SPIE
```

### 3.2 功能的逐步实现

1. **先看懂 trapframe 布局**：`SAVE_ALL` 把 `x0`～`x31` 存在 `sp + 0*REGBYTES` 到
   `sp + 31*REGBYTES`，四个 CSR 存在 `32*`～`35*REGBYTES`。`x2`(sp) 的保存值是**进入中断前**的 sp。
2. **实现 RESTORE_ALL**：与 SAVE_ALL 对称，但 sp 必须最后恢复——否则恢复 sp 之后就无法再用
   它寻址 trapframe 里其他寄存器了。
3. **实现 __alltraps**：`mv a0, sp` 把当前 sp（即 trapframe 起始地址）传给 `trap()`，
   用 `jal trap` 调用，返回后自然落到 `__trapret`。
4. **实现中断处理**：时钟中断要 `clock_set_next_event()` 重设 timecmp；每 100 次 tick 打印一次，
   累计 `SHUTDOWN_AFTER` 次后 `sbi_shutdown()`。
5. **实现异常处理**：非法指令和断点都要推进 `epc`，长度由指令低两位决定（`11` → 4 字节，否则 2 字节）。

---

## 四、实验内容与实现

### 功能模块一：trapentry.S 的 RESTORE_ALL 与 __alltraps

**对应文件**：`code/kern/trap/trapentry.S`

#### 4.1.1 RESTORE_ALL

与 `SAVE_ALL` 严格对称，顺序是：先恢复 4 个 CSR（用 `s1`～`s4` 作临时寄存器），再恢复 32 个
通用寄存器，**`x2`(sp) 放在最后**。

```asm
.macro RESTORE_ALL
    # 先恢复 4 个 CSR，避免恢复 sp 之后无法再寻址 trapframe
    LOAD s1, 32*REGBYTES(sp)
    LOAD s2, 33*REGBYTES(sp)
    LOAD s3, 34*REGBYTES(sp)
    LOAD s4, 35*REGBYTES(sp)
    csrw sstatus, s1
    csrw sepc, s2
    csrw sbadaddr, s3
    csrw scause, s4

    # 恢复通用寄存器，x0 恒为 0，x2(sp) 最后恢复
    LOAD x1, 1*REGBYTES(sp)
    LOAD x3, 3*REGBYTES(sp)
    ...
    LOAD x31, 31*REGBYTES(sp)
    LOAD x2, 2*REGBYTES(sp)
.endm
```

**关键约束**：`LOAD` 宏在 rv64 下是 `ld`，`STORE` 是 `sd`，`REGBYTES` 为 8。
CSR 恢复用 `s1`～`s4` 中转，而这些寄存器本身也在 trapframe 里（`x9`、`x18`～`x21`），
后面会被正确覆盖，不影响最终状态。

#### 4.1.2 __alltraps

```asm
__alltraps:
    SAVE_ALL
    mv a0, sp        # trapframe 指针作为 trap() 的第一个参数
    jal trap         # 调用 C 的 trap()，返回后落到 __trapret
__trapret:
    RESTORE_ALL
    sret
```

`mv a0, sp` 是 RISC-V 调用约定的要求：第一个参数走 `a0`。`jal trap` 会把返回地址写入 `ra`，
`trap()` 返回后 PC 指向 `__trapret`。

---

### 功能模块二：trap.c 的 interrupt_handler 与 exception_handler

**对应文件**：`code/kern/trap/trap.c`

#### 4.2.1 interrupt_handler

```c
void interrupt_handler(struct trapframe *tf) {
    int code = (int)((uintptr_t)tf->cause & 0xFF);
    switch (code) {
    case IRQ_S_TIMER:
        clock_set_next_event();          // 必须重设，否则只触发一次
        ticks++;
        if (ticks % TICK_NUM == 0) {
            print_ticks();
            if (ticks / TICK_NUM >= SHUTDOWN_AFTER) {
                sbi_shutdown();
            }
        }
        break;
    case IRQ_S_SOFT:
        cprintf("Supervisor software interrupt\n");
        break;
    case IRQ_S_EXT:
        cprintf("Supervisor external interrupt\n");
        break;
    default:
        cprintf("Unknown interrupt: cause %d\n", code);
        print_trapframe(tf);
        break;
    }
}
```

**要点**：
- `tf->cause` 的最高位（bit 63）为 1 表示中断，低 8 位是中断号；用 `& 0xFF` 取中断号。
- 时钟中断如果不调用 `clock_set_next_event()`，`mtimecmp` 不会更新，下一次时钟中断永远不会到来。
- `print_ticks()` 每 `TICK_NUM`（100）次 tick 打印一次，输出 `100 ticks`、`200 ticks`……
  累计 `SHUTDOWN_AFTER` 次后调用 `sbi_shutdown()` 让 QEMU 退出。

#### 4.2.2 exception_handler

```c
static size_t instruction_length(uintptr_t epc) {
    uint16_t insn = *(volatile uint16_t *)epc;
    return (insn & 0x3) == 0x3 ? 4 : 2;   // 低两位 11 → 32 位，否则 16 位压缩
}

void exception_handler(struct trapframe *tf) {
    switch ((int)tf->cause) {
    case CAUSE_ILLEGAL_INSTRUCTION:
        cprintf("Illegal instruction at 0x%016lx\n", tf->epc);
        tf->epc += instruction_length(tf->epc);
        break;
    case CAUSE_BREAKPOINT:
        cprintf("Breakpoint at 0x%016lx\n", tf->epc);
        tf->epc += instruction_length(tf->epc);
        break;
    default:
        print_trapframe(tf);
        break;
    }
}
```

**要点**：
- `epc` 不能简单 `+4`，因为 RISC-V 有 16 位压缩指令（C 扩展）。判断方法：读 `epc` 处的
  低 16 位，若低两位是 `11` 则是 32 位指令，否则是 16 位压缩指令。
- 非法指令（cause = 2）和断点（cause = 3）都要推进 `epc`，否则 `sret` 后会回到同一条指令
  无限循环。
- 未识别的异常走 `print_trapframe(tf)`，打印全部寄存器便于调试。

---

## 五、测试与验证

**QEMU 运行输出**（`make qemu`，内核启动后时钟中断每秒打印 `100 ticks`）：

```text
(THU.CST) os is loading ...
memory management: best_fit_pmm_manager
physical memory map:
  memory: 0x0000000008000000, [0x0000000080000000, 0x0000000087ffffff].
  pages: 32768, free: 31929
check_alloc_page() succeeded!
satp virtual address: 0xffffffffc0205000
satp physical address: 0x0000000080205000
++ setup timer interrupts
100 ticks
100 ticks
100 ticks
100 ticks
100 ticks
```

**评分结果**（`make grade`）：

```text
  - trap entry, register restore, and breakpoint EPC: PASS (35/35)
  - illegal instruction EPC advance and return: PASS (25/25)
  - re-armed supervisor timer and repeated trap return: PASS (40/40)
Total Score: 100/100
```

三个测试覆盖：
- **Test 1（35 分）**：`ebreak` 触发断点异常，验证 `RESTORE_ALL` 正确恢复所有寄存器、`epc` 推进后返回。
- **Test 2（25 分）**：非法指令（`.word 0x0000007b`）触发异常并按 4 字节推进；软件中断、未知中断、`cause=0x14` 的输出格式校验。
- **Test 3（40 分）**：时钟中断持续触发，`ticks` 累加到 200，验证 `clock_set_next_event` 重设正确。

---

## 六、实验总结与收获

### 6.1 重要知识点与 OS 原理的对应关系

| 本实验知识点 | OS 原理中的对应概念 | 含义、关系与差异 |
|--------------|--------------------|------------------|
| `stvec` + `__alltraps` | 中断向量表 / 中断入口 | 原理讲“中断发生后硬件跳转到向量表”；实验中 `stvec` 直接指向 `__alltraps`，没有向量表，所有中断统一入口 |
| trapframe + SAVE_ALL/RESTORE_ALL | 中断现场保护与恢复 | 原理讲“保存断点和寄存器”；实验中用栈上的 trapframe 保存 32 个 GPR + 4 个 CSR，恢复时 sp 必须最后处理 |
| `scause` 区分中断/异常 | 中断与异常的区分 | 原理区分“外部中断”和“内部异常”；实验中 `scause` 最高位为 1 是中断，否则是异常，低位数是具体原因 |
| `sstatus.SIE`/`SPIE` | 中断使能与嵌套 | 原理讲中断屏蔽、开/关中断；实验中硬件自动把 `SIE` 存到 `SPIE` 并清 `SIE`，`sret` 时恢复，实现原子性 |
| `sepc` + 指令长度判断 | 断点返回地址 | 原理讲“异常返回后跳过故障指令”；实验中必须按指令编码（16/32 位）推进 `epc`，不能写死 +4 |
| 时钟中断 + `clock_set_next_event` | 时钟中断 / 时间片 | 原理讲“时钟中断是调度的基础”；实验中每次时钟中断必须重设 `timecmp`，否则只触发一次 |
| `sbi_shutdown` | 系统关机 | 原理讲“电源管理”；实验中通过 SBI 调用让 QEMU 退出，方便自动化评分 |

### 6.2 OS 原理中很重要、但本实验没有对应上的知识点

1. **中断优先级与嵌套**：实验中所有中断同级处理，没有优先级，也没有在中断处理中开中断实现嵌套。
2. **多核中断与 IPI**：单核实验，没有核间中断（IPI）和 per-CPU 的中断控制。
3. **中断下半部（softirq / tasklet）**：实验中所有处理都在中断上下文同步完成，没有延迟处理机制。
4. **用户态中断与系统调用**：异常处理只覆盖了非法指令和断点，没有实现 `ecall`（系统调用）的分发（lab5 内容）。
5. **缺页异常处理**：`scause` 为 13/15 的页错误没有对应处理（lab9 内容）。
6. **中断控制器（PLIC/CLINT）**：实验中直接用 SBI 封装的时钟和中断，没有直接访问 CLINT/PLIC 寄存器。

### 6.3 AI 协作开发的经验

- **对称即正确**：`RESTORE_ALL` 必须与 `SAVE_ALL` 严格镜像，尤其是 sp 最后恢复这一点。
  把“sp 必须最后恢复”写进 prompt 的约束，AI 一次就能写对。
- **指令长度不能想当然**：RISC-V 有 C 扩展，异常处理里 `epc += 4` 在压缩指令场景会死循环。
  prompt 里明确要求“按指令编码低两位判断长度”，避免了这个隐蔽 bug。
- **时钟中断必须重设**：如果不调用 `clock_set_next_event()`，Test 3 会因为只触发一次中断而超时。
  这是 lab3 最容易遗漏的点，必须在 prompt 里作为硬约束。
- **评分要走完整链路**：直接跑 `bash tools/grade.sh` 不会注入测试代码（注入在 `make grade` 目标里），
  必须用 `make grade` 才能看到真实分数。
- **环境问题先于代码**：本次 WSL2 因 `hypervisorlaunchtype=Off` 无法启动，`make grade` 全 0 分
  一度被误判为代码问题，确认环境正常后代码一次通过。

---
