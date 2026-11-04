# Build specs

Specs for work not yet built: each is self-contained (context, scope, the files it touches, a
phased plan, acceptance and known gotchas). [CONVENTIONS.md](../Process/CONVENTIONS.md) is binding
for every one.

## Index

| Spec | Size | State |
|---|---|---|
| [2d-games.md](2d-games.md) | L | PROPOSED: 2D inside the 3D world, L0 to L7 |
| [plugins.md](plugins.md) | L | APPROVED 2026-11-03: source plugins for every host and platform, X0 to X6 |
| [whiteboxing.md](whiteboxing.md) | M | PROPOSED and reviewed 2026-09-23: parametric blockout pieces, P0 to P3 |
| [script-run-modules.md](script-run-modules.md) | M (L with P2) | PROPOSED 2026-09-26: one script module per run, hot reload that migrates |

A spec moves to `../Archive/<name>-history.md` once it is built (or made obsolete), with a banner
saying so, and leaves this index; whatever it still owes goes to the backlogs in `../Plans/`
(`weekly_backlog.md`, or `backlog-0.1.md` / `backlog-0.2.md` for a release). The sweep of
2026-11-02 archived every built spec of the time and put their leftovers under "Left over from the
specs archived 2026-11-02" in `weekly_backlog.md`.

Sizes: S = a session or less, M = a few sessions, L = a multi-session track landing in phases,
green after each.
