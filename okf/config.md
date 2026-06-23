---
type: Reference
title: Configuration & settings
description: fokf's XDG config file, every option, and the in-program settings screen.
tags: [config, settings, xdg, colors, theme]
timestamp: 2026-06-22T22:00:00Z
---
# Location

`$XDG_CONFIG_HOME/fokf/config` (falling back to `~/.config/fokf/config`). Plain text,
`key = value`, one per line; `#` comments and blank lines ignored. fokf **writes it on
every in-program change** and you may also edit it by hand. Unknown keys are preserved
verbatim on rewrite (liberal-consumer rule, like OKF frontmatter). On first run with no
config, fokf seeds sensible search roots (the parent of the cwd, and `~/Projects` if it
exists) and saves the file.

# Options

| Key | Meaning |
|---|---|
| `root = PATH` | A search root for [discovery](/discovery.md). Repeatable. |
| `theme = default \| bbs \| mono` | Base palette. `bbs` is the vivid old-school skin; `mono` forces attribute-only. |
| `editor = internal \| system` | `internal` uses fokf's [built-in editor](/editor.md); `system` opens `$VISUAL`/`$EDITOR` (default `vi`). |
| `group_order = type \| count \| priority` | Tree group order: alphabetical, by descending concept count, or by `group_priority`. `reserved` is always last. |
| `group_priority = A,B,C` | For `group_order = priority`: the type names to list first, in order; others follow alphabetically. |
| `fold = BUNDLE⇥GROUP` | A collapsed group, keyed by bundle path (tab-separated). Written automatically when you fold a group; persists collapse state across runs. |
| `color.<role> = FG[,BG]` | Override one style's color (0–255 terminal indices, or `default`/`-1`). Roles: `head bold ital code link codeblk key bar tag`. A set override beats the theme. |

Example:

```
root = /home/me/Projects
root = /home/me/work
theme = bbs
color.head = 51
color.bar = 231,21
```

# Settings screen

`,` (from the picker or browser) opens settings: cycle the theme, toggle the editor
(internal/system), cycle the group order (type/count/priority), add a search root (`Enter`
on `[ add search root ]`), delete a root (`d`), or set any `color.<role>` (`Enter` → type
`fg` or `fg,bg`). Every change re-applies live and writes the config immediately.
`Esc`/`q` saves and returns. Selecting the `priority` group order prompts for the
comma-separated type list right there (also editable in the config file).
