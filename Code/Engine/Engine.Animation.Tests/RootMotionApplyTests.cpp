// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Root motion applied by an animator's mode (root-motion.md P2): Entity moves and turns the
// animator's entity by the clip (through a moved and turned parent, over many loops); Ignore
// leaves it; Script holds the world delta for SceneAnimation; Character walks the nearest
// character at or above it through the scene's capability and lets it go when it stops; a v1
// record reads with root motion off. (The real physics character is Integration.Runtime's.)
#include <doctest/doctest.h>
#include "Core/Prelude.h"

#include <cmath>

import foundation.core;
import foundation.scene;
import foundation.animation;
import foundation.script.facades;
import foundation.xml;
import foundation.xml.serialization;
import engine.animation;
import engine.render;

using namespace foundation::core;
namespace scene = foundation::scene;
namespace animation = foundation::animation;
using engine::animation::RootMotionMode;

namespace
{
    // A clip that walks `metres` a second along +Z and turns `turn` radians a second.
    RefPtr<animation::AnimationClip> Walking(f32 metres, f32 turn = 0.0f)
    {
        RefPtr<animation::AnimationClip> clip = MakeRef<animation::AnimationClip>(DefaultAllocator(), u8"Walk", 1.0f, true);
        clip->rootMotion.horizontal = true;
        clip->rootMotion.yaw = turn != 0.0f;
        for (i32 i = 0; i <= 20; ++i)
        {
            const f32 t = static_cast<f32>(i) / 20.0f;
            clip->rootMotion.times.PushBack(t);
            clip->rootMotion.positions.PushBack(Float3{0, 0, metres * t});
            clip->rootMotion.yaws.PushBack(turn * t);
        }
        clip->GetOrCreatePositionTrack(0)->AddKeyframe(0.0f, Float3{});
        clip->GetOrCreatePositionTrack(0)->AddKeyframe(1.0f, Float3{});
        return clip;
    }

    struct Stage
    {
        scene::Scene level{DefaultAllocator(), u8"rm"};
        RefPtr<animation::Skeleton> skeleton = MakeRef<animation::Skeleton>(DefaultAllocator(), 1);
        scene::EntityHandle parent;
        scene::EntityHandle walker;

        explicit Stage(RefPtr<animation::AnimationClip> clip, RootMotionMode mode)
        {
            skeleton->Bones()[0].index = 0;
            skeleton->Bones()[0].parentIndex = -1;
            skeleton->FindRootBones();
            skeleton->BuildChildIndices();
            level.AddSystem<engine::render::MeshComponentManager>();
            engine::animation::AddAnimationSceneManagers(level);
            // A parent moved and turned a quarter: the walker's travel must come out in the world.
            parent = level.CreateEntity(u8"Herd");
            Transform p;
            p.position = Float3{10, 0, 0};
            p.rotation = Quaternion::FromAxisAngle(Float3{0, 1, 0}, kHalfPi);
            level.SetLocalTransform(parent, p);
            walker = level.CreateEntity(u8"Dog");
            level.SetParent(walker, parent);
            auto& a = level.GetSystem<engine::animation::SkeletalAnimationComponentManager>()->Add(walker);
            a.skeleton.SetDirect(skeleton);
            a.clip.SetDirect(clip);
            a.rootMotion = mode;
            level.UpdateTransforms(); // world positions read before the first tick are current
        }

        void Run(f32 seconds, f32 dt = 1.0f / 30.0f)
        {
            for (f32 t = 0.0f; t < seconds - 1.0e-4f; t += dt)
            {
                level.Update(dt);
            }
        }

        engine::animation::SkeletalAnimationComponent& Animator()
        {
            return *level.GetSystem<engine::animation::SkeletalAnimationComponentManager>()->Get(walker);
        }
    };
}

TEST_CASE("root motion: Entity mode walks the entity by its clip, through its parent, over many loops")
{
    Stage s{Walking(2.0f), RootMotionMode::Entity};
    const Float3 start = s.level.GetWorldPosition(s.walker);
    s.Run(3.0f);
    const Float3 moved = s.level.GetWorldPosition(s.walker) - start;
    // Three loops: 6 m along the walker's +Z, which the parent's quarter turn points along world +X.
    CHECK(Length(moved) == doctest::Approx(6.0f).epsilon(1e-3));
    CHECK(moved.x == doctest::Approx(6.0f).epsilon(1e-3));
}

TEST_CASE("root motion: Entity mode moves a named gameplay root, the model riding along under it")
{
    // The model (the animator) is a child of the gameplay root; moving the animator's own entity
    // would walk the model away from the root and its logic.
    Stage s{Walking(2.0f), RootMotionMode::Entity};
    s.Animator().rootMotionTarget = s.level.GetEntityId(s.parent);
    const Float3 root = s.level.GetWorldPosition(s.parent);
    s.Run(1.0f);
    CHECK(Length(s.level.GetWorldPosition(s.parent) - root) == doctest::Approx(2.0f).epsilon(1e-3));
    CHECK(Length(s.level.GetLocalTransform(s.walker).position) < 1.0e-6f); // the model stayed put under it
}

TEST_CASE("root motion: Entity mode turns with a turn clip")
{
    Stage s{Walking(0.0f, kHalfPi), RootMotionMode::Entity};
    s.Run(2.0f);
    const Float3 forward = RotateVector(s.level.GetLocalTransform(s.walker).rotation, Float3{0, 0, 1});
    CHECK(forward.z == doctest::Approx(-1.0f).epsilon(1e-3)); // a half turn in two seconds
}

TEST_CASE("root motion: Ignore leaves the entity where it was; Script holds the world delta")
{
    Stage ignore{Walking(2.0f), RootMotionMode::Ignore};
    const Float3 before = ignore.level.GetWorldPosition(ignore.walker);
    ignore.Run(1.0f);
    CHECK(Length(ignore.level.GetWorldPosition(ignore.walker) - before) < 1.0e-6f);

    Stage script{Walking(3.0f), RootMotionMode::Script};
    const Float3 still = script.level.GetWorldPosition(script.walker);
    script.Run(0.5f, 0.1f);
    CHECK(Length(script.level.GetWorldPosition(script.walker) - still) < 1.0e-6f); // moved nothing
    const foundation::script::Entity entity{&script.level, script.walker.index, script.walker.generation};
    const engine::animation::SceneAnimation animation{&script.level};
    const Float3 tick = animation.rootMotionTranslation(entity);
    CHECK(tick.x == doctest::Approx(0.3f).epsilon(1e-3)); // 3 m/s for 0.1 s, the parent turning +Z to +X
    CHECK(Abs(tick.z) < 1.0e-4f);
    CHECK(animation.rootMotionYaw(entity) == 0.0f);
}

namespace
{
    // The scene's character capability, as physics provides it, recording what it was told.
    class Characters final : public scene::SceneSystem, public scene::ISceneCharacterMotion
    {
    public:
        [[nodiscard]] scene::ISceneCharacterMotion* AsCharacterMotion() noexcept override { return this; }
        [[nodiscard]] bool HasCharacter(scene::EntityHandle entity) const override { return entity == character; }
        void MoveCharacter(scene::EntityHandle entity, Float3 velocity) override
        {
            CHECK(entity == character);
            last = velocity;
            ++moves;
        }
        scene::EntityHandle character = scene::EntityHandle::Invalid();
        Float3 last{};
        usize moves = 0;
    };
}

TEST_CASE("root motion: Character mode walks the character above it, and lets it go")
{
    Stage s{Walking(2.0f, 0.5f), RootMotionMode::Character};
    Characters* characters = s.level.AddSystem<Characters>();
    characters->character = s.parent; // the character is the walker's parent
    s.Run(0.5f, 0.1f);
    CHECK(characters->moves == 5u);
    CHECK(Length(characters->last) == doctest::Approx(2.0f).epsilon(0.01)); // m/s, horizontal
    CHECK(characters->last.y == 0.0f);
    // The character turns with the clip, from its quarter turn by half a second at 0.5 rad/s.
    const Float3 facing = RotateVector(s.level.GetLocalTransform(s.parent).rotation, Float3{0, 0, 1});
    CHECK(std::atan2(facing.x, facing.z) == doctest::Approx(kHalfPi + 0.25f).epsilon(1e-3));

    // Ignore: one zero move, then nothing more.
    s.Animator().rootMotion = RootMotionMode::Ignore;
    s.Run(0.1f, 0.1f);
    CHECK(characters->moves == 6u);
    CHECK(Length(characters->last) == 0.0f);
    s.Run(0.3f, 0.1f);
    CHECK(characters->moves == 6u);

    // Back on, then a script switches it to Script mode: one zero move again.
    s.Animator().rootMotion = RootMotionMode::Character;
    s.Run(0.1f, 0.1f);
    const usize walking = characters->moves;
    CHECK(Length(characters->last) > 1.0f);
    s.Animator().rootMotion = RootMotionMode::Script;
    s.Run(0.2f, 0.1f);
    CHECK(characters->moves == walking + 1u);
    CHECK(Length(characters->last) == 0.0f);

    // Back on, then the animator removed: one zero move as it goes.
    s.Animator().rootMotion = RootMotionMode::Character;
    s.Run(0.1f, 0.1f);
    CHECK(Length(characters->last) > 1.0f);
    s.level.GetSystem<engine::animation::SkeletalAnimationComponentManager>()->Remove(s.walker);
    CHECK(Length(characters->last) == 0.0f);
}

TEST_CASE("root motion: a v1 animator record reads with root motion off")
{
    engine::animation::RegisterAnimationComponentReflection();
    const TypeInfo& type = TypeOf<engine::animation::SkeletalAnimationComponent>();
    CHECK(type.dataVersion == 2u);
    CHECK(type.minReadDataVersion == 1u);
    foundation::xml::XmlDocument doc(DefaultAllocator());
    REQUIRE(doc.Parse(u8"<root><string name=\"skeleton\">00000000-0000-0000-0000-000000000000</string>"
                      u8"<string name=\"clip\">00000000-0000-0000-0000-000000000000</string>"
                      u8"<f32 name=\"speed\">1.5</f32><f32 name=\"startTime\">0</f32><bool name=\"autoPlay\">true</bool>"
                      u8"<array name=\"meshEntities\" count=\"0\"/></root>") == foundation::xml::XmlResult::Ok);
    foundation::xml::XmlSerializer reader(doc);
    const SerializedDataVersion chain[] = {{type.id, 1}};
    reader.PushVersionScope(chain, 1);
    engine::animation::SkeletalAnimationComponent c;
    Serialize(reader, c);
    reader.PopVersionScope();
    CHECK(reader.IsOk()); // no rootMotion field asked of a v1 record
    CHECK(c.speed == doctest::Approx(1.5f));
    CHECK(c.rootMotion == RootMotionMode::Ignore);
}

TEST_CASE("root motion: a pet turned by its script each frame walks the way it faces")
{
    // PaperKid's pet: the Pet entity (a scene root) is turned by its script every frame; its model
    // under it plays a Walk whose root travels 0.55 m over 1.0417 s, at speed 1.45, in Entity mode
    // aimed at the Pet. It must walk the way the Pet faces, at about 0.8 m/s.
    scene::Scene level{DefaultAllocator(), u8"pet"};
    level.AddSystem<engine::render::MeshComponentManager>();
    engine::animation::AddAnimationSceneManagers(level);
    RefPtr<animation::Skeleton> skeleton = MakeRef<animation::Skeleton>(DefaultAllocator(), 1);
    skeleton->Bones()[0].index = 0;
    skeleton->Bones()[0].parentIndex = -1;
    skeleton->FindRootBones();
    skeleton->BuildChildIndices();
    RefPtr<animation::AnimationClip> walk = MakeRef<animation::AnimationClip>(DefaultAllocator(), u8"Walk", 1.0416666f, true);
    walk->rootMotion.horizontal = true;
    for (i32 i = 0; i <= 50; ++i)
    {
        const f32 t = 1.0416666f * static_cast<f32>(i) / 50.0f;
        walk->rootMotion.times.PushBack(t);
        walk->rootMotion.positions.PushBack(Float3{0, 0, 0.55f * t / 1.0416666f});
        walk->rootMotion.yaws.PushBack(0.0f);
    }
    walk->GetOrCreatePositionTrack(0)->AddKeyframe(0.0f, Float3{});
    walk->GetOrCreatePositionTrack(0)->AddKeyframe(1.0416666f, Float3{});
    const scene::EntityHandle pet = level.CreateEntity(u8"Dog");
    const scene::EntityHandle model = level.CreateEntity(u8"DogModel");
    level.SetParent(model, pet);
    auto& a = level.GetSystem<engine::animation::SkeletalAnimationComponentManager>()->Add(model);
    a.skeleton.SetDirect(skeleton);
    a.clip.SetDirect(walk);
    a.speed = 1.45f;
    a.rootMotion = RootMotionMode::Entity;
    a.rootMotionTarget = level.GetEntityId(pet);
    level.UpdateTransforms();
    const f32 yaw = -88.0f * kDegToRad;
    const Float3 start = level.GetWorldPosition(pet);
    for (i32 frame = 0; frame < 120; ++frame)
    {
        Transform t = level.GetLocalTransform(pet); // the script turns it, every frame
        t.rotation = Quaternion::FromAxisAngle(Float3{0, 1, 0}, yaw);
        level.SetLocalTransform(pet, t);
        level.Update(1.0f / 60.0f);
    }
    const Float3 moved = level.GetWorldPosition(pet) - start;
    CHECK(Length(moved) == doctest::Approx(0.55f / 1.0416666f * 1.45f * 2.0f).epsilon(0.02));
    CHECK(moved.x < -1.5f); // facing -88 degrees: toward -X
}
