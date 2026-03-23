# CLAUDE.md

## Project Overview

This is a Node.js native module (Node-API/N-API) that monitors text selections across applications. It supports Windows, macOS, and Linux with platform-specific implementations.

## Build & Validation

- `npm run rebuild` - Rebuild native module for current platform
- `npm run demo` - Run demo/test
- `npm run prebuild` - Prebuild all platforms
- `npm run typecheck` - TypeScript type validation
- `npm run format` - Format all C++ source files with clang-format

After modifying code, run the relevant checks:
- C++ code (`src/`) → `npm run rebuild` && `npm run format`
- `index.js` or `index.d.ts` → `npm run typecheck`

## Architecture

### Entry Points
- `index.js` - Main Node.js wrapper class (SelectionHook) extending EventEmitter
- `index.d.ts` - TypeScript definitions
- `binding.gyp` - Node-gyp build configuration (NAPI version 8, Node.js 18+, Electron 23+)

### Native Module Structure

Each platform has `selection_hook.cc/.mm` (main implementation) and `lib/` (utilities like clipboard, keyboard, utils):

- `src/windows/` - UIAutomation for modern apps, Accessibility API for legacy apps, clipboard fallback
- `src/mac/` - Accessibility API (AXAPI), clipboard fallback
- `src/linux/` - X11 (Primary Selection via `protocols/x11.cc`), Wayland (via `protocols/wayland.cc`, in progress)

### Documentation Structure

- `docs/` - English documentation (API, Guide, Linux, Windows)
- `docs/zh-CN/` - Chinese documentation (mirrors English docs)
- `README.md` (English) / `README.zh-CN.md` (Chinese)

## Development Guidelines

- Write all code comments in English
- Do not remove existing comments or printf statements unless explicitly instructed
- Ask permission before modifying files beyond the scope of the task
- Cross-platform implementations should use consistent function names, method names, and variable names across `src/windows/`, `src/mac/`, and `src/linux/` unless they are platform-specific
- Never attempt to `git add` files that are listed in `.gitignore`, even with `-f`
- Use only ASCII characters in source code files (including comments). Avoid non-ASCII characters like em-dash (`—`), curly quotes, etc. to prevent MSVC C4819 warnings on non-Unicode code pages

### Keeping docs in sync

When interfaces or behavior change, update all related files to keep everything consistent:
- `index.js` / `index.d.ts` - JSDoc and TypeScript definitions
- `docs/API.md` - full API reference
- `docs/GUIDE.md` - usage guide
- `docs/LINUX.md` / `docs/WINDOWS.md` - platform documentation
- `README.md` - project overview and quick start
- `examples/node-demo.js` - demo/example code
- `binding.gyp` - build configuration (if adding/removing source files)

When updating documentation, keep English (`docs/`) and Chinese (`docs/zh-CN/`) versions in sync.

## Development Environments

- **Windows**: Windows 11, PowerShell terminal
- **macOS**: macOS 15.5, Terminal
- **Linux**: Ubuntu 24.04, Terminal
