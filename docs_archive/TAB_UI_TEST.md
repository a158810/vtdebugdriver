# Tab 控件双界面测试指南

## ✅ 实现完成

已成功实现 Tab 控件双界面切换功能。调试器现在有两个独立的界面：

### Tab 1: Interactive Mode（交互调试模式）
- **执行控制**: Attach, Detach, Stop, Break, Run, Go, Step In, Step Over
- **反汇编窗口**: 显示代码反汇编
- **寄存器窗口**: 显示和修改 CPU 寄存器
- **断点管理**: 设置、删除、启用/禁用断点
- **内存查看**: 读取内存内容
- **输出日志**: 调试信息输出

### Tab 2: Transparent Mode（透明监控模式）
- **执行控制**: Attach, Detach, Export, Clear（简化版）
- **模式选择**: 下拉框选择调试模式
- **事件列表**: 显示捕获的所有异常事件（5列：#/Timestamp/RIP/Exception/RSP）
- **统计信息**: Total Events, Dropped, Errors, Event Rate
- **事件详情**: 选中事件后显示完整寄存器状态
- **输出日志**: 透明模式专用日志

---

## 🧪 测试步骤

### 1. 启动调试器
```bash
x64/Debug/LMvtdriverDebugger.exe
```

### 2. Tab 切换测试
- **默认显示**: 应该显示 "Interactive Mode" Tab
- **点击 "Transparent Mode" Tab**: 
  - 交互模式控件应该全部隐藏
  - 透明模式控件应该全部显示
- **点击 "Interactive Mode" Tab**:
  - 透明模式控件应该全部隐藏
  - 交互模式控件应该全部显示

### 3. 功能测试

#### Interactive Mode
1. 输入 PID，点击 Attach
2. 验证反汇编、寄存器、断点功能正常

#### Transparent Mode
1. 切换到 Transparent Mode Tab
2. 从下拉框选择"透明模式"
3. 输入 PID，点击 Attach（使用 IDC_BTN_ATTACH_T）
4. 观察事件列表是否实时更新
5. 点击事件行，查看事件详情窗口
6. 观察统计信息是否更新
7. 点击 Clear 按钮清空事件列表

---

## 📋 已实现的功能

### 资源文件修改 (lmdbgr3.rc)
- ✅ 添加了 Tab 控件 (IDC_TAB_CONTROL)
- ✅ 重新布局所有控件，分为两组
- ✅ 添加了透明模式专用控件：
  - IDC_BTN_ATTACH_T / IDC_BTN_DETACH_T
  - IDC_GRP_STATISTICS + 4 个统计标签
  - IDC_GRP_EVENT_DETAILS + IDC_EVENT_DETAILS_EDIT
  - IDC_GRP_OUTPUT_T + IDC_OUTPUT_EDIT_T
  - IDC_BTN_EXPORT_EVENTS

### 代码实现 (MainDlg.h/cpp)
- ✅ 添加了 Tab 控件成员变量 `m_tabControl`
- ✅ 添加了透明模式专用控件成员变量
- ✅ 实现了 `OnTabSelChange()` 响应 Tab 切换
- ✅ 实现了 `ShowTab(int tabIndex)` 显示/隐藏控件
- ✅ 在 `OnInitDialog()` 中初始化 Tab 控件并设置默认 Tab

---

## 🎯 界面布局

### 顶部全局区域（始终可见）
```
[Open] [Ping] Driver: -  Status: Idle  PID: [____] Path: [__________] [Browse]
```

### Tab 控件区域
```
[Interactive Mode] [Transparent Mode] ← 点击切换
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
│ 当前激活的 Tab 内容                       │
│                                           │
└───────────────────────────────────────────┘
```

---

## 🔍 已知限制

1. **透明模式 Attach/Detach 按钮**: 
   - 当前 `IDC_BTN_ATTACH_T` 和 `IDC_BTN_DETACH_T` 尚未绑定事件处理函数
   - 建议它们调用相同的 `OnBtnAttach()` / `OnBtnDetach()`

2. **统计信息更新**:
   - 统计标签已添加但尚未实现定时更新逻辑
   - 需要在 `OnTimer` 中添加统计信息刷新

3. **事件详情窗口**:
   - 当前 `DisplayTransparentEvent()` 仍然输出到交互模式的 `m_regsEdit`
   - 应该改为输出到 `m_eventDetailsEdit`

---

## 📦 编译输出

- **文件**: `x64/Debug/LMvtdriverDebugger.exe`
- **状态**: 编译成功，0 错误，5 个警告（C4819 编码警告，可忽略）

---

## 🚀 后续优化建议

1. **绑定透明模式 Attach/Detach 按钮**
2. **实现统计信息实时更新**
3. **修正事件详情输出目标**
4. **添加导出功能 (IDC_BTN_EXPORT_EVENTS)**
5. **优化 Tab 切换时的界面刷新逻辑**
