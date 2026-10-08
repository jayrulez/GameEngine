// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Script.Facades - implementation unit: the REFLECT_* bodies (they
// never sit in a module interface unit - the GCC gcm-cluster rule).

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"
#include "Core/Log/Log.h"

module foundation.script.facades;

import foundation.core;
import foundation.scene;
import foundation.script;

using namespace foundation::core;
namespace core = foundation::core;

namespace foundation::script
{
    namespace
    {
        // Two handles to the same entity: the same scene, slot and generation (a handle to an
        // entity since destroyed differs from one to the slot's new occupant).
        bool SameEntity(Entity a, Entity b)
        {
            return a.scene == b.scene && a.entityIndex == b.entityIndex && a.entityGeneration == b.entityGeneration;
        }
    }

    REFLECT_VALUE(Entity, "rtti::script")
    {
        // a == b in a script: is the hit the rider, is this trunk the last one passed.
        builder.Method<&SameEntity>("Equals", {"a", "b"}).Operator(core::MethodOperator::Equals);
        builder.Method<&Entity::isValid>("isValid");
        builder.Method<&Entity::active>("active");
        builder.Method<&Entity::setActive>("setActive");
        builder.Method<&Entity::activeInHierarchy>("activeInHierarchy");
        builder.Method<&Entity::name>("name");
        builder.Method<&Entity::setName>("setName");
        builder.Method<&Entity::position>("position");
        builder.Method<static_cast<void (Entity::*)(f32, f32, f32)>(&Entity::setPosition)>(
            "setPosition", {"x", "y", "z"});
        builder.Method<static_cast<void (Entity::*)(Float3)>(&Entity::setPosition)>("setPosition",
                                                                                  {"position"});
        builder.Method<&Entity::worldPosition>("worldPosition");
        builder.Method<&Entity::setRotationEuler>("setRotationEuler");
        builder.Method<&Entity::rotation>("rotation");
        builder.Method<&Entity::worldRotation>("worldRotation");
        builder.Method<&Entity::worldScale>("worldScale");
        builder.Method<&Entity::setRotation>("setRotation", {"rotation"});
        builder.Method<&Entity::scale>("scale");
        builder.Method<static_cast<void (Entity::*)(f32, f32, f32)>(&Entity::setScale)>(
            "setScale", {"x", "y", "z"});
        builder.Method<static_cast<void (Entity::*)(Float3)>(&Entity::setScale)>("setScale",
                                                                               {"scale"});
        builder.Method<&Entity::localTransform>("localTransform");
        builder.Method<&Entity::setLocalTransform>("setLocalTransform", {"transform"});
        builder.Method<&Entity::destroy>("destroy");
        // Identity and the hierarchy, as Sedulous's Entity.
        builder.Method<&Entity::id>("id");
        builder.Method<&Entity::parent>("parent");
        builder.Method<&Entity::firstChild>("firstChild");
        builder.Method<&Entity::nextSibling>("nextSibling");
        builder.Method<&Entity::findChildByName>("findChildByName", {"name"});
        builder.Method<static_cast<void (Entity::*)(Entity) const>(&Entity::setParent)>(
            "setParent", {"parent"});
        builder.Method<static_cast<void (Entity::*)(Entity, bool) const>(&Entity::setParent)>(
            "setParent", {"parent", "keepWorldTransform"});
        builder.Method<static_cast<void (Entity::*)(Entity) const>(&Entity::moveBefore)>(
            "moveBefore", {"sibling"});
        builder.Method<static_cast<void (Entity::*)(Entity, bool) const>(&Entity::moveBefore)>(
            "moveBefore", {"sibling", "keepWorldTransform"});
        // A computed property (parens-less): `entity.scene` reads its bound Scene. Not a
        // method, so scripts write `entity.scene.find(...)` without call parens.
        builder.ComputedProperty<&Entity::sceneHandle>("scene");
        // send - ONE conceptual method as an ARITY FAMILY: send(name) and
        // send(name, payload:Variant), dispatched by argument count. The Variant carries any script
        // value onto the StringHash+Variant bus (the honest payload type).
        builder.Method<static_cast<void (Entity::*)(String) const>(&Entity::send)>("send");
        builder.Method<static_cast<void (Entity::*)(String, Variant) const>(&Entity::send)>(
            "send", {"name", "payload"});
        builder.Constructor(); // some backends only materialize constructible foreign classes
    }

    REFLECT_MEMBERS(Log, "rtti::script")
    {
        builder.Method<&Log::info>("info");
        builder.Method<&Log::warn>("warn");
        builder.Method<&Log::error>("error");
        builder.Constructor();
    }

    REFLECT_MEMBERS(Time, "rtti::script")
    {
        builder.Method<&Time::now>("now");
        builder.Method<&Time::delta>("delta");
        builder.Constructor();
    }

    REFLECT_MEMBERS(Random, "rtti::script")
    {
        builder.Method<&Random::value>("value");
        builder.Method<&Random::range>("range");
        builder.Method<&Random::intRange>("intRange");
        builder.Method<&Random::seed>("seed", {"seed"});
        // Sedulous's Random.Bool; lowercase `bool` is a keyword in AngelScript (and C++).
        builder.Method<&Random::boolean>("boolean");
        builder.Constructor();
    }

    REFLECT_VALUE(Scene, "rtti::script")
    {
        builder.Method<&Scene::spawn>("spawn");
        builder.Method<&Scene::find>("find");
        builder.Method<&Scene::findByPath>("findByPath");
        builder.Method<static_cast<Entity (Scene::*)() const>(&Scene::createEntity)>("createEntity");
        builder.Method<static_cast<Entity (Scene::*)(String) const>(&Scene::createEntity)>(
            "createEntity", {"name"});
        builder.Method<&Scene::destroyEntity>("destroyEntity", {"entity"});
        builder.Method<&Scene::findEntity>("findEntity", {"id"});
        builder.Method<&Scene::name>("name");
        builder.Method<&Scene::entityCount>("entityCount");
        // A computed property (parens-less): `scene.events` reads this scene's event-bus handle.
        builder.ComputedProperty<&Scene::eventsHandle>("events");
        builder.Constructor(); // some backends only materialize constructible foreign classes
    }

    REFLECT_VALUE(SceneEvents, "rtti::script")
    {
        // emit - ONE conceptual method as an ARITY FAMILY (mirrors Entity::send): emit(name) and
        // emit(name, payload:Variant), dispatched by argument count; the Variant carries any value.
        builder.Method<static_cast<void (SceneEvents::*)(String) const>(&SceneEvents::emit)>("emit");
        builder.Method<static_cast<void (SceneEvents::*)(String, Variant) const>(&SceneEvents::emit)>(
            "emit", {"name", "payload"});
        builder.Constructor(); // some backends only materialize constructible foreign classes
    }

    REFLECT_VALUE(ScenePrefabs, "rtti::script")
    {
        // spawn - an ARITY FAMILY: (prefab, position), + rotation, + parent.
        builder.Method<static_cast<Entity (ScenePrefabs::*)(Guid, Float3) const>(&ScenePrefabs::spawn)>(
            "spawn", {"prefab", "position"});
        builder.Method<static_cast<Entity (ScenePrefabs::*)(Guid, Float3, Quaternion) const>(
            &ScenePrefabs::spawn)>("spawn", {"prefab", "position", "rotation"});
        builder.Method<static_cast<Entity (ScenePrefabs::*)(Guid, Float3, Quaternion, Entity) const>(
            &ScenePrefabs::spawn)>("spawn", {"prefab", "position", "rotation", "parent"});
        builder.Method<&ScenePrefabs::of>("of", {"scene"});
        builder.Constructor(); // some backends only materialize constructible foreign classes
    }

    core::Span<const core::StringView> BehaviorFacadeNames()
    {
        // Kept in sync with RegisterScriptFacadeReflection below.
        static const core::StringView names[] = {
            u8"Entity", u8"Log", u8"Time", u8"Random", u8"Scene", u8"SceneEvents", u8"ScenePrefabs",
        };
        return core::Span<const core::StringView>{names, 7};
    }

    namespace
    {
        Array<String>& ExtraFacadeStorage()
        {
            static Array<String> names;
            return names;
        }
        Array<StringView>& ExtraFacadeViews()
        {
            static Array<StringView> views;
            return views;
        }
        Array<const core::TypeInfo*>& ExtraRootStorage()
        {
            static Array<const core::TypeInfo*> roots;
            return roots;
        }
    }

    void RegisterExtraScriptRootType(const core::TypeInfo* type)
    {
        if (type == nullptr)
        {
            return;
        }
        for (const core::TypeInfo* existing : ExtraRootStorage())
        {
            if (existing == type)
            {
                return; // idempotent
            }
        }
        ExtraRootStorage().PushBack(type);
    }

    core::Span<const core::TypeInfo* const> ExtraScriptRootTypes()
    {
        return {ExtraRootStorage().Data(), ExtraRootStorage().Size()};
    }

    void RegisterExtraFacadeName(StringView name)
    {
        // Reserved contract-class names: a user's own script class MUST take these - the game
        // orchestrator is class `Game` (StartScript does CreateInstance("Game")), and the scene
        // tier is class `Level` (instantiated once per scene). A facade sharing either name would
        // clash (AngelScript "Name conflict") and the user's class
        // could not compile. Refuse the registration.
        const StringView kReservedNames[] = {StringView(u8"Game"), StringView(u8"Level")};
        for (StringView reserved : kReservedNames)
        {
            if (name == reserved)
            {
                LOG_ERROR(
                    u8"Script",
                    u8"facade name '{}' is reserved for a user contract class - registration refused",
                    name);
                return; // refuse: the name is never added, so the user's class always wins

            }
        }
        for (const String& existing : ExtraFacadeStorage())
        {
            if (existing.AsView() == name)
            {
                return;
            }
        } // idempotent
        ExtraFacadeStorage().PushBack(String(name));
        // Rebuild the view list from the (possibly reallocated) storage.
        Array<StringView>& views = ExtraFacadeViews();
        views.Clear();
        for (const String& n : ExtraFacadeStorage())
        {
            views.PushBack(n.AsView());
        }
    }

    Span<const StringView> ExtraFacadeNames() { return ExtraFacadeViews().AsSpan(); }

    void RegisterScriptFacadeReflection()
    {
        static const bool once = []()
        {
            RttiRegisterValue_Entity();
            GlobalTypeRegistry().Register(TypeOf<Entity>());
            GlobalTypeRegistry().Register(Log::StaticType());
            GlobalTypeRegistry().Register(Time::StaticType());
            GlobalTypeRegistry().Register(Random::StaticType());
            RttiRegisterValue_Scene(); // now a bound value type (mirrors Entity)
            GlobalTypeRegistry().Register(TypeOf<Scene>());
            RttiRegisterValue_SceneEvents(); // the scene.events handle (mirrors Scene)
            GlobalTypeRegistry().Register(TypeOf<SceneEvents>());
            RttiRegisterValue_ScenePrefabs(); // ScenePrefabs.of(scene) (Sedulous's scene.Prefabs)
            GlobalTypeRegistry().Register(TypeOf<ScenePrefabs>());
            return true;
        }();
        (void)once;
    }

    bool HasBindableSurface(const TypeInfo& type) noexcept
    {
        return ConstructorCount(type) > 0 || PropertyCount(type) > 0 || MethodCount(type) > 0;
    }

    void CollectEmittableTypes(Span<const TypeInfo* const> allTypes, Array<const TypeInfo*>& out)
    {
        const auto managed = [&](const TypeInfo* t) -> bool
        {
            if (t == nullptr || t->name == nullptr || t->enumeratorCount > 0 ||
                t->container != nullptr || !HasBindableSurface(*t))
            {
                return false;
            }
            for (const TypeInfo* e : allTypes)
            {
                if (e == t)
                {
                    return true;
                }
            }
            return false; // only types the caller actually registered
        };
        const auto has = [&](const TypeInfo* t) -> bool
        {
            for (const TypeInfo* e : out)
            {
                if (e == t)
                {
                    return true;
                }
            }
            return false;
        };
        Array<const TypeInfo*> work;
        const auto push = [&](const TypeInfo* t)
        {
            if (managed(t) && !has(t))
            {
                out.PushBack(t);
                work.PushBack(t);
            }
        };
        // Seeds: script-constructable types + the registered factory-return roots.
        for (const TypeInfo* t : allTypes)
        {
            if (t != nullptr && ConstructorCount(*t) > 0)
            {
                push(t);
            }
        }
        for (const TypeInfo* t : ExtraScriptRootTypes())
        {
            push(t);
        }
        const auto edge = [&](const TypeInfo* u)
        {
            if (u == nullptr)
            {
                return;
            }
            if (u->container != nullptr) // a container-typed member reaches its element type(s)
            {
                const TypeInfo* el = u->container->elementType;
                push(el);
                if (el != nullptr)
                {
                    Array<const TypeInfo*> derived;
                    EnumerateDerived(*el, derived);
                    for (const TypeInfo* d : derived)
                    {
                        push(d);
                    }
                }
                return;
            }
            push(u);
        };
        while (!work.IsEmpty())
        {
            const TypeInfo* t = work[work.Size() - 1];
            work.RemoveAt(work.Size() - 1);
            for (usize i = 0; i < MethodCount(*t); ++i)
            {
                const MethodInfo& m = MethodAt(*t, i);
                edge(m.returnType != nullptr ? m.returnType() : nullptr);
                for (u32 p = 0; p < m.paramCount; ++p)
                {
                    edge(m.params[p].type());
                }
            }
            for (usize i = 0; i < PropertyCount(*t); ++i)
            {
                edge(PropertyAt(*t, i).type);
            }
        }
    }
}
