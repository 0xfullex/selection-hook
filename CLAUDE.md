# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

This is a Node.js native module (Node-API/N-API) that monitors text selections across applications. It supports Windows, macOS, and Linux with platform-specific implementations.

## Build Commands

- **Rebuild for current platform**: `npm run rebuild`
- **Run demo/test**: `npm run demo`
- **Prebuild all platforms**: `npm run prebuild`

Always run `npm run rebuild` after modifying C++ code to check for compilation errors.

## Architecture

### Entry Points
- `index.js` - Main Node.js wrapper class (SelectionHook) extending EventEmitter
- `index.d.ts` - TypeScript definitions

### Native Module Structure

```
src/
├── windows/
│   ├── selection_hook.cc    # Main implementation using UIAutomation
│   └── lib/                 # Utilities (string_pool, clipboard, keyboard)
├── mac/
│   ├── selection_hook.mm    # Main implementation using Accessibility API (AXAPI)
│   └── lib/                 # Utilities (clipboard, keyboard)
└── linux/
    ├── selection_hook.cc    # Main implementation
    ├── common.h             # Shared definitions
    └── protocols/
        ├── x11.cc           # X11 protocol implementation
        └── wayland.cc       # Wayland protocol implementation (WIP)
```

### Platform-Specific APIs
- **Windows**: UIAutomation for modern apps, Accessibility API for legacy apps, clipboard fallback
- **macOS**: Accessibility API (AXAPI), clipboard fallback
- **Linux**: X11 (Primary Selection), Wayland (in progress)

### Build Configuration
- `binding.gyp` - Node-gyp build configuration with platform-specific sources and libraries
- Uses NAPI version 8 (Node.js 18+, Electron 23+)

## Development Guidelines

- Write all code comments in English
- Do not create/update README.md or example files unless explicitly instructed
- Do not modify `examples/node-demo.js` unless explicitly instructed
- Do not remove existing comments or printf statements unless explicitly instructed
- Ask permission before modifying files beyond the scope of the task
- When modifying native code, also update related files to keep everything consistent:
  - `README.md` - project overview and quick start
  - `docs/API.md` - full API reference (methods, events, data structures, constants)
  - `docs/LINUX.md` - Linux platform documentation (limitations, Wayland compatibility)
  - `index.js` - main entry file and JSDoc comments
  - `index.d.ts` - TypeScript type definitions
  - `examples/node-demo.js` - demo/example code
  - `binding.gyp` - build configuration (if adding/removing source files)
- Cross-platform implementations should use consistent function names, method names, and variable names across `src/windows/`, `src/mac/`, and `src/linux/` unless they are platform-specific

## Development Environments

- **Windows**: Windows 11, PowerShell terminal
- **macOS**: macOS 15.5, Terminal
- **Linux**: Ubuntu 24.04, Terminal
