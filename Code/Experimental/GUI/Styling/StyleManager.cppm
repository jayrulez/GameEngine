// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - :style_manager partition
//
// StyleManager: the glue that makes a StyleSheet live on a widget tree. It resolves each
// UIWidget against the sheet (+ MediaContext), applies the result, and - by caching the
// last-applied ResolvedStyle per widget - animates transitioned changes on the next apply.
// Re-applying after a state change (e.g. :hover) therefore drives CSS transitions live.
//
// The per-widget cache lives here (not on UIWidget) to avoid a :ui_widget <-> :style_sheet
// partition cycle. ApplyTree re-resolves only what changed: the widgets a state, class, tag, id
// or parent change marked (Node::MarkStyleDirty) and their subtrees, the children of a widget
// whose custom properties changed, and everything after a new sheet, media or fonts. Cache entries are keyed by raw
// Node* - call Forget()/Clear() when widgets are destroyed (no automatic lifecycle wiring).

module;
#include "Core/Prelude.h"

export module experimental.gui:style_manager;

import foundation.core;  // HashMap, Cast, Move, Array, StringView, Float2, Duration, MakeRef
import foundation.fonts; // IFontService
import :node;
import :ui_node;
import :ui_widget;
import :style_rule; // StyleProperty
import :style_sheet;
import :media_query;
import :style_applier;
import :style_animation;
import :resource_provider;
import :css_values; // ParseLength

using namespace foundation::core;
namespace core = foundation::core;
namespace fonts = foundation::fonts;

export namespace experimental::gui
{
    class StyleManager
    {
    public:
        StyleManager() = default;
        explicit StyleManager(StyleSheet sheet) : m_sheet(core::Move(sheet)) {}

        /// The GUI's sheet (the author origin): over the user agent's, under a subtree's and inline.
        void SetStyleSheet(StyleSheet sheet)
        {
            m_sheet = core::Move(sheet);
            m_cache = {};
            m_allDirty = true; // hot-reload: every widget restyles
            m_relationsKnown = false;
        }
        [[nodiscard]] const StyleSheet& GetStyleSheet() const noexcept { return m_sheet; }

        /// The user agent's sheet, the lowest origin: the built-in theme, what every rule of the
        /// GUI's own sheets overrides.
        void SetUserAgentSheet(StyleSheet sheet)
        {
            m_userAgent = core::Move(sheet);
            m_cache = {};
            m_allDirty = true;
            m_relationsKnown = false;
        }
        [[nodiscard]] const StyleSheet& GetUserAgentSheet() const noexcept { return m_userAgent; }

        /// A sheet for `root` and what is under it alone (a page's own .css): over the GUI's
        /// sheet, under inline; a subtree inside another's outranks it. The root must be in a GUI
        /// (it is held weakly, through its GUI's element table); false when it is in none.
        bool SetSubtreeSheet(Node& root, StyleSheet sheet)
        {
            const Node::ElementRef ref = root.TakeElementRef();
            if (ref.table == nullptr)
                return false;
            RemoveSubtreeSheet(root);
            m_subtrees.PushBack(SubtreeSheet{ref, core::Move(sheet)});
            root.MarkStyleDirty(true);
            m_relationsKnown = false;
            return true;
        }
        void RemoveSubtreeSheet(Node& root)
        {
            for (usize i = 0; i < m_subtrees.Size(); ++i)
                if (RootOf(m_subtrees[i]) == &root)
                {
                    m_subtrees.RemoveAt(i);
                    root.MarkStyleDirty(true);
                    m_relationsKnown = false;
                    return;
                }
        }

        void SetMediaContext(const MediaContext& context)
        {
            if (context.Width == m_context.Width && context.Height == m_context.Height && context.Dpi == m_context.Dpi)
                return; // unchanged: nothing to restyle (a host may set it every frame)
            m_context = context;
            m_allDirty = true;
        }
        [[nodiscard]] const MediaContext& GetMediaContext() const noexcept { return m_context; }

        // Root font size for CSS `rem` units (default 16px).
        void SetRootFontSize(f32 pixels) noexcept
        {
            m_allDirty = m_allDirty || pixels != m_rootFontSize;
            m_rootFontSize = pixels;
        }
        [[nodiscard]] f32 GetRootFontSize() const noexcept { return m_rootFontSize; }

        // Loads background-image assets referenced by the sheet. Null = background-image skipped.
        void SetResourceProvider(IResourceProvider* resources) noexcept
        {
            m_allDirty = m_allDirty || resources != m_resources;
            m_resources = resources;
        }
        [[nodiscard]] IResourceProvider* GetResourceProvider() const noexcept
        {
            return m_resources;
        }

        // Resolves font-family/-size in the sheet (the app's font service - VFS-backed or not;
        // the GUI is agnostic). Null = font-family skipped.
        void SetFontService(fonts::IFontService* fontService) noexcept
        {
            m_allDirty = m_allDirty || fontService != m_fontService;
            m_fontService = fontService;
        }
        [[nodiscard]] fonts::IFontService* GetFontService() const noexcept { return m_fontService; }

        // Resolve + apply one widget. A property that changed and has a `transition` moves from
        // the value showing toward its new one, an `animation` runs its @keyframes, both through
        // the widget's StyleAnimation on its GUI's clock; otherwise the style applies at once.
        void ApplyTo(UIWidget& widget) { (void)ApplyOne(widget); }

        /// Restyles what changed in the subtree (only what changed: a widget whose class, id, tag
        /// or state changed, or that joined the tree, with its descendants; the children of one
        /// whose custom properties changed; everything after the sheet, the media, the fonts or
        /// the resources changed). A clean subtree is not visited.
        void ApplyTree(Node& root)
        {
            m_lastRestyleCount = 0;
            if (!m_relationsKnown)
                LearnRelations();
            const bool all = m_allDirty;
            m_allDirty = false;
            Visit(root, all, false);
        }

        /// How many widgets the last ApplyTree restyled (none in a frame where nothing changed).
        [[nodiscard]] u32 LastRestyleCount() const noexcept { return m_lastRestyleCount; }

    private:
        void Visit(Node& node, bool forceSubtree, bool forceSelf)
        {
            // A state a rule reads through this node (as an ancestor's, or an earlier sibling's)
            // restyles what it reaches; any other state change restyles the node alone.
            const u32 states = node.GetChangedStyleStates();
            if ((states & m_siblingStates) != 0)
                if (Node* parent = node.GetParent())
                    for (usize i = parent->GetChildIndex(&node) + 1; i < parent->ChildCount(); ++i)
                        parent->GetChildAt(i)->MarkStyleDirty(true);
            const bool self = forceSubtree || forceSelf || node.IsStyleDirty();
            const bool subtree = forceSubtree || node.IsStyleSubtreeDirty() || (states & m_ancestorStates) != 0;
            const bool descend = subtree || node.HasStyleDirtyDescendant();
            node.ClearStyleDirty();
            bool customsChanged = false;
            if (self)
            {
                if (UIWidget* widget = core::Cast<UIWidget>(&node))
                {
                    customsChanged = ApplyOne(*widget);
                    ++m_lastRestyleCount;
                }
            }
            if (!descend && !customsChanged)
                return;
            for (usize i = 0; i < node.ChildCount(); ++i)
                if (Node* child = node.GetChildAt(i))
                    Visit(*child, subtree, customsChanged);
        }

        [[nodiscard]] static bool SameCustomProperties(const ResolvedStyle& a, const ResolvedStyle& b)
        {
            const auto contains = [](const ResolvedStyle& in, const ResolvedProperty& p)
            { return in.Get(p.Name.AsView()) == p.Value.AsView() && in.Has(p.Name.AsView()); };
            for (const ResolvedProperty& p : a.Properties())
                if (ResolvedStyle::IsCustomProperty(p.Name.AsView()) && !contains(b, p))
                    return false;
            for (const ResolvedProperty& p : b.Properties())
                if (ResolvedStyle::IsCustomProperty(p.Name.AsView()) && !contains(a, p))
                    return false;
            return true;
        }

        // Restyles one widget; whether its custom properties changed (its children then restyle,
        // as they inherit them).
        bool ApplyOne(UIWidget& widget)
        {
            // Custom properties inherit: the nearest styled ancestor's resolved style gives those
            // this widget does not set (ApplyTree styles parents first, so theirs are current).
            const ResolvedStyle* inherited = nullptr;
            for (Node* ancestor = widget.GetParent(); ancestor != nullptr && inherited == nullptr;
                 ancestor = ancestor->GetParent())
                inherited = m_cache.Find(ancestor);
            const Array<const StyleSheet*> subtrees = SubtreeSheetsOver(widget);
            ResolvedStyle resolved = ResolveFor(widget, subtrees, {}, inherited);
            resolved.SettleKeywords(inherited);
            const UINode::TextStyle textBefore = widget.GetTextStyle();

            // Build the length-resolution context: viewport from the media context, root font
            // size configurable, element font size from this widget's resolved font-size (for em).
            StyleApplyContext context;
            context.Resources = m_resources;
            context.FontService = m_fontService;
            context.Lengths.RootFontSize = m_rootFontSize;
            context.Lengths.ViewportWidth = m_context.Width;
            context.Lengths.ViewportHeight = m_context.Height;
            context.Lengths.ElementFontSize =
                ParseLength(resolved.Get(core::StringView(u8"font-size"), core::StringView(u8"")))
                    .ValueOr(m_rootFontSize);

            RefPtr<StyleAnimation>* running = m_animations.Find(&widget);
            if (running != nullptr && (*running)->IsDone())
            {
                m_animations.Remove(&widget);
                running = nullptr;
            }
            RefPtr<StyleAnimation> animation = running != nullptr ? *running : RefPtr<StyleAnimation>{};
            bool created = false;
            const auto ensure = [&]()
            {
                if (!animation && widget.GetActionManager() != nullptr)
                {
                    animation = core::MakeRef<StyleAnimation>(widget.MemoryAllocator(), widget);
                    animation->SetTargetStyle(resolved, context);
                    created = true;
                }
                return animation.Get() != nullptr;
            };

            // Transitions: each property that changed since the last apply and a `transition`
            // entry covers, from what shows now.
            if (const ResolvedStyle* previous = m_cache.Find(&widget))
            {
                const Array<TransitionSpec> transitions =
                    ComposeTransitions(resolved.Get(u8"transition-property"), resolved.Get(u8"transition-duration"),
                                       resolved.Get(u8"transition-timing-function"), resolved.Get(u8"transition-delay"));
                if (!transitions.IsEmpty())
                    StartTransitions(transitions, *previous, resolved, animation, ensure);
            }

            // Keyframe animations: start those newly named (once: one that has ended stays ended
            // while it is named), stop those no longer named.
            const Array<AnimationSpec> animations = ComposeAnimations(
                resolved.Get(u8"animation-name"), resolved.Get(u8"animation-duration"),
                resolved.Get(u8"animation-timing-function"), resolved.Get(u8"animation-delay"),
                resolved.Get(u8"animation-iteration-count"), resolved.Get(u8"animation-direction"),
                resolved.Get(u8"animation-fill-mode"), resolved.Get(u8"animation-play-state"));
            Array<core::String>* startedSlot = m_startedAnimations.Find(&widget);
            Array<core::String>& started =
                startedSlot != nullptr ? *startedSlot : m_startedAnimations.InsertOrAssign(&widget, Array<core::String>{});
            for (usize i = 0; i < started.Size();)
            {
                bool named = false;
                for (const AnimationSpec& spec : animations)
                    named = named || spec.Name.AsView() == started[i].AsView();
                if (named)
                {
                    ++i;
                    continue;
                }
                if (animation)
                    animation->StopKeyframes(started[i].AsView());
                started.RemoveAt(i);
            }
            for (const AnimationSpec& spec : animations)
            {
                bool already = false;
                for (const core::String& name : started)
                    already = already || name.AsView() == spec.Name.AsView();
                if (already && animation)
                    animation->SetPaused(spec.Name.AsView(), spec.Paused);
                const Keyframes* keyframes = FindKeyframes(subtrees, spec.Name.AsView());
                if (already || keyframes == nullptr)
                    continue;
                if (ensure())
                {
                    animation->SetTargetStyle(resolved, context);
                    animation->StartKeyframes(spec, *keyframes);
                    started.PushBack(spec.Name);
                }
            }

            if (animation && animation->TrackCount() > 0)
            {
                animation->SetTargetStyle(resolved, context);
                if (created)
                {
                    m_animations.InsertOrAssign(&widget, animation);
                    widget.RunAction(animation); // Start applies it now
                }
                else
                {
                    animation->ApplyNow();
                }
            }
            else
            {
                if (animation)
                {
                    // Nothing moves any more: the target, with what the stopped tracks moved
                    // returned to it (or to its initial value).
                    animation->SetTargetStyle(resolved, context);
                    animation->ApplyNow();
                    animation->Stop();
                    m_animations.Remove(&widget);
                }
                else
                {
                    ApplyStyle(widget, resolved, context.Resources, context.FontService, context.Lengths);
                }
            }
            const ResolvedStyle* before = m_cache.Find(&widget);
            // Its children restyle when what they inherit changed: a custom property, or its text style.
            const bool customsChanged = before == nullptr || !SameCustomProperties(*before, resolved) ||
                                        !(textBefore == widget.GetTextStyle());
            m_cache.InsertOrAssign(&widget, core::Move(resolved));

            // Pseudo-element parts (tag::part): resolve + apply each part the widget declares.
            Array<core::StringView> parts;
            widget.CollectStyleParts(parts);
            if (widget.IsScrollContainer()) // any node that scrolls has its bars' parts
            {
                parts.PushBack(core::StringView(u8"scrollbar"));
                parts.PushBack(core::StringView(u8"scrollbar-thumb"));
            }
            for (const core::StringView part : parts)
            {
                const ResolvedStyle partStyle = ResolveFor(widget, subtrees, part, nullptr, widget.GetPartStates(part));
                ApplyPartStyle(widget, part, partStyle, context.Resources, context.Lengths);
            }
            return customsChanged;
        }

    public:
        // Drop a widget's cached style (call before it is destroyed).
        void Forget(Node* widget)
        {
            m_cache.Remove(widget);
            m_animations.Remove(widget);
            m_startedAnimations.Remove(widget);
        }
        void Clear()
        {
            m_cache = {};
            m_animations = {};
            m_startedAnimations = {};
        }

    private:
        struct SubtreeSheet
        {
            Node::ElementRef Root;
            StyleSheet Sheet;
        };
        [[nodiscard]] static Node* RootOf(const SubtreeSheet& s)
        {
            return s.Root.table != nullptr ? s.Root.table->Resolve(s.Root.slot, s.Root.generation) : nullptr;
        }

        // The subtree sheets whose root is `widget` or holds it, the outermost first.
        [[nodiscard]] Array<const StyleSheet*> SubtreeSheetsOver(const Node& widget) const
        {
            Array<const StyleSheet*> out;
            if (m_subtrees.IsEmpty())
                return out;
            for (const Node* n = &widget; n != nullptr; n = n->GetParent())
                for (const SubtreeSheet& s : m_subtrees)
                    if (RootOf(s) == n)
                        out.PushBack(&s.Sheet);
            for (usize a = 0, b = out.Size(); a + 1 < b; ++a, --b) // innermost first, so reverse
                core::Swap(out[a], out[b - 1]);
            return out;
        }

        // The cascade for `widget` (or its `part`) over every origin: the user agent's sheet, the
        // GUI's, the subtrees' (outermost first), then the widget's inline style.
        [[nodiscard]] ResolvedStyle ResolveFor(const UIWidget& widget, const Array<const StyleSheet*>& subtrees,
                                               core::StringView part, const ResolvedStyle* inherited, u32 partStates = 0) const
        {
            Array<StyleRuleMatch> matches;
            m_userAgent.CollectMatches(widget, m_context, true, part, 0, matches, partStates);
            m_sheet.CollectMatches(widget, m_context, true, part, 1, matches, partStates);
            u32 origin = 2;
            for (const StyleSheet* sheet : subtrees)
                sheet->CollectMatches(widget, m_context, true, part, origin++, matches, partStates);
            return StyleSheet::Cascade(matches, inherited,
                                       part.Size() == 0 ? widget.GetInlineStyle() : core::Span<const InlineStyleDeclaration>{});
        }

        // A @keyframes by name: the innermost subtree's, then the GUI's, then the user agent's.
        [[nodiscard]] const Keyframes* FindKeyframes(const Array<const StyleSheet*>& subtrees, core::StringView name) const
        {
            for (usize i = subtrees.Size(); i > 0; --i)
                if (const Keyframes* k = subtrees[i - 1]->FindKeyframes(name))
                    return k;
            if (const Keyframes* k = m_sheet.FindKeyframes(name))
                return k;
            return m_userAgent.FindKeyframes(name);
        }

        // A transition for each property that changed between `previous` and `resolved` and an
        // entry covers (its own name, or `all` for the transitionable ones; the last entry naming
        // a property wins), from the value showing now toward the new one.
        template <typename Ensure>
        static void StartTransitions(const Array<TransitionSpec>& transitions, const ResolvedStyle& previous,
                                     const ResolvedStyle& resolved, RefPtr<StyleAnimation>& animation, Ensure&& ensure)
        {
            for (const ResolvedProperty& p : resolved.Properties())
            {
                const core::StringView name = p.Name.AsView();
                if (ResolvedStyle::IsCustomProperty(name) || p.Typed.Kind == StyleValueKind::None)
                    continue;
                const TransitionSpec* spec = nullptr;
                for (const TransitionSpec& t : transitions)
                    if (t.Property.AsView() == name || (t.Property.AsView() == core::StringView(u8"all") && IsTransitionable(name)))
                        spec = &t;
                if (spec == nullptr || spec->Duration <= 0.0f)
                    continue;
                const StyleValue* before = previous.Value(name);
                if (before == nullptr || previous.Get(name) == p.Value.AsView())
                    continue; // no starting value, or no change
                StyleValue from = *before;
                if (animation)
                    (void)animation->TrackValue(name, from); // retarget from where a running track is
                if (ensure())
                    animation->StartTransition(name, from, p.Typed, *spec);
            }
        }

        // The states the sheets read through an ancestor and through a sibling (LearnRelations).
        void LearnRelations()
        {
            m_ancestorStates = 0;
            m_siblingStates = 0;
            const auto learn = [&](const StyleSheet& sheet)
            {
                for (const StyleRule& rule : sheet.Rules())
                    rule.Selector().CollectRelationStates(m_ancestorStates, m_siblingStates);
            };
            learn(m_userAgent);
            learn(m_sheet);
            for (const SubtreeSheet& s : m_subtrees)
                learn(s.Sheet);
            m_relationsKnown = true;
        }

        StyleSheet m_userAgent;
        StyleSheet m_sheet;
        Array<SubtreeSheet> m_subtrees;
        MediaContext m_context;
        IResourceProvider* m_resources = nullptr;     // non-owning; loads background-image assets
        fonts::IFontService* m_fontService = nullptr; // non-owning; resolves font-family
        f32 m_rootFontSize = 16.0f;                   // CSS `rem` base
        HashMap<Node*, ResolvedStyle> m_cache; // last-applied style per widget (non-owning keys)
        HashMap<Node*, RefPtr<StyleAnimation>> m_animations; // widget -> its running transitions and animations
        HashMap<Node*, Array<core::String>> m_startedAnimations; // widget -> the @keyframes it has started
        bool m_allDirty = true; // the next ApplyTree restyles everything (a new sheet, media, fonts)
        bool m_relationsKnown = false;
        u32 m_ancestorStates = 0; // states a rule reads through an ancestor: a change restyles the subtree
        u32 m_siblingStates = 0;  // states a rule reads through an earlier sibling: a change restyles the later siblings
        u32 m_lastRestyleCount = 0;
    };
}
