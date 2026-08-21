// Engine::SceneSurface - implementation unit (the wide subsystem imports live here, keeping the
// interface BMI lean). The full scene composition is built ONCE from a single per-domain module
// list; AddAllSceneManagers / RegisterAllSceneComponentReflection are thin wrappers over that
// composition. A manager added to a domain's Add<Domain>SceneManagers function reaches the
// composition automatically - there is no parallel list (and no count tripwire) to drift.

module;
#include "Core/Prelude.h"

module engine.scenesurface;

import foundation.core;
import foundation.scene;
import foundation.net.replication;
import engine.render;
import engine.animation;
import engine.particles;
import engine.physics;
import engine.navigation;
import engine.audio;
import engine.script;
import engine.ui;

using namespace foundation::core;
namespace scene = foundation::scene;

namespace
{
    // One module per domain, each pairing the domain's manager-install function with its component
    // reflection registrar - the SAME functions the runtime subsystems' OnSceneCreated delegate to.
    // Order matters only in that it binds the default (dependency-free) instantiation order; it mirrors
    // the old AddAllSceneManagers call order so behavior is byte-for-byte identical.
    const scene::SceneModule kRenderModule{u8"render", &engine::render::AddRenderSceneManagers,
                                           &engine::render::RegisterRenderComponentReflection};
    const scene::SceneModule
        kAnimationModule{u8"animation", &engine::animation::AddAnimationSceneManagers,
                         &engine::animation::RegisterAnimationComponentReflection};
    const scene::SceneModule
        kParticleModule{u8"particles", &engine::particles::AddParticleSceneManagers,
                        &engine::particles::RegisterParticleComponentReflection};
    const scene::SceneModule
        kPhysicsModule{u8"physics", &engine::physics::AddPhysicsSceneManagers,
                       &engine::physics::RegisterPhysicsComponentReflection};
    const scene::SceneModule
        kNavigationModule{u8"navigation", &engine::navigation::AddNavigationSceneManagers,
                          &engine::navigation::RegisterNavigationComponentReflection};
    const scene::SceneModule kAudioModule{u8"audio", &engine::audio::AddAudioSceneManagers,
                                          &engine::audio::RegisterAudioComponentReflection};
    const scene::SceneModule kScriptModule{u8"script", &engine::script::AddScriptSceneManagers,
                                           &engine::script::RegisterScriptComponentReflection};
    const scene::SceneModule kUiModule{u8"ui", &engine::ui::AddUISceneManagers,
                                       &engine::ui::RegisterUIComponentReflection};
    const scene::SceneModule kNetModule{u8"net", &foundation::net::AddNetworkSceneManagers,
                                        &foundation::net::RegisterReplicationComponents};

    const scene::SceneModule* kAllModules[] = {
        &kRenderModule,   &kAnimationModule, &kParticleModule, &kPhysicsModule, &kNavigationModule,
        &kAudioModule,    &kScriptModule,    &kUiModule,       &kNetModule,
    };
}

namespace engine
{
    const scene::SceneComposition& FullSceneComposition()
    {
        static const scene::SceneComposition composition =
            scene::SceneComposition::Build(Span<const scene::SceneModule*>{kAllModules});
        return composition;
    }

    void AddAllSceneManagers(foundation::scene::Scene& scene)
    {
        FullSceneComposition().Instantiate(scene);
    }

    void RegisterAllSceneComponentReflection()
    {
        FullSceneComposition().RegisterReflection();
    }
}