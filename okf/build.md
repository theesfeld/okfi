---
type: Playbook
title: Build & run
description: How to compile fokf and open an OKF bundle.
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
./fokf                          # browse all bundles found under your search roots
./fokf <bundle-dir>             # open one bundle directly (skips the picker)
./fokf --root DIR               # add a search root for this run (repeatable)
./fokf --mono                   # force the monochrome interface (alias: --no-color)
./fokf --new-bundle DIR         # scaffold a new bundle (index.md + log.md)
./fokf --new-concept B NAME [T] # scaffold a concept of type T in bundle B
./fokf --export-pdf <c.md>      # export one concept to ./<name>.pdf (milstd milmanual)
./fokf --selftest               # run the built-in self-check; exits 0 on success
```

Discovery, theme, and per-color options live in an XDG config — see
[Configuration](/config.md). Color is automatic on a capable terminal; `NO_COLOR` or
`--mono` forces monochrome. See [Bundle discovery](/discovery.md), the
[editor](/editor.md), [creating](/create.md), and [PDF export](/pdf-export.md).

See [TUI viewer](/tui-viewer.md) for key bindings and styling.
