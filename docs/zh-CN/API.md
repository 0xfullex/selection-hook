# API 参考

[English](../API.md)

**[selection-hook](https://github.com/0xfullex/selection-hook)** 的一部分 — 一个用于跨应用程序监听文本选择的 Node.js 原生模块。

> **另请参阅：** [指南](GUIDE.md) · [Windows 平台详情](WINDOWS.md) · [Linux 平台详情](LINUX.md)

---

- [构造函数](#constructor)
- [方法](#methods)
  - [生命周期](#lifecycle) — `start()`、`stop()`、`isRunning()`、`cleanup()`
  - [文本选择](#selection) — `getCurrentSelection()`、`setSelectionPassiveMode()`
  - [鼠标追踪](#mouse-tracking) — `enableMouseMoveEvent()`、`disableMouseMoveEvent()`
  - [剪贴板](#clipboard) — `enableClipboard()`、`disableClipboard()`、`setClipboardMode()`、`writeToClipboard()`、`readFromClipboard()`
  - [过滤](#filtering) — `setGlobalFilterMode()`、`setFineTunedList()`
  - [平台特定](#platform-specific) — `macIsProcessTrusted()`、`macRequestProcessTrust()`、`linuxGetEnvInfo()`
- [事件](#events) — `text-selection`、`mouse-move`、`mouse-up`、`mouse-down`、`mouse-wheel`、`key-down`、`key-up`、`status`、`error`
- [类型](#types) — `SelectionConfig`、`TextSelectionData`、`MouseEventData`、`MouseWheelEventData`、`KeyboardEventData`、`LinuxEnvInfo`、`Point`
- [常量](#constants) — `INVALID_COORDINATE`、`SelectionMethod`、`PositionLevel`、`FilterMode`、`FineTunedListType`、`DisplayProtocol`、`CompositorType`
- [TypeScript 支持](#typescript-support)

---

## 构造函数

```javascript
const hook = new SelectionHook();
```

创建一个新的 SelectionHook 实例并初始化原生模块。原生实例会在构造函数中立即创建，因此查询方法（例如 `linuxGetEnvInfo()`、`macIsProcessTrusted()`）和配置方法（例如 `enableClipboard()`、`setGlobalFilterMode()`）可以在 `start()` 之前调用。

---

## 方法

> 除非另有说明，所有配置和查询方法都可以在 `start()` 之前调用。

### 生命周期

#### `start(config?): boolean`

开始监听文本选择。

配置方法可以在 `start()` 之前调用以预配置 hook。如果 `start()` 传入了 config 对象，只有与默认值不同的配置值才会被应用 — 等于默认值的配置会被跳过（这些字段的预启动设置将被保留）。

| 参数 | 类型 | 必填 | 默认值 | 描述 |
|------|------|------|--------|------|
| `config` | [`SelectionConfig`](#selectionconfig) | 否 | — | 配置选项。所有可用字段和默认值请参见 [`SelectionConfig`](#selectionconfig)。 |

**返回值：** `boolean` — 启动成功返回 `true`。

```javascript
hook.start({
  debug: true,
  enableClipboard: false,
  globalFilterMode: SelectionHook.FilterMode.EXCLUDE_LIST,
  globalFilterList: ["WindowsTerminal.exe", "cmd.exe"],
});
```

过滤模式详情请参见 [`SelectionHook.FilterMode`](#selectionhookfiltermode)。

> **Linux：** `enableClipboard`、`clipboardMode` 和 `clipboardFilterList` 在 Linux 上无效（未实现剪贴板回退）。在 Wayland 上，`globalFilterMode`/`globalFilterList` 无效，因为 `programName` 始终为空。完整详情请参见 [Linux 平台详情](LINUX.md)。

> **macOS：** macOS 需要辅助功能权限才能使 selection-hook 正常工作。请确保用户在调用 `start()` 之前已启用辅助功能权限。
> - **Node**：使用 `selection-hook` 的 `macIsProcessTrusted()` 和 `macRequestProcessTrust()` 来检查和请求权限。
> - **Electron**：使用 Electron 的 `systemPreferences.isTrustedAccessibilityClient()` 来检查权限。

#### `stop(): boolean`

停止监听文本选择。

**返回值：** `boolean` — 停止成功返回 `true`。

#### `isRunning(): boolean`

检查 selection-hook 是否正在运行。

**返回值：** `boolean` — 如果正在监听则返回 `true`。

#### `cleanup(): void`

释放资源并停止监听。应在应用程序退出前调用。

---

### 文本选择

#### `getCurrentSelection(): TextSelectionData | null`

获取当前的文本选择（如果存在）。

**返回值：** [`TextSelectionData`](#textselectiondata) `| null` — 当前的选择数据，如果不存在选择或 hook 未运行则返回 `null`。

#### `setSelectionPassiveMode(passive): boolean`

设置选择的被动模式。在被动模式下，不会发出 `text-selection` 事件 — 选择只能通过 `getCurrentSelection()` 获取。

| 参数 | 类型 | 必填 | 默认值 | 描述 |
|------|------|------|--------|------|
| `passive` | `boolean` | 是 | — | `true` 启用被动模式，`false` 禁用。 |

**返回值：** `boolean` — 设置成功返回 `true`。

```javascript
// 启用被动模式 — 按需获取选择
hook.setSelectionPassiveMode(true);

// 之后，当通过快捷键或按键触发时：
const selection = hook.getCurrentSelection();
if (selection) {
  console.log("选中的文本:", selection.text);
}
```

---

### 鼠标追踪

#### `enableMouseMoveEvent(): boolean`

启用鼠标移动事件。由于频繁触发事件会导致高 CPU 使用率。默认禁用。

**返回值：** `boolean` — 启用成功返回 `true`。

#### `disableMouseMoveEvent(): boolean`

禁用鼠标移动事件。这是默认状态。

**返回值：** `boolean` — 禁用成功返回 `true`。

---

### 剪贴板

> **Linux：** Linux 使用 PRIMARY 选择而非剪贴板回退。`enableClipboard()`、`disableClipboard()` 和 `setClipboardMode()` 无效。`writeToClipboard()` 返回 `false`，`readFromClipboard()` 返回 `null`。宿主应用程序应使用自己的剪贴板 API（例如 Electron clipboard）。

#### `enableClipboard(): boolean`

启用文本选择的剪贴板回退。默认启用。

**返回值：** `boolean` — 启用成功返回 `true`。

#### `disableClipboard(): boolean`

禁用文本选择的剪贴板回退。剪贴板默认启用。

**返回值：** `boolean` — 禁用成功返回 `true`。

#### `setClipboardMode(mode, programList?): boolean`

配置剪贴板回退在不同程序中的工作方式。过滤模式常量详情请参见 [`SelectionHook.FilterMode`](#selectionhookfiltermode)。

| 参数 | 类型 | 必填 | 默认值 | 描述 |
|------|------|------|--------|------|
| `mode` | [`FilterMode`](#selectionhookfiltermode) | 是 | — | 剪贴板过滤模式。 |
| `programList` | `string[]` | 否 | `[]` | 要包含或排除的程序名称。 |

**返回值：** `boolean` — 设置成功返回 `true`。

```javascript
// 仅对需要剪贴板回退的特定应用使用
hook.setClipboardMode(SelectionHook.FilterMode.INCLUDE_LIST, [
  "acrobat.exe", "wps.exe"
]);

// 在 Ctrl+C 有特殊行为的应用中阻止剪贴板回退
hook.setClipboardMode(SelectionHook.FilterMode.EXCLUDE_LIST, [
  "code.exe", "devenv.exe"
]);
```

> 关于何时以及为何需要配置此项，请参见 [Windows 平台详情 — 剪贴板回退](WINDOWS.md#clipboard-fallback)。

#### `writeToClipboard(text): boolean`

将文本写入系统剪贴板。适用于实现自定义复制功能。

| 参数 | 类型 | 必填 | 默认值 | 描述 |
|------|------|------|--------|------|
| `text` | `string` | 是 | — | 要写入剪贴板的文本。 |

**返回值：** `boolean` — 写入成功返回 `true`。

#### `readFromClipboard(): string | null`

从系统剪贴板读取文本。

**返回值：** `string | null` — 剪贴板文本内容，如果剪贴板为空或包含非文本数据则返回 `null`。

---

### 过滤

#### `setGlobalFilterMode(mode, programList?): boolean`

配置哪些应用程序应触发文本选择事件。可以包含或排除特定应用程序的选择监听。过滤模式常量详情请参见 [`SelectionHook.FilterMode`](#selectionhookfiltermode)。

| 参数 | 类型 | 必填 | 默认值 | 描述 |
|------|------|------|--------|------|
| `mode` | [`FilterMode`](#selectionhookfiltermode) | 是 | — | 全局过滤模式。 |
| `programList` | `string[]` | 否 | `[]` | 要包含或排除的程序名称。 |

**返回值：** `boolean` — 设置成功返回 `true`。

```javascript
// 仅监听特定程序中的选择
hook.setGlobalFilterMode(SelectionHook.FilterMode.INCLUDE_LIST, [
  "chrome.exe", "firefox.exe", "code.exe"
]);

// 监听除终端外的所有程序
hook.setGlobalFilterMode(SelectionHook.FilterMode.EXCLUDE_LIST, [
  "WindowsTerminal.exe", "cmd.exe", "powershell.exe"
]);
```

> **Linux：** 在 Wayland 上，`programName` 始终为空，因此基于程序的过滤将不起作用。

#### `setFineTunedList(listType, programList?): boolean`

配置针对特定应用程序行为的精细调整列表。这允许你自定义 selection hook 对某些具有独特特性的应用程序的行为方式。

例如，可以将 `acrobat.exe` 添加到这些列表中以启用在 Acrobat 中选择文本的功能。

| 参数 | 类型 | 必填 | 默认值 | 描述 |
|------|------|------|--------|------|
| `listType` | [`FineTunedListType`](#selectionhookfinetunedlisttype) | 是 | — | 精细调整列表类型。 |
| `programList` | `string[]` | 否 | `[]` | 精细调整列表的程序名称。 |

**返回值：** `boolean` — 设置成功返回 `true`。

```javascript
// 对使用自定义光标的应用跳过光标检测
hook.setFineTunedList(
  SelectionHook.FineTunedListType.EXCLUDE_CLIPBOARD_CURSOR_DETECT,
  ["acrobat.exe", "cajviewer.exe"]
);

// 对多次修改剪贴板的应用添加剪贴板读取延迟
hook.setFineTunedList(
  SelectionHook.FineTunedListType.INCLUDE_CLIPBOARD_DELAY_READ,
  ["acrobat.exe"]
);
```

> **平台：** 仅限 Windows。关于何时以及为何使用这些列表，请参见 [Windows 平台详情 — 应用兼容性](WINDOWS.md#app-compatibility-setfinetunedlist)。

---

### 平台特定

#### macOS

#### `macIsProcessTrusted(): boolean`

检查进程是否被授予辅助功能信任权限。如果进程未被信任，selection-hook 仍会运行，但不会响应任何事件。请确保在调用 `start()` 之前引导用户完成授权流程。

**返回值：** `boolean` — 如果进程被授予辅助功能信任权限则返回 `true`。

> **平台：** 仅限 macOS。

#### `macRequestProcessTrust(): boolean`

尝试请求辅助功能权限。如果权限未授予，此方法可能会向用户显示对话框。

**返回值：** `boolean` — 当前的权限状态，而非请求结果。

> **平台：** 仅限 macOS。

#### Linux

#### `linuxGetEnvInfo(): LinuxEnvInfo | null`

获取 Linux 环境信息。返回包含显示协议、合成器类型、输入设备访问状态和 root 状态的对象。所有值在构造时检测一次并缓存。在非 Linux 平台上返回 `null`。

**返回值：** [`LinuxEnvInfo`](#linuxenvinfo) `| null` — Linux 环境信息，在非 Linux 平台上返回 `null`。

完整结构请参见 [`LinuxEnvInfo`](#linuxenvinfo)，合成器常量请参见 [`SelectionHook.CompositorType`](#selectionhookcompositortype)。

```javascript
const info = hook.linuxGetEnvInfo();
// info = {
//   displayProtocol: 2,       // SelectionHook.DisplayProtocol.WAYLAND
//   compositorType: 1,        // SelectionHook.CompositorType.KWIN
//   hasInputDeviceAccess: true, // 用户可以访问输入设备
//   isRoot: false
// }
```

> **平台：** 仅限 Linux。

---

## 事件

#### `text-selection`

当在任何应用程序中选择文本时发出。`data` 结构请参见 [`TextSelectionData`](#textselectiondata)。

```javascript
hook.on("text-selection", (data) => {
  // data 包含选择信息
});
```

#### `mouse-move`、`mouse-up`、`mouse-down`

鼠标事件。`data` 结构请参见 [`MouseEventData`](#mouseeventdata)。

```javascript
hook.on("mouse-up", (data) => {
  // data 包含鼠标坐标和按键信息
});
```

#### `mouse-wheel`

鼠标滚轮事件。`data` 结构请参见 [`MouseWheelEventData`](#mousewheeleventdata)。

```javascript
hook.on("mouse-wheel", (data) => {
  // data 包含滚轮方向信息
});
```

#### `key-down`、`key-up`

键盘事件。`data` 结构请参见 [`KeyboardEventData`](#keyboardeventdata)。

```javascript
hook.on("key-down", (data) => {
  // data 包含按键码和修饰键信息
});
```

#### `status`

Hook 状态变更。

```javascript
hook.on("status", (status) => {
  // status 是一个字符串，例如 "started"、"stopped"
});
```

#### `error`

错误事件。一般错误仅在 `start()` 中设置 `debug` 为 `true` 时才会发出。致命错误（例如 hook 启动/关闭失败）无论 `debug` 设置如何都会始终发出。

```javascript
hook.on("error", (error) => {
  // error 是一个 Error 对象
});
```

---

## 类型

> **坐标说明：** 所有坐标均以**屏幕坐标**返回 — 即各平台显示系统提供的原始值。要转换为用于 UI 定位的**逻辑坐标（DIP）**：
> - **Windows：** 在 Electron 中使用 `screen.screenToDipPoint(point)`。
> - **macOS：** 无需转换 — macOS 的屏幕坐标已经是逻辑坐标。注意：`screen.screenToDipPoint()` 在 macOS 上不可用。
> - **Linux：** 在 Electron 中使用 `screen.screenToDipPoint(point)` — X11 和 Wayland 会话均可统一使用。Wayland 上坐标不可用时为 `-99999`。详见[坐标体系与 HiDPI 缩放](LINUX.md#坐标体系与-hidpi-缩放)。

### `Point`

表示一个二维坐标点。

```typescript
{ x: number; y: number }
```

---

### `SelectionConfig`

`start()` 的配置选项。所有字段均为可选。也可以在 `start()` 之前或之后通过配置方法单独设置。

| 属性 | 类型 | 默认值 | 描述 |
|------|------|--------|------|
| `debug` | `boolean` | `false` | 启用调试日志。 |
| `enableMouseMoveEvent` | `boolean` | `false` | 启用鼠标移动追踪。可在运行时设置。 |
| `enableClipboard` | `boolean` | `true` | 启用剪贴板回退。可在运行时设置。 |
| `selectionPassiveMode` | `boolean` | `false` | 启用被动模式。可在运行时设置。 |
| `clipboardMode` | [`FilterMode`](#selectionhookfiltermode) | `DEFAULT` | 剪贴板过滤模式。可在运行时设置。 |
| `clipboardFilterList` | `string[]` | `[]` | 剪贴板模式的程序列表。可在运行时设置。 |
| `globalFilterMode` | [`FilterMode`](#selectionhookfiltermode) | `DEFAULT` | 全局过滤模式。可在运行时设置。 |
| `globalFilterList` | `string[]` | `[]` | 全局过滤模式的程序列表。可在运行时设置。 |

过滤模式详情请参见 [`SelectionHook.FilterMode`](#selectionhookfiltermode)。

---

### `TextSelectionData`

表示文本选择信息，包括内容、来源应用程序和坐标。

| 属性 | 类型 | 描述 |
|------|------|------|
| `text` | `string` | 选中的文本内容。 |
| `programName` | `string` | 发生选择的应用程序名称。在 Linux Wayland 上始终为空。 |
| `startTop` | [`Point`](#point) | 第一段的左上角坐标（像素）。 |
| `startBottom` | [`Point`](#point) | 第一段的左下角坐标（像素）。 |
| `endTop` | [`Point`](#point) | 最后一段的右上角坐标（像素）。 |
| `endBottom` | [`Point`](#point) | 最后一段的右下角坐标（像素）。 |
| `mousePosStart` | [`Point`](#point) | 选择开始时的鼠标位置（像素）。 |
| `mousePosEnd` | [`Point`](#point) | 选择结束时的鼠标位置（像素）。 |
| `method` | [`SelectionMethod`](#selectionhookselectionmethod) | 指示使用哪种方法检测文本选择。 |
| `posLevel` | [`PositionLevel`](#selectionhookpositionlevel) | 指示提供了哪些位置数据。 |
| `isFullscreen` | `boolean` | 窗口是否处于全屏模式。_仅限 macOS。_ |

> **Linux：** `startTop`/`startBottom`/`endTop`/`endBottom` 始终为 `-99999`（[`INVALID_COORDINATE`](#selectionhookinvalid_coordinate)），因为选择边界矩形不可用。在 Wayland 上，当坐标来源（libevdev）无法提供实际屏幕位置时，`mousePosStart`/`mousePosEnd` 也可能为 `-99999` — 请参见 [Linux 平台详情](LINUX.md) 了解依赖合成器的回退链。

关于 `posLevel` 如何决定哪些坐标字段有意义，请参见 [`PositionLevel`](#selectionhookpositionlevel)。

---

### `MouseEventData`

包含屏幕坐标中的鼠标点击/移动信息。

| 属性 | 类型 | 描述 |
|------|------|------|
| `x` | `number` | 水平指针位置（像素）。 |
| `y` | `number` | 垂直指针位置（像素）。 |
| `button` | `number` | 与 WebAPIs 的 `MouseEvent.button` 相同。`0`=左键，`1`=中键，`2`=右键，`3`=后退，`4`=前进，`-1`=无，`99`=未知。 |

> **Linux Wayland：** `x`/`y` 可能为 [`INVALID_COORDINATE`](#selectionhookinvalid_coordinate)（`-99999`）。参见[坐标说明](#types)。

如果在 `mouse-move` 事件中 `button != -1`，则表示正在拖拽。

---

### `MouseWheelEventData`

描述鼠标滚轮滚动事件。

| 属性 | 类型 | 描述 |
|------|------|------|
| `x` | `number` | 水平指针位置（像素）。 |
| `y` | `number` | 垂直指针位置（像素）。 |
| `button` | `number` | `0`=垂直滚动，`1`=水平滚动。 |
| `flag` | `number` | `1`=向上/向右，`-1`=向下/向左。 |

> **Linux Wayland：** `x`/`y` 可能为 [`INVALID_COORDINATE`](#selectionhookinvalid_coordinate)（`-99999`）。参见[坐标说明](#types)。

---

### `KeyboardEventData`

表示键盘按键按下/释放。

| 属性 | 类型 | 描述 |
|------|------|------|
| `uniKey` | `string` | 统一键名，参考 MDN `KeyboardEvent.key`，从 `vkCode` 转换而来。 |
| `vkCode` | `number` | 虚拟键码。定义和值因平台而异（见下文）。 |
| `sys` | `boolean` | 是否同时按下了修饰键（Ctrl/Alt/Win(Super)/⌘/⌥/Fn）。 |
| `scanCode` | `number?` | 硬件扫描码。_仅限 Windows。_ |
| `flags` | `number` | 附加状态标志。在 Linux 上为修饰键位掩码（`0x01`=Shift，`0x02`=Ctrl，`0x04`=Alt，`0x08`=Meta）。 |

各平台的 `vkCode` 值：

- **Windows**：`vkCode` 的 `VK_*` 值
- **macOS**：`kCGKeyboardEventKeycode` 的 `kVK_*` 值
- **Linux**：来自 `<linux/input-event-codes.h>` 的 `KEY_*` 值

---

### `LinuxEnvInfo`

由 `linuxGetEnvInfo()` 返回。包含在构造时检测的缓存 Linux 环境信息。

| 属性 | 类型 | 描述 |
|------|------|------|
| `displayProtocol` | `number` | 显示协议（[`SelectionHook.DisplayProtocol`](#selectionhookdisplayprotocol)）。 |
| `compositorType` | `number` | 合成器类型（[`SelectionHook.CompositorType`](#selectionhookcompositortype)）。 |
| `hasInputDeviceAccess` | `boolean` | 用户是否可以访问输入设备（Wayland libevdev 输入监听所需）。检查 `input` 组、ACL、capabilities 和实际设备访问权限。在 X11 上始终为 `true`。 |
| `isRoot` | `boolean` | 进程是否以 root 身份运行。 |

> **平台：** 仅限 Linux。

---

## 常量

### `SelectionHook.INVALID_COORDINATE`

哨兵值（`-99999`），表示坐标不可用或不可靠。在 Linux Wayland 上，当输入源（libevdev）无法提供实际屏幕位置时，鼠标事件坐标和选择位置坐标可能被设置为此值。在使用坐标进行 UI 定位之前，请检查坐标字段是否等于此值。

```javascript
if (data.mousePosEnd.x !== SelectionHook.INVALID_COORDINATE) {
  // 位置可靠，可以使用
}
```

---

### `SelectionHook.SelectionMethod`

指示使用哪种方法检测文本选择。

| 常量 | 值 | 平台 | 描述 |
|------|-----|------|------|
| `NONE` | `0` | — | 未检测到选择。 |
| `UIA` | `1` | Windows | UI Automation。 |
| `FOCUSCTL` | `2` | Windows | 已弃用 — 不再发出。保留用于与历史数据的向后兼容。 |
| `ACCESSIBLE` | `3` | Windows | 辅助功能接口。 |
| `AXAPI` | `11` | macOS | Accessibility API。 |
| `ATSPI` | `21` | Linux | 辅助技术服务提供者接口。已预留 — 当前未使用。 |
| `PRIMARY` | `22` | Linux | Primary Selection。 |
| `CLIPBOARD` | `99` | Windows、macOS | 剪贴板回退。Linux 上不使用。 |

---

### `SelectionHook.PositionLevel`

指示提供了哪些位置数据。

| 常量 | 值 | 描述 |
|------|-----|------|
| `NONE` | `0` | 无位置信息。 |
| `MOUSE_SINGLE` | `1` | 仅提供 `mousePosStart` 和 `mousePosEnd`，且它们相等。 |
| `MOUSE_DUAL` | `2` | 提供 `mousePosStart` 和 `mousePosEnd`，位置不同（拖拽选择）。在 Linux Wayland 上，当合成器在鼠标按下和鼠标松开时都提供准确的光标位置时可实现。 |
| `SEL_FULL` | `3` | 提供所有鼠标位置和段落坐标（`startTop`/`startBottom`/`endTop`/`endBottom`）。Linux 上不可用。 |
| `SEL_DETAILED` | `4` | 详细选择坐标。预留供将来使用。 |

---

### `SelectionHook.FilterMode`

| 常量 | 值 | 描述 |
|------|-----|------|
| `DEFAULT` | `0` | 不过滤 — 所有程序均通过。 |
| `INCLUDE_LIST` | `1` | 仅列表中的程序会通过过滤。 |
| `EXCLUDE_LIST` | `2` | 仅不在列表中的程序会通过过滤。 |

---

### `SelectionHook.FineTunedListType`

定义精细调整应用程序行为列表的类型。

| 常量 | 值 | 描述 |
|------|-----|------|
| `EXCLUDE_CLIPBOARD_CURSOR_DETECT` | `0` | 排除剪贴板操作的光标检测。适用于使用自定义光标的应用程序（例如 Adobe Acrobat），这些应用中光标形状检测可能不可靠。 |
| `INCLUDE_CLIPBOARD_DELAY_READ` | `1` | 读取剪贴板内容时包含延迟。适用于快速连续多次修改剪贴板内容的应用程序（例如 Adobe Acrobat）。 |

> **平台：** 仅限 Windows。

---

### `SelectionHook.DisplayProtocol`

定义 Linux 系统上使用的显示协议类型。

| 常量 | 值 | 描述 |
|------|-----|------|
| `UNKNOWN` | `0` | 未检测到协议或不适用。 |
| `X11` | `1` | X11 窗口系统协议。 |
| `WAYLAND` | `2` | Wayland 显示服务协议。 |

> **平台：** 仅限 Linux。

---

### `SelectionHook.CompositorType`

标识合成器。值代表的是**合成器**，而非桌面环境（DE）。DE 自带的合成器通过 `XDG_CURRENT_DESKTOP` 检测（每个 DE 使用唯一一个合成器）；独立合成器通过其自身的环境变量检测。

| 常量 | 合成器 | 桌面环境 | 检测方式 |
|------|--------|----------|----------|
| `UNKNOWN` | — | — | — |
| `KWIN` | KWin (`kwin_wayland`) | KDE Plasma | `XDG_CURRENT_DESKTOP` 包含 "KDE" |
| `MUTTER` | mutter (`gnome-shell`) | GNOME | `XDG_CURRENT_DESKTOP` 包含 "GNOME" |
| `HYPRLAND` | Hyprland | （独立） | `HYPRLAND_INSTANCE_SIGNATURE` 环境变量 |
| `SWAY` | sway | （独立） | `SWAYSOCK` 环境变量 |
| `WLROOTS` | various (labwc, river, ...) | （独立） | `XDG_CURRENT_DESKTOP` 包含 "wlroots" |
| `COSMIC_COMP` | cosmic-comp | COSMIC (System76) | `XDG_CURRENT_DESKTOP` 包含 "COSMIC" |

> **平台：** 仅限 Linux。关于各合成器的光标位置精度和选择监听详情，请参见 [Wayland 合成器兼容性](LINUX.md#wayland-compositor-compatibility)。

---

## TypeScript 支持

本模块包含 TypeScript 类型定义。由于 `selection-hook` 是原生 Node-API 模块，它使用 CommonJS 导出。使用 `import` 导入类型，使用 `require` 获取运行时值：

```typescript
import {
  SelectionHookConstructor,
  SelectionHookInstance,
  SelectionConfig,
  TextSelectionData,
  MouseEventData,
  MouseWheelEventData,
  KeyboardEventData,
  LinuxEnvInfo,
  Point,
} from "selection-hook";

// 使用 `SelectionHookConstructor` 作为 SelectionHook 类的类型
const SelectionHook: SelectionHookConstructor = require("selection-hook");
// 使用 `SelectionHookInstance` 作为 SelectionHook 实例的类型
const hook: SelectionHookInstance = new SelectionHook();
```

详情请参见 [`index.d.ts`](../index.d.ts)。
