---
type: Playbook
title: Creating bundles & concepts
description: Scaffold a new OKF bundle or concept from okfi, in-program or headless.
tags: [create, scaffold, new]
timestamp: 2026-06-22T22:00:00Z
---
# New bundle

Seeds a conformant bundle root: `index.md` carrying `okf_version: "0.1"` and a `log.md`
with a dated creation entry. Existing files are never overwritten.

- TUI: `N` in the [picker](/discovery.md) → type the directory (a leading `~/` expands).
- CLI: `okfi --new-bundle <dir>`.

# New concept

Writes a skeleton concept with a non-empty `type` (the one required OKF field), a `title`
derived from the filename, an empty `description`/`tags`, and a dated `timestamp`, then
prepends a newest-first entry to the bundle's `log.md` (§7 structure). Parent directories
are created as needed; reserved names (`index.md`, `log.md`) are refused, and an existing
file is never clobbered.

- TUI: `n` in the [browser](/tui-viewer.md) → concept path (e.g. `tables/orders.md`),
  then a type.
- CLI: `okfi --new-concept <bundle> <name> [type]` (type defaults to `Concept`).

# Note on log.md

Scaffolding writes `log.md`; ordinary edits via the [editor](/editor.md) do **not** touch
it. Keeping log updates an explicit action avoids cluttering or corrupting the reserved
change-history file on every save.
