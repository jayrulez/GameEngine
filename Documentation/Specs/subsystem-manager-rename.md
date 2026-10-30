# The subsystem manager's name

Status: APPROVED and built 2026-10-30 (user: "we do it, SubsystemManager and Subsystems()", now,
before 0.1, while such a change is still cheap). Split out of `Specs/run-context.md`, where it was
that spec's P0; no longer its prerequisite (the run is `run::Run`).

Decisions: rename (question 1), to `SubsystemManager` with `Ctx()` -> `Subsystems()` (2), alone
and now (3). As built: the type, its partition (`:subsystem_manager`) and file
(`SubsystemManager.cppm`); `Subsystem::Manager()`; `IApplicationHost::Subsystems()`; the plugin
interface and `PluginHost`; the hosts' members (`m_subsystems`). Variables that still say `ctx` or
`context` for a manager were left as they are. Older specs keep the old names as written.

## The question

`foundation::runtime::Context` (`Code/Foundation/Runtime/RuntimeContext.cppm`) is a subsystem
manager: it registers subsystems by stable type id, finds them, and drives their lifecycle and
frame phases. It carries two application-wide settings besides (`TimeScale`, the outermost term
of the time model, and `FixedTimeStep`, which scenes copy). There is one per process, and one
per embedded runtime (the editor's `EmbeddedHost`). Its name says nothing of that, and
"context" is used elsewhere for other things: the script context (`IScriptContext`), render and
GUI contexts.

Should it be renamed `runtime::SubsystemManager`?

## What the rename would be

- `runtime::Context` -> `runtime::SubsystemManager`; the module partition `:context` and its
  file `RuntimeContext.cppm` to match. `AddSubsystem` keeps its allocator.
- `Subsystem::GetContext()` -> `Subsystem::Manager()`; `IApplicationHost::Ctx()` ->
  `IApplicationHost::Subsystems()`; the plugin interface and `PluginHost` take a
  `SubsystemManager&`.
- Comments that say "the context's time scale" -> "the application's time scale".
- Mechanical, one commit (no half-renamed tree builds), no behaviour change. About 34 files name
  `runtime::Context` and 37 call `Ctx()`. Other `Context` names are not touched.
- Tests: the full suite, unchanged in count. Acceptance: no `runtime::Context`, `Ctx()` or
  subsystem `GetContext()` remains.

## For and against

- For: the name says what it is; "context" stops meaning three things; Sedulous's draft wanted
  the same rename, so both engines would agree.
- Against: churn across about 70 files and every sample, tool and extension that names it, with
  no behaviour gained; `Ctx()` is short and everywhere; once the run is `run::Run`, nothing
  forces it.

## Open questions

1. Rename at all, or keep `Context`?
2. If renamed: `SubsystemManager`, or another name (`Subsystems`, `Application`)? And
   `Ctx()` -> `Subsystems()`?
3. When: alone, or folded into the run work's P2 when those files are touched anyway?
