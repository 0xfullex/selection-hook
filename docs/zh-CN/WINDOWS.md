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

对于不在 `INCLUDE_CLIPBOARD_DELAY_READ` 列表中的应用，回退采用以下优先恢复原剪贴板的事务：

1. **保存**当前剪贴板内容
2. **模拟 Ctrl+Insert**（更安全 — 不太可能与应用程序快捷键冲突）
3. **等待**一小段时间，检查剪贴板是否发生变化
4. 如果没有变化，**模拟 Ctrl+C** 作为第二次尝试
5. **等待**剪贴板发生变化
6. **读取**新的剪贴板内容
7. **尝试恢复**原始剪贴板内容

程序会尽力恢复原始剪贴板，包括将原本为空的剪贴板恢复为空。所有能够备份的格式都会保存；不支持或无法读取的格式会被跳过，不会因此拒绝取词。被跳过的格式无法恢复。

只要回退实际注入了任意复制快捷键输入（包括部分注入），就始终尝试恢复原剪贴板，并仍然返回已经读到的文字。因此，注入后用户按 Ctrl+C 写入的内容也可能被恢复操作覆盖，这是优先恢复原剪贴板所接受的取舍。

唯一可以保留用户写入的例外是：在剪贴板事务期间（回退开始之后；此前按下的按键，例如仍在尝试辅助功能 API 时，不算）、任何注入之前观察到用户实际按下 **Ctrl+C**、**Ctrl+X**、**Ctrl+Insert** 或 **Shift+Delete**，整个事务实际注入的复制快捷键输入数为零，并随后确认剪贴板序列号变化。这些组合不能带有额外修饰键；程序注入的快捷键不算用户复制证据。候选序列号必须严格晚于按键时的序列号，以及清空并关闭剪贴板后的序列号，比较会考虑序列号回绕。候选写入必须在打开、读取剪贴板和事务结束时保持稳定。自身持锁读取产生的序列号变化不会使有效的用户复制失效。无法确认稳定证据时，仍会恢复原剪贴板。

一旦识别到上述实际按键，即使序列号尚未变化，本次事务也不会再注入复制快捷键。显式调用 `getCurrentSelection()` 时，会在原有等待预算内继续轮询，并在用户写入到达时返回其文字，即使用户仍按着 Ctrl+C。

自动取词会在三个键盘中断检查点检查 Ctrl 是否按着：Ctrl+Insert 阶段开始前、Ctrl+C 阶段开始前，以及 Ctrl+C 阶段读取前。如果执行到的检查点发现 Ctrl 仍按着，就会中止取词，不触发 `text-selection` 事件，并尝试恢复原剪贴板。如果执行到的检查点没有按着 Ctrl，则事务期间、任何注入之前的实际复制或剪切遵循与显式调用相同的规则：停止后续注入、在原有预算内继续轮询、返回用户文字，并保留已确认的写入。这包括 Shift+Delete，以及 Ctrl 已松开的 Ctrl+C。只要实际注入了复制快捷键输入，仍须按上述规则尝试恢复。

普通按键、鼠标点击、滚轮和右键菜单复制都不算用户复制证据。特别是，事务期间通过右键菜单复制的新内容可能被恢复操作覆盖。如果无法可靠地观察钩子输入，仍以恢复原剪贴板为默认行为。

本事务每次打开剪贴板最多尝试五次，失败后的重试间隔为 5 ms。某个已备份格式恢复失败时，会继续恢复其余格式；恢复失败不会丢弃已经读到的文字。事务期间调用 `stop()` 或发生清理时，仍会尝试恢复，除非此前已确认用户复制了新内容。Windows API 持续失败时，剪贴板可能未恢复或仅部分恢复，无法保证完整恢复。

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

列表中的应用继续使用原有剪贴板路径，行为保持不变。上述优先恢复原剪贴板的行为，包括空剪贴板恢复、实际复制或剪切按键检测以及有上限的打开重试，均不适用于这些应用。

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
