// Draconic::ScriptFacades - implementation unit: the DRACONIC_REFLECT_* bodies (they
// never sit in a module interface unit - the GCC gcm-cluster rule).

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"
#include "Draconic.Foundation/Log/Log.h"

module draconic.script.facades;

import draconic.foundation;
import draconic.scene;
import draconic.script;

using namespace draconic::foundation;

namespace draconic::script
{
    DRACONIC_REFLECT_VALUE(Entity, "draconic::script")
    {
        builder.Method<&Entity::isValid>("isValid");
        builder.Method<&Entity::name>("name");
        builder.Method<&Entity::setName>("setName");
        builder.Method<&Entity::position>("position");
        builder.Method<&Entity::setPosition>("setPosition");
        builder.Method<&Entity::worldPosition>("worldPosition");
        builder.Method<&Entity::setRotationEuler>("setRotationEuler");
        builder.Method<&Entity::setScale>("setScale");
        builder.Method<&Entity::destroy>("destroy");
        // A computed property (parens-less): `entity.scene` reads its bound Scene. Not a
        // method, so scripts write `entity.scene.find(...)` without call parens.
        builder.ComputedProperty<&Entity::sceneHandle>("scene");
        // send overloads (P2 messaging) - one reflected name, resolved by arg type.
        builder.Method<static_cast<void (Entity::*)(String) const>(&Entity::send)>("send");
        builder.Method<static_cast<void (Entity::*)(String, f64) const>(&Entity::send)>("send");
        builder.Method<static_cast<void (Entity::*)(String, String) const>(&Entity::send)>("send");
        builder.Method<static_cast<void (Entity::*)(String, Entity) const>(&Entity::send)>("send");
        builder.Constructor(); // Wren only materializes constructible foreign classes
    }

    DRACONIC_REFLECT(Log, "draconic::script")
    {
        builder.Method<&Log::info>("info");
        builder.Method<&Log::warn>("warn");
        builder.Method<&Log::error>("error");
        builder.Constructor();
    }

    DRACONIC_REFLECT(Time, "draconic::script")
    {
        builder.Method<&Time::now>("now");
        builder.Method<&Time::delta>("delta");
        builder.Constructor();
    }

    DRACONIC_REFLECT(Random, "draconic::script")
    {
        builder.Method<&Random::value>("value");
        builder.Method<&Random::range>("range");
        builder.Method<&Random::intRange>("intRange");
        builder.Constructor();
    }

    DRACONIC_REFLECT_VALUE(Scene, "draconic::script")
    {
        builder.Method<&Scene::spawn>("spawn");
        builder.Method<&Scene::find>("find");
        builder.Method<&Scene::findByPath>("findByPath");
        // A computed property (parens-less): `scene.events` reads this scene's event-bus handle.
        builder.ComputedProperty<&Scene::eventsHandle>("events");
        builder.Constructor(); // Wren only materializes constructible foreign classes
    }

    DRACONIC_REFLECT_VALUE(SceneEvents, "draconic::script")
    {
        // emit overloads - one reflected name, resolved by payload arg type (mirrors Entity::send).
        builder.Method<static_cast<void (SceneEvents::*)(String) const>(&SceneEvents::emit)>("emit");
        builder.Method<static_cast<void (SceneEvents::*)(String, f64) const>(&SceneEvents::emit)>(
            "emit");
        builder.Method<static_cast<void (SceneEvents::*)(String, String) const>(&SceneEvents::emit)>(
            "emit");
        builder.Method<static_cast<void (SceneEvents::*)(String, bool) const>(&SceneEvents::emit)>(
            "emit");
        builder.Method<static_cast<void (SceneEvents::*)(String, Entity) const>(&SceneEvents::emit)>(
            "emit");
        // The generic sink, reflected LAST (a reflected-value payload that matches no typed overload).
        builder.Method<static_cast<void (SceneEvents::*)(String, Variant) const>(&SceneEvents::emit)>(
            "emit");
        builder.Constructor(); // Wren only materializes constructible foreign classes
    }

    foundation::Span<const foundation::StringView> BehaviorFacadeNames()
    {
        // Kept in sync with RegisterScriptFacadeReflection below.
        static const foundation::StringView names[] = {
            u8"Entity", u8"Log", u8"Time", u8"Random", u8"Scene", u8"SceneEvents",
        };
        return foundation::Span<const foundation::StringView>{names, 6};
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
        Array<const foundation::TypeInfo*>& ExtraRootStorage()
        {
            static Array<const foundation::TypeInfo*> roots;
            return roots;
        }
    }

    void RegisterExtraScriptRootType(const foundation::TypeInfo* type)
    {
        if (type == nullptr)
        {
            return;
        }
        for (const foundation::TypeInfo* existing : ExtraRootStorage())
        {
            if (existing == type)
            {
                return; // idempotent
            }
        }
        ExtraRootStorage().PushBack(type);
    }

    foundation::Span<const foundation::TypeInfo* const> ExtraScriptRootTypes()
    {
        return {ExtraRootStorage().Data(), ExtraRootStorage().Size()};
    }

    void RegisterExtraFacadeName(StringView name)
    {
        // Reserved contract-class names: a user's own script class MUST take these - the game
        // orchestrator is class `Game` (StartScript does CreateInstance("Game")), and the scene
        // tier is class `Level` (instantiated once per scene). A facade sharing either name would
        // clash (AngelScript "Name conflict", Wren "import ... for <name>") and the user's class
        // could not compile. Refuse the registration. See docs/design/adding-facades.md.
        const StringView kReservedNames[] = {StringView(u8"Game"), StringView(u8"Level")};
        for (StringView reserved : kReservedNames)
        {
            if (name == reserved)
            {
                DRACONIC_LOG_ERROR(
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
            DraconicRegisterValue_Entity();
            GlobalTypeRegistry().Register(TypeOf<Entity>());
            GlobalTypeRegistry().Register(Log::StaticType());
            GlobalTypeRegistry().Register(Time::StaticType());
            GlobalTypeRegistry().Register(Random::StaticType());
            DraconicRegisterValue_Scene(); // now a bound value type (mirrors Entity)
            GlobalTypeRegistry().Register(TypeOf<Scene>());
            DraconicRegisterValue_SceneEvents(); // the scene.events handle (mirrors Scene)
            GlobalTypeRegistry().Register(TypeOf<SceneEvents>());
            return true;
        }();
        (void)once;
    }
}
