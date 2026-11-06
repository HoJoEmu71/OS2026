# Lab 3 提示词汇总（prompt.md）

> 本文件按 lab0.5 学习并沿用至 lab2 的四段式框架（PROMPT / RELY / GUARANTEE / SPECIFICATION）
> 记录 lab3 实际使用的提示词。实验报告只引用结论，完整 prompt 以本文件为准。
>
> 说明：lab3 的 prompt 在实现前先由小组给出任务与约束，AI 按约束生成代码，
> 再由小组通过 `make qemu` 与 `make grade` 的真实输出逐条验收；报告中“实现迭代过程”
> 记录的是验收中发现的问题与对 prompt / 实现的修正。

---

## 提示词 1：实现 RESTORE_ALL 与 __alltraps（对应功能模块一）

**使用场景**：`kern/trap/trapentry.S` 中 `SAVE_ALL` 已给定，`RESTORE_ALL` 与 `__alltraps`
只有 `//lab3 function1 YOUR CODE` 占位。中断能触发但无法正确返回，内核在第一次中断后崩溃或死循环。

```text
[PROMPT]
**任务**：在 kern/trap/trapentry.S 中实现两个部分：
1. `.macro RESTORE_ALL`：从栈上的 trapframe 恢复 4 个 CSR（sstatus/sepc/sbadaddr/scause）
   和 32 个通用寄存器，与 SAVE_ALL 严格对称；
2. `__alltraps`：在 SAVE_ALL 之后，把当前 sp（即 trapframe 起始地址）按 RISC-V 调用约定
   作为第一个参数传给 C 函数 trap()，调用返回后落到 __trapret 执行 RESTORE_ALL 和 sret。

**操作要求**：
1. RESTORE_ALL 恢复 CSR 时，先用 s1/s2/s3/s4 作为临时寄存器从栈上 LOAD 再 csrw，
   因为 csrw 不支持直接从内存取值；
2. 通用寄存器 x0 恒为 0 不需要恢复，x2(sp) 必须放在最后恢复——否则恢复 sp 之后
   就无法再用它寻址 trapframe 里的其他寄存器；
3. __alltraps 用 `mv a0, sp` 传参，用 `jal trap` 调用（不要用 call 伪指令，保持显式）；
4. 只修改 RESTORE_ALL 宏体和 __alltraps 函数体，不要改动 SAVE_ALL、__trapret、sret。

**输出要求**：给出最终汇编代码，并说明为什么 sp 必须最后恢复、为什么 CSR 恢复要放在 GPR 恢复之前。

[RELY]
- trapframe 布局（由 SAVE_ALL 决定）：
  sp+0*8  = x0, sp+1*8 = x1, ..., sp+31*8 = x31
  sp+32*8 = sstatus, sp+33*8 = sepc, sp+34*8 = sbadaddr, sp+35*8 = scause
- REGBYTES = 8（rv64），LOAD = ld，STORE = sd，定义在 trap.h
- SAVE_ALL 中 x2 的保存值是“进入中断前的 sp”，RESTORE_ALL 必须把它写回 x2
- s1=x9, s2=x18, s3=x19, s4=x20, s5=x21；这些寄存器本身也在 trapframe 中，
  用它们作临时 CSR 中转后，后续 LOAD x9/x18/x19/x20/x21 会覆盖，不影响最终状态
- RISC-V 调用约定：第一个参数走 a0(x10)；jal 指令把返回地址写入 ra(x1)
- C 函数 trap(struct trapframe *tf) 在 trap.c 中定义，接收 trapframe 指针

[GUARANTEE]
- 不修改 SAVE_ALL、trap.h、trap.c；
- 编译必须通过（-march=rv64imac -mcmodel=medany）；
- 运行 make qemu 时内核必须能从第一次时钟中断正确返回并继续打印 100 ticks，
  不能在第一次中断后崩溃或死循环。

[SPECIFICATION]
- Post-Condition：
  * RESTORE_ALL 执行后，所有 32 个 GPR（x1～x31）和 4 个 CSR 恢复到中断前的值；
  * x2(sp) 在所有其他 LOAD 之后才被恢复；
  * __alltraps 调用 trap() 时 a0 == sp（trapframe 地址）；
  * trap() 返回后 PC 指向 __trapret，执行 RESTORE_ALL + sret。
- Requirements：禁止在恢复 sp 之后再用 sp 寻址 trapframe；禁止用 call 伪指令（必须显式 jal）。
```

**AI 关键产出**：`RESTORE_ALL` 先恢复 sstatus/sepc/sbadaddr/scause（用 s1～s4 中转），
再按 x1、x3～x31、x2 的顺序恢复寄存器，sp 最后一条 `LOAD x2, 2*REGBYTES(sp)`；
`__alltraps` 用 `mv a0, sp; jal trap`。`make qemu` 正常输出 `100 ticks`，中断返回正确。

---

## 提示词 2：实现 interrupt_handler 与 exception_handler（对应功能模块二）

**使用场景**：`kern/trap/trap.c` 中 `trap_dispatch()` 已给定，会根据 `tf->cause` 的符号
调用 `interrupt_handler(tf)` 或 `exception_handler(tf)`，但两个函数体只有
`//lab3 function2 YOUR CODE` 占位。时钟中断不触发第二次、非法指令异常死循环。

```text
[PROMPT]
**任务**：在 kern/trap/trap.c 中实现两个函数：
1. interrupt_handler(struct trapframe *tf)：处理中断，按时钟/软件/外部/未知分类；
2. exception_handler(struct trapframe *tf)：处理异常，对非法指令和断点推进 tf->epc。

**操作要求**：
1. 时钟中断（IRQ_S_TIMER）必须调用 clock_set_next_event() 重设下一次事件，
   否则只会触发一次；每 TICK_NUM 次 tick 调用 print_ticks()，累计 SHUTDOWN_AFTER 次后
   调用 sbi_shutdown()；
2. 软件中断（IRQ_S_SOFT）打印 "Supervisor software interrupt"；
   外部中断（IRQ_S_EXT）打印 "Supervisor external interrupt"；
   未知中断打印 "Unknown interrupt: cause %d"（取 cause 低 8 位）并调用 print_trapframe；
3. 非法指令异常（CAUSE_ILLEGAL_INSTRUCTION）打印 "Illegal instruction at 0x%016lx"，
   断点异常（CAUSE_BREAKPOINT）打印 "Breakpoint at 0x%016lx"；
4. epc 的推进量必须根据指令编码动态判断：读 epc 处的低 16 位，若低两位为 0b11 则是
   32 位指令（+4），否则是 16 位压缩指令（+2）——禁止写死 +4；
5. 未识别的异常调用 print_trapframe(tf) 打印全部寄存器。

**输出要求**：给出最终 C 代码，并说明时钟中断为什么必须重设、epc 为什么不能写死 +4。

[RELY]
- trap.h：struct trapframe { uint64_t gpr[32]; uint64_t status; uint64_t epc;
  uint64_t badvaddr; uint64_t cause; }，字段顺序即 SAVE_ALL 的压栈顺序
- riscv.h：IRQ_S_SOFT=1, IRQ_S_TIMER=5, IRQ_S_EXT=9；
  CAUSE_ILLEGAL_INSTRUCTION=2, CAUSE_BREAKPOINT=3
- cause 的 bit63 为 1 表示中断，低 8 位是中断号；trap_dispatch 中 cause<0 走中断分支
- clock.h：clock_set_next_event() 写 mtimecmp 到当前 mtime + 一个 tick
- sbi.h：sbi_shutdown() 通过 SBI 调用让 QEMU 退出
- trap.c 已有全局变量 ticks 和 print_ticks()（每 TICK_NUM 次打印 "%d ticks"）
- RISC-V 指令编码：32 位指令低两位必为 0b11；16 位压缩指令低两位为 0b00/01/10

[GUARANTEE]
- 不修改 trap_dispatch、trap.h、clock.h；
- 编译必须通过 -Werror；
- make grade 三个测试必须全过：
  * Test 1：断点异常返回，epc 正确推进；
  * Test 2：非法指令按 4 字节推进、软件中断/未知中断输出格式正确、cause=0x14；
  * Test 3：时钟中断持续触发到 ticks=200。

[SPECIFICATION]
- Post-Condition：
  * 时钟中断每次都重设 timecmp，ticks 单调递增；
  * 非法指令/断点异常返回后 epc 跳过故障指令（长度按编码判断）；
  * 未知中断/异常有诊断输出，不静默。
- Requirements：禁止在异常处理中写死 epc += 4；禁止在时钟中断中省略 clock_set_next_event。
```

**AI 关键产出**：`interrupt_handler` 按 `cause & 0xFF` 分类，时钟分支重设事件并计数；
`exception_handler` 用 `instruction_length(epc)` 辅助函数（读低 16 位判断 2/4 字节）推进 epc。
`make grade` 三个测试全部 PASS，总分 100/100。
