---
type: Playbook
title: Build & run
description: How to compile okfi and open an OKF bundle.
tags: [build, make, ncurses]
timestamp: 2026-06-22T22:00:00Z
---
# Dependencies

- A C11 compiler (`cc`/`gcc`) and POSIX (`fork`/`exec`, `mkdtemp`, `open_memstream`).
- `ncursesw` (wide-character ncurses). Detected via `pkg-config --cflags --libs ncursesw`.
- **For `--export-pdf` only:** `pdflatex` and the `milstd` LaTeX kit at `~/.config/milstd/`.
  Not needed to build or view — only to export.

# Build

```sh
make
```

# Run

```sh
./okfi                          # browse all bundles found under your search roots
./okfi <bundle-dir>             # open one bundle directly (skips the picker)
./okfi --root DIR               # add a search root for this run (repeatable)
./okfi --mono                   # force the monochrome interface (alias: --no-color)
./okfi --new-bundle DIR         # scaffold a new bundle (index.md + log.md)
./okfi --new-concept B NAME [T] # scaffold a concept of type T in bundle B
./okfi --export-pdf <c.md>      # export one concept to ./<name>.pdf (milstd milmanual)
./okfi --selftest               # run the built-in self-check; exits 0 on success
```

Discovery, theme, and per-color options live in an XDG config — see
[Configuration](/config.md). Color is automatic on a capable terminal; `NO_COLOR` or
`--mono` forces monochrome. See [Bundle discovery](/discovery.md), the
[editor](/editor.md), [creating](/create.md), and [PDF export](/pdf-export.md).

See [TUI viewer](/tui-viewer.md) for key bindings and styling.
