# Integration - cross-collection flow tests

High-level tests whose SUBJECT is a flow crossing collections, not a single
module's contract. First-party code (lives in Code/ for sweep safety); same
doctest + battery infrastructure as every module suite, both compilers.

## The admission rule

- A test that exercises ONE module's contract belongs in that module's
  flat-sibling `.Tests` target - colocation is why the tests-required rule
  works, and this folder must never erode it.
- A test whose subject is a FLOW crossing collections (pipeline -> runtime,
  editor -> player, protocol -> pipeline) belongs here.
- Litmus: if you can name the single module under test, it is not
  integration.
- Heaviness is NOT the criterion: GPU-device probe suites (VG.Backend.Tests,
  Render.Backend.Tests) are single-subject contract tests that happen to
  need hardware - they stay with their modules.

## Layout

Targets by flow-family, mirroring the collection conventions
(folder == target; `Integration.<Flow>`):

- `Integration.Mcp` - the MCP golden sequences (agent-shaped tool-call flows
  against a fixture project, headless).
- `Integration.Pipeline` - import -> cook -> export -> player-load
  round-trips. (Created when its first test migrates or lands.)
- `Integration.Runtime` - boot flow, scene/script end-to-end scenarios
  (Roll Call-class). (Same.)
- `Integration.GuiScript` - the GUI (foundation.gui) and its satellites driven
  from the script backends (the GUI's widgets, the gamekit's screen stack, the
  engine's GUI).

New families follow the same shape. No big-bang migrations: existing
cross-collection tests move here opportunistically when next touched.

## Battery tiers

The `Integration.` target prefix is the tier switch: the fast dev loop runs
module suites only; review gates and pre-push runs include Integration.*.
