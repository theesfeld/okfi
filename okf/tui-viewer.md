---
type: Component
title: TUI viewer
description: The ncurses front-end of fokf — a two-pane bundle browser with keyboard navigation.
tags: [tui, ncurses, ui]
timestamp: 2026-06-22T23:00:00Z
---
# Layout

A two-pane split:

- **Left pane** — a tree of the open bundle's concepts **grouped by `type`**: a header per
  type showing a fold marker and a count (`▾ Reference (3)`), concepts indented under it.
  `Space`/`Enter` on a header **collapses/expands** the group (`*` folds/unfolds all); a
  header's content pane shows a folder summary. Collapse state **persists across runs**
  (saved per-bundle in the config). Group order is configurable (type / count / priority,
  see [config](/config.md)). Reserved files (`index.md`, `log.md`) form a `reserved` group,
  always last.
- The two panes sit in a **frame**; the **focused** pane's border is brightened and its
  title (`TREE` / `CONTENT`) highlighted.
- **Right pane** — the selected concept's frontmatter (keys shown styled, values plain)
  followed by its body, word-wrapped and markdown-styled.

A top bar shows the bundle root path; a bottom bar shows key hints.

# Markdown styling

The body is word-wrapped and styled inline; markup characters are stripped:

- ATX headings (`#`..`######`) → heading style.
- `` `code` `` inline spans, `**bold**`, `*italic*` / `_italic_`, and `` `[text](url)` ``
  (link text shown, URL hidden). Spans nest — e.g. a code span inside bold, or bold
  inside a link — layering their styles; code-span contents stay literal.
- ` ``` ` fenced code blocks → dim/plain, fence lines hidden, inline markup left literal.
- `-` / `*` / `+` bullet lists → `•`.
- Blockquotes (`> `) → italic.
- Pipe tables (a `|` row followed by a `|---|` separator) → aligned columns with a bold
  header and a box-drawing rule; cell contents are themselves inline-styled.

Consecutive non-blank lines are reflowed into one logical paragraph (bullets and
blockquotes likewise absorb their continuation lines) before styling, so emphasis that
spans a soft line break in the source still renders. A blank line separates paragraphs.

Unclosed delimiters are shown literally (best-effort, like a pager). Table columns honor
the separator's alignment colons — `:--` left, `--:` right, `:--:` center. A table wider
than the pane is clipped at the right edge — there is no horizontal scroll yet.

# Cross-links

Markdown links in the body whose target resolves to a concept in the bundle (absolute
`/path.md` or current-dir-relative `./x.md` / `x.md`) are collected into the bottom bar as
`[1] title`, `[2] title`, … (up to 9). Press the matching digit key to jump to that
concept. Links that resolve to nothing are still shown styled in the body but are not
listed as followable.

# Color

On a color-capable terminal fokf draws a color interface (a 256-color palette when
`COLORS >= 256`, otherwise the 8/16-color set), themed and per-color configurable — see
[Configuration](/config.md). It falls back to monochrome when color is unavailable, when
`NO_COLOR` is set, or with `--mono` / `--no-color`.

# Navigation

The browser is one view of fokf: it opens from the bundle [picker](/discovery.md) (or
directly via `fokf <dir>`). A menu bar runs across the top.

Two panes have **focus**: the tree (left) and the content (right). `l`/`→` focuses the
content so you can scroll it; `h`/`←` returns to the tree. The menu bar shows which pane
has focus.

| Key | Action |
|-----|--------|
| `j` / `↓`, `k` / `↑` | move within the focused pane (tree row, or scroll body) |
| `l` / `→`, `h` / `←` | focus the content pane / the tree |
| `Space` / `Enter` | collapse/expand a group header, or open a concept |
| `*` | collapse all groups / expand all |
| `g` / `G` | first / last in the focused pane |
| `J` / `K`, `PgDn` / `PgUp` | scroll the body from either pane |
| `1`..`9` | follow the n-th body link (see Cross-links) |
| `e` | [edit](/editor.md) this concept |
| `n` | [new concept](/create.md) in this bundle |
| `Tab` | back to the bundle picker |
| `,` | [settings](/config.md)    `?` help |
| `q` | quit fokf |

# Scope

The browser is read-only; mutation happens through the [editor](/editor.md) (`e`) and the
[scaffolders](/create.md) (`n`, picker `N`). See [OKF format](/okf-format.md) for what the
parser accepts and [Build & run](/build.md) to compile and run.
