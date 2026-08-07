// Draconic::ScriptSubsystem - :components partition.
//
// The attachment model (docs/design/scripting.md §3.1): ONE ScriptComponent per entity
// holding an ORDERED array of behaviors - each a cooked ScriptClass reference, an
// enabled flag, and hash-keyed property OVERRIDES (values differing from the class's
// harvested defaults; Godot's default-diff semantics with Lumix's rename-safe hashes).
// Runtime fields (the live ScriptObject instance, dispatch state) are transient -
// never serialized. Because overrides live in the component payload, the prefab
// delta machinery (baseline blob compare) covers them with zero new code.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.engine.script:components;

import draconic.foundation;
import draconic.scene;
import draconic.resource;
import draconic.script;
import draconic.script.resource;

using namespace draconic::foundation;

export namespace draconic::script
{
    struct ScriptPropertyOverride
    {
        u64 nameHash = 0; // ScriptPropertyNameHash of the property's name
        ScriptPropertyValue value;
    };

    inline void Serialize(ISerializer& ar, ScriptPropertyOverride& o)
    {
        draconic::foundation::Serialize(ar, "nameHash", o.nameHash);
        draconic::foundation::Serialize(ar, "value", o.value);
    }

    struct ScriptBehavior
    {
        // Authored:
        draconic::resource::Ref<ScriptClass> script;
        bool enabled = true;
        f32 updateInterval = 0.0f; // seconds between onUpdate calls; <=0 = every tick (P3
                                   // throttling). The delivered dt is the ACCUMULATED time.
        Array<ScriptPropertyOverride> overrides;

        // Runtime (transient):
        RefPtr<ScriptObject> instance;
        const ScriptClass* boundClass = nullptr; // product the instance was built from
                                                 // (a reload swaps the product -> re-instantiate)
        bool started = false;                    // onStart delivered
        bool active = false;          // last delivered enable state (onEnable/onDisable edges)
        bool faulted = false;         // a fault disables the one behavior (cleared by reload)
        f32 updateAccumulator = 0.0f; // time banked toward the next throttled onUpdate

        [[nodiscard]] const ScriptPropertyOverride* FindOverride(u64 nameHash) const
        {
            for (const ScriptPropertyOverride& entry : overrides)
            {
                if (entry.nameHash == nameHash)
                {
                    return &entry;
                }
            }
            return nullptr;
        }
        void SetOverride(u64 nameHash, const ScriptPropertyValue& value)
        {
            for (ScriptPropertyOverride& entry : overrides)
            {
                if (entry.nameHash == nameHash)
                {
                    entry.value = value;
                    return;
                }
            }
            overrides.PushBack(ScriptPropertyOverride{nameHash, value});
        }
        void RemoveOverride(u64 nameHash)
        {
            for (usize i = 0; i < overrides.Size(); ++i)
            {
                if (overrides[i].nameHash == nameHash)
                {
                    overrides.RemoveAt(i);
                    return;
                }
            }
        }
    };

    inline void Serialize(ISerializer& ar, ScriptBehavior& b)
    {
        draconic::foundation::Serialize(ar, "script", b.script);
        draconic::foundation::Serialize(ar, "enabled", b.enabled);
        draconic::foundation::Serialize(ar, "updateInterval", b.updateInterval);
        draconic::foundation::Serialize(ar, "overrides", b.overrides); // count-prefixed array scope
    }

    struct ScriptComponent
    {
        Array<ScriptBehavior> behaviors; // execution order = array order
    };

    inline void Serialize(ISerializer& ar, ScriptComponent& c)
    {
        draconic::foundation::Serialize(ar, "behaviors", c.behaviors);
    }

    inline void ResolveResources(draconic::resource::ResourceManager& manager, ScriptComponent& c)
    {
        for (ScriptBehavior& behavior : c.behaviors)
        {
            behavior.script.Bind(manager);
        }
    }

    class ScriptSceneSystem; // forward (:subsystem) - the destroy hook dispatches onDestroy

    class ScriptComponentManager final
        : public draconic::scene::SerializableComponentManager<ScriptComponent>
    {
    public:
        ScriptComponentManager() : SerializableComponentManager<ScriptComponent>(u8"script") {}

        /// The per-scene script system, wired by the subsystem right after AddSystem -
        /// entity/component destruction routes onDestroy through it.
        void SetScriptSystem(ScriptSceneSystem* system) noexcept { m_scriptSystem = system; }
        [[nodiscard]] ScriptSceneSystem* ScriptSystem() const noexcept { return m_scriptSystem; }

    protected:
        // Defined in the :subsystem partition's implementation (needs ScriptSceneSystem).
        void OnComponentDestroyed(ScriptComponent& component,
                                  draconic::scene::EntityHandle entity) override;

    private:
        ScriptSceneSystem* m_scriptSystem = nullptr;
    };

    // Defined in SubsystemImpl.cpp (DRACONIC_REFLECT_* bodies never sit in a module
    // interface unit - the GCC gcm-cluster rule).
    void RegisterScriptComponentReflection();
}
