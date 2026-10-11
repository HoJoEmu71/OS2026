# Lab 1 提示词汇总（prompt.md）

> 本实验为"最小可执行内核"分析型实验，核心是理解启动流程与 GDB 调试，编码量很小，
> 因此本文件汇总实验过程中**实际向 AI 编程助手（TraeCode / GLM-5.3-Flash）提出的全部提示词**，
> 并按 lab0.5 学习的四段式框架（PROMPT / RELY / GUARANTEE / SPECIFICATION）整理。
> 从 lab2 起，每个编码功能都会严格使用完整四段式提示词。

---

## 提示词 1：分析内核入口汇编（对应练习1）

**使用场景**：阅读 `kern/init/entry.S`，需要弄清 `la sp, bootstacktop` 与 `tail kern_init` 的作用。

```text
[PROMPT]
**任务**：阅读并分析 kern/init/entry.S，回答两个问题：
1. `la sp, bootstacktop` 完成了什么操作，目的是什么？
2. `tail kern_init` 完成了什么操作，目的是什么？
**操作要求**：结合 kern/mm/mmu.h、kern/mm/memlayout.h、tools/kernel.ld 一起分析，
并用 riscv64-unknown-elf-objdump 反汇编 bin/kernel 验证你的结论，不要只凭伪指令名称猜测。
**输出要求**：给出每条伪指令展开后的真实指令、相关符号（bootstack/bootstacktop/kern_init）
的链接地址，以及"为什么必须这样做"的因果解释。

[RELY]
- entry.S 中 `.section .data` 下定义了 `bootstack:`（.space KSTACKSIZE）与 `bootstacktop:`
- mmu.h: KSTACKSIZE = 2 * PGSIZE，PGSHIFT = 12（即 8KB）
- kernel.ld: .text 从 0x80200000 开始，.data 按 PGSHIFT(4KB) 对齐
- kern_init 在 kern/init/init.c 中，声明为 __attribute__((noreturn))，末尾 while(1)

[GUARANTEE]
只做分析，不修改任何源代码；结论必须能被 objdump / nm 的真实输出佐证。

[SPECIFICATION]
- Post-Condition：说清 la 展开为 auipc+addi、tail 展开为 jal x0/j，并给出 sp 的最终值与
  RISC-V 函数调用约定（sp、ra）之间的关系。
- Requirements：区分"尾调用 tail"与普通 call 在是否保存返回地址上的差异。
```

**AI 关键产出**：`la sp` 展开为 `auipc sp,0x3; addi sp,sp,0`，sp=0x80203000；
`tail` 展开为 `j 8020000a`。均经反汇编核实，写入实验报告练习1。

---

## 提示词 2：设计 GDB 启动流程跟踪方案（对应练习2）

**使用场景**：需要用 GDB 实际观察 0x1000 → 0x80000000 → 0x80200000 的跳转，验证加电后第一条指令的位置。

```text
[PROMPT]
**任务**：为 lab1 内核设计一套可复现的 GDB 调试流程，跟踪 CPU 从上电到进入 kern_init 的完整路径，
回答"RISC-V 加电后最初的几条指令位于哪里"。
**操作要求**：利用 Makefile 的 make debug（QEMU 带 -s -S）与 gdb-multiarch，
用批处理脚本（gdb -batch -x）自动下断点、单步并保存输出，不要只给交互式手动步骤。
**输出要求**：给出每个阶段（复位向量 / OpenSBI / 内核入口 / C 函数）的 pc、关键寄存器值与指令。

[RELY]
- QEMU virt 机器：-bios default（OpenSBI v0.9），-kernel 加载内核到 0x80200000
- gdbserver 端口 localhost:1234；架构 set architecture riscv:rv64
- 已知符号：0x80000000=固件基址，0x80200000=kern_entry，kern_init=0x8020000a

[GUARANTEE]
断点地址 0x1000 / 0x80000000 / 0x80200000 必须与指导书一致；脚本结束后要 kill QEMU，
不得留下挂起进程。

[SPECIFICATION]
- Case 1：在 0x1000 用 x/6i 反汇编 MROM，并解释 auipc/addi/csrr mhartid/ld/jr 各自准备的
  启动参数（a0=mhartid、a1=DTB、a2=配置结构）。
- Case 2：在 0x80200000 用 stepi 单步，验证 sp 由 OpenSBI 遗留值变为 bootstacktop，
  再由 tail 跳到 0x8020000a 进入 C 源码。
- Requirements：所有寄存器值必须来自真实 GDB 输出，禁止编造。
```

**AI 关键产出**：批处理 GDB 脚本，证实首条指令在 `0x1000`（MROM）而非 `0x80000000`，
并记录 sp `0x80017ee0 → 0x80203000`、pc `0x80200000 → 0x8020000a`，截图见 `images/gdb_boot_trace.png`。

---

## 提示词 3：诊断 make qemu 无内核输出（环境适配）

**使用场景**：骨架代码 `make` 编译成功、OpenSBI banner 正常，但始终没有 `(THU.CST) os is loading ...`。

```text
[PROMPT]
**任务**：定位"make qemu 时 OpenSBI 已启动但内核没有任何输出"的根因并修复 Makefile。
**操作要求**：先用 GDB 在 0x80200000 下断点确认内核到底有没有被执行；再对比 OpenSBI banner 中
Domain0 Next Address 的实际值；只修改 Makefile 的 qemu/debug 目标，不动内核源码。
**输出要求**：说明根因、给出最小修改，并用 make qemu 重新验证出现 (THU.CST) 输出。

[RELY]
- 原 Makefile qemu 规则：-device loader,file=bin/ucore.img,addr=0x80200000
- 现象：banner 中 Domain0 Next Address = 0x0
- 手动用 -kernel bin/ucore.img 时 Next Address = 0x80200000 且内核正常输出

[GUARANTEE]
修改后 qemu 与 debug 两个目标都要可用；debug 仍需保留 -s -S 供 GDB 连接。

[SPECIFICATION]
- Post-Condition：make qemu 输出 (THU.CST) os is loading ...；Next Address=0x80200000。
- Requirements：解释为何 QEMU 6.2/OpenSBI v0.9 下 -device loader 不能像 QEMU 4.1 那样
  被固件当作下一阶段跳转地址，而 -kernel 会登记到 fw_dynamic 的启动信息中。
```

**AI 关键产出**：确认根因是 QEMU 版本差异——旧的 `-device loader` 方式不会把内核登记为
SBI 的下一阶段地址（Next Address=0x0），改为 `-kernel bin/ucore.img` 后启动正常。
该适配已写入实验报告"测试与验证"备注。

---

## 提示词工程小结

- 分析型任务也要给 AI **可验证的约束**（"必须用 objdump/GDB 真实输出佐证，禁止编造"），
  能有效避免 AI 凭印象给出似是而非的地址和结论。
- 调试型任务把**现象、已知事实、怀疑点**分段列清（[RELY]），AI 给出的 GDB 脚本一次就能跑通。
- 环境类问题让 AI 直接执行命令取证，比让它"猜原因"高效：Next Address=0x0 就是通过真实
  banner 与 GDB 断点共同锁定的。
