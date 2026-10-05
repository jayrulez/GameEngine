// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Engine::Composition - the `engine.composition` module.
//
// THE composition root: the one list of engine domains (Documentation/Specs/engine-composition.md,
// D5), each declared once in its own library (engine.domain's DomainModule), and the facets a
// consumer asks the list for - the scene composition every scene assembles from, the component
// reflection, the resource types, the factories a host can create, the script facades. The
// runtime (DefaultApplication), the editor, the headless tools (export, cook, the MCP hosts)
// and the samples compose from this root and from nothing else, so no executable can drift
// from the domains it links. It replaces Engine.Composition (the scene list) and
// Engine.Composition (the facade list), whose entry points keep their names below.
//
// The wide domain imports live in the implementation unit, keeping this interface BMI lean
// (GCC module-interface hygiene).

module;
#include "Core/Prelude.h"

export module engine.composition;

import foundation.core;
import foundation.content;
import foundation.scene;
import foundation.resource;
export import engine.domain;

using namespace foundation::core;

export namespace engine
{
    /// The full engine composition: every domain, in dependency order. Built once.
    [[nodiscard]] const EngineComposition& FullComposition();

    /// The scene facet: every serializable component manager and settings-bearing scene system
    /// across all domains. `Instantiate(scratch)` installs the full manager set;
    /// `RegisterReflection()` registers every scene component's reflection.
    [[nodiscard]] const foundation::scene::SceneComposition& FullSceneComposition();

    /// Adds EVERY serializable component manager and settings system to `scene`: the headless
    /// scratch scenes (export transcode, MCP validation, the scene format reference) and the
    /// runtime assemble from the same list.
    void AddAllSceneManagers(foundation::scene::Scene& scene);

    // ---- Scene export support (Sedulous's SceneExportSupport) ----
    // What an export needs from the full scene composition, for every host that exports (the
    // export CLI, the editor, the MCP hosts): the scene streams transcoded to the binary wire,
    // and a scene's direct references. Both load over EVERY manager, since a hand-kept set
    // silently drops the records it does not cover.

    /// The data stream a scene or prefab document keeps its text source in.
    inline constexpr StringView kSceneStream = u8"scene";

    /// Whether a content instance is a scene or a prefab document.
    [[nodiscard]] bool IsSceneLike(const foundation::content::Instance& instance);

    /// Every scene's and prefab's TEXT source under `group` transcoded to the binary wire, by
    /// guid; a prefab without its settings, a scene with. A stream that does not transcode is
    /// left out (the stager then stages it verbatim, and the runtime sniffs).
    void CollectSceneStreams(IAllocator& allocator, foundation::content::Group& group,
                             HashMap<Guid, Array<byte>>& outStreams);

    /// A scene's or prefab's DIRECT references: the component resource Refs, through a
    /// factory-less ResourceManager whose every bind lands unresolved, plus the parked prefab
    /// instance ids. False when the stored stream does not load.
    bool ScanSceneReferences(IAllocator& allocator, foundation::content::Instance& instance,
                             foundation::content::IContentDatabase& db, Array<Guid>& outResources,
                             Array<Guid>& outPrefabs);

    /// Registers every domain's component reflection (data-version gates + field metadata); must
    /// run once before any scene stream with component payloads deserializes. Idempotent.
    void RegisterAllSceneComponentReflection();

    /// Registers every resource module's types: the cooked records and products factories
    /// construct by type name. Idempotent.
    void RegisterAllResourceTypes();

    /// Registers the COMPLETE engine script surface: core types, the base behaviour facades
    /// (Entity/Log/Time/Random) and every domain's facade. Metadata only - no subsystem is
    /// instantiated, no device is created; safe in a fully headless host. Idempotent.
    void RegisterAllScriptFacades();

    /// Tripwire count (Engine.Composition.Tests asserts against this): the number of EXTRA facade
    /// names RegisterAllScriptFacades installs beyond the base behaviour facades - the domain
    /// facades. A new domain facade bumps this deliberately; a lost registration fails loudly.
    // 33 = +RayCastHit: ScenePhysics.rayCast returns the explicit hit-result value handle.
    // 34 = +DebugDraw: DebugDraw.of(scene) immediate-mode debug draw facade.
    // 36 = + SplineHit + SceneSplines.
    // 37 = +SceneScripts: SceneScripts.of(scene), the scene's script time, send/emit, addBehavior.
    // 38 = +Save: Save.getInt/setInt/..., the run's save data (kept between runs).
    // 40 = + TwoBoneIkComponent + AimIkComponent (inverse kinematics on scene entities).
    // 41 = + FootIkComponent.
    inline constexpr usize kSubsystemFacadeNameCount = 41;
}
