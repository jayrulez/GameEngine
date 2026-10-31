// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Input - :model partition.
//
// The action-mapping data model: one InputMap holds a whole game's bindings. ActionSets
// (contexts with priority) hold Actions (declared kinds, never inferred), which hold Bindings
// (a tagged flat record covering every physical source; flat is trivially serializable and
// suits an editor grid). Per-action processors carry key-axis smoothing, a response curve and
// a time-scale flag. Pure data and serialization; the evaluation lives in :runtime.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.input:input_map;

import foundation.core;

using namespace foundation::core;

export namespace foundation::input
{
    // Declared, never inferred: querying a Button as a vector is a caller error surfaced by the
    // editor's validation, not a silent 0.
    enum class ActionKind : u8
    {
        Button,
        Axis1D,
        Axis2D,
    };

    enum class BindingSource : u8
    {
        Key,           // code = shell::KeyCode; optional modifier mask
        MouseButton,   // code = shell::MouseButton
        MouseAxis,     // Axis1D rate: code = MouseAxisCode; never time-scaled
        MouseDelta,    // Axis2D rate: (dx, dy)
        GamepadButton, // code = shell::GamepadButton; device -1 = any
        GamepadAxis,   // Axis1D: code = shell::GamepadAxis; deadZone/invert/scale
        GamepadStick,  // Axis2D: code = StickCode; circular dead zone
        Composite2D,   // Axis2D from four digital keys (WASD); normalize flag
        TouchButton,   // Button: any touch inside the normalized screen region
        TouchStick,    // Axis2D: a floating virtual stick; a touch inside the region anchors
                       // where it is captured; deflection/stickRadius = the value
    };

    enum class MouseAxisCode : u32
    {
        DeltaX = 0,
        DeltaY = 1,
        Wheel = 2
    };
    enum class StickCode : u32
    {
        Left = 0,
        Right = 1
    };

    // One physical binding, flat and tagged: only the fields the source uses are meaningful,
    // the rest stay at defaults (they serialize compactly and the editor grid hides them).
    struct Binding
    {
        BindingSource source = BindingSource::Key;
        u32 code =
            0; // KeyCode / MouseButton / MouseAxisCode / GamepadButton / GamepadAxis / StickCode
        u32 modifiers = 0;    // Key only: required shell::KeyModifiers mask (0 = none required)
        i32 device = -1;      // gamepad index; -1 = any connected pad
        f32 deadZone = 0.15f; // analog sources; circular for sticks
        f32 scale =
            1.0f; // response scale (a key bound to an axis uses -1 for the negative direction)
        bool invert = false;   // flips the value (sticks: flips Y)
        bool normalize = true; // Composite2D: clamp diagonal length to 1
        u32 negX = 0;          // Composite2D key codes
        u32 posX = 0;
        u32 negY = 0;
        u32 posY = 0;
        // Touch sources: the activation region in normalized window coordinates (SDL
        // finger coords are [0,1]); sticks deflect over stickRadius (also normalized).
        f32 regionX = 0.0f;
        f32 regionY = 0.0f;
        f32 regionW = 1.0f;
        f32 regionH = 1.0f;
        f32 stickRadius = 0.15f;
    };

    // Button-action trigger shaping (a small per-action state machine).
    // None = plain press/release edges.
    //   Hold:      the pressed edge fires only once the press has been held `seconds`.
    //   Tap:       a one-frame pulse at release, only if the press lasted <= `seconds`.
    //   DoubleTap: a one-frame pulse on the second press within `seconds` of the first.
    enum class InteractionKind : u8
    {
        None,
        Hold,
        Tap,
        DoubleTap
    };

    struct Interaction
    {
        InteractionKind kind = InteractionKind::None;
        f32 seconds = 0.3f;
    };

    // Per-action value conditioning (applied to the folded target each frame).
    struct ActionProcessors
    {
        f32 sensitivity =
            0.0f;           // >0: key-driven axes ramp toward the target at this rate/sec
        f32 gravity = 0.0f; // >0: recenter rate/sec when the target is 0 (else sensitivity)
        bool snap = false;  // zero first on direction flip
        f32 responseExponent = 1.0f; // analog curve: sign(v)*|v|^e
        bool timeScale = false;      // value multiplies by the runtime's time scale (SetTimeScale)
    };

    struct Action
    {
        String name;
        ActionKind kind = ActionKind::Button;
        Array<Binding> bindings;
        ActionProcessors processors;
        Interaction interaction; // Button actions only (validated)
    };

    // A context: "Gameplay", "Menu", "Vehicle". Priority orders query resolution when the
    // same action name exists in several sets (higher wins among enabled sets).
    struct ActionSet
    {
        String name;
        i32 priority = 0;
        Array<Action> actions;
    };

    struct InputMap
    {
        Array<ActionSet> sets;
    };

    /// Reflects the input-map leaf value types (Binding, Interaction, ActionProcessors) and their
    /// enums (BindingSource, ActionKind, InteractionKind) so tooling and scripting can see them.
    /// Idempotent; defined in InputReflectionImpl.cpp. Call from a startup registrar.
    void RegisterInputTypeReflection();

    // ---- serialization (shared by the source asset and the cooked resource) ----

    inline constexpr u32 kInputMapVersion = 2; // the one supported map layout (readers refuse others)

    inline void SerializeBinding(ISerializer& ar, Binding& b)
    {
        u8 source = static_cast<u8>(b.source);
        foundation::core::Serialize(ar, "source", source);
        b.source = static_cast<BindingSource>(source);
        foundation::core::Serialize(ar, "code", b.code);
        foundation::core::Serialize(ar, "modifiers", b.modifiers);
        foundation::core::Serialize(ar, "device", b.device);
        foundation::core::Serialize(ar, "deadZone", b.deadZone);
        foundation::core::Serialize(ar, "scale", b.scale);
        foundation::core::Serialize(ar, "invert", b.invert);
        foundation::core::Serialize(ar, "normalize", b.normalize);
        foundation::core::Serialize(ar, "negX", b.negX);
        foundation::core::Serialize(ar, "posX", b.posX);
        foundation::core::Serialize(ar, "negY", b.negY);
        foundation::core::Serialize(ar, "posY", b.posY);
        foundation::core::Serialize(ar, "regionX", b.regionX);
        foundation::core::Serialize(ar, "regionY", b.regionY);
        foundation::core::Serialize(ar, "regionW", b.regionW);
        foundation::core::Serialize(ar, "regionH", b.regionH);
        foundation::core::Serialize(ar, "stickRadius", b.stickRadius);
    }

    inline void SerializeInputMap(ISerializer& ar, InputMap& map)
    {
        const bool writing = ar.Mode() == SerializeMode::Write;
        u32 version = kInputMapVersion;
        foundation::core::Serialize(ar, "version", version);
        if (!writing && version != kInputMapVersion)
        {
            // One layout: the field list differs between versions, so reading on would decode
            // the wrong fields into plausible-looking bindings. Refuse; re-save the map.
            ar.FailPayload(ErrorCode::NotSupported);
            return;
        }

        u32 setCount = writing ? static_cast<u32>(map.sets.Size()) : 0;
        ar.Key("sets");
        ar.BeginArray(setCount);
        if (!writing)
        {
            map.sets.Clear();
            map.sets.Resize(setCount);
        }
        for (u32 s = 0; s < setCount; ++s)
        {
            ActionSet& set = map.sets[s];
            foundation::core::Serialize(ar, "name", set.name);
            foundation::core::Serialize(ar, "priority", set.priority);
            u32 actionCount = writing ? static_cast<u32>(set.actions.Size()) : 0;
            ar.Key("actions");
            ar.BeginArray(actionCount);
            if (!writing)
            {
                set.actions.Resize(actionCount);
            }
            for (u32 a = 0; a < actionCount; ++a)
            {
                Action& action = set.actions[a];
                foundation::core::Serialize(ar, "name", action.name);
                u8 kind = static_cast<u8>(action.kind);
                foundation::core::Serialize(ar, "kind", kind);
                action.kind = static_cast<ActionKind>(kind);
                foundation::core::Serialize(ar, "sensitivity", action.processors.sensitivity);
                foundation::core::Serialize(ar, "gravity", action.processors.gravity);
                foundation::core::Serialize(ar, "snap", action.processors.snap);
                foundation::core::Serialize(ar, "responseExponent",
                                          action.processors.responseExponent);
                foundation::core::Serialize(ar, "timeScale", action.processors.timeScale);
                u8 interaction = static_cast<u8>(action.interaction.kind);
                foundation::core::Serialize(ar, "interaction", interaction);
                action.interaction.kind = static_cast<InteractionKind>(interaction);
                foundation::core::Serialize(ar, "interactionSeconds", action.interaction.seconds);
                u32 bindingCount = writing ? static_cast<u32>(action.bindings.Size()) : 0;
                ar.Key("bindings");
                ar.BeginArray(bindingCount);
                if (!writing)
                {
                    action.bindings.Resize(bindingCount);
                }
                for (u32 b = 0; b < bindingCount; ++b)
                {
                    SerializeBinding(ar, action.bindings[b]);
                }
                ar.EndArray();
            }
            ar.EndArray();
        }
        ar.EndArray();
    }

    // ---- user rebind overlay ----
    // Not part of the asset: a settings section persisted in the user file. Per-action
    // replacement binding lists apply over a pristine asset copy at load and after each
    // rebind; reset-to-default removes the override (the asset never mutates).
    struct InputBindingOverride
    {
        String setName;
        String actionName;
        Array<Binding> bindings;
    };

    class InputBindingOverrides final : public ISerializable
    {
        RTTI_OBJECT(InputBindingOverrides, ISerializable)
    public:
        Array<InputBindingOverride> overrides;

        void Serialize(ISerializer& ar) override
        {
            const bool writing = ar.Mode() == SerializeMode::Write;
            u32 count = writing ? static_cast<u32>(overrides.Size()) : 0;
            ar.Key("overrides");
            ar.BeginArray(count);
            if (!writing)
            {
                overrides.Clear();
                overrides.Resize(count);
            }
            for (u32 i = 0; i < count; ++i)
            {
                InputBindingOverride& o = overrides[i];
                foundation::core::Serialize(ar, "set", o.setName);
                foundation::core::Serialize(ar, "action", o.actionName);
                u32 bindingCount = writing ? static_cast<u32>(o.bindings.Size()) : 0;
                ar.Key("bindings");
                ar.BeginArray(bindingCount);
                if (!writing)
                {
                    o.bindings.Resize(bindingCount);
                }
                for (u32 b = 0; b < bindingCount; ++b)
                {
                    SerializeBinding(ar, o.bindings[b]);
                }
                ar.EndArray();
            }
            ar.EndArray();
        }

        /// Upsert the replacement list for one action.
        void Set(StringView set, StringView action, Array<Binding> bindings)
        {
            for (InputBindingOverride& o : overrides)
            {
                if (o.setName.AsView() == set && o.actionName.AsView() == action)
                {
                    o.bindings = static_cast<Array<Binding>&&>(bindings);
                    return;
                }
            }
            InputBindingOverride fresh;
            fresh.setName = String(set);
            fresh.actionName = String(action);
            fresh.bindings = static_cast<Array<Binding>&&>(bindings);
            overrides.PushBack(static_cast<InputBindingOverride&&>(fresh));
        }

        /// Reset one action to the asset's bindings (drop its override).
        void Clear(StringView set, StringView action)
        {
            for (usize i = 0; i < overrides.Size(); ++i)
            {
                if (overrides[i].setName.AsView() == set &&
                    overrides[i].actionName.AsView() == action)
                {
                    overrides.RemoveAt(i);
                    return;
                }
            }
        }
    };

    /// Applies the overlay onto `map` (a copy of the asset: the caller keeps the asset
    /// pristine; SetMap copies anyway, so the flow is load, Apply, then SetMap).
    /// Overrides naming unknown sets/actions are ignored (a map edit invalidated them).
    inline void ApplyBindingOverrides(InputMap& map, const InputBindingOverrides& overlay)
    {
        for (const InputBindingOverride& o : overlay.overrides)
        {
            for (ActionSet& set : map.sets)
            {
                if (set.name.AsView() != o.setName.AsView())
                {
                    continue;
                }
                for (Action& action : set.actions)
                {
                    if (action.name.AsView() == o.actionName.AsView())
                    {
                        action.bindings = o.bindings;
                    }
                }
            }
        }
    }

    // ---- validation (asset save and cook share it) ----
    // Kind mismatches are data errors: surfaced here, not silently zeroed at runtime.
    [[nodiscard]] inline bool ValidateInputMap(const InputMap& map, String* firstError = nullptr)
    {
        auto fail = [&](StringView message)
        {
            if (firstError != nullptr)
            {
                *firstError = String(message);
            }
            return false;
        };
        for (const ActionSet& set : map.sets)
        {
            if (set.name.IsEmpty())
            {
                return fail(u8"action set with an empty name");
            }
            for (const Action& action : set.actions)
            {
                if (action.name.IsEmpty())
                {
                    return fail(u8"action with an empty name");
                }
                if (action.interaction.kind != InteractionKind::None &&
                    action.kind != ActionKind::Button)
                {
                    return fail(u8"interaction on a non-Button action");
                }
                for (const Binding& b : action.bindings)
                {
                    const bool is2D = b.source == BindingSource::GamepadStick ||
                                      b.source == BindingSource::Composite2D ||
                                      b.source == BindingSource::MouseDelta ||
                                      b.source == BindingSource::TouchStick;
                    const bool isAxis = b.source == BindingSource::MouseAxis ||
                                        b.source == BindingSource::GamepadAxis;
                    switch (action.kind)
                    {
                    case ActionKind::Button:
                        if (is2D || isAxis)
                        {
                            return fail(u8"analog binding on a Button action");
                        }
                        break;
                    case ActionKind::Axis1D:
                        if (is2D)
                        {
                            return fail(u8"2D binding on an Axis1D action");
                        }
                        break;
                    case ActionKind::Axis2D:
                        if (!is2D)
                        {
                            return fail(u8"non-2D binding on an Axis2D action");
                        }
                        break;
                    }
                }
            }
        }
        return true;
    }

    /// Registers the input model's serializable types (the rebind-overlay settings
    /// section). Call once at startup wherever the overlay is persisted/loaded.
    inline void RegisterInputTypes()
    {
        GlobalTypeRegistry().Register(InputBindingOverrides::StaticType());
        RegisterSerializable<InputBindingOverrides>();
    }

    RTTI_DEFINE_OBJECT(InputBindingOverrides, "rtti::input")
}
