# CLAUDE.md

## Project Overview

This is a Node.js native module (Node-API/N-API) that monitors text selections across applications. It supports Windows, macOS, and Linux with platform-specific implementations.

## Build & Validation

- `npm run rebuild` - Rebuild native module for current platform
- `npm run demo` - Run demo
- `npm run prebuild` - Prebuild all platforms
- `npm run typecheck` - TypeScript type validation
- `npm run format` - Format all C++ source files with clang-format (cross-platform, `CLANG_FORMAT` env selects the binary)
- `npm run format:check` - Fail if any C++ file is not formatted
- `npm test` - Unit tests then integration tests (what CI runs)
- `npm run test:unit` - L1 unit tests of `index.js` against a fake native module (`test/unit/`)
- `npm run test:integration` - L2 integration tests against the real `.node` (`test/integration/`)
- `npm run test:local` - Release gate: forced rebuild, unit + integration with local tier enabled, machine-judged (macOS only)

After modifying code, run the relevant checks:
- C++ code (`src/`) → `npm run rebuild` && `npm run format` && `npm test`
- `index.js` or `index.d.ts` → `npm run typecheck` && `npm run test:unit`
- `package.json` → `npm install` (to keep `package-lock.json` in sync)
- Test infrastructure (`scripts/run-tests.js`, `test/lib/`, `test/helpers/`) → `npm test`

## Testing

### Tiers

- **gate**: build, typecheck, format, `native-load`, all unit tests, `api-smoke`, `lifecycle`, clipboard contract. Runs in CI on all platforms and locally. Must never skip except for the two Windows native unit fixtures described below; `scripts/run-tests.js` fails the run on any skip outside the allowlist.
- **env**: needs real input monitoring (macOS accessibility permission, a display on Linux). Skips with `requires()` where the capability is missing (GitHub-hosted macOS runners cannot be granted accessibility).
- **local**: writes to the real clipboard. Always runs in CI; on dev machines only with `SELECTION_HOOK_TEST_LOCAL=1` (set automatically by `npm run test:local`).

CI (`.github/workflows/ci.yml`) runs gate + env + local on Windows, macOS, Linux x64 and Linux arm64 (Linux under `xvfb-run`). The release workflow depends on it.

### Release gate (before every release)

On a macOS dev machine with accessibility permission granted, `npm run test:local` must exit 0. It rebuilds first (a stale `build/` binary is loaded in preference to `prebuilds/`), then requires: no failures, no test skipped for missing input-monitoring, and `stop-race: input-monitoring` (issue #23) in the passed list. `npm run test:local -- --no-rebuild` is a quick mode and is not a release gate.

### Rules

- A bug fix must come with a regression test that was shown to fail before the fix. Reference the issue or PR number when one exists; otherwise the test's header comment must describe the pre-fix behavior it guards against.
- `start()` may only be called inside `test/integration/scenarios/*.js`. Scenario files run in a watchdog child process via `runScenario()`, must not contain `.test.` in their name, and must check the `SELECTION_HOOK_SCENARIO` sentinel as their first statement.
- `*.test.js` files under `test/integration/` only spawn scenarios and assert; parent-process calls are limited to APIs that do not start the hook.
- Skipping is only allowed through `requires()` / `localTier()` from `test/helpers/capabilities.js`, and only for test names registered in `SKIP_ALLOWLIST` in `test/lib/gate.js`. Register new env / local tier tests there; never rename a test that is in the allowlist (`stop-race: input-monitoring` is also referenced by the release gate).
- Unit tests must not skip and must not touch the native module; use `loadIndexWith()` from `test/helpers/fake-native.js`. The only exceptions are `windows clipboard native: restore` and `windows clipboard native: activity`, which compile production C++ against deterministic Win32 fakes without loading the addon. They may skip only on non-Windows platforms, with the exact reason `needs windows-native-fixtures: Windows only`; neither may skip on Windows.
- Do not add test-only entry points to `index.js` or the native code.

## Architecture

### Entry Points
- `index.js` - Main Node.js wrapper class (SelectionHook) extending EventEmitter
- `index.d.ts` - TypeScript definitions
- `binding.gyp` - Node-gyp build configuration (NAPI version 8, Node.js 18+, Electron 23+)

### Native Module Structure

Each platform has `selection_hook.cc/.mm` (module entry and platform coordination) and `lib/` (utilities such as clipboard, keyboard, and helpers). Windows and macOS also have `core/` for the extracted retrieval engine and shared value types:

- `src/windows/` - N-API and hook lifecycle in `selection_hook.cc`; UIA/MSAA/clipboard retrieval, shared types, and the automatic MTA worker in `core/`; reusable platform utilities in `lib/`
- `src/mac/` - N-API, event taps, and gesture detection in `selection_hook.mm`; AXAPI/clipboard retrieval and shared types in `core/` (no worker thread; retrieval stays on the JS thread); reusable platform utilities in `lib/`
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

### C++ Comment Style

- Use Doxygen-compatible `/** ... */` comments on cross-file interfaces and important internal contracts. Document inputs with `@param[in]`, outputs with `@param[out]`, return semantics with `@return`, non-obvious behavior with `@note`, and compatibility or safety hazards with `@warning`.
- Put interface contracts on declarations in headers when a header exists. Use `//` comments inside implementations to explain local rationale, invariants, compatibility history, and cleanup or threading requirements.
- Comments should explain why a constraint exists, not restate the code. A warning must name the invariant, the consequence of breaking it, and the validation or compatibility evidence required before changing it. Avoid context-free comments such as `do not modify`.
- Preserve comments that record application-specific behavior, operating-system quirks, thread or COM ownership, clipboard restoration, event timing, fallback order, and external issue references. Update those comments in the same change when the related behavior changes.

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
