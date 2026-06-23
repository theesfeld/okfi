---
type: Component
title: Bundle discovery
description: How okfi finds OKF bundles across projects, and the placement model it assumes.
tags: [discovery, bundles, picker, standard]
timestamp: 2026-06-22T22:00:00Z
---
# The placement model (and what the standard says)

The OKF v0.1 spec is **deliberately silent** on deployment: it does not mandate
one-bundle-per-project vs a central store, says nothing about discovery or
federation (an explicit non-goal, §1), and only notes a bundle MAY be "a subdirectory
within a larger repository" (§3). So okfi chooses a convention the spec permits:

- **Storage stays decentralized** — one `okf/` bundle per project repo (the user's §5
  convention; matches spec §3). The catalog travels with the code it documents.
- **Browsing is centralized** — okfi discovers many bundles under configured *search
  roots* and presents them in one picker, so you roam project → project without a
  central repo to drift from the code.

# Detection

A directory is treated as a **bundle root** when either:

1. its `index.md` contains `okf_version` — the spec's canonical bundle-root marker
   (only the root index carries it); or
2. it is named `okf` and contains at least one `.md` (loose fallback).

`discover()` walks each search root (see [config](/config.md)) up to depth 5, skipping
hidden dirs and `node_modules` / `target` / `build` / `dist`, and does **not** descend
into a directory once it is recognized as a bundle. Results are sorted by name; a bundle
named `okf` shows under its parent project's name.

# Picker

The top-level view lists discovered bundles (`name  path`). `Enter` opens one into the
[browser](/tui-viewer.md); `Tab` from the browser returns here. `N` scaffolds a
[new bundle](/create.md); `,` opens [settings](/config.md); `?` shows help. Running
`okfi <bundle-dir>` skips the picker and opens that bundle directly.
