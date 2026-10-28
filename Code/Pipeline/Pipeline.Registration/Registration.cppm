// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Pipeline::Registration - the `pipeline.registration` module.
//
// The pipeline's COMPOSITION ROOT as a library. Every host that cooks or imports - the CLI
// cooker, the export packager, the editor, the headless MCP server - needs the SAME set of
// builders, importers, and product/resource type registrations. Registering that set inline in
// each host's main() duplicates it verbatim; a builder added to two of three copies is a silent
// gap (cook works in the editor, missing from CLI export - the collision-cook class of bug).
// This library is the single source of truth: it links every pipeline module (its entire job -
// the deliberate fan-in point) and exposes three entry points the hosts call instead of
// restating the list.
//
// The wide imports live in the implementation unit, not here: this interface stays lean (three
// declarations + the tripwire counts) so the four hosts that consume it do not each pull the
// whole pipeline into their BMI (GCC module-interface hygiene).

module;
#include "Core/Prelude.h"

export module pipeline.registration;

import foundation.core;
import foundation.content;
import pipeline.core;
import pipeline.importer;

using namespace foundation::core;

export namespace pipeline
{
    /// Register every asset/product/resource type, script backend, and per-language cook the
    /// pipeline ships, into the global type/serializable/cook registries. A headless cook needs
    /// these because ReadObject constructs cooked products BY TYPE NAME. Call once at startup,
    /// before planning a cook. Does NOT touch a BuilderRegistry (that is RegisterAllBuilders) and
    /// registers NO editor-UI services (those are the editor's own concern, kept host-side).
    void RegisterPipelineTypes();

    /// Populate `registry` with every asset builder the engine ships. Independent of
    /// RegisterPipelineTypes (constructs builder instances; needs no prior type registration),
    /// but a cook needs both. Idempotent per registry (a fresh registry each call). `dataRoot`
    /// is the engine data root the host resolved (its `--data-root`, else the discovery walk):
    /// a custom shader's cook compiles against the engine's shader library under it.
    void RegisterAllBuilders(BuilderRegistry& registry, StringView dataRoot);

    /// Populate `registry` with every OS-file importer the engine ships (the drag-drop / MCP
    /// asset_import surface): texture, model, UI, audio, script, font.
    void RegisterAllImporters(ImporterRegistry& registry);

    /// Populate `registry` with every New Asset creator the pipeline domains offer (File > New,
    /// asset_create), in the menu's order, the scripts' last. Run RegisterPipelineTypes first:
    /// the script creators are one set per language with a registered cook. Answers how many
    /// script creators that added (three per such language).
    usize RegisterAllCreators(AssetCreatorRegistry& registry);

    /// What the pipeline makes once an import has landed, beyond what the importer itself made:
    /// a model's prefab (and its scene, when asked). A host without the editor's import
    /// listeners (the stdio MCP host) runs this after every import so a model gets the same
    /// assets there as in the editor, whose scene module runs the same generation.
    void AfterImport(IAllocator& allocator, foundation::content::Instance& primary,
                     const ImportOptions* options);

    // === Tripwire counts (Pipeline.Registration.Tests asserts against these) ===
    // A new builder/importer bumps the matching constant DELIBERATELY; a lost registration then
    // fails the test loudly. This converts "nobody checks the three copies stay in sync" into
    // "the build checks the one copy is complete".
    /// The GUI's (a document and a style sheet), when the GUI is built (gui-lab-evolution-3.md E3).
    inline constexpr usize kGuiAssetKinds = 2;
    inline constexpr usize kBuilderCount = 31 + kGuiAssetKinds;
    inline constexpr usize kImporterCount = 13;
    /// The creators every build has; the scripts add three per language with a cook on top.
    inline constexpr usize kCreatorCount = 32 + kGuiAssetKinds; // 32: the Phong Material (phong-material.md)
}
