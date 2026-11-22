# 操作系统实验报告：Lab 4

## 实验基本信息

| 项目 | 内容 |
|------|------|
| 实验名称 | Lab 4：内核线程管理 |
| 小组成员 | 2413238-罗清、2412951-申一博、2413191-马云飞 |
| 完成日期 | 2026-11-20 |
| 实验环境 | Windows 11、WSL2 Ubuntu 22.04、riscv64-unknown-elf-gcc 10.2.0、QEMU 6.2.0、OpenSBI 0.9 |

---

## 一、本章节整体逻辑线

本实验围绕“虚拟内存页操作 → 进程创建与初始化 → 进程调度”三条主线展开。内核首先利用 lab2 的物理页分配能力和 lab3 的中断返回能力，随后建立页映射、创建内核线程，最后让新线程真正占用 CPU 执行。

完整执行链为：

```text
kern_init
  → pmm_init                 物理页管理、页表和映射自检
  → pic_init / idt_init      中断入口初始化
  → vmm_init                 虚拟内存区域管理初始化
  → proc_init                建立 idleproc 和 initproc
  → cpu_idle                 idle 线程主动请求调度
  → schedule                 选择下一个就绪线程
  → proc_run                 切换 current、页表和执行上下文
  → switch_to                保存/恢复寄存器
  → forkret → forkrets → __trapret
  → kernel_thread_entry      调用新线程指定函数
```

三条主线的关系如下：

1. **虚拟内存页操作**提供地址映射基础。`page_insert` 和 `page_remove` 维护页表项、物理页引用计数和 TLB 的一致性。
2. **进程创建及初始化**把内核中的一段执行流包装成可调度的线程。`idleproc` 复用启动时的执行现场，`initproc` 则通过 `kernel_thread` 和 `do_fork` 创建。
3. **进程调度**完成从“存在线程对象”到“线程真正运行”的跨越。调度器选择就绪线程，`proc_run` 和 `switch_to` 完成上下文切换。

此外，按指导书要求补回了 lab2 的 Best-Fit 物理页分配、`page_init`，以及 lab3 的 `RESTORE_ALL`、`__alltraps` 和中断/异常分发代码。未实现 Challenge，也未扩展 `do_exit`。

---

## 二、功能模块一：虚拟内存页操作

### 2.1 Spec 怎么写

本模块的输入是页目录、物理页描述符、线性地址和权限位，输出是映射建立结果或删除后的页表状态。规格重点不是“写入一个 PTE”这么简单，而是要求以下三者始终同步：

- 页表项中的物理页号和权限；
- `struct Page` 的引用计数；
- CPU 中可能缓存旧映射的 TLB。

需要覆盖四种情况：首次建立映射、重复插入同一物理页、用新物理页替换旧映射、删除不存在的映射，分配中间页表失败时返回 `-E_NO_MEM`。

### 2.2 核心函数理解

#### `page_insert`

1. 调用 `get_pte(pgdir, la, 1)` 找到目标 PTE，必要时创建中间级页表。
2. 如果原 PTE 无效，直接增加新页引用计数。
3. 如果原 PTE 已映射到另一物理页，先调用 `page_remove` 撤销旧映射，再增加新页引用计数。
4. 如果重复插入同一个物理页，只更新权限，不重复增加引用计数，也不能先把引用计数减到零而误释放该页。
5. 使用 `pte_create` 写入新 PTE，并调用 `tlb_invalidate` 使旧地址翻译失效。

#### `page_remove`

1. 使用 `get_pte(pgdir, la, 0)` 查找现有 PTE，不创建新页表。
2. 地址未映射时直接返回，保证删除操作幂等。
3. 对物理页引用计数减一；降到零时调用 `free_page` 回收物理页。
4. 清空 PTE 并使对应 TLB 项失效。

### 2.3 Prompt 怎么写

提示词明确限定了函数、数据结构、异常情况和禁止事项，要求只填写指导书指定位置，实际规格和指令记录见 `prompt.md` 的功能模块一部分。

---

## 三、功能模块二：进程创建及初始化

### 3.1 Spec 怎么写

本模块需要把 PCB、内核栈、PID、链表、trapframe 和 context 组合成一个可运行线程，规格分别约束：

- `alloc_proc`：返回字段安全的空白 PCB；
- `proc_init`：建立进程链表和哈希链表，构造 `idleproc`，再创建 `initproc`；
- `kernel_thread`：把函数指针和参数包装成临时 trapframe；
- `do_fork`：分配资源、登记线程、处理失败清理并返回 PID；
- `copy_thread`：在新线程内核栈上安排首次返回所需的 trapframe 和 context；
- `copy_mm`：本实验只保留地址空间接口，不创建用户地址空间。

### 3.2 核心函数理解

#### `alloc_proc`

通过 `kmalloc` 分配 PCB 并清零，设置 `PROC_UNINIT`、`pid = -1` 和内核页目录物理地址 `boot_pgdir_pa`，同时初始化两个链表节点，这样未分配资源保持为零，后续出错清理不会误用随机字段。

#### `proc_init`

`idleproc` 不是普通 fork 出的线程，而是当前启动执行流的 PCB 表示。它被赋予 PID 0、`bootstack` 和 `PROC_RUNNABLE` 状态，并令 `current = idleproc`、`need_resched = 1`。

随后调用：

```c
kernel_thread(init_main, "Hello world!!", 0)
```

创建 PID 1 的 `initproc`。最后通过 PID 哈希表找到它并设置名称。`idleproc` 不加入普通进程链表，`initproc` 由 `do_fork` 加入链表。

#### `kernel_thread`

临时 trapframe 中：

- `s0` 保存线程函数地址；
- `s1` 保存函数参数；
- `epc` 指向 `kernel_thread_entry`；
- `status` 设置 `SSTATUS_SPP | SSTATUS_SPIE` 并清除 `SSTATUS_SIE`，保证 `sret` 回到 S 态，并在返回后按 SPIE 恢复中断。

#### `do_fork`

依次执行：检查进程数量、分配 PCB、记录父进程、分配内核栈、处理 mm、复制线程现场、在关中断区域分配 PID 并加入哈希表和进程链表、增加进程计数、设为就绪态。任一步失败都按相反顺序释放已申请资源。

#### `copy_thread`

在新内核栈顶部放置 trapframe，将子线程返回值 `a0` 设为零，并设置恢复后的栈指针。context 中：

- `ra = forkret`；
- `sp = proc->tf`。

因此第一次 `switch_to` 的 `ret` 会进入 `forkret`，再经 `forkrets` 把栈指针切到 trapframe，最终由 `__trapret` 恢复寄存器并 `sret` 到 `kernel_thread_entry`。

#### `copy_mm`

当前只有内核线程，`current->mm == NULL`，不涉及用户地址空间复制，所以检查该前提并返回成功。

### 3.3 Prompt 怎么写

提示词按资源创建顺序描述每个函数的后置条件，并特别写明首次运行链路和失败清理要求，避免只初始化 PCB 却无法启动线程。实际记录见 `prompt.md` 的功能模块二部分。

---

## 四、功能模块三：进程调度

### 4.1 Spec 怎么写

调度模块需要完成四项工作：

- `cpu_idle` 在 `need_resched` 置位时主动调用 `schedule`；
- `schedule` 从当前位置循环扫描进程链表，选择下一个 `PROC_RUNNABLE` 线程，找不到时退回 `idleproc`；
- `proc_run` 在关中断区域更新 `current`、切换 Sv39 页表、刷新 TLB 并调用 `switch_to`；
- `switch_to` 保存和恢复 `ra`、`sp`、`s0`～`s11`。

调度选择与底层切换分离，关键区域不允许被时钟中断打断。

### 4.2 核心函数理解

#### `schedule`

清除当前线程的 `need_resched`。若当前是 idle，则从链表头开始；否则从当前线程的下一个节点开始。扫描整个循环链表但不重复选择当前线程，从而实现轮转。没有就绪线程时选择 `idleproc`。

#### `proc_run`

只有目标线程不同于 `current` 才切换。进入临界区后先保存旧线程指针，再更新 `current`。`satp` 由 Sv39 模式位和 `proc->pgdir >> RISCV_PGSHIFT` 组成；不能使用只能表达 Sv32 的 `lsatp()`。随后刷新 TLB、增加运行次数并执行 `switch_to`。

#### `switch_to`

`a0` 指向旧 context，`a1` 指向新 context。前半段把旧线程的 callee-saved 寄存器写入 `a0`，后半段从 `a1` 恢复新线程寄存器，最后 `ret` 到新 context 的 `ra`。无需保存临时寄存器，因为 C 调用约定允许调用者自行处理它们。

#### `cpu_idle`

idle 不主动执行具体任务，只持续检查调度标记。存在其他就绪线程时，调度器应直接选中它们，而不是错误退回 idle。

### 4.3 Prompt 怎么写

提示词要求“循环遍历整个链表”和“找不到才退回 idle”，并把关中断、Sv39 `satp`、callee-saved 寄存器范围列为验收条件。实际记录见 `prompt.md` 的功能模块三部分。

---

## 五、生成与调试过程中出现的 Bug

### 5.1 原始骨架无法直接编译

**现象：** 首次 `make` 报 `best_fit_pmm.h: No such file or directory`，同时 lab2/lab3 的占位代码尚未填写。

**处理：** 按指导书先补回此前实验代码；lab4 的管理器导出名为 `default_pmm_manager`，因此保留该接口并使用骨架已有的 `default_pmm.h`，内部算法仍为此前完成的 Best-Fit，不另加新分配器。

### 5.2 QEMU 6.2 下内核无法按旧参数启动

**现象：** 本环境沿用骨架的 `-device loader,file=...,addr=0x80200000` 不适合当前 QEMU 6.2 + OpenSBI 0.9 组合。

**处理：** 与前几个实验一致，将 Makefile 和 `tools/grade.sh` 的加载参数改为 `-kernel bin/ucore.img`，不改变内核代码。

### 5.3 首次评分为 20/30，调度测试未通过

**现象：** PMM 和 Process Create 通过，但缺少 `[Test Schedule] PASS` 以及 initproc 的三行输出。

**原因：** 初版 `schedule` 只从当前节点扫描到链表头，没有绕回当前节点之前的部分。当 initproc 位于链表尾部时，即使链表中仍有就绪测试线程，也会错误选择 idleproc。idle 的 `need_resched` 已被清除后，会在空闲循环中等待，调度测试无法按预期完成。

**修复：** 记录扫描起点，沿循环链表扫描一周，跳过链表头和当前线程；只有完整扫描后仍没有就绪线程时才选择 `idleproc`。

**结果：** 重新运行 `make grade`，三个项目全部通过，总分 30/30。

### 5.4 保留的编译警告

评分注入文件中的测试线程返回类型为 `void (*)(void *)`，而骨架接口为 `int (*)(void *)`；`kmalloc.c` 也存在骨架原有的指针类型警告。这些警告不妨碍构建和测试，且相关文件不属于本次必做修改点，因此未擅自改动。

---

## 六、运行结果

### 6.1 `make qemu`

关键输出如下：

```text
memory management: best_fit_pmm_manager
check_alloc_page() succeeded!
check_pgdir() succeeded!
check_boot_pgdir() succeeded!
kmalloc_init() succeeded!
check_vma_struct() succeeded!
check_vmm() succeeded.
++ setup timer interrupts
this initproc, pid = 1, name = "init"
To U: "Hello world!!".
To U: "en.., Bye, Bye. :)"
kernel panic at kern/process/proc.c:208:
    process exit!!.
```

initproc 的三行输出说明创建、首次恢复和调度切换均已执行。随后的 `process exit!!` 来自骨架中尚未展开的 `do_exit`，本实验不要求实现进程退出，因此保留该行为。

### 6.2 `make grade`

```text
-check PMM:                                OK
-check Process Create:                     OK
-check Schedule:                           OK
Total Score: 30/30
```

---

## 七、重要实验知识点与 OS 原理的关系

| 实验知识点 | 对应 OS 原理 | 含义、关系与差异 |
|------------|--------------|------------------|
| 页表项、引用计数、TLB 同步 | 虚拟内存与地址翻译 | 页表描述映射，引用计数描述物理页共享程度，TLB 是硬件缓存。修改页表不代表旧 TLB 自动失效，因此软件需要维护一致性。 |
| 多级页表 | 分页存储管理 | Sv39 用多级索引减少页表本身的空间开销；本实验主要使用其中两级查找逻辑，原理上可扩展到更多级。 |
| PCB | 进程控制块 | PCB 保存状态、PID、栈、上下文和地址空间等管理信息。本实验的 PCB 服务于内核线程，用户资源字段尚未完整使用。 |
| 内核线程 | 线程与执行流 | 内核线程共享内核地址空间，但有独立内核栈和寄存器上下文。相比用户进程，它不需要独立用户页表。 |
| 上下文切换 | CPU 在任务间切换 | 调度器决定“切换到谁”，`switch_to` 负责“怎样切换”。本实验只保存 callee-saved 寄存器，是内核主动切换的精简实现。 |
| 就绪、睡眠、僵尸等状态 | 进程状态模型 | 状态决定线程能否被选中。本实验主要使用 UNINIT 和 RUNNABLE，完整状态转换留待后续实验。 |
| FIFO/轮转扫描 | CPU 调度 | 本实验从当前位置选择下一个就绪线程，体现简单轮转思想；没有时间片、优先级和动态反馈等复杂策略。 |
| 关中断临界区 | 同步与原子性 | 更新当前线程、页表和寄存器上下文必须作为一个整体完成，否则中断处理可能看到不一致状态。 |
| trapframe 与 context | 中断现场和任务现场 | trapframe 面向硬件 trap 的完整恢复，context 面向内核主动切换的少量寄存器保存；新线程首次运行把二者串联起来。 |

---

## 八、OS 原理中重要但本实验未对应的知识点

1. **用户态进程与地址空间隔离：** 本实验只有共享内核页表的内核线程，尚未创建独立用户地址空间。
2. **进程退出与资源回收：** `do_exit` 仍为骨架 panic，尚未覆盖僵尸线程回收、父进程等待和资源释放。
3. **阻塞与唤醒的完整机制：** 虽定义了 SLEEPING 等状态，但没有等待队列、睡眠原因和事件驱动唤醒。
4. **时间片与抢占式调度：** 当前调度点主要是主动设置 `need_resched`，未实现基于时钟的完整抢占策略。
5. **优先级、公平性与负载均衡：** 没有优先级、nice、调度延迟指标，也不涉及多核负载迁移。
6. **并发同步原语：** 关中断只适用于很短的单核临界区，尚未实现锁、信号量及多核同步。
7. **线程与进程的资源共享语义：** `CLONE_VM`、`CLONE_THREAD` 等标志尚未展开，文件描述符和信号等资源也未涉及。

---

## 九、实验结论

本实验完成了页映射维护、内核线程创建和调度切换三条必做主线。最终 `make qemu` 能运行 initproc，`make grade` 得分 30/30。通过调度 bug 的修复，可以看到循环链表遍历范围会直接影响调度正确性；仅“能够切换一次”并不足以证明调度器在其他线程反复让出 CPU 时仍然正确。
