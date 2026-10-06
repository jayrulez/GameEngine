// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI - :markup_registry partition
//
// Maps XML element names to View factories and property/layout-param setters (registration-based, not
// reflection). Ported from Sedulous.UI/src/Markup/MarkupRegistry.bf. Divergences: Beef `delegate View()`
// / `delegate void(View, StringView)` -> plain function pointers (the built-in factories/setters capture
// nothing; a captureless lambda converts to a fn-ptr); Beef nested `Dictionary<String, Registration>`
// with an inner Dictionary -> flat HashMaps keyed by "element\x1fname" (avoids nested-container copies);
// the static tables -> function-local statics (no static-init-order issues); Beef `as X` -> Cast<X>;
// float.Parse -> core::ParseFloat. Layout attributes write the view's LayoutStyle (one vocabulary,
// every element; see LayoutAttributeNames) - no per-parent param factories.

module;
#include "Core/Prelude.h"

export module foundation.ui:markup_registry;

import foundation.core;
import foundation.vg; // CornerRadii (an ImageView's corner-radius)
import :view;
import :layout_style;
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
import :content_button;
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

using namespace foundation::core;

export namespace foundation::ui
{
    /// Maps XML element names to View factories and property setters. Registration-based (explicit and
    /// debuggable) - used by MarkupLoader to create views and set attributes from .sml files.
    struct MarkupRegistry
    {
        using ViewFactory = RefPtr<View> (*)(IAllocator&);
        using PropertySetter = void (*)(View*, StringView);

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

        // === Lookup ===

        /// Create a view for the given element name. Null if not registered.
        [[nodiscard]] static RefPtr<View> CreateView(StringView elementName,
                                                     IAllocator& allocator)
        {
            if (ViewFactory* f = ViewFactories().Find(String(elementName)))
            {
                return (*f)(allocator);
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

        // === Layout attributes (the LayoutStyle vocabulary; the same on EVERY element) ===

        /// The attribute names that write a view's LayoutStyle. One table feeds the loader,
        /// the completion vocabulary and the tests, so they cannot drift.
        [[nodiscard]] static Span<const StringView> LayoutAttributeNames()
        {
            static constexpr StringView kNames[] = {
                u8"width",    u8"height",   u8"margin",        u8"flex-grow",
                u8"flex-shrink", u8"align-self", u8"gravity",  u8"dock",
                u8"left",     u8"top",      u8"right",         u8"bottom",
                u8"position", u8"z-index",  u8"min-width",     u8"min-height",
                u8"max-width", u8"max-height", u8"flex-basis", u8"grid-row",   u8"grid-column",
                u8"grid-row-span", u8"grid-column-span"};
            return Span<const StringView>{kNames, sizeof(kNames) / sizeof(kNames[0])};
        }

        /// Apply one layout attribute to `layout`. Returns false when `name` is not a layout
        /// attribute (the caller then tries the element's registered properties). An
        /// unparseable value leaves the field untouched.
        static bool ApplyLayoutAttribute(LayoutStyle& layout, StringView name, StringView value)
        {
            if (name == u8"width")
            {
                layout.Width = ParseSizeSpec(value);
                return true;
            }
            if (name == u8"height")
            {
                layout.Height = ParseSizeSpec(value);
                return true;
            }
            if (name == u8"margin")
            {
                layout.Margin = ParseThickness(value);
                return true;
            }
            if (name == u8"flex-grow")
            {
                if (Optional<f64> f = ParseFloat(value); f.HasValue())
                {
                    layout.FlexGrow = static_cast<f32>(f.Value());
                }
                return true;
            }
            if (name == u8"flex-shrink")
            {
                if (Optional<f64> f = ParseFloat(value); f.HasValue())
                {
                    layout.FlexShrink = static_cast<f32>(f.Value());
                }
                return true;
            }
            if (name == u8"align-self")
            {
                layout.AlignSelf = ParseAlign(value);
                return true;
            }
            if (name == u8"flex-basis")
            {
                if (Optional<Unit> length = StyleValueParser::ParseLengthText(value); length.HasValue())
                {
                    layout.FlexBasis = length.Value();
                }
                return true;
            }
            if (name == u8"gravity")
            {
                layout.Gravity = ParseGravity(value);
                return true;
            }
            if (name == u8"dock")
            {
                if (Optional<Dock> d = ParseDock(value); d.HasValue())
                {
                    layout.Dock = d.Value();
                }
                return true;
            }
            if (name == u8"left")
            {
                if (Optional<f64> f = ParseFloat(value); f.HasValue())
                {
                    layout.Left = static_cast<f32>(f.Value());
                }
                return true;
            }
            if (name == u8"top")
            {
                if (Optional<f64> f = ParseFloat(value); f.HasValue())
                {
                    layout.Top = static_cast<f32>(f.Value());
                }
                return true;
            }
            if (name == u8"right")
            {
                if (Optional<f64> f = ParseFloat(value); f.HasValue())
                {
                    layout.Right = static_cast<f32>(f.Value());
                }
                return true;
            }
            if (name == u8"bottom")
            {
                if (Optional<f64> f = ParseFloat(value); f.HasValue())
                {
                    layout.Bottom = static_cast<f32>(f.Value());
                }
                return true;
            }
            if (name == u8"position")
            {
                if (value == u8"absolute")
                {
                    layout.Position = Position::Absolute;
                }
                else if (value == u8"static")
                {
                    layout.Position = Position::Static;
                }
                return true;
            }
            if (name == u8"z-index")
            {
                if (Optional<i64> i = ParseInt(value); i.HasValue())
                {
                    layout.ZIndex = static_cast<i32>(i.Value());
                }
                return true;
            }
            if (name == u8"min-width" || name == u8"min-height" || name == u8"max-width" ||
                name == u8"max-height")
            {
                if (Optional<Unit> length = StyleValueParser::ParseLengthText(value); length.HasValue())
                {
                    if (name == u8"min-width")
                        layout.MinWidth = length.Value();
                    else if (name == u8"min-height")
                        layout.MinHeight = length.Value();
                    else if (name == u8"max-width")
                        layout.MaxWidth = length.Value();
                    else
                        layout.MaxHeight = length.Value();
                }
                return true;
            }
            if (name == u8"grid-row")
            {
                if (Optional<i64> i = ParseInt(value); i.HasValue())
                {
                    layout.GridRow = static_cast<i32>(i.Value());
                }
                return true;
            }
            if (name == u8"grid-column")
            {
                if (Optional<i64> i = ParseInt(value); i.HasValue())
                {
                    layout.GridColumn = static_cast<i32>(i.Value());
                }
                return true;
            }
            if (name == u8"grid-row-span")
            {
                if (Optional<i64> i = ParseInt(value); i.HasValue())
                {
                    layout.GridRowSpan = static_cast<i32>(i.Value());
                }
                return true;
            }
            if (name == u8"grid-column-span")
            {
                if (Optional<i64> i = ParseInt(value); i.HasValue())
                {
                    layout.GridColumnSpan = static_cast<i32>(i.Value());
                }
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

        /// Attribute names usable on `elementName`: its registered properties plus the
        /// LayoutStyle vocabulary (the same on every element).
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
            for (const StringView& name : LayoutAttributeNames())
            {
                pushUnique(name);
            }
        }

        [[nodiscard]] static bool IsRegistered(StringView elementName)
        {
            return ViewFactories().Find(String(elementName)) != nullptr;
        }
        // === Value parsing helpers ===

        /// Parse a SizeSpec from markup: "wrap", "match", or a length - "240", "240px", "16dp",
        /// "50%", "2em", "calc(100% - 20dp)" (see StyleValueParser::ParseLengthText).
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
            if (Optional<Unit> length = StyleValueParser::ParseLengthText(value); length.HasValue())
            {
                return SizeSpec::Fixed(length.Value());
            }
            return SizeSpec::Wrap();
        }

        /// Parse an Align value: "start", "end", "center", "stretch", "baseline". Empty otherwise.
        [[nodiscard]] static Optional<Align> ParseAlign(StringView value)
        {
            if (value == u8"start")
            {
                return Align::Start;
            }
            if (value == u8"end")
            {
                return Align::End;
            }
            if (value == u8"center")
            {
                return Align::Center;
            }
            if (value == u8"stretch")
            {
                return Align::Stretch;
            }
            if (value == u8"baseline")
            {
                return Align::Baseline;
            }
            return {};
        }

        /// Parse a Dock side: "left", "top", "right", "bottom", "fill". Empty otherwise.
        [[nodiscard]] static Optional<Dock> ParseDock(StringView value)
        {
            if (value == u8"left")
            {
                return Dock::Left;
            }
            if (value == u8"top")
            {
                return Dock::Top;
            }
            if (value == u8"right")
            {
                return Dock::Right;
            }
            if (value == u8"bottom")
            {
                return Dock::Bottom;
            }
            if (value == u8"fill")
            {
                return Dock::Fill;
            }
            return {};
        }

        /// Parse a Gravity value: "Center", "Fill", "TopLeft", "Bottom|Right", etc. The names are
        /// PascalCase and matched exactly; a name that matches none adds nothing, and is listed in
        /// `unknown` when given (the loader turns them into warnings: "top|right" is a typo that
        /// otherwise silently leaves a view where it was).
        [[nodiscard]] static Gravity ParseGravity(StringView value, Array<String>* unknown = nullptr)
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
                    else if (unknown != nullptr && s.Size() > 0)
                    {
                        unknown->PushBack(String(s));
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
            const i32 count = ParseNumbers(value, values);
            return StyleValueParser::ParseThickness(values, count);
        }

        /// Parse corner radii: "8" (all), "8 8 0 0" (top-left, top-right, bottom-right,
        /// bottom-left, as the style's radius=).
        [[nodiscard]] static vg::CornerRadii ParseCornerRadii(StringView value)
        {
            f32 values[4] = {0, 0, 0, 0};
            const i32 count = ParseNumbers(value, values);
            if (count >= 4)
            {
                return vg::CornerRadii(values[0], values[1], values[2], values[3]);
            }
            return vg::CornerRadii(count > 0 ? values[0] : 0.0f);
        }

        /// Up to four space-separated numbers into `values`; answers how many were read.
        static i32 ParseNumbers(StringView value, f32 (&values)[4])
        {
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
            return count;
        }

        /// Register all built-in view types with their markup-settable properties. Safe to call
        /// twice, and from several threads at once (the UI document cook runs per build on job
        /// workers): the first call registers, every other call waits for it and returns.
        static void RegisterBuiltins();

    private:
        static void RegisterBuiltinsBody();
        // Non-inline (UiRegistryStateImpl.cpp): the rendezvous rule, see ViewFactories.
        [[nodiscard]] static Mutex& RegistrationLock();
        [[nodiscard]] static bool& BuiltinsRegisteredFlag();
        // Registry storage accessors are NON-inline (UiRegistryStateImpl.cpp): the
        // function-local statics were chosen for init-order safety, but an inline body
        // duplicates the map per shared library - registered in one, empty in another
        // (shared-libraries.md rendezvous rule).
        [[nodiscard]] static HashMap<String, ViewFactory>& ViewFactories();
        [[nodiscard]] static HashMap<String, PropertySetter>& ViewProps();

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
        // A plain static bool let two workers both pass the check and rehash the maps under
        // each other. The lock makes the second caller WAIT for the first registration to
        // finish (it returns to read a complete registry); the flag lives in the impl unit so
        // every shared library sees the same one.
        ScopedLock lock(RegistrationLock());
        bool& registered = BuiltinsRegisteredFlag();
        if (registered)
        {
            return;
        }
        RegisterBuiltinsBody();
        registered = true;
    }

    inline void MarkupRegistry::RegisterBuiltinsBody()
    {

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

        auto flexWrap = [](View* v, StringView val)
        {
            if (FlexLayout* c = Cast<FlexLayout>(v))
            {
                c->Wrap = val == u8"wrap" || val == u8"true";
            }
        };
        auto flexAlignContent = [](View* v, StringView val)
        {
            if (FlexLayout* c = Cast<FlexLayout>(v))
            {
                if (val == u8"start")
                    c->AlignContent = AlignContent::Start;
                else if (val == u8"end")
                    c->AlignContent = AlignContent::End;
                else if (val == u8"center")
                    c->AlignContent = AlignContent::Center;
                else if (val == u8"space-between")
                    c->AlignContent = AlignContent::SpaceBetween;
                else if (val == u8"space-around")
                    c->AlignContent = AlignContent::SpaceAround;
                else if (val == u8"stretch")
                    c->AlignContent = AlignContent::Stretch;
            }
        };
        // CSS `gap: <row-gap> [<column-gap>]` (one value = both); row/column are AXES, not
        // directions - the layout maps them onto its main/cross gaps by Direction.
        auto flexGap = [](View* v, StringView val)
        {
            if (FlexLayout* c = Cast<FlexLayout>(v))
            {
                usize split = 0;
                while (split < val.Size() && val[split] != u8' ')
                {
                    ++split;
                }
                const Optional<f32> first = PF(val.SubStr(0, split));
                if (!first.HasValue())
                {
                    return;
                }
                c->RowGap = static_cast<f32>(first.Value());
                c->ColumnGap = static_cast<f32>(first.Value());
                if (split < val.Size())
                {
                    if (const Optional<f32> second = PF(val.SubStr(split + 1, val.Size() - split - 1)); second.HasValue())
                    {
                        c->ColumnGap = static_cast<f32>(second.Value());
                    }
                }
            }
        };
        auto flexRowGap = [](View* v, StringView val)
        {
            if (FlexLayout* c = Cast<FlexLayout>(v))
            {
                if (auto f = PF(val); f.HasValue())
                    c->RowGap = static_cast<f32>(f.Value());
            }
        };
        auto flexColumnGap = [](View* v, StringView val)
        {
            if (FlexLayout* c = Cast<FlexLayout>(v))
            {
                if (auto f = PF(val); f.HasValue())
                    c->ColumnGap = static_cast<f32>(f.Value());
            }
        };

        RegisterView(u8"Flex",
                     [](IAllocator& allocator) -> RefPtr<View> { return MakeRef<FlexLayout>(allocator); });
        RegisterView(u8"FlexLayout",
                     [](IAllocator& allocator) -> RefPtr<View> { return MakeRef<FlexLayout>(allocator); });
        RegisterProperty(u8"Flex", u8"direction", flexDirection);
        RegisterProperty(u8"Flex", u8"justify", flexJustify);
        RegisterProperty(u8"Flex", u8"align", flexAlign);
        RegisterProperty(u8"Flex", u8"spacing", flexSpacing);
        RegisterProperty(u8"FlexLayout", u8"direction", flexDirection);
        RegisterProperty(u8"FlexLayout", u8"justify", flexJustify);
        RegisterProperty(u8"FlexLayout", u8"align", flexAlign);
        RegisterProperty(u8"FlexLayout", u8"spacing", flexSpacing);
        static constexpr const char8_t* kFlexElements[] = {u8"Flex", u8"FlexLayout"};
        for (const char8_t* element : kFlexElements)
        {
            RegisterProperty(element, u8"wrap", flexWrap);
            RegisterProperty(element, u8"align-content", flexAlignContent);
            RegisterProperty(element, u8"gap", flexGap);
            RegisterProperty(element, u8"row-gap", flexRowGap);
            RegisterProperty(element, u8"column-gap", flexColumnGap);
        }

        RegisterView(u8"Frame",
                     [](IAllocator& allocator) -> RefPtr<View> { return MakeRef<FrameLayout>(allocator); });
        RegisterView(u8"FrameLayout",
                     [](IAllocator& allocator) -> RefPtr<View> { return MakeRef<FrameLayout>(allocator); });

        auto dockLastFill = [](View* v, StringView val)
        {
            if (DockLayout* c = Cast<DockLayout>(v))
            {
                c->LastChildFill = PB(val);
            }
        };
        RegisterView(u8"Dock",
                     [](IAllocator& allocator) -> RefPtr<View> { return MakeRef<DockLayout>(allocator); });
        RegisterView(u8"DockLayout",
                     [](IAllocator& allocator) -> RefPtr<View> { return MakeRef<DockLayout>(allocator); });
        RegisterProperty(u8"Dock", u8"last-child-fill", dockLastFill);
        RegisterProperty(u8"DockLayout", u8"last-child-fill", dockLastFill);

        RegisterView(u8"Flow",
                     [](IAllocator& allocator) -> RefPtr<View> { return MakeRef<FlowLayout>(allocator); });
        RegisterView(u8"FlowLayout",
                     [](IAllocator& allocator) -> RefPtr<View> { return MakeRef<FlowLayout>(allocator); });
        RegisterView(u8"Absolute",
                     [](IAllocator& allocator) -> RefPtr<View> { return MakeRef<AbsoluteLayout>(allocator); });
        RegisterView(u8"AbsoluteLayout",
                     [](IAllocator& allocator) -> RefPtr<View> { return MakeRef<AbsoluteLayout>(allocator); });
        RegisterView(u8"Grid",
                     [](IAllocator& allocator) -> RefPtr<View> { return MakeRef<GridLayout>(allocator); });
        RegisterView(u8"GridLayout",
                     [](IAllocator& allocator) -> RefPtr<View> { return MakeRef<GridLayout>(allocator); });

        // === Controls ===

        RegisterView(u8"Panel",
                     [](IAllocator& allocator) -> RefPtr<View> { return MakeRef<Panel>(allocator); });
        RegisterView(u8"ScrollView",
                     [](IAllocator& allocator) -> RefPtr<View> { return MakeRef<ScrollView>(allocator); });

        RegisterView(u8"Label",
                     [](IAllocator& allocator) -> RefPtr<View> { return MakeRef<Label>(allocator); });
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

        RegisterView(u8"Button", [](IAllocator& allocator) -> RefPtr<View>
                     { return MakeRef<Button>(allocator, StringView{}); });
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

        // A button that draws any view as its content (a picture card: an image and labels): its
        // one child element is that content (MarkupLoader, through View::SetContentView).
        RegisterView(u8"ContentButton", [](IAllocator& allocator) -> RefPtr<View>
                     { return MakeRef<ContentButton>(allocator); });

        // Icon button: the icon drawable is set in code (or a theme part); markup exposes its size.
        RegisterView(u8"IconButton", [](IAllocator& allocator) -> RefPtr<View>
                     { return MakeRef<IconButton>(allocator, nullptr); });
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
                     [](IAllocator& allocator) -> RefPtr<View> { return MakeRef<CheckBox>(allocator); });
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
                     [](IAllocator& allocator) -> RefPtr<View> { return MakeRef<RadioButton>(allocator); });
        RegisterProperty(u8"RadioButton", u8"text",
                         [](View* v, StringView val)
                         {
                             if (RadioButton* c = Cast<RadioButton>(v))
                             {
                                 c->Text.SetValue(String(val));
                             }
                         });

        RegisterView(u8"RadioGroup",
                     [](IAllocator& allocator) -> RefPtr<View> { return MakeRef<RadioGroup>(allocator); });

        RegisterView(u8"ToggleSwitch",
                     [](IAllocator& allocator) -> RefPtr<View> { return MakeRef<ToggleSwitch>(allocator); });
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
                     [](IAllocator& allocator) -> RefPtr<View> { return MakeRef<ToggleButton>(allocator); });
        RegisterProperty(u8"ToggleButton", u8"is-checked",
                         [](View* v, StringView val)
                         {
                             if (ToggleButton* c = Cast<ToggleButton>(v))
                             {
                                 c->IsChecked.SetValue(PB(val));
                             }
                         });

        RegisterView(u8"Slider",
                     [](IAllocator& allocator) -> RefPtr<View> { return MakeRef<Slider>(allocator); });
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
                     [](IAllocator& allocator) -> RefPtr<View> { return MakeRef<ProgressBar>(allocator); });
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
                     [](IAllocator& allocator) -> RefPtr<View> { return MakeRef<EditText>(allocator); });
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
                     [](IAllocator& allocator) -> RefPtr<View> { return MakeRef<PasswordBox>(allocator); });
        RegisterProperty(u8"PasswordBox", u8"placeholder",
                         [](View* v, StringView val)
                         {
                             if (PasswordBox* c = Cast<PasswordBox>(v))
                             {
                                 c->SetPlaceholder(val);
                             }
                         });

        RegisterView(u8"NumericField",
                     [](IAllocator& allocator) -> RefPtr<View> { return MakeRef<NumericField>(allocator); });
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
                     [](IAllocator& allocator) -> RefPtr<View> { return MakeRef<Expander>(allocator); });
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
                     [](IAllocator& allocator) -> RefPtr<View> { return MakeRef<TabView>(allocator); });

        RegisterView(u8"ComboBox",
                     [](IAllocator& allocator) -> RefPtr<View> { return MakeRef<ComboBox>(allocator); });
        RegisterView(u8"Spacer",
                     [](IAllocator& allocator) -> RefPtr<View> { return MakeRef<Spacer>(allocator); });
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
                     [](IAllocator& allocator) -> RefPtr<View> { return MakeRef<Separator>(allocator); });
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
                     [](IAllocator& allocator) -> RefPtr<View> { return MakeRef<ColorView>(allocator); });
        RegisterView(u8"ImageView",
                     [](IAllocator& allocator) -> RefPtr<View> { return MakeRef<ImageView>(allocator); });
        // What the image shows, resolved by the context's resource provider (an asset id in the
        // engine): <ImageView source="{guid}"/>.
        RegisterProperty(u8"ImageView", u8"source",
                         [](View* v, StringView val)
                         {
                             if (ImageView* c = Cast<ImageView>(v))
                             {
                                 c->Source.SetValue(String(val));
                             }
                         });
        // Rounds the picture's corners: <ImageView corner-radius="8"/> or four values.
        RegisterProperty(u8"ImageView", u8"corner-radius",
                         [](View* v, StringView val)
                         {
                             if (ImageView* c = Cast<ImageView>(v))
                             {
                                 c->CornerRadius.SetValue(ParseCornerRadii(val));
                             }
                         });
        RegisterView(u8"DrawableView",
                     [](IAllocator& allocator) -> RefPtr<View> { return MakeRef<DrawableView>(allocator); });
        RegisterView(u8"ListView",
                     [](IAllocator& allocator) -> RefPtr<View> { return MakeRef<ListView>(allocator); });
        RegisterView(u8"TreeView",
                     [](IAllocator& allocator) -> RefPtr<View> { return MakeRef<TreeView>(allocator); });
        RegisterView(u8"GridView",
                     [](IAllocator& allocator) -> RefPtr<View> { return MakeRef<GridView>(allocator); });
    }
}
