# NPT 调试入口：当前边界与验收

目标：Guest Windows 的任意普通数据读取 `KiDispatchException` 时看见原始字节；执行入口时进入 Guest 驱动的调试桩，由 Guest 的线程等待调试器继续。Host 的 VMEXIT 不能等待 R3。

## 当前状态

- `--hook-smoke` 检查 GPA 直接映射至补丁影子页；数据读取也看见补丁，这是旧测试的预期结果，不代表读／执行双视图。
- 新的 `--hook-read-smoke` 在 Guest 内分配独立页，安装旧 Hook 后通过每个在线处理器读取同一个 Guest VA，要求观察到原始字节。当前旧映射预计返回 `FAIL status=c000003e step=5`（数据读取观察到补丁）。测试失败时仍执行原有卸钩、释放区域和全处理器 TLB 刷新；读结果显示为 `observed`。此测试没有执行该页，单独通过也不能证明双视图完成。
- 目前 `LMvtdriverDebugger.exe` 的真实异常仍通过 Windows 调试接口投递，Host `#DE` 仅记录并重注入；Guest Stub 队列仍只处理模拟事件。

## 接下来要实现的执行入口

不要在当前全局 NPT PTE 中永久映射补丁影子页：AMD NPT 的正常可执行映射也允许读取，因此不满足“任意 Guest 线程读到原码”。更适合单个已知入口的候选方案：

1. 将 KiDispatchException 所在的原始 GPA 保持原页映射且标记 NX；任何线程读该 Guest VA 都仍指向原始页。不可直接对生产内核页启用此配置，必须先在独立测试页上验证。
2. Host 收到该 GPA 的取指 NPF 时核验退出位、Guest RIP、Guest CPL、钩子所有权和状态。仅当 RIP 正好指向经 Guest 侧解析确认的入口时，重定向 RIP 到 Guest 驱动单独的可执行桩；此时不映射影子页。Guest 桩沿用 dbgsys 的原指令回放、线程过滤、事件队列与上下文修改逻辑。
3. 同页其他指令也会取指失败，必须在切换到正式内核页前实现可恢复的透传；若采用单指令暂时放行，需每 CPU 独立 NPT 状态、受控 #DB、完整维护原 TF/DR6、跨页与并发访问，以及明确的卸载屏障。不能以“取指一次后全局打开 NX”规避此问题，否则会丢钩子。
4. 在 VM 内验证静态独立页、所有在线 CPU 的读原码、入口触发、非入口透传、异常后可继续、重复卸钩/卸载。任何一项不能稳定通过时，不将这个入口安装到 KiDispatchException。

第一版只处理用户态 first-chance 异常；对非目标进程、非 first-chance 和不允许等待的 IRQL 必须直接继续 Guest 的原异常路径。用户态调试器切换为 Guest 事件后端之前要做新旧事件路径的互斥，不能同时对同一目标进程附加 Win32 调试器。
## 隔离页执行探针（仅验证，非生产钩子）

`LMvtdriverGuestControl.exe --exec-probe-smoke` 在调用进程中分配三个独立页面：原始代码页、相同字节的镜像代码页以及返回不同值的入口桩。Guest 驱动锁定原页，Host 仅将该 GPA 的 NPT PTE 设为 NX（保留原 HPA）；入口取指 NPF 重定向到独立桩，同页其他取指重定向到镜像的相同偏移。验收要求读到原字节、入口走桩、同页非入口运行原指令、卸载后恢复原入口。测试依赖简单的无相对寻址机器码，尚未解决真实内核入口的相对寻址、跨页指令、其他 CR3 别名、并发与退出重入；不要对 `KiDispatchException` 使用该探针。仅在可还原快照的 VM 中执行；如果进程崩溃或卸载失败，先恢复快照，勿继续运行或释放页面。

### 并发与边界验证

`LMvtdriverGuestControl.exe --exec-probe-stress` 在三个独立的用户态可执行区域中验证三轮安装/卸载。每轮装钩和卸钩后分别为每个在线逻辑处理器创建绑定线程，并行重复读取原始字节及执行入口、同页非入口、跨页指令与 RIP 相对寻址指令。测试要求入口执行结果为 `33`（卸钩后 `11`），非入口为 `22`，跨页指令为 `44`。RIP 相对寻址在装钩期间预计读到镜像页中的 `66`，卸钩后读到原页中的 `55`：这是**预期会暴露的语义限制**，需要在虚拟机中确认；不能称为“原代码透明执行”。只有命令返回 `PASS` 才表示隔离页在所测试的 CPU 和三轮生命周期中通过；不能据此部署内核代码 Hook。多核热插拔、取指跨越两个受保护页、生产代码的相对跳转/访存、进程退出并发以及非单页镜像仍未验证。

### RIP-relative 重定位（隔离实验）

`LMvtdriverGuestControl.exe --relocator-smoke` 不依赖驱动，测试使用项目内现有 Zydis 解码的显式 `MOV`/`LEA` RIP-relative 内存操作数。计算原指令目标 VA，按镜像执行位置重写 32 位位移；当目标超出 ±2 GiB、编码不支持、指令不完整或遇到 `CALL`、`JMP`、`RET` 等控制流时拒绝生成，不修改输出。

`LMvtdriverGuestControl.exe --exec-probe-reloc-smoke` 在独立用户态页上只重定位一条已知 RIP-relative `MOV`，然后使用 NX 探针测试所有在线 CPU 的原码读、入口重定向、非入口执行、跨页取指和卸载恢复。预期装钩及卸钩后 `relative={armed=55 restored=55}`，即镜像代码从原始页读取数据；镜像自身的测试数据仍是 `66`。这个实验**不是通用页复制器**：其他偏移的指令不会自动解码或重定位，控制流、数据与指令交错、距离超限、别名、跨页受保护代码和内核入口尚未覆盖。若 VM 内安装或卸载失败，请恢复快照。

## Ki 序言回放桩（合成上下文验证）

`LMvtdriverGuestControl.exe --ki-replay-selftest` 使用当前已解析的 32 字节序言夹具，在用户态生成受控回放桩，验证 10 条指令、`sub rsp, 0x188`、RIP-relative `mov` 重定位和尾部栈恢复。它还会修改一字节并确认桩拒绝执行。`--ki-replay-smoke` 则从 Guest 读取当前真实序言，再使用相同的受控回放逻辑；输出中的 `synthetic=1` 表示这不是在真实 `KiDispatchException` 上下文中调用。该验证不安装 NPT Hook、不修改内核页，也不能证明真实异常处理路径已经接通。

## Ki 回跳桩地址 dry-run（不安装）

`LMvtdriverGuestControl.exe --ki-stub-selftest` 使用固定的 32 字节序言夹具生成 46 字节候选桩，并验证 RIP-relative 目标地址、`KiDispatchException + 0x20` 回跳以及序言变动、超出 ±2 GiB、无效候选地址和截断输入都拒绝生成且不会修改输出。`--ki-stub-dry-run` 从 Guest 驱动查询本次启动的真实 Ki 序言和 VA，**假设**桩位于入口后 1 MiB，以该候选 VA 计算位移，生成字节和报告目标地址。报告中 `candidate=hypothetical`、`allocated=0 installed=0 executed=0` 是关键：尚未保留任何内核地址，也未验证该候选地址可写、可执行或是否被其他模块占用；dry-run 的 PASS 仅说明这个假设地址下的编码可表达，绝不允许直接跳转或写入。当前仍无真实内核桩分配、同步、异常链路或稳定的生产卸载路径。

## 实际 Guest 内核地址探测（仍不安装）

`LMvtdriverGuestControl.exe --ki-stub-sites` 通过新的只读控制请求查询 Guest 驱动镜像内的实际函数地址（已占用的代码地址）和一个实际分配的 `NonPagedPoolNx` 暂存缓冲区地址。两个地址均送进现有 32 字节回放桩生成器，仅报告 RIP-relative 位移能否编码（`image-plan` 与 `nx-plan`：`0` 为编码成功，其余是拒绝原因）；NX 暂存区随 Guest 驱动卸载释放，不存储或执行桩机器码。镜像函数地址属于已经占用的代码，不能拿来写桩；NX 缓冲区虽然可写但不可执行，绝不能当作可用桩。报告固定显示 `eligible=0 installed=0 executed=0`；当前没有同时满足安全写入、执行、距离与生命周期要求的真实桩地址。运行前请同时更新 Guest 驱动和控制程序；不需要更新 Host SVM 驱动。Windows 内存完整性 (HVCI) 禁止可写可执行的内核页，不能为了让探针成功而开启 RWX。

## Guest 镜像静态桩隔离测试

`LMvtdriverGuestControl.exe --static-stub-smoke` 通过 Guest 驱动 IOCTL 在普通驱动调用上下文中运行预编译的镜像内 MASM 静态桩。两次调用分别检查“处理后直接返回”和“未处理则执行静态继续路径”；校验入参、处理次数、返回值和入口／出口的算术标志位及 DF。桩的机器码不可运行时改写，也不执行 NX 缓冲区。本测试不安装 NPT Hook、不触发真实异常、不修改 TrapFrame，不验证真实 Ki 的 17/32 字节回放或线程等待。先在可还原快照的 VM 中更新 Guest 驱动和控制程序后测试；未确认 CPU 上下文与卸载并发安全前禁止连接真实 `KiDispatchException`。

## 内核专用执行页取指探针

`LMvtdriverGuestControl.exe --kernel-exec-smoke` 使用 Guest 驱动镜像内独立 8 KiB 的只读可执行 `LMVPROBE` 节：第一页包含专用原入口及同页非入口，第二页为静态镜像。安装前检查两个 VA 的页对齐、原入口字节及 GPA，调用 Host 现有 NPT NX 探针重定向到预编译静态桩。Guest 读取原入口时仍应看见原字节；处理分支直接返回，未处理分支跳到镜像页执行真实静态原始指令，同页非入口也走镜像；最后撤销 NPT 保护、跨处理器刷新并释放区域。`processors` 只表示 TLB 刷新覆盖的 CPU 数量，不等于每个 CPU 都执行过入口。

必须在可还原快照的 AMD VM 中同时更新 Guest 驱动和控制程序，确认现有 Host SVM 驱动已启动、没有其他活动的调试附加或探针，再运行该命令。若返回 FAIL，特别是 `remaining-stage` 非 0，停止测试并恢复快照；不要卸载驱动或继续复用页面。此测试只作用于专用代码页，不安装 `KiDispatchException` 钩子；仍未验证真实内核序言回放、异常上下文、其他 CR3 别名、多 CPU 并发执行入口及卸载竞态。

## 内核执行页并发压力测试

在 `--kernel-exec-smoke` 通过后，`LMvtdriverGuestControl.exe --kernel-exec-stress` 连续做三轮独立安装、并发执行、撤销和恢复。每轮动态枚举当前在线的全部逻辑处理器，为每个处理器创建一个短命系统线程，在 PASSIVE_LEVEL 固定组亲和性后统一放行；各线程执行 4 次读原字节、处理分支、镜像继续分支和同页非入口，全部线程结束并回收后才撤销 NPT。`executed=expected` 表示每个目标 CPU 确实执行过入口，区别于 `processors` 只统计刷新 TLB 的 CPU。处理器热插拔将导致本轮 FAIL，不能将失败归结为成功。

仍不在执行中的线程上撤销钩子，也没有覆盖任意第三方线程从原页其他偏移取指、内核入口的全局语义及真实 `KiDispatchException`。仅在可还原快照的 VM 中、无其他调试附加和执行探针时测试；每轮必须 `remaining-stage=0`，否则停止测试并恢复快照。压力测试失败时控制程序不启动下一轮。

## Ki 页只读预检查

`LMvtdriverGuestControl.exe --ki-page-preflight` 查询当前 `KiDispatchException` 所在的 4 KiB 页，Guest 侧连续读取两次页内容，返回入口地址、页 GPA/HPA、镜像范围、页哈希和 4 KiB 快照；控制程序使用 Zydis 对快照做线性解码并统计控制流、RIP-relative 引用、跨页指令和解码失败。该命令不会修改代码页、不会设置 NPT NX、不会安装或执行 Ki 桩。

输出 `PASS` 只表示本次页快照稳定、入口序言一致、地址在内核镜像范围内且线性解码没有发现跨页或解码失败；它不是生产 Hook 准入条件。线性扫描可能把数据误当指令，也不能发现所有其他 CR3 别名、真实调用路径、异常重入、并发卸载和同页动态代码。任何 `REVIEW` 都应保持不安装 Ki Hook。

预检查报告修正：旧版输出中的 `instructions=1019 control-flow=281 rip-relative=20 cross-page=0` 是从页首线性解码得到的参考数字；其中 `cross-page=0` **不能**证明页尾不存在跨页指令，因为只有本页快照，解码器无法检查下一页。新版只从已解析 Ki 入口起对前 64 字节顺序解码，输出 `entry-window`；如果入口太靠近页尾或解码失败则返回 `REVIEW`。`page-tail=unknown`、`other-entries=unknown`、`reachable=unproven` 均属于尚未解决的风险，即使显示 `PASS` 也绝不能安装生产 Ki NPT Hook。

## Ki 函数表边界只读检查

`--ki-page-preflight` 现在额外读取 ntoskrnl 加载镜像的 PE 异常目录（`.pdata`），查找覆盖当前 `KiDispatchException` 的函数表记录，报告 `start/end/unwind`、入口是否恰好在记录开头、入口起始 32 字节是否落在该记录之内，以及与 Ki 页相交的其他函数表记录数量。解析时核对镜像大小、目录边界和 RVA，不写代码或 NPT。若没有唯一匹配记录或入口不在函数起点，返回 `REVIEW`。

函数表记录是编译器提供的展开信息，不是所有可执行地址的完备清单；`other-records` 是与目标页重叠的记录数，`other-entries=unproven` 表示未验证这些入口的执行语义。报告 `PASS` 只确认这次静态元数据和 64 字节入口解码一致，不代表真实 Ki Hook 可以安装。

## Ki 页同页函数记录清单

`--ki-page-preflight` 现在会按固定容量返回与目标 4 KiB 页相交的 `.pdata` 函数记录，控制程序逐条打印起止地址、UnwindInfo 地址、页内偏移及 `ki/other` 角色，并检查唯一目标记录、数量一致和未截断。该清单只描述 PE 异常展开元数据；它不等于所有可执行入口，也不能证明同页其他入口在 NPT NX 后能够安全透传。

只有在清单数量稳定、目标记录唯一且没有截断时，才进入下一步的同页代码入口取指模拟；当前仍不安装真实 `KiDispatchException` Hook。

## Ki 六页只读窗口分析

在虚拟机中更新 Guest 驱动和控制程序，并保持 Host SVM 驱动运行，然后执行 `LMvtdriverGuestControl.exe --ki-window-plan`。命令查询 Ki 所在页的前两页、目标页和后三页（偏移 -2 到 +3）；每页双读校验稳定性，按函数起止地址和 UnwindInfo 地址去重 `.pdata` 记录，仅对完整落在窗口内的函数预演 RIP 相对寻址和相对分支重定位。跨越六页边界的函数输出 `extends-beyond-window`，不计入完整分析。窗口扩大后新边界仍可能有函数跨出，不能预设 `boundary=0`。

输出始终为 `REVIEW`：`failures` 表示预演中的解码或位移失败，`boundary` 表示延伸到窗口外的函数。以实时 Guest 字节为预演输入，不以磁盘镜像覆盖；即使两项均为零，`.pdata` 也无法证明所有实际取指入口或并发安全。该命令不安装或执行真实 Ki Hook，旧双页命令不受影响。

窗口分析随后输出 `ki-entry-path`：它从 `KiDispatchException` 入口开始，只沿目标函数内的顺序执行和直接相对分支做静态可达性预演；直接调用到其他函数、窗口外分支、间接控制流和异常边缘不会被伪装成已证明。`indirect-targets=unproven` 与 `exception-edges=unproven` 是预期的保守结果。只有在后续为这些边缘设计安全回原页机制并完成独立运行时测试后，才考虑真实镜像安装。

当 Ki 入口有直接调用指向六页窗口末端且其 `.pdata` 函数延伸至后一页时，命令会按需再双读偏移 `+4` 的单页，并核对函数记录、调用目标以及镜像调用的位移编码。`ki-tail-call` 的 `mirror-return` 是假设整段镜像位于假设地址的静态计算结果，不表示真实镜像地址已分配，也不证明调用返回时的动态执行路径。`ki-tail-plan` 仅对该函数完整范围线性解码和预演相对引用，仍不证明间接跳转、外部调用或异常边缘。新旧日志均应保持 `REVIEW`；不要据此安装 Ki Hook。控制端可运行 `--ki-tail-selftest` 做无需驱动的合成验证，虚拟机实测必须同时更新 Guest 驱动与控制程序。

## 单次安装前门禁

`LMvtdriverGuestControl.exe --ki-install-preflight` 将六页快照、Ki 页独立预检（函数元数据、页内容和序言一致性）、入口可达性及按需的第七页调用审查合并为一次只读检查。命令最后输出 `ki-install-preflight: BLOCKED` 和阻塞位：`0x01` 快照不一致，`0x02` 目标唯一性或重定位失败，`0x04` 跨页尾部未覆盖，`0x08` 其他入口与窗口外路由尚未证实，`0x10` 缺少实际可执行镜像，`0x20` 动态返回路径未经验证。阻塞时退出码为 5，表示不可安装，并非要求反复重试。`candidate` 是计算用假设地址，若 `candidate-overlaps-image=1`，更不能将其视为可用代码页。

`required-mirror-pages` 是当前快照的静态容量下限；可达函数跨入第七页时报告 7。即使容量足够，也不能直接把假设地址当作已分配或已映射的可执行页；实际镜像布局、安全的可执行内存供应与回滚仍需单独实现。

关闭内存完整性的隔离测试虚拟机可运行 `LMvtdriverGuestControl.exe --ki-exec-mirror-smoke`：Guest 驱动分配一页独立的 MDL 页面，写入固定的返回常量探针，通过 `MmProtectMdlSystemAddress` 请求只读可执行保护，调用该页并核对返回值，最后立即解除映射并释放页面。命令打印 `stage`、虚拟地址、Guest 物理地址及验证结果；只在测试虚拟机快照中运行。此测试**不复制或执行 Ki 代码**、不安装 NPT Hook，也不能消除 `--ki-install-preflight` 的路由与动态返回阻塞。

该门禁目前必然保留可执行镜像和运行时返回两项硬阻塞：项目没有提交可供 Ki 镜像使用的真实可执行内核地址，也没有证明外部调用和同页其他入口的安全路由。**不要**因为静态重定位错误为零而安装 Hook。无需现在到虚拟机运行本命令；待解决这些实现问题后再做一次整体测试即可。`--ki-install-preflight-selftest` 只用合成数据验证门禁，不访问 Guest 设备。

预检现在还会让 Guest 驱动在 `NonPagedPoolNx` 中暂存来自运行中 ntoskrnl 的 7 页副本（偏移 -2 到 +4），双读验证每页并报告页对齐 Guest VA、逐页物理地址与内容哈希；控制端和前述快照比对，随后在同一个句柄上释放。句柄关闭和驱动卸载同样会清理暂存区，其他句柄不能释放它。`nx-storage=1` 仅表示暂存副本校验通过；池页不可执行，释放后的 VA/HPA 不得复用，不是实际可执行镜像。未经确定 HVCI 兼容边界前不做 NX→RX，也不触碰 NPT。

`LMvtdriverGuestControl.exe --ki-real-mirror-plan` 独立执行真实地址预演：Guest 分配七页 MDL 镜像，将运行中的七页内核字节复制并双读核对哈希，再将 MDL 映射切换为只读可执行；控制端在持有镜像的期间，用实际返回的 VA 检查六页窗口相对分支和 RIP-relative 操作数的编码范围。命令结束前释放七页镜像；`BLOCKED` 表示实际地址存在无法编码的引用或数据/释放校验失败，`REVIEW` 表示仅六页静态范围预演通过，**不表示七页全部完成重定位**。复制的原始字节不会被执行，不安装 NPT Hook，其他入口、尾页、调用返回和异常路径仍未验证。`--ki-real-mirror-selftest` 使用合成近/远地址验证边界拒绝，不访问设备。

真实地址预演额外报告 `ki-window-relay-plan`：`calls`、`jumps` 和 `conditional` 仅是远距离控制流的跳板候选数量，并非已生成跳板；`rip-data-range` 是无法用控制流跳板修复的 RIP 相对数据寻址，须另行规划数据引用重写或就近镜像。为便于诊断，逐条错误仅输出前 16 项，总数以 `failures` 为准；任何范围失败仍保持 `BLOCKED`，绝不安装 Ki Hook。
