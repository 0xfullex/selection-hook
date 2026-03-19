# Text Selection Hook for Node.js and Electron

[中文](README.zh-CN.md)

[![npm version](https://img.shields.io/npm/v/selection-hook?style=flat)](https://www.npmjs.org/package/selection-hook)

A native Node.js module with Node-API that allows monitoring text selections across applications using multiple methods.

## Features

Maybe the first-ever open-source, fully functional text selection tool.

- **Cross-application text selection monitoring**
  - Capture selected text content and its screen coordinates
  - Auto-triggers on user selection, or manual API triggers
  - Rich API to control the selection behaviors
- **Input event listeners**
  - Mouse events: `down`/`up`/`wheel`/`move` with buttons information
  - Keyboard events: `keydown`/`keyup` with keys information
  - _No additional hooks required_ – works natively.
- **Multi-method to get selected text** (automatic fallback):
  - For Windows:
    - _UI Automation_ (modern apps)
    - _Accessibility API_ (legacy apps)
    - _Clipboard fallback_ (simulated `Ctrl + C` with optimizations when all other methods fail)
  - For macOS:
    - _Accessibility API (AXAPI)_
    - _Clipboard fallback_ (simulated `⌘ + C` with optimizations when all other methods fail)
  - For Linux:
    - _Primary Selection_ (X11 and Wayland)
- **Clipboard**
  - Read/write clipboard (not supported on Linux, use host app's clipboard API)
- **Compatibility**
  - Node.js `v10+` | Electron `v3+`
  - TypeScript support included.

## Supported Platforms

| Platform | Status                                              |
| -------- | --------------------------------------------------- |
| Windows  | ✅ Fully supported. Windows 7+                        |
| macOS    | ✅ Fully supported. macOS 10.14+                    |
| Linux    | ✅ X11 - Well supported. ⚠️ Wayland - Supported with limitations |

Linux has platform-level limitations compared to Windows/macOS, especially under Wayland where some APIs are unavailable due to the security model. See [`docs/LINUX.md`](docs/LINUX.md) for details.

## Installation

```bash
npm install selection-hook
```

## Demo

```bash
npm run demo
```

## Building

### Use pre-built packages

The npm package ships with pre-built `.node` files in `prebuilds/*` — no rebuilding needed.

### Build from scratch

- Use `npm run rebuild` to build your platform's specific package.
- Use `npm run prebuild` to build packages for all the supported platforms.

#### Linux build dependencies

```bash
# Ubuntu/Debian
sudo apt install libevdev-dev libxtst-dev libx11-dev libxext-dev libxi-dev libwayland-dev

# Fedora
sudo dnf install libevdev-devel libXtst-devel libX11-devel libXext-devel libXi-devel wayland-devel

# Arch
sudo pacman -S libevdev libxtst libx11 libxext libxi wayland
```

The Wayland protocol C bindings are pre-generated and committed — see [`src/linux/protocols/wayland/README.md`](src/linux/protocols/wayland/README.md) for details.

#### Python setuptools

When building, if the `ModuleNotFoundError: No module named 'distutils'` error prompt appears, please install the necessary Python library via `pip install setuptools`.

### Electron rebuilding

When using `electron-builder` for packaging, Electron will forcibly rebuild Node packages. In this case, you may need to run `npm install` in `./node_modules/selection-hook` in advance to ensure the necessary packages are downloaded.

### Avoid Electron rebuilding

When using `electron-forge` for packaging, you can add these values to your `electron-forge` config to avoid rebuilding:

```javascript
rebuildConfig: {
    onlyModules: [],
},
```



## Usage

```javascript
const SelectionHook = require("selection-hook");

// Create a new instance
// You can design it as a singleton pattern to avoid resource consumption from multiple instantiations
const selectionHook = new SelectionHook();

// Listen for text selection events
selectionHook.on("text-selection", (data) => {
  console.log("Selected text:", data.text);
  // For mouse start/end position and text range coornidates
  // see API Reference
});

// Start monitoring (with default configuration)
selectionHook.start();

// When you want to get the current selection directly
const currentSelection = selectionHook.getCurrentSelection();
if (currentSelection) {
  console.log("Current selection:", currentSelection.text);
}

// Stop, you can start it again
selectionHook.stop();
// Clean up when done
selectionHook.cleanup();
```

See [`examples/node-demo.js`](https://github.com/0xfullex/selection-hook/blob/main/examples/node-demo.js) for detailed usage.

## Documentation

For full API documentation, see [API Reference](docs/API.md).

- [API Reference](docs/API.md) — Methods, events, data structures, and constants
- [Linux Platform](docs/LINUX.md) — Linux-specific details, limitations, and Wayland compositor compatibility
- [Wayland Protocol Bindings](src/linux/protocols/wayland/README.md) — Pre-generated Wayland protocol C bindings

## Used By

This project is used by:

- **[Cherry Studio](https://github.com/CherryHQ/cherry-studio)**: A full-featured AI client, with Selection Assistant that conveniently enables AI-powered translation, explanation, summarization, and more for selected text. _(This lib was originally developed specifically for Cherry Studio, which showcases the best practices for using)_
