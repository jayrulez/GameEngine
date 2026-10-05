// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Animation - animation.subsystem implementation unit: the component reflection bodies.
//
// Kept OUT of the :components interface partition: REFLECT_* bodies in an interface
// unit make GCC emit an unreadable gcm cluster for consumers (see gcc-module-interface-hygiene).
// Components.cppm declares RegisterAnimationComponentReflection(); this unit defines it and the
// RttiRegisterValue_* bodies.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

module engine.animation;
import engine.domain;
import foundation.animation.resource;
import foundation.propertyanimation.resource;

import foundation.core;
import foundation.script;
import foundation.script.facades; // ComponentOf<T> + RegisterExtra* (the script `.of` surface, Track A)
import foundation.runtime;
import engine.render; // RenderSubsystem::DebugScene (the IK components' debugDraw)

using namespace foundation::core;
using namespace foundation::animation;
namespace core = foundation::core;

namespace engine::animation
{

    REFLECT_VALUE(SkeletalAnimationComponent, "rtti::engine::animation")
    {
        builder.Attribute("displayName", String(u8"Skeletal Animation"))
            .Attribute("category", String(u8"Animation"))
            .DataVersion(2) // v1: meshEntities persist; v2: rootMotion
            .ReadsDataVersionsFrom(1) // a v1 record reads with root motion off
            // Script (Track A): SkeletalAnimationComponent.of(entity) -> live speed/startTime/autoPlay.
            // play/stop/setClip are player ops -> SceneAnimation.of(scene) (world ops keyed by entity).
            .Method<&foundation::script::ComponentOf<SkeletalAnimationComponent>,
                    SkeletalAnimationComponent>("of")
            .Property<&SkeletalAnimationComponent::skeleton>("skeleton")
            .Property<&SkeletalAnimationComponent::clip>("clip")
            .Property<&SkeletalAnimationComponent::meshEntities>("meshEntities")
            .PropAttribute("displayName", String(u8"Mesh Entities"))
            .Property<&SkeletalAnimationComponent::speed>("speed")
            .Property<&SkeletalAnimationComponent::startTime>("startTime")
            .Property<&SkeletalAnimationComponent::autoPlay>("autoPlay")
            .Property<&SkeletalAnimationComponent::rootMotionTarget>("rootMotionTarget")
            .PropAttribute("description",
                           String(u8"What Entity mode moves: a gameplay root holding the model (empty: this "
                                  u8"entity)."))
            .Property<&SkeletalAnimationComponent::rootMotion>("rootMotion")
            .PropAttribute("description",
                           String(u8"What the clip's root motion moves: nothing, this entity, the character at "
                                  u8"or above it, or a script's reading."));
    }

    REFLECT_VALUE(AnimationGraphComponent, "rtti::engine::animation")
    {
        builder.Attribute("displayName", String(u8"Animation Graph"))
            .Attribute("category", String(u8"Animation"))
            .DataVersion(2) // v1: meshEntities persist; v2: rootMotion
            .ReadsDataVersionsFrom(1) // a v1 record reads with root motion off
            // Script (Track A): AnimationGraphComponent.of(entity) -> live `active`; graph params
            // (setFloat/setBool/setTrigger) are player ops -> SceneAnimation.of(scene).
            .Method<&foundation::script::ComponentOf<AnimationGraphComponent>, AnimationGraphComponent>(
                "of")
            .Property<&AnimationGraphComponent::skeleton>("skeleton")
            .Property<&AnimationGraphComponent::graph>("graph")
            .Property<&AnimationGraphComponent::meshEntities>("meshEntities")
            .PropAttribute("displayName", String(u8"Mesh Entities"))
            .Property<&AnimationGraphComponent::active>("active")
            .Property<&AnimationGraphComponent::rootMotionTarget>("rootMotionTarget")
            .PropAttribute("description",
                           String(u8"What Entity mode moves: a gameplay root holding the model (empty: this "
                                  u8"entity)."))
            .Property<&AnimationGraphComponent::rootMotion>("rootMotion")
            .PropAttribute("description",
                           String(u8"What the graph's root motion moves: nothing, this entity, the character at "
                                  u8"or above it, or a script's reading."));
    }

    // The scene-bound animation handle: SceneAnimation.of(scene).play/stop/setClip (single clip) +
    // setFloat/setBool/setTrigger (graph params). `of` returns SceneAnimation by value (concrete
    // cross-backend return), like ScenePhysics. All world ops keyed by entity - they reach the
    // manager-owned runtime player.
    REFLECT_VALUE(SceneAnimation, "rtti::engine::animation")
    {
        builder.Method<&SceneAnimation::play>("play", {"entity"});
        builder.Method<&SceneAnimation::stop>("stop", {"entity"});
        builder.Method<&SceneAnimation::pause>("pause", {"entity"});
        builder.Method<&SceneAnimation::resume>("resume", {"entity"});
        builder.Method<&SceneAnimation::isPlaying>("isPlaying", {"entity"});
        builder.Method<&SceneAnimation::time>("time", {"entity"});
        builder.Method<&SceneAnimation::setTime>("setTime", {"entity", "seconds"});
        builder.Method<&SceneAnimation::setClip>("setClip", {"entity", "resourceId"});
        builder.Method<&SceneAnimation::setFloat>("setFloat", {"entity", "name", "value"});
        builder.Method<&SceneAnimation::setBool>("setBool", {"entity", "name", "value"});
        builder.Method<&SceneAnimation::setTrigger>("setTrigger", {"entity", "name"});
        builder.Method<&SceneAnimation::setIkTarget>("setIkTarget", {"entity", "worldPosition"});
        builder.Method<&SceneAnimation::ikReached>("ikReached", {"entity"});
        builder.Method<&SceneAnimation::ikError>("ikError", {"entity"});
        builder.Method<&SceneAnimation::rootMotionTranslation>("rootMotionTranslation", {"entity"});
        builder.Method<&SceneAnimation::rootMotionYaw>("rootMotionYaw", {"entity"});
        builder.Method<&SceneAnimation::of>("of", {"scene"});
        builder.Constructor(); // some backends only materialize constructible foreign classes
    }

    // Inverse kinematics (inverse-kinematics.md P2). The bone fields carry `boneName` (the same
    // attribute in both engines): the inspector offers the animator's bones for them.
    REFLECT_VALUE(TwoBoneIkComponent, "rtti::engine::animation")
    {
        builder.Attribute("displayName", String(u8"Two Bone IK"))
            .Attribute("category", String(u8"Animation"))
            .Method<&foundation::script::ComponentOf<TwoBoneIkComponent>, TwoBoneIkComponent>("of")
            .Property<&TwoBoneIkComponent::startBone>("startBone")
            .PropAttribute("boneName", true)
            .PropAttribute("description", String(u8"The chain's first bone (a thigh, an upper arm)."))
            .Property<&TwoBoneIkComponent::midBone>("midBone")
            .PropAttribute("boneName", true)
            .PropAttribute("description", String(u8"The joint that bends (a knee, an elbow)."))
            .Property<&TwoBoneIkComponent::endBone>("endBone")
            .PropAttribute("boneName", true)
            .PropAttribute("description", String(u8"The bone that reaches the target (a foot, a hand)."))
            .Property<&TwoBoneIkComponent::target>("target")
            .PropAttribute("description",
                           String(u8"The entity to reach. Empty: the point a script sets, else this entity."))
            .Property<&TwoBoneIkComponent::matchRotation>("matchRotation")
            .PropAttribute("description", String(u8"The end bone takes the target's rotation as well."))
            .Property<&TwoBoneIkComponent::pole>("pole")
            .PropAttribute("description", String(u8"Optional: the joint bends toward this entity."))
            .Property<&TwoBoneIkComponent::hingeAxis>("hingeAxis")
            .PropAttribute("description",
                           String(u8"The joint's axis in the first bone's space, for a chain that is straight "
                                  u8"with no pole and a straight bind pose."))
            .Property<&TwoBoneIkComponent::weight>("weight")
            .PropAttribute("range", Float4{0.0f, 1.0f, 0.01f, 0.0f})
            .Property<&TwoBoneIkComponent::fadeSeconds>("fadeSeconds")
            .PropAttribute("range", Float4{0.0f, 4.0f, 0.05f, 0.0f})
            .Property<&TwoBoneIkComponent::active>("active")
            .Property<&TwoBoneIkComponent::order>("order")
            .PropAttribute("description", String(u8"Lower runs first, across the animator's IK components."))
            .Property<&TwoBoneIkComponent::debugDraw>("debugDraw")
            .PropAttribute("description", String(u8"Draw the chain and its target while the scene runs."));
    }

    REFLECT_VALUE(AimIkBone, "rtti::engine::animation")
    {
        builder.Property<&AimIkBone::bone>("bone")
            .PropAttribute("boneName", true)
            .Property<&AimIkBone::share>("share")
            .PropAttribute("range", Float4{0.0f, 1.0f, 0.01f, 0.0f})
            .PropAttribute("description", String(u8"This bone's part of the turn still to go (the last: 1)."));
    }

    REFLECT_VALUE(AimIkComponent, "rtti::engine::animation")
    {
        builder.Attribute("displayName", String(u8"Aim IK"))
            .Attribute("category", String(u8"Animation"))
            .Method<&foundation::script::ComponentOf<AimIkComponent>, AimIkComponent>("of")
            .Property<&AimIkComponent::bones>("bones")
            .PropAttribute("description", String(u8"Root first; the last bone aims (a spine: 0.3, 0.5, 1)."))
            .Property<&AimIkComponent::target>("target")
            .PropAttribute("description",
                           String(u8"The entity to aim at. Empty: the point a script sets, else this entity."))
            .Property<&AimIkComponent::up>("up")
            .PropAttribute("description",
                           String(u8"Optional: the up axis leans toward this entity (else the animated up)."))
            .Property<&AimIkComponent::aimAxis>("aimAxis")
            .Property<&AimIkComponent::upAxis>("upAxis")
            .Property<&AimIkComponent::maxAngle>("maxAngle")
            .PropAttribute("range", Float4{0.0f, 180.0f, 1.0f, 0.0f})
            .PropAttribute("description", String(u8"Degrees from the animated direction."))
            .Property<&AimIkComponent::weight>("weight")
            .PropAttribute("range", Float4{0.0f, 1.0f, 0.01f, 0.0f})
            .Property<&AimIkComponent::fadeSeconds>("fadeSeconds")
            .PropAttribute("range", Float4{0.0f, 4.0f, 0.05f, 0.0f})
            .Property<&AimIkComponent::active>("active")
            .Property<&AimIkComponent::order>("order")
            .Property<&AimIkComponent::debugDraw>("debugDraw");
    }

    REFLECT_VALUE(FootIkLegBones, "rtti::engine::animation")
    {
        builder.Property<&FootIkLegBones::startBone>("startBone")
            .PropAttribute("boneName", true)
            .PropAttribute("description", String(u8"The thigh."))
            .Property<&FootIkLegBones::midBone>("midBone")
            .PropAttribute("boneName", true)
            .PropAttribute("description", String(u8"The shin (the knee bends)."))
            .Property<&FootIkLegBones::endBone>("endBone")
            .PropAttribute("boneName", true)
            .PropAttribute("description", String(u8"The foot."))
            .Property<&FootIkLegBones::hingeAxis>("hingeAxis")
            .PropAttribute("description",
                           String(u8"The knee's axis in the thigh's space, for a straight leg with a straight "
                                  u8"bind pose."));
    }

    REFLECT_VALUE(FootIkComponent, "rtti::engine::animation")
    {
        builder.Attribute("displayName", String(u8"Foot IK"))
            .Attribute("category", String(u8"Animation"))
            .Method<&foundation::script::ComponentOf<FootIkComponent>, FootIkComponent>("of")
            .Property<&FootIkComponent::legs>("legs")
            .PropAttribute("description", String(u8"Up to four legs, each thigh, shin, foot."))
            .Property<&FootIkComponent::pelvisBone>("pelvisBone")
            .PropAttribute("boneName", true)
            .PropAttribute("description", String(u8"Lowers so the lower foot can reach (empty: it stays)."))
            .Property<&FootIkComponent::rayUp>("rayUp")
            .PropAttribute("description", String(u8"The ground probe starts this far above the foot."))
            .Property<&FootIkComponent::rayDown>("rayDown")
            .PropAttribute("description", String(u8"And looks this far below it."))
            .Property<&FootIkComponent::groupMask>("groupMask")
            .PropAttribute("description", String(u8"The collision groups that count as ground."))
            .Property<&FootIkComponent::pelvisDropMax>("pelvisDropMax")
            .Property<&FootIkComponent::maxTilt>("maxTilt")
            .PropAttribute("range", Float4{0.0f, 90.0f, 1.0f, 0.0f})
            .PropAttribute("description", String(u8"Degrees a foot turns to its ground's slope."))
            .Property<&FootIkComponent::liftHeight>("liftHeight")
            .PropAttribute("description", String(u8"A foot animated higher than this is swinging and left alone."))
            .Property<&FootIkComponent::groundHeight>("groundHeight")
            .PropAttribute("description",
                           String(u8"The animation's ground along up: 0 when the model's origin is at its feet."))
            .Property<&FootIkComponent::raiseRate>("raiseRate")
            .Property<&FootIkComponent::lowerRate>("lowerRate")
            .Property<&FootIkComponent::weight>("weight")
            .PropAttribute("range", Float4{0.0f, 1.0f, 0.01f, 0.0f})
            .Property<&FootIkComponent::fadeSeconds>("fadeSeconds")
            .PropAttribute("range", Float4{0.0f, 4.0f, 0.05f, 0.0f})
            .Property<&FootIkComponent::active>("active")
            .Property<&FootIkComponent::order>("order")
            .Property<&FootIkComponent::debugDraw>("debugDraw");
    }

    REFLECT_VALUE(InstancedSkinningComponent, "rtti::engine::animation")
    {
        // Script (Track A): InstancedSkinningComponent.of(entity) -> live poseCount/speed (the crowd
        // skinning tunables). Pure data; the manager reads them each frame.
        builder
            .Attribute("displayName", String(u8"Instanced Skinning"))
            .Attribute("category", String(u8"Animation"))
            .Method<&foundation::script::ComponentOf<InstancedSkinningComponent>,
                    InstancedSkinningComponent>("of")
            .Property<&InstancedSkinningComponent::poseCount>("poseCount")
            .Property<&InstancedSkinningComponent::speed>("speed");
    }

    REFLECT_ENUM(RootMotionMode, "rtti::engine::animation")
    {
        builder.Value("Ignore", RootMotionMode::Ignore);
        builder.Value("Entity", RootMotionMode::Entity);
        builder.Value("Character", RootMotionMode::Character);
        builder.Value("Script", RootMotionMode::Script);
    }

    REFLECT_ENUM(PropertyLoopMode, "rtti::engine::animation")
    {
        builder.Value("Once", PropertyLoopMode::Once);
        builder.Value("Loop", PropertyLoopMode::Loop);
        builder.Value("PingPong", PropertyLoopMode::PingPong);
    }

    REFLECT_VALUE(PropertyAnimatorComponent, "rtti::engine::animation")
    {
        // Reflected property-curve animation: the clip Ref needs its InspectorView ref-picker dispatch
        // entry or no picker renders (known trap). Script (Track A): .of(entity) -> live tunables +
        // (Phase G) play/stop methods.
        builder.Attribute("displayName", String(u8"Property Animator"))
            .Attribute("category", String(u8"Animation"))
            .Method<&foundation::script::ComponentOf<PropertyAnimatorComponent>,
                    PropertyAnimatorComponent>("of")
            // Playback ops for scripts: animator.of(entity).play()/stop()/pause()/... (no facade lib).
            .Method<&PropertyAnimatorComponent::play>("play")
            .Method<&PropertyAnimatorComponent::stop>("stop")
            .Method<&PropertyAnimatorComponent::pause>("pause")
            .Method<&PropertyAnimatorComponent::resume>("resume")
            .Method<&PropertyAnimatorComponent::isPlaying>("isPlaying")
            .Method<&PropertyAnimatorComponent::currentTime>("time")
            .Method<&PropertyAnimatorComponent::setTime>("setTime", {"seconds"})
            .Property<&PropertyAnimatorComponent::clip>("clip")
            .PropAttribute("displayName", String(u8"Clip"))
            .Property<&PropertyAnimatorComponent::autoplay>("autoplay")
            .PropAttribute("displayName", String(u8"Autoplay"))
            .Property<&PropertyAnimatorComponent::speed>("speed")
            .PropAttribute("displayName", String(u8"Speed"))
            .Property<&PropertyAnimatorComponent::loopMode>("loopMode")
            .PropAttribute("displayName", String(u8"Loop Mode"));
    }

    void RegisterAnimationComponentReflection()
    {
        static const bool once = []()
        {
            // The meshEntities lists reflect as containers (inspector list editor + the prefab
            // spawn's EntityRef remap walker both introspect through ContainerInfo).
            core::RegisterArrayType<foundation::scene::EntityRef>();
            RttiRegisterEnum_RootMotionMode();
            RttiRegisterValue_SkeletalAnimationComponent();
            RttiRegisterValue_AnimationGraphComponent();
            RttiRegisterValue_InstancedSkinningComponent();
            RttiRegisterValue_TwoBoneIkComponent();
            RttiRegisterValue_AimIkBone();
            core::RegisterArrayType<AimIkBone>(); // the bone list (list editor + scripts)
            RttiRegisterValue_AimIkComponent();
            RttiRegisterValue_FootIkLegBones();
            core::RegisterArrayType<FootIkLegBones>();
            RttiRegisterValue_FootIkComponent();
            RttiRegisterEnum_PropertyLoopMode();
            RttiRegisterValue_PropertyAnimatorComponent();
            return true;
        }();
        (void)once;
    }

    void RegisterAnimationScriptFacade()
    {
        RegisterAnimationComponentReflection(); // ensure component TypeData (incl `of`) is built first
        // Surface the animation components to script (SkeletalAnimationComponent.of(entity), ...):
        // register them, seed the emission roots, and name them for the behavior prelude.
        struct Entry
        {
            const core::TypeInfo* type;
            core::StringView name;
        };
        const Entry components[] = {
            {&core::TypeOf<SkeletalAnimationComponent>(), u8"SkeletalAnimationComponent"},
            {&core::TypeOf<AnimationGraphComponent>(), u8"AnimationGraphComponent"},
            {&core::TypeOf<InstancedSkinningComponent>(), u8"InstancedSkinningComponent"},
            {&core::TypeOf<PropertyAnimatorComponent>(), u8"PropertyAnimatorComponent"},
            {&core::TypeOf<TwoBoneIkComponent>(), u8"TwoBoneIkComponent"},
            {&core::TypeOf<AimIkComponent>(), u8"AimIkComponent"},
            {&core::TypeOf<FootIkComponent>(), u8"FootIkComponent"}};
        for (const Entry& component : components)
        {
            GlobalTypeRegistry().Register(*component.type);
            foundation::script::RegisterExtraScriptRootType(component.type);
            foundation::script::RegisterExtraFacadeName(component.name);
        }

        // The scene-bound animation handle (SceneAnimation.of(scene)): reflect + register + seed + name.
        RttiRegisterValue_SceneAnimation();
        GlobalTypeRegistry().Register(core::TypeOf<SceneAnimation>());
        foundation::script::RegisterExtraScriptRootType(&core::TypeOf<SceneAnimation>());
        foundation::script::RegisterExtraFacadeName(u8"SceneAnimation");
    }
}

namespace engine::animation
{
    // The IK components on an entity, as the script calls see them.
    namespace
    {
        template <typename F>
        void ForEachIk(foundation::scene::Scene* scene, foundation::scene::EntityHandle entity, F&& visit)
        {
            if (scene == nullptr)
            {
                return;
            }
            if (auto* twoBone = scene->GetSystem<TwoBoneIkComponentManager>())
            {
                if (TwoBoneIkComponent* c = twoBone->Get(entity))
                {
                    visit(c->runtime);
                }
            }
            if (auto* aim = scene->GetSystem<AimIkComponentManager>())
            {
                if (AimIkComponent* c = aim->Get(entity))
                {
                    visit(c->runtime);
                }
            }
            if (auto* feet = scene->GetSystem<FootIkComponentManager>())
            {
                if (FootIkComponent* c = feet->Get(entity))
                {
                    visit(c->runtime);
                }
            }
        }
    }

    void SceneAnimation::setIkTarget(foundation::script::Entity entity, Float3 worldPosition) const
    {
        ForEachIk(scene, entity.Handle(),
                  [&](IkRuntime& rt)
                  {
                      rt.hasScriptTarget = true;
                      rt.scriptTarget = worldPosition;
                  });
    }

    bool SceneAnimation::ikReached(foundation::script::Entity entity) const
    {
        bool any = false;
        bool all = true;
        ForEachIk(scene, entity.Handle(),
                  [&](IkRuntime& rt)
                  {
                      any = true;
                      all = all && rt.status == IkStatus::Solving && rt.modifier.Get() != nullptr &&
                            rt.modifier->solved && rt.modifier->result.reached;
                  });
        return any && all;
    }

    f32 SceneAnimation::ikError(foundation::script::Entity entity) const
    {
        f32 worst = 0.0f;
        ForEachIk(scene, entity.Handle(),
                  [&](IkRuntime& rt)
                  {
                      if (rt.modifier.Get() != nullptr && rt.modifier->solved)
                      {
                          worst = Max(worst, rt.modifier->result.error);
                      }
                  });
        return worst;
    }

    void AnimationSubsystem::Update(f32)
    {
        foundation::runtime::Context* context = GetContext();
        auto* render = context != nullptr ? context->GetSubsystem<engine::render::RenderSubsystem>() : nullptr;
        if (render == nullptr)
        {
            return;
        }
        for (foundation::scene::Scene* scene : m_scenes)
        {
            auto* twoBone = scene->GetSystem<TwoBoneIkComponentManager>();
            auto* aim = scene->GetSystem<AimIkComponentManager>();
            auto* feet = scene->GetSystem<FootIkComponentManager>();
            if ((twoBone == nullptr || twoBone->Count() == 0) && (aim == nullptr || aim->Count() == 0) &&
                (feet == nullptr || feet->Count() == 0))
            {
                continue;
            }
            foundation::render::debug::DebugDraw& draw = render->DebugScene(*scene);
            if (twoBone != nullptr)
            {
                twoBone->DrawDebug(draw);
            }
            if (aim != nullptr)
            {
                aim->DrawDebug(draw);
            }
            if (feet != nullptr)
            {
                feet->DrawDebug(draw);
            }
        }
    }

    const engine::DomainModule& AnimationDomain() noexcept
    {
        static const foundation::resource::ResourceModule* const kResources[] = {
            &foundation::animation::kAnimationResourceModule,
            &foundation::propertyanimation::kPropertyAnimationResourceModule};
        static const engine::DomainModule kModule{
            .id = u8"animation",
            .installScene = &AddAnimationSceneManagers,
            .registerReflection = &RegisterAnimationComponentReflection,
            .registerScriptFacade = &RegisterAnimationScriptFacade,
            .resources = foundation::core::Span<const foundation::resource::ResourceModule* const>{
                kResources, sizeof(kResources) / sizeof(kResources[0])}};
        return kModule;
    }
}
