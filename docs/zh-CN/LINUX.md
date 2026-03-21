# Linux 协议实现

[English](../LINUX.md)

**[selection-hook](https://github.com/0xfullex/selection-hook) 的一部分** — 一个用于跨应用监控文本选择的 Node.js 原生模块。

---

本目录包含 selection-hook 在 Linux 上的 X11 和 Wayland 协议实现。

## 架构

```
protocols/
├── x11.cc              # X11 协议：XRecord（输入）+ XFixes（PRIMARY 选区）
├── wayland.cc          # Wayland 协议：libevdev（输入）+ data-control（PRIMARY 选区）
└── wayland/            # 预生成的 Wayland 协议 C 绑定
```

Linux 上的选中文本完全通过 **PRIMARY 选区** 获取 — 当用户选择文本时，文本会立即可用（无需 Ctrl+C）。这与 Windows/macOS 使用 UI Automation、无障碍 API 和剪贴板回退的方式有根本区别。

## 平台限制

### Linux 通用限制（X11 和 Wayland）

| 限制 | 详情 |
|---|---|
| **剪贴板读写已禁用** | `writeToClipboard()` 和 `readFromClipboard()` 在 Linux 上返回 false。X11 的懒加载剪贴板模型要求所有者保持窗口存活并响应 `SelectionRequest` 事件，这在库的上下文中是不可靠的。宿主应用应使用自己的剪贴板 API（例如 Electron 的 `clipboard` 模块）。 |
| **无剪贴板回退** | 未在 Linux 上实现 Ctrl+C 剪贴板回退机制（该机制在 Windows/macOS 上作为最后手段使用）。文本仅通过 PRIMARY 选区获取。 |
| **无文本范围坐标** | `startTop`、`startBottom`、`endTop`、`endBottom` 始终为 `-99999`（`INVALID_COORDINATE`）。选区边界矩形在 Linux 上不可用。`posLevel` 最高为 `MOUSE_SINGLE` 或 `MOUSE_DUAL`，永远不会达到 `SEL_FULL`。 |

### X11 特有

| 功能 | 状态 | 说明 |
|---|---|---|
| 选区监控 | ✅ 正常 | XFixes `SelectionNotify` 监听 PRIMARY 选区 |
| 输入事件（鼠标/键盘） | ✅ 正常 | XRecord 扩展 |
| 光标位置 | ✅ 精确 | `XQueryPointer` — 屏幕坐标 |
| 程序名称 | ✅ 正常 | `WM_CLASS` 属性 |
| 窗口矩形 | ✅ 正常 | `XGetWindowAttributes` + `XTranslateCoordinates` |

### Wayland 特有

| 功能 | 状态 | 说明 |
|---|---|---|
| 选区监控 | ✅ 正常 | `ext-data-control-v1` 或 `wlr-data-control-unstable-v1 v2+`（见合成器表格） |
| 输入事件（鼠标/键盘） | ✅ 正常 | libevdev 读取 `/dev/input/event*` — 需要 `input` 组成员资格 |
| 光标位置 | 取决于合成器 | 见下方合成器兼容性表格 |
| 程序名称 | ❌ 始终为空 | Wayland 安全模型不暴露窗口信息 |
| 窗口矩形 | ❌ 始终不可用 | Wayland 不暴露全局窗口坐标 |

**左手鼠标支持（仅 Wayland）：**

在 Wayland 上，libevdev 从 `/dev/input/event*` 读取原始物理按键代码，绕过了 libinput 的左手按键交换。selection-hook 同时监控 `BTN_LEFT` 和 `BTN_RIGHT` 用于手势检测（拖拽、双击、Shift+点击），因此通过系统设置交换鼠标按键的左手用户可以正常使用其主按键（物理右键）进行选区检测。现有的手势-选区关联机制会自然过滤掉不产生文本选区的右键菜单操作。

在 X11 上，XRecord 捕获的是交换后的逻辑事件，因此左手模式无需任何特殊处理即可正常工作。

**输入设备访问（仅 Wayland）：**

Wayland 的安全模型阻止应用通过显示服务器拦截全局输入事件。我们使用 libevdev 直接从 `/dev/input/event*` 设备读取，这要求用户具有访问这些设备的权限。最常见的方式是加入 `input` 组：

```bash
sudo usermod -aG input $USER
# 然后重新登录以使更改生效
```

其他可授予访问权限的方式包括 systemd-logind ACL（通常为活动会话自动设置）、自定义 udev 规则和 Linux capabilities。`hasInputDeviceAccess` 会检查所有这些方式。

你可以通过编程方式检查当前用户是否具有输入设备访问权限：

```javascript
const info = hook.linuxGetEnvInfo();
if (info && !info.hasInputDeviceAccess) {
  console.warn('User does not have input device access. Run: sudo usermod -aG input $USER');
}
```

**无输入设备访问时的回退（Wayland）：**

当输入设备不可访问时，selection-hook 会回退到 **data-control 防抖模式**（路径 C）。在此模式下，文本选区仅通过 Wayland data-control 协议事件检测，防抖时间为 300ms。这意味着：

- 鼠标/键盘事件**不会**被触发
- 选区检测仍然有效，但延迟略高（用户完成选择后约 300ms）
- `posLevel` 将为 `MOUSE_SINGLE`（在检测时从合成器查询光标位置，如果不可用则为 `-99999`）
- `programName` 始终为空（Wayland 限制）

### Wayland 合成器兼容性

#### 选区监控

选区监控依赖 Wayland data-control 协议。库优先使用 `ext-data-control-v1`（标准化），回退到 `wlr-data-control-unstable-v1 v2+`（wlroots 特有）。

| 合成器 | 协议 | 选区监控 |
|---|---|---|
| **KDE Plasma 5/6** (KWin) | wlr-data-control | ✅ 正常 |
| **Hyprland** | wlr-data-control | ✅ 正常 |
| **Sway** | wlr-data-control | ✅ 正常 |
| **基于 wlroots 的**（labwc、river 等） | wlr-data-control | ✅ 正常 |
| **COSMIC** | ext-data-control | ✅ 正常 |
| **GNOME** (Mutter) | — | ❌ 不支持 — Mutter 未实现 data-control 协议 |

#### 光标位置

Wayland 没有提供用于全局光标位置查询的标准 API。我们使用合成器特定的 IPC，并提供多级回退链：

1. **合成器原生 IPC**（如果可用）
2. **XWayland 回退** — 在 XWayland 显示器上使用 `XQueryPointer`
3. **libevdev 累积** — 设备相对增量（最后手段，不精确）

| 合成器 | 方式 | 精确度 | 说明 |
|---|---|---|---|
| **KDE Plasma 6** | ✅ KWin Scripting DBus | 精确的屏幕坐标 | 加载一个 JS 脚本读取 `workspace.cursorPos` 并通过 DBus 回调。自动检测不同 Plasma 6 构建版本的 per-script `run()` 与 manager `start()` |
| **KDE Plasma 5** | ✅ KWin Scripting DBus | 精确的屏幕坐标 | 与 Plasma 6 相同的方式，兼容两种 KWin DBus API 变体 |
| **Hyprland** | ✅ 原生 IPC | 精确的屏幕坐标 | 通过 Unix 套接字（`$HYPRLAND_INSTANCE_SIGNATURE`）执行 `hyprctl cursorpos` |
| **Sway** | ⚠️ XWayland 回退 | 部分可用 | 当光标位于原生 Wayland 窗口上方时坐标可能冻结 |
| **基于 wlroots 的**（labwc、river 等） | ⚠️ XWayland 回退 | 部分可用 | 当光标位于原生 Wayland 窗口上方时坐标可能冻结 |
| **COSMIC** | ⚠️ XWayland 回退 | 部分可用 | 当光标位于原生 Wayland 窗口上方时坐标可能冻结 |
| **GNOME** (Mutter) | ⚠️ XWayland 回退 | 部分可用 | 当光标位于原生 Wayland 窗口上方时坐标可能冻结 |

**XWayland 回退详情：**
- 需要 `DISPLAY` 环境变量（XWayland 必须正在运行）
- 在 XWayland X 显示器上使用 `XQueryPointer`
- 当光标位于 XWayland 窗口上方时坐标跟踪正确，但当光标移至原生 Wayland 窗口上方时可能冻结在最后已知位置
- 如果 XWayland 不可用，则回退到 libevdev 累积坐标（设备相对坐标，非屏幕坐标）

**坐标不可用（`INVALID_COORDINATE = -99999`）：**

在 Wayland 上，鼠标事件坐标（`x`、`y`）来自 libevdev 硬件事件（相对增量或绝对硬件值），不代表实际的屏幕位置。这些坐标被报告为 `-99999`（`SelectionHook.INVALID_COORDINATE`），以明确表示它们不可用。在使用坐标进行定位之前，始终应检查坐标字段是否为此哨兵值。

对于文本选区事件，坐标回退链的工作方式如下：
- **合成器 IPC**（Hyprland、KDE）：提供精确的屏幕坐标 → 真实值
- **XWayland**：当光标位于 XWayland 窗口上方时提供精确坐标 → 真实值
- **XWayland 冻结**：检测到鼠标按下和鼠标释放查询返回相同坐标（尽管有物理移动）→ `-99999`
- **无 IPC，无 XWayland**：libevdev 值 → `-99999`

对于 Wayland 上的拖拽选区，库会在鼠标按下和鼠标释放时分别查询合成器，当两次查询都成功且坐标不同时（表明光标确实在 XWayland/合成器追踪的窗口之间移动了），可以达到 `MOUSE_DUAL` 位置级别。

## Linux 上的 API 行为

以下 API 在 Linux 上与 Windows/macOS 的行为有所不同：

| API | X11 | Wayland | 说明 |
|---|---|---|---|
| `linuxGetEnvInfo()` | ✅ 返回环境信息 | ✅ 返回环境信息 | 可在 `start()` 之前调用。非 Linux 上返回 `null`。包含 `displayProtocol`、`compositorType`、`hasInputDeviceAccess`（X11 上始终为 `true`）、`isRoot` |
| `writeToClipboard()` | 返回 `false` | 返回 `false` | 在 JS 层被阻止。请使用宿主应用的剪贴板 API。 |
| `readFromClipboard()` | 返回 `null` | 返回 `null` | 在 JS 层被阻止。请使用宿主应用的剪贴板 API。 |
| `enableClipboard()` / `disableClipboard()` | 无效果 | 无效果 | 剪贴板回退未在 Linux 上实现 |
| `setClipboardMode()` | 无效果 | 无效果 | 剪贴板回退未在 Linux 上实现 |
| `setFineTunedList()` | 无效果 | 无效果 | 仅 Windows |
| `setGlobalFilterMode()` | ✅ 有效 | ⚠️ 无效 | `programName` 在 Wayland 上始终为空，因此基于程序名的过滤无法匹配 |
| 事件中的 `programName` | ✅ 通过 `WM_CLASS` | 始终为 `""` | Wayland 安全模型限制 |
| `startTop/startBottom/endTop/endBottom` | 始终为 `-99999` | 始终为 `-99999` | 选区边界矩形不可用。请与 `INVALID_COORDINATE` 进行比较检查。 |
| `posLevel` | `MOUSE_SINGLE` 或 `MOUSE_DUAL` | `MOUSE_SINGLE` 或 `MOUSE_DUAL` | Wayland 拖拽可在合成器在鼠标按下和释放时均提供精确位置的情况下达到 `MOUSE_DUAL`。Linux 上永远不会达到 `SEL_FULL`。 |
| `mousePosStart` / `mousePosEnd` | ✅ 屏幕坐标 | 取决于合成器 | 不可用时可能为 `-99999`。见合成器兼容性表格。 |

## Electron 应用提示

在 Wayland 上的 **Electron** 应用中使用 selection-hook 时，建议通过添加 `--ozone-platform=x11` 命令行参数让 Electron 在 XWayland 模式下运行。这是因为 Electron 本身在原生 Wayland 下存在显著限制：

- **`BrowserWindow.setPosition()` / `setBounds()`** — 在 Wayland 上不可用。Wayland 协议禁止以编程方式更改全局窗口坐标。
- **`BrowserWindow.getPosition()` / `getBounds()`** — 在 Wayland 上返回 `[0, 0]` / `{x: 0, y: 0, ...}`，因为无法获取全局窗口坐标。

这些 Electron 层面的限制使得实现将弹出窗口定位到选中文本附近等功能变得困难。在 XWayland 下运行可以避免这些问题，同时也让 selection-hook 通过 `XQueryPointer` 获得精确的光标坐标。

> **重要：** `app.commandLine.appendSwitch('ozone-platform', 'x11')` **不起作用** — ozone 平台初始化发生在 Chromium 的早期启动阶段，在应用 JavaScript 入口点执行之前。你必须在外部设置此参数。

**选项 1** — 命令行参数（推荐）：

```bash
your-electron-app --ozone-platform=x11
```

**选项 2** — 包装脚本或 `.desktop` 文件：

```ini
# 在你的 .desktop 文件中
Exec=your-electron-app --ozone-platform=x11 %U
```

**选项 3** — 环境变量（仅 Electron < 38）：

```bash
ELECTRON_OZONE_PLATFORM_HINT=x11 your-electron-app
```

> **注意：** 从 Electron 38 开始，`--ozone-platform` 的默认值为 `auto`，这意味着 Electron 将在 Wayland 会话中作为原生 Wayland 应用运行。`ELECTRON_OZONE_PLATFORM_HINT` 环境变量在 Electron 38 中已被移除，在 Electron 39+ 中将被忽略。请改用 `--ozone-platform=x11` 命令行参数。
