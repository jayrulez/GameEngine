# MCP: agent access to the engine, pipeline, and editor

> ARCHIVED 2026-11-02, partly built; what it still owes is in Documentation/Plans/weekly_backlog.md
> ("Left over from the specs archived 2026-11-02"). Non-authoritative: the original spec, kept as
> the record of what was built and why; the code and the tests are the truth.

**Status:** ACTIVE - P0/P1 SHIPPED (24 tools over stdio); P2 editor host BUILDING on branch `editor-mcp` from 2026-09-25 (plan below, "P2 build plan")
**[DISCUSS]**. Everything else is recommendation-grade and buildable once
those settle.

## Why this works unusually well here

Three shipped decisions make Draconic agent-friendly before writing a line
of MCP: scenes/prefabs are TEXT (XML) so agents read and write them
directly; the reflection system knows the whole authored surface
(properties, methods, enums, attributes) including the per-backend BOUND
script API; and the pipeline (import/cook/export) is API-driven with a CLI
precedent (Tools.Export). MCP is mostly a thin, well-typed door onto
machinery that already exists - plus one reorg that is worth doing anyway.

## Architecture (three layers, bottom-up)

1. **`foundation.json`** (Foundation - DECIDED, mirrors foundation.xml;
   BUILDING, Opus 2026-08-09): JSON value + parser + writer, hand-rolled
   like the XML DOM, at `Code/Foundation/Json` (module `foundation.json`),
   with its own Tests target.
   - **Wire-protocol + game-data interchange, never ENGINE data** - the
     no-JSON-for-engine-data policy stands (this is the WideString pattern).
     Scope note vs xml: NO ISerializer backend (foundation.xml.serialization
     exists because XML IS the engine data format; json deliberately gets
     none - that absence is the policy made structural). Engine types never
     serialize as JSON; a GAME choosing JSON for its own data via script is
     the game's business.
   - **RTTI-reflected, script-usable (user requirement 2026-08-09)**: the
     JSON value type leans on our reflection so scripts can parse, build,
     query, and stringify JSON through the standard bound-object machinery
     (reflected methods; constructor-less/constructible handles per the
     Track A patterns). Numbers are f64 - which matches both script
     backends' number model exactly. Design cares: typed accessors follow
     the facade-numerics rule on the METHOD path; a parse error returns a
     clean null/error to script (never a half-value); document the
     ownership model for nested value handles (owned handles or the borrow
     rules - whichever the value representation makes safe; NO raw interior
     pointers into a reallocatable document, per the re-resolving-handle
     lesson). Payoff: game scripts get a data-interchange type, and the MCP
     layer's own values are script-visible for free.
2. **`foundation.mcp`** (Foundation, `Code/Foundation/Mcp`, alias
   `Foundation::Mcp`, deps Foundation::Json + Foundation::Core): JSON-RPC
   2.0 + the MCP lifecycle + a TOOL REGISTRY + resources. Transport-abstract
   with stdio first. The registry is shaped like the script-facade pattern:
   tools REGISTER against the server; each module contributes its own. THE
   PROTOCOL APPENDIX BELOW IS BINDING - it pins the subset we implement so
   the build does not have to re-derive the MCP spec.

### foundation.mcp protocol appendix (binding)

- **Protocol revision**: implement against the current stable MCP revision
  and PIN its `protocolVersion` date string as a constant; the initialize
  response echoes the client's requested version when we support it, else
  our pinned one (per spec). Record the pinned string in a comment with the
  spec URL.
- **v1 SUBSET - implement ONLY**: `initialize` + `notifications/initialized`;
  `tools/list` + `tools/call`; `resources/list` + `resources/read`; `ping`.
  Capabilities advertise exactly {tools, resources} - NO prompts, NO
  sampling, NO roots, NO resource subscriptions/list_changed, NO batch
  requests (later MCP revisions dropped batching - reject arrays at the
  top level with -32600). Unknown methods -> -32601, politely. Unknown
  NOTIFICATIONS (including notifications/cancelled) are ignored without
  error, per JSON-RPC.
- **stdio framing**: NEWLINE-DELIMITED JSON-RPC messages on stdin/stdout -
  one message per line, no Content-Length headers (that is LSP, not MCP).
  Nothing else may write to stdout (LOGS GO TO STDERR - route the engine
  log there in the host; a stray stdout print corrupts the stream).
- **Error model - two layers, never conflated**:
  - PROTOCOL errors are JSON-RPC error responses: -32700 parse, -32600
    invalid request, -32602 invalid params (schema-validation failures land
    HERE with a message naming the field), -32601 method not found, -32603
    internal.
  - TOOL errors are SUCCESSFUL responses whose result carries
    `isError: true` and the underlying Status/message text as content -
    an agent must receive the real cook/import error text as tool output,
    not a protocol failure.
- **Tool results**: MCP content arrays; v1 emits `text` content only
  (JSON-stringified payloads go in text). Image content (screenshots) is a
  P2 concern.
- **JSON Schema subset** (both EMITTED for tools/list and VALIDATED on
  call): object/string/number/integer/boolean/array with `properties`,
  `required`, `items`, `enum`, `description`, `default`. Declared per tool
  through a small schema-builder API (no schema strings by hand, no
  full-JSON-Schema engine). Validation failure -> -32602 naming the
  offending field.
- **Registry API shape**: Register(name, description, schema,
  handler) where handler is `Function<Result<JsonValue>(const JsonValue&
  args)>`; a failed Result becomes the isError tool result with the Status
  message. Tool names use snake_case (the surface in this spec).
- **Concurrency**: v1 is a single-threaded serve loop - read a line,
  dispatch, write the response. No cancellation, no parallel requests;
  request `id`s are echoed verbatim (string or number - preserve the JSON
  type).
- **Tests** (module suite): each bullet above is a test case - framing
  (multi-message lines, oversized line, garbage line -> -32700 and the
  loop SURVIVES), version negotiation, subset rejections (-32601/-32600),
  both error layers, schema validation (-32602 with field name), id-type
  preservation, and the registry round-trip over an in-memory transport.
3. **Hosts**:
   - **P1: `Tools.Mcp`** - a headless server binary over a
     project: loads the content DB + pipeline + reflection, speaks stdio.
     An agent (Claude Code config: one command line) gets the full
     authoring surface with the editor CLOSED. This is where 80% of the
     value lands, with zero networking.
   - **P2: editor-embedded** - the SAME tool registry hosted inside the
     running editor over streamable HTTP (localhost + bearer token), adding
     LIVE tools the headless host cannot serve (viewport screenshot,
     simulate control, selection, open pages). The HTTP server rides the
     socket/accept infrastructure from the web-networking track's WS
     server - build that primitive once, both consumers use it.
   DECIDED: OPT-IN - Preferences toggle (persisted) + token shown there;
   a `--mcp` editor launch flag comes up pre-enabled (agent-initiated
   launches need no UI); the token is also written to `<userdata>/mcp-token`
   (user-readable file, same trust domain) so a local agent self-configures.

## P0 - the Pipeline reorg (prerequisite; valuable standalone)

Move asset + import/cook out of the `{Library}.Editor` libs into a new
collection:

    Code/Pipeline/{Library}.Pipeline   (module {library}.pipeline)

- Naming DECIDED (in progress 2026-08-08, Opus building): `Pipeline` - the
  Traktor term (our lineage), short, folder==target==module compliant.
- Split rule: `{Library}.Pipeline` = Asset class + importer + cook logic,
  NO UI dependency (foundation.ui never appears); `{Library}.Editor` shrinks
  to pages/inspectors and depends on its Pipeline lib. The editor-only
  state rule is unchanged (editor view state stays on the Asset class -
  Asset classes are authoring-side, which is exactly where they are
  moving; the rule's boundary was always authoring vs RUNTIME wire).
- Mechanics: the folder==target== convention applies (Process/CONVENTIONS.md; formerly the reorg recipe) (folder==target==
  module, in-place moves, no rewrites); the cook/export CLI and editor
  relink against Pipeline libs; tests move with their code. Land module by
  module (texture first as the pattern-setter, same as reflection P1 did).
- Acceptance: `Draconic.Tools.Export` and a new headless smoke (import +
  cook a texture with NO editor lib in the link) both green; editor
  unchanged on screen.

## P1 opening step - Pipeline.Importer extraction - DONE (Fable, c83e6195)

SHIPPED 2026-08-09 (note: lib named Pipeline.Importer - `import` is a C++
keyword and invalid as a module name). The importer framework moved out of
Editor::Core into its own lib (not Pipeline.Core - the collection already splits
by role: Core = the shared asset contract, Cook = the driver, Import = file
ingestion). Rationale: 7 of 13 pipeline libs are cook-only and must not
inherit importer machinery; the link line should say what a target does.

- Moves: IFileImporter, ImportOptions, CopyIntoSources/DeferredImportWrite,
  the filename helpers - the import EXECUTION framework.
- Stays: the editor's import DIALOGS (UI) and EditorProject. Pipeline.Import
  takes an INJECTED paths/db context (sources root + content DB) instead of
  EditorProject; the editor constructs the context from its project. This is
  the seam the MCP `asset_import` tool calls.
- Relinks: Texture/Audio/Fonts/Script/UI.Pipeline + ModelImporter (and their
  test targets) drop Editor::Core for Pipeline::Import. Editor::Core shrinks
  to genuinely editor-only concerns.
- Deps: Pipeline::Import -> Pipeline::Core + Content + VFS. Nothing UI.
- Acceptance: no Pipeline lib links Editor::Core (grep gate, both alias
  spellings); full battery green both compilers; editor import flow
  unchanged on screen.
- EXECUTION NOTE: Fable performs this move directly, ONLY after the user
  confirms Opus is paused (concurrent-build/gcm-corruption lesson).

## P1 - stdio server + the core tool surface

Tools (each a thin wrapper over an existing API; JSON-schema'd params;
errors return the underlying Status message - agents need the real error
text):

- **Project**: `project_open`, `project_info`, `asset_list` (path/type
  filters), `asset_info` (guid, type, source path, cook status).
- **Pipeline**: `asset_import` (file + per-type options), `asset_cook`
  (one/all; returns the cook log), `project_export` (preset name).
- **Scenes (text!)**: `scene_read` / `scene_write` (the XML source -
  validation on write: parse + schema + refusal reasons back to the
  agent), `scene_validate`, `prefab_read`/`prefab_write`.
- **Reflection**: `type_list` (domain-filtered), `type_info` (properties/
  methods/attributes/enums), `script_api` (the API-browser surface, PER
  BACKEND - the tool that lets an agent write correct Wren/AngelScript).
- **Scripts**: `script_validate` (compile a source against a backend
  without saving), `script_create` (from the per-backend starters).
- **Diagnostics**: `log_read` (recent engine/cook log), `known_issues`
  (KNOWN_ISSUES.md as a resource).
- Resources: scene/prefab XML sources, docs/design/*, docs/specs/* exposed
  read-only by URI - agents get context without shell access.

Tests: protocol round-trip (initialize/list/call over an in-memory
transport) and per-tool schema/failure cases live with foundation.mcp /
the tool owners as usual (module suites; .test-scratch discipline;
schema-validation failures return JSON-RPC errors, not crashes). The
GOLDEN END-TO-END - the agent-shaped call sequence (open -> import ->
cook -> scene_write -> validate) against a fixture project, headless -
lives in the NEW `Code/Integration/Integration.Mcp` target (see
Code/Integration/README.md for the collection's admission rule; hook it
into the root tests cluster with util_test_suite like every suite). It is
the collection's first resident; cross-collection flows belong there, not
in whichever module's test target has the most links.

### OPEN DESIGN QUESTION (Opus -> Fable, 2026-08-09): where does the host's builder/importer set live?

STATE: the read tools have shipped. `project_create/open/info` (Editor::Mcp,
4e3b73e3) and `asset_list`/`asset_info` (3f6c1ec7) are in, clang+gcc green,
golden extended (create -> open -> seed -> list -> info). The NEXT increment is
the WRITE side - `asset_import` (needs an ImporterRegistry) and `asset_cook`
(needs a BuilderRegistry populated with EVERY builder + all the product/resource
type registrations). That is where I stopped, because building it forces an
architecture decision I want your call on rather than picking unilaterally.

THE PROBLEM: the full builder+importer registration is duplicated VERBATIM in
three hosts today - Tools.Cook/Main.cpp (RegisterAllBuilders, ~55 lines),
Tools.Editor/Main.cpp (same list + the 6 IFileImporter registrations), and
Tools.Export/Main.cpp (same builder list again). Wiring `asset_cook`/`asset_import`
into the MCP host needs the SAME set, so the naive move adds a FOURTH copy. Two
knock-on facts: (1) whatever consumes it links every pipeline module
(texture/model/audio/physics/ui/script/...), so the currently-lean headless MCP
host stops being lean; (2) the three copies already drift-risk - a new builder
must be added in three places and nobody's checking.

OPTIONS I see (your pick, or a better one):
  A. Extract a shared module (e.g. Pipeline.Hosting) exposing
     RegisterAllBuilders(BuilderRegistry&) + RegisterAllImporters(ImporterRegistry&),
     and migrate ALL FOUR hosts (Cook/Editor/Export/Mcp) onto it. Correct DRY
     fix and kills the drift, but it touches three working hosts and forces an
     expensive Tools.Editor rebuild to verify - and it's squarely a
     pipeline-collection structural decision, which is your seam, not mine.
  B. Extract the same shared module but consume it ONLY from the MCP host for
     now; leave the three existing hosts on their inline copies, migrate later.
     Isolated (no Tools.Editor rebuild), but there are transiently 3 inline
     copies + 1 module until someone finishes the migration.
  C. Defer `asset_cook`; ship the lighter write tools first - `asset_import`
     (ImporterRegistry only, smaller set) + `scene_read`/`scene_write` - and let
     the registration question settle before the cook tool lands.

MY LEAN: A is the right end state (it also gives P0's "editor executable
assembles the same set" an actual single source of truth), but it's a
pipeline-structure move under your ownership and Opus/Fable concurrent-build
discipline applies, so I did not start it. Tell me the shape and I will build
the tools on top; or take the extraction yourself and I will consume it. Parked
here until you answer.

### Fable RULING (2026-08-09): OPTION A, shaped as `Pipeline.Registration`; Opus executes

A - and in ONE commit, not B's transitional state. The three existing copies
are already the collision-cook class of bug waiting (a builder added to two
of three hosts = cook works in the editor, silently missing from CLI export);
a fourth copy is how that ships. B's "migrate later" never comes, and C does
not even dodge the problem - asset_import needs the importer set too.

Shape:
- **`Pipeline.Registration`** (Code/Pipeline/Pipeline.Registration, module
  `pipeline.registration`, alias `Pipeline::Registration`): the pipeline's
  COMPOSITION ROOT as a library. It links every pipeline module - that is
  its entire job (the deliberate fan-in point); nothing else links wide.
- Three entry points: `RegisterAllBuilders(BuilderRegistry&)`,
  `RegisterAllImporters(ImporterRegistry&)`, and `RegisterPipelineTypes()`
  (the product/resource type + serializable registrations a headless cook
  needs - the bundle CALLS each module's existing registrar; it never
  restates registrations inline).
- **All four hosts migrate in the same commit** and the inline copies are
  DELETED - the single source of truth is only true if nothing else claims
  it. Cook, Editor, Export mains shrink to one call each; Tools.Mcp gains
  it when the cook/import tools land.
- Tripwire test (the missing drift-checker): Pipeline.Registration.Tests
  registers into fresh registries and asserts the COUNTS against explicit
  constants - a new builder bumps the constant deliberately, a lost
  registration fails loudly. Cheap, and it converts "nobody's checking"
  into "the build checks".
- Leanness tradeoff: ACCEPTED for Tools.Mcp - serving asset_cook requires
  every builder by definition; the host is the authoring server, not a
  reflection toy. (The reflection-only use case keeps working - it just
  ships more code than it strictly needs, which is fine at these sizes.)
- EXECUTION: Opus builds it - the structural decision is made above, so
  what remains is mechanical and sits on the path Opus is already walking
  (extraction -> migrate hosts -> then asset_import/asset_cook on top).
  One agent building keeps the concurrent-ninja discipline trivially
  satisfied. Verify bar: full battery both compilers (the Tools.Editor
  rebuild is the cost of correctness here - pay it), goldens extended with
  an import -> cook flow once the tools land.

### EXECUTED (Opus, 2026-08-09, b26ca85f): Pipeline.Registration extraction DONE

Shape built exactly as ruled. `Code/Pipeline/Pipeline.Registration` (module
`pipeline.registration`, alias `Pipeline::Registration`) exposes the three
entry points; wide imports in the impl unit, lean interface BMI. All three
existing hosts migrated in the one commit, inline copies DELETED - Cook/Export
mains call RegisterPipelineTypes()+RegisterAllBuilders(); Editor wraps them in
RegisterEditorBuilders() + its two editor-UI registrars (host-side) and calls
RegisterAllImporters() for the drag-drop set. The canonical set is the SUPERSET
(folded in the SceneDocument type+serializer that only Export had - drift
closed). Tripwire Pipeline.Registration.Tests asserts counts vs constants
(kBuilderCount=21, kImporterCount=6). Verify bar met: clang+gcc green on the
root, all 3 hosts, the tripwire (3/3), Integration.Mcp (47/47); a Tools.Cook
cook of a fresh project runs clean. NEXT: asset_import/asset_cook on top +
Tools.Mcp gains the registrars + golden extended to import->cook.

### OPEN DESIGN QUESTION (Opus -> Fable, 2026-08-09): the COMPLETE script_api surface

STATE: script_api SHIPPED (394b9a15, Foundation::Mcp.Script) - it dumps a backend's
DescribeBoundApi as JSON (per-language types + members + signatures). But it is
INCOMPLETE, and the user flagged exactly why: "can we register the real subsystems
with MCP? some are not headless - or do we dump their registered surface?"

ANSWER (confirmed, not the question): we DUMP the surface, never instantiate the
subsystems. The facade REFLECTION is already metadata-only and headless: each
subsystem exposes a free fn (RegisterPhysicsScriptFacade, RegisterAudioScriptFacade,
RegisterInputScriptFacade, RegisterUIScriptFacade, particles/net/...) that touches
ONLY the registries (GlobalTypeRegistry + RegisterExtraScriptRootType +
RegisterExtraFacadeName + TypeOf). No world/device/GPU. So MCP needs no running
subsystem - it registers the metadata and reads DescribeBoundApi.

THE GAP (corrected 2026-08-09 after user pushback - my first framing overstated it):
those metadata fns live in the subsystem libs, so today's script_api - which links only
core+pipeline, not Engine.* - shows just core types (Float3/Color/Transform + Entity/Log/
Time/Random + bound pipeline types) and MISSES every gameplay facade (RigidBody/Audio/
Input/UI/Particle/Animation/SceneLoader). An agent would conclude those do not exist.

*** The "not headless" objection is WEAKER than I first wrote. *** The user noted we DO
have headless RHI, and audio too: RHI.Null is a real null backend, and foundation.audio
ships a headless Null mixing mode (AudioEngine `headless`/Null - real fallback, not a
stub). So the Engine.* subsystem libs are headless-LINKABLE, and script_api never creates
a device anyway (it only calls the 8 metadata-only registrars: RegisterPhysicsScriptFacade
/ RegisterAudioScriptFacade / RegisterInputScriptFacade / RegisterUiScriptFacade /
RegisterRenderScriptFacade / RegisterParticleScriptFacade / RegisterAnimationScriptFacade
/ RegisterSceneLoaderScriptFacade, all exported free fns touching only the registries).
So linking the subsystems into MCP is NOT a device/GPU-in-a-headless-host correctness
problem - it is only a binary-SIZE/link-weight tradeoff. That reframes the options:

OPTIONS (your pick, or a better one):
  A. MCP host LINKS the Engine.* subsystems and calls the 8 RegisterXxxScriptFacade
     (ideally via one RegisterAllScriptFacades composition root, so hosts do not drift -
     the RegisterAllBuilders lesson). CORRECT + headless (RHI.Null / audio Null); cheap
     to build. Cost: a heavier MCP binary (links the subsystem set). Now the front-runner.
  B. EXTRACT each subsystem's facade-reflection into a device-free lib + the composition
     root - the Pipeline::Registration precedent. Now an OPTIMIZATION over A (keeps the
     MCP/host link lean), not a headless necessity. Real refactor across N subsystems on
     YOUR subsystem seam.
  C. GENERATE + CACHE: a tool (or the EDITOR, which already builds this surface for its
     API browser - ScriptApiSurface) emits script-api.json; MCP serves the artifact.
     Full decoupling; adds a regenerate/staleness step.

MY LEAN (revised): A now - it is correct and cheap, and the "headless" objection that
made me favor B is resolved by RHI.Null + audio Null mode. Do A with a
RegisterAllScriptFacades composition root so the surface has one source of truth; revisit
B only if the MCP host's size actually bites. Still your subsystem seam, so parked for
your nod - but I can build+PROVE A (link, register, confirm headless run, assert the
gameplay facades appear) quickly if you want. Current script_api stays core+pipeline
until you rule.

### Fable RULING (2026-08-09): A - and C is rejected for a reason worth recording

**A.** Link the subsystems, call the registrars, one `RegisterAllScriptFacades`
composition root (Engine-side sibling of Pipeline::Registration, same tripwire-
count test). The deciding argument goes beyond cheapness: `script_api`'s entire
VALUE is being TRUE - it exists so agents never write against an imagined
surface. That kills C on principle, not just convenience: a generated artifact
reintroduces staleness, and a stale surface is precisely the failure this tool
was built to prevent. Never serve cached truth from a tool whose job is truth.

**B is deferred-on-evidence, not rejected**: the composition root HIDES the
link shape, so if the host's size ever measurably bites, extracting device-free
facade-reflection libs later changes nothing above the root. Do not spend the
N-lib refactor on a local dev server's binary size today.

Scope notes:
- The composition root is for SURFACE-DESCRIBING hosts (MCP, a future API-
  browser export). The RUNTIME keeps per-subsystem registration - a game's
  bound surface is the subset its subsystems create, and that difference is
  correct: script_api documents the ENGINE's surface; per-project subsetting
  is out of scope for now (note it in the tool description so agents know).
- Registrars must stay idempotent against the host's existing core
  registration (they are - the registry dedups; keep a test on it).
- RIDER: the Luau backend core landed (464586d5) with DescribeBoundApi
  implemented - `script_api` should accept backend = wren | angelscript |
  luau NOW; three-way output is the first cross-backend surface diff we get
  for free, and it feeds the smoketest-fixes #8 parity diagnostic later.
- Acceptance: the Integration golden asserts the gameplay facades appear
  (RigidBody, SceneLoader, Ui...) in a headless run with no device; full
  battery both compilers.

Opus proceeds on A directly.

ADDENDUM (user catch, 2026-08-09): TYPE DOMAINS drifted in the P0 reorg. The
domain is an OPEN string-hash set answering "which processes have this type";
the reorg moved asset/importer/cook types into the Pipeline collection but
their registrations still tag `TypeDomain(u8"Editor")` - which is now FALSE
twice over (the headless MCP/CLI hosts have them WITHOUT the editor; the
player has them not at all, so the default Runtime would be worse). Fix,
riding the current work:
- Introduce the `Pipeline` domain (adding a domain is free - the set is
  open by design). Sweep the Pipeline-collection registrations
  Editor -> Pipeline; genuinely-editor types (pages, editor settings) keep
  Editor.
- The truth table: player registers Runtime only; pipeline/CLI/MCP hosts =
  Runtime + Pipeline; the editor = all three.
- `script_api` (and type_list/type_info) INCLUDE the domain per type - the
  agent-facing point of the whole exercise: a game script must know that
  TextureAsset exists for authoring tools but NOT in a shipped player.
  Anything keyed on "editor-only marker" (the API browser's [editor] tag)
  should treat non-Runtime as not-in-the-player, so the new domain slots in
  without special-casing.
- Tripwire: a test asserting NO Pipeline-collection registration carries
  the Editor domain (and none default to Runtime).


### EXECUTED (Opus, 2026-08-09): A + the domain addendum, both DONE

- Ruling A (fe1bd7c9): Engine::ScriptSurface (module engine.scriptsurface) =
  RegisterAllScriptFacades() over the 10 subsystem/net registrars (physics/audio/
  input/ui/render/particles/animation/scene-loader/net x2), wide imports in the
  impl unit, tripwire + idempotency test (kSubsystemFacadeNameCount=30). Confirmed
  headless-linkable (no concrete RHI backend needed - Fable/user were right). Luau
  rider: RegisterLuauScriptBackend() added; script_api now spans wren|angelscript|
  luau. Tools.Mcp calls RegisterAllScriptFacades() at startup. Live: wren 17->47,
  angelscript 164, luau 26; RigidBody/Audio/Ui/SceneLoader/Net all present. Golden
  asserts the gameplay facades appear headless (no device).
- Domain addendum (0a45edce): pipeline::kPipelineTypeDomain; swept 14 Pipeline-
  collection files Editor->Pipeline (~22 types). script_api + type_list/type_info
  emit {domain, inPlayer} per type (TextureAsset -> domain=Pipeline, inPlayer=false).
  Tripwire: no rtti::pipeline type carries Editor; swept assets land on Pipeline.
  IsEditorOnlyBinding (!= Runtime) unchanged, so Pipeline slots in without special-
  casing. clang+gcc green throughout.

## P1 RESUME POINT v2 (Fable, 2026-08-10) - the single source of "what's left"

For Opus resuming MCP after the Luau track. Reviewed state (HANDOFF.md pass 5,
baseline 0a45edce): protocol + host + these tools are SHIPPED and verified -
type_list/type_info (with {domain, inPlayer}), project_create/open/info,
asset_list/asset_info, asset_import/asset_cook (over Pipeline::Registration),
script_api (wren|angelscript|luau, live via Engine::ScriptSurface).

v2 folds in the prior-art adoptions (both surveys below). REMAINING P1 work,
in build order:

1. BUILT 2026-08-17 (Fable, ec543437): scene_read / scene_write /
   scene_validate + prefab_read / prefab_write (editor.mcp:scene_tools;
   ProjectSession + shared helpers extracted to :session). Writes VALIDATE
   FIRST (refusal reasons + the full report in the error text) and store the
   XML VERBATIM (byte-identical read-back proven against real SaveScene
   output); prefab_write enforces the single-root rule; wrong-type guids
   redirect to the sibling tool. Validation shipped STRUCTURAL, then was
   UPGRADED TO FULL same-day (user ruling on the skipped-component gap):
   the new Engine::SceneSurface composition root - per-domain
   Add<Domain>SceneManagers functions that the subsystems' own
   OnSceneCreated delegate to, aggregated as engine::AddAllSceneManagers +
   RegisterAllSceneComponentReflection with a count tripwire
   (was kSceneSystemCount; since scene-composition 2026-08-19 the guard is the
   ModuleCount() check in Engine.SceneSurface.Tests) - gives the validate
   scratch the COMPLETE manager set headlessly, so component payloads
   field-validate through their real managers (componentValidation:
   "full"); only genuinely unknown component types surface as warnings
   (captured from the Scene reader's log). The same root replaced
   Tools.Export's private manager list, which had DRIFTED (missing
   PropertyAnimator, PostProcess, all audio/script/UI/net managers - CLI
   exports were silently dropping those records) and its 4-domain
   reflection block. Integration.Mcp gained the agent-shaped scene-flow
   golden incl. the refusal battery + a real component payload round-trip
   and an unknown-type warning case.
2. BUILT 2026-08-17 (Fable): host_info - `RegisterHostInfoTool(server,
   buildStamp, hostState)` in foundation.mcp (the shared provider: pid via
   the NEW core::ProcessId() backend fn, build stamp, server + protocol
   versions, plus a LIVE host-state lambda - proven live, not
   captured-at-registration). The stdio host generates its own per-exe
   BuildStamp (the Runtime.Client pattern) and reports
   {projectOpen, projectName, projectDirectory}. P2 editor/game hosts call
   the same provider with their own stamp + state.
3. BUILT 2026-08-17 (Fable): asset_uses - the REVERSE dependency query
   (editor.mcp:asset_uses; RegisterAssetUsesTool(server, session,
   builders)). Edges computed LIVE (truth-tool rule, never a cached
   graph) from the same sources the engine uses: buildable assets run
   their builder's ScanDependencies (edge kinds `reads` + `references` -
   exactly what the cook hashes), scenes/prefabs run the export
   reachability-scanner recipe over the full Engine::SceneSurface manager
   set (`scene-resource` component Refs via a factory-less
   ResourceManager's unresolved set, `prefab-instance` parked prefab
   ids), plus the 7 ProjectSettings guid fields (`projectSettingsUses`:
   defaultScene/startupScript/defaultInputMap/defaultBusLayout/
   defaultUiTheme/defaultUiFont/loadingDocument). Direct users only by
   design - the agent re-runs on a user to walk the chain. Refusals:
   malformed guid, guid absent from the source DB (redirects to
   asset_list). Integration.Mcp golden covers a MaterialAsset->texture
   `references` edge, a MeshComponent-Ref `scene-resource` edge, the
   defaultScene settings edge, empty-result honesty, and both refusals.
4. BUILT 2026-08-17 (Fable): project_health - one call = "is this project
   sound" (editor.mcp:project_health; RegisterProjectHealthTool(server,
   session, builders); imports :asset_uses to share the live edge
   machinery). Sweeps: DANGLING REFS - every forward edge (builders'
   ScanDependencies reads/references, scene/prefab component Refs +
   prefab instances via the full-manager scan, the 7 ProjectSettings
   guid fields) whose target is missing from the source DB, each
   reported {from, to, edge}; BROKEN SOURCES - buildable assets whose
   envelope no longer deserializes + scenes/prefabs whose stream no
   longer loads; COOK STATE - Plan(false) dirty/upToDate/orphans, my own
   unbuildable count (scenes/prefabs excluded - they stage, not cook),
   and failed CookDb records. `sound` = nothing broken; dirty alone
   never unsounds (workflow state - run asset_cook). Integration.Mcp
   golden: empty project sound, intact-refs-while-dirty sound, three
   simultaneous breaks detected (references + scene-resource dangling
   edges to the same missing guid + defaultScene settings dangle), and
   healing flips sound back to true.
5. BUILT 2026-08-17 (Fable): log_read + log_write + known_issues
   (editor.mcp:log_tools; RegisterLogTools(server, logBuffer,
   knownIssuesPath)). The host registers ONE editor::EditorLogBuffer
   sink on the global logger FIRST thing in main (before the stderr
   mirror), so every LOG_* line of the run is captured with a monotonic
   sequence. log_read = incremental polling (sinceSequence from the
   previous call's lastSequence) + minLevel/category filters + newest-
   `limit` tail + a `dropped` overflow signal. log_write = agent marker
   into the same stream (category 'Agent', returns its sequence via the
   new EditorLogBuffer::LatestSequence() accessor) - the correlation
   loop: marker, act, read-from-marker. known_issues = KNOWN_ISSUES.md
   verbatim (host resolves it at startup by walking UP from the exe
   path, then the cwd; "" registers the tool with a guidance error
   instead of leaving it absent). Golden covers the marker loop, both
   filters, quiet high-water reads, empty-marker refusal, and the
   missing-register error.
   AMENDED same-day (user ruling): the MCP is a PRODUCT surface and must
   never feed agents internal development docs. known_issues now reads
   the CURATED, distribution-facing register
   Documentation/Shipping/KnownIssues.md (user-visible symptoms +
   impact + workaround only; curation rule in the file header) - NOT
   the repo-root development tracker, which is triage state and is not
   distributed. Resolution order: KnownIssues.md next to the executable,
   then Documentation/Shipping/KnownIssues.md up the tree (the
   engine-checkout layout). CLARIFIED (user, 2026-08-17): the MCP host
   is DEVELOPER TOOLING - game export/staging never touches it; the
   exe-adjacent layout exists only for a possible future
   engine-tooling/SDK binary channel, which owns its own staging.
6. BUILT 2026-08-17 (Fable): resources, read-only by URI, honoring the
   ruling (internal design/spec/process docs NEVER feed the MCP).
   - foundation.mcp gained ResourceProvider {list, read} - a DYNAMIC
     resource-set seam (static registration cannot track a project whose
     scenes appear/disappear); resources/list appends every provider's
     CURRENT entries; resources/read falls through static -> providers,
     with the mime type taken from the answering provider's listing.
   - editor.mcp:resources registers the open project's scene/prefab XML
     sources as project://scene/<guid> + project://prefab/<guid>
     (application/xml, listed live from the source DB, content verbatim
     = scene_read; read-only - mutation stays with scene_write).
   - The host registers every Documentation/Shipping/*.md as
     docs://<FileName> (text/markdown, readers re-read per request so
     edits are live), resolved by the same walk-up as KnownIssues.md.
   - AUTHORED the first shipping docs: Scripting.md (backends, the
     Behavior/Level/Game tiers, dispatch-by-presence lifecycle, harvested
     properties, script_api-is-truth), Assets.md (guid identity,
     import->cook->reference, asset_uses/project_health habits),
     Scenes.md (text sources, the validate-first authoring loop,
     single-root prefab rule) + KnownIssues.md from the item-5 amendment.
   - DEBRAND sweep (user catch): "draconic" scrubbed from all MCP
     surfaces - server name is now `engine-mcp` (default + host), the
     resource scheme is project://, docs say "engine/game project". The
     only remaining mention is the KnownIssues legacy-type-name entry,
     which is historical fact.
   Tests: foundation Mcp.Tests provider case (dynamic growth between
   list calls, provider-declared mime on read, static/dynamic
   coexistence, unowned uri -32602); the scene-flow golden asserts the
   written scene appears live as project://scene/<guid> and its
   resources/read text is byte-identical to scene_read. Fixed en route:
   a dangling StringView in resources/read (JsonValue::AsString returns
   BY VALUE - hold the String, then view it).
7. BUILT 2026-08-17 (Fable), the COMPILE-CHECK version per the split
   ruling (the TYPED version still lands with Luau P5 .d.luau +
   luau-analyze - unchanged): script_validate
   (editor.mcp:script_validate; project-independent) compiles in-memory
   source through the SAME per-language cook service the asset pipeline
   uses (ScriptLanguageCookRegistry -> IScriptLanguageCook::Cook), so
   what validates is exactly what would cook. Returns line-numbered
   compile errors, and on success the harvested metadata (className,
   handlers, properties, usesCoroutines) - the agent sees what the
   engine RECOGNIZED. checkLevel:"compile" honesty marker + the
   description states the limit (engine-API calls are NOT type-checked;
   check signatures with script_api). A disabled backend's missing cook
   errs with guidance. Golden: every ENABLED backend's own Behavior
   starter validates through the tool (className NewBehavior + onUpdate
   harvested), broken source reports line >= 1 with a message, and a
   missing required arg is a -32602 protocol error.
8. BUILT 2026-08-17 (Fable): script_create (editor.mcp:script_create) -
   the editor's New-Asset recipe headless: the chosen backend's own
   starter (the language cook's NewAssetTemplate, never hardcoded text)
   written to Sources/<name>.<ext> (extension from the backend
   registry) + a ScriptClassAsset envelope {fileName, language}. Tiers
   behavior|level|game; group placement; UniqueInstanceName so a name
   collision never overwrites. Returns {guid, name, sourceFile,
   fileName} and the description teaches the loop: edit the FILE,
   script_validate, asset_cook to attach. Golden: created file's
   on-disk content validates through script_validate (the tools
   compose), duplicate names uniquify, no-project refusal. All three
   languages ship at once as planned (Luau starter landed at P3).
9. BUILT 2026-08-17 (Fable): project_export (editor.mcp:project_export) -
   a thin wrapper over the ONE export entry point (editor::ExportOne),
   identical to the editor menu + export CLI: presets from
   export_presets.xml (else the synthesized host preset; unknown name
   errs listing the available), templates from the shared root + the
   host tool dir, scene streams pre-transcoded over the FULL
   Engine.SceneSurface manager set, the reachability scanner reusing
   the asset_uses scene scan. Args {preset?, out?, rebuild?}; returns
   outputDir + cook/stage/pack counts + engine-version warning +
   pruning summary; failure points the agent at log_read. En route the
   ifdef-free rule was honored: NEW core::ExecutablePath() System
   backend (Linux readlink /proc/self/exe, Win32 GetModuleFileName -
   the ProcessId precedent) replaced an inline platform branch; the
   host and tests resolve the exe dir through it. Golden: a REAL dist
   from an authored project (Content.pak + player.xml on disk, player
   staged, scene staged), unknown-preset and no-project refusals.
10. BUILT 2026-08-17 (Fable), SPLIT BY CHANNEL (user ruling: the skill
    is shipping material too): the OPERATING MANUAL lives at
    Documentation/Shipping/McpGuide.md - auto-served as docs://McpGuide.md
    by the existing directory registration, so ANY connected agent gets it
    over the wire, checkout or not (first moves, ground rules, workflows,
    per-tool gotchas, the looks-wrong checklist).
    .claude/skills/engine-mcp/SKILL.md keeps only the in-checkout half
    (build target, claude mcp add wiring, stale-buildStamp + no-open-editor
    rules) and points at the guide - no duplicated content to drift. Both
    stay honest HANDOFF-style: tools change => guide changes same commit.
11. BUILT 2026-08-17 (Fable): the full agent-shaped golden
    (Integration.Mcp McpFullFlowTests) - ONE sequence through the tools in
    the order the guide teaches: project_create -> project_open ->
    asset_import (a real on-disk Luau source: the starter) -> asset_cook
    (compiles, zero failures) -> scene_write (seed from a real SaveScene) ->
    scene_validate (full, by guid) -> project_health (sound, zero dirty,
    zero failed cooks). P1 RESUME LIST COMPLETE (items 1-11).

DESCRIPTION CRAFT (applies to every tool, now and later): the description is
the only thing the agent decides by. State what the tool returns AND what it
modifies; feedback-loop tools SAY SO ("use this as the validation loop when
authoring scenes" - the Traktor phrasing); failures return a sentence saying
what to do instead (already the Result<_, String> ruling).

NOTES:
- The script_api Luau gap (constructor-less .of handles absent from the luau
  dump) closes automatically with Luau P2 emission alignment - no MCP work.
- P2 (editor-live + HTTP/SSE - the SECOND transport; P1 stays stdio-only)
  remains parked behind web-networking's accept server. See the roadmap
  after the prior-art sections.

## PRIOR ART (Fable, 2026-08-10): ezEngine's MCP servers (de1ccf597, #2017)

ezEngine shipped editor + game MCP hosts 2026-08-08. Full read of the
implementation + their bundled agent skill. Verdict: our P1 direction is
independently validated (their skill literally instructs agents "for anything
the file system already answers, read the files instead" - files-first; their
stated philosophy is agent-as-sidekick, not AI scene generation). They have
no script_api equivalent - we are ahead on the scripting surface. What they
have that we should take:

ADOPT INTO P1 (cheap, do with the remaining tools):
- A repo CLAUDE SKILL shipped next to the server (theirs: .claude/skills/
  ez-mcp/SKILL.md) - launch recipe, poll-the-port-not-sleep, "read tools/list
  before guessing", per-tool gotchas, the files-first rule. This SUPERSEDES
  the bare dogfood config idea: the skill IS the dogfood config plus the
  operating manual, and it keeps working when tools change.
- `asset_uses` - REVERSE dependency query ("what uses this asset"). The cook
  DB has the edges; agents need it before any destructive change.
- `log_write` - agent writes a marker into the host log (correlating agent
  actions with engine output). Trivial.

ADOPT INTO P2 (design guidance - their transport is a validated blueprint):
- Streamable-HTTP subset works: they shipped no-SSE/no-sessions/no-batching
  and Claude connects (`claude mcp add --transport http`). USER DECISION
  (2026-08-10): we KEEP SSE in our P2 scope regardless - it is what carries
  server-initiated traffic (progress notifications for long cooks/exports,
  resource-change subscriptions - both already on our P3 list, and the P2
  play-mode ops are async by nature). Their no-SSE experience is the
  fallback proof: if SSE fights back mid-P2, the endpoint still ships
  without it and gains it after.
- Threading model: transport thread accepts + publishes ONE request; the
  main thread pumps ProcessPendingRequests() once per frame and answers
  (tools may touch anything - nothing else is thread safe). One request in
  flight, Connection: close. Gotcha they document: an idle host that sleeps
  must wake on HasPendingRequest or clients wait forever.
- The `notFinished` deferred re-run pattern: a tool that needs the host to
  MAKE PROGRESS (run N frames, async play-mode toggles) returns "not
  finished"; the transport re-enters it with the same args each pump until
  it completes. Tool keeps its own cursor + self-timeout. This is how
  long-running tools coexist with request/response HTTP.
- Ops hygiene worth copying: app_info reports pid (a hung host is killed by
  pid) + buildTimestamp (stale-binary detection beats "tool missing");
  app_ping answers only when the main thread is free (busy vs hung); a
  FAILED ASSERT is caught into a tool error carrying file/line/expression
  (host keeps serving, agent told to restart before trusting it).
- P2b CONFIRMED BY THEIR SHIPPED DESIGN: object_modify = one undoable step
  per call, labelled as MCP in the undo menu, object_undo shares the user's
  stack, failure CANCELS the command (no half-applied step), nothing saves
  implicitly. Adopt the labelling + cancel-not-finish details verbatim.
- `action_list`/`action_state`/`action_execute` over the editor's action
  system - a large tool surface for the cost of one generic bridge. Evaluate
  against our editor command/menu layer at P2.

NEW TRACK CANDIDATE (P3, park): a GAME-process host (their ezPlayer
-mcpport): app_screenshot (returns a PATH, not payload), input injection
that MERGES with real input (larger value wins - the human is never locked
out), input_set consumed per-frame so it pairs with game_wait,
game_pause/speed. Their pitch: agents TEST features (drive input, step
frames, screenshot, verify). For us this would ride GameInstance/run host +
our smoke-checklist pain - park until the editor endpoint proves out.

REJECTED for us:
- Reflection-discovered tool providers (derive a class = auto-registered,
  no list). Conflicts with our explicit-composition-root + tripwire
  philosophy; implicit registries are how their three-hosts drift class of
  bug happens. Keep explicit registration.
- HTTP-only transport. Their agents must bootstrap via curl + a skill;
  our stdio P1 host is directly speakable by Claude Code with zero setup.
  Keep stdio primary, HTTP arrives at P2 as the SECOND transport.
- Their structural live ops (addObject/moveObject/delete via object_modify)
  as the PRIMARY mutation path: our files-are-truth ruling stands for
  headless P1/P2; their shipped design is EVIDENCE for the parked
  in-memory-representation future note - revisit at P2b with usage data,
  as already decided.

## PRIOR ART 2 (Fable, 2026-08-10): Traktor's MCP (code/MCP, editor-plugin server)

Second survey. Traktor's bet is opposite to ezEngine's: deep DOMAIN tools
(full model editing incl. pose read/write, shader-graph update/validate)
rather than generic document/object tools. Four ideas worth taking, one
confirmation, one rejection:

THE ORIGINAL IDEA - SKILLS AS ENGINE ASSETS (adopt at P2/P3): SkillAsset is
a serialized DB instance {name, description, whenToUse, markdown body with
{{param}} substitution, typed parameters, engineVersion, published flag}.
Published skills surface over MCP prompts/list+get - i.e. they appear as
SLASH COMMANDS in the client. And skill_create is itself a TOOL: an agent
that works out a non-obvious procedure can PERSIST it as a skill for every
later session/user - a knowledge-capture loop, versioned with the project.
This replaces our vague P3 "Prompts (canned workflows)" with a concrete,
better design: prompts backed by an authorable asset type (reflected asset +
editor page - our stack is ideal for it), NOT a hardcoded prompt list. Keep
prompts OUT of the v1 capability set as ruled; reinstate when skill assets
exist as their source.

SESSION HANDLES FOR TRANSIENT OBJECTS (the parked future-note, shipped in
embryo): ModelSession keeps in-memory working models keyed by integer
handle across MCP calls - open (blank/file/asset) -> many edit calls ->
save -> close. Traktor shipped the agent-works-on-in-memory-representation
idea for MODELS, where a working object has no file identity mid-edit. For
us: the pattern to reach for IF real sessions show scene_write's
whole-file granularity is too coarse - a scene session (open -> edits ->
validate -> save) still lands on files-are-truth at save. Evidence for the
parked note, not a P1/P2 change.

BUILD-TIME SELF-DESCRIPTION CAPTURE (park the technique): a build script
extracts each shader node's GLSL emitter SOURCE + a one-line expression
summary + the [[deprecated]] set out of the engine source, baked into the
server - so it can explain what a node ACTUALLY does with no source tree
present, and steer agents off deprecated nodes. The generalizable rule:
when a tool must describe behavior, extract the description from the
source of truth at build time instead of hand-writing docs that drift.
(Our script_api already follows the stronger form - live reflection; this
technique covers things reflection cannot see, e.g. codegen semantics.)

DESCRIPTION CRAFT (adopt in P1, free): their validate tool ends with "Use
this as a feedback loop when authoring shader graphs." Explicitly telling
the agent HOW a tool fits the workflow loop - do this in our
scene_validate/script_validate descriptions.

CONFIRMATION: pipeline-build-through-MCP with real result reporting
(their 48b1c2c30 fixed exactly the "tool claims success regardless"
class) - our asset_cook already returns the true stats; keep the test on
it. engineVersion stamped on skills echoes our cook-fingerprint habit.

REJECTED: their threading (accept thread + per-connection pooled workers +
one global semaphore serializing tool invocation - tools touch the editor
DB off the UI thread). Survivable in their architecture; in ours it is
cross-thread editor access with extra steps. ezEngine's main-thread pump
remains our P2 model.

## P2 - editor-live endpoint (after web-networking's accept server exists)

TRANSPORT SUBSTRATE BUILT 2026-08-17 (Fable, task #137 phases 1+2; the
user chose the networking aspect first):
- foundation.http (Code/Foundation/Http): incremental HTTP/1.1 message
  parser (request+response, Content-Length only, chunked=Failed, 16 KiB
  head cap, close-delimited response bodies), pump-model HttpServer
  (one request per connection, Connection: close - the ez subset - plus
  HttpResponse::EventStream() -> a ref-counted SseStream held past the
  response; dead peers are LIVENESS-POLLED via SseStream::PollLive,
  because a write into a freshly closed socket still succeeds until the
  reset arrives), and a blocking localhost HttpFetch client (dotted
  quad, no DNS/TLS - the localhost trust domain). The Core/System TCP
  backends pre-existed. This server is ALSO the WS front door (phase 3:
  upgrade handshake + frame codec = web networking's native server).
- foundation.mcp.http (Code/Foundation/Mcp.Http, the satellite
  pattern): McpHttpHost hosts ANY McpServer - POST /mcp = one JSON-RPC
  message per request (202 for notifications), GET /events = the SSE
  channel with Broadcast(event, data) + listener sweep, bearer-token
  auth on everything (empty token refuses to Start), 404/405 refusals.
  Pump() from ONE thread (the editor's main thread per frame - tools
  may touch anything), simpler than ez's two-thread publish model and
  satisfying the same constraint.
Remaining P2 = the EDITOR host itself: Preferences opt-in + token file,
per-frame pump wiring, and the live action_* tools below.

Transport: minimal HTTP/1.1 + SSE on localhost, token auth, Editor
Preferences (enable + port + token). SSE KEPT (user decision 2026-08-10): it
carries the server-initiated traffic P2/P3 need (progress notifications,
resource-change subscriptions, async play-mode ops); ezEngine proves the
no-SSE fallback ships fine if SSE fights back mid-phase.

Threading (adopted from ezEngine - the validated blueprint): the transport
thread accepts and publishes ONE request; the editor main loop pumps
ProcessPendingRequests() once per frame and answers there (tools may touch
documents/world/renderer - nothing else is thread safe). One request in
flight. An idle host must wake on HasPendingRequest. Long-running tools use
the notFinished re-entry pattern: return "not yet", get re-entered with the
same args each pump, keep their own cursor + self-timeout. REJECTED
alternative (Traktor): worker-thread tool execution behind a global lock -
cross-thread editor access with extra steps.

Tools: `viewport_screenshot` (returns a PATH like ezEngine, not base64 -
agents read the file), `simulate_start/stop`, `page_open`, `selection_get/
set` (selection_set is also how the agent SHOWS the user which object it
means), `entity_inspect` (live component values via reflection RESOLVE
mode), `console_read`, plus host_info here too. NEW (from ezEngine):
`action_list` / `action_state` / `action_execute` over the editor's
command/menu layer - one generic bridge, a large surface for free; evaluate
the command-registry seam when building.

Hardening (ezEngine ops lessons): ping answers only when the main thread is
free (busy vs hung is diagnosable); a failed DIAGNOSTIC_ASSERT inside a tool
call is caught into a tool error carrying file/line/expression, the agent is
told to restart the host before trusting further state. (P1's stdio host
keeps honest process death - no assert-catching there.)

MUTATION SCOPE - DECIDED:
- P2 ships FILES-ARE-TRUTH mutation: agents change scenes/assets via the
  headless write tools; a `page_reload` tool asks the editor to reload from
  disk and is REFUSED while the page is DIRTY (unless forced) - the agent
  is told "user has unsaved changes" and must ask the user, never clobber.
- P2b (after P2 proves out): PROPERTY writes through the inspector's
  undo-command path - one undo step per MCP write, LABELLED as MCP in the
  undo menu (the user sees which changes were the agent's), normal
  dirty-marking, failure CANCELS the command so no half-applied step exists,
  an `undo` tool shares the user's stack. Writes during Simulate are
  refused. Nothing saves implicitly. (Design confirmed verbatim by
  ezEngine's shipped object_modify.) This is the ONLY live-write shape
  endorsed.
- Structural live ops (spawn/add-component/reparent): NOT planned - the
  file path covers them with structurally safer semantics.

FUTURE NOTE (user question, parked deliberately): agents operating on an
IN-MEMORY representation of open documents - the agent as a collaborative
peer on the live document model rather than the file. P2b's undo-command
writes are the embryonic form (the command stream IS the shared
representation); Traktor's ModelSession (integer-handle working objects
across calls -> save -> close) is the shipped embryo of the same idea and
the PATTERN to reach for if real sessions show scene_write's whole-file
granularity is too coarse: a scene session (open -> edits -> validate ->
save) still lands on files-are-truth at save. Revisit with usage evidence,
not before.

## PRIOR ART 3 (2026-09-25): the chat window - ezEngine, Traktor, Doriax

The user asked where the agent's conversation lives once the editor serves MCP. Three
clones re-read for exactly that (~/Dev/CPP/ezEngine, ~/Dev/CPP/traktor, ~/Dev/CPP/doriax).

**ezEngine: no chat, fully unattended.** The agent lives outside (Claude Code and the like)
and talks to the editor over MCP. Inside, the editor never waits for a human during a tool
call: `ezQtScopedUnattended` makes every message box log its text and return its declared
UNATTENDED ANSWER (each question names one - the answer that lets automated work proceed,
the accident-preventing one only where proceeding would destroy unrecoverable data),
`ezQtDialog::exec()` returns without showing, and anything entering a nested event loop
outside those paths (file pickers) checks `SuppressModalWindow` itself. What was suppressed
is recorded (bounded) and reported: `action_execute` lists it under `suppressedDialogs`
with a note ("the action most likely did nothing - look for a dedicated tool, or ask the
user"), the other tools rely on the log. Failed asserts are caught the same way and turn
the result into an error telling the agent to restart the editor. The user sees the agent's
work through the editor's own log and through the editor changing under them; there is no
panel.

**Traktor: no chat either.** `McpServerEditorPlugin` starts the server on a setting
(`Editor.McpServer`, port 13880) and pumps it on its own thread; ~45 tools plus the skill
tools and a prompt provider (skills as assets, surfaced as MCP prompts - the idea we adopted
in prior art 2). No editor UI beyond the log line "MCP server listening". The conversation
is the external client's.

**Doriax: a full in-editor assistant, without MCP.** `editor/window/AiChatWindow` (2.9k
lines) over `editor/ai/` (~13k): the editor IS the agent's front end and the tool loop.
- Providers: OpenAI, Anthropic, Gemini, OpenAI-compatible endpoints (`AiProvider.cpp`
  speaks each one's tool-calling dialect: `tools`/`tool_use`/`tool_result` for Anthropic,
  function tools for OpenAI, function declarations for Gemini), with a model catalog per
  account, rate-limit retry with backoff, and a `SecretStore` that keeps API keys
  obfuscated with a machine-bound seed in the config directory.
- The loop: `AiService` runs requests on a worker thread; the model's tool calls become
  `ActionProposal`s (name, arguments, a human `description`, `readOnly`); the window
  executes an approved proposal ON THE MAIN THREAD through `EditorActionExecutor`
  (5.7k lines of editor actions: entities, components, scenes, scripts, terrain, builds,
  play mode...), the result goes back as a tool result, and `update()` re-sends
  automatically while the conversation ends on unanswered tool results. Tool definitions
  come from a static `EditorActionRegistry` (name, description, JSON parameters,
  `readOnly`; validates arguments and describes a call in one line for the transcript).
- Three approval modes, the piece that answers "who decides": PREVIEW THEN APPROVE (every
  proposal shows with Approve / Dismiss), AUTO-RUN READ-ONLY (read-only proposals run at
  once, writes wait for approval), FULL AGENT (everything runs, budgeted per frame so one
  slow action cannot freeze the UI). The `readOnly` flag on each action is what makes the
  middle mode possible.
- The transcript: @mentions (selection, project, scene, entity, file, open file) that
  insert live editor context into the prompt, image attachments, conversation history
  persisted per project (`ConversationStore`), engine API context injected into the system
  prompt (`AiEngineApiContext`, so it can draft Lua/C++ against the real API), a
  notification badge when a reply lands while the window is hidden.
- The cost: a second tool surface (their registry, not MCP), every provider's wire
  dialect maintained by hand, keys and billing inside the editor, and the model choice as
  an editor setting.

**What this means for us.** Two products are on the table and they compose:
- A (the cheap half, ezEngine's model plus a panel): the agent stays external (Claude Code
  over our HTTP host); the editor gets an AGENT PANEL showing the calls and outcomes (the
  tool observer already delivers them) and, unlike ezEngine, the questions: an unattended
  call that hits a decision (reload a dirty page? overwrite?) parks the question in the
  panel and answers "not yet" (our re-entry) until the user clicks or a timeout takes the
  declared unattended answer. That is ezEngine's unattended contract with a human in the
  loop when one is present, and it is what the earlier "surface dialog choices to the chat"
  idea needs.
- B (Doriax's product): the conversation inside the editor. Two ways to get there, and
  only one is cheap: (1) host an agent RUNTIME - Claude Code via the Agent SDK or the CLI
  as a subprocess, with the editor's own MCP host registered to it - so the panel is a
  transcript and a text box and the tool loop, the skills, the docs and the approvals are
  the runtime's; or (2) Doriax's way, an API client and a tool loop of our own, which
  means re-building what the runtime already is and a second tool registry. Our MCP surface
  makes (1) natural: the approval modes map onto MCP tool annotations (readOnlyHint /
  destructiveHint, which our tools do not carry yet) and the runtime's permission system.

## P2 build plan (user rulings 2026-09-25; branch `editor-mcp`)

Restarted after the dev-box loss with the ezEngine editor plugin re-read (clone at
~/Dev/CPP/ezEngine, Code/EditorPlugins/Mcp). What it settled, beyond the P2 section above:

- **Async tools stay async.** The editor's import and cook are non-blocking (a background
  `EditorCookService` with a build lock, imports riding deferred writes) and MCP must not
  undo that. ezEngine's `asset_transform` BLOCKS the main thread for minutes; its
  `longop_execute` does not - it returns "not finished", the transport keeps the client
  waiting, and the host re-enters the tool with the same arguments every pump until it
  answers, the tool keeping its own cursor + timeout. We take the second shape everywhere
  a tool waits on the editor's own services: cook rides `RequestCookFor` + `IsCooking`,
  export rides the export job, a worker-side import rides its job. The STDIO host has no
  frame to pump, so `Serve` re-enters on a short sleep; its tools' synchronous paths are
  unchanged (the execution strategy is the host's, the tool's interface is shared).
- **One shared composition root, then per-host additions.** `editor::mcp::
  RegisterEngineTools(server, session, ...)` lists the base surface once with a count
  tripwire (the Pipeline::Registration pattern - Tools.Mcp/Main.cpp hand-listed 14 calls);
  each host adds what only it can serve (editor: page_/selection_/simulate_/screenshot/
  action_ tools; stdio: project_create/open). `host_info.serverName` tells the hosts apart.
- **`ProjectSession` is NON-OWNING** (`EditorProject*`): the editor host points it at the
  live project (same ContentDatabase object - no two-writer problem in-process; a UI
  refresh notice after an MCP write is what remains); the stdio host keeps its own
  UniquePtr beside the session.
- **Opt-in + port + token** as decided in P2, plus ezEngine's per-editor port flag
  (`--mcp-port <n>`, so tests can run several editors); the default port is fixed and
  documented in the skill. Token kept (Mcp.Http refuses without one).
- **Unattended mode for tool calls** (ezEngine's `ezQtScopedUnattended`): a scoped flag on
  EditorContext suppresses modal dialogs for the duration of a call and the call's result
  REPORTS what was suppressed - a suppressed dialog is otherwise indistinguishable from
  nothing having happened. Surfacing those choices to the agent's chat for the user to
  answer is a later idea, not this track.
- **Server lifetime = project lifetime**: starts on project open, stops on close
  (everything it exposes is project-specific).
- **Documents are never opened implicitly** and destructive decisions are ARGUMENTS
  (`discard`, `force`) - a tool never waits for a human. Every tool returns the identity
  of what it touched.

Landing order - one layer per commit, each with its passing tests, both compilers green:

1. `foundation.http`: a handler may answer NOT YET. The handler returns
   `Optional<HttpResponse>` and an empty answer means "ask me again": the server keeps
   the parsed request on its connection, skips reading, and re-dispatches it every Pump
   until the handler answers; a peer that closes while waiting is dropped;
   `PendingRequestCount()` so an idle host can tell a client is waiting. (First landed as
   a `deferred` field on HttpResponse; the user's review moved it: a response is only ever
   a response, "not yet" is the absence of one.) Test: a handler that says not yet N pumps
   then answers, the client sees one 200; the peer-closed-while-waiting drop.
2. `foundation.mcp`: `ToolOutcome` - a handler returns an answer (`ToolResult`, unchanged,
   so every registered tool compiles as is) or `ToolOutcome::NotFinished()`.
   `HandleLine` returns `{state, response}` with state Answered / Notification /
   NotFinished (the Optional<String> shape could not say "not yet"); `Serve` re-enters a
   NotFinished line after a 1 ms sleep. Tests: the three states through tools/call, and
   the Serve spin (handler entered N times, one output line).
3. `foundation.mcp.http`: NotFinished -> `HttpResponse::Deferred()`; `HasPendingRequest()`.
   Test: a deferring tool over real loopback, the pending flag observed mid-wait.
4. `editor.mcp`: non-owning ProjectSession + `RegisterEngineTools` with the tripwire;
   Tools.Mcp composes through it. Tests: the count, Integration.Mcp goldens unchanged.
5. `editor.app`: `EditorMcpHost` (server + McpHttpHost, Pump in OnUpdate, start/stop on
   project open/close), `EditorMcpSettings` (enabled, port, token) + Preferences UI, the
   `<userdata>/mcp-token` file, `--mcp` / `--mcp-port`, host_info with editor state.
   Tests: Editor.App.Tests starts the host over loopback and round-trips host_info +
   project_info; the settings section round-trip; the token file.
   BUILT 2026-09-25 (six commits: the docs locator shared by both hosts, the settings
   section + GenerateMcpToken, the host, the tool observer in foundation.mcp, the
   Preferences section with live apply). Two things the code decided: the UNATTENDED
   scope moved to layer 7 - no shared tool opens a dialog (editor.mcp is UI-free by
   construction), so it has no consumer before the live tools; and the post-write
   refresh needs no new path - a write lands in the editor's own source DB and files,
   the cook service's Sources/ watch cooks it, and OnCookFinished already rebuilds the
   Assets view; the status bar shows each finished call through the tool observer.
   Server name `engine-editor-mcp`; default port 7405.
6. `editor.app`: the async strategies - asset_cook over EditorCookService (refused with
   the reason while MutationLocked), project_export over the export job, asset_import
   over the deferred-write path. Tests: a cook that spans pumps answers with the real
   counts; the busy refusal.
   BUILT 2026-09-25 (three commits): editor.mcp's IProjectOperations seam (the tools keep
   their contract, the host supplies where the work runs; InlineProjectOperations is the
   stdio host's), the cook service's CookSummary (and a Shutdown that leaves it
   quiescent - the first cook-service test found the stuck m_cooking), and
   EditorProjectOperations on the editor's cook + job services. One ruling refined by the
   code: nothing is REFUSED while the databases are locked - the cook service remembers a
   request that arrives mid-cook and an import waits for the lock to clear, so the agent
   simply waits (with a timeout) instead of being told to retry.
7. The live tools, in the P2 section's list, with the unattended scope (a scoped flag
   on EditorContext: modals suppressed for the call and reported in its result); then
   P2b.
   FIRST STEP BUILT 2026-09-25 (settled the page system before the tools, per the user):
   a page publishes the interfaces it implements (IPageService, EditorPage::Provide /
   Service by type); ISceneEditorPage in Editor.Scene is the page-level surface of a scene
   or prefab page (its SceneEditContext + the Simulate control; ViewportToolHostContext
   stays the framework-level one); the scene editor contributes selection_get /
   selection_set / simulate_start / simulate_stop, page-addressed by the asset guid,
   defaulting to the active page. page_reload refreshes the page in place through its own
   OnAssetExternallyModified (force = the page's DiscardChanges first), and a scene_write /
   prefab_write over an open page's asset reaches it the same way (ProjectSession::
   onAssetWritten, wired by the editor host to EditorContext::NotifyAssetExternallyModified):
   a clean page reloads, a dirty page keeps its edits and warns. Found on the way: LoadScene
   ignored the reader's verdict (fixed), and the tracked PaperKid streams were a month stale
   (re-stamped; the sample test parses every stream now). BUILT 2026-09-26, the action
   bridge and the unattended scope, on the editor's action system
   (Documentation/Archive/editor-actions-history.md): action_list / action_state / action_execute over
   the registry, executed through the one funnel every surface uses; unattended = the UI
   context's DialogInterceptor for the call's duration, every dialog closed as cancelled (the
   accident-preventing answer) and named under `suppressedDialogs` with the note; no declared
   per-question answers yet (a flow that must PROCEED unattended will declare one when a real
   case appears; MCP elicitation stays the route for asking). entity_inspect BUILT 2026-09-26: the
   inspector's view of one entity through reflection (Properties / GetProperty, nested
   structures and lists expanded, enums by name), with reference-shaped values read by
   identity through the new ReferenceTraits customization point on TypeInfo (Resource
   marks Ref<T>; no per-type ladder anywhere). P2b's property writes BUILT
   2026-09-26: component_set through SceneEditContext (leaves by Variant, enums through the
   raw path, entity refs, and a generic reference command over the TypeInfo's ReferenceOps),
   one locked undo group labelled "mcp" per call so an agent's calls never merge with each
   other or the user's scrubs, a failed command dropped not pushed, refused while simulating,
   nothing saved implicitly; the undo tool is action_execute edit.undo over the active page.
   Nested structures and lists are not writable through it yet (scene_write edits the
   source). viewport_screenshot BUILT 2026-09-26 with viewport_camera_get / viewport_camera_set:
   the page owns the capture (ISceneEditorPage::RequestViewportCapture, recorded off the
   viewport's finished colour target in ShaderRead by the shared ScreenshotCapture, completed
   next frame), the tool returns NotFinished each pump until Written and gives up after ten
   seconds; the camera tools move the EditorCamera in degrees (lookAt wins), editor state only.
   The image is the view as the user sees it: the grid, the selection's gizmo and the gizmo's
   overlay text are in it, because the editor writes them to the PER-VIEW debug list keyed by
   the viewport (RenderScene's viewportKey, Views.cppm SetDebugView; the gizmo text is a debug
   draw text in the same list). A debug-draw-free shot is therefore NOT a flag on this capture
   but a second render, the way the camera preview (task #118) already gets one: RenderScene
   into a private offscreen target under its OWN key (an empty debug list), the per-scene
   debug list aside, captured from that target - the user's viewport untouched. Parked until a
   need shows; the user ruled it unnecessary for the tool now (2026-09-26).
   REMAINING in 7: the Agent panel.
The skill + McpGuide gain the editor recipe (`claude mcp add --transport http` with the
bearer header) in the commit that makes the host reachable (5).

## P2G - the GAME host (new phase; after P2, shares its transport)

Neither surveyed engine can test a game without a display, and ezEngine
documents the resulting weakness itself ("quite some lag, don't expect it
to play a game"). We can be categorically better, because the pieces
already exist in-engine:

- The run host / GameInstance serves MCP via `-mcpport <n>` (dev builds
  only; no flag = no server - the ezEngine convention).
- **Headless + offscreen**: the game renders offscreen through the RHI
  capture path (the web probe tests prove pixels-without-a-display today).
  `screenshot` returns a file path. Agents - and CI - verify GAMES by
  rendered evidence with no window, no GPU display surface.
- **Deterministic stepping**: `game_step(frames)` drives the fixed-timestep
  loop manually (the per-scene time infrastructure exists). Input injected
  for stepped frames is FRAME-EXACT - not "inject and hope", the lag
  problem ezEngine accepts. `game_info`, `game_pause`, `game_speed` ride
  the same clock.
- **Input injection** with the ezEngine merge rule: injected values merge
  with real devices, larger magnitude wins - a watching human is never
  locked out.
- **Event-bus injection** (OURS ALONE): `game_send_event(name, payload)`
  posts into the name-keyed EventBus (StringHash + Variant - the
  game-ready-scripting surface), and an SSE event stream can surface bus
  traffic back. Agents poke gameplay at the SEMANTIC level ("emit 'door
  opened', screenshot, assert the cutscene started") instead of only
  synthesizing raw input. No surveyed engine has a name-keyed bus to
  expose.
- Live state reads via reflection RESOLVE mode (entity_inspect against the
  running scene), log_read/log_write, host_info.

Acceptance: an Integration golden that launches the host headless, steps
frames deterministically, injects input + a bus event, captures a
screenshot, and asserts on pixels - the agent-testing loop, proven in CI.

## P3 - the knowledge layer + polish informed by use

- SKILLS AS ASSETS (adopted from Traktor - their best idea, upgraded by our
  stack): a reflected SkillAsset type {name, description, whenToUse,
  markdown body with {{param}} substitution, typed parameters,
  engineVersion, published} living in the project DB, authored in the
  editor (bespoke-pages recipe or the generic reflected page). Published
  skills surface over MCP prompts/list+get - slash commands in the client.
  `skill_create` is itself a tool: an agent that works out a non-obvious
  procedure PERSISTS it for every later session - the knowledge-capture
  loop, versioned with the project. The prompts capability enters the
  advertised set ONLY when this lands (the v1 no-prompts ruling stands
  until then).
- SSE consumers: progress notifications for long cooks/exports (the
  notFinished pattern's streaming sibling), resource subscriptions (file
  watch over sources).
- Build-time self-description (Traktor technique, parked until needed):
  when a tool must describe behavior reflection cannot see (codegen
  semantics, shader node behavior), EXTRACT the description from the source
  of truth at build time - never hand-write docs that drift. script_api
  already embodies the stronger live form.
- Whatever real agent sessions reveal. Do not speculate past this list.

### QUEUED RULING (Fable, 2026-08-23): import-tool importer disambiguation

Surfaced by the terrain track (Opus flag in terrain.md Build progress; ruling
recorded there and owned HERE - MCP is Fable's track, follow-up later): the
import tool resolves an extension with the SINGULAR ImporterRegistry::FindFor,
so the FIRST registered claimant always wins - `.png` is claimed by Texture,
then Image, then Heightfield, meaning an agent can never import a .png as an
Image or a Heightfield. Pre-existing (Texture/Image collided already); the
heightfield importer made it a three-way. The interactive editor is unaffected
(drag-drop uses FindAllFor + a chooser menu).

RULED FIX (land with the next MCP touch):
- The import tool resolves via FindAllFor (the interactive path's call).
- New OPTIONAL `importer` parameter selects among claimants by name.
- WITHOUT the hint, ambiguity keeps the first-claimant default (compat) but
  the tool RESULT names the alternatives ("imported as Texture; also
  claimable by: Image, Heightfield") so an agent can re-import with the hint.
- An unknown hint errors, listing the valid claimants.
- Agent-parity with the human chooser; no registry changes; tests = the
  three-way .png fixture (default + each hint + bad hint).

## Where we are deliberately BETTER (the differentiators - do not trade away)

1. TRUTH-FIRST SURFACES: script_api/type tools resolve live registries at
   call time, never caches; every registration set is a composition root
   with a count tripwire. Neither surveyed engine tests its tool surface
   against drift; we pin ours in CI.
2. THE SCRIPTING SURFACE: script_api (per-backend bound API + {domain,
   inPlayer}) and the script_validate ladder (compile-check -> Luau-typed
   errors against the real surface). Neither engine has ANY scripting
   surface for agents.
3. HEADLESS STRUCTURAL AUTHORING: text scenes/prefabs mean full structural
   editing through files with validation - ezEngine needs a live editor for
   structure; we need a text diff.
4. AGENT-TESTABLE GAMES (P2G): offscreen rendering + deterministic stepping
   + event-bus injection = game verification with no display, CI-able,
   frame-exact. Both engines require a window and accept input lag.
5. AGENT-SHAPED INTEGRATION GOLDENS: the Integration.Mcp suite runs the
   agent's actual call sequences headless in CI. Neither engine tests the
   agent workflow itself.
6. DUAL TRANSPORT: stdio (zero-setup for local agents - no curl bootstrap,
   no port juggling) AND HTTP/SSE for live processes. ezEngine is HTTP-only
   and needs a skill just to say hello; Traktor likewise.

## Explicitly out / later

- HTTP CLIENT (fetching remote data) - unrelated to MCP serving; plan
  separately if a real need appears.
- Multi-user / remote (non-localhost) access - localhost + token only.
- The wasm/browser build serving MCP - no.

## Dependency policy check

No new third-party deps: JSON is hand-rolled (XML DOM precedent), stdio is
free, the HTTP/1.1 + SSE server in P2 is minimal and shares the WS accept
work, P2G rides the same transport plus existing engine seams (RHI capture,
fixed-step clock, EventBus), and skills are a plain reflected asset type.
This keeps the offline-build + no-dep-without-approval rules intact.
