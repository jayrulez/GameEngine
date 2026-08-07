// Draconic::Input - reflection implementation unit: the input-map LEAF value types + enums.
//
// Reflected in their owning module (draconic.input) so the input types stop being tooling-
// invisible (reflection track P2). This unit covers the FLAT-SCALAR leaves - Binding,
// Interaction, ActionProcessors - and the enums (BindingSource/ActionKind/InteractionKind).
// The CONTAINER structs above them (Action/ActionSet/InputMap, nested Array<> lists) reach these
// leaves via container reflection + a Nested member on the asset; that tree + its list-editor
// rendering is a later step (see docs/design/reflection-track.md). DRACONIC_REFLECT_* bodies live
// out of the interface (GCC module hygiene). RegisterInputTypeReflection() is idempotent.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

module draconic.input;

import draconic.foundation;

using namespace draconic::foundation;

namespace draconic::input
{
    DRACONIC_REFLECT_ENUM(BindingSource, "draconic::input")
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

    DRACONIC_REFLECT_ENUM(ActionKind, "draconic::input")
    {
        builder.Value("Button", ActionKind::Button);
        builder.Value("Axis1D", ActionKind::Axis1D);
        builder.Value("Axis2D", ActionKind::Axis2D);
    }

    DRACONIC_REFLECT_ENUM(InteractionKind, "draconic::input")
    {
        builder.Value("None", InteractionKind::None);
        builder.Value("Hold", InteractionKind::Hold);
        builder.Value("Tap", InteractionKind::Tap);
        builder.Value("DoubleTap", InteractionKind::DoubleTap);
    }

    DRACONIC_REFLECT_VALUE(Binding, "draconic::input")
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

    DRACONIC_REFLECT_VALUE(Interaction, "draconic::input")
    {
        builder.Property<&Interaction::kind>("kind")
            .PropAttribute("displayName", String(u8"Interaction"))
            .Property<&Interaction::seconds>("seconds")
            .PropAttribute("displayName", String(u8"Seconds"));
    }

    DRACONIC_REFLECT_VALUE(ActionProcessors, "draconic::input")
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
    DRACONIC_REFLECT_VALUE(Action, "draconic::input")
    {
        builder.Property<&Action::name>("name")
            .PropAttribute("displayName", String(u8"Name"))
            .Property<&Action::kind>("kind")
            .PropAttribute("displayName", String(u8"Kind"))
            .Nested<&Action::bindings>("bindings")   // Array<Binding> (container)
            .Nested<&Action::processors>("processors")
            .Nested<&Action::interaction>("interaction");
    }

    DRACONIC_REFLECT_VALUE(ActionSet, "draconic::input")
    {
        builder.Property<&ActionSet::name>("name")
            .PropAttribute("displayName", String(u8"Name"))
            .Property<&ActionSet::priority>("priority")
            .PropAttribute("displayName", String(u8"Priority"))
            .Nested<&ActionSet::actions>("actions"); // Array<Action> (container)
    }

    DRACONIC_REFLECT_VALUE(InputMap, "draconic::input")
    {
        builder.Nested<&InputMap::sets>("sets"); // Array<ActionSet> (container)
    }

    void RegisterInputTypeReflection()
    {
        static const bool once = []()
        {
            DraconicRegisterEnum_BindingSource();
            DraconicRegisterEnum_ActionKind();
            DraconicRegisterEnum_InteractionKind();
            DraconicRegisterValue_Binding();
            DraconicRegisterValue_Interaction();
            DraconicRegisterValue_ActionProcessors();
            DraconicRegisterValue_Action();
            DraconicRegisterValue_ActionSet();
            DraconicRegisterValue_InputMap();
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
