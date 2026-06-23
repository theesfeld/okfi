---
type: Reference
title: OKF format (parser subset)
description: The OKF v0.1 bundle structure fokf reads and how its frontmatter parser behaves.
resource: https://raw.githubusercontent.com/GoogleCloudPlatform/knowledge-catalog/main/okf/SPEC.md
tags: [okf, spec, parser]
timestamp: 2026-06-22T00:00:00Z
---
# What fokf treats as a bundle

A **bundle** is a directory tree of UTF-8 markdown files. Each non-reserved `.md`
is one **concept**. Reserved filenames carry special meaning and are never concepts:

- `index.md` — directory listing for its level. No frontmatter, **except** the
  bundle-root `index.md`, which MAY carry `okf_version`.
- `log.md` — newest-first change history; `## YYYY-MM-DD` headings.

# Frontmatter fokf parses

A concept file opens and closes its frontmatter with a line containing exactly `---`.
Between those fences fokf reads a **flat `key: value` subset** of YAML — enough for OKF,
not a general YAML engine:

- Scalars: `key: value` (value trimmed; surrounding quotes stripped).
- Inline lists: `tags: [a, b, c]` → split on commas.
- Block lists:
  ```
  tags:
    - a
    - b
  ```

Recognized keys (§4.1): `type` (the only required field; must be non-empty), then
`title`, `description`, `resource`, `tags`, `timestamp`. Unknown keys are **preserved
and shown**, never dropped (§4.1 extension rule).

# Conformance fokf assumes of inputs

Per §9, a conformant bundle has a parseable frontmatter block with a non-empty `type`
on every non-reserved `.md`. fokf is a **consumer**: it MUST tolerate broken cross-links,
unknown `type` values, unknown extra keys, and missing optional fields — it displays what
is present rather than rejecting the bundle.

# Cross-links

Links are markdown. Absolute links are bundle-root relative and begin with `/`
(e.g. `/tables/customers.md`); relative links resolve against the current file's
directory. The relationship is conveyed by prose, not by link type.
