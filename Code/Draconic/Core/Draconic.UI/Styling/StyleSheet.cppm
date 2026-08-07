// Draconic UI - :style_sheet partition
//
// Rule-based cascading style system: rules match views by type/class/state; most specific match
// wins. Ported from Sedulous.UI/src/Styling/StyleSheet.bf.
//
// Divergence (language): Beef manual ownership (rules deleted, drawables ReleaseRef'd, resources
// deleted in ~this) -> RAII: rules are RefPtr<StyleRule>, owned drawables/resources are RefPtr.
// Fluent builders return StyleRule& to the sheet-owned rule. Object (RefCounted) so the sheet is
// shared between UIContexts via RefPtr.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.ui:style_sheet;

import draconic.foundation; // Object, Array, RefPtr, TypeInfo, Color, StringView, Optional
import :control_state;
import :thickness;
import :drawable;
import :color_drawable;
import :style_property;
import :style_value;
import :style_selector;
import :style_rule;

using namespace draconic::foundation;

export namespace draconic::ui
{
    class
        View; // defined in :view; Resolve(view,prop) body lives there (breaks the View<->styling cycle)

    /// Inheritable style properties - these walk the parent chain (in View.ResolveStyle) if not
    /// found on the view itself.
    [[nodiscard]] constexpr bool IsInheritableStyle(StyleProperty prop) noexcept
    {
        switch (prop)
        {
        case StyleProperty::TextColor:
        case StyleProperty::FontSize:
        case StyleProperty::FontFamily:
            return true;
        default:
            return false;
        }
    }

    class StyleSheet : public Object
    {
        DRACONIC_OBJECT(StyleSheet, Object)
    public:
        StyleSheet() = default;

        // === Rule management ===
        void AddRule(RefPtr<StyleRule> rule) { m_rules.PushBack(Move(rule)); }
        [[nodiscard]] usize RuleCount() const noexcept { return m_rules.Size(); }
        [[nodiscard]] bool IsEmpty() const noexcept { return m_rules.Size() == 0; }
        /// Access a rule by index (surfaced for tests; Beef reached mRules via [Friend]).
        [[nodiscard]] const StyleRule& GetRule(usize index) const { return *m_rules[index]; }

        /// Merge another sheet's rules and owned resources into this one (used by @import). Rules and
        /// owned drawables/resources are RefPtr (shared), so this copies the refs; `other` may be
        /// released afterwards without invalidating them.
        void MergeFrom(StyleSheet& other)
        {
            for (const RefPtr<StyleRule>& r : other.m_rules)
            {
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
        }

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

        // === Resource ownership (RAII; the sheet keeps a ref for its lifetime) ===
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
        /// Create a sheet-owned ColorDrawable and return it (shared ref).
        [[nodiscard]] RefPtr<ColorDrawable> OwnColor(Color color)
        {
            RefPtr<ColorDrawable> d = MakeRef<ColorDrawable>(DefaultAllocator(), color);
            m_ownedDrawables.PushBack(d);
            return d;
        }

        // === Resolution (per-sheet primitive; inline + inheritance live on View.ResolveStyle) ===
        // Body in :view (calls view.GetControlState(), needs View complete).
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
            RefPtr<StyleRule> r = MakeRef<StyleRule>(DefaultAllocator());
            StyleRule& ref = *r;
            m_rules.PushBack(Move(r));
            return ref;
        }

        [[nodiscard]] StyleValue ResolveMatching(const View& view, ControlState state,
                                                 StringView pseudo, StyleProperty prop) const
        {
            StyleValue best = StyleValue::None();
            i32 bestSpecificity = -1;
            for (const RefPtr<StyleRule>& rule : m_rules)
            {
                if (!rule->Selector.Matches(view, state, pseudo))
                {
                    continue;
                }
                Optional<StyleValue> val = rule->GetValue(prop);
                if (!val.HasValue())
                {
                    continue;
                }
                const i32 specificity = rule->Selector.Specificity();
                // CSS tie-break: on EQUAL specificity the LAST declared rule wins (source order), so
                // >= not >. Rules are stored in declaration order (base theme, then per-type, then
                // extensions appended last), so a later equal-specificity rule correctly overrides an
                // earlier one - e.g. a per-type FontSize beats the global `View { FontSize }`.
                if (specificity >= bestSpecificity)
                {
                    bestSpecificity = specificity;
                    best = val.Value();
                }
            }
            return best;
        }

        Array<RefPtr<StyleRule>> m_rules;
        Array<RefPtr<Drawable>> m_ownedDrawables;
        Array<RefPtr<Object>> m_ownedResources;
    };

    DRACONIC_DEFINE_OBJECT(StyleSheet, "draconic::ui")
}
