---
type: Feature
title: PDF export
description: Export a concept to a US-military-style PDF (milstd milmanual) via pdflatex.
tags: [pdf, latex, milstd, export]
timestamp: 2026-06-22T20:00:00Z
---
# Status

**Built (single concept).** `fokf --export-pdf <concept.md>` writes `./<name>.pdf`.

# Goal

Export an OKF concept to a typeset PDF, reusing the block/inline markdown model fokf
already parses for the [TUI viewer](/tui-viewer.md). One classifier
(`classify_line` / `table_block` / `split_cells`) drives both the screen renderer and the
LaTeX emitter, so markdown handling never diverges.

# Toolchain

Per the user's §8 directive, a personal project's PDF **documentation** uses the
**`milstd` military LaTeX kit** at `~/.config/milstd/` — not Tufte (§3), not Typst, not
Word. An OKF concept maps to `\documentclass{milmanual}` (the FM/TM technical-manual
instrument). The kit's `milserif.sty` already loads `hyperref`, so the export sets options
with `\hypersetup{hidelinks}` rather than re-loading it (a second load is an option clash);
it adds only `\usepackage{booktabs}`.

`pdflatex` and the milstd kit are an **optional runtime dependency** — required only for
export, not for viewing. The exporter sets
`TEXINPUTS=".:$XDG_CONFIG_HOME/milstd//:"` (falling back to `$HOME/.config`) so
`\documentclass{milmanual}` resolves, builds in a `mktemp` directory, runs `pdflatex`
twice (settling chapter-decimal page refs) via `fork`/`execlp` with no shell and
`-no-shell-escape`, copies the `.pdf` out, and removes the temp dir on success (keeping it
on failure and printing the first `pdflatex` errors).

# Markdown → milmanual mapping

| fokf block/inline | milmanual output |
|---|---|
| concept | `\makemilcover` (title, `type` as document number, timestamp) + one `\chapter` |
| frontmatter | a `booktabs` key/value table (URLs via `\url`) |
| `#` heading | `\section` (renders as `Section I.`, `Section II.`, …) |
| `##`+ heading | `\milpar{}` (numbered paragraph, `1-1.`, `1-2.`, …) |
| `**bold**` / `*italic*` | `\textbf{}` / `\emph{}` |
| `` `code` `` | `\texttt{}` (CommonMark backtick-run spans; hyphens and backticks escaped to avoid ligatures) |
| fenced code block | `lstlisting` (the `listings` package; tolerates arbitrary content) |
| `[text](url)` | `\href{url}{text}` |
| `-` / `*` / `+` bullets | `itemize` |
| `> ` blockquote | `quote` |
| pipe table | `booktabs` `tabular`, no vertical rules, alignment from the separator colons |

# Floors

- Every concept-controlled string is LaTeX-escaped (`& % $ # _ { } ~ ^ \` `` ` ``) in a
  single char-by-char pass — an input-validation boundary. Frontmatter URLs keep `\url{}`
  (so they line-break) only when free of break-out characters; otherwise they are escaped
  like any text. Self-checked via `open_memstream` in `--selftest`.
- `pdflatex` is launched with `fork`/`execlp` (no shell) and `-no-shell-escape`, so concept
  text cannot inject shell commands or `\write18`.
- The output `.pdf` copy checks short writes and `fclose`, unlinking a partial file on
  error; temp build dirs are removed on success (kept on `pdflatex` failure for debugging).
- **Known residual:** fenced code is emitted to `lstlisting`, which handles arbitrary
  content except a line literally containing `\end{lstlisting}` (it would close the
  environment early). Far rarer than the `verbatim` case it replaced; not yet guarded.

# Not yet built

Whole-bundle export (`milbook`, one chapter per concept), a TUI `p` key to export the
selected concept, and `..`-relative link rewriting in exported cross-links.

# Verification

`fokf --export-pdf okf/tui-viewer.md` compiles (heading hierarchy → Sections I–VII, the
Navigation pipe table → a booktabs table, links, bullets, and `--flag` code spans rendering
with literal double hyphens). A clean `pdflatex` run is itself the escaping test — an
unescaped special would abort the compile. See [Build & run](/build.md).
