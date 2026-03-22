# 使用指南

[English](../GUIDE.md)

**[selection-hook](https://github.com/0xfullex/selection-hook) 的一部分** — 一个用于跨应用监听文本选区的 Node.js 原生模块。

完整的 API 参考请参阅 [API.md](API.md)。平台相关的详细信息请参阅 [Windows](WINDOWS.md) 和 [Linux](LINUX.md)。

---

- [理解选区事件](#理解选区事件) — `method`、`posLevel`、坐标有效性
- [平台设置](#平台设置) — [Windows](#windows)、[macOS](#macos)、[Linux](#linux)
- [Linux: Wayland 决策树与降级处理](#linux-wayland-决策树与降级处理) — 决策树、消费者操作表、环境检测
- [Electron 集成](#electron-集成) — 主进程、TypeScript、坐标、剪贴板、生命周期、Wayland
- [配置](#配置) — `start()` 配置、全局过滤、剪贴板回退
- [被动模式与触发模式](#被动模式与触发模式) — 修饰键触发、快捷键触发
- [最佳实践](#最佳实践)

---

## 理解选区事件

当用户在任何应用程序中选择文本时，selection-hook 会发出一个 `text-selection` 事件，携带一个 [`TextSelectionData`](API.md#textselectiondata) 对象。其中有两个关键字段帮助你理解和使用事件数据：

### 选区方法

`method` 字段告诉你文本是**如何**获取的：

| 常量 | 平台 | 含义 |
|---|---|---|
| `SelectionMethod.UIA` | Windows | UI Automation（现代应用） |
| `SelectionMethod.ACCESSIBLE` | Windows | IAccessible（旧版应用） |
| `SelectionMethod.AXAPI` | macOS | Accessibility API |
| `SelectionMethod.PRIMARY` | Linux | PRIMARY 选区（X11/Wayland） |
| `SelectionMethod.CLIPBOARD` | Windows, macOS | 剪贴板回退（Ctrl+C / Cmd+C） |

在大多数情况下，你不需要针对不同的方法做不同处理 — 无论文本是通过哪种方式获取的，文本内容都是相同的。

### 位置级别

`posLevel` 字段告诉你**哪些坐标字段可用**：

| 级别 | 常量 | 可用坐标 |
|---|---|---|
| 0 | `PositionLevel.NONE` | 无 — 没有位置数据 |
| 1 | `PositionLevel.MOUSE_SINGLE` | `mousePosStart` 和 `mousePosEnd`（相同 — 双击或单点） |
| 2 | `PositionLevel.MOUSE_DUAL` | `mousePosStart` 和 `mousePosEnd`（不同 — 拖拽选择） |
| 3 | `PositionLevel.SEL_FULL` | 所有鼠标位置 + 段落坐标（`startTop`/`startBottom`/`endTop`/`endBottom`） |

使用 `posLevel` 来决定如何相对于选区定位 UI 元素（例如浮动工具栏）：

```javascript
hook.on("text-selection", (data) => {
  let anchorPoint;

  switch (data.posLevel) {
    case SelectionHook.PositionLevel.NONE:
      // 没有可用坐标 — 使用回退方案（如屏幕中心或光标查询）
      break;

    case SelectionHook.PositionLevel.MOUSE_SINGLE:
      // 单点 — 在鼠标位置附近显示 UI
      anchorPoint = { x: data.mousePosEnd.x, y: data.mousePosEnd.y + 16 };
      break;

    case SelectionHook.PositionLevel.MOUSE_DUAL:
      // 拖拽选择 — 在拖拽终点附近显示 UI
      anchorPoint = { x: data.mousePosEnd.x, y: data.mousePosEnd.y + 16 };
      break;

    case SelectionHook.PositionLevel.SEL_FULL:
      // 完整段落坐标 — 在最后一行下方显示 UI
      anchorPoint = { x: data.endBottom.x, y: data.endBottom.y + 4 };
      break;
  }
});
```

### 坐标有效性

在某些平台上，坐标字段可能不可用。在使用坐标进行定位之前，务必检查 `INVALID_COORDINATE`：

```javascript
if (data.mousePosEnd.x !== SelectionHook.INVALID_COORDINATE) {
  // 坐标有效 — 可以使用
  showToolbar(data.mousePosEnd.x, data.mousePosEnd.y);
}
```

> **什么时候坐标无效？**
> - **Linux：** `startTop`/`startBottom`/`endTop`/`endBottom` 始终为 `-99999`（Linux 上不支持文本范围坐标）
> - **Linux Wayland：** `mousePosStart`/`mousePosEnd` 也可能为 `-99999`，具体取决于合成器。参见 [Linux: Wayland 决策树](#linux-wayland-决策树与降级处理)

---

## 平台设置

### Windows

无需特殊设置。Selection-hook 在 Windows 7+ 上开箱即用。

某些使用自定义光标或特殊剪贴板行为的应用程序可能需要通过 [`setFineTunedList()`](API.md#setfinetunedlistlisttype-programlist-boolean) 进行额外配置。关于何时以及如何配置的完整说明，请参阅 [Windows 平台详情](WINDOWS.md)。

**坐标**使用物理坐标（虚拟屏幕坐标）。在 Electron 中，使用 `screen.screenToDipPoint()` 进行转换。参见[坐标处理](#坐标处理)。

### macOS

macOS 需要**辅助功能权限**才能让 selection-hook 响应事件。即使没有权限，hook 也能成功启动，但在授予权限之前不会检测到任何选区或输入事件。

**在 Node.js 中：**

```javascript
const hook = new SelectionHook();

if (!hook.macIsProcessTrusted()) {
  // 返回当前状态，可能会显示系统对话框
  hook.macRequestProcessTrust();
  console.log("请在系统设置中授予辅助功能权限，然后重新启动。");
  process.exit(0);
}

hook.start();
```

**在 Electron 中：**

```javascript
const { systemPreferences } = require("electron");

if (!systemPreferences.isTrustedAccessibilityClient(false)) {
  // 向用户显示提示
  systemPreferences.isTrustedAccessibilityClient(true);
  // 引导用户前往 系统设置 > 隐私与安全性 > 辅助功能
}
```

**Chrome/Electron 辅助功能激活：**

在 macOS 上，Chrome/Chromium 系浏览器和 Electron 应用默认不会暴露其辅助功能树。当 selection-hook 检测到 Accessibility API 无法从聚焦的应用程序获取文本时，它会自动设置 `AXEnhancedUserInterface`（针对 Chrome）和 `AXManualAccessibility`（针对 Electron 应用）以启用 AXAPI 访问。此激活仅对每个应用程序执行一次，且仅在默认 AXAPI 尝试失败时执行。

**副作用：** 启用 `AXEnhancedUserInterface` 会强制 Chrome 构建完整的辅助功能树，这可能会导致 Chrome 出现轻微的性能开销（在某些场景下增加内存使用和降低渲染速度）。这是一个已知的权衡 — 如果不启用它，AXAPI 将完全无法读取 Chrome 中的选定文本。

**其他 macOS 说明：**
- `setFineTunedList()` 在 macOS 上无效
- 坐标使用逻辑坐标 — 无需转换
- `TextSelectionData` 中的 `isFullscreen` 字段仅在 macOS 上可用

### Linux

**X11** 无需特殊设置即可正常工作。所有功能均受支持。

**Wayland** 存在平台级别的限制，需要运行时检测和降级处理。完整指南请参阅下方的[决策树](#linux-wayland-决策树与降级处理)。

完整的平台文档请参阅 [Linux 平台详情](LINUX.md)。

---

## Linux: Wayland 决策树与降级处理

在 Wayland 上，selection-hook 的功能取决于多个运行时条件。使用 `linuxGetEnvInfo()` 检测环境并优雅地处理降级。

### 决策树

```
const info = hook.linuxGetEnvInfo();

1. info.displayProtocol === X11?
   └─→ 一切正常工作。无需特殊处理。

2. info.displayProtocol === WAYLAND:

   2a. info.compositorType === MUTTER (GNOME)?
       └─→ ❌ 不支持选区监听
            Mutter 未实现 data-control 协议。
            → 通知用户：此功能在 GNOME Wayland 上不可用。

   2b. info.hasInputDeviceAccess === false?
       └─→ ⚠️ 降级模式（data-control 防抖）：
            - 文本选区仍然可用，但有约 200ms 延迟
            - 鼠标/键盘事件不可用
            - posLevel 最高为 MOUSE_SINGLE
            → 提示用户：运行 `sudo usermod -aG input $USER` 并重新登录。

   2c. info.hasInputDeviceAccess === true:
       └─→ ✅ 完整功能可用。
            光标位置精度取决于合成器：
            - KWIN, HYPRLAND → 精确的屏幕坐标
            - SWAY, WLROOTS, COSMIC → XWayland 回退（可能冻结）
            → 检查坐标的 INVALID_COORDINATE。

   2d. [仅 Electron] 在 XWayland 下运行？
       └─→ selection-hook 和 Electron 可能使用不同的坐标空间。
            → 参见 Electron 集成部分。
```

### 消费者操作表

| 条件 | 影响 | 应对措施 |
|---|---|---|
| GNOME Wayland | 选区监听不可用 | 通知用户；建议切换到 X11 会话或其他合成器 |
| 无输入设备访问权限 | 无鼠标/键盘事件；选区延迟约 200ms | 提示用户加入 `input` 组 |
| XWayland 回退合成器 | 光标坐标在原生 Wayland 窗口上可能冻结 | 检查 `INVALID_COORDINATE`；在 Electron 中回退到 `screen.getCursorScreenPoint()` |
| `programName` 始终为空 | 使用程序名称的 `setGlobalFilterMode()` 无效 | 在 Wayland 上跳过基于程序名称的过滤 |
| 无文本范围坐标 | `posLevel` 最高为 `MOUSE_DUAL`；`startTop`/`endBottom` 始终为 `-99999` | 调整 UI 定位以适应无段落坐标的情况 |

### 环境检测示例

```javascript
const SelectionHook = require("selection-hook");
const hook = new SelectionHook();

if (process.platform === "linux") {
  const info = hook.linuxGetEnvInfo();

  if (info.displayProtocol === SelectionHook.DisplayProtocol.WAYLAND) {
    // 检查合成器支持
    if (info.compositorType === SelectionHook.CompositorType.MUTTER) {
      console.warn("GNOME Wayland 上不支持选区监听。");
      // 禁用该功能或通知用户
    }

    // 检查输入设备访问权限
    if (!info.hasInputDeviceAccess) {
      console.warn(
        "功能受限：无输入设备访问权限。\n" +
        "运行：sudo usermod -aG input $USER\n" +
        "然后注销并重新登录。"
      );
    }
  }
}
```

---

## Electron 集成

### 仅限主进程

Selection-hook 是原生 Node.js 插件，**必须在 Electron 主进程中运行**。通过 IPC 将事件转发到渲染进程：

```javascript
// 主进程
const SelectionHook = require("selection-hook");
const { ipcMain, BrowserWindow } = require("electron");

const hook = new SelectionHook();

hook.on("text-selection", (data) => {
  const win = BrowserWindow.getFocusedWindow();
  if (win) {
    win.webContents.send("selection:text-selected", data);
  }
});

hook.start();
```

### TypeScript 模块加载

对于 TypeScript 项目，将类型导入与运行时模块分开：

```typescript
import type {
  SelectionHookConstructor,
  SelectionHookInstance,
  TextSelectionData,
} from "selection-hook";

const SelectionHook: SelectionHookConstructor = require("selection-hook");
const hook: SelectionHookInstance = new SelectionHook();
```

### 坐标处理

不同平台的坐标有所不同。在 Electron 中，使用以下模式：

```javascript
const { screen } = require("electron");

hook.on("text-selection", (data) => {
  if (data.endBottom.x === SelectionHook.INVALID_COORDINATE) {
    // 坐标不可用（Linux）— 使用光标位置作为回退
    const cursor = screen.getCursorScreenPoint();
    positionToolbar(cursor.x, cursor.y);
    return;
  }

  // Windows：将物理坐标转换为逻辑坐标
  const point = process.platform === "win32"
    ? screen.screenToDipPoint({ x: data.endBottom.x, y: data.endBottom.y })
    : { x: data.endBottom.x, y: data.endBottom.y };

  positionToolbar(point.x, point.y);
});
```

### Linux 剪贴板变通方案

`writeToClipboard()` 和 `readFromClipboard()` 在 Linux 上返回 `false`/`null`。请改用 Electron 的剪贴板 API：

```javascript
const { clipboard } = require("electron");

function writeClipboard(text) {
  if (process.platform === "linux") {
    clipboard.writeText(text);
    return true;
  }
  return hook.writeToClipboard(text);
}
```

### 生命周期管理

将 hook 的生命周期绑定到 Electron 应用的生命周期：

```javascript
const { app } = require("electron");

app.on("will-quit", () => {
  hook.stop();
  hook.cleanup();
});
```

### Wayland: XWayland 建议

在 Linux Wayland 上，Electron 的 `BrowserWindow.setPosition()` 和 `getBounds()` 在原生 Wayland 下无法正确工作。建议在 XWayland 模式下运行 Electron：

```bash
your-electron-app --ozone-platform=x11
```

> **注意：** `app.commandLine.appendSwitch("ozone-platform", "x11")` **不起作用** — ozone 平台在应用程序 JavaScript 运行之前就已初始化。你必须通过命令行或 `.desktop` 文件设置此标志。

> **Electron 38+：** 默认的 ozone 平台为 `auto`（原生 Wayland）。`ELECTRON_OZONE_PLATFORM_HINT` 在 Electron 39+ 中已被移除。请使用 `--ozone-platform=x11` 命令行标志。

完整详情请参阅 [Linux 平台详情 — Electron 应用提示](LINUX.md#hint-for-electron-applications)。

---

## 配置

### start() 配置与独立方法

你可以通过两种方式配置 selection-hook：

**方式一：** 向 `start()` 传入配置对象：

```javascript
hook.start({
  debug: true,
  enableClipboard: false,
  globalFilterMode: SelectionHook.FilterMode.EXCLUDE_LIST,
  globalFilterList: ["terminal.exe", "cmd.exe"],
});
```

**方式二：** 在 `start()` 之前或之后调用独立方法：

```javascript
hook.disableClipboard();
hook.setGlobalFilterMode(SelectionHook.FilterMode.EXCLUDE_LIST, ["terminal.exe", "cmd.exe"]);
hook.start();
```

两种方式产生相同的结果。配置方法可以在任何时候调用 — `start()` 之前、`start()` 之后，甚至在 `stop()` 之后、下次 `start()` 之前。

### 全局过滤

控制哪些应用程序触发选区事件：

```javascript
// 仅监听特定应用中的选区
hook.setGlobalFilterMode(SelectionHook.FilterMode.INCLUDE_LIST, [
  "chrome.exe", "firefox.exe", "code.exe"
]);

// 监听除终端外的所有应用
hook.setGlobalFilterMode(SelectionHook.FilterMode.EXCLUDE_LIST, [
  "WindowsTerminal.exe", "cmd.exe", "powershell.exe"
]);
```

> **Linux Wayland：** `programName` 在 Wayland 上始终为空，因此基于程序名称的过滤无效。参见 [Linux 平台详情](LINUX.md)。

### 剪贴板回退

剪贴板回退默认启用，在 Windows 和 macOS 上当原生 API 失败时作为最后手段使用。关于剪贴板回退如何工作以及如何为特定应用程序配置的完整详情，请参阅 [Windows 平台详情](WINDOWS.md)。

主要配置方法：
- `enableClipboard()` / `disableClipboard()` — 全局开关剪贴板回退
- `setClipboardMode(mode, list)` — 控制哪些应用使用剪贴板回退
- `setFineTunedList(type, list)` — 处理特定应用的剪贴板边界情况（仅 Windows）

> **Linux：** Linux 上未实现剪贴板回退。`writeToClipboard()` 返回 `false`，`readFromClipboard()` 返回 `null`。宿主应用应使用自己的剪贴板 API（例如 Electron 的 `clipboard` 模块）。

---

## 被动模式与触发模式

在被动模式下，`text-selection` 事件**不会被发出**。取而代之的是，你可以使用 `getCurrentSelection()` 按需获取选区。这对于用户显式请求当前选区的触发式工作流非常有用。

### 示例：修饰键触发

```javascript
// 启用被动模式 — 不自动发出 text-selection 事件
hook.setSelectionPassiveMode(true);
hook.start();

// 监听修饰键按住
let keyDownTime = 0;

hook.on("key-down", (data) => {
  // 检查 Ctrl 键（Windows vkCode: 162/163）
  if (data.vkCode === 162 || data.vkCode === 163) {
    if (keyDownTime === 0) keyDownTime = Date.now();

    // 按住超过 500ms 后触发
    if (Date.now() - keyDownTime > 500) {
      const selection = hook.getCurrentSelection();
      if (selection) {
        console.log("选中的文本:", selection.text);
      }
      keyDownTime = -1; // 防止重复触发
    }
  }
});

hook.on("key-up", (data) => {
  if (data.vkCode === 162 || data.vkCode === 163) {
    keyDownTime = 0;
  }
});
```

### 示例：快捷键触发

```javascript
hook.setSelectionPassiveMode(true);
hook.start();

// 外部快捷键触发此函数
function onShortcutPressed() {
  const selection = hook.getCurrentSelection();
  if (selection) {
    processSelection(selection);
  }
}
```

---

## 最佳实践

- **退出前务必调用 `cleanup()`。** 这会释放原生资源并停止事件监听。在 Electron 中，应在 `will-quit` 事件中调用。

- **处理 `error` 事件。** 一般性错误仅在设置 `debug: true` 时才会发出。致命错误（启动/关闭失败）始终会发出。

  ```javascript
  hook.on("error", (error) => {
    console.error("SelectionHook 错误:", error.message);
  });
  ```

- **除非必要，否则避免使用 `enableMouseMoveEvent()`。** 鼠标移动事件触发频率很高，会导致显著的 CPU 占用。仅在你确实需要光标跟踪时才启用。

- **使用跨平台坐标模式。** 始终先检查 `INVALID_COORDINATE`，然后按平台转换：

  ```javascript
  function getLogicalPoint(point) {
    if (point.x === SelectionHook.INVALID_COORDINATE) return null;
    if (process.platform === "win32") {
      // Electron：将物理坐标转换为逻辑坐标
      return screen.screenToDipPoint(point);
    }
    return point; // macOS 和 Linux：已经是逻辑坐标/屏幕坐标
  }
  ```
