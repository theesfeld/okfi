---
type: Component
title: In-TUI editor
description: fokf's built-in concept editor — keys, codepoint handling, and atomic save.
tags: [editor, edit, ui]
timestamp: 2026-06-22T22:00:00Z
---
# Invocation

In the [browser](/tui-viewer.md), `e` opens the selected concept in fokf's own editor.
On exit fokf reloads the bundle so the list and body reflect the change.

# Keys

| Key | Action |
|---|---|
| arrows / `Home` / `End` | move (by codepoint; line-wrapping at ends) |
| typing | insert (UTF-8 input assembled from its continuation bytes) |
| `Backspace` / `Del` | delete the codepoint before / at the cursor; join lines at edges |
| `Enter` | split the line |
| `^O` (or `^S`) | save and exit |
| `^X` / `Esc` | cancel (prompts to discard if modified) |

`^O`/`^X` are used because terminal XON/XOFF flow control can swallow `^S`; fokf also
disables `IXON` so `^S` works where the terminal allows it.

# Floors

- **Codepoint-aware, not byte-aware.** Cursor motion and delete operate on whole UTF-8
  codepoints (`u8_prev`/`u8_next`/`u8_snap`), so editing an em-dash or `•` never leaves a
  broken sequence on disk.
- **Atomic save.** `write_atomic` writes `<path>.fokftmp`, checks every write and the
  `fclose`, then `rename()`s over the original — a crash or full disk leaves the old file
  intact, never a truncated catalog. If the save fails (read-only dir, full disk), the
  editor **keeps the buffer**, shows the error, and does not exit — no silent data loss.
- **Lossless round trip.** A file ending in `\n` is not grown by a blank line each
  open→save; the trailing-newline artifact is dropped on load.

# Scope

A minimal editor: no undo, no search, no syntax highlight. It edits the raw markdown of a
single concept file. ponytail: enough to fix a typo or add a paragraph in place; reach for
your full editor for large rewrites.
