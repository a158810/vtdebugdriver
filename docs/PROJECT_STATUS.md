# LMvtdriver - 实际项目状态文档

**更新日期**: 2026-10-16  
**基于**: 实际代码分析

---

## 项目概述

**LMvtdriver** 是一个基于 AMD SVM 硬件虚拟化的 Windows 内核级调试器,支持透明监控和交互式调试两种工作模式。

### 核心架构

```
物理机 (AMD CPU with SVM)
  └─ Host Driver (LMvtdriver.sys) - Hypervisor 层
      └─ Virtual Machine (Windows Guest)
          ├─ Guest Driver (LMvtdriverGuest.sys) - 在 VM 内核中运行
          └─ R3 Debugger (LMvtdriverDebugger.exe) - 在 VM 用户态运行
```

这是一个 **Type-1 Hypervisor + 内核调试器** 的组合实现。

---

## 核心机制

### 1. DR3 硬件断点拦截

- Host 使用 DR3 寄存器拦截 `KiDispatchException` 入口
- 触发 #DB 异常时产生 VMEXIT
- Host 根据 CR3 判断是否为目标进程
- 是 → 重定向到 Event Stub (捕获异常)
- 否 → 重定向到 Pass Stub (直接继续)

### 2. 进程过滤: CR3 匹配

**位置**: `svm_core.cpp:2047-2100` (VMEXIT_EXCEPTION_DB 处理)

```cpp
UINT64 GuestCr3 = Vmcb->StateSaveArea.Cr3 & PhysicalAddressMask;
if (GuestCr3 == shared->TargetCr3) {
    // 目标进程 → Event Stub
} else {
    // 其他进程 → Pass Stub
}
```

### 3. 双工作模式架构

**定义**: `lmv_control_shared.h:19-20`
```cpp
#define LMV_DEBUG_MODE_INTERACTIVE 0u  // 交互模式: 暂停线程
#define LMV_DEBUG_MODE_TRANSPARENT 1u  // 透明模式: 不暂停,仅记录
```

**模式切换**: 
- Guest: `g_KiSessionMode` (guest_driver.cpp:66)
- Host: `shared->KiSessionMode` (npt_manager.hpp:214)
- 通过 CPUID 同步 (LmvCpuidCommandSyncKiSession)

---

## 双模式实现细节

### 交互模式 (Interactive)

**流程** (`guest_driver.cpp:2800-2900`):
1. 异常触发 → Host VMEXIT
2. Host 重定向到 Guest Event Stub
3. Guest 捕获完整上下文 (KTRAP_FRAME)
4. 入队到事件槽 (16个槽位)
5. `KeWaitForSingleObject` 等待 R3 处理
6. R3 调试器处理 (修改寄存器/RIP)
7. `KeSetEvent` 唤醒线程
8. 应用修改后继续执行

**事件队列**: Guest 端管理,支持最多 16 个线程同时暂停

### 透明模式 (Transparent)

**实现位置**:
- Host 事件记录: `svm_core.cpp:2151-2295`
- Host 事件队列: `npt_manager.hpp:221` (256 槽环形缓冲)
- Guest 轮询接口: `guest_driver.cpp:3553-3620` (IOCTL_LMV_GUEST_POLL_HOST_EVENTS)
- R3 定时器轮询: `MainDlg.cpp:838-850` (500ms 定时器)

**流程**:
```
1. 异常触发 → Host VMEXIT (#DB)
2. Host 检查模式: shared->KiSessionMode == LMV_DEBUG_MODE_TRANSPARENT
3. Host 读取 TrapFrame (从 R8 寄存器)
4. Host 记录到 HostEventQueue[tail] (原子操作)
5. Host 重定向到 Pass Stub
6. 线程继续运行,完全无感知

(R3 端 500ms 定时器触发)
7. R3 调用 PollHostEvents()
8. Guest IOCTL → CPUID (LmvCpuidCommandReadHostEvent)
9. Host 从 HostEventQueue[head] 复制到 SharedPage
10. Guest 返回事件给 R3
11. R3 显示在事件列表 (ListView)
```

**关键代码位置**:
- TrapFrame 读取: `svm_core.cpp:2236-2295`
- 事件入队: `svm_core.cpp:2037-2076` (LmvCpuidCommandSubmitHostEvent)
- 事件出队: `svm_core.cpp:2082-2108` (LmvCpuidCommandReadHostEvent)

---

## 内存读写机制

**方法**: Host CR3 切换 (完全隐蔽)

**实现**: `svm_core.cpp:465-512` (SvReadWriteVirtualMemory)

```cpp
ULONG64 originalCr3 = __readcr3();
__writecr3(Cr3);  // 切换到目标进程的页表
__try {
    RtlCopyMemory(buffer, (PVOID)virtualAddress, length);
} __except(EXCEPTION_EXECUTE_HANDLER) {
    status = STATUS_ACCESS_VIOLATION;
}
__writecr3(originalCr3);  // 恢复
```

**优势**: 
- 不调用 Windows API (KeStackAttachProcess/MmCopyVirtualMemory)
- 无法被检测
- 直接访问虚拟地址

---

## Shared Page 通信机制

**定义**: `npt_manager.hpp:52-97`

**Host 分配**: `svm_core.cpp` (NptInitialize)
```cpp
g_SharedPage = ExAllocatePoolZero(...);
g_SharedPageGpa = MmGetPhysicalAddress(g_SharedPage).QuadPart;
```

**Guest 映射**: `guest_driver.cpp` (LmvGuestQuerySharedPage)
```cpp
// CPUID 获取物理地址
PHYSICAL_ADDRESS pa = { .QuadPart = pageResult.Magic };
g_SharedPage = MmMapIoSpace(pa, PAGE_SIZE, MmNonCached);
```

**通信协议**: CPUID (LMV_CPUID_CALL_LEAF = 0x4C4D0001)
- Guest 写入参数到 SharedPage
- Guest 调用 CPUID (命令 ID 在 ECX)
- Host 处理请求 (SvHandleCpuid)
- Host 写入响应到 SharedPage
- Guest 读取响应

---

## R3 调试器 UI

**Tab 控件双界面** (`MainDlg.cpp:1297-1389`):

### Tab 1: 交互模式
- 反汇编列表 (IDC_DISASM_LIST)
- 寄存器编辑器 (IDC_REGS_EDIT)
- 内存查看 (IDC_MEMORY_EDIT)
- 断点列表 (IDC_BREAKPOINT_LIST)
- 执行控制按钮 (Run/Step/Break/Go)

### Tab 2: 透明模式
- 事件列表 (IDC_TRANSPARENT_EVENT_LIST) - 5列
  - Time, Exception, RIP, PID, TID
- 事件详情 (IDC_EVENT_DETAILS_EDIT) - 显示完整寄存器
- 统计信息面板:
  - IDC_STAT_TOTAL - 总事件数
  - IDC_STAT_DROPPED - 丢弃事件数
  - IDC_STAT_ERRORS - 错误计数
  - IDC_STAT_RATE - 事件速率
- DR2 硬件断点控制 (Watch/Stop)
- 输出日志 (IDC_OUTPUT_EDIT_T)

**Attach/Detach 按钮**: 
- 交互模式: IDC_BTN_ATTACH, IDC_BTN_DETACH
- 透明模式: IDC_BTN_ATTACH_T, IDC_BTN_DETACH_T
- **绑定状态**: ✅ 已正确绑定到 OnBtnAttach/OnBtnDetach (MainDlg.cpp:120-121)

**事件详情输出**: 
- **输出目标**: ✅ 已正确输出到 m_eventDetailsEdit (MainDlg.cpp:1547)

---

## 当前功能状态

### ✅ 已实现功能

1. **基础架构**
   - Host Driver SVM 生命周期管理
   - NPT 页表管理 (GPA=HPA 映射)
   - VMEXIT 处理 (#DB, #DE, NPF, CPUID, VMMCALL)
   - Guest Driver IOCTL 接口

2. **调试机制**
   - DR3 拦截 KiDispatchException
   - CR3 过滤进程隔离
   - Event Stub / Pass Stub 重定向
   - 交互模式事件队列 (16 槽)
   - 透明模式事件队列 (256 槽)

3. **内存操作**
   - Host CR3 切换读写
   - Shared Page 通信
   - CPUID 协议 (13 个命令)

4. **R3 调试器**
   - Tab 控件双界面
   - 反汇编显示 (Zydis)
   - 寄存器查看/编辑
   - 内存查看
   - 断点管理
   - 透明模式事件列表
   - 事件详情显示

5. **DR2 硬件断点**
   - 监控特定地址的读/写访问
   - 透明模式下捕获访问事件
   - 不占用 DR0-DR2

### 🚧 待完善功能

#### 高优先级

1. **ProcessId/ThreadId 提取** (当前为 0)
   - **位置**: `svm_core.cpp:2278-2279`
   - **需要**: 从 KTHREAD/EPROCESS 读取真实 ID
   - **方法**: 
     ```cpp
     // 从 GS 段读取 _KPCR->Prcb->CurrentThread
     // 从 KTHREAD.Process 获取 EPROCESS
     // 读取 EPROCESS.UniqueProcessId (offset 0x440)
     // 读取 KTHREAD.Cid.UniqueThread
     ```

2. **统计信息面板更新**
   - **当前状态**: 控件已创建,但未更新数据
   - **需要**: 在定时器中读取 Host 的统计数据
   - **位置**: `MainDlg.cpp:838` (OnTimer)
   - **数据源**: 
     - shared->HostEventDropped
     - shared->TransparentModeErrorCount
     - shared->HostEventTail - shared->HostEventHead (计算 Rate)

#### 中优先级

3. **事件过滤功能**
   - 按 RIP 范围过滤
   - 按异常类型过滤
   - 按模块名过滤

4. **事件导出功能**
   - 导出为 CSV
   - 导出为 JSON
   - 便于离线分析

5. **TrapFrame 读取可靠性改进**
   - **当前问题**: 从 R8 读取 TrapFrame 地址,偶尔失败
   - **失败处理**: 回退到 VMEXIT 寄存器,递增 TransparentModeErrorCount
   - **改进方向**: 探索更可靠的 TrapFrame 定位方法

#### 低优先级

6. **热插拔断点支持**
   - DR0/DR1/DR2 动态设置
   - 不需要重启会话

7. **多进程同时调试**
   - 当前只支持单进程

8. **远程调试协议**
   - GDB/LLDB 兼容

---

## 关键数据结构

### HOST_EXCEPTION_EVENT
**定义**: `lmv_control_shared.h:36-68`
**大小**: 328 字节
**用途**: 透明模式事件记录

```cpp
typedef struct _HOST_EXCEPTION_EVENT {
    UINT64 Rip, Rsp, Rflags;
    UINT64 Rax, Rbx, Rcx, Rdx, Rsi, Rdi, Rbp;
    UINT64 R8, R9, R10, R11, R12, R13, R14, R15;
    UINT64 Cr3, Dr6;
    UINT64 Timestamp;              // FILETIME (100ns 单位)
    ULONG ExceptionType;           // VMEXIT_EXCEPTION_*
    ULONG ProcessId;               // ⚠️ 当前为 0
    ULONG ThreadId;                // ⚠️ 当前为 0
    ULONG Reserved;                // HOST_EVENT_KIND_*
    UINT64 ExceptionAddress;
    UINT64 ExceptionInformation[15];
    ULONG NumberParameters;
    ULONG FirstChance;
    ULONG CaptureStatus;
    ULONG ContextValid;            // 1=成功读取 TrapFrame, 0=失败
} HOST_EXCEPTION_EVENT;
```

### SHARED_VIRTUAL_PROCESSOR_DATA
**定义**: `npt_manager.hpp:165-223`
**每 CPU 共享数据**:

```cpp
struct SHARED_VIRTUAL_PROCESSOR_DATA {
    // ... NPT 页表 ...
    
    // 双模式架构
    volatile ULONG KiSessionMode;           // LMV_DEBUG_MODE_*
    volatile ULONG KiSessionState;          // LMV_SESSION_STATE
    volatile ULONG HostEventHead;           // 环形缓冲头指针
    volatile ULONG HostEventTail;           // 环形缓冲尾指针
    volatile ULONG HostEventDropped;        // 队列满时丢弃的事件数
    volatile ULONG TransparentModeErrorCount; // TrapFrame 读取失败次数
    volatile ULONG InteractiveModeErrorCount; // 交互模式错误
    HOST_EXCEPTION_EVENT HostEventQueue[256]; // 透明模式事件队列
    volatile LONG HostEventLock;            // 队列锁
};
```

---

## IOCTL 接口

### Host Driver (LMvtdriver.sys)
- **设备名**: 未直接暴露 IOCTL (通过 Guest Driver 间接通信)

### Guest Driver (LMvtdriverGuest.sys)

**关键 IOCTL**:
- `IOCTL_LMV_SET_DEBUG_MODE` (0x802) - 设置调试模式
- `IOCTL_LMV_GUEST_POLL_HOST_EVENTS` (0x803) - 轮询透明模式事件
- `IOCTL_LMV_GUEST_READ_PROCESS_MEMORY` (0x814) - 读取进程内存
- `IOCTL_LMV_GUEST_WRITE_PROCESS_MEMORY` (0x815) - 写入进程内存
- `IOCTL_LMV_GUEST_KI_SESSION` (0x832) - 启动/停止 KI 会话
- `IOCTL_LMV_GUEST_DR2_WATCH` (0x833) - 设置 DR2 硬件断点

**完整列表**: `lmv_control_shared.h:71-109` (40+ 个 IOCTL)

---

## CPUID 通信命令

**定义**: `lmv_control_shared.h:118-133`

```cpp
typedef enum _LMV_CPUID_COMMAND {
    LmvCpuidCommandQueryStatus = 1,
    LmvCpuidCommandNotifySharedCommand = 2,
    LmvCpuidCommandQuerySharedPage = 3,
    LmvCpuidCommandQueryNptStats = 4,
    LmvCpuidCommandFlushNptTlb = 5,
    LmvCpuidCommandSyncKiSession = 6,       // 同步会话状态和模式
    LmvCpuidCommandReleaseKiGuest = 7,
    LmvCpuidCommandReadVirtualMemory = 8,
    LmvCpuidCommandWriteVirtualMemory = 9,
    LmvCpuidCommandPollHostEvents = 10,     // (已弃用,使用 12)
    LmvCpuidCommandReadHostEvent = 11,      // 读取一个 Host 事件
    LmvCpuidCommandSubmitHostEvent = 12,    // Host 提交事件到队列
    LmvCpuidCommandSyncDr2Watch = 13        // 同步 DR2 断点
} LMV_CPUID_COMMAND;
```

---

## 编译和部署

### 编译命令
```powershell
$MSBUILD = "F:\VS\VSIDE\MSBuild\Current\Bin\MSBuild.exe"

# 编译所有项目
& $MSBUILD LMvtdriver.sln /t:Rebuild /p:Configuration=Debug /p:Platform=x64
```

### 输出文件
- `x64/Debug/LMvtdriver.sys` - Host Driver
- `x64/Debug/LMvtdriverGuest.sys` - Guest Driver
- `x64/Debug/LMvtdriverDebugger.exe` - R3 调试器
- `x64/Debug/LMvtdriverGuestControl.exe` - CLI 控制工具

### 部署步骤 (在虚拟机中)
```bash
# 1. 停止旧驱动
sc stop LMvtdriver
sc stop LMvtdriverGuest

# 2. 复制新驱动
copy x64\Debug\*.sys C:\Drivers\
copy x64\Debug\LMvtdriverDebugger.exe C:\Debugger\

# 3. 启动驱动
sc start LMvtdriver
sc start LMvtdriverGuest

# 4. 运行调试器
C:\Debugger\LMvtdriverDebugger.exe
```

---

## 测试环境

### 宿主机
- **OS**: Windows 10 Pro 22H2 Build 19045
- **CPU**: AMD (支持 SVM)
- **开发工具**: Visual Studio 2022 + WDK

### 虚拟机
- **类型**: AMD SVM 虚拟机 (启用嵌套虚拟化)
- **OS**: Windows 10 Pro
- **用途**: 运行 Guest Driver 和 R3 Debugger

---

## 已知限制

1. **仅支持 AMD CPU**: 依赖 SVM 硬件虚拟化 (不支持 Intel VT-x)
2. **单进程调试**: 一次只能附加一个目标进程
3. **ProcessId/ThreadId 缺失**: 透明模式事件中这两个字段当前为 0
4. **Windows 版本依赖**: EPROCESS/KTHREAD 偏移需要适配不同 Windows 版本
5. **事件队列容量**: 
   - 交互模式: 16 槽 (支持 16 个线程同时暂停)
   - 透明模式: 256 槽 (队列满时丢弃新事件)

---

## 关键代码位置索引

### Host Driver (LMvtdriver.sys)
```
svm_core.cpp
├── 465-512    SvReadWriteVirtualMemory (Host CR3 切换)
├── 642-2160   SvHandleCpuid (CPUID 处理)
├── 1527-1600  LmvCpuidCommandSyncKiSession (模式设置)
├── 2037-2076  LmvCpuidCommandSubmitHostEvent (事件入队)
├── 2082-2108  LmvCpuidCommandReadHostEvent (事件出队)
├── 2151-2295  VMEXIT_EXCEPTION_DB 处理 (TrapFrame 读取)
└── 2800-3100  VMEXIT_NPF 处理 (NPT 页错误)

npt_manager.hpp
├── 36-68      HOST_EXCEPTION_EVENT 结构
├── 52-97      LMV_SHARED_PAGE 结构
├── 165-223    SHARED_VIRTUAL_PROCESSOR_DATA 结构
└── 214        KiSessionMode (模式标志)
```

### Guest Driver (LMvtdriverGuest.sys)
```
guest_driver.cpp
├── 66         g_KiSessionMode (Guest 端模式标志)
├── 2473-2650  KI 会话管理 (启动/停止)
├── 2800-2900  Guest Event Handler (交互模式)
├── 3301-3620  IOCTL 处理
├── 3516-3540  IOCTL_LMV_SET_DEBUG_MODE
└── 3553-3620  IOCTL_LMV_GUEST_POLL_HOST_EVENTS

guest_memory.cpp
└── 426-511    LmvGuestTransfer (内存读写)
```

### R3 Debugger (LMvtdriverDebugger.exe)
```
MainDlg.cpp
├── 120-121    Attach/Detach 按钮绑定
├── 305-347    ShowTab (Tab 切换逻辑)
├── 505        模式选择 (根据当前 Tab)
├── 646-708    OnBtnAttach (附加处理)
├── 838-850    OnTimer (透明模式轮询)
├── 1150-1296  透明事件列表 (Init/Add/Display)
├── 1297-1389  Tab 切换处理
└── 1462-1547  DisplayTransparentEvent (事件详情)

debugger.cpp
├── 43         m_debugMode 初始化
├── 792-850    SetDr2Watch (DR2 断点设置)
└── 1179-1210  SetDebugMode (模式设置)
```

---

## 下一步建议

### 立即可做 (30分钟内)
1. 实现统计信息面板更新 (在 OnTimer 中读取 Host 数据)
2. 测试透明模式完整流程 (Attach → 触发异常 → 查看事件列表)

### 短期目标 (1-2小时)
3. 实现 ProcessId/ThreadId 提取 (从 KTHREAD/EPROCESS 读取)
4. 改进 TrapFrame 读取可靠性 (降低 TransparentModeErrorCount)

### 长期增强 (3+小时)
5. 事件过滤功能
6. 事件导出功能
7. 多进程调试支持

---

**文档完成** - 基于实际代码分析,反映真实实现状态
