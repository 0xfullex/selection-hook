<div align="center">

<pre>
┌─┐┌─┐┬  ┌─┐┌─┐┌┬┐┬┌─┐┌┐┌   ┬ ┬┌─┐┌─┐┬┌─
└─┐├┤ │  ├┤ │   │ ││ ││││───├─┤│ ││ │├┴┐
└─┘└─┘┴─┘└─┘└─┘ ┴ ┴└─┘┘└┘   ┴ ┴└─┘└─┘┴ ┴
</pre>

<h1>selection-hook</h1>

<p><strong>首个开源的全功能跨平台跨应用划词取词库。</strong></p>

[![npm version](https://img.shields.io/npm/v/selection-hook?style=flat)](https://www.npmjs.org/package/selection-hook)
[![license](https://img.shields.io/npm/l/selection-hook?style=flat)](LICENSE)
![platform](https://img.shields.io/badge/platform-Windows%20%7C%20macOS%20%7C%20Linux-blue?style=flat)

[English](README.md) · [中文](README.zh-CN.md)

</div>

<div align="center">
<img src="docs/images/selection-hook.gif" alt="在任何应用中获取选中文本" style="width: 446px;">
</div>

检测用户在**任意应用程序**中选中文本的操作，并**实时**获取选中的文本内容、屏幕坐标和来源程序名称。支持 **Windows、macOS 和 Linux**，使用原生无障碍 API，极少触及剪贴板。以原生 **Node.js/Electron** 插件形式构建，可直接用于生产环境。

划词取词一直是闭源商业软件的专属能力，现有的所有实现都封闭在商业产品内部。selection-hook **首次将这一能力带入开源世界**，其表现**比肩甚至超越众多闭源方案**。它还**内置了全平台的全局鼠标和键盘事件监听**——而该领域现有的开源方案要么已停止维护，要么仅支持单一平台。

## ✨ 核心特性

- ⚡ **实时检测** — 自动捕获文本选中事件，无需轮询
- 📋 **丰富的元数据** — 选中文本、屏幕坐标、鼠标位置和来源程序名称
- 🌍 **跨平台** — Windows、macOS 和 Linux（X11 & Wayland），统一 API
- 🛡️ **剪贴板友好** — 优先使用原生 OS 无障碍 API；剪贴板回退默认启用作为最后手段，但极少触发，也可完全禁用
- 🖱️ **输入事件** — 鼠标（`down`/`up`/`wheel`/`move`）和键盘（`keydown`/`keyup`）事件，包含完整细节，无需额外钩子
- ⚙️ **可配置** — 剪贴板回退开关、按应用过滤、被动模式等

## 💡 应用场景

- 🤖 **AI 助手** — 在任意应用中选中文本即可触发 AI 操作，如 [Cherry Studio](https://github.com/CherryHQ/cherry-studio) 的划词助手或豆包
- 💬 **划词工具** — 选中文本后弹出操作菜单，如 PopClip
- 📖 **词典 / 翻译工具** — 选中即查，如欧路词典、GoldenDict 或 Bob
- 📎 **剪贴板管理器** — 捕获选中内容而不污染剪贴板，如 Ditto 或 Paste
- ♿ **无障碍工具** — 朗读或放大选中的文本
- 🛠️ **开发者工具** — 即时检查或转换选中的内容

同类工具大多闭源且仅支持单一平台。selection-hook 完全开源，通过统一的 API 支持 Windows、macOS 和 Linux。

## 🖥️ 支持的平台

| 平台 | 状态 |
| -------- | ------ |
| Windows  | ✅ 完全支持（Windows 7+） |
| macOS    | ✅ 完全支持（macOS 10.14+） |
| Linux    | ✅ X11 — 良好支持<br>⚠️ Wayland — 支持但有限制 |

与 Windows/macOS 相比，Linux 由于显示服务器架构的原因存在平台级限制。Wayland 由于其严格的安全模型还有额外限制。详见 [Linux 平台文档](docs/zh-CN/LINUX.md)。

## 🚀 快速开始

### 安装

包含预构建二进制文件 — 无需编译。

```bash
npm install selection-hook
```

### 基本用法

```javascript
const SelectionHook = require("selection-hook");

const selectionHook = new SelectionHook();

// 监听文本选中事件
selectionHook.on("text-selection", (data) => {
  console.log("Selected text:", data.text);
  console.log("Program:", data.programName);
  console.log("Coordinates:", data.endBottom);
});

// 开始监听
selectionHook.start();

// 按需获取当前选中内容
const currentSelection = selectionHook.getCurrentSelection();
if (currentSelection) {
  console.log("Current selection:", currentSelection.text);
}

// 停止监听（可稍后重新启动）
selectionHook.stop();

// 使用完毕后清理资源
selectionHook.cleanup();
```

### 返回数据

`text-selection` 事件发出的对象示例：

```json
{
  "text": "Hello, world!",
  "programName": "Google Chrome",
  "startTop": { "x": 100, "y": 200 },
  "startBottom": { "x": 100, "y": 220 },
  "endTop": { "x": 250, "y": 200 },
  "endBottom": { "x": 250, "y": 220 },
  "mousePosStart": { "x": 95, "y": 210 },
  "mousePosEnd": { "x": 255, "y": 210 },
  "method": 1,
  "posLevel": 3
}
```

参阅[使用指南](docs/zh-CN/GUIDE.md)了解深入的使用模式、平台配置和 Electron 集成。参阅 [`examples/node-demo.js`](https://github.com/0xfullex/selection-hook/blob/main/examples/node-demo.js) 查看交互式示例。

## 🔧 工作原理

| 平台 | 主要方法 | 回退方案 |
| -------- | -------------- | -------- |
| Windows  | UI Automation、Accessibility API | 模拟 `Ctrl+C` |
| macOS    | Accessibility API (AXAPI) | 模拟 `⌘+C` |
| Linux    | PRIMARY selection (X11/Wayland) | — |

Selection Hook 使用原生 OS 无障碍 API 直接从当前聚焦的应用程序读取选中文本 — 无需轮询。剪贴板回退默认启用，但仅在无障碍 API 无法获取文本时作为最后手段触发，因此在绝大多数情况下剪贴板不会被触及。如果需要确保零剪贴板干扰，可以通过 `disableClipboard()` 或在启动配置中设置 `{ enableClipboard: false }` 完全禁用回退。

## 📖 使用指南

如需了解深入的使用模式、平台配置、Electron 集成和配置选项，请参阅 [docs/GUIDE.md](docs/zh-CN/GUIDE.md)。

## 📚 API 参考

如需完整的 API 文档 — 方法、事件、数据结构和常量，请参阅 [docs/API.md](docs/zh-CN/API.md)。

## 🏗️ 从源码构建

npm 包已附带预构建二进制文件 — 仅在修改原生代码时需要构建。

- `npm run rebuild` — 为当前平台构建
- `npm run prebuild` — 为所有支持的平台构建
- `npm run demo` — 运行示例

<details>
<summary>Linux 构建依赖</summary>

```bash
# Ubuntu/Debian
sudo apt install libevdev-dev libxtst-dev libx11-dev libxfixes-dev libwayland-dev

# Fedora
sudo dnf install libevdev-devel libXtst-devel libX11-devel libXfixes-devel wayland-devel

# Arch
sudo pacman -S libevdev libxtst libx11 libxfixes wayland
```

Wayland 协议 C 绑定已预生成并提交到仓库 — 详见 [`src/linux/protocols/wayland/README.md`](src/linux/protocols/wayland/README.md)。

</details>

<details>
<summary>Python setuptools</summary>

如果在构建过程中遇到 `ModuleNotFoundError: No module named 'distutils'`，请安装所需的 Python 包：

```bash
pip install setuptools
```

</details>

<details>
<summary>Electron 注意事项</summary>

**electron-builder**：Electron 会在打包时强制重新构建 Node 包。您可能需要事先在 `./node_modules/selection-hook` 中运行 `npm install`，以确保必要的包已下载。

**electron-forge**：在配置中添加以下内容以避免不必要的重新构建：

```javascript
rebuildConfig: {
    onlyModules: [],
},
```

</details>

### 兼容性

- Node.js 12.22+ | Electron 14+
- 包含 TypeScript 支持

## 💎 谁在使用

以下项目正在使用 selection-hook：

- **[Cherry Studio](https://github.com/CherryHQ/cherry-studio)** — 一款功能全面的 AI 客户端，其划词助手可便捷地对选中文本进行 AI 驱动的翻译、解释、摘要等操作。_（本库最初为 Cherry Studio 开发，该项目展示了最佳使用实践。）_

在您的项目中使用了 selection-hook？[告诉我们！](https://github.com/0xfullex/selection-hook/issues)

## 📄 许可证

[MIT](LICENSE)
