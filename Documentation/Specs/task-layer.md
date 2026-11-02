# Tasks: long work off the frame, handed back on the main thread

Status: APPROVED 2026-11-02, the open questions decided (user): the names as proposed; the
resource manager's decodes are part of this work (T6); the job service's users move to the pool
directly, the service going. Designed on request (user: "design the task layer first, survey the
references"). Started
from a freeze: the import window's model preview cooks the whole model on the main thread, which
holds the editor for as long as a model like Bistro takes. The fix wanted (user 2026-11-02) is a
cook on a worker that Import can cancel; doing it well needs a cancellation token, and the token
belongs to a layer the engine does not have yet.

## The problem

The engine has a job system (`Core/Threading/JobSystem.cppm`): a work-stealing pool for frame
work, with `Submit`, `SubmitAfter` (counter dependencies), `ParallelFor` and a `Wait` that runs
jobs instead of blocking. One global pool (`GlobalJobs()`) serves the renderer's fan-out. It knows
nothing of results, the main thread, cancelling or progress, and long work on it holds workers the
frame's `ParallelFor` needs.

Long work, everywhere it happens, builds the rest itself:

| Where | Runs on | Handed back by | Cancel | Progress |
|---|---|---|---|---|
| `ImportPreviewReader` (Editor.Core) | a private 1-worker JobSystem | `Pump` in the editor's frame | a flag on the ticket | no |
| `EditorJobService`, build lane (Editor.Project): import reads and writes, project operations, export, native build | its own thread, one job at a time | `Update`, `onDone(Status)` | `CancelActive`, polled | fraction, step, log |
| `EditorJobService`, light lane: thumbnails, font page bakes | its own thread | `Update`, `onDone` | no | no |
| `CookService` (Editor.Project) | a private JobSystem | through the job service | | |
| HTTP client (Foundation) | its own thread | `Pump` deliveries | a flag under a mutex | a streamed body |
| `FileLoads` (ModelViewer) | its own thread | `Take()`, polled | a generation counter | no |
| `ResourceManager` async loads | the global frame pool | `Pump` finalizes | no | no |

Six versions of one shape: run something long away from the frame, hand its result back on the
main thread, maybe cancel it, maybe show how far it got. None can hand its cancel to code that does
not know it (the cook in the pipeline cannot be stopped by the window that started it), and the
resource manager's decodes compete with the frame for the same workers.

The build lane runs one job at a time on purpose: a cook must never race the content databases.
That rule is real and the layer keeps it.

## What the references do (read 2026-11-02)

- **ezEngine** (`Foundation/Threading`): separate thread pools by kind. Short-task threads take
  only frame priorities (this frame, next frame, within N frames), long-task threads only
  `LongRunning`, one thread for `FileAccess` so reads never run in parallel, and the main thread
  its own two priorities. Results reach the main thread as a dependent group at a main-thread
  priority, run within a frame-time budget (25 ms by default). Cancel removes a queued task (its
  dependents still run) and returns failure for a running one, optionally waiting for it; running
  work polls `HasBeenCanceled()`. Progress (`ezProgress`) is separate from tasks, joined by hand.
  Heavy asset processing runs in separate processes, so a crash or a stall stays out of the editor.
- **Godot** (`WorkerThreadPool`): one pool, two queues; low-priority work (loads, reimports) may
  hold at most a share of the threads, always leaving one for the rest. Results come back through
  deferred calls drained by the main loop, or by polling. The pool has no cancel; the progress
  dialog's Cancel is only seen by code that checks it. Progress on the main thread re-enters the
  main loop, which needs a guard against re-entry.
- **Lumix** (`jobs`): fibers, no priorities, asset compiles on the frame's workers; file reads on a
  thread of their own. The main thread drains finished reads within 0.1 s a frame. A cancelled
  read is dropped when it finishes; a compile is superseded by a newer one (a generation number),
  never stopped. Jobs capture their owner raw and nothing waits for them when it goes.
- **traktor**: fork/join jobs for the frame; asset builds on a thread of their own, polled by a UI
  timer; cancel by a stop flag checked between assets. Its job cancel writes the job's function
  unsynchronised, a race.
- **Zero**: one pool for everything; editor tasks with states (not started, running, completed,
  failed, cancelled) changed only on the main thread from events the worker posts, which skip a
  target already gone. Cancel is a virtual each task overrides to abort its real work (a download
  cancels its request).
- **doriax**: one pool returning futures; the editor's main-thread inbox is swap-drained at the top
  of the frame, with a wake for an idle loop. Owners cancel and then wait in their destructors.

What they agree on: long work kept from the frame's threads (ezEngine, Godot); results drained on
the main thread at a fixed point; cancelling cooperative, a queued task dropped, a running one
told and left to notice; the main thread never touched from a worker.

## The design

### D1. A cancel token in Core

`CancelSource` is held by whoever may cancel and has `Cancel()`. `CancelToken` is a cheap copy
handed to the work, which asks `IsCancelled()`. Both share a small reference-counted state from the
owner's allocator, so a token outlives its source safely. A default token never cancels, so code
with nothing to cancel passes nothing. Not `std::stop_token`: its state comes from global `new`.
`ErrorCode::Cancelled` is what cancelled work returns.

### D2. A task pool beside the job system, for long work

`TaskPool` (Core, Threading) runs long work on workers of its own, never the frame's. A task is
`Function<Status(TaskContext&)>` with a name; submitting returns a `TaskHandle`
(reference-counted): its state (queued, running, done, failed, cancelled), its progress, and
`Cancel()`. The job system stays as it is, for frame fan-out.

Worker count: the logical cores less two, at least one and at most four (ezEngine and Godot both
leave the frame its threads; four keeps a big cook from flooding the disk and memory). The owner
may choose.

### D3. Lanes: one at a time where it must be

A task goes to a lane. Tasks in one lane run one at a time, in the order submitted; different
lanes run side by side. A lane is a queue over the pool's workers, not a thread of its own. The
editor's build lane (cooks, imports, exports, project operations) is one lane, which keeps the rule
that nothing races the content databases; previews, thumbnails and bakes go to a lane each, or to
the pool's shared lane, which runs tasks side by side.

### D4. Handed back on the main thread, at one point in the frame

A task may carry a completion, `Function<void(Status)>`. The pool's owner calls `Pump()` once a
frame on the main thread; it runs the completions of the tasks finished since, in finishing order,
and drains their log lines. A cancelled task's completion does not run, so an owner that cancels
on its way out is never called back afterwards. A completion may submit more tasks.

### D5. Cancelling

`TaskHandle::Cancel()` cancels the task's token. A queued task is dropped without running. A
running task sees `IsCancelled()` at its next check and returns `ErrorCode::Cancelled`. Cancel does
not wait; `TaskPool::Wait(handle)` waits on the main thread for a task to end, for the one case
where the main thread must have it gone before it goes on (Import, after cancelling the preview's
cook, waits for the item under way). Work that blocks outside the engine (a socket, a child
process) needs more than a flag; the task may register a callback that `Cancel()` runs (Zero's
download, doriax's process).

### D6. Progress and log

`TaskContext` gives the work its token, `SetProgress(fraction, step)` and `Log(line)`, all safe
from the worker. The main thread reads a handle's progress as a snapshot; the log lines come out
in `Pump`. Nothing from a worker touches the GUI.

### D7. Ownership

The work owns what it uses: it captures by reference count, never a raw owner. An owner that goes
away cancels its tasks, and its completions then never run. The pool's destructor cancels
everything queued and waits for the tasks under way.

### D8. The web build: no workers

The web build has no threads (`Core/CMakeLists.txt`). A pool of no workers runs queued tasks in
`Pump` itself, one task at a time within a budget, so the page keeps drawing between tasks; a long
single task still holds it while it runs, as it does today.

## Not taken

- Frame-deadline priorities (ezEngine's this frame, next frame, within N frames): frame work is
  the job system's, and the counters already order it.
- A separate process for heavy processing (ezEngine's asset processors): worth it for crashes, but
  a step of its own after this.
- Rerunning a task submitted while it runs (Zero), and superseding by generation (Lumix): a lane's
  owner can cancel the old task and submit the new one.

## Phases

Each its own commit with its tests; each move keeps its users' behaviour.

- **T0.** Core: `CancelSource`, `CancelToken`, `ErrorCode::Cancelled`.
- **T1.** Core: `TaskPool`, `TaskHandle`, `TaskContext`, lanes, `Pump`, `Wait`, the no-worker mode.
- **T2.** The editor owns one pool and pumps it in its frame. The import previews move onto it
  (`ImportPreviewReader` goes; `ImportPreviewRead` becomes a `TaskHandle`).
- **T3.** The freeze: `CookModel` takes a `CancelToken` and checks it between textures, clips and
  meshes; the model preview cooks on the pool and binds in its completion; Import cancels the
  preview's cook and waits for the item under way. `CookModel` stops writing the model's bounds
  (a const model it casts to write; the loaders compute them already).
- **T4.** `EditorJobService` goes: its users submit to the editor's pool directly, the build
  lane's work to the build lane, the light lane's to the shared lane; whatever shows the running
  job's progress reads the build lane's task. `CookService`'s private pool goes too.
- **T5.** The model viewer's `FileLoads` onto a pool of its own.
- **T6.** The resource manager's async decodes off the frame pool and onto a task pool, its
  finalize staying in its `Pump`; the application host owns that pool beside the job system.
- The HTTP client keeps its thread: it blocks in sockets, which a task worker should not.

## Decided

1. The names: `TaskPool`, `TaskHandle`, `TaskContext`, `CancelSource`, `CancelToken`.
2. T6 (resource decodes) is part of this work.
3. `EditorJobService` goes; its users move to the pool directly.
