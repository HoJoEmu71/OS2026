# Lab 2 提示词汇总（prompt.md）

> 本文件按 lab0.5 学习并沿用至 lab1 的四段式框架（PROMPT / RELY / GUARANTEE / SPECIFICATION）
> 记录 lab2 实际使用的提示词。实验报告只引用结论，完整 prompt 以本文件为准。
>
> 说明：lab2 的 prompt 在实现前先由小组给出任务与约束，AI 按约束生成代码，
> 再由小组通过 `make qemu` 与 `make grade` 的真实输出逐条验收；报告中“实现迭代过程”
> 记录的是验收中发现的问题与对 prompt / 实现的修正。

---

## 提示词 1：实现 page_init（对应功能模块一任务一）

**使用场景**：`kern/mm/pmm.c` 中只保留了 `//lab2 function1 YOUR CODE` 注释，`pmm_init()`
调用 `page_init()`，但函数尚未实现，内核无法建立页描述符与空闲页链表。

```text
[PROMPT]
**任务**：在 kern/mm/pmm.c 中实现 static void page_init(void)，完成物理内存探测与页描述符初始化。
**操作要求**：
1. 从设备树接口 get_memory_base()/get_memory_size() 取得物理内存范围；
2. 正确设置全局变量 va_pa_offset、npage、pages，其中 pages 指向内核 end 之后、页对齐的
   struct Page 描述符数组；
3. 把排除 OpenSBI 区、内核镜像和描述符数组之后的完整空闲页，通过 init_memmap(base, n)
   注册给当前 pmm_manager；
4. 内存信息无效时必须 panic，不得继续初始化；没有完整空闲页时不得调用 init_memmap；
5. 只允许修改 page_init 及其所需局部变量，不得修改 pmm_init 的调用顺序、entry.S 和启动页表。
**输出要求**：给出最终代码，并说明虚拟地址、物理地址、页号三者之间的换算依据；
同时说明 pages 数组覆盖的页号范围，以及内核尾页为什么必须保持 reserved。

[RELY]
- 启动页表 boot_page_table_sv39 采用 1GiB 大页：VA 0xffffffffc0000000 → PA 0x80000000，
  因此 0x80000000～0x88000000 的物理内存全部可经 PHYSICAL_MEMORY_OFFSET=0xFFFFFFFF40000000
  线性访问；KERNBASE=0xFFFFFFFFC0200000 对应 PA 0x80200000
- pmm.h：PADDR(kva)=kva-va_pa_offset；page2ppn(page)=page-pages+nbase；pa2page(pa) 要求
  PPN(pa)<npage；struct Page { ref; flags; property; page_link; }
- memlayout.h：PG_reserved/PG_property 宏，free_area_t，PHYSICAL_MEMORY_OFFSET/END
- mmu.h：PGSIZE=4096、PPN(la)=la>>12
- kernel.ld 提供 end 符号（.bss 结束、虚拟地址）；init_memmap 的调用约定是
  “base 起的 n 个页描述符当前必须是 PageReserved，函数内部会清除该标志并挂入空闲链表”
- QEMU virt：DTB 报告的内存为 [0x80000000, 0x88000000)，共 128MB；0x80000000～0x80200000
  被 OpenSBI 占用，nbase=DRAM_BASE/PGSIZE=0x80000

[GUARANTEE]
- 不修改 check_alloc_page、default_pmm.c、best_fit_pmm.c 中的测试函数；
- 编译必须通过 -Werror；运行 make grade 时 [Test 1] Page Init 必须 PASS；
- 内核不得因描述符数组覆盖内核镜像或把内核尾页交给分配器而崩溃。

[SPECIFICATION]
- Post-Condition：
  * va_pa_offset = PHYSICAL_MEMORY_OFFSET；
  * npage = PPN(get_memory_base()+get_memory_size())，且 npage > nbase；
  * pages 页对齐，pages ≥ ROUNDUP(end, PGSIZE)；pages[0..npage-nbase-1] 全部先置为
    reserved、ref=0、property=0；
  * 空闲区物理地址 = [ROUNDUP(PADDR(pages)+描述符数组大小, PGSIZE), ROUNDDOWN(内存末尾, PGSIZE))，
    以 pa2page(起点) 和页数调用 init_memmap；
  * 打印物理内存范围、描述符页数与空闲页数。
- Requirements：所有地址运算区分 VA/PA/PPN，禁止把虚拟地址直接当作物理地址交给 pa2page。
```

**AI 关键产出**：`page_init()` 以 `end` 为界放置 32768 个描述符（128MB/4KB），
先把全部描述符标为 reserved，再将内核镜像尾页之后到 0x88000000 的 31912 页交给
`init_memmap` 注册；`make grade` 的 `[Test 1] Page Init: PASS` 一次通过。

---

## 提示词 2：实现 Best-Fit 并接入内核（对应功能模块二任务二）

**使用场景**：`kern/mm/best_fit_pmm.c` 只提供 `basic_check()` 与 `best_fit_check()`，
没有管理函数、没有 `free_area`、也没有 `best_fit_pmm_manager`。

```text
[PROMPT]
**任务**：在 kern/mm/best_fit_pmm.c 中从零实现完整的 Best-Fit 物理内存管理模块，并在
kern/mm/pmm.c 的 init_pmm_manager() 中把当前管理器切换为 best_fit_pmm_manager。
**操作要求**：
1. 阅读 pmm.h 的 struct pmm_manager，按成员职责实现 init、init_memmap、alloc_pages、
   free_pages、nr_free_pages、check，并定义 const struct pmm_manager best_fit_pmm_manager；
2. 空闲块用 free_area_t + list.h 双向链表管理，块头页用 property 记录块大小并置 PG_property；
3. alloc_pages 必须真正选择“满足请求的最小空闲块”，不能退化为 First-Fit；
4. free_pages 需要按地址有序插入，并向前、向后合并相邻空闲块；
5. 不得修改 basic_check / best_fit_check 两个测试函数，也不得改测试中使用的状态约定
   （free_list、nr_free、PageProperty、property 的含义）。
**输出要求**：给出完整代码，并说明 Best-Fit 与 First-Fit 在扫描策略上的差异，
以及当前实现的时间复杂度。

[RELY]
- struct pmm_manager 六个成员：name/init/init_memmap/alloc_pages/free_pages/nr_free_pages/check
- default_pmm.c 已给出 First-Fit 参考：init_memmap 把连续 n 页登记为一个块，
  base->property=n + SetPageProperty(base)，nr_free+=n，按地址有序插入；
  free_pages 先按地址插入，再与前后相邻块合并
- best_fit_check 的关键序列：
  free_pages(p0+1,2); free_pages(p0+4,1);
  alloc_pages(4) 必须为 NULL；
  alloc_pages(1) 必须返回 p0+4（唯一的最小可满足块，而不是 p0+1 的 2 页块）；
  随后 free_pages(p0,5) 必须能合并 p0、p0+1..2、p0+4 并支持 alloc_pages(5)
- 描述符字段：ref、flags(PG_reserved/PG_property)、property、page_link；
  list.h 提供 list_init/list_add/list_add_before/list_del/list_next/list_prev/list_empty
- 页描述符由 page_init 建好，alloc/free 只操作描述符，不直接碰物理内存内容

[GUARANTEE]
- 不改测试函数与 pmm.h 接口；不引入 malloc/动态内存；
- 编译通过 -Werror；make grade 的 [Test 2] Best-Fit 必须 PASS；
- 任意 alloc/free 序列后必须满足 total(所有空闲块 property) == nr_free_pages()。

[SPECIFICATION]
- init：初始化 free_list、nr_free=0；
- init_memmap：断言 n>0、每页 reserved，清 flags/property/ref，块头 property=n、
  SetPageProperty、nr_free+=n，并按地址插入；
- alloc_pages：断言 n>0；n>nr_free 返回 NULL；遍历整条空闲链，记录 property≥n 且
  property 最小的块，遇到 property==n 可提前结束；取出后若块更大，把剩余部分
  （page+n 起）作为新块插回原位置；nr_free-=n；ClearPageProperty(page)；
- free_pages：断言 n>0，逐页断言 !PageReserved && !PageProperty 并清 ref/flags；
  块头 property=n、SetPageProperty、nr_free+=n；按地址插入后与前一后一共两个相邻块合并；
- check：直接复用文件内 best_fit_check。
```

**AI 关键产出**：`best_fit_alloc_pages` 遍历全部空闲块取最小可满足块（相等即 break），
释放沿用有序插入 + 双向合并；`pmm_manager` 切换为 `best_fit_pmm_manager` 后
`make grade` 的 `[Test 2] Best-Fit: PASS`，总分 20/20，初版实现一次通过。

---

## 提示词 3：从零生成 Buddy System 模块（对应功能模块二任务三，第一份 prompt）

**使用场景**：实验不提供 Buddy System 的任何框架代码，需要新增模块并接入内核。

```text
[PROMPT]
**任务**：参考现有 ucore 物理内存管理框架，从零生成伙伴系统（Buddy System）物理内存分配模块，
新增 kern/mm/buddy_pmm.c 与 kern/mm/buddy_pmm.h，注册 buddy_pmm_manager，
使 kernel/mm 的构建系统能自动编译该文件（放入 kern/mm 即可）。
**操作要求**：
1. 只使用 struct Page 现有字段（ref/flags/property/page_link），不新增全局页数组；
   建议用 property 保存块阶 order，块大小为 2^order 页，用 PG_property 标记空闲块头；
2. 每种 order 维护一条空闲链表（free_area_t 数组），不能只用一条链表；
3. init_memmap 要把 [base, base+n) 按“地址对齐 + 不超过剩余长度”的贪心策略拆成
   若干 2^k 页的块登记，保证每个块头相对物理地址按自身大小对齐；
4. alloc_pages(n) 把 n 向上取整到 2^k，从 k 阶开始向上找第一个非空链表，
   取块后逐级对半分裂，多余的一半挂入低阶空闲链；
5. free_pages 清页状态后按阶挂链，并用“物理地址 XOR 块大小”求伙伴，
   伙伴空闲且同阶时逐级合并，合并后必须把块从低阶链移到高阶链；
6. 合并只能在 init_memmap 登记过的页范围内进行，禁止越界读写页描述符。
**输出要求**：给出完整代码，并说明块头约定、分裂/合并流程、nr_free 与每阶计数的关系，
以及为什么块地址必须按自身大小对齐。

[RELY]
- struct Page 字段与 PG_reserved/PG_property 宏（memlayout.h）
- free_area_t { list_entry_t free_list; unsigned int nr_free; } 与 list.h 接口
- page2pa(page)=(page-pages+nbase)<<12；PGSIZE=4096；PADDR/KADDR 约定
- pmm_manager 接口与 default_pmm.c 的调用语义
- init_memmap 的每页初始状态为 PageReserved（page_init 刚建立描述符时的状态）
- QEMU 物理内存 128MB，页号范围 [nbase, npage)，nbase=0x80000

[GUARANTEE]
- 评分路径仍使用 best_fit_pmm_manager，不改变 make grade 的 Test 1/Test 2 结果；
- 不修改 default_pmm.c、best_fit_pmm.c 的测试函数；编译通过 -Werror；
- 不依赖浮点、不递归，合并循环最多 MAX_ORDER 次。

[SPECIFICATION]
- 常量：MAX_ORDER=10（单块最大 1024 页=4MB）；
- init：清空全部阶的空闲链与计数，nr_free=0；
- init_memmap：断言 n>0 且全 reserved，清 flags/property/ref；记录本次管理范围
  [base, base+n)；循环取当前地址可对齐的最大 order（size≤remaining 且 PA 对齐），
  置块头 property=order、SetPageProperty、挂入 free_list[order]，更新每阶计数与 nr_free；
- alloc_pages：n==0 或 n>nr_free 返回 NULL；order=ceil(log2(n))，order>MAX_ORDER 返回 NULL；
  从 order 向上找非空链表；取头块后逐级分裂，右半块置 property=cur、SetPageProperty、
  挂入 free_list[cur]；最终把整块的 flags/property/ref 清零，nr_free-=2^order；
- free_pages：order=ceil(log2(n))，逐页断言 !PageReserved，清 flags/property/ref；
  块头挂入 free_list[order] 并 nr_free+=2^order；while(order<MAX_ORDER)：
  伙伴 PA=pa^(2^order<<12)，若伙伴在管理范围内、PageProperty 为真且 property==order，
  则把二者从当前阶链表摘下，合并后的低地址块作为新块头，order++ 后挂入高一阶链表；
- nr_free_pages 返回 nr_free；check 校验每条链上块头 property 与阶一致、块地址按阶对齐、
  各阶计数之和等于 nr_free。
```

**AI 关键产出**：`buddy_pmm.c` 实现了 11 条空闲链（order 0～10）、贪心登记、逐级分裂、
基于地址异或的伙伴合并与跨阶链表迁移；后续第二份 prompt 生成的自测全部通过。

---

## 提示词 4：为 Buddy System 设计测试（对应功能模块二任务三，第二份 prompt）

**使用场景**：实验不提供 Buddy 的测试函数，需要自行设计一套可重复、可判定的测试。

```text
[PROMPT]
**任务**：针对上一步生成的 Buddy System 模块设计测试函数 buddy_test()，
检验分配、释放、分裂、合并与边界行为是否正确。
**操作要求**：
1. 测试不得改动当前 pmm_manager，也不得破坏真实物理内存的空闲链表：
   使用一块私有的、页对齐的 struct Page 描述符池（如 1024 个描述符）作为测试对象，
   在测试前把池内描述符全部置为 reserved，再调用 buddy_init/buddy_init_memmap；
2. 每个测试用例用 assert 判定，失败直接触发内核 panic；用例结束必须让池恢复到初始状态；
3. 至少覆盖：
   (a) 初始化后总空闲页数、块头 property、块地址对齐、各阶计数一致；
   (b) 非法请求：n=0、n 超过总空闲页；
   (c) 逐页分配直至耗尽，再请求必须返回 NULL，且所有阶链表为空；
   (d) 全部释放后，空闲页数与“各阶块数”必须恢复到初始快照（验证完全合并）；
   (e) 取走最大块再释放，验证返回的块头与恢复后的结构；
   (f) 先占满所有低阶块，再申请 1 页以强制分裂大块，随后全部释放，验证级联合并；
   (g) 非 2 的幂请求（3 页、5 页）按 2^k 向上取整并正确回收；
   (h) 无空闲页时小请求仍返回 NULL。
**输出要求**：给出测试代码，并逐条说明每个用例验证的不变量、为什么能发现分裂/合并错误。

[RELY]
- buddy 模块接口：buddy_init、buddy_init_memmap(base,n)、buddy_alloc_pages(n)、
  buddy_free_pages(base,n)、buddy_nr_free_pages()
- struct Page：property 表示块阶、PG_property 表示空闲块头、page_link 挂链
- 页描述符池是普通 struct Page 数组，page2pa 对连续描述符仍按 4KB 递增，
  因此可以在私有池上完整复现分裂与合并（不影响真实 pages 数组）
- 测试运行环境为单核、无中断，assert 失败即 panic，输出可被 make qemu 捕获

[GUARANTEE]
- 测试只读写私有池的描述符，不调用真实内存的 alloc/free 路径；
- 每个用例结束后池状态可重建，测试可重复执行；
- 不修改 buddy 分配算法本身，不为了让测试通过而放宽断言。

[SPECIFICATION]
- 快照：记录初始化后每阶块数 init_block_count[order]；
- 恢复判定：total_free()==池页数，且每阶块数与每阶计数均等于快照；
- 用例 (f) 的关键：先按 order 从小到大把每个非空链表的块全部分配掉，
  确认只剩最大阶块，再申请 1 页，必然触发大块分裂；释放顺序与分配相反，
  最终必须恢复快照，否则说明合并没有级联到正确阶数；
- 所有测试输出形如 “[Buddy] Test k ...: PASS”，最后输出 “[Buddy] All tests passed”。
```

**AI 关键产出**：`buddy_test()` 共 8 个用例，全部通过；测试过程中暴露了三个真实 bug
（见实验报告“实现迭代过程”），修正后 `make qemu` 输出 8 条 PASS。

---

## 提示词工程小结

- 对内存管理类任务，[RELY] 里把地址换算公式、页状态宏、调用约定逐条写清，
  AI 就不容易把虚拟地址当物理地址、或把 property 的含义用错。
- [SPECIFICATION] 里写“Post-Condition + 必须保持的不变量”（如
  total(property)==nr_free、合并块要换链表），比只写“实现 Best-Fit/Buddy”更能约束实现细节。
- 测试型 prompt 要明确“不得为了通过测试改算法/断言”，并要求用私有资源隔离测试，
  否则测试会污染真实内存管理器的状态。
