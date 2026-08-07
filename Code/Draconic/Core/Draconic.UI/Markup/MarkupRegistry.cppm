// Draconic UI - :markup_registry partition
//
// Maps XML element names to View factories and property/layout-param setters (registration-based, not
// reflection). Ported from Sedulous.UI/src/Markup/MarkupRegistry.bf. Divergences: Beef `delegate View()`
// / `delegate void(View, StringView)` -> plain function pointers (the built-in factories/setters capture
// nothing; a captureless lambda converts to a fn-ptr); Beef nested `Dictionary<String, Registration>`
// with an inner Dictionary -> flat HashMaps keyed by "element\x1fname" (avoids nested-container copies);
// the static tables -> function-local statics (no static-init-order issues); Beef `as X` -> Cast<X>;
// FlexLayout.LayoutParams -> FlexLayoutParams; float.Parse -> foundation::ParseFloat.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.ui:markup_registry;

import draconic.foundation;
import :view;
import :layout_params;
import :size_spec;
import :unit;
import :gravity;
import :thickness;
import :style_value_parser;
// Layouts
import :flex_layout;
import :frame_layout;
import :dock_layout;
import :flow_layout;
import :absolute_layout;
import :grid_layout;
// Controls
import :panel;
import :scroll_view;
import :label;
import :button;
import :icon_button;
import :checkbox;
import :radio_button;
import :radio_group;
import :toggle_switch;
import :toggle_button;
import :slider;
import :progress_bar;
import :edit_text;
import :password_box;
import :numeric_field;
import :expander;
import :tab_view;
import :combo_box;
import :spacer;
import :separator;
import :color_view;
import :image_view;
import :drawable_view;
import :list_view;
import :tree_view;
import :grid_view;

using namespace draconic::foundation;

export namespace draconic::ui
{
    /// Maps XML element names to View factories and property setters. Registration-based (explicit and
    /// debuggable) - used by MarkupLoader to create views and set attributes from .sml files.
    struct MarkupRegistry
    {
        using ViewFactory = RefPtr<View> (*)();
        using PropertySetter = void (*)(View*, StringView);
        using LayoutParamsFactory = RefPtr<LayoutParams> (*)();
        using LayoutParamSetter = void (*)(LayoutParams*, StringView);

        // === Registration ===

        static void RegisterView(StringView elementName, ViewFactory factory)
        {
            ViewFactories().InsertOrAssign(String(elementName), factory);
        }
        static void RegisterProperty(StringView elementName, StringView propertyName,
                                     PropertySetter setter)
        {
            ViewProps().InsertOrAssign(Key(elementName, propertyName), setter);
        }
        static void RegisterLayout(StringView elementName, LayoutParamsFactory createParams)
        {
            LayoutFactories().InsertOrAssign(String(elementName), createParams);
        }
        static void RegisterLayoutParam(StringView elementName, StringView paramName,
                                        LayoutParamSetter setter)
        {
            LayoutParams_().InsertOrAssign(Key(elementName, paramName), setter);
        }

        // === Lookup ===

        /// Create a view for the given element name. Null if not registered.
        [[nodiscard]] static RefPtr<View> CreateView(StringView elementName)
        {
            if (ViewFactory* f = ViewFactories().Find(String(elementName)))
            {
                return (*f)();
            }
            return {};
        }

        /// Try to set a property on a view from a string value. Returns true if found and set.
        static bool SetProperty(StringView elementName, View* view, StringView propertyName,
                                StringView value)
        {
            if (PropertySetter* s = ViewProps().Find(Key(elementName, propertyName)))
            {
                (*s)(view, value);
                return true;
            }
            return false;
        }

        /// Create default LayoutParams for a container. Null if not a registered layout.
        [[nodiscard]] static RefPtr<LayoutParams> CreateLayoutParams(StringView containerName)
        {
            if (LayoutParamsFactory* f = LayoutFactories().Find(String(containerName)))
            {
                return (*f)();
            }
            return {};
        }

        /// Try to set a layout param from a string value. Returns true if found and set.
        static bool SetLayoutParam(StringView containerName, LayoutParams* lp, StringView paramName,
                                   StringView value)
        {
            if (LayoutParamSetter* s = LayoutParams_().Find(Key(containerName, paramName)))
            {
                (*s)(lp, value);
                return true;
            }
            return false;
        }

        /// Every registered element name (editor completion vocabularies).
        static void CollectElementNames(Array<String>& out)
        {
            out.Clear();
            for (const auto& entry : ViewFactories())
            {
                out.PushBack(String(entry.key.AsView()));
            }
        }

        /// Attribute names usable on `elementName`: its registered properties plus the union
        /// of every layout-param name (which of those apply depends on the PARENT container -
        /// the registry cannot know it from the element alone).
        static void CollectAttributeNames(StringView elementName, Array<String>& out)
        {
            out.Clear();
            const auto splitKey = [](StringView key, StringView& element, StringView& name)
            {
                for (usize i = 0; i < key.Size(); ++i)
                {
                    if (key[i] == char8_t(0x1f))
                    {
                        element = key.SubStr(0, i);
                        name = key.SubStr(i + 1, key.Size() - i - 1);
                        return true;
                    }
                }
                return false;
            };
            const auto pushUnique = [&out](StringView name)
            {
                for (usize i = 0; i < out.Size(); ++i)
                {
                    if (out[i].AsView() == name)
                    {
                        return;
                    }
                }
                out.PushBack(String(name));
            };
            for (const auto& entry : ViewProps())
            {
                StringView element;
                StringView name;
                if (splitKey(entry.key.AsView(), element, name) && element == elementName)
                {
                    pushUnique(name);
                }
            }
            for (const auto& entry : LayoutParams_())
            {
                StringView element;
                StringView name;
                if (splitKey(entry.key.AsView(), element, name))
                {
                    pushUnique(name);
                }
            }
        }

        [[nodiscard]] static bool IsRegistered(StringView elementName)
        {
            return ViewFactories().Find(String(elementName)) != nullptr;
        }
        [[nodiscard]] static bool IsLayoutRegistered(StringView elementName)
        {
            return LayoutFactories().Find(String(elementName)) != nullptr;
        }

        // === Value parsing helpers ===

        /// Parse a SizeSpec from markup: "wrap", "match", "240", "240px", "16dp".
        [[nodiscard]] static SizeSpec ParseSizeSpec(StringView value)
        {
            if (value == u8"wrap")
            {
                return SizeSpec::Wrap();
            }
            if (value == u8"match")
            {
                return SizeSpec::Match();
            }
            const StringView t = Trimmed(value);
            if (EndsWith(t, u8"px"))
            {
                if (Optional<f64> v = ParseFloat(Chop(t, 2)); v.HasValue())
                {
                    return SizeSpec::Fixed(Unit::Px(static_cast<f32>(v.Value())));
                }
            }
            else if (EndsWith(t, u8"dp"))
            {
                if (Optional<f64> v = ParseFloat(Chop(t, 2)); v.HasValue())
                {
                    return SizeSpec::Fixed(Unit::Dp(static_cast<f32>(v.Value())));
                }
            }
            else if (EndsWith(t, u8"pt"))
            {
                if (Optional<f64> v = ParseFloat(Chop(t, 2)); v.HasValue())
                {
                    return SizeSpec::Fixed(Unit::Pt(static_cast<f32>(v.Value())));
                }
            }
            if (Optional<f64> v = ParseFloat(value); v.HasValue())
            {
                return SizeSpec::Fixed(Unit::Dp(static_cast<f32>(v.Value())));
            }
            return SizeSpec::Wrap();
        }

        /// Parse a Gravity value: "Center", "Fill", "TopLeft", "Bottom|Right", etc.
        [[nodiscard]] static Gravity ParseGravity(StringView value)
        {
            Gravity result = Gravity::None;
            const char8_t* data = value.Data();
            const usize n = value.Size();
            usize start = 0;
            for (usize i = 0; i <= n; ++i)
            {
                if (i == n || data[i] == u8'|')
                {
                    const StringView s = Trimmed(StringView{data + start, i - start});
                    if (s == u8"Left")
                    {
                        result = result | Gravity::Left;
                    }
                    else if (s == u8"Right")
                    {
                        result = result | Gravity::Right;
                    }
                    else if (s == u8"CenterH")
                    {
                        result = result | Gravity::CenterH;
                    }
                    else if (s == u8"FillH")
                    {
                        result = result | Gravity::FillH;
                    }
                    else if (s == u8"Top")
                    {
                        result = result | Gravity::Top;
                    }
                    else if (s == u8"Bottom")
                    {
                        result = result | Gravity::Bottom;
                    }
                    else if (s == u8"CenterV")
                    {
                        result = result | Gravity::CenterV;
                    }
                    else if (s == u8"FillV")
                    {
                        result = result | Gravity::FillV;
                    }
                    else if (s == u8"Center")
                    {
                        result = result | Gravity::Center;
                    }
                    else if (s == u8"Fill")
                    {
                        result = result | Gravity::Fill;
                    }
                    else if (s == u8"TopLeft")
                    {
                        result = result | Gravity::TopLeft;
                    }
                    else if (s == u8"TopRight")
                    {
                        result = result | Gravity::TopRight;
                    }
                    else if (s == u8"BottomLeft")
                    {
                        result = result | Gravity::BottomLeft;
                    }
                    else if (s == u8"BottomRight")
                    {
                        result = result | Gravity::BottomRight;
                    }
                    start = i + 1;
                }
            }
            return result;
        }

        /// Parse a Thickness: "8" (all), "8 12" (vert horiz), "1 2 3 4" (top right bottom left).
        [[nodiscard]] static Thickness ParseThickness(StringView value)
        {
            f32 values[4] = {0, 0, 0, 0};
            i32 count = 0;
            const char8_t* data = value.Data();
            const usize n = value.Size();
            usize start = 0;
            for (usize i = 0; i <= n; ++i)
            {
                if (i == n || data[i] == u8' ')
                {
                    const StringView s = Trimmed(StringView{data + start, i - start});
                    if (s.Size() > 0 && count < 4)
                    {
                        if (Optional<f64> f = ParseFloat(s); f.HasValue())
                        {
                            values[count++] = static_cast<f32>(f.Value());
                        }
                    }
                    start = i + 1;
                }
            }
            return StyleValueParser::ParseThickness(values, count);
        }

        /// Register all built-in view types with their markup-settable properties. Safe to call twice.
        static void RegisterBuiltins();

    private:
        [[nodiscard]] static HashMap<String, ViewFactory>& ViewFactories()
        {
            static HashMap<String, ViewFactory> v;
            return v;
        }
        [[nodiscard]] static HashMap<String, PropertySetter>& ViewProps()
        {
            static HashMap<String, PropertySetter> v;
            return v;
        }
        [[nodiscard]] static HashMap<String, LayoutParamsFactory>& LayoutFactories()
        {
            static HashMap<String, LayoutParamsFactory> v;
            return v;
        }
        [[nodiscard]] static HashMap<String, LayoutParamSetter>& LayoutParams_()
        {
            static HashMap<String, LayoutParamSetter> v;
            return v;
        }

        [[nodiscard]] static String Key(StringView elem, StringView name)
        {
            String k(elem);
            k.Append(u8"\x1f");
            k.Append(name);
            return k;
        }

        [[nodiscard]] static bool EndsWith(StringView s, StringView suffix)
        {
            return s.Size() >= suffix.Size() &&
                   StringView{s.Data() + (s.Size() - suffix.Size()), suffix.Size()} == suffix;
        }
        [[nodiscard]] static StringView Chop(StringView s, usize n)
        {
            return (s.Size() >= n) ? StringView{s.Data(), s.Size() - n} : StringView{};
        }

        // Parse helpers used by the built-in setters.
        [[nodiscard]] static Optional<f32> PF(StringView v)
        {
            Optional<f64> d = ParseFloat(v);
            return d.HasValue() ? Optional<f32>{static_cast<f32>(d.Value())} : Optional<f32>{};
        }
        [[nodiscard]] static bool PB(StringView v) { return v == u8"true"; }
    };

    inline void MarkupRegistry::RegisterBuiltins()
    {
        static bool registered = false;
        if (registered)
        {
            return;
        }
        registered = true;

        // Common View properties (id/class/style/visibility/opacity/padding/tooltip/cursor/...) are
        // handled inline by MarkupLoader.

        // === Layouts ===

        auto flexDirection = [](View* v, StringView val)
        {
            if (FlexLayout* c = Cast<FlexLayout>(v))
            {
                c->Direction =
                    (val == u8"vertical") ? Orientation::Vertical : Orientation::Horizontal;
            }
        };
        auto flexJustify = [](View* v, StringView val)
        {
            if (FlexLayout* c = Cast<FlexLayout>(v))
            {
                if (val == u8"start")
                {
                    c->JustifyContent = Justify::Start;
                }
                else if (val == u8"end")
                {
                    c->JustifyContent = Justify::End;
                }
                else if (val == u8"center")
                {
                    c->JustifyContent = Justify::Center;
                }
                else if (val == u8"space-between")
                {
                    c->JustifyContent = Justify::SpaceBetween;
                }
                else if (val == u8"space-around")
                {
                    c->JustifyContent = Justify::SpaceAround;
                }
                else if (val == u8"space-evenly")
                {
                    c->JustifyContent = Justify::SpaceEvenly;
                }
            }
        };
        auto flexAlign = [](View* v, StringView val)
        {
            if (FlexLayout* c = Cast<FlexLayout>(v))
            {
                if (val == u8"start")
                {
                    c->AlignItems = Align::Start;
                }
                else if (val == u8"end")
                {
                    c->AlignItems = Align::End;
                }
                else if (val == u8"center")
                {
                    c->AlignItems = Align::Center;
                }
                else if (val == u8"stretch")
                {
                    c->AlignItems = Align::Stretch;
                }
                else if (val == u8"baseline")
                {
                    c->AlignItems = Align::Baseline;
                }
            }
        };
        auto flexSpacing = [](View* v, StringView val)
        {
            if (FlexLayout* c = Cast<FlexLayout>(v))
            {
                if (auto f = PF(val); f.HasValue())
                {
                    c->Spacing = f.Value();
                }
            }
        };

        RegisterView(u8"Flex",
                     []() -> RefPtr<View> { return MakeRef<FlexLayout>(DefaultAllocator()); });
        RegisterView(u8"FlexLayout",
                     []() -> RefPtr<View> { return MakeRef<FlexLayout>(DefaultAllocator()); });
        RegisterProperty(u8"Flex", u8"direction", flexDirection);
        RegisterProperty(u8"Flex", u8"justify", flexJustify);
        RegisterProperty(u8"Flex", u8"align", flexAlign);
        RegisterProperty(u8"Flex", u8"spacing", flexSpacing);
        RegisterProperty(u8"FlexLayout", u8"direction", flexDirection);
        RegisterProperty(u8"FlexLayout", u8"justify", flexJustify);
        RegisterProperty(u8"FlexLayout", u8"align", flexAlign);
        RegisterProperty(u8"FlexLayout", u8"spacing", flexSpacing);

        auto flexGrow = [](LayoutParams* lp, StringView val)
        {
            if (FlexLayoutParams* flp = Cast<FlexLayoutParams>(lp))
            {
                if (auto f = PF(val); f.HasValue())
                {
                    flp->Grow = f.Value();
                }
            }
        };
        auto flexShrink = [](LayoutParams* lp, StringView val)
        {
            if (FlexLayoutParams* flp = Cast<FlexLayoutParams>(lp))
            {
                if (auto f = PF(val); f.HasValue())
                {
                    flp->Shrink = f.Value();
                }
            }
        };
        RegisterLayout(u8"Flex", []() -> RefPtr<LayoutParams>
                       { return MakeRef<FlexLayoutParams>(DefaultAllocator()); });
        RegisterLayoutParam(u8"Flex", u8"grow", flexGrow);
        RegisterLayoutParam(u8"Flex", u8"shrink", flexShrink);
        RegisterLayout(u8"FlexLayout", []() -> RefPtr<LayoutParams>
                       { return MakeRef<FlexLayoutParams>(DefaultAllocator()); });
        RegisterLayoutParam(u8"FlexLayout", u8"grow", flexGrow);
        RegisterLayoutParam(u8"FlexLayout", u8"shrink", flexShrink);

        auto frameGravity = [](LayoutParams* lp, StringView val)
        {
            if (FrameLayoutParams* flp = Cast<FrameLayoutParams>(lp))
            {
                flp->Gravity = ParseGravity(val);
            }
        };
        RegisterView(u8"Frame",
                     []() -> RefPtr<View> { return MakeRef<FrameLayout>(DefaultAllocator()); });
        RegisterView(u8"FrameLayout",
                     []() -> RefPtr<View> { return MakeRef<FrameLayout>(DefaultAllocator()); });
        RegisterLayout(u8"Frame", []() -> RefPtr<LayoutParams>
                       { return MakeRef<FrameLayoutParams>(DefaultAllocator()); });
        RegisterLayoutParam(u8"Frame", u8"gravity", frameGravity);
        RegisterLayout(u8"FrameLayout", []() -> RefPtr<LayoutParams>
                       { return MakeRef<FrameLayoutParams>(DefaultAllocator()); });
        RegisterLayoutParam(u8"FrameLayout", u8"gravity", frameGravity);

        auto dockLastFill = [](View* v, StringView val)
        {
            if (DockLayout* c = Cast<DockLayout>(v))
            {
                c->LastChildFill = PB(val);
            }
        };
        auto dockParam = [](LayoutParams* lp, StringView val)
        {
            if (DockLayoutParams* dlp = Cast<DockLayoutParams>(lp))
            {
                if (val == u8"left")
                {
                    dlp->Dock = Dock::Left;
                }
                else if (val == u8"top")
                {
                    dlp->Dock = Dock::Top;
                }
                else if (val == u8"right")
                {
                    dlp->Dock = Dock::Right;
                }
                else if (val == u8"bottom")
                {
                    dlp->Dock = Dock::Bottom;
                }
                else if (val == u8"fill")
                {
                    dlp->Dock = Dock::Fill;
                }
            }
        };
        RegisterView(u8"Dock",
                     []() -> RefPtr<View> { return MakeRef<DockLayout>(DefaultAllocator()); });
        RegisterView(u8"DockLayout",
                     []() -> RefPtr<View> { return MakeRef<DockLayout>(DefaultAllocator()); });
        RegisterProperty(u8"Dock", u8"last-child-fill", dockLastFill);
        RegisterProperty(u8"DockLayout", u8"last-child-fill", dockLastFill);
        RegisterLayout(u8"Dock", []() -> RefPtr<LayoutParams>
                       { return MakeRef<DockLayoutParams>(DefaultAllocator()); });
        RegisterLayoutParam(u8"Dock", u8"dock", dockParam);
        RegisterLayout(u8"DockLayout", []() -> RefPtr<LayoutParams>
                       { return MakeRef<DockLayoutParams>(DefaultAllocator()); });
        RegisterLayoutParam(u8"DockLayout", u8"dock", dockParam);

        RegisterView(u8"Flow",
                     []() -> RefPtr<View> { return MakeRef<FlowLayout>(DefaultAllocator()); });
        RegisterView(u8"FlowLayout",
                     []() -> RefPtr<View> { return MakeRef<FlowLayout>(DefaultAllocator()); });
        RegisterView(u8"Absolute",
                     []() -> RefPtr<View> { return MakeRef<AbsoluteLayout>(DefaultAllocator()); });
        RegisterView(u8"AbsoluteLayout",
                     []() -> RefPtr<View> { return MakeRef<AbsoluteLayout>(DefaultAllocator()); });
        RegisterView(u8"Grid",
                     []() -> RefPtr<View> { return MakeRef<GridLayout>(DefaultAllocator()); });
        RegisterView(u8"GridLayout",
                     []() -> RefPtr<View> { return MakeRef<GridLayout>(DefaultAllocator()); });

        // === Controls ===

        RegisterView(u8"Panel",
                     []() -> RefPtr<View> { return MakeRef<Panel>(DefaultAllocator()); });
        RegisterView(u8"ScrollView",
                     []() -> RefPtr<View> { return MakeRef<ScrollView>(DefaultAllocator()); });

        RegisterView(u8"Label",
                     []() -> RefPtr<View> { return MakeRef<Label>(DefaultAllocator()); });
        RegisterProperty(u8"Label", u8"text",
                         [](View* v, StringView val)
                         {
                             if (Label* c = Cast<Label>(v))
                             {
                                 c->Text.SetValue(String(val));
                             }
                         });
        RegisterProperty(u8"Label", u8"font-size",
                         [](View* v, StringView val)
                         {
                             if (Label* c = Cast<Label>(v))
                             {
                                 if (auto f = PF(val); f.HasValue())
                                 {
                                     c->FontSize.SetValue(f);
                                 }
                             }
                         });
        RegisterProperty(u8"Label", u8"font-family",
                         [](View* v, StringView val)
                         {
                             if (Label* c = Cast<Label>(v))
                             {
                                 c->FontFamily.SetValue(String(val));
                             }
                         });
        RegisterProperty(u8"Label", u8"word-wrap",
                         [](View* v, StringView val)
                         {
                             if (Label* c = Cast<Label>(v))
                             {
                                 c->WordWrap.SetValue(PB(val));
                             }
                         });
        RegisterProperty(u8"Label", u8"ellipsis",
                         [](View* v, StringView val)
                         {
                             if (Label* c = Cast<Label>(v))
                             {
                                 c->Ellipsis.SetValue(PB(val));
                             }
                         });

        RegisterView(u8"Button", []() -> RefPtr<View>
                     { return MakeRef<Button>(DefaultAllocator(), StringView{}); });
        RegisterProperty(u8"Button", u8"text",
                         [](View* v, StringView val)
                         {
                             if (Button* c = Cast<Button>(v))
                             {
                                 c->SetText(val);
                             }
                         });
        RegisterProperty(u8"Button", u8"font-size",
                         [](View* v, StringView val)
                         {
                             if (Button* c = Cast<Button>(v))
                             {
                                 if (auto f = PF(val); f.HasValue())
                                 {
                                     c->FontSize.SetValue(f);
                                 }
                             }
                         });
        RegisterProperty(u8"Button", u8"font-family",
                         [](View* v, StringView val)
                         {
                             if (Button* c = Cast<Button>(v))
                             {
                                 c->FontFamily.SetValue(String(val));
                             }
                         });

        // Icon button: the icon drawable is set in code (or a theme part); markup exposes its size.
        RegisterView(u8"IconButton", []() -> RefPtr<View>
                     { return MakeRef<IconButton>(DefaultAllocator(), nullptr); });
        RegisterProperty(u8"IconButton", u8"size",
                         [](View* v, StringView val)
                         {
                             if (IconButton* c = Cast<IconButton>(v))
                             {
                                 if (auto f = PF(val); f.HasValue())
                                 {
                                     c->SetSize(f.Value());
                                 }
                             }
                         });

        RegisterView(u8"CheckBox",
                     []() -> RefPtr<View> { return MakeRef<CheckBox>(DefaultAllocator()); });
        RegisterProperty(u8"CheckBox", u8"text",
                         [](View* v, StringView val)
                         {
                             if (CheckBox* c = Cast<CheckBox>(v))
                             {
                                 c->Text.SetValue(String(val));
                             }
                         });
        RegisterProperty(u8"CheckBox", u8"is-checked",
                         [](View* v, StringView val)
                         {
                             if (CheckBox* c = Cast<CheckBox>(v))
                             {
                                 c->IsChecked.SetValue(PB(val));
                             }
                         });

        RegisterView(u8"RadioButton",
                     []() -> RefPtr<View> { return MakeRef<RadioButton>(DefaultAllocator()); });
        RegisterProperty(u8"RadioButton", u8"text",
                         [](View* v, StringView val)
                         {
                             if (RadioButton* c = Cast<RadioButton>(v))
                             {
                                 c->Text.SetValue(String(val));
                             }
                         });

        RegisterView(u8"RadioGroup",
                     []() -> RefPtr<View> { return MakeRef<RadioGroup>(DefaultAllocator()); });

        RegisterView(u8"ToggleSwitch",
                     []() -> RefPtr<View> { return MakeRef<ToggleSwitch>(DefaultAllocator()); });
        RegisterProperty(u8"ToggleSwitch", u8"text",
                         [](View* v, StringView val)
                         {
                             if (ToggleSwitch* c = Cast<ToggleSwitch>(v))
                             {
                                 c->Text.SetValue(String(val));
                             }
                         });
        RegisterProperty(u8"ToggleSwitch", u8"is-checked",
                         [](View* v, StringView val)
                         {
                             if (ToggleSwitch* c = Cast<ToggleSwitch>(v))
                             {
                                 c->IsChecked.SetValue(PB(val));
                             }
                         });

        RegisterView(u8"ToggleButton",
                     []() -> RefPtr<View> { return MakeRef<ToggleButton>(DefaultAllocator()); });
        RegisterProperty(u8"ToggleButton", u8"is-checked",
                         [](View* v, StringView val)
                         {
                             if (ToggleButton* c = Cast<ToggleButton>(v))
                             {
                                 c->IsChecked.SetValue(PB(val));
                             }
                         });

        RegisterView(u8"Slider",
                     []() -> RefPtr<View> { return MakeRef<Slider>(DefaultAllocator()); });
        RegisterProperty(u8"Slider", u8"min",
                         [](View* v, StringView val)
                         {
                             if (Slider* c = Cast<Slider>(v))
                             {
                                 if (auto f = PF(val); f.HasValue())
                                 {
                                     c->Min.SetValue(f.Value());
                                 }
                             }
                         });
        RegisterProperty(u8"Slider", u8"max",
                         [](View* v, StringView val)
                         {
                             if (Slider* c = Cast<Slider>(v))
                             {
                                 if (auto f = PF(val); f.HasValue())
                                 {
                                     c->Max.SetValue(f.Value());
                                 }
                             }
                         });
        RegisterProperty(u8"Slider", u8"value",
                         [](View* v, StringView val)
                         {
                             if (Slider* c = Cast<Slider>(v))
                             {
                                 if (auto f = PF(val); f.HasValue())
                                 {
                                     c->Value.SetValue(f.Value());
                                 }
                             }
                         });
        RegisterProperty(u8"Slider", u8"step",
                         [](View* v, StringView val)
                         {
                             if (Slider* c = Cast<Slider>(v))
                             {
                                 if (auto f = PF(val); f.HasValue())
                                 {
                                     c->Step.SetValue(f.Value());
                                 }
                             }
                         });

        RegisterView(u8"ProgressBar",
                     []() -> RefPtr<View> { return MakeRef<ProgressBar>(DefaultAllocator()); });
        RegisterProperty(u8"ProgressBar", u8"value",
                         [](View* v, StringView val)
                         {
                             if (ProgressBar* c = Cast<ProgressBar>(v))
                             {
                                 if (auto f = PF(val); f.HasValue())
                                 {
                                     c->Value.SetValue(f.Value());
                                 }
                             }
                         });

        RegisterView(u8"EditText",
                     []() -> RefPtr<View> { return MakeRef<EditText>(DefaultAllocator()); });
        RegisterProperty(u8"EditText", u8"text",
                         [](View* v, StringView val)
                         {
                             if (EditText* c = Cast<EditText>(v))
                             {
                                 c->SetText(val);
                             }
                         });
        RegisterProperty(u8"EditText", u8"placeholder",
                         [](View* v, StringView val)
                         {
                             if (EditText* c = Cast<EditText>(v))
                             {
                                 c->SetPlaceholder(val);
                             }
                         });
        RegisterProperty(u8"EditText", u8"is-read-only",
                         [](View* v, StringView val)
                         {
                             if (EditText* c = Cast<EditText>(v))
                             {
                                 c->IsReadOnly.SetValue(PB(val));
                             }
                         });
        RegisterProperty(u8"EditText", u8"multiline",
                         [](View* v, StringView val)
                         {
                             if (EditText* c = Cast<EditText>(v))
                             {
                                 c->Multiline.SetValue(PB(val));
                             }
                         });
        RegisterProperty(u8"EditText", u8"max-length",
                         [](View* v, StringView val)
                         {
                             if (EditText* c = Cast<EditText>(v))
                             {
                                 if (Optional<i64> n = ParseInt(val); n.HasValue())
                                 {
                                     c->MaxLength.SetValue(static_cast<i32>(n.Value()));
                                 }
                             }
                         });

        RegisterView(u8"PasswordBox",
                     []() -> RefPtr<View> { return MakeRef<PasswordBox>(DefaultAllocator()); });
        RegisterProperty(u8"PasswordBox", u8"placeholder",
                         [](View* v, StringView val)
                         {
                             if (PasswordBox* c = Cast<PasswordBox>(v))
                             {
                                 c->SetPlaceholder(val);
                             }
                         });

        RegisterView(u8"NumericField",
                     []() -> RefPtr<View> { return MakeRef<NumericField>(DefaultAllocator()); });
        RegisterProperty(u8"NumericField", u8"value",
                         [](View* v, StringView val)
                         {
                             if (NumericField* c = Cast<NumericField>(v))
                             {
                                 if (Optional<f64> d = ParseFloat(val); d.HasValue())
                                 {
                                     c->SetValue(d.Value());
                                 }
                             }
                         });
        RegisterProperty(u8"NumericField", u8"min",
                         [](View* v, StringView val)
                         {
                             if (NumericField* c = Cast<NumericField>(v))
                             {
                                 if (Optional<f64> d = ParseFloat(val); d.HasValue())
                                 {
                                     c->SetMin(d.Value());
                                 }
                             }
                         });
        RegisterProperty(u8"NumericField", u8"max",
                         [](View* v, StringView val)
                         {
                             if (NumericField* c = Cast<NumericField>(v))
                             {
                                 if (Optional<f64> d = ParseFloat(val); d.HasValue())
                                 {
                                     c->SetMax(d.Value());
                                 }
                             }
                         });
        RegisterProperty(u8"NumericField", u8"step",
                         [](View* v, StringView val)
                         {
                             if (NumericField* c = Cast<NumericField>(v))
                             {
                                 if (Optional<f64> d = ParseFloat(val); d.HasValue())
                                 {
                                     c->SetStep(d.Value());
                                 }
                             }
                         });
        RegisterProperty(u8"NumericField", u8"show-spin-buttons",
                         [](View* v, StringView val)
                         {
                             if (NumericField* c = Cast<NumericField>(v))
                             {
                                 c->ShowSpinButtons.SetValue(PB(val));
                             }
                         });

        RegisterView(u8"Expander",
                     []() -> RefPtr<View> { return MakeRef<Expander>(DefaultAllocator()); });
        RegisterProperty(u8"Expander", u8"header-text",
                         [](View* v, StringView val)
                         {
                             if (Expander* c = Cast<Expander>(v))
                             {
                                 c->SetHeaderText(val);
                             }
                         });
        RegisterProperty(u8"Expander", u8"is-expanded",
                         [](View* v, StringView val)
                         {
                             if (Expander* c = Cast<Expander>(v))
                             {
                                 c->SetIsExpanded(PB(val));
                             }
                         });

        RegisterView(u8"TabView",
                     []() -> RefPtr<View> { return MakeRef<TabView>(DefaultAllocator()); });

        RegisterView(u8"ComboBox",
                     []() -> RefPtr<View> { return MakeRef<ComboBox>(DefaultAllocator()); });
        RegisterView(u8"Spacer",
                     []() -> RefPtr<View> { return MakeRef<Spacer>(DefaultAllocator()); });
        RegisterProperty(u8"Spacer", u8"spacer-width",
                         [](View* v, StringView val)
                         {
                             if (Spacer* c = Cast<Spacer>(v))
                             {
                                 if (auto f = PF(val); f.HasValue())
                                 {
                                     c->SpacerWidth.SetValue(f.Value());
                                 }
                             }
                         });
        RegisterProperty(u8"Spacer", u8"spacer-height",
                         [](View* v, StringView val)
                         {
                             if (Spacer* c = Cast<Spacer>(v))
                             {
                                 if (auto f = PF(val); f.HasValue())
                                 {
                                     c->SpacerHeight.SetValue(f.Value());
                                 }
                             }
                         });

        RegisterView(u8"Separator",
                     []() -> RefPtr<View> { return MakeRef<Separator>(DefaultAllocator()); });
        RegisterProperty(u8"Separator", u8"orientation",
                         [](View* v, StringView val)
                         {
                             if (Separator* c = Cast<Separator>(v))
                             {
                                 c->Orientation.SetValue((val == u8"horizontal")
                                                             ? Orientation::Horizontal
                                                             : Orientation::Vertical);
                             }
                         });

        RegisterView(u8"ColorView",
                     []() -> RefPtr<View> { return MakeRef<ColorView>(DefaultAllocator()); });
        RegisterView(u8"ImageView",
                     []() -> RefPtr<View> { return MakeRef<ImageView>(DefaultAllocator()); });
        RegisterView(u8"DrawableView",
                     []() -> RefPtr<View> { return MakeRef<DrawableView>(DefaultAllocator()); });
        RegisterView(u8"ListView",
                     []() -> RefPtr<View> { return MakeRef<ListView>(DefaultAllocator()); });
        RegisterView(u8"TreeView",
                     []() -> RefPtr<View> { return MakeRef<TreeView>(DefaultAllocator()); });
        RegisterView(u8"GridView",
                     []() -> RefPtr<View> { return MakeRef<GridView>(DefaultAllocator()); });
    }
}
