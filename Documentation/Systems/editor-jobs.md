# Editor background work: the editor's task pool and its lanes

> Status: CURRENT
> Verified: 2026-11-02
> Track: [[editor-track]]

The editor's long work runs on one `TaskPool` (`Code/Foundation/Core/Threading/TaskPool.cppm`, the task
layer of `Documentation/Specs/task-layer.md`), owned by the `EditorContext` (`Context().Tasks()`) and
pumped once a frame by the application, which runs the completions and sends the tasks' log lines to the
status bar. It is THE structural way editor code gets work off the UI thread. The context makes two lanes
on it; the pool's shared lane runs the rest side by side.

Submitting: `Context().Tasks().Submit(name, work, done, lane)`. `work` is `Status(TaskContext&)` and runs
on a worker; `done(Status)` runs on the MAIN thread in the pool's Pump, never for a cancelled task. The
returned `TaskHandle` gives the task's state and progress and `Cancel()`.

## Build lane - `Context().BuildLane()`

For work that touches the content databases or the filesystem the cook reads (export, an import's reads
and writes, the project operations the MCP tools run, the native build):
- One at a time, in the order submitted: the editor build lock, so a cook, an export or an import never
  races the databases.
- `Tasks().IsLaneBusy(BuildLane())` while one runs or waits, folded into the cook service's
  `ExternalMutationLock`, so database mutations and new cooks hold off until the lane is idle.
- Progress through `TaskContext` (`SetFraction`, `SetStep(label, index, count)`, `Log`), which the status
  bar reads from `Tasks().RunningIn(BuildLane())`.

## Light lane - `Context().LightLane()`

For short CPU-only side work whose results feed the UI (thumbnails and their files, the font page's
preview bake):
- One at a time, as it always ran, so no generator or bake runs beside another; never behind the build
  lane, and outside the cook's lock (a preview must never hold the build).

## Rules

1. **Never let a completion reach a closed owner.** Keep the `TaskHandle` and `Cancel()` it as the owner
   goes: a cancelled task's completion never runs, so the completion may use the owner freely. Whatever
   the work writes and the completion reads goes in a reference-counted slot both closures hold, freed
   with them whether the task ran or was dropped.
2. **Latest-wins for re-entrant requests.** If a new request arrives while one is in flight, stash it (one
   pending slot, overwritten freely) and submit it from the completion; tag outcomes with a generation
   and drop stale ones. Do NOT submit every edit: the lane would replay stale work.
3. **The work closure is self-contained**: it touches only what it captured and its slot. No UI objects,
   no databases outside the build lane, no owner members. Anything registry-like it needs (such as
   `DistanceFieldFonts::Initialize()`) is initialized on the main thread BEFORE submitting.
4. **Closing the project cancels the pool's work** (`EditorSession::Close`: `CancelAll`, then `Finish`),
   so nothing reads a project that is going; long work should check `IsCancelled()` between its steps.
5. **Tests** run the pool themselves: `Tasks().Finish()` waits for every task and runs the completions.

## Reference implementation

`FontEditorPage` (`Code/Editor/Editor.Fonts/`): `CaptureBakeRequest()` snapshots the asset options on the
UI thread, `RunBake(request, outcome)` is the pure work, `StartBake` submits it to the light lane with a
reference-counted `BakeSlot` and keeps the handle (cancelled as the page closes), and `ApplyBakeOutcome`
applies on the main thread with a generation check. The old preview stays on screen until its
replacement lands.
