// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI - :view partition (the mutually-recursive View cluster)
//
// View / ViewGroup / RootView / UIContext / MutationQueue. Ported from Sedulous.UI/src/Core/*.bf.
// These classes reference one another cyclically (View<->ViewGroup<->UIContext<->MutationQueue) AND
// View<->StyleSheet, which C++ module partitions cannot express as a DAG - so the whole cluster lives
// in ONE partition, and the two styling methods that call into View (StyleSelector::Matches,
// StyleSheet::Resolve) are declared in their styling partitions and DEFINED here (after View is
// complete).
//
// OWNERSHIP: the view tree is RefPtr-owned (RAII). A ViewGroup keeps a RefPtr to each child;
// AddView adds a ref, RemoveView drops it. This replaces Beef's raw-pointer-owned tree + manual
// `delete`. Per-child placement is a LayoutStyle VALUE on the view (View::Layout / SetLayout).
//
// UNWIRED SEAMS (need not-yet-ported subsystems; documented seams, not bugs):
//  - Input/Focus/DragDrop/Animation/Shortcut/Tooltip managers on UIContext (Input/Overlay/Animation
//    subsystems). Kept as nullable seams; IsHovered/IsFocused therefore return false.
//  - RootView's PopupLayer (Overlay subsystem) - omitted; RootView is a plain viewport ViewGroup.
//  - Input-event virtuals (OnMouseDown/OnKeyDown/...), directional-focus + tooltip fields, font-family
//    resolution via IFontService, ScrollIntoView (ScrollView), and UIContext::DrawRootView.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.ui:view;

import foundation.core; // Object, RefPtr, Array, HashMap, String, StringView, Float2, Rectangle, Function, Cast, IsDerivedFrom, TypeInfo, Max, Min, Optional
import foundation.vg;   // VGContext (child draw transforms)
import foundation.fonts; // IFontService (UIContext seam + font-family resolution)
import :enums;         // Visibility, CursorType, InvalidationKind
import :control_state;
import :property_owner; // IPropertyOwner
import :view_id;
import :view_transform;
import :thickness;
import :box_constraints;
import :size_spec;
import :unit;
import :layout_style;
import :draw_context;     // UIDrawContext
import :ui_debug_overlay; // UIDebugOverlay::DrawOverlays (debug draw after each child)
import :drawable;
import :style_property;
import :style_value;
import :style_selector;
import :style_rule;
import :style_sheet;
import :event_args; // MouseEventArgs/KeyEventArgs/MouseWheelEventArgs/TextInputEventArgs
import :iaccelerator_handler;
import :itooltip_provider; // pattern-A tooltip content provider (View::AsTooltipProvider())
import :tooltip_placement; // TooltipPlacement enum (View.TooltipPlacement field)
import :tooltip_manager;   // by-value member of UIContext
import :idrag_source;      // pattern-A drag source (View::AsDragSource())
import :idrop_target;      // pattern-A drop target (View::AsDropTarget())
import :drag_drop_manager; // by-value member of UIContext
import :animation_manager; // by-value member of UIContext
import :iclipboard;        // clipboard seam (injected by the app; nullable)
import :iresource_provider; // image/text seam (injected by the app; nullable)
import :input_manager;
import :focus_manager;
import :shortcut_manager;

using namespace foundation::core;
namespace fonts = foundation::fonts;
namespace vg = foundation::vg;

export namespace foundation::ui
{
    class View;
    class ViewGroup;
    class RootView;
    class UIContext;
    class Dialog; // :dialog; named here for the context's DialogInterceptor
    class
        PopupLayer; // defined in :popup_layer; RootView holds one (created lazily in the impl unit).

    // Aliases so faithful field names such as `Visibility` (which shadow their own types
    // inside the class) can still name those types elsewhere in the cluster.
    using VisibilityValue = Visibility;
    using TooltipPlacementValue = TooltipPlacement;

    // ===================================================================================
    // MutationQueue - deferred tree changes drained at safe sync points.
    // ===================================================================================
    class MutationQueue
    {
    public:
        /// Enqueue an action to run at the next drain point.
        void QueueAction(Function<void()> action) { m_queue.PushBack(Move(action)); }

        /// Queue a view for deferred removal-from-parent at the next drain point. Defined below
        /// (needs View/ViewGroup complete).
        void QueueDelete(View* view);

        /// True if there are pending mutations.
        [[nodiscard]] bool HasPending() const noexcept { return m_queue.Size() > 0; }

        /// Execute all pending mutations. Actions may enqueue more - loops until empty.
        void Drain()
        {
            while (m_queue.Size() > 0)
            {
                Array<Function<void()>> batch = Move(m_queue);
                m_queue = Array<Function<void()>>{};
                for (Function<void()>& action : batch)
                {
                    action();
                }
            }
        }

    private:
        Array<Function<void()>> m_queue;
    };

    // ===================================================================================
    // BoxMetrics - the ONE resolved chrome of a view. Padding is the
    // component-wise max of the three historical channels (ViewGroup field, style property,
    // background DrawablePadding) so a padding declared through ANY of them takes effect.
    // Border is layout-participating chrome (border-box): MeasuredSize = content + padding +
    // border; margin stays the parent's aggregation concern.
    // ===================================================================================
    struct BoxMetrics
    {
        Thickness Margin{};
        Thickness Padding{};
        Thickness Border{};

        /// Padding + border - what deflates content constraints under border-box.
        [[nodiscard]] Thickness Chrome() const noexcept
        {
            return Thickness{Padding.Left + Border.Left, Padding.Top + Border.Top,
                             Padding.Right + Border.Right, Padding.Bottom + Border.Bottom};
        }
    };

    // ===================================================================================
    // View - base of the retained-mode view hierarchy.
    // ===================================================================================
    class View : public Object, public IPropertyOwner
    {
        RTTI_OBJECT(View, Object)
    public:
        // === Identity ===
        const ViewId Id = ViewId::Create();
        String Name{}; ///< Optional debug/lookup name (empty = none).
        Array<String> StyleClasses;

        // === Layout state ===
        Float2 MeasuredSize{};
        Rectangle Bounds{};
        [[nodiscard]] f32 Width() const noexcept { return Bounds.width; }
        [[nodiscard]] f32 Height() const noexcept { return Bounds.height; }

        /// This view's EFFECTIVE placement (size specs, margin, insets, and the per-container
        /// fields): every declared inline field, with the undeclared ones filled from the
        /// cascade (`width:`, `margin:`, `position:` ... in a sheet). Read by whichever
        /// container holds the view; the inline part survives reparenting unchanged. Refreshed
        /// by SetLayout and at the top of every Measure (for this view and its children).
        [[nodiscard]] const LayoutStyle& Layout() const noexcept { return m_effectiveLayout; }
        /// The inline placement intent alone (what SetLayout stored; undeclared fields read as
        /// their defaults).
        [[nodiscard]] const LayoutStyle& DeclaredLayout() const noexcept { return m_layout; }
        /// Replace the inline placement intent. Marks layout damage (geometry may change).
        void SetLayout(const LayoutStyle& layout)
        {
            if (m_layout == layout)
            {
                return;
            }
            m_layout = layout;
            RefreshEffectiveLayout();
            Invalidate();
        }
        /// Recompute the effective LayoutStyle from the inline value and the cascade. Impl unit.
        void RefreshEffectiveLayout();
        /// In the parent's flow: not Gone and not Position::Absolute. Containers lay out only
        /// in-flow children; the base ViewGroup places absolute ones against its content box.
        [[nodiscard]] static bool IsInFlow(const View* child) noexcept
        {
            return child->Visibility != VisibilityValue::Gone &&
                   child->Layout().Position.Value() != Position::Absolute;
        }
        /// Clips children to the border box: the ClipsContent field or `overflow: hidden`
        /// from the cascade (cached by RefreshEffectiveLayout).
        [[nodiscard]] bool EffectiveClipsContent() const noexcept
        {
            return ClipsContent || m_styledOverflowHidden;
        }

        // === Visibility & interaction ===
        VisibilityValue Visibility = VisibilityValue::Visible;
        bool IsEnabled = true;
        bool IsInteractionEnabled = true;
        bool IsHitTestVisible = true;
        bool IsFocusable = false;
        bool IsTabStop = false;
        i32 TabIndex = 0;
        bool ClipsContent = false;
        bool WantsArrowKeys = false;
        // Tab normally drives focus traversal BEFORE dispatch and never reaches a view. A view
        // that edits tab characters (code editors) sets this to receive Tab in OnKeyDown first;
        // traversal remains the fallback when the view leaves the event unhandled (the same
        // dispatch-first shape as the Return/OnActivate deviation in InputManager).
        bool WantsTabKey = false;
        bool IsPendingDeletion = false;

        // === Tooltip (shown by UIContext's TooltipManager) ===
        String
            TooltipText{}; ///< Plain-text tooltip (empty = none, unless AsTooltipProvider() is set).
        TooltipPlacementValue TooltipPlacement = TooltipPlacementValue::Bottom;
        bool IsTooltipInteractive = false; ///< Keep the tooltip hit-testable (hoverable/clickable).

        // === Directional focus overrides (empty = use the spatial picker) ===
        Optional<ViewId> NextFocusUp;
        Optional<ViewId> NextFocusDown;
        Optional<ViewId> NextFocusLeft;
        Optional<ViewId> NextFocusRight;

        // === Visual ===
        f32 Opacity = 1.0f;
        ViewTransform Transform{};
        CursorType Cursor = CursorType::Default;

        // === Tree (raw back-pointers; ownership is the parent's RefPtr) ===
        View* Parent = nullptr;
        UIContext* Context = nullptr;
        [[nodiscard]] bool IsAttached() const noexcept { return Context != nullptr; }
        /// The RootView this view belongs to (walks up the parent chain). Defined below (needs RootView).
        [[nodiscard]] RootView* Root() const;

        View() = default;

        // === Cursor ===
        /// Per-position cursor: defaults to the whole-view Cursor field. Views with internal
        /// regions wanting different pointers (a code editor's gutter vs its text area)
        /// override this instead of mutating Cursor from hover events.
        [[nodiscard]] virtual CursorType CursorAt(Float2 localPoint) const
        {
            (void)localPoint;
            return Cursor;
        }

        /// Walks the parent chain from the hit view, returning the first non-Default cursor
        /// (each view judged at the SAME screen point via CursorAt).
        [[nodiscard]] CursorType EffectiveCursor(Float2 screenPoint) const
        {
            const View* v = this;
            while (v != nullptr)
            {
                const CursorType cursor = v->CursorAt(v->ScreenToLocal(screenPoint));
                if (cursor != CursorType::Default)
                {
                    return cursor;
                }
                v = v->Parent;
            }
            return CursorType::Default;
        }

        // === User data (arbitrary void*; non-owning) ===
        void SetUserData(StringView key, void* data)
        {
            m_userData.InsertOrAssign(String(key), data);
        }
        [[nodiscard]] void* GetUserData(StringView key) const
        {
            if (void* const* v = m_userData.Find(String(key)))
            {
                return *v;
            }
            return nullptr;
        }
        template <typename T>
        [[nodiscard]] T* GetUserData(StringView key) const
        {
            return static_cast<T*>(GetUserData(key));
        }

        // === Coordinate conversion ===
        [[nodiscard]] Float2 LocalToScreen(Float2 local) const
        {
            Float2 result = local;
            const View* v = this;
            while (v != nullptr)
            {
                result.x += v->Bounds.x;
                result.y += v->Bounds.y;
                v = v->Parent;
            }
            return result;
        }
        [[nodiscard]] Float2 ScreenToLocal(Float2 screen) const
        {
            Float2 result = screen;
            const View* v = this;
            while (v != nullptr)
            {
                result.x -= v->Bounds.x;
                result.y -= v->Bounds.y;
                v = v->Parent;
            }
            return result;
        }

        // === Draw invalidation ===
        /// Layout-affecting change: the host re-measures + re-lays-out + redraws. The SAFE
        /// default for any mutation (yesterday's only spelling).
        void Invalidate(); // defined in the impl unit (touches Context)
        /// Visual-only change (hover tint, press state, focus ring, caret): redraw WITHOUT
        /// the whole-tree relayout. Use only where geometry provably cannot change.
        void InvalidateVisual();
        [[nodiscard]] bool NeedsRedraw() const noexcept { return m_needsRedraw; }
        void ClearRedrawFlag() noexcept { m_needsRedraw = false; }
        void OnPropertyChanged(InvalidationKind kind) override
        {
            // Properties DECLARE their damage: visual-only props (Opacity, TextColor, Cursor)
            // skip the relayout; everything else takes the safe layout path.
            if (kind == InvalidationKind::Visual)
            {
                InvalidateVisual();
            }
            else
            {
                Invalidate();
            }
        }

        // === Layout ===
        /// Measure this view. The BASE owns two things uniformly:
        /// MARGIN (deflated from the incoming constraints - parents do not) and the
        /// FIXED size spec (resolved HERE ONCE, in logical units - no Dp double-scale;
        /// Px means physical pixels). Match/Wrap remain parent-negotiated
        /// looseness: a base-side Match would defeat FlexLayout's deliberate cross-axis
        /// demotion. Controls implement OnMeasureContent (content-box in, content
        /// size out); plain OnMeasure overrides receive the post-margin post-spec box.
        void Measure(BoxConstraints c); // impl unit (needs RootView complete for the dpi query)

        /// How wide this view's content is when nothing narrows it, at `height`: what a container
        /// that scrolls sideways to show all of it (a list's rows) lays it out at. Default: its
        /// measure with an unbounded width. A control that measures to fill (an editable label)
        /// overrides it with what it draws.
        [[nodiscard]] virtual f32 NaturalWidth(f32 height)
        {
            Measure(BoxConstraints{0.0f, kFloatMax, height, height});
            return MeasuredSize.x;
        }

        /// MeasuredSize plus this view's margins - what parents aggregate and place (parents
        /// hand Layout the MARGIN box; the base insets to the border box).
        [[nodiscard]] Float2 MarginBoxSize() const
        {
            const Thickness m = m_effectiveLayout.Margin;
            return Float2{MeasuredSize.x + m.TotalHorizontal(),
                          MeasuredSize.y + m.TotalVertical()};
        }

        /// Arrange this view. `(x, y, width, height)` is the MARGIN BOX:
        /// the base insets by margin once, so every container honors margins identically without
        /// per-site margin arithmetic. Views without margins resolve to the same
        /// border box as a direct border-box call. The final border box is ROUNDED to the
        /// device grid - fractional centering does not land content on half-pixels
        /// (the blurry-1px-border/soft-text class). Body in the impl unit (dpi query).
        void Layout(f32 x, f32 y, f32 width, f32 height);

        // === Virtual methods ===
        [[nodiscard]] virtual f32 GetBaseline() const { return -1.0f; }
        virtual void OnDraw(UIDrawContext& ctx) { (void)ctx; }

        /// Current visual state for drawable/theme lookups. Focused/Hover need input managers
        /// (not yet wired), so they contribute nothing.
        [[nodiscard]] virtual ControlState GetControlState() const
        {
            ControlState state = ControlState::Normal;
            if (!IsEffectivelyEnabled())
            {
                state |= ControlState::Disabled;
            }
            // Focused STATE (and thus the themed focus visual) only for focus that should be
            // visible - keyboard-acquired, or a text-input view. Pointer-clicked controls hold
            // focus without lighting up (:focus-visible semantics; see FocusSource).
            if (IsFocusVisible())
            {
                state |= ControlState::Focused;
            }
            if (IsHovered())
            {
                state |= ControlState::Hover;
            }
            return state;
        }

        // === Input events (bubble phase - default) ===
        virtual void OnMouseDown(MouseEventArgs& e) { (void)e; }
        virtual void OnMouseUp(MouseEventArgs& e) { (void)e; }
        virtual void OnMouseMove(MouseEventArgs& e) { (void)e; }
        virtual void OnMouseWheel(MouseWheelEventArgs& e) { (void)e; }
        virtual void OnMouseEnter() {}
        virtual void OnMouseLeave() {}
        virtual void OnKeyDown(KeyEventArgs& e) { (void)e; }
        virtual void OnKeyUp(KeyEventArgs& e) { (void)e; }
        virtual void OnTextInput(TextInputEventArgs& e) { (void)e; }
        virtual void OnFocusGained() {}
        virtual void OnFocusLost() {}

        // === Input events (capture phase; root->target before the target sees it) ===
        virtual void OnMouseDownCapture(MouseEventArgs& e) { (void)e; }
        virtual void OnMouseUpCapture(MouseEventArgs& e) { (void)e; }
        virtual void OnMouseMoveCapture(MouseEventArgs& e) { (void)e; }
        virtual void OnMouseWheelCapture(MouseWheelEventArgs& e) { (void)e; }
        virtual void OnKeyDownCapture(KeyEventArgs& e) { (void)e; }
        virtual void OnKeyUpCapture(KeyEventArgs& e) { (void)e; }
        virtual void OnTextInputCapture(TextInputEventArgs& e) { (void)e; }

        // === Gamepad / directional activation ===
        /// Activated (Gamepad A / Enter on the focused view). ButtonBase overrides to fire OnClick.
        virtual void OnActivate() {}
        /// Cancel (Gamepad B / Escape). Default: bubbles to parent.
        virtual void OnCancel()
        {
            if (Parent != nullptr)
            {
                Parent->OnCancel();
            }
        }

        // === Capability query (tree-searched, As*() idiom; -fno-rtti-safe) ===
        /// IAcceleratorHandler this view implements, or null. Override to return `this`.
        [[nodiscard]] virtual IAcceleratorHandler* AsAcceleratorHandler() { return nullptr; }
        /// ITooltipProvider this view implements (custom tooltip content), or null. Override to return `this`.
        [[nodiscard]] virtual ITooltipProvider* AsTooltipProvider() { return nullptr; }
        /// IDragSource this view implements (draggable), or null. Override to return `this`.
        [[nodiscard]] virtual IDragSource* AsDragSource() { return nullptr; }
        /// IDropTarget this view implements (accepts drops), or null. Override to return `this`.
        [[nodiscard]] virtual IDropTarget* AsDropTarget() { return nullptr; }
        /// Whether a left press on this view starts a drag of its own (a scrollbar's thumb, a
        /// slider's), so no drag source above it may take the gesture: a scrollbar laid over a
        /// draggable tree's rows scrolls the tree, never drags a row.
        [[nodiscard]] virtual bool OwnsPressDrag() const { return false; }

        /// Whether this view wants platform text input (IME) while it holds focus. Text-editing controls
        /// override to return true. The ui.shell bridge reads UIContext::WantsTextInput() (this view, if
        /// focused) to start/stop the window's text input - the mechanism Sedulous shell never finished.
        [[nodiscard]] virtual bool WantsTextInput() const { return false; }

        // === Effective state ===
        [[nodiscard]] bool IsEffectivelyEnabled() const
        {
            const View* v = this;
            while (v != nullptr)
            {
                if (!v->IsEnabled)
                {
                    return false;
                }
                v = v->Parent;
            }
            return true;
        }
        [[nodiscard]] bool IsEffectivelyVisible() const; // defined below (checks RootView)

        // Input-manager backed (defined in the impl unit; query Context's Input/Focus managers).
        [[nodiscard]] bool IsHovered() const;
        [[nodiscard]] bool IsFocused() const;
        /// Whether this view's FOCUS VISUAL should draw: focused via keyboard, or a text-input
        /// view (which always shows its border + caret). Pointer/programmatic focus is held but
        /// not drawn. Draw sites use this; logic (typing, nav) keeps using IsFocused().
        [[nodiscard]] bool IsFocusVisible() const;
        /// True if this view or any descendant has keyboard focus.
        [[nodiscard]] bool IsFocusWithin() const;

        // === Style classes ===
        [[nodiscard]] bool HasClass(StringView name) const
        {
            for (const String& cls : StyleClasses)
            {
                if (cls == name)
                {
                    return true;
                }
            }
            return false;
        }
        void AddClass(StringView name)
        {
            if (HasClass(name))
            {
                return;
            }
            StyleClasses.PushBack(String(name));
            InvalidateStyle();
        }
        void RemoveClass(StringView name)
        {
            for (usize i = 0; i < StyleClasses.Size(); ++i)
            {
                if (StyleClasses[i] == name)
                {
                    StyleClasses.RemoveAt(i);
                    InvalidateStyle();
                    return;
                }
            }
        }
        /// Something that changes which rules match (a class, an id rename after attach, a
        /// sheet) happened: flush every computed-style cache in the context and relayout.
        /// Class/sheet setters call this; a `Name` write after attach must call it itself.
        void InvalidateStyle(); // impl unit (touches Context)
        /// As InvalidateStyle, for a change that REPLACES rule objects (a sheet swap): the
        /// context's sheet epoch moves so no stale cache is ever read for a transition
        /// snapshot (its rule pointers may be gone). Impl unit.
        void InvalidateStyleSheets();

        // === Transitions (P3; spec section 4) ===
        /// Running style transitions on this view (for tests/tools).
        [[nodiscard]] usize ActiveTransitionCount() const noexcept;
        [[nodiscard]] bool IsTransitioning() const noexcept;
        /// Advance every running transition by `deltaTime` and mark damage; returns whether
        /// any is still running. Called by UIContext::BeginFrame.
        bool AdvanceTransitions(f32 deltaTime);
        /// Drop every running transition (detach); the cascade value applies at once.
        void ClearTransitions();
        /// What DrawChildren hands the draw context before this view draws (see DrawBlend).
        [[nodiscard]] DrawBlend CurrentDrawBlend() const;
        void ToggleClass(StringView name)
        {
            if (HasClass(name))
            {
                RemoveClass(name);
            }
            else
            {
                AddClass(name);
            }
        }

        // === Inline / local stylesheets ===
        [[nodiscard]] StyleSheet* InlineSheet() const { return m_inlineSheet.Get(); }
        [[nodiscard]] bool HasAnyInlineStyles() const
        {
            return m_inlineSheet && !m_inlineSheet->IsEmpty();
        }
        [[nodiscard]] StyleSheet& GetOrCreateInlineSheet() { return EnsureInlineSheet(); }

        [[nodiscard]] StyleSheet* GetLocalStyleSheet() const { return m_localStyleSheet.Get(); }
        void SetLocalStyleSheet(RefPtr<StyleSheet> sheet)
        {
            if (m_localStyleSheet.Get() == sheet.Get())
            {
                return;
            }
            m_localStyleSheet = Move(sheet);
            InvalidateStyleSheets();
        }

        // Typed inline-style setters (map straight onto the inline element rule).
        void SetStyle(StyleProperty prop, Color color)
        {
            EnsureInlineSheet().GetOrCreateInlineElementRule().Set(prop, color);
            Invalidate();
        }
        void SetStyle(StyleProperty prop, f32 value)
        {
            EnsureInlineSheet().GetOrCreateInlineElementRule().Set(prop, value);
            Invalidate();
        }
        void SetStyle(StyleProperty prop, Thickness value)
        {
            EnsureInlineSheet().GetOrCreateInlineElementRule().Set(prop, value);
            Invalidate();
        }
        void SetStyle(StyleProperty prop, bool value)
        {
            EnsureInlineSheet().GetOrCreateInlineElementRule().Set(prop, value);
            Invalidate();
        }
        void SetStyle(StyleProperty prop, RefPtr<Drawable> drawable)
        {
            EnsureInlineSheet().GetOrCreateInlineElementRule().Set(prop, Move(drawable));
            Invalidate();
        }
        void SetStyle(StyleProperty prop, StringView value)
        {
            EnsureInlineSheet().GetOrCreateInlineElementRule().Set(prop, value);
            Invalidate();
        }
        void SetPartStyle(StringView part, StyleProperty prop, Color color)
        {
            EnsureInlineSheet().GetOrCreateInlinePartRule(part).Set(prop, color);
            Invalidate();
        }
        void SetPartStyle(StringView part, StyleProperty prop, f32 value)
        {
            EnsureInlineSheet().GetOrCreateInlinePartRule(part).Set(prop, value);
            Invalidate();
        }
        void SetPartStyle(StringView part, StyleProperty prop, RefPtr<Drawable> drawable)
        {
            EnsureInlineSheet().GetOrCreateInlinePartRule(part).Set(prop, Move(drawable));
            Invalidate();
        }

        /// Remove one inline override. No-op if not set.
        void ClearInlineStyle(StyleProperty prop)
        {
            if (!m_inlineSheet)
            {
                return;
            }
            if (StyleRule* rule = m_inlineSheet->FindInlineElementRule())
            {
                if (rule->Remove(prop))
                {
                    Invalidate();
                }
            }
        }
        /// Remove ALL inline overrides (element + part). Drops the whole inline sheet, releasing every rule
        /// and every drawable those rules held; a fresh sheet is reallocated on the next SetStyle.
        void ClearInlineStyles()
        {
            if (!m_inlineSheet || m_inlineSheet->IsEmpty())
            {
                return;
            }
            m_inlineSheet.Reset();
            Invalidate();
        }
        /// Inline override for `prop`, or None.
        [[nodiscard]] StyleValue GetInlineStyle(StyleProperty prop) const
        {
            if (!m_inlineSheet)
            {
                return StyleValue::None();
            }
            StyleRule* rule = m_inlineSheet->FindInlineElementRule();
            if (rule == nullptr)
            {
                return StyleValue::None();
            }
            if (Optional<StyleValue> v = rule->GetValue(prop); v.HasValue())
            {
                return v.Value();
            }
            return StyleValue::None();
        }
        [[nodiscard]] bool HasInlineStyle(StyleProperty prop) const
        {
            if (!m_inlineSheet)
            {
                return false;
            }
            StyleRule* rule = m_inlineSheet->FindInlineElementRule();
            return rule != nullptr && rule->GetValue(prop).HasValue();
        }

        // === Style resolution (orchestrators; defined below - need Context/StyleSheet/inheritance) ===
        /// The computed value of `prop` for this view: the ordered cascade over the context
        /// sheet, the local sheets from the outermost ancestor inward, and the inline sheet
        /// (later wins); `inherit`/`initial`/`var()` resolved; text properties inherit from
        /// the parent's computed value when unset. Cached per view (see RebuildStyleCache).
        [[nodiscard]] StyleValue ResolveStyle(StyleProperty prop);
        /// A custom property (`--name`, including the dashes) from the cascade, inherited
        /// through the parent chain like every custom property; None when unset. Also the
        /// custom-layout extension point: a container reads `child->CustomProperty(u8"--ring-angle")`.
        [[nodiscard]] StyleValue CustomProperty(StringView name);
        /// A Length-valued property resolved against `referenceSize` (the containing box on
        /// that axis) and this view's font size; a plain Float value passes through; else
        /// `defaultVal`.
        [[nodiscard]] f32 ResolveStyleLength(StyleProperty prop, f32 referenceSize,
                                             f32 defaultVal = 0.0f);
        [[nodiscard]] StyleValue ResolvePartStyle(StringView part, StyleProperty prop,
                                                  ControlState partState);

        /// Resolve the effective font family: the .FontFamily style cascade, falling back to the active
        /// font service's default family. Returns by value (our ResolveStyle returns a StyleValue by value,
        /// so a borrowed view would dangle). (Defined in the impl unit - needs Context.)
        [[nodiscard]] String ResolveStyleFontFamily();
        /// As above, but a non-empty per-instance override wins (controls with a typed FontFamily property).
        [[nodiscard]] String ResolveStyleFontFamily(StringView instanceOverride);

        [[nodiscard]] Color ResolveStyleColor(StyleProperty prop, Color defaultVal = Color::White)
        {
            if (Optional<Color> c = ResolveStyle(prop).AsColor(); c.HasValue())
            {
                return c.Value();
            }
            return defaultVal;
        }
        /// A Float property; a Length value (em/px/pt/dp, calc) resolves too, against this
        /// view's font size - so `font-size: 1.2em` or `corner-radius: 0.5em` in a theme reach
        /// every control that reads floats. Percent has no reference box on this path (0).
        [[nodiscard]] f32 ResolveStyleFloat(StyleProperty prop, f32 defaultVal = 0.0f)
        {
            const StyleValue value = ResolveStyle(prop);
            if (Optional<f32> f = value.AsFloat(); f.HasValue())
            {
                return f.Value();
            }
            if (value.GetKind() == StyleValue::Kind::Length)
            {
                return ResolveStyleLength(prop, 0.0f, defaultVal);
            }
            return defaultVal;
        }
        [[nodiscard]] Thickness ResolveStyleThickness(StyleProperty prop, Thickness defaultVal = {})
        {
            if (Optional<Thickness> t = ResolveStyle(prop).AsThickness(); t.HasValue())
            {
                return t.Value();
            }
            return defaultVal;
        }
        [[nodiscard]] Drawable* ResolveStyleDrawable(StyleProperty prop)
        {
            return ResolveStyle(prop).AsDrawable();
        }

        /// Resolve this view's chrome ONCE: margin from the LayoutStyle; padding
        /// = max of the ViewGroup Padding field (via OwnPaddingField), the styled padding (or
        /// the control's DefaultStylePadding when no style sets one), and the background
        /// drawable's DrawablePadding; border from StyleProperty::BorderWidth. The single
        /// source every measure/arrange/content-bounds consumer converges on.
        [[nodiscard]] BoxMetrics ResolveBoxMetrics()
        {
            BoxMetrics m;
            m.Margin = m_effectiveLayout.Margin;
            const Optional<Thickness> styled = ResolveStyle(StyleProperty::Padding).AsThickness();
            const Thickness stylePad = styled.HasValue() ? styled.Value() : DefaultStylePadding();
            Thickness drawablePad{};
            if (Drawable* bg = ResolveStyleDrawable(StyleProperty::Background))
            {
                drawablePad = bg->DrawablePadding();
            }
            const Thickness fieldPad = OwnPaddingField();
            m.Padding = Thickness{
                Max(Max(stylePad.Left, drawablePad.Left), fieldPad.Left),
                Max(Max(stylePad.Top, drawablePad.Top), fieldPad.Top),
                Max(Max(stylePad.Right, drawablePad.Right), fieldPad.Right),
                Max(Max(stylePad.Bottom, drawablePad.Bottom), fieldPad.Bottom)};
            const f32 borderWidth = ResolveStyleFloat(StyleProperty::BorderWidth, 0.0f);
            m.Border = Thickness{borderWidth, borderWidth, borderWidth, borderWidth};
            return m;
        }
        /// A String property, returned by VALUE: ResolveStyle hands back a StyleValue temporary,
        /// so a borrowed view would dangle (the ResolveStyleFontFamily lesson, caught by ASAN).
        [[nodiscard]] String ResolveStyleString(StyleProperty prop, StringView defaultVal = {})
        {
            const StyleValue value = ResolveStyle(prop);
            if (Optional<StringView> s = value.AsString(); s.HasValue())
            {
                return String(s.Value());
            }
            return String(defaultVal);
        }

        [[nodiscard]] Drawable* ResolvePartDrawable(StringView part, StyleProperty prop,
                                                    ControlState partState)
        {
            return ResolvePartStyle(part, prop, partState).AsDrawable();
        }
        [[nodiscard]] Color ResolvePartColor(StringView part, StyleProperty prop,
                                             ControlState partState,
                                             Color defaultVal = Color::White)
        {
            if (Optional<Color> c = ResolvePartStyle(part, prop, partState).AsColor(); c.HasValue())
            {
                return c.Value();
            }
            return defaultVal;
        }
        [[nodiscard]] f32 ResolvePartFloat(StringView part, StyleProperty prop,
                                           ControlState partState, f32 defaultVal = 0.0f)
        {
            if (Optional<f32> f = ResolvePartStyle(part, prop, partState).AsFloat(); f.HasValue())
            {
                return f.Value();
            }
            return defaultVal;
        }

        // === Content ===
        /// A view that draws one other view as its content without holding it as a child (a
        /// ContentButton, a ToggleButton) answers it here, so a search by name and markup reach it
        /// without naming the control types. None by default.
        [[nodiscard]] virtual View* ContentChild() const noexcept { return nullptr; }
        /// Takes `content` as that content; false for a view that has none to take.
        virtual bool SetContentChild(RefPtr<View> content)
        {
            (void)content;
            return false;
        }

        // === Hit testing ===
        [[nodiscard]] virtual View* HitTest(Float2 localPoint)
        {
            if (!IsInteractionEnabled || Visibility != VisibilityValue::Visible)
            {
                return nullptr;
            }
            if (localPoint.x < 0 || localPoint.y < 0 || localPoint.x >= Width() ||
                localPoint.y >= Height())
            {
                return nullptr;
            }
            if (!IsHitTestVisible)
            {
                return nullptr;
            }
            return this;
        }

        // === Deferred mutation convenience (defined below) ===
        void QueueRemove();
        void QueueDestroy();

    protected:
        /// Position::Absolute children hooks (ViewGroup implements; leaves have no children).
        /// Called by View::Measure after OnMeasure with the content box, and by View::Layout
        /// after OnLayout.
        virtual void MeasureAbsoluteChildren(f32 contentWidth, f32 contentHeight)
        {
            (void)contentWidth;
            (void)contentHeight;
        }
        virtual void LayoutAbsoluteChildren() {}
        /// Refresh every child's effective LayoutStyle (ViewGroup implements); called by
        /// View::Measure before OnMeasure, since containers read child->Layout() first.
        virtual void RefreshChildEffectiveLayouts() {}

        /// The container-field padding channel merged by ResolveBoxMetrics (ViewGroup overrides
        /// with its Padding field; leaf views have none).
        [[nodiscard]] virtual Thickness OwnPaddingField() const { return Thickness{}; }

        /// The control's FALLBACK padding when no style sets StyleProperty::Padding - stated
        /// once per control. A themed padding wins outright (fallback, not max-merge).
        [[nodiscard]] virtual Thickness DefaultStylePadding() const { return Thickness{}; }

        /// Content-box measure seam: receives CONTENT-box constraints (margin,
        /// spec, padding, and border already deflated by the base Measure) and returns the
        /// content size; the base re-inflates by chrome and clamps. Return the {-1,-1} sentinel
        /// (the default) to fall back to the OnMeasure seam. Controls that implement this seam
        /// carry no hand-rolled padding math.
        [[nodiscard]] virtual Float2 OnMeasureContent(BoxConstraints contentConstraints)
        {
            (void)contentConstraints;
            return Float2{-1.0f, -1.0f};
        }

        /// LEGACY measure seam - the control handles its own chrome. Receives the post-margin,
        /// post-Fixed-spec box. Deleted when the last override migrates to OnMeasureContent.
        virtual void OnMeasure(BoxConstraints constraints)
        {
            MeasuredSize =
                Float2{constraints.ConstrainWidth(0.0f), constraints.ConstrainHeight(0.0f)};
        }
        virtual void OnLayout(f32 left, f32 top, f32 width, f32 height)
        {
            (void)left;
            (void)top;
            (void)width;
            (void)height;
        }

        StyleSheet& EnsureInlineSheet()
        {
            if (!m_inlineSheet)
            {
                m_inlineSheet = MakeRef<StyleSheet>(MemoryAllocator());
                InvalidateStyle();
            }
            return *m_inlineSheet;
        }

        // The computed-style cache: the matching rules in ascending cascade order and, per
        // property, the winning rule. Valid while the context's style generation, the summed
        // versions of the sheets in THIS view's chain (context sheet, ancestors' local sheets,
        // own inline sheet) and the control state are unchanged. Sheets in other contexts, or
        // on other branches, never invalidate it. Impl-unit methods.
        struct StyleCache
        {
            bool valid = false;
            u32 generation = 0;
            u32 chainVersion = 0;
            u32 sheetEpoch = 0;
            ControlState state = ControlState::Normal;
            Array<const StyleRule*> rules;
            const StyleRule* winners[static_cast<usize>(StyleProperty::COUNT)] = {};
        };
        /// Ensures the cache matches the current state; returns it.
        [[nodiscard]] const StyleCache& EnsureStyleCache();
        /// The cascade's raw winning value for `prop` (no keyword/variable resolution), or None.
        [[nodiscard]] StyleValue RawStyleValue(StyleProperty prop);
        /// Turns Inherit/Initial/Variable into a concrete value (depth-limited for var chains).
        [[nodiscard]] StyleValue ResolveKeywords(StyleProperty prop, const StyleValue& raw, i32 depth);
        [[nodiscard]] StyleValue ResolveVariableValue(const StyleValue& reference, i32 depth);
        /// The cascade's value with keywords/inheritance resolved and NO transition overlay:
        /// what ResolveStyle returns once every transition on `prop` has finished.
        [[nodiscard]] StyleValue ComputeStyle(StyleProperty prop);
        /// After a cache rebuild: for every listed animatable property whose WINNING RULE
        /// changed (`oldWinners` vs the new cache), start or retarget a transition from the
        /// value before the rebuild (a running entry's current value, mid-flight included) to
        /// the new computed value; and the state cross-fade when the control state moved.
        /// Impl unit.
        void BeginTransitions(const StyleRule* const* oldWinners, ControlState oldState,
                              ControlState newState);
        friend class UIContext; // detach de-lists transitions
        StyleCache m_styleCache;

        struct StyleTransition
        {
            StyleProperty Property = StyleProperty::COUNT;
            StyleValue From;
            StyleValue To;
            f32 Elapsed = 0.0f;
            f32 Duration = 0.0f;
            f32 Delay = 0.0f;
            TransitionEasing Easing = TransitionEasing::Ease;
        };
        struct TransitionState
        {
            Array<StyleTransition> Active;
            bool StateBlendActive = false;
            ControlState BlendFrom = ControlState::Normal;
            ControlState BlendTo = ControlState::Normal;
            f32 BlendElapsed = 0.0f;
            f32 BlendDuration = 0.0f;
            f32 BlendDelay = 0.0f;
            TransitionEasing BlendEasing = TransitionEasing::Ease;
        };
        UniquePtr<TransitionState> m_transitionState; ///< null until the first transition
        bool m_transitionRegistered = false;          ///< listed in the context's tick set
        void RegisterTransitioning();                  // impl unit

        bool m_needsRedraw = true;
        LayoutStyle m_layout;          ///< inline (declared) intent
        LayoutStyle m_effectiveLayout; ///< inline + cascade (see Layout())
        bool m_styledOverflowHidden = false;
        RefPtr<StyleSheet> m_inlineSheet;
        RefPtr<StyleSheet> m_localStyleSheet;
        HashMap<String, void*> m_userData;
    };

    // ===================================================================================
    // ViewGroup - container of child views.
    // ===================================================================================
    class ViewGroup : public View
    {
        RTTI_OBJECT(ViewGroup, View)
    public:
        Thickness Padding{};

        ViewGroup() = default;

        // Defined in UIClusterImpl.cpp (needs the complete UIContext for DetachView): clears
        // child back-pointers and detaches the subtree from the context so nothing keeps a
        // pointer into a destroyed group.
        ~ViewGroup() override;

    protected:
        [[nodiscard]] Thickness OwnPaddingField() const override { return Padding; }

    public:

        [[nodiscard]] usize ChildCount() const noexcept { return m_children.Size(); }
        [[nodiscard]] View* GetChildAt(usize index) const { return m_children[index].Get(); }
        [[nodiscard]] virtual usize VisualChildCount() const { return m_children.Size(); }
        [[nodiscard]] virtual View* GetVisualChild(usize index) const
        {
            return (index < m_children.Size()) ? m_children[index].Get() : nullptr;
        }

        /// Adds a child, keeping its current LayoutStyle. Defined below (uses UIContext::AttachView).
        virtual ViewGroup* AddView(View* child);
        /// Adds a child and sets its LayoutStyle in one step.
        ViewGroup* AddView(View* child, const LayoutStyle& layout)
        {
            if (child != nullptr)
            {
                child->SetLayout(layout);
            }
            return AddView(child);
        }
        /// Removes a child (dropping the tree's ref). Defined below (uses UIContext::DetachView).
        void RemoveView(View* child, bool deleteChild = false);
        void RemoveAllViews(bool deleteChildren = false);
        void InsertView(View* child, usize index);
        void InsertView(View* child, usize index, const LayoutStyle& layout)
        {
            if (child != nullptr)
            {
                child->SetLayout(layout);
            }
            InsertView(child, index);
        }
        /// Moves an EXISTING child to `index` (clamped) - a pure reorder with NO
        /// Detach/Attach round-trip, so focus/hover/registration survive. For z-order
        /// maintenance (e.g. canvas order-stacking). Defined in the impl unit.
        void MoveView(View* child, usize index);

        [[nodiscard]] Rectangle ContentBounds() const
        {
            return Rectangle{Padding.Left, Padding.Top,
                             Max(0.0f, Width() - Padding.Left - Padding.Right),
                             Max(0.0f, Height() - Padding.Top - Padding.Bottom)};
        }

        /// Find a descendant view by name (recursive), into a child's content too (a
        /// ContentButton's labels).
        [[nodiscard]] View* FindByName(StringView name) const
        {
            for (usize i = 0; i < ChildCount(); ++i)
            {
                if (View* found = FindInSubtree(GetChildAt(i), name))
                {
                    return found;
                }
            }
            return nullptr;
        }
        /// `view` itself if it is named `name`, else the first such view under it (its children,
        /// then its content).
        [[nodiscard]] static View* FindInSubtree(View* view, StringView name)
        {
            if (view == nullptr)
            {
                return nullptr;
            }
            if (view->Name.Size() > 0 && view->Name.AsView() == name)
            {
                return view;
            }
            if (ViewGroup* group = Cast<ViewGroup>(view))
            {
                if (View* found = group->FindByName(name))
                {
                    return found;
                }
            }
            return FindInSubtree(view->ContentChild(), name);
        }
        template <typename T>
        [[nodiscard]] T* FindByName(StringView name) const
        {
            return Cast<T>(FindByName(name));
        }

        View* HitTest(Float2 localPoint) override
        {
            if (!IsInteractionEnabled || Visibility != VisibilityValue::Visible)
            {
                return nullptr;
            }
            if (localPoint.x < 0 || localPoint.y < 0 || localPoint.x >= Width() ||
                localPoint.y >= Height())
            {
                return nullptr;
            }

            // Front to back: the reverse of DrawChildren's order (z-index, then child order).
            View* zOrdered[kZOrderStackCapacity];
            Array<View*> zOrderedHeap;
            const Span<View*> order = OrderedVisualChildren(zOrdered, zOrderedHeap);
            const usize count = order.Size();
            for (usize i = count; i-- > 0;)
            {
                View* child = order[i];
                if (child == nullptr || child->Visibility != VisibilityValue::Visible ||
                    !child->IsInteractionEnabled)
                {
                    continue;
                }

                Float2 childLocal{localPoint.x - child->Bounds.x, localPoint.y - child->Bounds.y};
                // Apply the inverse ViewTransform so a transformed child hit-tests at its drawn position
                // (the draw path applies translate -> origin -> scale -> rotate -> -origin; undo in reverse).
                if (!child->Transform.IsIdentity())
                {
                    const f32 ox = child->Width() * child->Transform.Origin.x;
                    const f32 oy = child->Height() * child->Transform.Origin.y;
                    childLocal.x -= child->Transform.Translation.x;
                    childLocal.y -= child->Transform.Translation.y;
                    childLocal.x -= ox;
                    childLocal.y -= oy;
                    if (child->Transform.Rotation != 0.0f)
                    {
                        const f32 c = Cos(-child->Transform.Rotation);
                        const f32 s = Sin(-child->Transform.Rotation);
                        const f32 rx = childLocal.x * c - childLocal.y * s;
                        const f32 ry = childLocal.x * s + childLocal.y * c;
                        childLocal.x = rx;
                        childLocal.y = ry;
                    }
                    if (child->Transform.Scale.x != 0.0f && child->Transform.Scale.y != 0.0f)
                    {
                        childLocal.x /= child->Transform.Scale.x;
                        childLocal.y /= child->Transform.Scale.y;
                    }
                    childLocal.x += ox;
                    childLocal.y += oy;
                }
                if (View* hit = child->HitTest(childLocal))
                {
                    return hit;
                }
            }

            if (!IsHitTestVisible)
            {
                return nullptr;
            }
            return this;
        }

        void OnDraw(UIDrawContext& ctx) override { DrawChildren(ctx); }

    protected:
        /// Build child constraints from parent constraints and the child's LayoutStyle SizeSpec.
        /// Loose-vs-fill child availability: Fixed and margin are handled
        /// by the base Measure; the parent's only spec decision left is whether a Match
        /// child fills the available box.
        [[nodiscard]] static BoxConstraints AvailForChild(f32 availW, f32 availH, View* child)
        {
            const LayoutStyle& ls = child->Layout();
            const bool fillW = ls.Width->kind == SizeSpec::Kind::Match;
            const bool fillH = ls.Height->kind == SizeSpec::Kind::Match;
            return BoxConstraints{fillW ? availW : 0.0f, availW, fillH ? availH : 0.0f, availH};
        }

        void OnMeasure(BoxConstraints constraints) override
        {
            // Default container measure (FrameLayout-shaped): children measured loose within
            // the content box, aggregated by margin-box max. Chrome comes from the merged
            // metrics, so stylesheet padding/borders count on plain ViewGroups too.
            const Thickness chrome = ResolveBoxMetrics().Chrome();
            const BoxConstraints inner = constraints.Deflate(chrome);
            f32 maxW = 0, maxH = 0;
            for (const RefPtr<View>& child : m_children)
            {
                if (!IsInFlow(child.Get()))
                {
                    continue;
                }
                child->Measure(AvailForChild(inner.MaxWidth, inner.MaxHeight, child.Get()));
                const Float2 mb = child->MarginBoxSize();
                maxW = Max(maxW, mb.x);
                maxH = Max(maxH, mb.y);
            }
            MeasuredSize =
                Float2{constraints.ConstrainWidth(maxW + chrome.TotalHorizontal()),
                       constraints.ConstrainHeight(maxH + chrome.TotalVertical())};
        }

        void OnLayout(f32 left, f32 top, f32 width, f32 height) override
        {
            // Default container arrange: each child's margin box at the content origin (the
            // base ViewGroup must position children it measures, not leave a measure/arrange
            // asymmetry).
            (void)left;
            (void)top;
            (void)width;
            (void)height;
            const Thickness chrome = ResolveBoxMetrics().Chrome();
            for (const RefPtr<View>& child : m_children)
            {
                if (!IsInFlow(child.Get()))
                {
                    continue;
                }
                const Float2 mb = child->MarginBoxSize();
                child->Layout(chrome.Left, chrome.Top, mb.x, mb.y);
            }
        }

        void DrawChildren(UIDrawContext& ctx)
        {
            // Back to front: ascending z-index, child order within a z level.
            View* zOrdered[kZOrderStackCapacity];
            Array<View*> zOrderedHeap;
            const Span<View*> order = OrderedVisualChildren(zOrdered, zOrderedHeap);
            const usize count = order.Size();
            for (usize i = 0; i < count; ++i)
            {
                View* child = order[i];
                if (child == nullptr || child->Visibility != VisibilityValue::Visible)
                {
                    continue;
                }

                // Clip culling: under an active scissor (a ScrollView'd list, any ClipsContent
                // ancestor), a child whose bounds cannot intersect the clip contributes nothing -
                // skip its whole subtree so big scrolled content tessellates its VIEWPORT, not its
                // row count (the ImportTest blank-UI lesson: a ~1300-row generic form blew past
                // the VG per-frame vertex ceiling and blanked the window). Conservative: only
                // identity render transforms (a transform can move content into view), and only
                // under an active clip (unclipped layers keep out-of-bounds drawing freedom).
                if (child->Transform.IsIdentity() && !ctx.IsRectVisible(child->Bounds))
                {
                    continue;
                }

                ctx.VG().PushState();
                ctx.VG().Translate(child->Bounds.x, child->Bounds.y);

                if (!child->Transform.IsIdentity())
                {
                    const f32 ox = child->Width() * child->Transform.Origin.x;
                    const f32 oy = child->Height() * child->Transform.Origin.y;
                    if (child->Transform.Translation.x != 0 || child->Transform.Translation.y != 0)
                        ctx.VG().Translate(child->Transform.Translation.x,
                                           child->Transform.Translation.y);
                    if (child->Transform.Rotation != 0 || child->Transform.Scale.x != 1 ||
                        child->Transform.Scale.y != 1)
                    {
                        ctx.VG().Translate(ox, oy);
                        if (child->Transform.Scale.x != 1 || child->Transform.Scale.y != 1)
                            ctx.VG().Scale(child->Transform.Scale.x, child->Transform.Scale.y);
                        if (child->Transform.Rotation != 0)
                            ctx.VG().Rotate(child->Transform.Rotation);
                        ctx.VG().Translate(-ox, -oy);
                    }
                }

                if (child->Opacity < 1.0f)
                {
                    ctx.VG().PushOpacity(child->Opacity);
                }
                // The outer box-shadow sits UNDER the child's own drawing and outside its
                // clip; an inset shadow is drawn over the child (after OnDraw) inside it.
                Optional<BoxShadow> shadow = child->ResolveStyle(StyleProperty::BoxShadow).AsShadow();
                if (shadow.HasValue() && !shadow.Value().Inset)
                {
                    DrawBoxShadow(ctx, *child, shadow.Value());
                }
                const bool clips = child->EffectiveClipsContent();
                if (clips)
                {
                    ctx.PushClip(Rectangle{0, 0, child->Width(), child->Height()});
                }

                const DrawBlend previousBlend = ctx.SetBlend(child->CurrentDrawBlend());
                child->OnDraw(ctx);
                ctx.SetBlend(previousBlend);
                if (shadow.HasValue() && shadow.Value().Inset)
                {
                    DrawBoxShadow(ctx, *child, shadow.Value());
                }

                if (ctx.DebugSettings().AnyEnabled())
                {
                    UIDebugOverlay::DrawOverlays(ctx, *child);
                }

                if (clips)
                {
                    ctx.PopClip();
                }
                if (child->Opacity < 1.0f)
                {
                    ctx.VG().PopOpacity();
                }
                ctx.VG().PopState();
            }
        }

        /// One `box-shadow` for `child` (the VG is already translated to the child's origin):
        /// the border box grown by the spread, offset, blurred by the VG's DF shadow mode,
        /// rounded by the child's corner radius (plus the spread, as CSS does).
        static void DrawBoxShadow(UIDrawContext& ctx, View& child, const BoxShadow& shadow)
        {
            const f32 radius = Max(0.0f, child.ResolveStyleFloat(StyleProperty::CornerRadius, 0.0f));
            const f32 spread = shadow.Inset ? -shadow.Spread : shadow.Spread;
            const Rectangle rect{shadow.OffsetX - spread, shadow.OffsetY - spread,
                                 child.Width() + 2.0f * spread, child.Height() + 2.0f * spread};
            const f32 r = Max(0.0f, radius + spread);
            ctx.VG().FillBoxShadow(rect, vg::CornerRadii{r, r, r, r}, shadow.Blur, shadow.Color,
                                   shadow.Inset);
        }

        /// The visual children in draw order: ascending ZIndex, child order within a level
        /// (stable). Absent any non-zero z-index the child order is returned untouched.
        /// `stack` holds up to kZOrderStackCapacity entries; larger groups spill to `heap`.
        [[nodiscard]] Span<View*> OrderedVisualChildren(View** stack, Array<View*>& heap) const
        {
            const usize count = VisualChildCount();
            View** out = stack;
            if (count > kZOrderStackCapacity)
            {
                heap.Resize(count);
                out = heap.Data();
            }
            bool anyZ = false;
            for (usize i = 0; i < count; ++i)
            {
                out[i] = GetVisualChild(i);
                anyZ = anyZ || (out[i] != nullptr && out[i]->Layout().ZIndex.Value() != 0);
            }
            if (anyZ)
            {
                // Insertion sort: sibling counts are small and the order is nearly sorted.
                for (usize i = 1; i < count; ++i)
                {
                    View* v = out[i];
                    const i32 z = v != nullptr ? v->Layout().ZIndex.Value() : 0;
                    usize j = i;
                    while (j > 0 && (out[j - 1] != nullptr ? out[j - 1]->Layout().ZIndex.Value() : 0) > z)
                    {
                        out[j] = out[j - 1];
                        --j;
                    }
                    out[j] = v;
                }
            }
            return Span<View*>{out, count};
        }
        static constexpr usize kZOrderStackCapacity = 64;

        void RefreshChildEffectiveLayouts() override
        {
            for (const RefPtr<View>& child : m_children)
            {
                child->RefreshEffectiveLayout();
            }
        }

        /// Measure the Position::Absolute children (skipped by every container's OnMeasure)
        /// loosely within the content box; called by View::Measure after OnMeasure.
        void MeasureAbsoluteChildren(f32 contentWidth, f32 contentHeight) override
        {
            for (const RefPtr<View>& child : m_children)
            {
                if (child->Visibility == VisibilityValue::Gone ||
                    child->Layout().Position.Value() != Position::Absolute)
                {
                    continue;
                }
                const LayoutStyle& ls = child->Layout();
                // CSS shrink-to-fit: the available box is the content box minus the declared
                // insets on each axis; both insets declared pin BOTH edges (the margin box is
                // exactly the remainder).
                const f32 availW = Max(0.0f, contentWidth - (ls.Left.IsDeclared() ? ls.Left.Value() : 0.0f) -
                                                 (ls.Right.IsDeclared() ? ls.Right.Value() : 0.0f));
                const f32 availH = Max(0.0f, contentHeight - (ls.Top.IsDeclared() ? ls.Top.Value() : 0.0f) -
                                                 (ls.Bottom.IsDeclared() ? ls.Bottom.Value() : 0.0f));
                BoxConstraints c = AvailForChild(availW, availH, child.Get());
                if (ls.Left.IsDeclared() && ls.Right.IsDeclared())
                {
                    c.MinWidth = availW;
                    c.MaxWidth = availW;
                }
                if (ls.Top.IsDeclared() && ls.Bottom.IsDeclared())
                {
                    c.MinHeight = availH;
                    c.MaxHeight = availH;
                }
                child->Measure(c);
            }
        }

        /// Place the Position::Absolute children against the content box from their insets
        /// (Left/Top, or Right/Bottom anchoring the far edge; undeclared = 0 from the near
        /// edge); called by View::Layout after OnLayout.
        void LayoutAbsoluteChildren() override
        {
            const Thickness chrome = ResolveBoxMetrics().Chrome();
            const f32 contentWidth = Max(0.0f, Width() - chrome.TotalHorizontal());
            const f32 contentHeight = Max(0.0f, Height() - chrome.TotalVertical());
            for (const RefPtr<View>& child : m_children)
            {
                if (child->Visibility == VisibilityValue::Gone ||
                    child->Layout().Position.Value() != Position::Absolute)
                {
                    continue;
                }
                const LayoutStyle& ls = child->Layout();
                const Float2 mb = child->MarginBoxSize();
                f32 x = ls.Left.Value();
                f32 y = ls.Top.Value();
                if (!ls.Left.IsDeclared() && ls.Right.IsDeclared())
                {
                    x = contentWidth - ls.Right.Value() - mb.x;
                }
                if (!ls.Top.IsDeclared() && ls.Bottom.IsDeclared())
                {
                    y = contentHeight - ls.Bottom.Value() - mb.y;
                }
                child->Layout(chrome.Left + x, chrome.Top + y, mb.x, mb.y);
            }
        }

        Array<RefPtr<View>> m_children;

    private:
        [[nodiscard]] static f32 RootDpiScale(RootView* root); // defined below (RootView complete)
    };

    // ===================================================================================
    // RootView - top-level viewport view. Owns a PopupLayer kept as the last child (topmost for draw +
    // hit-test). The PopupLayer is created lazily in GetPopupLayer() (impl unit) - RootView lives in the
    // :view partition and cannot name the :popup_layer type at construction (module cycle).
    // ===================================================================================
    class RootView : public ViewGroup
    {
        RTTI_OBJECT(RootView, ViewGroup)
    public:
        Float2 ViewportSize{};
        f32 DpiScale = 1.0f;

        RootView() = default;

        [[nodiscard]] Float2 LogicalSize() const
        {
            const f32 dpi = Max(DpiScale, 0.01f);
            return Float2{ViewportSize.x / dpi, ViewportSize.y / dpi};
        }

        /// The per-window popup/overlay layer (created on first access).
        [[nodiscard]] PopupLayer*
        GetPopupLayer(); // defined in the impl unit (needs the complete type)

        /// The popup layer if it exists, WITHOUT creating it (base-typed - this partition cannot
        /// name PopupLayer; callers in impl units Cast). Used by focus-root scoping.
        [[nodiscard]] ViewGroup* PeekPopupLayer() const noexcept { return m_popupLayer.Get(); }

        /// Adds a child, keeping the PopupLayer as the last child for z-order.
        using ViewGroup::AddView; // keep the (child, LayoutStyle) overload visible
        ViewGroup* AddView(View* child) override
        {
            if (child == nullptr)
            {
                return this;
            }
            if (child == m_popupLayer.Get())
            {
                return ViewGroup::AddView(child);
            } // the popup layer itself
            usize insertIndex = ChildCount();
            if (ChildCount() > 0 && GetChildAt(ChildCount() - 1) == m_popupLayer.Get())
            {
                insertIndex = ChildCount() - 1;
            }
            InsertView(child, insertIndex);
            return this;
        }

    protected:
        void OnMeasure(BoxConstraints constraints) override
        {
            (void)constraints;
            const f32 dpi = Max(DpiScale, 0.01f);
            const f32 logicalW = ViewportSize.x / dpi;
            const f32 logicalH = ViewportSize.y / dpi;
            MeasuredSize = Float2{logicalW, logicalH};

            const BoxConstraints childConstraints = BoxConstraints::Tight(logicalW, logicalH);
            for (usize i = 0; i < ChildCount(); ++i)
            {
                View* child = GetChildAt(i);
                if (child->Visibility != VisibilityValue::Gone)
                {
                    child->Measure(childConstraints);
                }
            }
        }
        void OnLayout(f32 left, f32 top, f32 width, f32 height) override
        {
            (void)left;
            (void)top;
            for (usize i = 0; i < ChildCount(); ++i)
            {
                View* child = GetChildAt(i);
                if (child->Visibility != VisibilityValue::Gone)
                {
                    child->Layout(0, 0, width, height);
                }
            }
        }

    private:
        // Held as the base ViewGroup type (the :popup_layer type is incomplete in this partition); the
        // concrete PopupLayer is created and returned by GetPopupLayer() in the impl unit.
        RefPtr<ViewGroup> m_popupLayer;
    };

    // ===================================================================================
    // UIContext - central coordinator (unwired managers held as nullable seams).
    // ===================================================================================
    class UIContext
    {
    public:
        enum class Phase
        {
            Idle,
            Layout,
            Drawing,
            Animating // ticking the AnimationManager; tree mutations from onComplete defer to the queue
        };

        // The allocator (required - the host decides) is the whole UI tree's root:
        // every view, drawable, and style object created under this context inherits
        // it (views via MemoryAllocator(), managers via Allocator()).
        explicit UIContext(IAllocator& allocator)
            : m_allocator(&allocator), m_inputManager(this), m_focusManager(this),
              m_shortcutManager(this, allocator), m_tooltipManager(this), m_dragDropManager(this)
        {
        }
        /// Runs at the start of every Dialog::Show; answering false keeps the dialog off the
        /// screen (it closes at once as cancelled). An unattended caller (an agent's tool call
        /// on the editor) installs one for the duration of its call and records what it
        /// suppressed; unset = every dialog shows.
        Function<bool(Dialog&)> DialogInterceptor;

        // (m_animationManager is default-constructed - it holds no back-pointer to the context.)
        ~UIContext()
        {
            m_mutationQueue.Drain();
            if (m_styleSheet)
            {
                m_styleSheet = nullptr;
            }
        }

        UIContext(const UIContext&) = delete;
        UIContext& operator=(const UIContext&) = delete;

        [[nodiscard]] IAllocator& Allocator() const noexcept { return *m_allocator; }

        [[nodiscard]] Phase CurrentPhase() const noexcept { return m_phase; }
        [[nodiscard]] bool NeedsRedraw() const noexcept { return m_needsRedraw; }
        /// Layout damage: some view's GEOMETRY may have changed since the last layout pass.
        /// Visual-only damage (hover/caret/press) leaves this clear, so the host can redraw
        /// without re-measuring the whole tree - the interaction-frame cost cut.
        [[nodiscard]] bool NeedsLayout() const noexcept { return m_needsLayout; }
        void ClearLayoutDamage() noexcept { m_needsLayout = false; }
        [[nodiscard]] f32 DeltaTime() const noexcept { return m_deltaTime; }
        [[nodiscard]] f32 TotalTime() const noexcept { return m_totalTime; }
        [[nodiscard]] usize RootViewCount() const noexcept { return m_rootViews.Size(); }
        [[nodiscard]] RootView* GetRootView(usize index) const { return m_rootViews[index]; }

        [[nodiscard]] RootView* ActiveInputRoot() const noexcept { return m_activeInputRoot; }
        void SetActiveInputRoot(RootView* root) noexcept { m_activeInputRoot = root; }

        /// DPI scale for the active input root.
        [[nodiscard]] f32 DpiScale() const noexcept
        {
            return m_activeInputRoot ? m_activeInputRoot->DpiScale : 1.0f;
        }

        [[nodiscard]] MutationQueue& MutationQueueRef() noexcept { return m_mutationQueue; }

        // Owned managers (Input/Focus/Shortcut/Tooltip). DragDrop/Animation are not yet wired.
        [[nodiscard]] InputManager* GetInputManager() noexcept { return &m_inputManager; }
        [[nodiscard]] const InputManager* GetInputManager() const noexcept
        {
            return &m_inputManager;
        }
        [[nodiscard]] FocusManager* GetFocusManager() noexcept { return &m_focusManager; }
        [[nodiscard]] const FocusManager* GetFocusManager() const noexcept
        {
            return &m_focusManager;
        }
        [[nodiscard]] ShortcutManager* GetShortcuts() noexcept { return &m_shortcutManager; }
        [[nodiscard]] TooltipManager* Tooltips() noexcept { return &m_tooltipManager; }
        [[nodiscard]] DragDropManager* DragDrop() noexcept { return &m_dragDropManager; }
        [[nodiscard]] AnimationManager* Animations() noexcept { return &m_animationManager; }

        /// Whether the currently focused view wants platform text input (IME). The ui.shell bridge uses
        /// this to enable/disable the window's text input as focus moves. Defined in the impl unit (needs
        /// FocusManager::FocusedView()).
        [[nodiscard]] bool WantsTextInput() const;

        [[nodiscard]] StyleSheet* GetStyleSheet() const noexcept { return m_styleSheet.Get(); }
        /// Install the context sheet (the theme; its motion included - see the shipped
        /// themes' `View { transition }` rule) and move the sheet epoch. Impl unit.
        void SetStyleSheet(RefPtr<StyleSheet> sheet);
        /// Bumped whenever rule OBJECTS may have been replaced (context or local sheet swap):
        /// a View's style cache from another epoch is rebuilt without reading it.
        [[nodiscard]] u32 SheetEpoch() const noexcept { return m_sheetEpoch; }
        void BumpSheetEpoch() noexcept
        {
            ++m_sheetEpoch;
            InvalidateStyles();
        }
        /// A view with a running transition asks to be ticked (BeginFrame); de-listed when its
        /// transitions end or it detaches.
        void RegisterTransitioning(View* view)
        {
            m_transitioning.PushBack(view);
        }
        void UnregisterTransitioning(View* view)
        {
            for (usize i = m_transitioning.Size(); i-- > 0;)
            {
                if (m_transitioning[i] == view)
                {
                    m_transitioning.RemoveAtSwap(i);
                }
            }
        }
        [[nodiscard]] usize TransitioningViewCount() const noexcept { return m_transitioning.Size(); }

        /// The style generation: bumped by anything that changes which rules match a view
        /// (classes, ids, sheets, tree structure). Every View's computed-style cache keys on it.
        [[nodiscard]] u32 StyleGeneration() const noexcept { return m_styleGeneration; }
        void InvalidateStyles() noexcept { ++m_styleGeneration; }

        // Clipboard adapter, set by the application / ui.shell bridge (non-owning, nullable). The core
        // stays platform-agnostic; EditText reads it through ITextEditHost.
        [[nodiscard]] IClipboard* Clipboard() const noexcept { return m_clipboard; }
        void SetClipboard(IClipboard* clipboard) noexcept { m_clipboard = clipboard; }

        // What resolves an image a view names by a string (an ImageView's Source), set by the
        // application (non-owning, nullable). The UI never interprets the string: the engine's
        // provider answers asset ids, a sample's answers file paths.
        [[nodiscard]] IResourceProvider* ResourceProvider() const noexcept
        {
            return m_resourceProvider;
        }
        void SetResourceProvider(IResourceProvider* provider) noexcept
        {
            m_resourceProvider = provider;
        }

        // Font service, set by the application (non-owning, nullable). Controls resolve fonts through it
        // for measuring (Context->FontService()) and DrawRootView feeds it into the draw context + VG.
        [[nodiscard]] fonts::IFontService* FontService() const noexcept { return m_fontService; }
        void SetFontService(fonts::IFontService* fontService) noexcept
        {
            m_fontService = fontService;
        }

        /// Draw a root view's tree into `vg`. Builds a UIDrawContext over the (caller-supplied) VG and
        /// the font service, then walks the tree via ViewGroup::OnDraw. The context's CURRENT font
        /// service is pushed into the VG here, every draw - construction-time agreement is not
        /// trusted, because a service swap after the VG was built (SetDefaultFont binding a cooked
        /// font) would leave the VG resolving atlases against the stale service: silently invisible
        /// text (the 2026-08-12 dist incident). Ported from Sedulous UIContext.DrawRootView.
        void DrawRootView(RootView* root, vg::VGContext& vg)
        {
            if (root == nullptr)
            {
                return;
            }
            vg.SetFontService(m_fontService); // the one source of truth, re-asserted per draw
            m_phase = Phase::Drawing;
            UIDrawContext ctx{vg, root->DpiScale, m_fontService};
            if (root->DpiScale != 1.0f)
            {
                vg.Scale(root->DpiScale, root->DpiScale);
            }
            root->OnDraw(ctx);
            m_phase = Phase::Idle;
            m_needsRedraw = false;
        }

        // === Root view management ===
        void AddRootView(RootView* root)
        {
            if (root == nullptr || Contains(m_rootViews, root))
            {
                return;
            }
            m_rootViews.PushBack(root);
            AttachView(root);
            if (m_activeInputRoot == nullptr)
            {
                m_activeInputRoot = root;
            }
        }
        void RemoveRootView(RootView* root)
        {
            if (root == nullptr)
            {
                return;
            }
            for (usize i = 0; i < m_rootViews.Size(); ++i)
            {
                if (m_rootViews[i] == root)
                {
                    DetachView(root);
                    m_rootViews.RemoveAt(i);
                    if (m_activeInputRoot == root)
                    {
                        m_activeInputRoot = m_rootViews.Size() > 0 ? m_rootViews[0] : nullptr;
                    }
                    return;
                }
            }
        }

        // === View registry ===
        void Register(View* view)
        {
            if (view != nullptr && view->Id.IsValid())
            {
                m_registry.InsertOrAssign(view->Id.RawValue(), view);
            }
        }
        void Unregister(View* view)
        {
            if (view != nullptr && view->Id.IsValid())
            {
                m_inputManager.OnViewDeleted(view);
                m_focusManager.OnViewDeleted(view);
                m_tooltipManager.OnViewDeleted(view);
                m_dragDropManager.OnViewDeleted(view);
                m_animationManager.CancelForView(view);
                m_shortcutManager.RemoveScopedTo(view);
                m_registry.Remove(view->Id.RawValue());
            }
        }
        [[nodiscard]] View* GetViewById(ViewId id) const
        {
            if (View* const* v = m_registry.Find(id.RawValue()))
            {
                return *v;
            }
            return nullptr;
        }
        template <typename T>
        [[nodiscard]] T* GetViewById(ViewId id) const
        {
            return Cast<T>(GetViewById(id));
        }

        // === Frame lifecycle ===
        void MarkNeedsRedraw() noexcept { m_needsRedraw = true; }
        void MarkNeedsLayout() noexcept
        {
            m_needsLayout = true;
            m_needsRedraw = true; // a relayout always redraws
        }
        void BeginFrame(f32 deltaTime)
        {
            m_deltaTime = deltaTime;
            m_totalTime += deltaTime;
            m_mutationQueue.Drain();
            m_tooltipManager.Update(deltaTime);
            // Style transitions: each listed view advances its own clocks and marks its damage
            // (visual or layout by the property's kind); done views leave the set.
            for (usize i = m_transitioning.Size(); i-- > 0;)
            {
                if (!m_transitioning[i]->AdvanceTransitions(deltaTime))
                {
                    m_transitioning.RemoveAtSwap(i);
                }
            }
            // Continuous-damage producers (the redraw gate has no free per-frame redraws):
            // live animations move things every tick, and a focused text input needs its
            // caret blink serviced. Both mark BEFORE the host samples the damage state.
            if (m_animationManager.ActiveCount() > 0 || WantsTextInput())
            {
                MarkNeedsRedraw();
            }
            // Animation onComplete callbacks may mutate the view tree (a screen transition that
            // removes its screen on finish). Tick under a NON-Idle phase so those structural changes
            // route through the mutation queue (drained next frame) instead of running inline -
            // an inline RemoveView -> AnimationManager::CancelForView would re-enter this very loop.
            m_phase = Phase::Animating;
            m_animationManager.Update(deltaTime);
            m_phase = Phase::Idle;
        }
        void UpdateRootView(RootView* root)
        {
            if (root == nullptr)
            {
                return;
            }
            m_phase = Phase::Layout;
            const f32 dpi = Max(root->DpiScale, 0.01f);
            const f32 logicalW = root->ViewportSize.x / dpi;
            const f32 logicalH = root->ViewportSize.y / dpi;
            const BoxConstraints constraints = BoxConstraints::Tight(logicalW, logicalH);
            root->Measure(constraints);
            root->Layout(0, 0, logicalW, logicalH);
            m_phase = Phase::Idle;
        }

        // === Subtree attach/detach ===
        void AttachView(View* view)
        {
            InvalidateStyles(); // ancestors, siblings (:first-child/:last-child) changed
            view->Context = this;
            Register(view);
            if (ViewGroup* group = Cast<ViewGroup>(view))
            {
                for (usize i = 0; i < group->ChildCount(); ++i)
                {
                    AttachView(group->GetChildAt(i));
                }
                for (usize i = 0; i < group->VisualChildCount(); ++i)
                {
                    View* vc = group->GetVisualChild(i);
                    if (vc != nullptr && vc->Context != this)
                    {
                        AttachView(vc);
                    }
                }
            }
        }
        void DetachView(View* view)
        {
            InvalidateStyles();
            Unregister(view);
            if (view->m_transitionRegistered)
            {
                UnregisterTransitioning(view);
            }
            view->ClearTransitions();
            view->Context = nullptr;
            if (ViewGroup* group = Cast<ViewGroup>(view))
            {
                for (usize i = 0; i < group->ChildCount(); ++i)
                {
                    DetachView(group->GetChildAt(i));
                }
                for (usize i = 0; i < group->VisualChildCount(); ++i)
                {
                    View* vc = group->GetVisualChild(i);
                    if (vc != nullptr && vc->Context != nullptr)
                    {
                        DetachView(vc);
                    }
                }
            }
        }

    private:
        template <typename T>
        [[nodiscard]] static bool Contains(const Array<T>& arr, const T& value)
        {
            for (const T& e : arr)
            {
                if (e == value)
                {
                    return true;
                }
            }
            return false;
        }

        HashMap<u32, View*> m_registry;
        Array<RootView*> m_rootViews; // non-owning
        RootView* m_activeInputRoot = nullptr;
        MutationQueue m_mutationQueue;
        IAllocator* m_allocator;
        InputManager m_inputManager;
        FocusManager m_focusManager;
        ShortcutManager m_shortcutManager;
        TooltipManager m_tooltipManager;
        DragDropManager m_dragDropManager;
        AnimationManager m_animationManager;
        Array<View*> m_transitioning; ///< views with running style transitions (raw: de-listed on detach)
        u32 m_sheetEpoch = 0;
        RefPtr<StyleSheet> m_styleSheet;
        u32 m_styleGeneration = 1;
        IClipboard* m_clipboard = nullptr;
        IResourceProvider* m_resourceProvider = nullptr;
        fonts::IFontService* m_fontService = nullptr;
        Phase m_phase = Phase::Idle;
        bool m_needsRedraw = true;
        bool m_needsLayout = true;
        f32 m_deltaTime = 0.0f;
        f32 m_totalTime = 0.0f;
    };

    RTTI_DEFINE_OBJECT(View, "rtti::ui")
    RTTI_DEFINE_OBJECT(ViewGroup, "rtti::ui")
    RTTI_DEFINE_OBJECT(RootView, "rtti::ui")

    // Out-of-line method definitions for the cluster (View::Invalidate/Root/ResolveStyle/...,
    // ViewGroup::AddView/RemoveView/..., MutationQueue::QueueDelete) live in the module implementation
    // unit Core/UIClusterImpl.cpp, together with StyleSelector::Matches / StyleSheet::Resolve. Keeping
    // bodies out of this interface partition slims its BMI (and the styling two must be there anyway to
    // avoid a :style_selector/:style_sheet -> :view module cycle).
}
