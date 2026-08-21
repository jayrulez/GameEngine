// Engine.SceneSurface.Tests - the scene-surface composition root.
//
// The full scene set is now ONE composition (scene-composition.md): a single per-domain module list
// whose `install` entries ARE the domain Add<Domain>SceneManagers functions. The old count tripwire
// (`kSceneSystemCount`) is gone because its failure mode - a manager added to a domain function but
// forgotten from the parallel headless list - is structurally impossible now (there is one list, and
// it delegates to the domain functions). What remains worth guarding: every DOMAIN is present (the
// module count), and the historically-dropped managers resolve (each was missing from the export
// tool's private copy of the list at some point - see Tools.Export history).
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.scene;
import foundation.net.replication;
import engine.scenesurface;
import engine.render;
import engine.animation;
import engine.script;
import engine.ui;
import engine.audio;

using namespace foundation::core;
namespace scene = foundation::scene;

TEST_CASE("engine.scenesurface: full composition covers every domain")
{
    // One module per engine domain + net (the single source of truth in SceneSurfaceImpl).
    CHECK(engine::FullSceneComposition().ModuleCount() == 9u);

    // Reproducing the aggregate: Instantiate yields the full manager set with no parallel list.
    scene::Scene scratch(u8"surface");
    engine::AddAllSceneManagers(scratch);

    // The historically-dropped ones stay present by name (each was missing from the export
    // tool's private copy of this list at some point).
    CHECK(scratch.HasSystem<engine::animation::PropertyAnimatorComponentManager>());
    CHECK(scratch.HasSystem<engine::render::PostProcessSystem>());
    CHECK(scratch.HasSystem<engine::script::ScriptComponentManager>());
    CHECK(scratch.HasSystem<engine::ui::UICanvasComponentManager>());
    CHECK(scratch.HasSystem<engine::audio::AudioSourceComponentManager>());
    CHECK(scratch.HasSystem<foundation::net::NetworkComponentManager>());

    // Serialization routing works: on-disk type ids resolve to their managers.
    CHECK(scratch.FindManagerBySerializationId(u8"net.Network") != nullptr);
    CHECK(scratch.FindManagerBySerializationId(u8"no.such.component") == nullptr);
}

TEST_CASE("engine.scenesurface: reflection registration is callable and idempotent")
{
    engine::RegisterAllSceneComponentReflection();
    engine::RegisterAllSceneComponentReflection(); // second call must be harmless
}