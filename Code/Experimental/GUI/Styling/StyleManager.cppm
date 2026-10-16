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
// partition cycle. Re-resolves the whole subtree on ApplyTree; dirty-tracking (only
// re-resolve invalidated widgets) is not implemented. Cache entries are keyed by raw
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

        void SetStyleSheet(StyleSheet sheet)
        {
            m_sheet = core::Move(sheet);
            m_cache = {};
        } // hot-reload: drop cache
        [[nodiscard]] const StyleSheet& GetStyleSheet() const noexcept { return m_sheet; }

        void SetMediaContext(const MediaContext& context) { m_context = context; }
        [[nodiscard]] const MediaContext& GetMediaContext() const noexcept { return m_context; }

        // Root font size for CSS `rem` units (default 16px).
        void SetRootFontSize(f32 pixels) noexcept { m_rootFontSize = pixels; }
        [[nodiscard]] f32 GetRootFontSize() const noexcept { return m_rootFontSize; }

        // Loads background-image assets referenced by the sheet. Null = background-image skipped.
        void SetResourceProvider(IResourceProvider* resources) noexcept { m_resources = resources; }
        [[nodiscard]] IResourceProvider* GetResourceProvider() const noexcept
        {
            return m_resources;
        }

        // Resolves font-family/-size in the sheet (the app's font service - VFS-backed or not;
        // the GUI is agnostic). Null = font-family skipped.
        void SetFontService(fonts::IFontService* fontService) noexcept
        {
            m_fontService = fontService;
        }
        [[nodiscard]] fonts::IFontService* GetFontService() const noexcept { return m_fontService; }

        // Resolve + apply one widget. A property that changed and has a `transition` moves from
        // the value showing toward its new one, an `animation` runs its @keyframes, both through
        // the widget's StyleAnimation on its GUI's clock; otherwise the style applies at once.
        void ApplyTo(UIWidget& widget)
        {
            // Custom properties inherit: the nearest styled ancestor's resolved style gives those
            // this widget does not set (ApplyTree styles parents first, so theirs are current).
            const ResolvedStyle* inherited = nullptr;
            for (Node* ancestor = widget.GetParent(); ancestor != nullptr && inherited == nullptr;
                 ancestor = ancestor->GetParent())
                inherited = m_cache.Find(ancestor);
            ResolvedStyle resolved = m_sheet.Resolve(widget, m_context, true, {}, inherited);

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
                const Array<TransitionSpec> transitions = ParseTransitionList(resolved.Get(core::StringView(u8"transition")));
                if (!transitions.IsEmpty())
                    StartTransitions(transitions, *previous, resolved, animation, ensure);
            }

            // Keyframe animations: start those newly named (once: one that has ended stays ended
            // while it is named), stop those no longer named.
            const Array<AnimationSpec> animations = ParseAnimationList(resolved.Get(core::StringView(u8"animation")));
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
                const Keyframes* keyframes = m_sheet.FindKeyframes(spec.Name.AsView());
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
            m_cache.InsertOrAssign(&widget, core::Move(resolved));

            // Pseudo-element parts (tag::part): resolve + apply each part the widget declares.
            Array<core::StringView> parts;
            widget.CollectStyleParts(parts);
            for (const core::StringView part : parts)
            {
                const ResolvedStyle partStyle = m_sheet.Resolve(widget, m_context, true, part);
                ApplyPartStyle(widget, part, partStyle);
            }
        }

        // Apply to every UIWidget in the subtree.
        void ApplyTree(Node& root)
        {
            if (UIWidget* widget = core::Cast<UIWidget>(&root))
                ApplyTo(*widget);
            for (usize i = 0; i < root.ChildCount(); ++i)
                if (Node* child = root.GetChildAt(i))
                    ApplyTree(*child);
        }

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

        StyleSheet m_sheet;
        MediaContext m_context;
        IResourceProvider* m_resources = nullptr;     // non-owning; loads background-image assets
        fonts::IFontService* m_fontService = nullptr; // non-owning; resolves font-family
        f32 m_rootFontSize = 16.0f;                   // CSS `rem` base
        HashMap<Node*, ResolvedStyle> m_cache; // last-applied style per widget (non-owning keys)
        HashMap<Node*, RefPtr<StyleAnimation>> m_animations; // widget -> its running transitions and animations
        HashMap<Node*, Array<core::String>> m_startedAnimations; // widget -> the @keyframes it has started
    };
}
