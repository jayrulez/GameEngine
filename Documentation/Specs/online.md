# Online

> STATUS: PROPOSED 2026-11-04, for discussion. Sized L, in phases O0 to O6, green after each.
> Origin: the 0.1 release's last system (user, 2026-11-03): "Survey the reference engines", and
> "the point here is not to copy one reference or the other. It is to come up with a suitable API
> that fits our engine". Its first platform backend, Steam, is a plugin, which is why the plugin
> API (`Specs/plugins.md`) came first. Read CONVENTIONS.md first.

## Goal

A game reaches its platform's online services through one engine API, the same on every platform:
achievements, stats, leaderboards, cloud saves, rich presence and DLC. Which service answers is
the platform's: Steam on desktop when the project enables it, a Local backend everywhere else (the
web included) and in play in editor. A game written against the API runs, and behaves sensibly,
on a platform with none of those services.

Not goals: lobbies, matchmaking, voice and friends lists (later; the user ruled them out of 0.1),
purchases and stores, the platform's overlay UI, platforms other than Linux, Windows and the web
(the design leaves room for consoles and mobile; nothing for them is built here).

## Decided (user, 2026-11-03)

1. **0.1 scope**: achievements, stats, leaderboards, cloud saves, rich presence, DLC. Lobbies and
   voice later.
2. **The asynchronous shape is the `Http` facade's**: a call that can wait returns a request id and
   takes a handler; the handler runs on the game's thread; `cancel` drops it.
3. **Not a copy of a reference**: an API that fits this engine.
4. **Steam is a plugin**, the plugin API's first customer, on Linux64 and Win64.

## What exists (checked)

- **The `Http` facade** (`Code/Foundation/Http.Script/HttpScript.cppm`): static methods on an
  `Object` type (`get`, `post`, `send`, `cancel`), each returning an id (0 when the run has no HTTP,
  its handler then never called), a handler `IScriptDelegate` called on the game's thread from the
  binding's `Pump`. The run holds an `HttpScriptBinding` among its services
  (`GameInstance.cppm:340`, `Register<HttpScriptBinding>`); the facade finds it with
  `script::CurrentService<T>()`, and the client is made on the first request. `Reset` drops every
  request when the run's scripts stop.
- **Provider slots in the plugin manifest**: `PluginManifest::provides`
  (`Code/Foundation/Runtime/PluginManifest.cppm:222`, "named slots, at most one enabled provider
  each"), enforced by `PluginHost::Load`, first enabled wins, the other refused with both names
  (`PluginHost.cppm`, the slot check). The host tests already use `online.backend` with two
  providers.
- **Per-platform enablement** of a plugin (an entry's `platforms`, X3), its typed settings, its
  deploy files shipped beside the player (X6), and X7's rule: a platform service reaches scripts
  through an engine facade with per-platform providers, so a script never names a plugin's facade
  that a platform may lack.
- **The `Save` facade** (`Code/Engine/Engine.GameInstance/RunSave.cppm`, `Documentation/Shipping/
  Scripting.md`, "Saving"): typed values by key in one settings file the host names (the player
  in the user data directory, play in editor in the project's `Editor/` folder), written whole and
  atomically when the run ends or at `Save::flush()`. The web player mounts IndexedDB over the
  user data directory (`Engine.Player/WebMain.cpp:93`), so a file there persists in the browser.
- **The run's services** (`foundation.run`): per-run state a facade finds by type, app-wide state
  on the subsystem manager (`SubsystemManager::RegisterService`).

## Prior art (surveyed 2026-11-03, read again 2026-11-04)

- **Traktor** (`code/Online`): one `ISessionManager` over a provider (Steam, GameCenter, LAN, a
  SQLite Local), chosen at run time by class name from a setting, a per-provider configuration
  asset holding the app id and every achievement, stat, leaderboard and DLC id. Calls return a
  `Result` completed on one of two worker threads (Steam's callbacks are pumped there), its
  deferred function run on that worker; the getters block the calling thread until the first
  enumeration is done. Stats are int32 only. A failed Steam initialisation fails the application's
  start.
- **Flax** (`Source/Engine/Online`, `IOnlinePlatform`, and its MIT Steam plugin): every call is
  synchronous, returning true for failure; Steam's call results are waited on by spinning the game
  thread. Stats are float only; no DLC; rich presence is one status string. A failed Steam
  initialisation leaves the platform null, and every caller must check. The plugin commits the
  Steamworks headers and binaries.
- **LunarSong** (`PlatformServices`): a versioned C table of function pointers with capability
  bits (accounts, local and cloud saves, achievements, entitlements, platform UI, offline mode) and
  result codes (Ok, Unsupported, NotInitialized, InvalidArgument, NotFound, IOError, ProviderError,
  FileTooLarge); a missing optional function is Unsupported. Synchronous, Local only, chosen at
  compile time. No stats, leaderboards or presence.

What to keep: LunarSong's capabilities and result codes (a game asks what is supported instead of
what the backend is, and an unsupported call says so instead of failing silently); Traktor's
configuration of ids per game. What to avoid: every one blocks a thread or completes on a worker,
and none has a defined behaviour for a missing platform that a game can rely on.

**Steamworks** (facts from Valve's documentation, 2026-11-04):
- The SDK access agreement lets a developer reproduce the SDK in source form "solely to develop the
  Licensee Software" (§1.1(a)) and distribute only the `redistributable_bin` folder's libraries with
  the software, in object code (§1.1(b)); other rights are reserved (§1.3). Nothing grants
  publishing the headers. So `steam_api64.dll` and `libsteam_api.so` may ship beside a player; the
  SDK may not go into this public repository.
  (https://partner.steamgames.com/documentation/sdk_access_agreement)
- `SteamAPI_InitFlat` (SDK 1.59) initialises from a dynamically loaded library and reports why it
  failed (`ESteamAPIInitResult`: `NoSteamClient`, `VersionMismatch`, ...); with no Steam client it
  fails. `steam_appid.txt` beside the executable sets the app id for development and must not ship;
  `SteamAPI_RestartAppIfNecessary` relaunches through Steam when the game was started outside it.
- Callbacks and call results arrive in the thread that dispatches them: `SteamAPI_RunCallbacks`,
  or the manual dispatch API (`SteamAPI_ManualDispatch_*`) a host runs once a frame itself.
- Achievements and stats are synchronous in memory and uploaded by `StoreStats`, rate limited ("on
  the order of minutes"); since SDK 1.61 the client syncs the user's stats before the game starts
  (`RequestCurrentStats` is gone). Leaderboards are call results (find the board first; uploads
  limited to 10 per 10 minutes). Cloud saves are either Auto-Cloud (configured on Steam, no code) or
  `ISteamRemoteStorage` (files written and read by the game). Rich presence is up to 30 keys, its
  `steam_display` key a localisation token. DLC ownership is synchronous (`BIsDlcInstalled`).

## Decision 1 - two layers: a Foundation service and an Engine domain

- **`foundation.online`** (Foundation): the interface. The backend interface `IOnlineBackend`, the
  value types (an achievement's state, a stat, a leaderboard entry, a user), the result codes, the
  capabilities, and the request queue every backend completes through, which turns completions
  into handler calls on the thread that pumps it. A platform backend (the Steam plugin) and the
  tests' fake backend link this alone.
- **`foundation.online.local`** (Foundation): the Local backend, over a file and the definitions,
  separate as the script backends are from `foundation.script`. Only `engine.online` links it, to
  make it when no plugin provides a backend.
- **`engine.online`** (an Engine domain): the `OnlineSubsystem`, which owns the process's one
  backend (a platform service is one per process), pumps it each frame on the game's thread and
  stores stats at a rate the backend allows; a per-run `OnlineScriptBinding` among the run's
  services, holding the run's pending handlers (dropped, never called, when the run's scripts
  stop, as `Http`'s are); and the `Online` script facade over it. The subsystem is what falls
  back: it takes the backend the enabled plugin providing `online.backend` registers, and makes a
  Local one itself when there is none.

A native game part uses the same subsystem through C++ (`Function` handlers), never a backend.

## Decision 2 - asynchronous by request, answers on the game thread

Every operation that may wait (an upload, a fetch, a store) returns a request id and takes a
handler; the backend completes it whenever its platform answers, and the subsystem calls the
handler during its update on the game's thread, never inside the call and never on another thread.
`Online.cancel(id)` drops a handler. A handler hears `(ok, error, ...)`: `error` is a result code
name (`unsupported`, `offline`, `notFound`, `invalidArgument`, `rateLimited`, `backendError`) and a
message.

What the backend already holds is read at once, without a handler: whether an achievement is
unlocked, a stat's value, whether a DLC is owned, the signed-in user. This is the shape Steam has
since its client syncs stats before launch, and the Local backend's.

## Decision 3 - what a game can ask, per feature

A backend reports its **capabilities** (`achievements`, `stats`, `leaderboards`, `cloudSaves`,
`presence`, `dlc`); `Online.supports(name)` asks. An operation the backend does not support answers
`unsupported` (its handler called, or a read returning the fallback), never a crash and never a
silent success.

- **Identity**: `Online.backend()` (`"local"`, `"steam"`), `Online.signedIn()`, `Online.userName()`.
- **Achievements**: `unlock(id)`; `progress(id, current)` (unlocks at the definition's target);
  `isUnlocked(id)`; `achievements()` (each id, unlocked, progress). Clearing is the editor's and the
  Local backend's only (a development reset), never in a shipped player.
- **Stats**: `getInt`/`getFloat`, `setInt`/`setFloat`, `addInt`; written to the backend in batches
  by the subsystem (at most as often as the backend allows, and when the run ends), as `Save` writes
  its file.
- **Leaderboards**: `submit(board, score, handler)`, keeping the player's best unless the board says
  otherwise; `fetch(board, range, count, handler)` with `range` `top`, `aroundMe` or `friends`,
  each entry its rank, name, score and whether it is the player.
- **Cloud saves**: no new API. The `Save` facade's file is what syncs: the backend reads it before
  the run's first read and writes it when `Save` flushes (Decision 6).
- **Rich presence**: `setPresence(key, value)`, `clearPresence()`.
- **DLC**: `owns(id)`, `ownedDlc()`. Installing and buying are not here.

## Decision 4 - the definitions are a project asset

What a game has (its achievements with names, descriptions, hidden flags and progress targets; its
stats with kind, default and bounds; its leaderboards with sort order and score format; its DLC with
names) is an **Online Definitions** asset, set as the project's (a `onlineDefinitionsId` project
setting, as the default input map is). It cooks to a product every host loads. The Local backend
works from it (it knows the names and targets without a platform), the editor edits it as a page,
an agent writes it through the asset tools, and an id a call names that the definitions lack
answers `notFound` in every backend, so a typo shows on Local before it reaches Steam.

A platform backend maps the game's ids to its own: by default they are the same (Steam's API names
are set to match on Steam's side), and a backend's plugin settings may map them where they cannot be.

## Decision 5 - which backend, and a platform without one

- The backend is the enabled plugin that **provides `online.backend`** on the platform; with none,
  the built-in **Local** backend. Local is the engine's, not a plugin, so every platform has one,
  the web included.
- A platform backend that cannot start (no Steam client, an unknown app id) leaves the subsystem
  **offline**: `Online.backend()` still names it, `signedIn()` is false, reads answer their
  fallbacks and operations answer `offline`. It does not fall back to Local in a shipped player: a
  player whose achievements went to a local file would never see them on Steam. Steam's own client
  queues achievements earned offline once it started.
- **Play in editor uses Local** by default, its state in the project's `Editor/` folder (as `Save`
  keeps its own there), so testing never touches a real account; a project setting can let play in
  editor use the platform backend instead, for a developer testing on Steam.
- The Local backend keeps its state in one file in the user data directory (in the browser's
  storage on the web): unlocked achievements, stats, a local table per leaderboard (the one user's
  scores), presence in memory, DLC owned as the definitions mark for development.

## Decision 6 - cloud saves are the save file

The `Save` facade stays the one save API. A backend with `cloudSaves` keeps the save file in its
cloud: it reads the cloud copy before the run's first read (the newer of the cloud and the local
file wins, the other kept beside it), and writes it when `Save` flushes. Steam does this through
`ISteamRemoteStorage` (so the engine decides when and what), rather than Auto-Cloud (configured on
Steam's site, which a project can still choose instead, with the plugin's cloud setting off).

## Decision 7 - the Steam plugin

- `Code/Plugins/Steam` (`plugins.steam`, namespace `steam`): an Engine part providing
  `online.backend`, on Linux64 and Win64; its typed settings: the app id, whether to restart through
  Steam, whether to sync the save file, the id map.
- **The SDK is not in the repository** (its agreement grants no right to publish it). The build
  finds it through `STEAMWORKS_SDK_DIR`; without it the plugin's part is not built, and a project
  that enables it is told so (the host's "not built" refusal). The SDK's redistributable library is
  staged beside the player as the plugin's deploy file for the platform. The plugin talks to the
  flat C API (`steam_api_flat.h`) and starts with `SteamAPI_InitFlat`.
- Callbacks and call results are dispatched by the plugin with the manual dispatch API from the
  subsystem's update, on the game's thread, so nothing of Steam's runs on a thread of its own.
- `steam_appid.txt` is written for development runs only (a desktop dev player started outside
  Steam), never into an export; a shipped player restarts through Steam when its setting says so.

## Phases

- **O0 - the service and the Local backend.** `foundation.online`: `IOnlineBackend`, the value
  types, result codes and capabilities, the request queue (completions to handlers on the pumping
  thread, cancel); `foundation.online.local`: the Local backend over a file and a definitions
  object, tested through the interface as any backend would be. Tests: every
  operation's answer, unsupported and notFound answers, cancel, the file round trip, handlers only
  ever called from the pump.
- **O1 - the subsystem and the facade, achievements and stats.** `engine.online`: the subsystem
  (the backend chosen from the slot, else Local; offline when it cannot start), stats stored in
  batches and at run end, the per-run binding, the `Online` facade's identity, achievements and
  stats in both languages; play in editor on Local in `Editor/`. `Scripting.md` gains "Online".
  Tests: a script unlocks, reads and resets on Local; a run's pending handlers never run after it
  stops; a fake backend provided through the slot is the one used, and an offline one answers
  `offline`.
- **O2 - leaderboards, rich presence, DLC.** The rest of Decision 3 on the facade and Local, the
  fake backend extended. Tests: submit keeps the best, fetch ranges, presence keys, DLC ownership.
- **O3 - the definitions asset.** The asset type, its builder and creator, the project setting, the
  editor's page for it, `project_info` reporting it, an MCP tool reading and resetting play in
  editor's Local state (`online_state`), `McpGuide.md`. Tests: an id the definitions lack answers
  notFound; the page edits and saves; the tool resets.
- **O4 - cloud saves.** The save file through a backend with `cloudSaves` (Decision 6), the fake
  backend's cloud. Tests: the newer copy wins and the other is kept; a flush writes both.
- **O5 - the Steam plugin.** Decision 7, built where `STEAMWORKS_SDK_DIR` is set; its parts tested
  where it is (a Steam client running, the test app id 480) and its absence tested everywhere (the
  project told the part is not built, an export refused). The export templates' builds decide
  whether they carry it (see Open questions).
- **O6 - a sample uses it.** Sky Hopper (it has scores, stars and five levels) gains achievements,
  stats and a leaderboard, played on Local in the editor and the player.

## Gotchas

- **Rate limits**: Steam's `StoreStats` and leaderboard uploads are limited, so the subsystem
  batches stats and the Steam backend queues uploads, answering `rateLimited` only when its queue
  is full.
- **Leaderboards need finding first** on Steam (a call result); the backend keeps the handles.
- **Rich presence on Steam shows only localisation tokens** for `steam_display`; the docs say so,
  and the facade passes keys and values as given.
- **DLC under Steam Family Sharing**: ownership is the borrower's; the backend reports what Steam
  reports.
- **Tests cannot use Steam** on the lanes: a fake backend in the tests stands in for a platform,
  and the Steam plugin's own tests run only where the SDK and a client are.
- **One backend per process**: play in editor's instances share it (they are one editor process),
  so Local's state is the project's, not an instance's.

## Open questions

1. **Where the Steam plugin's SDK comes from for the export templates.** The SDK cannot be in the
   repository, so the CI that builds the templates would need it from a secret, or the official
   templates carry no Steam backend and a project that enables it gets a player built for it (which
   needs an engine checkout with the SDK). The flagship game needs it either way.
2. **Achievement and stat ids mapped per backend, or the same everywhere.** Same by default, a map in
   the plugin's settings where needed, as above; or required to match.
3. **Play in editor on the platform backend**: a project setting (as proposed), or only ever Local.

## Tests (the contract)

Each phase lands with its tests (listed per phase). Across the spec: a fake backend, provided
through the `online.backend` slot as a plugin would provide it, proves the subsystem, the facade
and the Local backend behave alike for every operation, in both script languages, on the clang,
gcc, clang-shared, wasm (Local only) and ASAN lanes.
