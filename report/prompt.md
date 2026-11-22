# Lab 4 实际提示词与反馈记录

## 记录说明

本文件记录本次实际使用的用户指令、实现前读取的指导书规格，以及测试后真实发生的反馈。

## 0. 用户总指令（原文）

> 继续按照指导书的步骤完成lab4，不要自作主张

据此确定的执行约束是：先读取指导书并确认必做项；按“虚拟内存页操作 → 进程创建与初始化 → 进程调度”的顺序执行；补回指导书要求的 lab2/lab3 代码；不实现未要求的进程退出等扩展功能；不自行提交或推送。

## 1. 功能模块一：虚拟内存页操作

### 实现前读取的任务规格

来源：指导书“练习”中的功能模块一。

```text
只实现 kern/mm/pmm.c 中的 page_insert 和 page_remove。

page_insert：
1. 使用 get_pte(pgdir, la, 1) 获取页表项，失败返回 -E_NO_MEM。
2. 正确处理首次映射、重复映射同一物理页、替换其他物理页三种情况。
3. 保持页表项、物理页引用计数和 TLB 状态一致。
4. 更新后执行 TLB 失效。

page_remove：
1. 使用 get_pte(pgdir, la, 0) 查找已有映射。
2. 未映射时安静返回，接口应具有幂等性。
3. 删除有效映射时维护引用计数，引用计数降到零时回收物理页。
4. 清空页表项并执行 TLB 失效。

可使用 pte_create、pte2page、page_ref_inc、page_ref_dec、free_page、
tlb_invalidate 等现有接口。不要修改骨架自检代码，不做 Challenge。
```

### 实际验证

骨架的 `check_pgdir`、`check_boot_pgdir` 以及注入的 `[Test PMM] PASS` 均通过，重点检查了“同一物理页重复插入时引用计数不增加、空闲页数不变化”的情况。

## 2. 功能模块二：进程创建及初始化

### 实现前读取的任务规格

来源：指导书“练习”功能模块二，以及“创建第 0 个内核线程 idleproc”“创建第 1 个内核线程 initproc”。

```text
只完成 alloc_proc、proc_init、kernel_thread、do_fork、copy_thread、copy_mm。

alloc_proc：分配并清零 PCB，设置 PROC_UNINIT、pid=-1、boot_pgdir_pa，
使未分配资源保持安全的零值。

proc_init：初始化进程链表和哈希链表；把当前执行现场包装成 PID 0 的
idleproc，使用 bootstack，设为 RUNNABLE，need_resched=1，current=idleproc；
再通过 kernel_thread(init_main, "Hello world!!", 0) 创建 PID 1 的 initproc。

kernel_thread：用临时 trapframe 保存 fn、arg 和 kernel_thread_entry；
status 设置 SPP、SPIE 并清除 SIE，然后调用 do_fork。

do_fork：依次分配 PCB、内核栈，处理 mm，复制线程现场；在关中断区域分配
PID、加入哈希表和进程链表、增加进程计数；最后 wakeup_proc 并返回 PID。
失败时释放已经申请的资源。

copy_thread：在新内核栈顶部保存 trapframe，设置子线程 a0=0 和恢复后的 sp；
context.ra=forkret，context.sp=proc->tf，形成
forkret → forkrets → __trapret → kernel_thread_entry 的首次运行链路。

copy_mm：本实验不复制用户地址空间，保持接口完整即可。
```

### 实际验证

`[Test Process Create] PASS` 通过，确认 idleproc/initproc 的 PID、状态、父进程、内核栈、mm、tf 和 context 满足注入测试要求。

## 3. 功能模块三：进程调度

### 实现前读取的任务规格

来源：指导书“练习”功能模块三和“调度并执行内核线程 initproc”。

```text
只实现 cpu_idle、schedule、proc_run、switch_to。

cpu_idle：持续检查 current->need_resched，置位时调用 schedule。

schedule：清除当前线程的 need_resched；从当前线程之后开始选择下一个
PROC_RUNNABLE 线程，idle 从链表头开始；扫描范围必须覆盖整个循环链表，
找不到就绪线程时才退回 idleproc。关键区域关中断。

proc_run：目标不同于 current 时，在关中断区域更新 current，按照 Sv39
格式写 satp，刷新 TLB，增加 runs，并调用 switch_to。不能使用 Sv32 遗留
的 lsatp()。

switch_to：a0 是旧 context，a1 是新 context；保存并恢复 ra、sp、s0-s11，
最后 ret。不要加入调度策略，不保存本接口无需保存的临时寄存器。
```

### 真实测试反馈与修正指令

第一次 `make grade` 的真实结果为：

```text
-check PMM:                                OK
-check Process Create:                     OK
-check Schedule:                           WRONG
Total Score: 20/30
```

根据该结果形成的修正要求是：

```text
检查 schedule 的链表扫描范围。当前实现只从当前节点扫描到链表头，没有绕回，
会在 initproc 位于链表尾部时错误退回 idleproc。改为记录扫描起点并循环
扫描一周，跳过链表头和当前线程；只有完整扫描后仍没有 RUNNABLE 线程时才
选择 idleproc。其他模块不改。
```

修正后的真实结果为：

```text
-check PMM:                                OK
-check Process Create:                     OK
-check Schedule:                           OK
Total Score: 30/30
```

## 4. 环境适配指令

沿用前几个实验已确认的环境约束：

```text
当前使用 QEMU 6.2.0 + OpenSBI 0.9。将 Makefile 和 tools/grade.sh 中旧的
-device loader,file=...,addr=0x80200000 加载方式改为 -kernel bin/ucore.img。
该修改只解决当前模拟器加载兼容性，不改变实验功能。
```

## 5. 未执行的任务

以下内容没有进入实现提示词，也没有实现：

- Challenge：`local_intr_save/local_intr_restore` 原理说明；
- Challenge：Sv32/Sv39/Sv48 对比及 `get_pte` 是否拆分的讨论；
- `do_exit` 的完整进程退出与资源回收；
- 其他调度算法、用户进程或额外内核功能。
