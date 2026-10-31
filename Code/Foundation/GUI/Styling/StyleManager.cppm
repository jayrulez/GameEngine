// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - :style_manager partition
//
// StyleManager: applies a StyleSheet live to a widget tree. It resolves each UIWidget against
// the sheet and the MediaContext, applies the result, and, by caching the last-applied
// ResolvedStyle per widget, animates transitioned changes on the next apply. Re-applying after a
// state change (:hover, say) therefore drives CSS transitions live.
//
// The per-widget cache lives here (not on UIWidget) to avoid a partition cycle between
// :ui_widget and :style_sheet. ApplyTree re-resolves only what changed: the widgets marked by a
// state, class, tag, id or parent change (Node::MarkStyleDirty), plus their subtrees or later
// siblings when a sheet reads that state through them; the children of a widget whose children
// were rearranged, when a sheet reads siblings; the children of a widget whose custom properties
// changed; and everything after a new sheet, media or fonts. Cache entries are keyed by the
// widget's address and checked against its slot in its GUI's element table, so a destroyed
// widget's entries are never mistaken for those of a new widget at the same address. A sweep
// drops stale entries as the caches grow, and Forget() or Clear() drops them at once.

module;
#include "Core/Prelude.h"
#include "Profiler/Profiler.h" // PROFILE_SCOPE

export module foundation.gui:style_manager;

import foundation.core;  // HashMap, Cast, Move, Array, StringView, Float2, Duration, MakeRef
import foundation.fonts; // IFontService
import foundation.profiler;
import :node;
import :frame_stats;
import :ui_node;
import :ui_widget;
import :style_rule; // StyleProperty
import :style_sheet;
import :media_query;
import :style_applier;
import :style_animation;
import :style_interpolation; // StyleInterpolationContext
import :resource_provider;
import :css_values; // ParseLength
import :css_parser; // parses an @import's sheet

using namespace foundation::core;
namespace core = foundation::core;
namespace fonts = foundation::fonts;

export namespace foundation::gui
{
    /// Where a node's value of a property came from (for the inspector).
    enum class StyleOrigin : u8
    {
        Sheet,     ///< a rule of a sheet
        Inline,    ///< the node's inline style (markup's attributes, a script's style)
        Inherited, ///< an ancestor's value (an inherited property that no rule set on this node)
        Code,      ///< a setter called from code; the sheet leaves it alone
        Own,       ///< nothing set it, so it is the node's own value
    };
    /// One declaration of a property: where it was written and what it said.
    struct StyleDeclarationSource
    {
        core::String Sheet;    ///< the sheet's name ("inline" for the inline style)
        u32 Line = 0;          ///< the rule's line in that sheet (0 when unknown)
        core::String Selector; ///< the rule's selector as written
        core::String Value;    ///< the value as written
        bool Important = false;
        bool Inline = false;
        const StyleRule* Rule = nullptr; ///< the rule (null for the inline style), for EditDeclaration
    };
    /// A property of a node, its value and where the value came from.
    struct StyleExplanation
    {
        core::String Property;
        StyleOrigin Origin = StyleOrigin::Own;
        core::String Value; ///< the cascade's value (the ancestor's when inherited; empty when code set it or for the node's own)
        /// This node's declarations of the property, the winner first (all overridden when code
        /// set it).
        Array<StyleDeclarationSource> Declarations;
        const UIWidget* InheritedFrom = nullptr; ///< the ancestor the value is inherited from
        bool Animated = false;                   ///< a transition or an animation is animating it now
    };

    class StyleManager
    {
    public:
        StyleManager() = default;
        explicit StyleManager(StyleSheet sheet) : m_sheet(core::Move(sheet)) {}

        /// Sets the GUI's sheet (the author origin). It outranks the user agent's sheet and is
        /// outranked by subtree sheets and inline styles.
        void SetStyleSheet(StyleSheet sheet)
        {
            m_sheet = core::Move(sheet);
            m_cache = {};
            m_allDirty = true; // hot-reload: every widget restyles
            m_relationsKnown = false;
            m_fontFacesLoaded = false;
            m_importsExpanded = false;
        }
        [[nodiscard]] const StyleSheet& GetStyleSheet() const noexcept { return m_sheet; }

        /// Sets the user agent's sheet, the lowest origin: the built-in theme, which every rule of
        /// the GUI's own sheets overrides.
        void SetUserAgentSheet(StyleSheet sheet)
        {
            m_userAgent = core::Move(sheet);
            m_cache = {};
            m_allDirty = true;
            m_relationsKnown = false;
            m_fontFacesLoaded = false;
            m_importsExpanded = false; // expand its sprites, as for the author sheet
        }
        [[nodiscard]] const StyleSheet& GetUserAgentSheet() const noexcept { return m_userAgent; }

        /// Sets a sheet that applies only to `root` and its descendants (a page's own .css). It
        /// outranks the GUI's sheet and is outranked by inline styles; a subtree sheet nested inside
        /// another outranks the outer one. The root must be in a GUI, since it is held weakly
        /// through its GUI's element table. Returns false when it is in none.
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
            if (context == m_context)
                return; // unchanged, so nothing to restyle (a host may set it every frame)
            const MediaContext before = m_context;
            m_context = context;
            // A change to the viewport's size alone (a window being resized, every frame of the
            // drag) restyles everything only when a sheet reads it, through vw or vh or a size query
            // that now answers differently; otherwise only the widgets whose own inline style reads
            // it restyle. A change to the input kind alone (a mouse bumped while a pad is in use)
            // restyles only when a query answers differently.
            MediaContext moved = before;
            moved.Width = context.Width;
            moved.Height = context.Height;
            moved.Input = context.Input;
            if (moved == context && m_importsExpanded)
            {
                if (!m_relationsKnown)
                    LearnRelations();
                const bool resized = before.Width != context.Width || before.Height != context.Height;
                bool flips = resized && m_sheetsReadViewport;
                for (usize i = 0; !flips && i < m_sizeQueries.Size(); ++i)
                    flips = m_sizeQueries[i]->Evaluate(before) != m_sizeQueries[i]->Evaluate(context);
                if (!flips)
                {
                    m_viewportResized = m_viewportResized || resized;
                    return;
                }
            }
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

        // Loads background-image assets referenced by the sheet. Null: background-image is skipped.
        void SetResourceProvider(IResourceProvider* resources) noexcept
        {
            m_allDirty = m_allDirty || resources != m_resources;
            m_importsExpanded = m_importsExpanded && resources == m_resources;
            m_resources = resources;
        }
        [[nodiscard]] IResourceProvider* GetResourceProvider() const noexcept
        {
            return m_resources;
        }

        // Resolves font-family and font-size in the sheet (the app's font service, VFS-backed or
        // not; the GUI does not care). Null: font-family is skipped.
        void SetFontService(fonts::IFontService* fontService) noexcept
        {
            m_allDirty = m_allDirty || fontService != m_fontService;
            m_fontFacesLoaded = m_fontFacesLoaded && fontService == m_fontService; // a new service does not have the faces yet
            m_fontService = fontService;
        }
        [[nodiscard]] fonts::IFontService* GetFontService() const noexcept { return m_fontService; }

        /// Sets the loader for a sheet's @font-face. The host loads the file at `source` (a path as
        /// the sheet wrote it) into its font service under `face`, the name the face is chosen by
        /// ("Family", "Family Bold", "Family Light Italic"), and returns whether it did. Each face
        /// is requested once per sheet, before the restyle that may draw with it; a face the
        /// service already has is not requested. With null, @font-face is skipped.
        void SetFontFaceLoader(Function<bool(core::StringView face, core::StringView source)> loader)
        {
            m_fontFaceLoader = core::Move(loader);
            m_fontFacesLoaded = false;
            m_allDirty = true;
        }

        // Resolves and applies one widget. A property that changed and has a `transition` animates
        // from the value shown toward its new value, and an `animation` runs its @keyframes, both
        // through the widget's StyleAnimation on its GUI's clock. Otherwise the style applies at
        // once.
        void ApplyTo(UIWidget& widget)
        {
            const bool filtering = m_filtering; // the walk's ancestor filter does not describe this widget's ancestors
            m_filtering = false;
            (void)ApplyOne(widget);
            m_filtering = filtering;
        }

        /// Restyles only what changed in the subtree: a widget whose class, id, tag or state
        /// changed, or that joined the tree, with its descendants; the children of a widget whose
        /// custom properties changed; and everything after the sheet, the media, the fonts or the
        /// resources changed. A clean subtree is not visited.
        void ApplyTree(Node& root)
        {
            PROFILE_SCOPE("GUI.Style");
            const core::Stopwatch watch = core::Stopwatch::StartNew();
            m_lastRestyleCount = 0;
            if (!m_relationsKnown)
                LearnRelations();
            if (!m_importsExpanded)
                ExpandImports();
            if (!m_fontFacesLoaded)
                LoadFontFaces();
            const bool all = m_allDirty;
            m_allDirty = false;
            if (m_viewportResized && !all)
                MarkViewportReaders(root);
            m_viewportResized = false;
            // The ancestor names above the widget being styled, from the walk's start upward;
            // pushed on the way down.
            m_ancestors.Clear();
            for (const Node* n = root.GetParent(); n != nullptr; n = n->GetParent())
                if (const UIWidget* w = core::Cast<UIWidget>(n))
                    m_ancestors.Push(*w);
            root.GetRootNode()->NoteStylePass(); // lets a node leaving this tree wait for its exit
            m_filtering = true;
            Visit(root, all, false);
            m_filtering = false;
            if (m_owners.Size() > m_sweepAt)
                SweepGone();
            if (FrameStats* stats = root.GetFrameStats()) // the GUI's frame stats
            {
                stats->StyleMs += watch.Elapsed().AsMilliseconds();
                stats->Restyled += m_lastRestyleCount;
            }
        }

        /// How many widgets the last ApplyTree restyled (none in a frame where nothing changed).
        [[nodiscard]] u32 LastRestyleCount() const noexcept { return m_lastRestyleCount; }
        /// How many widgets have a cached style (a destroyed widget's entry goes at the next sweep).
        [[nodiscard]] usize CachedWidgetCount() const noexcept { return m_cache.Size(); }

    private:
        void Visit(Node& node, bool forceSubtree, bool forceSelf)
        {
            // A state that a rule reads through this node (as an ancestor's or an earlier sibling's
            // state) restyles what the rule reaches; any other state change restyles only the node.
            const u32 states = node.GetChangedStyleStates();
            if ((states & m_siblingStates) != 0)
                if (Node* parent = node.GetParent())
                    for (usize i = parent->GetChildIndex(&node) + 1; i < parent->ChildCount(); ++i)
                        parent->GetChildAt(i)->MarkStyleDirty(true);
            const bool self = forceSubtree || forceSelf || node.IsStyleDirty();
            const bool subtree = forceSubtree || node.IsStyleSubtreeDirty() || RestylesBelow(node, states);
            // The node's children were added, removed or moved. When a sheet reads siblings, each
            // child may match differently, so each restyles; a child's subtree restyles too only when
            // a rule reads siblings through an ancestor step. Otherwise only the new children restyle
            // (they are marked). Restyling every child's subtree would make each popup or tooltip
            // joining the root restyle the whole page.
            const bool rearranged = m_readsSiblings && node.ChildrenArrangementChanged();
            bool descend = subtree || rearranged || node.HasStyleDirtyDescendant();
            const bool moved = node.IsStyleMotionDirty();
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
            else if (moved)
            {
                if (UIWidget* widget = core::Cast<UIWidget>(&node))
                    ApplyMotion(*widget);
            }
            // Applying the style may have marked descendants (for example a new direction).
            descend = descend || node.HasStyleDirtyDescendant();
            if (!descend && !customsChanged)
                return;
            const UIWidget* above = core::Cast<UIWidget>(&node);
            if (above != nullptr)
                m_ancestors.Push(*above);
            for (usize i = 0; i < node.ChildCount(); ++i)
                if (Node* child = node.GetChildAt(i))
                    Visit(*child, subtree || (rearranged && HoldsSiblingReads(*child)), customsChanged || rearranged);
            if (above != nullptr)
                m_ancestors.Pop();
        }

        // Whether a change of `states` on `node` can change what matches below it. That is so when a
        // rule reads one of the states through an ancestor compound that `node` could match, or
        // through a nested selector, which cannot be pinned to one widget. A hover passes through
        // every ancestor of the pointer, so restyling each one's subtree for every rule with an
        // ancestor hover (`button:hover > icon`) would restyle whole pages at every move.
        [[nodiscard]] bool RestylesBelow(const Node& node, u32 states) const
        {
            if ((states & m_ancestorStates) == 0)
                return false;
            if ((states & m_unplacedAncestorStates) != 0)
                return true;
            const UIWidget* widget = core::Cast<UIWidget>(&node);
            if (widget == nullptr)
                return true;
            for (const AncestorStateRead& read : m_ancestorStateReads)
                if ((read.States & states) != 0 && read.Compound.MatchesIdentity(*widget))
                    return true;
            return false;
        }

        // Whether a rearrangement of `child`'s siblings can change what matches inside it: that is
        // so when a rule reads siblings in a compound that `child` could match, followed by a step
        // below it.
        [[nodiscard]] bool HoldsSiblingReads(const Node& child) const
        {
            const UIWidget* widget = core::Cast<UIWidget>(&child);
            if (widget == nullptr)
                return !m_siblingReadsAbove.IsEmpty();
            for (const StyleSelectorRule& compound : m_siblingReadsAbove)
                if (compound.MatchesIdentity(*widget))
                    return true;
            return false;
        }

        [[nodiscard]] static bool SameCustomProperties(const ResolvedStyle& a, const ResolvedStyle& b)
        {
            return ResolvedStyle::SameCustomProperties(a, b);
        }

        // Builds what applying `resolved` to `widget` needs: the length-resolution context (the
        // viewport from the media context, the configured root font size, and the element's font
        // size from its resolved font-size, for em), the resources and the fonts.
        [[nodiscard]] StyleApplyContext ApplyContextFor(const UIWidget& widget, const ResolvedStyle& resolved) const
        {
            StyleApplyContext context;
            context.Resources = m_resources;
            context.FontService = m_fontService;
            context.Lengths.RootFontSize = m_rootFontSize;
            context.Lengths.ViewportWidth = m_context.Width;
            context.Lengths.ViewportHeight = m_context.Height;
            // The element's font size, which its em lengths are measured by: its own font-size
            // resolved against its parent's (em and percentages relative to the parent's, rem to
            // the root's), else the parent's, which it inherits.
            const UINode* parentNode = core::Cast<UINode>(widget.GetParent());
            const f32 parentSize =
                parentNode != nullptr && parentNode->GetTextStyle().FontSize > 0.0f ? parentNode->GetTextStyle().FontSize : m_rootFontSize;
            LengthContext parentLengths = context.Lengths;
            parentLengths.ElementFontSize = parentSize;
            const core::StringView fontSize = resolved.Get(core::StringView(u8"font-size"), core::StringView(u8""));
            context.Lengths.ElementFontSize =
                fontSize.Size() != 0 ? ResolveLength(fontSize, parentLengths, parentSize).ValueOr(parentSize) : parentSize;
            return context;
        }

        // Called when a tween moved and nothing else changed. Applies the widget's current style
        // again with the animated layer over it (through its transitions and animations when they
        // are running).
        void ApplyMotion(UIWidget& widget)
        {
            if (!IsOwn(widget))
            {
                (void)ApplyOne(widget); // the entries at its address belonged to another widget, so start over
                return;
            }
            if (const RefPtr<StyleAnimation>* running = m_animations.Find(&widget); running != nullptr && *running && !(*running)->IsDone())
            {
                (*running)->ApplyNow();
                return;
            }
            const ResolvedStyle* cached = m_cache.Find(&widget);
            if (cached == nullptr)
            {
                (void)ApplyOne(widget);
                return;
            }
            ResolvedStyle effective = *cached;
            const StyleApplyContext context = ApplyContextFor(widget, effective);
            StyleInterpolationContext lengths;
            lengths.Lengths = context.Lengths;
            lengths.BoxSize = widget.GetSize();
            OverlayMotion(widget, effective, &lengths);
            ApplyStyle(widget, effective, context.Resources, context.FontService, context.Lengths);
        }

        // Restyles one widget. Returns whether its custom properties changed, in which case its
        // children restyle too, since they inherit them.
        bool ApplyOne(UIWidget& widget)
        {
            Claim(widget);
            // Custom properties inherit: the nearest styled ancestor's resolved style supplies those
            // this widget does not set (ApplyTree styles parents first, so the ancestors' styles are
            // current).
            const ResolvedStyle* inherited = nullptr;
            for (Node* ancestor = widget.GetParent(); ancestor != nullptr && inherited == nullptr;
                 ancestor = ancestor->GetParent())
                inherited = m_cache.Find(ancestor);
            const Array<const StyleSheet*> subtrees = SubtreeSheetsOver(widget);
            ResolvedStyle resolved = ResolveFor(widget, subtrees, {}, inherited);
            resolved.SettleKeywords(inherited);
            const UINode::TextStyle textBefore = widget.GetTextStyle();

            const StyleApplyContext context = ApplyContextFor(widget, resolved);

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

            // Transitions: each property that changed since the last apply and that a `transition`
            // entry covers animates from what is shown now. An entrance's first style does not
            // transition from anything, since it is where the entrance starts.
            const bool entranceStart = widget.TakeEntranceStart();
            if (const ResolvedStyle* previous = entranceStart ? nullptr : m_cache.Find(&widget))
            {
                const Array<TransitionSpec> transitions =
                    ComposeTransitions(resolved.Get(u8"transition-property"), resolved.Get(u8"transition-duration"),
                                       resolved.Get(u8"transition-timing-function"), resolved.Get(u8"transition-delay"));
                if (!transitions.IsEmpty())
                    StartTransitions(transitions, *previous, resolved, animation, ensure);
            }

            // Keyframe animations: start those newly named (only once, so an ended animation stays
            // ended while it is still named) and stop those no longer named.
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
                    widget.RunAction(animation); // Start applies the style now
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
                    // Nothing is animating any more: apply the target, with the properties the
                    // stopped tracks animated returned to it (or to their initial values).
                    animation->SetTargetStyle(resolved, context);
                    animation->ApplyNow();
                    animation->Stop();
                    m_animations.Remove(&widget);
                }
                else if (widget.HasMotion())
                {
                    // Lay the tweens over it; the cache keeps only the cascade.
                    ResolvedStyle effective = resolved;
                    StyleInterpolationContext lengths;
                    lengths.Lengths = context.Lengths;
                    lengths.BoxSize = widget.GetSize();
                    OverlayMotion(widget, effective, &lengths);
                    ApplyStyle(widget, effective, context.Resources, context.FontService, context.Lengths);
                }
                else
                {
                    ApplyStyle(widget, resolved, context.Resources, context.FontService, context.Lengths);
                }
            }
            // A leaving widget with nothing the sheet started still waiting to end goes now (at the
            // GUI's next drain, not inside this walk). Otherwise its animation lets it go when it
            // ends.
            if (widget.WatchesMotion() && !(animation && !animation->IsDone() && animation->WaitsForEnd()))
                widget.MotionSettled(); // a leaving node goes, and an entering one advances its entrance
            const ResolvedStyle* before = m_cache.Find(&widget);
            // The children restyle when what they inherit changed: a custom property, or the text
            // style.
            const bool customsChanged = before == nullptr || !SameCustomProperties(*before, resolved) ||
                                        !(textBefore == widget.GetTextStyle());
            m_cache.InsertOrAssign(&widget, core::Move(resolved));

            // Pseudo-element parts (tag::part): resolves and applies each part the widget declares.
            Array<core::StringView> parts;
            widget.CollectStyleParts(parts);
            if (widget.IsScrollContainer()) // any node that scrolls has its scroll bars' parts
            {
                parts.PushBack(core::StringView(u8"scrollbar"));
                parts.PushBack(core::StringView(u8"scrollbar-thumb"));
            }
            // A part inherits from its widget, as CSS pseudo-elements do, so the widget's custom
            // properties (a theme's tokens) and currentColor reach it.
            const ResolvedStyle* widgetStyle = m_cache.Find(&widget);
            for (const core::StringView part : parts)
            {
                const ResolvedStyle partStyle = ResolveFor(widget, subtrees, part, widgetStyle, widget.GetPartStates(part));
                ApplyPartStyle(widget, part, partStyle, context.Resources, context.Lengths);
            }
            return customsChanged;
        }

    public:
        /// Returns every property something sets on `widget`, each with its value and origin: the
        /// cascade's longhands (each declaration, the winner first), the properties code pinned,
        /// and the inherited properties that an ancestor's rules set. Meant for the inspector, not
        /// for per-frame use.
        [[nodiscard]] Array<StyleExplanation> ExplainStyle(const UIWidget& widget) const
        {
            const Array<const StyleSheet*> subtrees = SubtreeSheetsOver(widget);
            Array<StyleRuleMatch> matches;
            Collect(widget, subtrees, {}, 0, nullptr, matches);
            const Array<LonghandTrace> traces = StyleSheet::Trace(matches, widget.GetInlineStyle());
            Array<StyleExplanation> out;
            const auto listed = [&out](core::StringView name)
            {
                for (const StyleExplanation& e : out)
                    if (e.Property.AsView() == name)
                        return true;
                return false;
            };
            for (const LonghandTrace& trace : traces)
                out.PushBack(Explain(widget, trace.Name.AsView(), &trace, subtrees));
            for (u32 slot = 0; slot < SlotCount; ++slot)
            {
                const core::StringView name(StyleSlotProperty(static_cast<StyleSlot>(slot)));
                if (widget.IsStylePinned(static_cast<StyleSlot>(slot)) && !listed(name))
                    out.PushBack(Explain(widget, name, nullptr, subtrees));
            }
            for (const StylePropertySpec& spec : AllStyleProperties())
            {
                const core::StringView name(spec.Name);
                if (spec.Inherited && !listed(name) && InheritedFrom(widget, name) != nullptr)
                    out.PushBack(Explain(widget, name, nullptr, subtrees));
            }
            return out;
        }
        /// Sets `property` to `value` in `rule` (a rule of one of this manager's sheets), as an
        /// inspector edits a rule, and restyles every node the rule matches. A longhand set in a
        /// rule written with its shorthand goes after the shorthand, so it wins for that side alone.
        /// Returns false when the rule belongs to none of the sheets. The pointer is only compared,
        /// so passing a rule from a sheet that has since been replaced is safe.
        bool EditDeclaration(const StyleRule* rule, core::StringView property, core::StringView value)
        {
            StyleRule* editable = m_userAgent.MutableRule(rule);
            if (editable == nullptr)
                editable = m_sheet.MutableRule(rule);
            for (usize i = 0; editable == nullptr && i < m_subtrees.Size(); ++i)
                editable = m_subtrees[i].Sheet.MutableRule(rule);
            if (editable == nullptr)
                return false;
            editable->SetProperty(property, value);
            m_allDirty = true;
            return true;
        }

        /// Explains one property of `widget` as ExplainStyle does (the node's own value when nothing
        /// sets it).
        [[nodiscard]] StyleExplanation ExplainProperty(const UIWidget& widget, core::StringView property) const
        {
            for (StyleExplanation& e : ExplainStyle(widget))
                if (e.Property.AsView() == property)
                    return core::Move(e);
            StyleExplanation own;
            own.Property = core::String(property);
            return own;
        }

        // Drops a widget's cached style (a destroyed widget's entries go by themselves at the next
        // sweep).
        void Forget(Node* widget)
        {
            m_cache.Remove(widget);
            m_animations.Remove(widget);
            m_startedAnimations.Remove(widget);
            m_owners.Remove(widget);
        }
        void Clear()
        {
            m_cache = {};
            m_animations = {};
            m_startedAnimations = {};
            m_owners = {};
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

        // The subtree sheets whose root is `widget` or one of its ancestors, the outermost first.
        [[nodiscard]] Array<const StyleSheet*> SubtreeSheetsOver(const Node& widget) const
        {
            Array<const StyleSheet*> out;
            if (m_subtrees.IsEmpty())
                return out;
            for (const Node* n = &widget; n != nullptr; n = n->GetParent())
                for (const SubtreeSheet& s : m_subtrees)
                    if (RootOf(s) == n)
                        out.PushBack(&s.Sheet);
            for (usize a = 0, b = out.Size(); a + 1 < b; ++a, --b) // collected innermost first, so reverse
                core::Swap(out[a], out[b - 1]);
            return out;
        }

        // Collects the rules matching `widget` (or its `part`) from every origin: the user agent's
        // sheet, the GUI's sheet, and the subtree sheets (outermost first).
        void Collect(const UIWidget& widget, const Array<const StyleSheet*>& subtrees, core::StringView part, u32 partStates,
                     const AncestorFilter* ancestors, Array<StyleRuleMatch>& matches) const
        {
            m_userAgent.CollectMatches(widget, m_context, true, part, 0, matches, partStates, ancestors);
            m_sheet.CollectMatches(widget, m_context, true, part, 1, matches, partStates, ancestors);
            u32 origin = 2;
            for (const StyleSheet* sheet : subtrees)
                sheet->CollectMatches(widget, m_context, true, part, origin++, matches, partStates, ancestors);
        }
        // The sheet a match's origin refers to.
        [[nodiscard]] const StyleSheet& SheetAt(u32 origin, const Array<const StyleSheet*>& subtrees) const
        {
            return origin == 0 ? m_userAgent : origin == 1 ? m_sheet : *subtrees[origin - 2];
        }
        // The nearest ancestor whose cascade set `property`.
        [[nodiscard]] const UIWidget* InheritedFrom(const UIWidget& widget, core::StringView property) const
        {
            for (Node* n = widget.GetParent(); n != nullptr; n = n->GetParent())
                if (const ResolvedStyle* style = m_cache.Find(n); style != nullptr && style->Has(property))
                    return core::Cast<UIWidget>(n);
            return nullptr;
        }
        [[nodiscard]] StyleExplanation Explain(const UIWidget& widget, core::StringView property, const LonghandTrace* trace,
                                               const Array<const StyleSheet*>& subtrees) const
        {
            StyleExplanation e;
            e.Property = core::String(property);
            if (trace != nullptr)
                for (const DeclarationTrace& d : trace->Declarations)
                {
                    StyleDeclarationSource source;
                    source.Value = d.Value;
                    source.Important = d.Important;
                    if (d.Rule == nullptr)
                    {
                        source.Sheet = core::String(core::StringView(u8"inline"));
                        source.Inline = true;
                    }
                    else
                    {
                        source.Rule = d.Rule;
                        source.Sheet = core::String(SheetAt(d.Origin, subtrees).Name());
                        source.Line = d.Rule->Line();
                        source.Selector = core::String(d.Rule->SelectorText());
                    }
                    e.Declarations.PushBack(core::Move(source));
                }
            bool pinned = false;
            for (u32 slot = 0; slot < SlotCount; ++slot)
                if (core::StringView(StyleSlotProperty(static_cast<StyleSlot>(slot))) == property)
                    pinned = widget.IsStylePinned(static_cast<StyleSlot>(slot));
            if (pinned)
                e.Origin = StyleOrigin::Code;
            else if (!e.Declarations.IsEmpty())
            {
                e.Origin = e.Declarations[0].Inline ? StyleOrigin::Inline : StyleOrigin::Sheet;
                const ResolvedStyle* style = m_cache.Find(const_cast<UIWidget*>(&widget));
                e.Value = style != nullptr && style->Has(property) ? core::String(style->Get(property)) : e.Declarations[0].Value;
            }
            else if (const StylePropertySpec* spec = FindStyleProperty(property); spec != nullptr && spec->Inherited)
                if (const UIWidget* from = InheritedFrom(widget, property))
                {
                    e.Origin = StyleOrigin::Inherited;
                    e.InheritedFrom = from;
                    e.Value = core::String(m_cache.Find(const_cast<UIWidget*>(from))->Get(property));
                }
            if (const RefPtr<StyleAnimation>* animation = m_animations.Find(const_cast<UIWidget*>(&widget)); animation != nullptr && *animation)
            {
                StyleValue now;
                e.Animated = (*animation)->TrackValue(property, now);
            }
            return e;
        }

        // Resolves the cascade for `widget` (or its `part`) over every origin: the user agent's
        // sheet, the GUI's sheet, the subtree sheets (outermost first), then the widget's inline
        // style.
        [[nodiscard]] ResolvedStyle ResolveFor(const UIWidget& widget, const Array<const StyleSheet*>& subtrees,
                                               core::StringView part, const ResolvedStyle* inherited, u32 partStates = 0) const
        {
            Array<StyleRuleMatch> matches;
            Collect(widget, subtrees, part, partStates, m_filtering ? &m_ancestors : nullptr, matches); // the ancestor filter applies only inside ApplyTree's walk
            return StyleSheet::Cascade(matches, inherited,
                                       part.Size() == 0 ? widget.GetInlineStyle() : core::Span<const InlineStyleDeclaration>{});
        }

        // Expands the sheets' @import through the resource provider. Each imported sheet (with its
        // own imports first) goes before the sheet that imported it, and an import chain that
        // comes back to a sheet already seen stops there.
        void ExpandImports()
        {
            m_importsExpanded = true;
            ExpandSheetImports();
            // Sprites: a url() naming one, in either sheet, is replaced by its image and region.
            Array<Spritesheet> sprites;
            for (const Spritesheet& s : m_userAgent.Spritesheets())
                sprites.PushBack(s);
            for (const Spritesheet& s : m_sheet.Spritesheets())
                sprites.PushBack(s);
            if (sprites.IsEmpty())
                return;
            m_userAgent.ResolveSprites(sprites.AsSpan());
            m_sheet.ResolveSprites(sprites.AsSpan());
            m_cache = {};
            m_allDirty = true;
        }
        void ExpandSheetImports()
        {
            if (m_resources == nullptr || m_sheet.Imports().IsEmpty())
                return;
            Array<core::String> chain;
            chain.PushBack(core::String(m_sheet.Name()));
            ExpandImportsOf(m_sheet, chain);
            m_cache = {};
            m_allDirty = true;
            m_relationsKnown = false;
        }
        void ExpandImportsOf(StyleSheet& sheet, Array<core::String>& chain)
        {
            const Array<core::String> imports = sheet.Imports();
            for (usize i = imports.Size(); i-- > 0;) // the last first, so each goes in front of the rest
            {
                bool cycle = false;
                for (const core::String& seen : chain)
                    cycle = cycle || seen.AsView() == imports[i].AsView();
                core::String text;
                if (cycle || !m_resources->LoadText(imports[i].AsView(), text))
                    continue;
                StyleSheet imported = CSSParser::Parse(text.AsView(), imports[i].AsView());
                chain.PushBack(imports[i]);
                ExpandImportsOf(imported, chain);
                chain.PopBack();
                sheet.PrependImported(core::Move(imported));
            }
        }

        // Loads the sheets' @font-face through the host's loader, each under the name ChooseFace
        // looks for (the family, its weight's name, " Italic").
        void LoadFontFaces()
        {
            m_fontFacesLoaded = true;
            if (!m_fontFaceLoader)
                return;
            const StyleSheet* const sheets[] = {&m_userAgent, &m_sheet};
            for (const StyleSheet* sheet : sheets)
                for (const FontFaceRule& rule : sheet->FontFaces())
                {
                    core::String face = detail::WithSuffix(rule.Family.AsView(), detail::WeightSuffix(rule.Weight));
                    if (rule.Italic)
                        face.Append(core::StringView(u8" Italic"));
                    if (m_fontService != nullptr && m_fontService->HasFamily(face.AsView()))
                        continue;
                    (void)m_fontFaceLoader(face.AsView(), rule.Source.AsView());
                }
        }

        // Finds a @keyframes by name: in the innermost subtree sheet first, then the GUI's sheet,
        // then the user agent's.
        [[nodiscard]] const Keyframes* FindKeyframes(const Array<const StyleSheet*>& subtrees, core::StringView name) const
        {
            for (usize i = subtrees.Size(); i > 0; --i)
                if (const Keyframes* k = subtrees[i - 1]->FindKeyframes(name))
                    return k;
            if (const Keyframes* k = m_sheet.FindKeyframes(name))
                return k;
            return m_userAgent.FindKeyframes(name);
        }

        // Starts a transition for each property that changed between `previous` and `resolved`
        // and that an entry covers (by its own name, or by `all` for transitionable properties;
        // the last entry naming a property wins), from the value shown now toward the new one.
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
                StyleValue initial;
                if (before == nullptr)
                {
                    // The property is set now but was set by no rule before (a toast's
                    // `:leaving { opacity: 0 }` over a rule that never set opacity). It transitions
                    // from its initial value, which was CSS's computed value. An inherited property
                    // took its parent's value, so it is skipped here.
                    const StylePropertySpec* property = FindStyleProperty(name);
                    if (property == nullptr || property->Inherited)
                        continue;
                    const core::StringView text = property->Initial != nullptr ? core::StringView(property->Initial)
                                                  : property->Grammar == StyleGrammar::Transforms ? core::StringView(u8"none")
                                                                                                  : core::StringView{};
                    if (text.Size() == 0 || text == p.Value.AsView() || !ParseStyleValue(property->Grammar, text, initial))
                        continue; // no initial value, or no change from it
                    before = &initial;
                }
                else if (previous.Get(name) == p.Value.AsView())
                    continue; // no change
                StyleValue from = *before;
                if (animation)
                    (void)animation->TrackValue(name, from); // retarget from a running track's current value
                if (ensure())
                    animation->StartTransition(name, from, p.Typed, *spec);
            }
            // A property no rule sets any more (a class was removed, such as a toast's `.entering`)
            // goes back to its initial value with a transition, as in CSS, instead of snapping: it
            // animates toward the table's initial value (`none` for a transform). An inherited
            // property takes its parent's value, so it is skipped here.
            for (const ResolvedProperty& p : previous.Properties())
            {
                const core::StringView name = p.Name.AsView();
                if (ResolvedStyle::IsCustomProperty(name) || p.Typed.Kind == StyleValueKind::None || resolved.Value(name) != nullptr)
                    continue;
                const StylePropertySpec* property = FindStyleProperty(name);
                if (property == nullptr || property->Inherited)
                    continue;
                const core::StringView initial = property->Initial != nullptr          ? core::StringView(property->Initial)
                                                 : property->Grammar == StyleGrammar::Transforms ? core::StringView(u8"none")
                                                                                                 : core::StringView{};
                StyleValue to;
                if (initial.Size() == 0 || !ParseStyleValue(property->Grammar, initial, to))
                    continue;
                const TransitionSpec* spec = nullptr;
                for (const TransitionSpec& t : transitions)
                    if (t.Property.AsView() == name || (t.Property.AsView() == core::StringView(u8"all") && IsTransitionable(name)))
                        spec = &t;
                if (spec == nullptr || spec->Duration <= 0.0f)
                    continue;
                StyleValue from = p.Typed;
                if (animation)
                    (void)animation->TrackValue(name, from);
                if (ensure())
                    animation->StartTransition(name, from, to, *spec);
            }
        }

        // Whether a value reads the viewport's size: a number in vw or vh (`50vw`,
        // `calc(100vh - 2px)`).
        [[nodiscard]] static bool ReadsViewport(core::StringView value) noexcept
        {
            for (usize i = 1; i + 1 < value.Size(); ++i)
                if (value[i] == u8'v' && (value[i + 1] == u8'w' || value[i + 1] == u8'h') &&
                    ((value[i - 1] >= u8'0' && value[i - 1] <= u8'9') || value[i - 1] == u8'.'))
                    return true;
            return false;
        }
        // Marks the widgets whose inline style reads the viewport, so they restyle after a resize
        // (used when no sheet reads the viewport).
        void MarkViewportReaders(Node& node)
        {
            if (const UINode* ui = Cast<UINode>(&node))
                for (const InlineStyleDeclaration& d : ui->GetInlineStyle())
                    if (ReadsViewport(d.Value.AsView()))
                    {
                        node.MarkStyleDirty(false);
                        break;
                    }
            for (usize i = 0; i < node.ChildCount(); ++i)
                MarkViewportReaders(*node.GetChildAt(i));
        }

        // Learns which states the sheets read through an ancestor and through a sibling.
        void LearnRelations()
        {
            m_ancestorStates = 0;
            m_siblingStates = 0;
            m_readsSiblings = false;
            m_siblingReadsAbove.Clear();
            m_ancestorStateReads.Clear();
            m_unplacedAncestorStates = 0;
            m_sheetsReadViewport = false;
            m_sizeQueries.Clear();
            const auto learn = [&](const StyleSheet& sheet)
            {
                for (const StyleRule& rule : sheet.Rules())
                {
                    if (rule.Media().FeatureCount() != 0)
                        m_sizeQueries.PushBack(&rule.Media()); // any feature counts, since it may include size
                    for (const StyleProperty& p : rule.Properties())
                        m_sheetsReadViewport = m_sheetsReadViewport || ReadsViewport(p.Value.AsView());
                    rule.Selector().CollectRelationStates(m_ancestorStates, m_siblingStates);
                    m_readsSiblings = m_readsSiblings || rule.Selector().ReadsSiblings();
                    rule.Selector().ForEachSiblingReadAbove([&](const StyleSelectorRule& compound) { m_siblingReadsAbove.PushBack(compound); });
                    rule.Selector().ForEachStateReadAbove([&](const StyleSelectorRule& compound, u32 states)
                                                          { m_ancestorStateReads.PushBack(AncestorStateRead{compound, states}); },
                                                          m_unplacedAncestorStates);
                }
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
        Function<bool(core::StringView, core::StringView)> m_fontFaceLoader; // owned by the host; loads @font-face
        bool m_fontFacesLoaded = false; // the current sheets' faces have been requested
        bool m_importsExpanded = false; // the current sheets' @imports have been expanded
        f32 m_rootFontSize = 16.0f;                   // CSS `rem` base
        // Which widget owns the entries at an address: the widget's slot in its GUI's element
        // table when the entries were made. The table is held, so after its GUI closes the slot
        // reads as gone rather than pointing at freed memory.
        struct Owner
        {
            RefPtr<ElementTable> Table;
            u32 Slot = 0;
            u32 Generation = 0;
        };
        // Whether the entries at `widget`'s address belong to it: either there are none yet, or
        // they were made for it (a widget in no GUI has nothing to check against, and keeps them).
        [[nodiscard]] bool IsOwn(const Node& widget) const
        {
            const Owner* owner = m_owners.Find(const_cast<Node*>(&widget));
            return owner == nullptr || !owner->Table || owner->Table->Resolve(owner->Slot, owner->Generation) == &widget;
        }
        // Makes the entries at `widget`'s address its own, first dropping any that belonged to a
        // destroyed widget.
        void Claim(Node& widget)
        {
            if (!IsOwn(widget))
                Forget(&widget);
            if (m_owners.Find(&widget) != nullptr)
                return;
            const Node::ElementRef ref = widget.TakeElementRef();
            m_owners.InsertOrAssign(&widget, Owner{RefPtr<ElementTable>(ref.table), ref.slot, ref.generation});
        }
        // Drops the entries of every widget destroyed since the last sweep, once the caches have
        // doubled in size since then (so a GUI that keeps its widgets pays nothing).
        void SweepGone()
        {
            Array<Node*> gone;
            for (const auto& entry : m_owners)
                if (entry.value.Table && entry.value.Table->Resolve(entry.value.Slot, entry.value.Generation) != entry.key)
                    gone.PushBack(entry.key);
            for (Node* node : gone)
                Forget(node);
            m_sweepAt = Max<usize>(kFirstSweep, m_owners.Size() * 2);
        }
        static constexpr usize kFirstSweep = 256;

        HashMap<Node*, Owner> m_owners; // which widget owns the entries at each address
        usize m_sweepAt = kFirstSweep;
        HashMap<Node*, ResolvedStyle> m_cache; // last-applied style per widget (non-owning keys)
        HashMap<Node*, RefPtr<StyleAnimation>> m_animations; // widget -> its running transitions and animations
        HashMap<Node*, Array<core::String>> m_startedAnimations; // widget -> the @keyframes it has started
        bool m_allDirty = true; // the next ApplyTree restyles everything (a new sheet, media, fonts)
        bool m_relationsKnown = false;
        bool m_sheetsReadViewport = false;     // some declaration uses vw or vh, so a resize restyles everything
        Array<const MediaQuery*> m_sizeQueries; // the rules' media queries with features, re-evaluated on a resize
        bool m_viewportResized = false;        // the viewport's size changed, and no sheet reads it
        u32 m_ancestorStates = 0; // states a rule reads through an ancestor; a change restyles the subtree
        u32 m_siblingStates = 0;  // states a rule reads through an earlier sibling; a change restyles the later siblings
        u32 m_lastRestyleCount = 0;
        bool m_readsSiblings = false; // some rule reads siblings, so a rearrangement restyles them
        Array<StyleSelectorRule> m_siblingReadsAbove; // compounds reading siblings, followed by a step below; a matching child's subtree restyles too
        // A compound whose states a rule reads through a descendant or child step, and which states
        // those are.
        struct AncestorStateRead
        {
            StyleSelectorRule Compound;
            u32 States = 0;
        };
        Array<AncestorStateRead> m_ancestorStateReads;
        u32 m_unplacedAncestorStates = 0; // read through an ancestor in a nested selector; a change on any widget restyles below it
        AncestorFilter m_ancestors; // the names of the ancestors of the widget ApplyTree's walk is at
        bool m_filtering = false;   // inside that walk (a widget styled any other way uses no filter)
    };
}
