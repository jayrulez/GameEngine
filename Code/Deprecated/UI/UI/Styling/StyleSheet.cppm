// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI - :style_sheet partition
//
// Rule-based cascading style system: rules match views by selector chain, the cascade orders
// every matching rule by specificity and then source order, and the last declaration of a
// property wins. Rules, owned drawables and resources are all held by RefPtr. Fluent builders
// return a StyleRule& to the sheet-owned rule. The sheet is an Object so UIContexts can share it
// through RefPtr.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.ui:style_sheet;

import foundation.core;
import :control_state;
import :thickness;
import :drawable;
import :color_drawable;
import :style_property;
import :style_value;
import :style_selector;
import :style_rule;

using namespace foundation::core;

export namespace foundation::ui
{
    class
        View; // defined in :view, where Resolve(view, prop) is defined (breaks the View and styling cycle)

    /// Inheritable style properties (the text properties). These take the parent's computed
    /// value (in View.ResolveStyle) when nothing sets them on the view itself.
    [[nodiscard]] constexpr bool IsInheritableStyle(StyleProperty prop) noexcept
    {
        switch (prop)
        {
        case StyleProperty::TextColor:
        case StyleProperty::TextDimColor:
        case StyleProperty::FontSize:
        case StyleProperty::FontFamily:
        case StyleProperty::WordWrap:
            return true;
        default:
            return false;
        }
    }

    /// A sheet's `@keyframes name { ... }`: its frames by offset (0 to 1, ascending), each the
    /// declarations written under it held as a rule.
    class KeyframesRule : public Object
    {
        RTTI_OBJECT(KeyframesRule, Object)
    public:
        struct Frame
        {
            f32 Offset = 0.0f;
            RefPtr<StyleRule> Declarations;
        };
        String Name;
        Array<Frame> Frames;

        KeyframesRule() = default;

        /// Whether any frame declares `prop`.
        [[nodiscard]] bool Animates(StyleProperty prop) const
        {
            for (const Frame& frame : Frames)
            {
                if (frame.Declarations->GetValue(prop).HasValue())
                {
                    return true;
                }
            }
            return false;
        }
    };

    class StyleSheet : public Object
    {
        RTTI_OBJECT(StyleSheet, Object)
    public:
        StyleSheet() = default;
        ~StyleSheet() override
        {
            // Rules may outlive the sheet (shared through MergeFrom): never leave them pointing
            // at this counter.
            for (const RefPtr<StyleRule>& r : m_rules)
            {
                if (r->OwnerVersion() == &m_version)
                {
                    r->BindOwnerVersion(nullptr);
                }
            }
        }

        /// This sheet's edit counter: bumped by every rule added and every edit to a rule it
        /// owns. A view's computed-style cache keys on the versions of the sheets in its own chain
        /// (the context sheet, its ancestors' local sheets, its inline sheet). Nothing is
        /// process-wide, so contexts with different themes never flush each other.
        [[nodiscard]] u32 Version() const noexcept { return m_version; }

        // === Rule management ===
        void AddRule(RefPtr<StyleRule> rule)
        {
            rule->BindOwnerVersion(&m_version);
            m_rules.PushBack(Move(rule));
            ++m_version;
        }
        /// Insert a rule before every existing one, at the lowest source order, so a rule of the
        /// same specificity declared in the sheet beats it. The loader uses this for its
        /// palette-variables rule.
        void PrependRule(RefPtr<StyleRule> rule)
        {
            rule->BindOwnerVersion(&m_version);
            m_rules.Insert(0, Move(rule));
            ++m_version;
        }
        [[nodiscard]] usize RuleCount() const noexcept { return m_rules.Size(); }

        /// Adds an @keyframes, replacing one of the same name (the later wins, as in CSS).
        void AddKeyframes(RefPtr<KeyframesRule> keyframes)
        {
            for (RefPtr<KeyframesRule>& existing : m_keyframes)
            {
                if (existing->Name == keyframes->Name)
                {
                    existing = Move(keyframes);
                    ++m_version;
                    return;
                }
            }
            m_keyframes.PushBack(Move(keyframes));
            ++m_version;
        }
        /// The @keyframes of this name, or null.
        [[nodiscard]] const KeyframesRule* FindKeyframes(StringView name) const
        {
            for (const RefPtr<KeyframesRule>& keyframes : m_keyframes)
            {
                if (keyframes->Name == name)
                {
                    return keyframes.Get();
                }
            }
            return nullptr;
        }

        [[nodiscard]] bool IsEmpty() const noexcept { return m_rules.Size() == 0; }
        /// Access a rule by index (for tests).
        [[nodiscard]] const StyleRule& GetRule(usize index) const { return *m_rules[index]; }

        /// Merge another sheet's rules and owned resources into this one (used by @import). Rules and
        /// owned drawables and resources are shared RefPtrs, so this copies the references; `other` may be
        /// released afterwards without invalidating them.
        void MergeFrom(StyleSheet& other)
        {
            for (const RefPtr<StyleRule>& r : other.m_rules)
            {
                r->BindOwnerVersion(&m_version); // this sheet owns it now: `other` is transient (@import)
                m_rules.PushBack(r);
            }
            for (const RefPtr<Drawable>& d : other.m_ownedDrawables)
            {
                m_ownedDrawables.PushBack(d);
            }
            for (const RefPtr<Object>& res : other.m_ownedResources)
            {
                m_ownedResources.PushBack(res);
            }
            for (const RefPtr<KeyframesRule>& keyframes : other.m_keyframes)
            {
                AddKeyframes(keyframes);
            }
            ++m_version;
        }

        // === The cascade primitive ===
        /// Appends every rule matching (view, state, pseudo) in ascending cascade order
        /// (specificity, then source order), so the last rule in `out` that declares a property
        /// wins it. Callers concatenate several sheets (context, local, inline) into one list.
        /// Defined in :view, which has View complete for Matches.
        void CollectMatching(const View& view, ControlState state, StringView pseudo,
                             Array<const StyleRule*>& out) const;

        // === Inline-sheet rule helpers ===
        [[nodiscard]] StyleRule& GetOrCreateInlineElementRule()
        {
            if (StyleRule* r = FindInlineElementRule())
            {
                return *r;
            }
            return AddNewRule();
        }
        [[nodiscard]] StyleRule* FindInlineElementRule()
        {
            for (const RefPtr<StyleRule>& r : m_rules)
            {
                if (r->Selector.IsEmpty())
                {
                    return r.Get();
                }
            }
            return nullptr;
        }
        [[nodiscard]] StyleRule& GetOrCreateInlinePartRule(StringView part)
        {
            if (StyleRule* r = FindInlinePartRule(part))
            {
                return *r;
            }
            StyleRule& rule = AddNewRule();
            rule.Selector.SetPseudoElement(part);
            return rule;
        }
        [[nodiscard]] StyleRule* FindInlinePartRule(StringView part)
        {
            for (const RefPtr<StyleRule>& r : m_rules)
            {
                if (r->Selector.IsPseudoElementOnly(part))
                {
                    return r.Get();
                }
            }
            return nullptr;
        }

        // === Convenience rule builders (return the sheet-owned rule for fluent .Set chaining) ===
        StyleRule& ForAll() { return AddNewRule(); }
        StyleRule& ForType(const TypeInfo* viewType)
        {
            StyleRule& r = AddNewRule();
            r.Selector.ViewType = viewType;
            return r;
        }
        StyleRule& ForType(const TypeInfo* viewType, StringView styleClass)
        {
            StyleRule& r = AddNewRule();
            r.Selector.ViewType = viewType;
            r.Selector.AddClass(styleClass);
            return r;
        }
        StyleRule& ForTypeState(const TypeInfo* viewType, ControlState state)
        {
            StyleRule& r = AddNewRule();
            r.Selector.ViewType = viewType;
            r.Selector.State = state;
            return r;
        }
        StyleRule& ForTypeClassState(const TypeInfo* viewType, StringView styleClass,
                                     ControlState state)
        {
            StyleRule& r = AddNewRule();
            r.Selector.ViewType = viewType;
            r.Selector.AddClass(styleClass);
            r.Selector.State = state;
            return r;
        }
        StyleRule& ForClass(StringView styleClass)
        {
            StyleRule& r = AddNewRule();
            r.Selector.AddClass(styleClass);
            return r;
        }
        StyleRule& ForTypePseudo(const TypeInfo* viewType, StringView pseudoElement)
        {
            StyleRule& r = AddNewRule();
            r.Selector.ViewType = viewType;
            r.Selector.SetPseudoElement(pseudoElement);
            return r;
        }
        StyleRule& ForTypePseudoState(const TypeInfo* viewType, StringView pseudoElement,
                                      ControlState state)
        {
            StyleRule& r = AddNewRule();
            r.Selector.ViewType = viewType;
            r.Selector.SetPseudoElement(pseudoElement);
            r.Selector.State = state;
            return r;
        }

        // === Resource ownership (the sheet keeps a reference for its lifetime) ===
        void OwnDrawable(RefPtr<Drawable> drawable)
        {
            if (drawable)
            {
                m_ownedDrawables.PushBack(Move(drawable));
            }
        }
        void OwnResource(RefPtr<Object> resource)
        {
            if (resource)
            {
                m_ownedResources.PushBack(Move(resource));
            }
        }
        /// Create a sheet-owned ColorDrawable and return a shared reference to it.
        [[nodiscard]] RefPtr<ColorDrawable> OwnColor(Color color)
        {
            RefPtr<ColorDrawable> d = MakeRef<ColorDrawable>(MemoryAllocator(), color);
            m_ownedDrawables.PushBack(d);
            return d;
        }

        // === Resolution (per sheet; inline styles and inheritance live on View.ResolveStyle) ===
        // Defined in :view, since it calls view.GetControlState() and needs View complete.
        [[nodiscard]] StyleValue Resolve(const View& view, StyleProperty prop) const;
        [[nodiscard]] StyleValue ResolvePart(const View& view, StringView pseudoElement,
                                             StyleProperty prop, ControlState partState) const
        {
            return ResolveMatching(view, partState, pseudoElement, prop);
        }

        [[nodiscard]] Color ResolveColor(const View& view, StyleProperty prop,
                                         Color defaultVal = Color::White) const
        {
            if (Optional<Color> c = Resolve(view, prop).AsColor(); c.HasValue())
            {
                return c.Value();
            }
            return defaultVal;
        }
        [[nodiscard]] f32 ResolveFloat(const View& view, StyleProperty prop,
                                       f32 defaultVal = 0.0f) const
        {
            if (Optional<f32> f = Resolve(view, prop).AsFloat(); f.HasValue())
            {
                return f.Value();
            }
            return defaultVal;
        }
        [[nodiscard]] Thickness ResolveThickness(const View& view, StyleProperty prop,
                                                 Thickness defaultVal = {}) const
        {
            if (Optional<Thickness> t = Resolve(view, prop).AsThickness(); t.HasValue())
            {
                return t.Value();
            }
            return defaultVal;
        }
        [[nodiscard]] Drawable* ResolveDrawable(const View& view, StyleProperty prop) const
        {
            return Resolve(view, prop).AsDrawable();
        }
        [[nodiscard]] bool ResolveBool(const View& view, StyleProperty prop,
                                       bool defaultVal = false) const
        {
            if (Optional<bool> b = Resolve(view, prop).AsBool(); b.HasValue())
            {
                return b.Value();
            }
            return defaultVal;
        }

        [[nodiscard]] Drawable* ResolvePartDrawable(const View& view, StringView part,
                                                    StyleProperty prop,
                                                    ControlState partState) const
        {
            return ResolvePart(view, part, prop, partState).AsDrawable();
        }
        [[nodiscard]] Color ResolvePartColor(const View& view, StringView part, StyleProperty prop,
                                             ControlState partState,
                                             Color defaultVal = Color::White) const
        {
            if (Optional<Color> c = ResolvePart(view, part, prop, partState).AsColor();
                c.HasValue())
            {
                return c.Value();
            }
            return defaultVal;
        }
        [[nodiscard]] f32 ResolvePartFloat(const View& view, StringView part, StyleProperty prop,
                                           ControlState partState, f32 defaultVal = 0.0f) const
        {
            if (Optional<f32> f = ResolvePart(view, part, prop, partState).AsFloat(); f.HasValue())
            {
                return f.Value();
            }
            return defaultVal;
        }

    private:
        StyleRule& AddNewRule()
        {
            RefPtr<StyleRule> r = MakeRef<StyleRule>(MemoryAllocator());
            StyleRule& ref = *r;
            r->BindOwnerVersion(&m_version);
            m_rules.PushBack(Move(r));
            ++m_version;
            return ref;
        }

        [[nodiscard]] StyleValue ResolveMatching(const View& view, ControlState state,
                                                 StringView pseudo, StyleProperty prop) const
        {
            // The ordered cascade over this sheet: the last matching declaration wins. Rules are
            // stored in declaration order, so an equal-specificity rule declared later beats an
            // earlier one, and a per-type FontSize beats the global `View { FontSize }`.
            Array<const StyleRule*> matching;
            CollectMatching(view, state, pseudo, matching);
            for (usize i = matching.Size(); i-- > 0;)
            {
                if (Optional<StyleValue> val = matching[i]->GetValue(prop); val.HasValue())
                {
                    return val.Value();
                }
            }
            return StyleValue::None();
        }

        Array<RefPtr<StyleRule>> m_rules;
        Array<RefPtr<KeyframesRule>> m_keyframes;
        Array<RefPtr<Drawable>> m_ownedDrawables;
        Array<RefPtr<Object>> m_ownedResources;
        u32 m_version = 1;
    };

    RTTI_DEFINE_OBJECT(KeyframesRule, "rtti::ui")
    RTTI_DEFINE_OBJECT(StyleSheet, "rtti::ui")
}
