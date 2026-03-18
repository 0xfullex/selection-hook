# Wayland Data Control Protocol Bindings

This directory contains vendored Wayland protocol XML files and their pre-generated C bindings for PRIMARY selection monitoring.

## Protocols

| Protocol | Source | Purpose |
|----------|--------|---------|
| `ext-data-control-v1` | [wayland-protocols (staging)](https://gitlab.freedesktop.org/wayland/wayland-protocols/-/tree/main/staging/ext-data-control) | Preferred — standardized data control protocol |
| `wlr-data-control-unstable-v1` | [wlr-protocols](https://gitlab.freedesktop.org/wlroots/wlr-protocols) | Fallback — wlroots-specific, requires version ≥ 2 for `primary_selection` |

## Files

```
ext-data-control-v1.xml                    # Protocol XML (from wayland-protocols)
ext-data-control-v1-client.h               # Generated client header
ext-data-control-v1-protocol.c             # Generated protocol glue code

wlr-data-control-unstable-v1.xml           # Protocol XML (from wlr-protocols)
wlr-data-control-unstable-v1-client.h      # Generated client header
wlr-data-control-unstable-v1-protocol.c    # Generated protocol glue code
```

## Regenerating

The C bindings are pre-generated with `wayland-scanner` and committed to the repository so that builds do not require `wayland-scanner` to be installed.

If you update the protocol XML files, regenerate the C bindings with:

```bash
wayland-scanner client-header ext-data-control-v1.xml ext-data-control-v1-client.h
wayland-scanner private-code  ext-data-control-v1.xml ext-data-control-v1-protocol.c
wayland-scanner client-header wlr-data-control-unstable-v1.xml wlr-data-control-unstable-v1-client.h
wayland-scanner private-code  wlr-data-control-unstable-v1.xml wlr-data-control-unstable-v1-protocol.c
```

`wayland-scanner` is provided by `libwayland-dev` (Debian/Ubuntu), `wayland-devel` (Fedora), or `wayland` (Arch).
