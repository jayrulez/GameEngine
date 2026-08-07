// Draconic UI - module implementation unit for UITypeRegistry::RegisterBuiltins.
//
// Registers every built-in View/layout/control type name so .sss element selectors (View, ButtonBase,
// ComboBox, EditText, ComboBox::arrow, ...) resolve to a concrete type. Kept in an impl unit because it
// references the whole control set (reached via the module's primary interface), which the leaf
// :ui_type_registry partition cannot import. Faithful port of Sedulous.UI/src/Styling/Parser/
// UITypeRegistry.bf RegisterBuiltins (a `static class` method); run-once guarded (the map is global).

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

module draconic.ui;

namespace draconic::ui
{
    void UITypeRegistry::RegisterBuiltins()
    {
        static bool registered = false;
        if (registered)
        {
            return;
        }
        registered = true;

        // Core.
        Register(u8"View", &View::StaticType());
        Register(u8"ViewGroup", &ViewGroup::StaticType());
        Register(u8"RootView", &RootView::StaticType());

        // Layouts + aliases.
        Register(u8"FlexLayout", &FlexLayout::StaticType());
        Register(u8"Flex", &FlexLayout::StaticType());
        Register(u8"GridLayout", &GridLayout::StaticType());
        Register(u8"Grid", &GridLayout::StaticType());
        Register(u8"DockLayout", &DockLayout::StaticType());
        Register(u8"Dock", &DockLayout::StaticType());
        Register(u8"FrameLayout", &FrameLayout::StaticType());
        Register(u8"Frame", &FrameLayout::StaticType());
        Register(u8"AbsoluteLayout", &AbsoluteLayout::StaticType());
        Register(u8"Absolute", &AbsoluteLayout::StaticType());
        Register(u8"FlowLayout", &FlowLayout::StaticType());
        Register(u8"Flow", &FlowLayout::StaticType());

        // Controls.
        Register(u8"Panel", &Panel::StaticType());
        Register(u8"Label", &Label::StaticType());
        Register(u8"Button", &Button::StaticType());
        Register(u8"ButtonBase", &ButtonBase::StaticType());
        Register(u8"ContentButton", &ContentButton::StaticType());
        Register(u8"RepeatButton", &RepeatButton::StaticType());
        Register(u8"ToggleButton", &ToggleButton::StaticType());
        Register(u8"CheckBox", &CheckBox::StaticType());
        Register(u8"RadioButton", &RadioButton::StaticType());
        Register(u8"RadioGroup", &RadioGroup::StaticType());
        Register(u8"ToggleSwitch", &ToggleSwitch::StaticType());
        Register(u8"EditText", &EditText::StaticType());
        Register(u8"PasswordBox", &PasswordBox::StaticType());
        Register(u8"NumericField", &NumericField::StaticType());
        Register(u8"EditableLabel", &EditableLabel::StaticType());
        Register(u8"Slider", &Slider::StaticType());
        Register(u8"ProgressBar", &ProgressBar::StaticType());
        Register(u8"ScrollBar", &ScrollBar::StaticType());
        Register(u8"ScrollView", &ScrollView::StaticType());
        Register(u8"ImageView", &ImageView::StaticType());
        Register(u8"ColorView", &ColorView::StaticType());
        Register(u8"DrawableView", &DrawableView::StaticType());
        Register(u8"Separator", &Separator::StaticType());
        Register(u8"Spacer", &Spacer::StaticType());
        Register(u8"ComboBox", &ComboBox::StaticType());
        Register(u8"TabView", &TabView::StaticType());
        Register(u8"Expander", &Expander::StaticType());
        Register(u8"ListView", &ListView::StaticType());
        Register(u8"GridView", &GridView::StaticType());
        Register(u8"TreeView", &TreeView::StaticType());

        // Overlay.
        Register(u8"ContextMenu", &ContextMenu::StaticType());
        Register(u8"Dialog", &Dialog::StaticType());
        Register(u8"TooltipView", &TooltipView::StaticType());
    }
}
