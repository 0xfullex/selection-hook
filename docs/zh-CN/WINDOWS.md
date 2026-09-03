# Windows 平台详情

[English](../WINDOWS.md)

**[selection-hook](https://github.com/0xfullex/selection-hook)** 的一部分 — 一个用于跨应用程序监控文本选择的 Node.js 原生模块。

---

## 文本检测工作原理

Selection-hook 在 Windows 上使用**三级回退策略**来提取选中的文本：

```
用户选择文本
    ↓
┌─────────────────────────┐
│ 1. UI Automation (UIA)  │  现代应用（Chrome、Edge、VS Code 等）
└──────────┬──────────────┘
           ↓ 失败
┌─────────────────────────┐
│ 2. IAccessible          │  旧版应用（较老的 Win32 应用程序）
└──────────┬──────────────┘
           ↓ 失败
┌─────────────────────────┐
│ 3. Clipboard Fallback   │  最后手段 — 模拟 Ctrl+C
└─────────────────────────┘
```

1. **UI Automation (UIA)** — 首选方法。直接从应用程序的 UI 自动化树中读取选中文本。适用于实现了 UIA 文本模式的现代应用程序。当焦点元素本身不持有选区时（例如网页中带 `tabindex` 的容器获得焦点），还会向上检查祖先元素中的选区。
2. **IAccessible** — 针对旧版应用程序，回退到较老的 Windows 辅助功能接口。
3. **Clipboard Fallback（剪贴板回退）** — 最后手段。模拟键盘快捷键将选中的文本复制到剪贴板，然后读取。详见[剪贴板回退](#剪贴板回退)。

[`TextSelectionData`](API.md#textselectiondata) 中的 `method` 字段会告诉你使用了哪种方法：

| 方法常量 | 值 | 含义 |
|---|---|---|
| `SelectionMethod.UIA` | `1` | UI Automation |
| `SelectionMethod.ACCESSIBLE` | `3` | IAccessible |
| `SelectionMethod.CLIPBOARD` | `99` | 剪贴板回退 |

在大多数情况下，你不需要区别处理这些方法 — 无论使用哪种方法，文本内容和坐标都以相同的格式提供。

自动 `text-selection` 事件会在专用的 Windows MTA worker 上执行这套管线，因此 UIA、IAccessible 和剪贴板回退不会阻塞 JavaScript 主线程。普通 Node.js、Electron 以及其他 Windows Node-API 宿主使用同一套实现，不需要 Electron 专用进程或 API。为保持既有事件语义，自动或同步 retrieval 任一执行期间，新的底层 hook 事件仍会被抑制。

---

## 剪贴板回退

剪贴板回退是 Windows 实现中最复杂的部分。它**默认启用**，仅在 UIA 和 IAccessible 都无法提取选中文本时才会激活。

### 工作原理

1. **保存**当前剪贴板内容
2. **模拟 Ctrl+Insert**（更安全 — 不太可能与应用程序快捷键冲突）
3. **等待**一小段时间，检查剪贴板是否发生变化
4. 如果没有变化，**模拟 Ctrl+C** 作为第二次尝试
5. **等待**剪贴板发生变化
6. **读取**新的剪贴板内容
7. **恢复**原始剪贴板内容

此过程设计为非破坏性的 — 用户的剪贴板会被保存和恢复，因此在绝大多数情况下，剪贴板操作对用户是不可见的。

剪贴板事务会串行执行。如果自动回退正在使用剪贴板，公开剪贴板方法会立即返回既有的失败值，而不会等待 worker。

回退还会拒绝由调用线程创建的窗口，因为那条线程需要在阻塞等待剪贴板的同时去派发模拟按键。参见[同进程文本选择](#同进程文本选择)。

### 光标形状检测

在触发剪贴板操作之前，selection-hook 会检查**当前光标形状**，以避免不必要的剪贴板操作：

| 光标形状 | 行为 |
|---|---|
| **I-beam**（文本光标） | 执行剪贴板操作 — 很可能是文本选择 |
| **箭头**或**手形** | 跳过剪贴板操作 — 很可能不是文本选择 |
| **自定义光标** | 取决于 [fine-tuned list](#应用兼容性setfinetunedlist) 配置 |

这个启发式方法对大多数应用程序效果很好。然而，某些应用程序使用**自定义光标**，这些光标不匹配任何标准形状，导致即使用户已选择文本，剪贴板回退也会被跳过。请参阅 [setFineTunedList()](#应用兼容性setfinetunedlist) 了解如何处理这些情况。

### 禁用剪贴板回退

如果你想确保完全不干扰剪贴板：

```javascript
// 方式 1：启动时禁用
hook.start({ enableClipboard: false });

// 方式 2：运行时禁用
hook.disableClipboard();
```

禁用后，仅使用 UIA 和 IAccessible 方法。依赖剪贴板回退的应用程序将无法检测到其文本选择。

---

## 应用兼容性：setFineTunedList()

某些 Windows 应用程序的行为会干扰默认的剪贴板回退。`setFineTunedList()` 提供按应用程序的配置来处理这些边缘情况。

### EXCLUDE_CLIPBOARD_CURSOR_DETECT（type 0）

**问题：** 使用自定义光标的应用程序（例如 Adobe Acrobat 的 PDF 阅读光标、CAJViewer）会导致光标形状检测跳过剪贴板回退，即使用户已选择了文本。

**解决方案：** 将这些应用程序添加到 `EXCLUDE_CLIPBOARD_CURSOR_DETECT` 列表中，以绕过光标形状检查。对于这些应用程序，将始终尝试剪贴板回退。

```javascript
hook.setFineTunedList(
  SelectionHook.FineTunedListType.EXCLUDE_CLIPBOARD_CURSOR_DETECT,
  ["acrobat.exe", "cajviewer.exe"]
);
```

**适用场景：** 某个应用程序中检测不到文本选择，而其他应用程序工作正常。已知该应用程序使用自定义光标。

### INCLUDE_CLIPBOARD_DELAY_READ（type 1）

**问题：** 某些应用程序在 Ctrl+C 操作后会多次修改剪贴板（例如 Adobe Acrobat 先写入纯文本，然后用富文本覆盖）。过早读取剪贴板可能会返回不完整或中间状态的内容。

**解决方案：** 将这些应用程序添加到 `INCLUDE_CLIPBOARD_DELAY_READ` 列表中。这会在读取剪贴板之前增加额外的延迟，让应用程序有时间完成写入。

```javascript
hook.setFineTunedList(
  SelectionHook.FineTunedListType.INCLUDE_CLIPBOARD_DELAY_READ,
  ["acrobat.exe"]
);
```

**适用场景：** 检测到了选中文本，但内容不完整、被截断或格式异常。已知该应用程序会执行多阶段剪贴板写入。

### 组合使用两个列表

像 Adobe Acrobat 这样的应用程序可能需要同时使用两种配置：

```javascript
hook.setFineTunedList(
  SelectionHook.FineTunedListType.EXCLUDE_CLIPBOARD_CURSOR_DETECT,
  ["acrobat.exe", "cajviewer.exe"]
);

hook.setFineTunedList(
  SelectionHook.FineTunedListType.INCLUDE_CLIPBOARD_DELAY_READ,
  ["acrobat.exe"]
);
```

---

## 已知限制

### 提升权限（管理员）窗口

由于 [用户界面特权隔离（UIPI）](https://learn.microsoft.com/en-us/windows/security/application-security/application-control/user-account-control/how-it-works)，非提升权限的进程无法接收来自以管理员权限运行的窗口的低级钩子事件。这意味着与提升权限的应用程序交互时（例如任务管理器，或通过"以管理员身份运行"启动的应用），无法检测到文本选择。

**解决方法：**
- **焦点监控（推荐）：** 在应用层监控全局窗口焦点变化事件，当焦点切换到提升权限的窗口时，关闭划词弹窗。
- **UIAccess：** [UIAccess](https://learn.microsoft.com/en-us/windows/security/application-security/application-control/user-account-control/how-it-works#uiaccess-for-ui-automation-applications) 进程可以跨所有完整性级别接收钩子事件，而无需以管理员身份运行。要求：可执行文件 manifest 中包含 `uiAccess="true"`、受信任的数字签名、安装在安全位置（`Program Files` 或 `Windows\System32`）。

### 同步 `getCurrentSelection()`

为保持 API 兼容，`getCurrentSelection()` 仍为同步方法，并使用调用线程上的 UI Automation client。因此，如果 accessibility provider 的同步 UIA 调用不返回，调用线程仍可能被阻塞。MTA worker 隔离的是自动 retrieval，并不为这个显式同步 API 提供硬取消边界。微软明确警告，client 在 UI 线程上访问[自身 UI](https://learn.microsoft.com/en-us/windows/win32/winauto/uiauto-threading) 可能变得极慢甚至无响应，因此同进程选区的这一风险最高。

### 同进程文本选择

支持在宿主应用自身内部划词，只有剪贴板回退受限。

Windows 将模拟按键投递到[创建目标窗口的那条线程的消息队列](https://learn.microsoft.com/en-us/windows/win32/winmsg/about-messages-and-message-queues)，且只有那条线程的消息循环能派发它。剪贴板回退会先模拟 Ctrl+Insert/Ctrl+C，再在 `Sleep()` 轮询循环中阻塞等待剪贴板变化，因此它不能运行在拥有目标窗口的那条线程上。

- **自动 `text-selection` 事件不受影响。** 它们运行在不拥有任何窗口的 MTA worker 上，主线程仍可正常派发按键。这也符合微软的指引：client 需要自动化自身 UI 时，应[在一条不拥有窗口的独立 MTA 线程上发起 UI Automation 调用](https://learn.microsoft.com/en-us/windows/win32/winauto/uiauto-threading)。如果主线程正忙于一个耗时的 JavaScript 任务，回退仍可能超时，但不会死锁。
- **`getCurrentSelection()` 运行在调用线程上。** 当鼠标下的窗口正是由该线程创建时，剪贴板回退会被拒绝，调用立即返回 `null` 而不会阻塞。UIA 和 IAccessible 仍照常运行，所以只有任何 accessibility provider 都无法提取的选区会受影响。

这一拒绝对使用 `--disable-renderer-accessibility` 启动的 Electron 应用影响最大。该标志是为规避一个 [Chromium accessibility 崩溃](https://issues.chromium.org/issues/40809069)，但它会完全禁用渲染进程的 accessibility tree，导致 UIA 和 IAccessible 都失败，使剪贴板回退成为唯一路径。

**解决方法：** 使用 Electron 的 [`utilityProcess`](https://www.electronjs.org/docs/latest/api/utility-process) 或 `child_process` 在独立进程中运行 selection-hook。这样宿主主线程可以正常处理模拟按键，而 selection-hook 在自己的进程中等待剪贴板变化。
