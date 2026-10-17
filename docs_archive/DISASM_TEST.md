# 反汇编功能测试说明

## 📋 测试目标

验证反汇编窗口能正确显示目标进程的汇编代码，并诊断 "Cannot disassemble" 错误。

---

## 🔧 准备工作

### 1. 编译产物位置
```
E:\VSFILE\LMvtdriver\LMvtdriverDebugger\x64\Debug\LMvtdriverDebugger.exe
```

### 2. 必需的运行环境
- ✅ Host 驱动已加载：`LMvtdriver.sys`
- ✅ Guest 驱动已加载：`LMvtdriverGuest.sys`
- ✅ DebugView 或 DbgView 运行（捕获内核日志）

### 3. 测试目标程序
使用简单的测试程序，比如：
- `notepad.exe`
- `LMvtdriverR3Test.exe`
- 或任何简单的控制台程序

---

## 🧪 测试步骤

### **步骤 1：启动 DebugView**
1. 运行 `DebugView64.exe`（管理员权限）
2. 菜单 → `Capture` → 勾选 `Capture Kernel`
3. 菜单 → `Capture` → 勾选 `Enable Verbose Kernel Output`

### **步骤 2：启动调试器**
1. 在 VM 中运行 `LMvtdriverDebugger.exe`
2. 确认驱动状态显示 "Driver: Online"

### **步骤 3：附加到目标进程**
1. 启动测试程序（如 `notepad.exe`）
2. 在调试器中输入进程 PID
3. 点击 **Attach**
4. 观察调试器输出窗口和 DebugView

**预期日志**：
```
[+] Attached to process 1234
[guest] LmvGuestControlKiSession: starting session for PID 1234
[guest] KiDispatchException found at 0xFFFFF80012345678
[guest] CR3: 0x123456789000
[guest] Session started successfully
```

### **步骤 4：触发异常（让目标暂停）**

**方法 A：设置断点**
1. 在 "Breakpoint Address" 输入一个有效地址（如 ntdll 基址）
2. 点击 "Set Breakpoint"
3. 让目标进程执行到断点

**方法 B：等待自然异常**
1. 让目标程序运行
2. 等待调试事件（如初始断点）

### **步骤 5：测试反汇编窗口**

#### 测试 5.1：使用当前 RIP
1. 确保目标进程已暂停（有调试事件）
2. 汇编地址框留空
3. 点击 **Refresh Disassembly**

**观察点**：
- 输出窗口是否显示诊断日志？
- DebugView 是否有内存读取日志？
- 汇编窗口显示什么？

#### 测试 5.2：手动输入地址
1. 在 "Disassembly Address" 输入已知有效地址
   - 例如：`7FF700000000`（用户模块基址）
   - 或查看寄存器窗口的 RIP 值
2. 点击 **Refresh Disassembly**

**观察点**：
- 输出窗口的诊断日志内容
- 是否显示了字节码？（例如：`[disasm] 0x7FF700001234: 48 83 EC 28 ...`）
- 是否成功反汇编？

---

## 📊 诊断日志解读

### ✅ 正常情况
```
[disasm] 0x7FF700001234: 48 83 EC 28 48 8D 0D 12 34 00 00 E8 AB CD EF 12
[disasm] Successfully disassembled 16 instructions
```

### ❌ 内存读取失败
```
[!] Disassemble: ReadMemory failed at 0x7FF700001234 (error 299)
```
**原因**：
- 地址无效或不可读
- 进程未附加或已退出
- Guest 驱动内存读取 IOCTL 失败

### ❌ 解码失败
```
[disasm] 0x7FF700001234: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
[!] Disassemble: DisasmOne failed at 0x7FF700001234
```
**原因**：
- 读取到的是数据段，不是代码
- 地址错误
- 全零字节无法反汇编

### ❌ 调试器未激活
```
[!] Disassemble: debugger not active
```
**原因**：
- 未附加到进程
- 附加失败

---

## 🔍 常见问题排查

### 问题 1：显示 "Cannot disassemble ... (error XXX)"

**检查清单**：
1. 确认进程已成功附加（输出窗口有 "Attached to process" 消息）
2. 确认地址有效（使用寄存器窗口的 RIP 值）
3. 查看 DebugView 中 Guest 驱动的内存读取日志
4. 检查 error code 含义：
   - `299` (ERROR_PARTIAL_COPY)：内存部分复制
   - `5` (ERROR_ACCESS_DENIED)：访问被拒绝
   - `87` (ERROR_INVALID_PARAMETER)：参数无效

**诊断命令**（在输出窗口观察）：
- 点击 "Refresh Disassembly" 后立即查看诊断日志
- 如果显示 `ReadMemory failed`，说明 Guest 驱动的内存读取有问题

### 问题 2：汇编窗口显示 "Attached. Waiting for a debug event"

**原因**：没有当前上下文（未暂停）

**解决方案**：
1. 手动输入一个有效地址
2. 或等待调试事件（断点、异常）后再刷新

### 问题 3：显示 "Register context unavailable"

**原因**：目标进程正在运行（m_running == true）

**解决方案**：
1. 设置断点让进程暂停
2. 或手动输入地址

### 问题 4：字节码显示但无法解码

**示例**：
```
[disasm] 0x7FF700001234: FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF
[!] Disassemble: DisasmOne failed at 0x7FF700001234
```

**原因**：
- 读取到的不是有效代码
- 地址指向数据段或未初始化内存

**解决方案**：
1. 确认地址是代码段（.text）
2. 使用模块基址 + 偏移

---

## 🎯 预期效果

成功后的汇编窗口应该显示：

```
-> 00007FF700001234  4883EC28              sub rsp, 0x28
   00007FF700001238  488D0D12340000        lea rcx, [rip+0x3412]
   00007FF70000123F  E8ABCDEF12            call 0x7FF713CDEF12
   00007FF700001244  85C0                  test eax, eax
   00007FF700001246  7405                  je 0x7FF70000124D
   ...
```

**特点**：
- `->` 标记当前 RIP 位置
- 当前行自动高亮
- 地址、字节码、助记符、操作数对齐显示

---

## 📝 测试后反馈

请提供以下信息：

1. **输出窗口的诊断日志**（从 `[disasm]` 或 `[!]` 开始的行）
2. **DebugView 中的相关日志**（Guest 驱动的内存操作）
3. **汇编窗口显示的内容**（截图或文字）
4. **使用的测试地址**（手动输入的地址或 RIP 值）

这些信息将帮助定位具体问题。

---

## ✅ 成功标准

- [ ] 调试器能成功附加到进程
- [ ] 寄存器窗口显示正确的 RIP 值
- [ ] 汇编窗口显示至少 10 条指令
- [ ] 当前指令有 `->` 标记并高亮
- [ ] 输出窗口显示 "Successfully disassembled X instructions"
- [ ] 字节码显示正确（与实际内存匹配）
