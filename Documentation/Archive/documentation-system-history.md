# Documentation system: audit, reorganize, and START TRACKING docs

> ARCHIVED 2026-11-02, fully built; anything it left open is in
> Documentation/Plans/weekly_backlog.md ("Left over from the specs archived 2026-11-02"). Non-
> authoritative: the original spec, kept as the record of what was built and why; the code and the
> tests are the truth.

**Status:** SPEC, approved direction (user, 2026-08-11). Opus executes; Fable
reviews per batch. This REVERSES the docs-never-committed policy - deliberately
and in phases, because 71 docs in docs/ (42 design, 24 specs, 5 loose) plus
root handoffs are in varying states of truth, and months of decisions live
only in them. The risk being fixed: losing information, and docs that LIE
about reality (this week's incidents showed what drift costs).

## Target structure - `Documentation/` at the repo root

- `Documentation/Systems/` - how the engine IS. Per-subsystem reference
  (renderer, physics, scripting, fonts, UI, pipeline, MCP, ...). The ONLY
  category that claims present-tense truth, and every claim must be
  verified against code during the audit. One doc per subsystem, merged
  from however many partial docs exist today.
- `Documentation/Specs/` - ACTIVE build specs only (the Opus-workflow
  docs currently mid-track: luau-backend, mcp-agent-access, the three
  parity P0 specs, ...). A spec whose track COMPLETES moves to Archive.
- `Documentation/Plans/` - approved designs not yet started + future
  phases split out of completed tracks (e.g. deferred P2/P3 sections).
- (RETIRED 2026-09-01) `Documentation/Backlog/` - absorbed into the weekly docs; was deferred-by-design, queued leftovers,
  triage lists. Includes today's deferred-by-design.md and the
  per-track "remaining" sections that outlive their spec.
- `Documentation/Guides/` - how-to (adding-facades, emscripten-windows,
  smoke checklists, editor workflows).
- `Documentation/Process/` - the working agreements: CONVENTIONS.md,
  HANDOFF.md (review baselines), this migration's audit log.
- `Documentation/Archive/` - completed specs WITH their rulings intact,
  session handoffs, old reviews. Historical record, explicitly
  NON-AUTHORITATIVE (the header says so). Nothing is deleted if it has
  any archaeology value - pruning means MOVING here, deleting is only
  for content that is wrong AND worthless (each deletion gets one line
  in the audit log).
- `KNOWN_ISSUES.md` STAYS at the repo root (already tracked, working
  state, high visibility).

## The status header (every doc, enforced by review)

    > Status: CURRENT | DRAFT | ARCHIVED
    > Verified: 2026-08-11 @ <commit> (Systems docs only - the commit the
    >   claims were checked against)
    > Track: <memory slug / task # / "none">

Systems docs may only claim what was verified at the stamped commit;
unverified sections are explicitly marked "UNVERIFIED". An ARCHIVED header
names its superseding doc when one exists.

## Phases (each = one bounded Opus batch, committed + reviewed)

**P0 - track first, judge later (one session).** Create the tree; MOVE
every doc into its provisional category WITHOUT editing content (root
handoff-*.md and review-*.md go straight to Archive/); write
`Documentation/Process/audit-log.md` with the full inventory table (file,
old path, category, state guess: current/stale/unknown); fix .gitignore;
COMMIT EVERYTHING.

DISCOVERED MECHANICS (Fable, pre-flight census):
- docs/ was mostly UNTRACKED, not ignored - .gitignore names exactly FOUR
  doc files (docs/design/shaders-materials-hot-reload.md, runtime-host.md,
  renderer.md, and Documentation/Planning/Core.md). Remove those four
  entries; no blanket rule exists to remove.
- `Documentation/Planning/Core.md` ALREADY EXISTS (ignored) - a prior seed
  of this same idea. Inventory it like everything else; do not clobber the
  Documentation/ root when creating the tree, and ASK THE USER whether
  Core.md is living planning content (-> Plans/) or historical (-> Archive/)
  before moving it.

From the P0 commit on, nothing more can be lost and the audit itself has
history. The user's personal root notes (start.txt, net-demo-scripts.txt,
smoke-test-checklist.txt) are NOT touched.

**P1 - the audit, category by category (multiple sessions, priority
order).** For each doc: classify finally; for Systems content VERIFY the
claims against code (module names, APIs, paths - grep and read; the
spelling of reality, not memory of it); rewrite present-tense; split mixed
docs (a design doc that is half reference + half plan becomes a Systems
doc + a Plans doc); stamp the header; log every move/merge/deletion in the
audit log. Priority: Systems docs for ACTIVE subsystems first (scripting,
pipeline/MCP, fonts, renderer, editor), then Plans, Archive last
(archive needs only headers, not verification). Batch size: one subsystem
or ~8-10 docs per session, so each batch is reviewable.

**P2 - extraction + refresh.** Lift still-load-bearing RULINGS out of
archived specs into the owning Systems doc (the ruling's rationale stays
in Archive; the RULE lives where the next reader looks). Refresh
docs/design/roadmap.md into Documentation/Archive/roadmap-history.md against the
current tracker state (it is known-stale).

**P3 - policy mechanics.** CONVENTIONS.md gains the new rules: (1) docs
are code - committed, reviewed, same standards (ASCII, no em-dashes);
(2) a behavior change that invalidates a Systems doc updates that doc IN
THE SAME COMMIT (the anti-drift rule - review enforces it); (3) new
specs/plans are born in their category with the header. Update the
Claude memory rules that encode the old policy (Fable does this half).

## Rules that carry over from this week's lessons

- Truth is verified against real files/code, never assumed from the
  rename story (the legacy-name lesson, twice).
- No doc claims "done" for anything without naming how it was verified.
- The audit log is append-only; a reviewer can reconstruct every move.

## Acceptance (per phase)

P0: every former docs/ file reachable under Documentation/, git history
begins, inventory complete. P1 (per batch): headers stamped, Systems
claims verified with the verification named in the audit log, review pass
by Fable. P2: roadmap current; no archived spec holds the only copy of a
living rule. P3: conventions updated; the old-policy memory rules
replaced; one full review of the final tree.
