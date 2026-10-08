# LMvtdriver R3 异常测试程序

## 使用前

在可恢复快照的虚拟机里使用。这个 EXE 不加载驱动，不主动调用调试 IOCTL，也不自动卸载驱动。
Release EXE 使用静态 CRT，可单独复制到 Windows x64 虚拟机运行。
本地 --selftest 仅验证除法和 SEH，不证明 Host/Guest 调试或卸载安全。

直接双击 LMvtdriverR3Test.exe，或从 CMD 启动。启动后会显示 PID、主线程 TID 和 div ecx 的实际地址。
附加之前程序不会自动触发异常。输入菜单字母并回车执行，每轮结束保持同一个进程等待下一轮。
所有触发路径都有 SEH 兜底：没有附加或调试器放行时，测试程序仍能继续运行。

## s：单线程修改寄存器

1. 启动测试程序，用调试器附加显示的 PID。
2. 在测试程序输入 s 并回车。
3. 调试器应收到 0xC0000094，TID 等于主线程 TID，RIP 等于打印的 div ecx 地址。
4. 此时 EAX=100（十六进制 0x64）、EDX=0、ECX=0。
5. 将 RCX 改为 2；不改变 RIP，继续原除法指令。
6. 测试程序应输出 [PASS] single repair，value=50、SEH=00000000。
7. 分离后用 Control --ki-drain 确认 inflight=0、armed=0，再次附加同一 PID 重复 10 轮。

若输出 FAIL 且 SEH=c0000094，说明异常进入正常 Windows SEH，没有观察到成功的寄存器修复。

## e：异常等待时分离

1. 附加后输入 e 并回车。
2. 异常停住时不修改寄存器，直接在调试器中分离。
3. 预期正常异常分发进入程序自己的 SEH，输出 [PASS] SEH/detach、SEH=c0000094，并回到菜单。
4. 确认 --ki-drain 为 inflight=0、armed=0。

不附加运行 e 也会 PASS，它只能验证 SEH；必须附加、观察事件停止后再分离，才能验证取消调试事件路径。
不要在未修改除数时选择“已处理并继续”，否则 div ecx 会再次产生除零异常。

## t：双线程并发异常

1. 输入 t 并回车，程序创建两个等待启动事件的线程，并打印它们的 TID。
2. 确保调试器已附加，再按一次回车，同时释放两个线程。
3. 事件到达顺序不固定，两线程共享同一 div ecx 地址。按 TID 和 EAX 区分：
   - Worker A：EAX=100（0x64），把 RCX 改为 2，返回 50。
   - Worker B：EAX=240（0xF0），把 RCX 改为 4，返回 60。
4. 每次核对事件线程、读取/写入对应上下文并继续，最终应有两条 PASS、SEH=0。
5. 主线程等待两个线程结束后返回菜单。若只处理一个事件，程序会继续等待另一个线程，这本身不代表死锁。

“同时释放”不保证硬件在同一时刻触发，也不保证位于不同 CPU。本程序不设置 CPU affinity。
若 GUI 只显示一条事件，应先处理该事件并继续，观察下一条；该模式可揭示队列或 GUI 的并发限制。

## q 与卸载

普通轮次间仅分离，不执行 --ki-prepare-unload。
测试结束先分离、关闭调试器，再输入 q 退出测试程序。
最后运行 Control --ki-prepare-unload。只有 PASS、inflight=0、armed=0、flags=00000004 才正常卸载 Guest，再卸载 Host。
准备卸载后禁止再次附加，需要重新加载两个驱动。
发生蓝屏或卡死时保存 dump/调试日志并恢复快照，不强制卸载。

## 本地自测和构建

LMvtdriverR3Test.exe --selftest

自测包含正常除法 100/2、240/4，单线程除零 SEH、双线程除零 SEH。
自测应在没有附加调试器的情况下运行。

源码工程：LMvtdriverR3Test/LMvtdriverR3Test.vcxproj，已加入 LMvtdriver.sln 的 Debug/Release x64 配置。
汇编函数接收 numerator/divisor，执行 mov eax,ecx; mov ecx,edx; xor edx,edx; div ecx; ret。
