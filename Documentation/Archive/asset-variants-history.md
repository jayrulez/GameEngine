# Per-platform asset variants + texture compression

> ARCHIVED 2026-11-02, fully built; anything it left open is in
> Documentation/Plans/weekly_backlog.md ("Left over from the specs archived 2026-11-02"). Non-
> authoritative: the original spec, kept as the record of what was built and why; the code and the
> tests are the truth.

> STATUS: APPROVED shape (user + Fable, 2026-08-15), ready to build after
> property animation (or interleaved at review gates - coordinate build windows).
> Encoder dependencies (bc7enc/rgbcx + astcenc, vendored) and meshoptimizer
> (for the sibling mesh-lod spec) are USER-APPROVED 2026-08-15.
> Supersedes the sketch in Documentation/Backlog/issues-triage.md I6.
>
> VERIFICATION GAP (user, 2026-08-16): DX12/Windows has NOT run the BC upload
> path or the compressed-texture GPU probes - Vulkan + WebGPU only. Tracked in
> KNOWN_ISSUES.md; next Windows session runs them on DX12 (block row-pitch is
> the risk spot).

## Why this is one spec, not two

Texture compression is the first REAL producer of platform-divergent cooked
data: desktop wants BCn, future mobile wants ASTC, and the same source asset
must cook differently per export target. Rather than bolt a texture-only
switch into the cook, this spec builds the general PER-TARGET VARIANT AXIS
the pipeline already reserved ("cooked per-target platform variants (one
cooked DB per target - the recipe hash already has a platform-salt seam)" -
Documentation/Systems/asset-pipeline.md, Deferred). Texture compression is
V1's consumer; shader packs, audio codecs, and mesh quality tiers ride the
same axis later without new architecture.

## Decision 1 - variant axis: one cooked DB per target (lazy), NOT
## per-asset variant streams

Weighed 2026-08-15:

- Per-asset variant streams in ONE DB store platform-agnostic products once,
  but change the cooked-DB schema, touch every reader (editor, player,
  export, MCP), split cache invalidation per-variant, and buy that
  complexity for what is today ONE producer (textures).
- Per-target DBs keep the schema untouched and the mental model flat: a
  cooked DB IS a platform's view of the project. The cost is duplication of
  platform-invariant products - addressed below.

RESOLUTION: per-target cooked DBs, with the two sharpenings that remove the
size objection:

1. **Lazy materialization.** The HOST platform DB is the only always-warm
   DB - the editor dev loop is unchanged. A target DB is created/refreshed
   only when an export preset for that target runs (or an explicit
   `Tools.Cook --target <platform>`). No target export = no second DB.
2. **Platform-invariant copy-forward.** Builders declare variance (see
   Decision 3). An INVARIANT product whose recipe hash (minus the platform
   salt) matches the host DB's is copied forward into the target DB, never
   recooked. Scenes, scripts, prefabs, audio, input maps, fonts, and (until
   mesh tiers exist) meshes all copy forward; only genuinely variant bytes
   (textures) cook twice. Disk duplication is bounded by export staging,
   which already copies everything it ships.

## Decision 2 - encoders: vendor bc7enc/rgbcx AND astcenc now

User picked both-now (2026-08-15). BCn (bc7enc/rgbcx: BC1/3/4/5/7 + BC6H
via bc7enc's companion or rgbcx HDR path - confirm exact repo coverage at
vendor time) serves desktop and desktop-web. astcenc (ARM's reference
encoder) has a REAL consumer from day one: mobile-web, in scope now
(Decision 4) - the web export's ASTC variant pak.
Both vendored under ThirdParty/ per the SDL precedent, MSVC-native, no
toolchain additions (NO ispc).

## Decision 3 - where things live (library shape)

- `ThirdParty/bc7enc`, `ThirdParty/astcenc` - vendored, untouched layout,
  distinct archive names (the imgui case-collision lesson, 3bb34756).
- `Code/Pipeline/Texture.Compression` (module `texture.compression`) - the
  ONLY code that includes encoder headers. Exposes:
  - `EncodeBlockCompressed(image, TextureCompressionFormat, quality) -> bytes`
    per mip level (caller iterates the existing mip chain);
  - `ResolveCompressedFormat(usage, colorSpace, alpha, TargetProfile) ->
    rhi::TextureFormat` - THE POLICY TABLE (Decision 5) in one place.
    `TargetProfile` is a CAPABILITY struct ({bc, astc, etc2} + quality
    default), derived from the export target - the resolver asks "what
    format families does this target support", never "which platform is
    this" (adopted from the 2026-08-15 external review). Consoles/new
    targets later = a new profile, not new resolver branches.
  Pipeline-side and UI-free (the Pipeline rule); heavy third-party headers
  stay in implementation units (GCC module hygiene rule).
- `Code/Pipeline/Pipeline.Core` - `AssetBuilder` gains
  `Variance() -> PlatformInvariant | PlatformVariant` (default INVARIANT so
  every existing builder keeps its behavior; TextureAssetBuilder overrides).
  The recipe hash gains the platform salt ONLY for variant builders - so
  turning on the axis does not dirty every invariant product.
- `Code/Pipeline/Texture.Pipeline` - the consumer: after the existing mip
  generation, encode each level via Texture.Compression when the resolved
  policy says compress. Builder `Version()` 2 -> 3 IN THE SAME COMMIT as
  the product change (the bump IS the migration).
- Runtime: NO new code paths beyond upload. `TextureResource` already
  carries `rhi::TextureFormat`; RHI already enumerates BC1-BC7 + ASTC and
  the WebGPU adapter already reports `textureCompressionBC`/`ASTC`.
  Verify block-compressed upload row-pitch handling per backend (WebGPU
  bytesPerRow is per BLOCK row, 256-byte aligned - the strict path, per
  the WebGPU-stricter rule; validate on the WGSL path too).
- Editor: import dialog + texture page gain the authored compression choice
  (`Editor.Texture`). TWO asset fields (adopted from the external review -
  inference alone cannot classify a standalone PNG):
  - `usage = Color | Normal | Mask | HDR` - the SEMANTIC hint the policy
    table keys on. Model import sets it automatically from the material
    slot (normal/mask slots known); standalone imports default to Color
    with a dropdown.
  - `compression = Default | None | Quality` (Default = policy table;
    None = today's raw path, the escape hatch; Quality = force
    BC7/ASTC-4x4).
  Authored data lives on the ASSET (never the runtime struct - the
  no-editor-data-in-runtime rule).

## Decision 4 - the web answer (user asked 2026-08-15)

WebGPU guarantees exactly one compressed-texture family per device:
`texture-compression-bc` on DESKTOP browsers (Windows/macOS/Linux),
ASTC and/or ETC2 on MOBILE browsers. One exported web bundle may be opened
by either - the cook cannot know the client at cook time.

RESOLUTION (mobile-web is IN SCOPE NOW - user directive 2026-08-15; we
already ship web, so the web export must serve whatever browser opens it):

- The web export produces TWO variant paks from the same axis: **BC**
  (desktop browsers) and **ASTC** (mobile browsers). The wasm boot selects
  the pak by the adapter feature flag we already detect
  (`textureCompressionBC` / `ASTC` in WebGpuAdapter). Variant-selection-
  at-load, NOT runtime transcoding - no Basis Universal dependency, no
  wasm transcoder cost.
- PRECURSOR (before any compression work makes this urgent): smoke-test
  the CURRENT web build on a mobile browser (Chrome Android; iOS Safari
  where WebGPU is enabled). Today's textures are uncompressed RGBA, which
  every WebGPU device accepts - so if the current build fails on mobile,
  the failure is input/surface/memory, not formats, and it blocks this
  spec's web acceptance. Added to the UAT web session.
- ETC2: only if an ASTC-less mobile budget device ever matters; noted as
  the escape hatch (etc2comp-class encoder), deliberately NOT vendored.

## Decision 5 - the format policy table (Default mapping)

| Source (by authored `usage`) | BC-capable profile | ASTC-capable profile |
|---|---|---|
| sRGB color, no alpha | BC1 sRGB (fast) or BC7 sRGB (Quality) | ASTC 6x6 / 4x4 |
| sRGB color + alpha | BC7 sRGB (BC3 only under a size knob) | ASTC 4x4 |
| Normal map (Linear RG) | BC5 | ASTC 4x4 (LA swizzle) |
| Single channel (mask/height) | BC4 | ASTC 4x4 |
| HDR (RGBE/half) | BC6H | ASTC HDR profile |
| UI/pixel-art/small (<= 64px) or None | uncompressed (unchanged) | uncompressed |

Mip composition: generate the existing mip chain FIRST (sRGB-correct
averaging already shipped), then encode every level. Levels smaller than
the 4x4 block are padded per the BC/ASTC spec (encoder handles it); NPOT
textures are fine (block-ceil per level). The 5,592,404-byte full-chain
verification case becomes ~1/4 (BC7) or ~1/8 (BC1) of that - assert the
exact expected size in the cook test.

## Phasing

- **P1 - desktop BCn end-to-end (this week's build item).** Vendor both
  encoders (astcenc builds but is only exercised by tests). Texture.
  Compression lib + policy table; TextureAssetBuilder encodes (v3);
  authored knob + import dialog; upload verified on Vulkan AND WebGPU
  (desktop-web BC). Acceptance: per-format PSNR-floor round-trip tests;
  cooked-product format + exact-size assertions; on-screen probe (a
  BC-cooked texture renders identically-enough on both backends); a real
  project's cooked DB size drop recorded in the commit message.
- **P2 - the variant axis.** Variance() + platform-salted recipe hash for
  variant builders; lazy per-target DB materialization driven by export
  presets (`Tools.Cook --target` for headless); invariant copy-forward;
  export staging reads the target DB. Acceptance: web export produces a
  web-target DB whose textures are BC and whose invariant products were
  copied not recooked (cook-stats assertion); desktop export byte-identical
  to pre-P2; editor dev loop untouched (host DB only).
  STATUS 2026-08-15: the CORE is DONE + tested (a-e). IAssetBuilder::Variance
  (TextureAssetBuilder = Variant), CookTarget{id,bc,astc,etc2} +
  AssetBuildContext::target, recipe platform-salt for variant builders (folds
  through read-deps), ContentDatabase::CopyContentForward, CookDriver
  SetTarget/SetCopyForwardSource + TryCopyForward (CookStats.copiedForward),
  CookForTarget + CookTargetFor helpers, `Tools.Cook --target <id>`
  materializing Cooked-<id>/ + .cache/<id>/ (sibling, Fable Q2). Cook tests: a
  web-astc target recooks the one variant product + copies the invariants
  forward; an invariant product reading variant content recooks per target.
  STATUS 2026-08-15: P2 COMPLETE (a-f). Export is target-aware (ExportContent
  packs a content-variant list; ExportProject/ExportOne thread it through both
  paths; CookVariantTargets reuses CookForTarget). Desktop byte-identical guard
  is executable (a junk sibling Cooked-web-astc/ present -> identical
  Content.pak). Editor dev loop untouched (host DB only). See the resolved
  design-questions section for the Fable rulings driving this.
- **P3 - mobile-web: ASTC variant + boot-time pak selection.** The web
  export preset produces the BC pak AND the ASTC pak; the wasm boot picks
  by adapter capability before resource binding starts. Acceptance: PSNR +
  format assertions for ASTC; desktop browser loads the BC pak, a mobile
  browser (or a forced-capability test hook) loads the ASTC pak and
  renders the probe scene. Precondition: the mobile-web smoke of the
  CURRENT build (Decision 4) has run, so format work isn't debugging
  input/surface issues at the same time.
  STATUS 2026-08-15: P3 BUILT. ASTC PSNR + format assertions landed in P1. The
  web export ships Content-bc.pak + Content-astc.pak (test: two paks, no
  Content.pak); the wasm boot (WebMain SelectAndFetchContentPak) probes the
  adapter's bc/astc and fetches the matching pak as Content.pak (builds for
  wasm). REMAINING for P3 acceptance: (1) the manifest deviation (built
  manifest-free by pak-name convention) needs Fable/user sign-off; (2) the
  BROWSER functional check - a desktop browser loads the BC pak + a mobile
  browser (or forced-capability hook) loads the ASTC pak - is a UAT item (no
  browser in the build env), gated on the Decision-4 mobile-web smoke precursor.
- **Deferred (do NOT build now):** ETC2, RDO/rate-distortion knobs,
  GPU-compute encoding, per-asset per-platform overrides in the UI.

## Prior art survey (Lumix / ezEngine / Traktor, 2026-08-15)

Surveyed the three engines' real sources after P2/P3 shipped (we had never
compared). Verdict: our approach is SHARPER on the two things this spec targets
(capability-keyed format + boot-time BC/ASTC selection); two of their cook-dedup
mechanics are more mature than ours and are the refinements to make IF a second/
third variant producer lands (today textures are the only one).

- **Lumix** - `rgbcx` (BC1/3/5 only) or Basis behind a compile-time `#ifdef`; no
  BC7/BC6H/ASTC; format auto from two booleans (normalmap, has-alpha). NO
  per-target axis (one content-hashed .res, one main.pak, backend frozen by
  compile flag); even its Basis path bakes the target BCn at cook time. Simplest,
  least capable. Good perceptual defaults worth noting: sRGB-correct mip
  downsample, alpha-COVERAGE preservation for cutouts, a stochastic normal-map
  downsampler.
- **ezEngine** - DirectXTex (BC1/4/5/6H) + bc7enc (BC7). Has
  `ezTexConvUsage{Color/Linear/Hdr/NormalMap/BumpMap}` + `CompressionMode{None/
  Medium/High}` - essentially OUR usage+compression knobs (independent
  validation), plus an Auto-usage that samples image CONTENT ("proper hue of
  blue" -> normal). BUT the format function only implements the PC/BC branch
  (Android commented out) - ASTC unreachable; keyed on platform NAME not
  capability. Variants via a platform-PROFILE system: invariant assets cooked
  ONCE into a shared `AssetCache/Common/`, REFERENCED by each profile's GUID->
  path table (`.ezAidlt`); only texture/cubemap/decal/etc. classes get a
  per-profile folder. Runtime picks a profile by OS name at boot.
- **Traktor** - closest prior art. squish + bc6h_enc + astcenc + etc1 + PVRTC
  (full range). Per-target output DB + per-target pipeline config; the platform
  salt is EMERGENT - a product is variant only if a platform-varying setting is
  reachable in its recursive dependency hash (`getPropertyIncludeHash`), so
  invariant products dedupe across targets via a shared content-addressed cache
  (even a networked team cache) with NO explicit copy-forward list. BUT no usage
  taxonomy (format = a global policy STRING "DXTn"/"ASTC"), the normal-map
  compress path is literally disabled (falls back to RGBA16F), ASTC hardwired to
  4x4, one format per target (no boot-time selection).

### Where we are ahead of all three
1. **Capability-profile policy table** (usage x profile -> BC/ASTC). ez keys on a
   platform name (BC-only in practice); Traktor on a global string with the
   normal path disabled + ASTC stuck at 4x4. Our per-usage correctness
   (mask->BC4, normal->BC5, HDR->BC6H, capability-not-platform) is more principled.
2. **Boot-time BC/ASTC pak selection from the actual adapter feature** - NONE of
   them do this (ez picks by OS name, Traktor one-format-per-target, Lumix
   single-target). The right model for the heterogeneous-GPU web case.

### Refinements worth making (their ideas > ours) - FUTURE, not now
1. **ez's shared `Common/` store + per-target REFERENCE tables > our physical
   copy-forward.** Cook invariant products once, store once, reference per target -
   vs duplicating into each target DB. Less disk / cook-IO, no drift risk. Our
   copy-forward is simpler and export staging duplicates anyway; revisit if a
   second variant producer or large invariant sets make duplication hurt.
2. **Traktor's EMERGENT include-hash salting > our explicit `Variance()` label.**
   Variance falls out of data-flow reachability, so there is no variant-builder
   list to mislabel. Ours is more auditable + predictable-cost (honest tradeoff);
   emergent is more automatic. If the variant set grows, move toward deriving
   variance from which salted settings a recipe actually reaches.
3. **Per-setting include/exclude-hash CONTRACT + a "why did this recook" hash
   log** (Traktor). Every knob declares whether it affects output bytes; the log
   makes rebuilds debuggable. Cheap discipline to adopt on our recipe hash.
4. Nice-to-haves: ez's image-CONTENT auto-usage (beyond our filename-suffix
   heuristic); Traktor's NETWORKED content-addressed cache (cook-once per-org).
   Lumix's alpha-coverage-preserving + stochastic-normal mip downsamplers.

### P1 status (2026-08-15) - COMPLETE

The whole desktop-BCn AND mobile-web-ASTC encode/format path is landed and
proven. Web support was DEVELOPED, not deferred (user directive): the ASTC
encoder + format plumbing are real, not test-only. What still sequences after
P2 is only the two-pak PRODUCTION + boot selection, which needs the variant
axis - not the encoder.

- bc7enc/rgbcx + astcenc vendored (`ThirdParty::Bc7enc` / `::Astcenc`);
  Texture.Compression = the ONLY encoder-header includer. Policy table
  (Decision 5) + EncodeBlockCompressed: BC1/3/4/5/7 (block loop) AND ASTC4x4
  (astcenc whole-image, scalar/portable). PSNR round-trip tests for both.
- Authored `usage` + `compression` knobs on TextureAsset (editor-data,
  reflected + a "Compression" group in the texture page). Normal-map import
  heuristic (_normal/... -> Normal + linear). Builder encodes the RGBA8 mip
  chain (2D file + embedded), Version 2 -> 3.
- RHI block helpers (BlockWidth/Height/Bytes, CompressedRowPitch/LevelBytes);
  block-aware per-level upload in the TextureResource factory; Vulkan
  compressed copy fixed (tightly-packed 0/0). ASTC added to the WebGPU format
  map + GetFormatSupport + requested at device creation (Vulkan already had
  it); the profile prefers BC when a target supports both (desktop-first).
- Cook test: format policy + exact per-level block-byte sizes + real size
  drop (128x128 sRGB color = 10936 B BC1 vs 87380 B raw, 8x).
- Real-GPU probe (Integration.TextureCompression): BC1/BC7/BC5 upload +
  sample correct hue on Vulkan AND WebGPU (covers the 256-align path); the
  ASTC-blue leg is capability-gated (self-skips on desktop, runs on mobile HW).

Next: P2 - the per-target variant axis (Variance() + lazy per-target DBs +
invariant copy-forward). That is the prerequisite for the web export to
PRODUCE the BC + ASTC paks and for boot-time selection (Decision 4).

### DDS sources (2026-09-22) - pass-through + decode fallback

Packages that ship BC-compressed DDS (Bistro) import without a re-encode:
`foundation.image.dds` reads the container (both header forms; bcdec is its
one implementation site, no longer test-only) and the texture builder passes
the levels through byte-for-byte when they fit the asset + target (the rules
above: format-by-usage, BC target, compression not None, the chain present
when mips are wanted). Everything else decodes level 0 and takes the image
path - so the ASTC variant of a DDS-backed asset still cooks (decode + astcenc),
None still means raw, and a BC5 normal re-encodes BC7-linear per the shader
rule. The model importer keeps referenced DDS files file-backed (no embedded
decode: a 2K BC1 stays 2.7 MB on disk instead of a 16 MB RGBA8 sidecar). Tests:
Image.DDS.Tests (10), texture.pipeline DDS cases (4), model-import DDS (1).

## Design questions for Fable - P2f + P3 (export + dist + web boot) - RESOLVED

Fable ruled 2026-08-15 (notes inline below). BUILD PLAN adopted from the rulings:
- Q2 fix DONE: `Tools.Cook --target` now roots the per-target DB at a sibling
  `Cooked-<id>/` (not `Cooked/<id>/`); host stays `Cooked/`, cache stays
  `.cache/<id>/`.
- Export (P2f/P3a): `ExportContent` packs a LIST of content variants (default =
  one `{key:"", Content.pak, Cooked/}`); the Web preset cooks web-bc + web-astc
  via `CookForTarget` (Fable Q4.4 - reuse, no forked cook) and packs two
  COMPLETE paks. Reachability scanned ONCE from the host DB, applied to both
  (Fable Q4.1) with a same-guid-set assertion. `shaders.dpak` stays single
  (Q4.3). Executable "desktop byte-identical" test with a sibling target DB
  present (Q2).
- Manifest (Q3): DEVIATION - built manifest-FREE (Fable: please accept or push
  back). Rationale: the dist manifest is a `ProjectSettings` payload SHARED with
  the editor Project.xml; adding `contentVariants` there bumps the shared
  version (desktop player.xml stops being byte-identical) and pulls in
  array-of-struct XML serialization. Instead the pak names are a CONVENTION
  (`Content-<key>.pak`) the web boot derives directly, with a `Content.pak`
  fallback for single-pak/old bundles. Net effect matches Fable's intent: no
  manifest section on desktop (byte-identical), the web boot fetches exactly one
  pak. Cost vs the manifest-list: on a single-pak web bundle the boot does one
  extra 404-fallback fetch (a web dist built by this engine always has both
  variants, so the common path never 404s). If you want the explicit
  `contentVariants` list, it's an additive follow-up - the boot already works.
- Web boot (P3b) - DONE, builds for wasm: `SelectAndFetchContentPak()` in
  WebMain MakeOptions creates a throwaway WebGPU backend, reads the adapter's
  `textureCompressionBC`/`ASTC`, fetches `Content-<key>.pak` AS `Content.pak`
  (loader unchanged, Fable Q1), tears the probe down before the app device
  init. Neither family (spec-impossible) or a missing variant pak -> the
  `Content.pak` fallback + a loud log. Belt device-init cross-check deferred
  (probe + real device share the adapter, so they cannot diverge on one page).
  Browser/mobile functional check is a UAT item (no browser in the build env).

Relevant facts (verified in code):
- Export: `Code/Editor/Editor.Core/Export.cppm` + `ExportImpl.cpp`.
  `ExportContent()` packs `Content.pak` from the project's hardcoded `Cooked/`
  dir (ExportImpl.cpp ~425) and reads `project.CookedDb()` for reachability
  pruning (~394). `ExportProject()` cooks (CookDriver over `project.CookedDb()`)
  then calls `ExportContent()`.
- Dist layout: `<out>/Content.pak` (products + scenes + script, one binary DB),
  `<out>/player.xml` (dist manifest, ProjectSettings shape), `<out>/shaders.dpak`.
- Export preset carries a platform STRING ("Win64"/"Linux64"/"Web") - no enum.
- Web boot: `Code/Engine/Engine.Player/WebMain.cpp` - `MakeOptions()` calls
  `FetchDistFile("player.xml"/"Content.pak"/"shaders.dpak")` synchronously
  BEFORE the app boots, so the content pak is fetched before the WebGPU device
  exists. The adapter's `textureCompressionBC`/`ASTC` flags are only known
  after device creation (inside Initialize).
- `Tools.Cook --target <id>` roots the per-target DB at `Cooked/<id>/` with
  `.cache/<id>/`. NOTE (my own concern): the desktop pack walks `Cooked/`
  RECURSIVELY (`PackTree(cookedMount, "")`), so a `Cooked/web-astc/` subtree
  would be swept into the DESKTOP `Content.pak` unless the per-target DBs live
  OUTSIDE `Cooked/` (e.g. `Cooked.web-astc/` sibling) or the pack walk is
  scoped. This affects "desktop byte-identical". Flagging for the ruling in Q2.

My proposed shape (react / redirect):
1. Desktop target (Win64/Linux64): unchanged - cook host, pack the single
   `Content.pak` from the host DB. Byte-identical to pre-P2.
2. Web target: cook TWO variant targets (web-bc, web-astc) via `CookForTarget`
   (invariant products copy-forward from host, only textures recook per
   target). Pack TWO paks (`Content-bc.pak`, `Content-astc.pak`). `player.xml`
   gains a small content-variants list `[{key:"bc",pak:...},{key:"astc",pak:...}]`;
   desktop dists keep the single pak + empty list.
3. Web boot: resolve the fetch-before-adapter order by fetching only
   `player.xml` (+ `shaders.dpak`) up front, creating the WebGPU device early
   in Initialize, reading the adapter bc/astc flags, then fetching + mounting
   the matching variant pak before the content loader runs. Fallback to
   `Content.pak` when the manifest has no variants (desktop / old bundle).

Questions:
- **Q1 (boot ordering).** Is moving the content-pak fetch to AFTER device-init
  in the web boot sound, or does the PlayerApplication / MEMFS / project-loader
  lifecycle forbid it? If risky, is a pre-Initialize adapter PROBE
  (`requestAdapter` only, read features, then fetch) safer despite
  `requestAdapter` being async while `FetchDistFile` is sync `emscripten_wget`?

  > **FABLE RULING (2026-08-15): the pre-boot adapter PROBE, inside
  > MakeOptions - do NOT reorder the app lifecycle.** The build already runs
  > ASYNCIFY and WebMain's own comment notes device bring-up uses the same
  > yield mechanism as the fetches - so a `requestAdapter` + Asyncify-wait in
  > MakeOptions is the established idiom, not a new trick. Sequence: fetch
  > `player.xml`; if it lists variants, probe the adapter (instance-level
  > requestAdapter, NO surface needed), read `textureCompressionBC/ASTC`,
  > fetch the matching pak under its variant name but SAVE it to MEMFS as
  > the name the loader expects; else fetch `Content.pak` exactly as today.
  > Two adapter requests per page (probe + real device init) are fine.
  > Belt: at device init, if the created device lacks the family the mounted
  > pak was chosen for (should be impossible - same adapter), fail loudly
  > with a log naming both, never render garbage.

- **Q2 (pak layout + dir roots).** Two separate paks vs one pak with both
  variant sub-trees selected by path prefix? And where do per-target cooked DBs
  live so the desktop pack stays byte-identical - `Cooked/<id>/` (needs a
  scoped pack walk) or a sibling `Cooked.<id>/` outside the desktop tree?

  > **FABLE RULING: two complete paks; per-target DBs move OUTSIDE
  > `Cooked/`.**
  > - Paks: `Content-bc.pak` + `Content-astc.pak`, each COMPLETE (invariant
  >   products duplicated across the two). One combined pak would make every
  >   mobile client download the desktop BC bytes - defeats the point. The
  >   dist-side duplication of scenes/scripts is server disk, which is cheap;
  >   each client downloads exactly one pak. A common+overlay 3-pak split is
  >   a NOTED size optimization, deliberately not v1 (mount-order
  >   complexity).
  > - DB roots: your recursive-pack-walk concern is real and the fix must be
  >   structural, not a scoped-walk blocklist (the next PackTree caller
  >   would re-trip it). Per-target DBs root at a SIBLING `Cooked-<id>/`
  >   (e.g. `Cooked-web-astc/`); host stays `Cooked/` untouched;
  >   `.cache/<id>/` can stay as shipped. Adjust P2e + its tests.
  > - LOCK IT WITH A TEST: desktop export of a project WITH materialized
  >   target DBs present is byte-identical to one without them. That test IS
  >   the "desktop byte-identical" constraint, executable.

- **Q3 (manifest schema).** Extend `player.xml` (ProjectSettings shape) with the
  variants list, or a sidecar? Keep it minimal.

  > **FABLE RULING: extend `player.xml`, no sidecar** - one fewer fetch, and
  > the manifest is already the first thing the boot reads. A minimal
  > `contentVariants` list of `{key, pak}` where key is the CAPABILITY
  > FAMILY ("bc"/"astc" - profile keys, not platform names, per Decision 3).
  > The section is ABSENT (not empty) on desktop dists - desktop `player.xml`
  > stays byte-identical. Absent/unknown section = single-`Content.pak`
  > fallback; the settings unknown-section passthrough precedent covers old
  > readers.
  >
  > **AMENDED - deviation ACCEPTED (Fable review, 2026-08-15 late).** The
  > build went MANIFEST-FREE: pak-NAME convention (`Content-bc.pak` /
  > `Content-astc.pak`, no `Content.pak` in a variant dist) with the boot
  > probing the adapter and falling back to `Content.pak` when the variant
  > fetch 404s (FileExists check after wget). Accepted because it is
  > STRONGER on the constraints than my original ruling: `player.xml` is
  > untouched on every platform (not merely absent-section), the dist names
  > were already a convention the boot hardcodes, and the fallback covers
  > legacy bundles with the only cost being one extra failed request on an
  > OLD single-pak web bundle. Conditions attached:
  > 1. The selection is logged (done - the probe LOG_INFO line) and the
  >    legacy-bundle fallback gets exercised in the UAT browser check.
  > 2. THE MANIFEST IS PLANNED, NOT CONTINGENT (user, 2026-08-15: quality
  >    tiers are foreseen "at the very least", locales later). It lands
  >    WITH the first CHOICE-DRIVEN variant axis (quality tiers), because
  >    that axis cannot be adapter-probed - tier and locale are deployment/
  >    user choices, so the boot needs a listing to resolve them. Shape
  >    when it lands: `contentVariants` lists variant DIMENSIONS; the boot
  >    resolves the capability dimension (bc/astc) by probe exactly as
  >    today, and choice dimensions (tier, locale) by config/query. The
  >    name convention stays valid as the single-dimension degenerate case,
  >    so today's dists keep working. Do NOT build the manifest before the
  >    second dimension exists.

- **Q4 (correctness traps).** Anything in the copy-forward-per-target + two-pak
  flow I'm missing - scenes/scripts staged once vs per-pak, reachability
  pruning per target, the shader pack (already platform-keyed via
  `StageShaderPack(preset.platform)`)?

  > **FABLE NOTES:**
  > 1. **Reachability: scan ONCE, apply to both paks.** The closure is a
  >    guid-set walk - identical across variants by construction. Run it on
  >    one DB (host or web-bc), reuse the root/closure set for both packs.
  >    Assert both paks carry the SAME guid set (set equality, not just
  >    count) - that assertion catches any variant-divergence bug forever.
  > 2. Scenes/scripts stage per-pak (each complete) - yes, by design (Q2).
  > 3. `shaders.dpak` stays SINGLE per web dist - already platform-keyed,
  >    compression-orthogonal. Do not duplicate it per variant.
  > 4. **Export must reuse the `CookForTarget` path** (the Tools.Cook
  >    --target codepath) for the web preset's two targets - no forked
  >    cook-for-export logic, or the two paths drift.
  > 5. Target-DB staleness between exports is handled by the normal dirty
  >    checks inside CookForTarget - no special-casing; just make sure the
  >    export runs it (never packs a stale target DB silently).
  > 6. Boot fallback order in WebMain: manifest variants -> matching pak;
  >    no variants -> `Content.pak` (old bundles + desktop). If the adapter
  >    reports NEITHER family (spec-impossible), fail with a clear log.

Constraints to hold: "variant-selection-at-load, no transcoder" (Decision 4);
"desktop byte-identical" (now executable via the Q2 test);
"editor dev loop untouched (host DB only)".

## Test notes

Doctest in Texture.Pipeline.Tests + Pipeline.Cook.Tests (variance/copy-
forward), both compilers, no UI deps in any Pipeline test target. The
count-tripwire pattern applies if a registration list grows. Re-import is
NOT required for existing projects (compression is cook-side; `Default`
policy applies on next cook after the version bump).
