# 调试器附加蓝屏诊断指南

## 问题现象
点击 attach 按钮后系统立即蓝屏。

## 蓝屏可能原因（按优先级）

### 🔴 原因1：EPROCESS 偏移错误（最高危）

**症状**：立即蓝屏，可能显示 `IRQL_NOT_LESS_OR_EQUAL` 或 `PAGE_FAULT_IN_NONPAGED_AREA`

**原因**：
`guest_layout.hpp` 中硬编码的偏移是为特定 Windows 版本设计的：
```cpp
#define LMV_KPROCESS_DIRECTORY_TABLE_BASE_OFFSET 0x28u  // Win10 22H2 Build 19045.6466
```

如果你的 VM 运行的是不同版本（Win10 21H2、Win11 等），偏移会错误。

**验证步骤**：
1. 在 VM 中检查 Windows 版本：
   ```cmd
   winver
   ver
   ```

2. 使用 WinDbg 连接内核调试器，检查正确的偏移：
   ```
   kd> dt nt!_KPROCESS DirectoryTableBase
   ```
   
   输出示例：
   ```
   +0x028 DirectoryTableBase : Uint8B  ← 如果是 0x28 则正确
   ```

3. 如果偏移不是 `0x28`，修改 `guest_layout.hpp:6` 为正确值。

---

### 🔴 原因2：KiDispatchException 地址查找失败

**症状**：蓝屏前可能有短暂延迟

**原因**：
- `LmvGuestQueryKiDispatchInfo` 无法找到 `KiDispatchException`
- 返回无效地址
- Host 在错误地址设置 DR3 → 访问非法内存

**诊断方法**：
使用 DebugView 查看内核日志（在蓝屏前）：
```
[LMvtdriverGuest] KiDispatchException query failed: status=0xXXXXXXXX
```

**修复**：
- 确保 `ntoskrnl.exe` 没有被修改
- 检查是否启用了 PatchGuard 或其他内核保护

---

### 🔴 原因3：Guest Stub 地址无效

**症状**：蓝屏代码可能是 `KERNEL_MODE_EXCEPTION_NOT_HANDLED`

**原因**：
Host 重定向到的 Guest Stub 地址无效：
```cpp
reinterpret_cast<ULONGLONG>(&LmvGuestKiSessionEventStub)
```

**验证**：
检查汇编文件 `ki_dr3_redirect.asm` 中这些符号是否正确导出：
- `LmvGuestKiSessionEventStub`
- `LmvGuestKiSessionPassStub`
- `LmvGuestKiCaptureExit`
- `LmvGuestKiCommitExit`

---

### 🔴 原因4：DR3 同步时机问题

**症状**：间歇性蓝屏，不是每次都发生

**原因**：
`LmvGuestSyncKiSession` 通过 IPI 在所有 CPU 上配置 DR3，如果某个 CPU 正好在执行 `KiDispatchException`：
- DR3 立即触发 `#DB` VMEXIT
- 但 Host 还没完全准备好
- 重定向到未初始化的地址

**临时规避**：
在附加前确保目标进程是暂停状态，避免异常触发。

---

## 诊断步骤（按顺序执行）

### 步骤1：收集内核日志

1. 在 VM 中下载并运行 [DebugView](https://learn.microsoft.com/sysinternals/downloads/debugview)
2. 启用 "Capture Kernel" 和 "Enable Verbose Kernel Output"
3. 加载 Host 和 Guest 驱动
4. 尝试附加目标进程
5. **在蓝屏前** 查看日志输出

预期看到的日志序列（正常情况）：
```
[LMvtdriverGuest] KiDispatchException found at 0xFFFFF80012345678
[LMvtdriverGuest] Process lookup succeeded for PID 1234
[LMvtdriverGuest] Read CR3 from offset 0x28: raw=0x... masked=0x...
[LMvtdriverGuest] Registering Guest exits with Host...
[LMvtdriverGuest] Guest exits registered successfully
[LMvtdriverGuest] Starting Ki session: CR3=0x... Ki=0x...
[LMvtdriverGuest] Host session started, syncing DR3 across all CPUs...
[LMvtdriverGuest] Ki session fully active: 4 CPUs synced
```

如果在某一步停止，记录最后一行日志。

---

### 步骤2：验证 EPROCESS 偏移

使用 WinDbg 内核调试（推荐在另一台机器上通过网络连接）：

1. 配置内核调试：
   ```cmd
   bcdedit /debug on
   bcdedit /dbgsettings net hostip:192.168.1.100 port:50000
   ```

2. 重启 VM，在主机上启动 WinDbg 连接

3. 验证偏移：
   ```
   kd> dt nt!_KPROCESS
   kd> dt nt!_EPROCESS
   ```

4. 查找 `DirectoryTableBase` 字段的偏移

---

### 步骤3：测试最小化场景

创建最简单的测试用例：

```cpp
// 测试代码：只测试 PsLookupProcessByProcessId 和读取 CR3
PEPROCESS process;
NTSTATUS status = PsLookupProcessByProcessId(ULongToHandle(4), &process);  // PID 4 = System
if (NT_SUCCESS(status)) {
    PUCHAR p = (PUCHAR)process;
    ULONGLONG cr3 = *(ULONGLONG*)(p + 0x28);
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, 0, "System CR3: 0x%016llX\n", cr3);
    ObDereferenceObject(process);
}
```

如果这段代码蓝屏，说明 **EPROCESS 偏移肯定是错的**。

---

### 步骤4：检查 VM 配置

确认 VM 设置：
- **必须启用 AMD-V / SVM**
- **必须启用嵌套分页 (NPT)**
- CPU 核心数量（日志会显示同步的 CPU 数量）

在 VMware 中：
```
processors.numvcpus = "4"
vhv.enable = "TRUE"
```

---

### 步骤5：单独测试 Host 驱动

在附加前，测试 Host 驱动是否工作正常：

```cpp
// 通过 IOCTL 查询 Host 状态
LMV_QUERY_STATUS status = {};
DeviceIoControl(hDevice, IOCTL_LMV_QUERY_STATUS, ...);
```

确认 SVM 虚拟化已启用。

---

## 常见蓝屏代码对应

| 蓝屏代码 | 可能原因 |
|---------|---------|
| `IRQL_NOT_LESS_OR_EQUAL` | EPROCESS 偏移错误，访问非法内存 |
| `PAGE_FAULT_IN_NONPAGED_AREA` | CR3 值错误，或 KiDispatchException 地址无效 |
| `KERNEL_MODE_EXCEPTION_NOT_HANDLED` | Guest Stub 地址无效，Host 跳转到错误代码 |
| `SYSTEM_SERVICE_EXCEPTION` | DR3 同步时触发异常，但处理程序未就绪 |
| `DRIVER_IRQL_NOT_LESS_OR_EQUAL` | 在错误的 IRQL 级别执行操作 |

---

## 应急措施

如果反复蓝屏导致 VM 无法启动：

1. **进入安全模式**：
   - 启动时按 F8
   - 选择"安全模式"

2. **禁用驱动自动启动**：
   ```cmd
   sc config LMvtdriver start= demand
   sc config LMvtdriverGuest start= demand
   ```

3. **恢复 VM 快照**（推荐）

---

## 下一步

1. 先在 VM 中运行 DebugView 收集日志
2. 尝试附加，记录蓝屏前的最后一行日志
3. 把日志发给我，我会精确定位问题

如果需要修改 EPROCESS 偏移或其他配置，我会帮你修改并重新编译。
