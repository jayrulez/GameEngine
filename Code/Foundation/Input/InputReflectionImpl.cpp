// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Input - reflection implementation unit: the input-map LEAF value types + enums.
//
// Reflected in their owning module (foundation.input) so the input types are visible to
// tooling. This unit covers the FLAT-SCALAR leaves - Binding,
// Interaction, ActionProcessors - and the enums (BindingSource/ActionKind/InteractionKind).
// The CONTAINER structs above them (Action/ActionSet/InputMap, nested Array<> lists) reach these
// leaves via container reflection + a Nested member on the asset. REFLECT_* bodies live
// out of the interface (GCC module hygiene). RegisterInputTypeReflection() is idempotent.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

module foundation.input;

import foundation.core;
import foundation.shell; // RegisterShellInputReflection

using namespace foundation::core;

namespace foundation::input
{
    REFLECT_ENUM(BindingSource, "rtti::input")
    {
        builder.Value("Key", BindingSource::Key);
        builder.Value("MouseButton", BindingSource::MouseButton);
        builder.Value("MouseAxis", BindingSource::MouseAxis);
        builder.Value("MouseDelta", BindingSource::MouseDelta);
        builder.Value("GamepadButton", BindingSource::GamepadButton);
        builder.Value("GamepadAxis", BindingSource::GamepadAxis);
        builder.Value("GamepadStick", BindingSource::GamepadStick);
        builder.Value("Composite2D", BindingSource::Composite2D);
        builder.Value("TouchButton", BindingSource::TouchButton);
        builder.Value("TouchStick", BindingSource::TouchStick);
    }

    // The input map's own codes beside the shell's (Sedulous a700e581): what a binding's `code`
    // means for a MouseAxis and a GamepadStick binding.
    REFLECT_ENUM(MouseAxisCode, "rtti::input")
    {
        builder.Value("DeltaX", MouseAxisCode::DeltaX);
        builder.Value("DeltaY", MouseAxisCode::DeltaY);
        builder.Value("Wheel", MouseAxisCode::Wheel);
    }

    REFLECT_ENUM(StickCode, "rtti::input")
    {
        builder.Value("Left", StickCode::Left);
        builder.Value("Right", StickCode::Right);
    }

    REFLECT_ENUM(ActionKind, "rtti::input")
    {
        builder.Value("Button", ActionKind::Button);
        builder.Value("Axis1D", ActionKind::Axis1D);
        builder.Value("Axis2D", ActionKind::Axis2D);
    }

    REFLECT_ENUM(InteractionKind, "rtti::input")
    {
        builder.Value("None", InteractionKind::None);
        builder.Value("Hold", InteractionKind::Hold);
        builder.Value("Tap", InteractionKind::Tap);
        builder.Value("DoubleTap", InteractionKind::DoubleTap);
    }

    REFLECT_VALUE(Binding, "rtti::input")
    {
        builder.Property<&Binding::source>("source")
            .PropAttribute("displayName", String(u8"Source"))
            .Property<&Binding::code>("code")
            .PropAttribute("displayName", String(u8"Code"))
            .Property<&Binding::modifiers>("modifiers")
            .Property<&Binding::device>("device")
            .PropAttribute("displayName", String(u8"Device"))
            .Property<&Binding::deadZone>("deadZone")
            .PropAttribute("displayName", String(u8"Dead Zone"))
            .Property<&Binding::scale>("scale")
            .Property<&Binding::invert>("invert")
            .Property<&Binding::normalize>("normalize")
            .Property<&Binding::negX>("negX")
            .Property<&Binding::posX>("posX")
            .Property<&Binding::negY>("negY")
            .Property<&Binding::posY>("posY")
            .Property<&Binding::regionX>("regionX")
            .Property<&Binding::regionY>("regionY")
            .Property<&Binding::regionW>("regionW")
            .Property<&Binding::regionH>("regionH")
            .Property<&Binding::stickRadius>("stickRadius");
    }

    REFLECT_VALUE(Interaction, "rtti::input")
    {
        builder.Property<&Interaction::kind>("kind")
            .PropAttribute("displayName", String(u8"Interaction"))
            .Property<&Interaction::seconds>("seconds")
            .PropAttribute("displayName", String(u8"Seconds"));
    }

    REFLECT_VALUE(ActionProcessors, "rtti::input")
    {
        builder.Property<&ActionProcessors::sensitivity>("sensitivity")
            .PropAttribute("displayName", String(u8"Sensitivity"))
            .Property<&ActionProcessors::gravity>("gravity")
            .PropAttribute("displayName", String(u8"Gravity"))
            .Property<&ActionProcessors::snap>("snap")
            .Property<&ActionProcessors::responseExponent>("responseExponent")
            .PropAttribute("displayName", String(u8"Response Exponent"))
            .Property<&ActionProcessors::timeScale>("timeScale");
    }

    // The CONTAINER structs above the leaves. Their Array<> and value-struct members are Nested
    // (address-based: the tree is TRAVERSED in place via container reflection, not marshalled by
    // value), so the whole InputMap is reflection-visible for scripting/tooling. The input editor
    // page stays bespoke - this is for scriptability, not a reflection-driven inspector.
    REFLECT_VALUE(Action, "rtti::input")
    {
        builder.Property<&Action::name>("name")
            .PropAttribute("displayName", String(u8"Name"))
            .Property<&Action::kind>("kind")
            .PropAttribute("displayName", String(u8"Kind"))
            .Nested<&Action::interaction>("interaction") // beside the kind: when it fires
            .Nested<&Action::bindings>("bindings")       // Array<Binding> (container)
            .Nested<&Action::processors>("processors");
    }

    REFLECT_VALUE(ActionSet, "rtti::input")
    {
        builder.Property<&ActionSet::name>("name")
            .PropAttribute("displayName", String(u8"Name"))
            .Property<&ActionSet::priority>("priority")
            .PropAttribute("displayName", String(u8"Priority"))
            .Nested<&ActionSet::actions>("actions"); // Array<Action> (container)
    }

    REFLECT_VALUE(InputMap, "rtti::input")
    {
        builder.Nested<&InputMap::sets>("sets"); // Array<ActionSet> (container)
    }

    void RegisterInputTypeReflection()
    {
        static const bool once = []()
        {
            foundation::shell::RegisterShellInputReflection(); // the key/button/axis codes
            RttiRegisterEnum_BindingSource();
            RttiRegisterEnum_MouseAxisCode();
            RttiRegisterEnum_StickCode();
            GlobalTypeRegistry().Register(TypeOf<MouseAxisCode>()); // type_info finds them by name
            GlobalTypeRegistry().Register(TypeOf<StickCode>());
            RttiRegisterEnum_ActionKind();
            RttiRegisterEnum_InteractionKind();
            RttiRegisterValue_Binding();
            RttiRegisterValue_Interaction();
            RttiRegisterValue_ActionProcessors();
            RttiRegisterValue_Action();
            RttiRegisterValue_ActionSet();
            RttiRegisterValue_InputMap();
            // The array element types are reflected above; register the containers so a reflected
            // Array<> member is IsContainer with generic indexed access to its elements.
            RegisterArrayType<Binding>();
            RegisterArrayType<Action>();
            RegisterArrayType<ActionSet>();
            return true;
        }();
        (void)once;
    }
}
