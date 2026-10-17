# LMvtdriver 架构设计文档

## 1. 项目概述

LMvtdriver 是一个基于 AMD SVM（硬件虚拟化）的 Windows 内核级调试器，支持交互式调试和透明监控两种工作模式。

### 1.1 核心特性

- **硬件虚拟化基础**：利用 AMD SVM 和 NPT（嵌套页表）实现对 Guest OS 的透明拦截
- **双工作模式**：
  - 交互式调试：暂停线程，支持单步、寄存器修改、内存查看
  - 透明监控：不暂停线程，仅记录事件，目标进程无感知
- **硬件断点驱动**：使用 DR3 寄存器拦截 `KiDispatchException`，无需修改内核代码
- **进程隔离**：基于 CR3 过滤，只拦截目标进程的异常

### 1.2 设计目标

- ✅ 透明性：目标进程无法通过常规手段检测调试器存在
- ✅ 稳定性：支持长时间运行，安全卸载，不影响系统其他进程
- ✅ 高性能：最小化 VMEXIT 开销，支持多核并发
- ✅ 兼容性：支持多个 Windows 版本，与 PatchGuard 共存

---

## 2. 三层架构

```
┌─────────────────────────────────────────────────────────────┐
│                    R3 Debugger (用户态)                      │
│  LMvtdriverDebugger.exe / LMvtdriverGuestControl.exe        │
│  - GUI 调试界面                                              │
│  - 寄存器/内存查看编辑                                        │
│  - 反汇编显示                                                │
│  - 事件处理循环                                              │
└────────────────────────┬────────────────────────────────────┘
                         │ IOCTL
                         ↓
┌─────────────────────────────────────────────────────────────┐
│              Guest Driver (虚拟化 Windows 内核)              │
│  LMvtdriverGuest.sys                                        │
│  - 事件队列管理 (16 slots)                                   │
│  - 异常上下文捕获 (KTRAP_FRAME/KEXCEPTION_FRAME)            │
│  - DR3 会话管理 (CR3 过滤)                                   │
│  - 进程内存读写                                              │
│  - Guest Stub (异常入口桩代码)                               │
└────────────────────────┬────────────────────────────────────┘
                         │ CPUID/共享页
                         ↓
┌─────────────────────────────────────────────────────────────┐
│                 Host Driver (SVM 宿主)                       │
│  LMvtdriver.sys                                             │
│  - SVM 生命周期管理 (启动/停止/卸载)                         │
│  - NPT 管理 (GPA=HPA 映射)                                  │
│  - VMEXIT 处理 (#DB, #DE, NPF, CPUID, VMMCALL)            │
│  - DR3 拦截与 CR3 匹配                                       │
│  - 共享页通信                                                │
└─────────────────────────────────────────────────────────────┘
                         │
                         ↓
┌─────────────────────────────────────────────────────────────┐
│                    Hardware (AMD CPU)                        │
│  - SVM 指令集 (VMRUN, VMEXIT, VMMCALL)                     │
│  - NPT (嵌套页表)                                           │
│  - 硬件调试寄存器 (DR0-DR7)                                 │
└─────────────────────────────────────────────────────────────┘
```

---

## 3. 核心调试机制

### 3.1 基于 DR3 的异常拦截

**关键设计**：牺牲一个硬件调试寄存器（DR3）来拦截 `KiDispatchException` 入口。

#### 工作流程

```
1. 调试器附加进程 (PID)
   ↓
2. Guest 驱动查找 EPROCESS，读取进程的 CR3 (页目录基址)
   ↓
3. Guest 驱动解析 KiDispatchException 地址
   ↓
4. Guest 通过 CPUID 通知 Host：
   - 目标 CR3
   - KiDispatchException VA
   - Guest Stub 地址 (事件桩和直通桩)
   ↓
5. Host 在所有 CPU 上同步配置：
   - VMCB.DR3 = KiDispatchException
   - VMCB.DR7 = 启用 DR3 执行断点
   - 记录目标 CR3 和 Guest Stub 地址
   ↓
6. 目标进程触发异常
   ↓
7. Windows 调用 KiDispatchException
   ↓
8. 触发 #DB 异常 → VMEXIT_EXCEPTION_DB
   ↓
9. Host VMEXIT 处理器：
   - 检查 DR6.B3 (DR3 命中)
   - 读取 Guest CR3
   - CR3 匹配目标进程？
      YES → 重定向到 Guest Event Stub
      NO  → 重定向到 Guest Pass Stub
   ↓
10a. Guest Event Stub (目标进程)：
    - 保存完整 GPRs/Flags/XMM
    - 从 KTRAP_FRAME 读取异常上下文
    - 检查：用户态？First-chance？正确 PID？IRQL=PASSIVE？IF=1？
    - 满足条件 → 入队，等待 R3 处理
    - 不满足 → 直接继续
    - R3 处理后，应用寄存器修改
    - 通过 VMMCALL 退出：
       * Handled 模式：跳过异常，返回到 Ki 调用者
       * Continue 模式：replay Ki 序言，继续原异常分发
   ↓
10b. Guest Pass Stub (其他进程)：
    - 执行 `push rbp` (Ki 序言首条指令)
    - 通过 VMMCALL 返回 Ki+2 继续执行
```

#### 关键数据结构

```cpp
// Host per-CPU state
struct KI_SESSION_STATE {
    UINT64 TargetCr3;              // 目标进程 CR3
    UINT64 KiAddress;              // KiDispatchException 地址
    UINT64 EventStubAddress;       // Guest 事件桩地址
    UINT64 PassStubAddress;        // Guest 直通桩地址
    UINT64 InFlightCount;          // 当前在桩代码中的线程数
    BOOLEAN Enabled;               // 会话是否启用
};

// Guest event queue slot
struct EVENT_SLOT {
    volatile ULONG State;          // FREE/PENDING/CONTINUED
    ULONG Sequence;                // 序列号 (防重放)
    KEVENT CompletionEvent;        // 等待对象
    FILE_OBJECT* OwnerHandle;      // 拥有者句柄
    LMV_GUEST_KI_FRAME_SNAPSHOT Snapshot;  // 寄存器快照
    ULONG ProcessId;
    ULONG ThreadId;
    ULONG ExceptionCode;
    UINT64 ExceptionAddress;
    ULONG Generation;              // 会话代数
};
```

### 3.2 进程过滤：CR3 匹配

**为什么用 CR3？**
- CR3 是进程页目录基址，每个进程独一无二
- 在 VMEXIT 时可以直接从 VMCB 读取，无需额外查找
- 比 PID 更底层、更可靠

**实现细节**：
```cpp
// Guest 读取目标进程 CR3
PEPROCESS Process;
PsLookupProcessByProcessId(TargetPid, &Process);
UINT64 ProcessCr3 = *(UINT64*)((UINT8*)Process + 0x28);  // DirectoryTableBase
ProcessCr3 &= 0x000FFFFFFFFFF000;  // 掩码提取物理地址

// Host VMEXIT 处理
UINT64 GuestCr3 = Vmcb->StateSaveArea.Cr3 & PhysicalAddressMask;
if (GuestCr3 == Session->TargetCr3) {
    // 目标进程，重定向到事件桩
    Vmcb->StateSaveArea.Rip = Session->EventStubAddress;
} else {
    // 其他进程，重定向到直通桩
    Vmcb->StateSaveArea.Rip = Session->PassStubAddress;
}
```

### 3.3 Guest Stub 设计

#### Event Stub (事件桩)
负责捕获目标进程的异常并等待 R3 处理。

**汇编骨架**：
```asm
LmvGuestKiEventStub:
    ; 1. 保存所有寄存器到栈
    push rax
    push rcx
    push rdx
    ; ... 保存所有 GPRs
    sub rsp, 100h      ; XMM 空间
    movaps [rsp], xmm0
    ; ... 保存 XMM0-XMM5
    
    ; 2. 调用 C 处理函数
    mov rcx, rsp       ; 传递寄存器结构体指针
    call LmvGuestKiEventHandler
    
    ; 3. 恢复寄存器
    movaps xmm5, [rsp+50h]
    ; ...
    add rsp, 100h
    pop r15
    ; ...
    pop rax
    
    ; 4. VMMCALL 退出到 Host
    ; Host 根据 disposition 选择返回地址
    mov rax, LMV_VMMCALL_KI_EVENT_EXIT
    vmmcall
    
    ; 不会执行到这里
    int 3
```

**C 处理函数**：
```cpp
VOID LmvGuestKiEventHandler(PRESERVED_CONTEXT* Regs) {
    // 1. 检查条件
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) return;
    if (!InterruptsEnabled()) return;
    if (PreviousMode != UserMode) return;
    
    // 2. 读取 KTRAP_FRAME
    KTRAP_FRAME* TrapFrame = (KTRAP_FRAME*)((UINT8*)Regs->Rcx - offset);
    
    // 3. 查找空闲 slot
    EVENT_SLOT* slot = AcquireFreeSlot();
    if (!slot) return;  // 队列满，直接继续
    
    // 4. 填充 slot
    slot->Snapshot.Rip = TrapFrame->Rip;
    slot->Snapshot.Rsp = TrapFrame->Rsp;
    slot->Snapshot.Rax = TrapFrame->Rax;
    // ...
    slot->ProcessId = PsGetCurrentProcessId();
    slot->ThreadId = PsGetCurrentThreadId();
    slot->State = PENDING;
    
    // 5. 等待 R3 处理（最多 2 秒）
    LARGE_INTEGER timeout;
    timeout.QuadPart = -20000000;  // 2 秒
    KeWaitForSingleObject(&slot->CompletionEvent, 
                          Executive, 
                          KernelMode, 
                          FALSE, 
                          &timeout);
    
    // 6. 应用修改
    if (slot->State == CONTINUED) {
        if (slot->Disposition == HANDLED) {
            TrapFrame->Rip = slot->Snapshot.Rip;  // R3 可能修改了 RIP
            TrapFrame->Rax = slot->Snapshot.Rax;
            // ...
            Regs->Rax = 1;  // 告诉 VMMCALL 使用 Handled 路径
        } else {
            TrapFrame->Rax = slot->Snapshot.Rax;  // 仅应用寄存器修改
            Regs->Rax = 0;  // Continue 路径
        }
    }
    
    // 7. 释放 slot
    ReleaseSlot(slot);
}
```

#### Pass Stub (直通桩)
负责不暂停其他进程的异常。

```asm
LmvGuestKiPassStub:
    ; 执行 Ki 序言首条指令
    push rbp
    
    ; VMMCALL 退出，Host 返回 KiDispatchException+2
    mov rax, LMV_VMMCALL_KI_PASS
    vmmcall
    
    ; 不会执行到这里
    int 3
```

#### Handled 路径的特殊处理

当调试器修复了异常（如修改除数寄存器），需要跳过 Windows 的异常分发：

```asm
; Host 在 VMMCALL 处理中
if (Disposition == HANDLED) {
    // 1. 捕获当前 RAX 和 RFLAGS
    SavedRax = Vmcb->StateSaveArea.Rax;
    SavedRflags = Vmcb->StateSaveArea.Rflags;
    
    // 2. 禁用中断和单步
    Vmcb->StateSaveArea.Rflags &= ~(0x200 | 0x100);  // IF=0, TF=0
    
    // 3. 返回到 Guest 执行 `pop rax`
    Vmcb->StateSaveArea.Rip = EventStubExitAddress;
    VMRUN();
    
    // 4. Guest 执行 pop rax 后再次 VMMCALL
    
    // 5. 恢复 RAX/RFLAGS，返回到 Ki 调用者
    Vmcb->StateSaveArea.Rax = SavedRax;
    Vmcb->StateSaveArea.Rflags = SavedRflags;
    Vmcb->StateSaveArea.Rip = ReturnAddress;  // 从 KTRAP_FRAME 读取
}
```

---

## 4. 辅助功能

### 4.1 内存断点（基于 NPT）

**用途**：监控内存访问（读/写/执行），不占用 DR0-DR2。

```cpp
// 工作原理
1. 调试器设置断点：VA + 类型 (READ/WRITE/EXECUTE)
2. Guest 驱动将 VA 转换为 GPA
3. Guest 通知 Host 修改 NPT 权限：
   - READ 断点：清除 Present 位
   - WRITE 断点：清除 Write 位
   - EXECUTE 断点：设置 NX 位
4. 访问触发 NPF → VMEXIT_NPF
5. Host 检查 GPA 是否在断点列表
6. 是 → 生成调试事件，记录 RIP/寄存器
7. 单次命中后可选择：
   - 保持断点（重复触发）
   - 临时禁用（单步后恢复）
   - 永久删除
```

**数据结构**：
```cpp
#define MAX_MEMORY_BREAKPOINTS 16

struct MEMORY_BREAKPOINT {
    UINT64 GuestPhysicalAddress;
    UINT64 Length;
    ULONG Type;                    // READ/WRITE/EXECUTE
    ULONG State;                   // FREE/ARMED/HIT/DISABLED
    UINT64 HitCount;
    UINT64 LastRip;
    UINT64 OriginalPtePermissions; // 用于恢复
};
```

### 4.2 透明监控模式

**设计目标**：记录异常但不暂停线程，适合生产环境监控。

```cpp
// Host 事件队列
#define HOST_EVENT_QUEUE_SIZE 256

struct HOST_EXCEPTION_EVENT {
    UINT64 Timestamp;
    UINT64 Rip, Rsp, Rflags;
    UINT64 Rax, Rbx, Rcx, Rdx, Rsi, Rdi, Rbp;
    UINT64 R8, R9, R10, R11, R12, R13, R14, R15;
    UINT64 Cr3, Dr6;
    ULONG ExceptionType;          // VMEXIT_EXCEPTION_*
    ULONG ProcessId, ThreadId;
};

// 环形缓冲区
HOST_EXCEPTION_EVENT HostEventQueue[HOST_EVENT_QUEUE_SIZE];
volatile ULONG QueueHead, QueueTail;
```

**工作流程**：
```
1. 调试器设置模式为 TRANSPARENT
2. Host 在 #DB VMEXIT 时：
   - 记录事件到环形队列
   - 立即继续 Guest（不等待 R3）
3. Guest 驱动定期轮询 Host 队列（IOCTL_LMV_GUEST_POLL_HOST_EVENTS）
4. R3 调试器异步读取事件，离线分析
```

---

## 5. 生命周期管理

### 5.1 会话启动

```
1. R3 调试器发送 IOCTL_LMV_GUEST_KI_SESSION (operation=START, pid=xxx)
   ↓
2. Guest 驱动：
   - PsLookupProcessByProcessId (保留 EPROCESS 引用)
   - 读取 CR3 (offset 0x28, 需按 Windows 版本适配)
   - 解析 KiDispatchException 地址 (扫描 ntoskrnl 导出表或特征匹配)
   - 验证 Ki 序言 (必须以 0x40 0x55 开头，即 `push rbp`)
   - 分配 Guest Stub 代码页 (NonPagedPoolExecute 或 MDL)
   ↓
3. Guest 通过 CPUID 同步到 Host：
   - 命令：LmvCpuidCommandSyncKiSession
   - 参数：CR3, KiAddress, EventStubAddress, PassStubAddress
   ↓
4. Host 在所有 CPU 上执行 (IPI)：
   - Vmcb->ControlArea.InterceptException |= (1 << 1);  // #DB
   - Vmcb->ControlArea.InterceptDrRead |= (1 << 3);     // DR3 read
   - Vmcb->ControlArea.InterceptDrWrite |= (1 << 3);    // DR3 write
   - Vmcb->StateSaveArea.Dr7 = 0x00000501;  // L3=1, RW3=00 (exec), LEN3=00
   - Vmcb->StateSaveArea.Dr3 = KiAddress;
   - Session->TargetCr3 = Cr3;
   - Session->Enabled = TRUE;
   ↓
5. Guest 返回成功，R3 开始事件循环
```

### 5.2 会话停止

```
1. R3 调试器发送 IOCTL_LMV_GUEST_KI_SESSION (operation=STOP)
   ↓
2. Guest 驱动：
   - 取消所有待处理事件 (wake 所有等待线程)
   - 通过 CPUID 通知 Host
   ↓
3. Host 在所有 CPU 上执行：
   - Session->Enabled = FALSE;
   - Vmcb->StateSaveArea.Dr7 &= ~0x00000500;  // 禁用 DR3
   - 等待 InFlightCount == 0 (最多 5 秒)
   ↓
4. Guest 驱动：
   - ObDereferenceObject(EPROCESS);
   - 返回成功
```

### 5.3 卸载握手

**关键问题**：必须确保没有线程在 Guest Stub 中才能卸载驱动。

```
1. R3 调试器关闭
   ↓
2. R3 控制工具：LMvtdriverGuestControl.exe --ki-drain
   - 检查 InFlightCount, ArmedProcessors, Enabled 标志
   ↓
3. R3 控制工具：LMvtdriverGuestControl.exe --ki-prepare-unload
   ↓
4. Guest 驱动：
   - 拒绝新的会话启动
   - 再次确认 InFlightCount == 0
   - 通过 CPUID 通知 Host 退役 (LmvCpuidCommandRetireKiGuest)
   ↓
5. Host：
   - 清除 Session 状态
   - 恢复 Host DriverUnload 回调
   ↓
6. Guest 驱动：
   - 恢复 Guest DriverUnload 回调
   - 在 Unload 中发送 CPUID 最终释放信号
   ↓
7. 手动停止 Guest 服务，再停止 Host 服务
```

---

## 6. 关键数据流

### 6.1 异常事件流

```
Target Process             Guest Driver              R3 Debugger
     |                          |                          |
     | raise exception          |                          |
     |------------------------->|                          |
     |   (KiDispatchException)  |                          |
     |                          |                          |
     |    #DB VMEXIT            |                          |
     |=========================>|  (Host redirects RIP)    |
     |                          |                          |
     |  [in Event Stub]         |                          |
     |  save context            |                          |
     |  queue event             |                          |
     |  KeWaitForSingleObject --+----------------------->  |
     |                          |                          |
     |                          |  IOCTL_WAIT_EVENT        |
     |                          |<------------------------ |
     |                          |  return event            |
     |                          |------------------------> |
     |                          |                          |
     |                          |                  [user edits registers]
     |                          |                          |
     |                          |  IOCTL_SET_CONTEXT       |
     |                          |<------------------------ |
     |                          |                          |
     |                          |  IOCTL_CONTINUE          |
     |  KeSetEvent <-----------+------------------------- |
     |                          |                          |
     |  apply modifications     |                          |
     |  VMMCALL exit            |                          |
     |                          |                          |
     |  [resume execution]      |                          |
     |------------------------->|                          |
```

### 6.2 CPUID 通信协议

Guest 和 Host 通过特殊 CPUID 叶交互（避免共享内存的复杂性）。

```cpp
// Guest 调用
CPUID_RESULT result;
__cpuid_ex(LMV_CPUID_CALL_LEAF,        // EAX = 0x4C4D0001
           LmvCpuidCommandSyncKiSession,  // ECX = 命令
           result.regs);

// Host VMEXIT_CPUID 处理
if (Rax == LMV_CPUID_CALL_LEAF) {
    ULONG command = Rcx;
    switch (command) {
    case LmvCpuidCommandSyncKiSession:
        // 从共享页读取参数
        Session->TargetCr3 = SharedPage->Command.Argument0;
        Session->KiAddress = SharedPage->Command.Argument1;
        // ...
        // IPI 到所有 CPU 同步配置
        KeIpiGenericCall(SyncKiSessionOnCpu, NULL);
        break;
    }
}
```

---

## 7. 多核同步策略

### 7.1 会话配置同步

使用 IPI（处理器间中断）确保所有 CPU 同时启用/禁用 DR3。

```cpp
ULONG_PTR SyncKiSessionOnCpu(ULONG_PTR Argument) {
    ULONG cpuIndex = KeGetCurrentProcessorNumber();
    VIRTUAL_PROCESSOR_DATA* vp = &g_VpData[cpuIndex];
    
    // 读取共享配置
    vp->Session.TargetCr3 = g_SharedSession.TargetCr3;
    vp->Session.KiAddress = g_SharedSession.KiAddress;
    vp->Session.Enabled = TRUE;
    
    // 修改当前 CPU 的 VMCB
    vp->Vmcb->StateSaveArea.Dr3 = g_SharedSession.KiAddress;
    vp->Vmcb->StateSaveArea.Dr7 = 0x00000501;
    
    return 0;
}

// 调用方
KeIpiGenericCall(SyncKiSessionOnCpu, NULL);
```

### 7.2 事件队列并发控制

使用快速互斥锁保护队列操作，但等待事件本身不持有锁。

```cpp
FAST_MUTEX g_EventQueueMutex;
EVENT_SLOT g_EventSlots[16];

EVENT_SLOT* AcquireFreeSlot() {
    ExAcquireFastMutex(&g_EventQueueMutex);
    
    for (int i = 0; i < 16; i++) {
        if (g_EventSlots[i].State == FREE) {
            g_EventSlots[i].State = PENDING;
            g_EventSlots[i].Sequence = ++g_GlobalSequence;
            ExReleaseFastMutex(&g_EventQueueMutex);
            return &g_EventSlots[i];
        }
    }
    
    ExReleaseFastMutex(&g_EventQueueMutex);
    return NULL;  // 队列满
}

// 等待时不持有锁
KeWaitForSingleObject(&slot->CompletionEvent, ...);
```

---

## 8. 兼容性设计

### 8.1 Windows 版本适配

关键偏移需要运行时检测：

```cpp
struct WINDOWS_OFFSETS {
    ULONG EprocessDirectoryTableBase;  // 当前 0x28
    ULONG EprocessPid;                 // 当前 0x440
    ULONG KthreadProcess;              // 当前 0x220
    ULONG TrapFrameSize;               // 当前 0x190
};

// 运行时检测
NTSTATUS DetectWindowsOffsets(WINDOWS_OFFSETS* offsets) {
    RTL_OSVERSIONINFOW version;
    RtlGetVersion(&version);
    
    if (version.dwBuildNumber == 19041 ||  // Win10 2004
        version.dwBuildNumber == 19042 ||  // Win10 20H2
        version.dwBuildNumber == 19043 ||  // Win10 21H1
        version.dwBuildNumber == 19044 ||  // Win10 21H2
        version.dwBuildNumber == 19045) {  // Win10 22H2
        offsets->EprocessDirectoryTableBase = 0x28;
        offsets->EprocessPid = 0x440;
        return STATUS_SUCCESS;
    }
    
    // 其他版本：特征匹配或拒绝加载
    return STATUS_NOT_SUPPORTED;
}
```

### 8.2 PatchGuard 共存

- **不修改 IDT/SSDT**：仅使用 SVM 拦截，不 hook 系统表
- **不持久修改内核代码**：Guest Stub 是独立分配的页
- **不修改关键内核数据结构**：只读取 EPROCESS/KTRAP_FRAME，不修改
- **及时恢复 DR 寄存器**：卸载时清除 DR3

---

## 9. 错误处理与诊断

### 9.1 分阶段验证

每个功能都有对应的 smoke test：

```bash
# 基础 DR3 测试
LMvtdriverGuestControl.exe --dr3-smoke
LMvtdriverGuestControl.exe --dr3-all-cpus-smoke

# Ki 拦截测试
LMvtdriverGuestControl.exe --ki-dr3-probe
LMvtdriverGuestControl.exe --ki-dr3-event-smoke
LMvtdriverGuestControl.exe --ki-dr3-all-cpus-event-smoke

# 并发测试
LMvtdriverGuestControl.exe --ki-queue-concurrency-smoke
LMvtdriverGuestControl.exe --ki-queue-cancel-smoke

# 卸载测试
LMvtdriverGuestControl.exe --ki-drain
LMvtdriverGuestControl.exe --ki-prepare-unload
```

### 9.2 失败诊断字段

所有 IOCTL 结构体包含：
- `Status`：NTSTATUS 错误码
- `Step`：失败在哪一步 (1-N)
- `Reason`：具体原因码（自定义枚举）

示例：
```cpp
if (result.Status != 0) {
    printf("FAIL status=%08X step=%u reason=%u\n",
           result.Status, result.Step, result.Reason);
}
```

### 9.3 日志策略

- **Host**：关键 VMEXIT 计数（CPUID, #DB, NPF）
- **Guest**：事件队列状态（pending/continued/timeout）
- **R3**：UI 操作和 IOCTL 耗时

---

## 10. 性能考量

### 10.1 VMEXIT 开销

- **DR3 #DB**：只有目标进程触发异常时才 VMEXIT
- **DR read/write 拦截**：提供影子 DR 状态，Guest 读写不 VMEXIT
- **CPUID**：仅在会话启动/停止时使用，非热路径

### 10.2 事件队列容量

- 16 个槽位支持最多 16 个线程同时暂停
- 队列满时新异常直接继续（不暂停），避免死锁

### 10.3 等待超时

- 事件等待最多 2 秒，防止死锁
- 超时后自动继续异常分发

---

## 11. 安全边界

### 11.1 权限隔离

- **Host 驱动**：只能由 SYSTEM 加载
- **Guest 驱动**：只能由管理员加载
- **R3 调试器**：需要 SeDebugPrivilege

### 11.2 输入验证

- 所有 IOCTL 检查缓冲区大小
- 所有用户提供的地址使用 `ProbeForRead/Write`
- 寄存器修改拒绝内核 RIP/RSP 和特权标志位

### 11.3 资源限制

- 最多 16 个并发事件
- 最多 16 个内存断点
- 单次内存读写限制 64 KB

---

## 12. 未来扩展方向

### 12.1 短期（已有框架）

- ✅ 内存断点实现
- ✅ 异步暂停（F12）
- ✅ 生命周期事件（线程/模块）

### 12.2 中期（需要设计）

- ⏸️ Intel VT-x 支持（EPT 替代 NPT）
- ⏸️ 多进程同时调试
- ⏸️ 远程调试协议（GDB/LLDB）

### 12.3 长期（研究性）

- ⏸️ NPT 执行钩子（完全不占用 DR）
- ⏸️ 时间旅行调试（录制回放）
- ⏸️ 符号服务器集成

---

## 13. 开发规范

### 13.1 代码风格

- Host/Guest 驱动：遵循 WDK 驱动开发规范
- 使用 SAL 注解（`_In_`, `_Out_`, `_Inout_`）
- 所有内存操作检查返回值

### 13.2 测试要求

- 所有新功能必须有 smoke test
- 在 VM 快照中测试，失败后恢复
- 记录测试日志和预期结果

### 13.3 文档维护

- 修改架构时更新本文档
- 新增 IOCTL 时更新 `lmv_control_shared.h`
- 实验性功能标注 `EXPERIMENTAL`

---

## 14. 附录

### 14.1 关键文件清单

```
Host 驱动 (LMvtdriver.sys)
├── svm_core.cpp          # SVM 生命周期和 VMEXIT 处理
├── svm_core.hpp          # VMCB 结构定义
├── svm_entry.asm         # VMRUN 汇编包装
├── npt_manager.cpp       # NPT 页表管理
└── npt_manager.hpp       # NPT 接口

Guest 驱动 (LMvtdriverGuest.sys)
├── guest_driver.cpp      # 驱动入口和 IOCTL 分发
├── guest_control.cpp     # 会话管理和 CPUID 通信
├── ki_replay.cpp         # Ki 序言解析和 Stub 生成
├── guest_memory.cpp      # 内存读写
└── [Stub 汇编文件]       # Event/Pass Stub

R3 调试器 (LMvtdriverDebugger.exe)
├── MainDlg.cpp           # GUI 主窗口
├── debugger.cpp          # 调试引擎
├── driver_comm.cpp       # IOCTL 通信
└── disasm.cpp            # 反汇编（Zydis）

共享头文件
├── lmv_control_shared.h  # IOCTL 定义和数据结构
└── svm_core.hpp          # VMCB 定义（来自 SimpleSvm）

文档
├── ARCHITECTURE.md       # 本文档
├── DESIGN.md             # 早期设计笔记
├── DR3_DEBUG_PATH.md     # DR3 实现路径
├── KI_CR3_SESSION.md     # 持久会话设计
└── NPT_DEBUG_PATH.md     # NPT 实验路径（未完成）
```

### 14.2 术语表

- **SVM**: AMD Secure Virtual Machine，AMD 的硬件虚拟化技术
- **NPT**: Nested Page Tables，嵌套页表，AMD 的二级地址转换
- **VMCB**: Virtual Machine Control Block，虚拟机控制块
- **VMEXIT**: Guest 退出到 Host 的事件
- **VMRUN**: Host 进入 Guest 的指令
- **VMMCALL**: Guest 主动调用 Host 的指令
- **GPA**: Guest Physical Address，Guest 物理地址
- **HPA**: Host Physical Address，Host 物理地址
- **DR**: Debug Register，调试寄存器（DR0-DR7）
- **CR3**: Control Register 3，页目录基址寄存器
- **KTRAP_FRAME**: Windows 内核保存的异常上下文
- **EPROCESS**: Windows 内核进程对象
- **IPI**: Inter-Processor Interrupt，处理器间中断

---

**文档版本**: 1.0  
**最后更新**: 2026-10-09  
**维护者**: LMvtdriver 开发团队
