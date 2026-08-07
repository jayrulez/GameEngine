// Draconic UI - :view partition (the mutually-recursive View cluster)
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
// `delete`. LayoutParams (Object) is likewise RefPtr-held.
//
// DEFERRED (need not-yet-ported subsystems; documented seams, not bugs):
//  - Input/Focus/DragDrop/Animation/Shortcut/Tooltip managers on UIContext (Input/Overlay/Animation
//    subsystems). Kept as nullable seams; IsHovered/IsFocused therefore return false for now.
//  - RootView's PopupLayer (Overlay subsystem) - omitted; RootView is a plain viewport ViewGroup.
//  - Input-event virtuals (OnMouseDown/OnKeyDown/...), directional-focus + tooltip fields, font-family
//    resolution via IFontService, ScrollIntoView (ScrollView), and UIContext::DrawRootView.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.ui:view;

import draconic.foundation; // Object, RefPtr, Array, HashMap, String, StringView, Float2, Rectangle, Function, Cast, IsDerivedFrom, TypeInfo, Max, Min, Optional
import draconic.vg;   // VGContext (child draw transforms)
import draconic.fonts; // IFontService (UIContext seam + font-family resolution)
import :enums;         // Visibility, CursorType, InvalidationKind
import :control_state;
import :property_owner; // IPropertyOwner
import :view_id;
import :view_transform;
import :thickness;
import :box_constraints;
import :size_spec;
import :unit;
import :layout_params;
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
import :input_manager;
import :focus_manager;
import :shortcut_manager;

using namespace draconic::foundation;
namespace fonts = draconic::fonts;
namespace vg = draconic::vg;

export namespace draconic::ui
{
    class View;
    class ViewGroup;
    class RootView;
    class UIContext;
    class
        PopupLayer; // defined in :popup_layer; RootView holds one (created lazily in the impl unit).

    using LayoutParamsPtr = RefPtr<LayoutParams>;
    // Aliases so the faithful field names `LayoutParams`/`Visibility` (which shadow their own types
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
    // View - base of the retained-mode view hierarchy.
    // ===================================================================================
    class View : public Object, public IPropertyOwner
    {
        DRACONIC_OBJECT(View, Object)
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
        LayoutParamsPtr LayoutParams;

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
        void Invalidate(); // defined below (touches Context)
        [[nodiscard]] bool NeedsRedraw() const noexcept { return m_needsRedraw; }
        void ClearRedrawFlag() noexcept { m_needsRedraw = false; }
        void OnPropertyChanged(InvalidationKind kind) override
        {
            (void)kind;
            Invalidate();
        }

        // === Layout ===
        void Measure(BoxConstraints constraints) { OnMeasure(constraints); }
        void Layout(f32 x, f32 y, f32 width, f32 height)
        {
            Bounds = Rectangle{x, y, width, height};
            OnLayout(x, y, width, height);
        }

        // === Virtual methods ===
        [[nodiscard]] virtual f32 GetBaseline() const { return -1.0f; }
        virtual void OnDraw(UIDrawContext& ctx) { (void)ctx; }

        /// Current visual state for drawable/theme lookups. Focused/Hover need input managers
        /// (deferred), so they contribute nothing yet.
        [[nodiscard]] virtual ControlState GetControlState() const
        {
            ControlState state = ControlState::Normal;
            if (!IsEffectivelyEnabled())
            {
                state |= ControlState::Disabled;
            }
            if (IsFocused())
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
            Invalidate();
        }
        void RemoveClass(StringView name)
        {
            for (usize i = 0; i < StyleClasses.Size(); ++i)
            {
                if (StyleClasses[i] == name)
                {
                    StyleClasses.RemoveAt(i);
                    Invalidate();
                    return;
                }
            }
        }
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
            Invalidate();
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
        [[nodiscard]] StyleValue ResolveStyle(StyleProperty prop);
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
        [[nodiscard]] f32 ResolveStyleFloat(StyleProperty prop, f32 defaultVal = 0.0f)
        {
            if (Optional<f32> f = ResolveStyle(prop).AsFloat(); f.HasValue())
            {
                return f.Value();
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
        [[nodiscard]] StringView ResolveStyleString(StyleProperty prop, StringView defaultVal = {})
        {
            if (Optional<StringView> s = ResolveStyle(prop).AsString(); s.HasValue())
            {
                return s.Value();
            }
            return defaultVal;
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
                m_inlineSheet = MakeRef<StyleSheet>(DefaultAllocator());
            }
            return *m_inlineSheet;
        }

        bool m_needsRedraw = true;
        RefPtr<StyleSheet> m_inlineSheet;
        RefPtr<StyleSheet> m_localStyleSheet;
        HashMap<String, void*> m_userData;
    };

    // ===================================================================================
    // ViewGroup - container of child views.
    // ===================================================================================
    class ViewGroup : public View
    {
        DRACONIC_OBJECT(ViewGroup, View)
    public:
        Thickness Padding{};

        ViewGroup() = default;

        [[nodiscard]] usize ChildCount() const noexcept { return m_children.Size(); }
        [[nodiscard]] View* GetChildAt(usize index) const { return m_children[index].Get(); }
        [[nodiscard]] virtual usize VisualChildCount() const { return m_children.Size(); }
        [[nodiscard]] virtual View* GetVisualChild(usize index) const
        {
            return (index < m_children.Size()) ? m_children[index].Get() : nullptr;
        }

        /// Adds a child with optional layout params. Defined below (uses UIContext::AttachView).
        virtual ViewGroup* AddView(View* child, LayoutParamsPtr lp = {});
        /// Removes a child (dropping the tree's ref). Defined below (uses UIContext::DetachView).
        void RemoveView(View* child, bool deleteChild = false);
        void RemoveAllViews(bool deleteChildren = false);
        void InsertView(View* child, usize index, LayoutParamsPtr lp = {});
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

        /// Find a descendant view by name (recursive).
        [[nodiscard]] View* FindByName(StringView name) const
        {
            for (usize i = 0; i < ChildCount(); ++i)
            {
                View* child = GetChildAt(i);
                if (child->Name.Size() > 0 && child->Name.AsView() == name)
                {
                    return child;
                }
                if (ViewGroup* childGroup = Cast<ViewGroup>(child))
                {
                    if (View* found = childGroup->FindByName(name))
                    {
                        return found;
                    }
                }
            }
            return nullptr;
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

            const usize count = VisualChildCount();
            for (usize i = count; i-- > 0;)
            {
                View* child = GetVisualChild(i);
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
        virtual LayoutParamsPtr CreateDefaultLayoutParams()
        {
            return MakeRef<::draconic::ui::LayoutParams>(DefaultAllocator());
        }

        /// Build child constraints from parent constraints and the child's LayoutParams SizeSpec.
        static BoxConstraints MakeChildConstraints(BoxConstraints parent, View* child,
                                                   f32 usedW = 0.0f, f32 usedH = 0.0f)
        {
            const LayoutParamsPtr& lp = child->LayoutParams;
            const Thickness margin = lp ? lp->Margin : Thickness{};
            RootView* root = child->Root();
            const f32 dpiScale = RootDpiScale(root);

            const f32 availW = Max(0.0f, parent.MaxWidth - usedW - margin.TotalHorizontal());
            const f32 availH = Max(0.0f, parent.MaxHeight - usedH - margin.TotalVertical());

            const SizeSpec widthSpec = lp ? lp->Width : SizeSpec::Wrap();
            const SizeSpec heightSpec = lp ? lp->Height : SizeSpec::Wrap();

            f32 minW = 0, maxW = 0, minH = 0, maxH = 0;
            switch (widthSpec.kind)
            {
            case SizeSpec::Kind::Fixed:
            {
                const f32 w = widthSpec.ResolveFixed(dpiScale);
                minW = w;
                maxW = w;
                break;
            }
            case SizeSpec::Kind::Match:
                minW = availW;
                maxW = availW;
                break;
            case SizeSpec::Kind::Wrap:
                minW = 0;
                maxW = availW;
                break;
            }
            switch (heightSpec.kind)
            {
            case SizeSpec::Kind::Fixed:
            {
                const f32 h = heightSpec.ResolveFixed(dpiScale);
                minH = h;
                maxH = h;
                break;
            }
            case SizeSpec::Kind::Match:
                minH = availH;
                maxH = availH;
                break;
            case SizeSpec::Kind::Wrap:
                minH = 0;
                maxH = availH;
                break;
            }
            return BoxConstraints{minW, maxW, minH, maxH};
        }

        void OnMeasure(BoxConstraints constraints) override
        {
            const BoxConstraints inner = constraints.Deflate(Padding);
            f32 maxW = 0, maxH = 0;
            for (const RefPtr<View>& child : m_children)
            {
                if (child->Visibility == VisibilityValue::Gone)
                {
                    continue;
                }
                child->Measure(inner);
                maxW = Max(maxW, child->MeasuredSize.x);
                maxH = Max(maxH, child->MeasuredSize.y);
            }
            MeasuredSize = Float2{constraints.ConstrainWidth(maxW + Padding.Left + Padding.Right),
                                  constraints.ConstrainHeight(maxH + Padding.Top + Padding.Bottom)};
        }

        void DrawChildren(UIDrawContext& ctx)
        {
            const usize count = VisualChildCount();
            for (usize i = 0; i < count; ++i)
            {
                View* child = GetVisualChild(i);
                if (child == nullptr || child->Visibility != VisibilityValue::Visible)
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
                if (child->ClipsContent)
                {
                    ctx.PushClip(Rectangle{0, 0, child->Width(), child->Height()});
                }

                child->OnDraw(ctx);

                if (ctx.DebugSettings().AnyEnabled())
                {
                    UIDebugOverlay::DrawOverlays(ctx, *child);
                }

                if (child->ClipsContent)
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
        DRACONIC_OBJECT(RootView, ViewGroup)
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

        /// Adds a child, keeping the PopupLayer as the last child for z-order.
        ViewGroup* AddView(View* child, LayoutParamsPtr lp = {}) override
        {
            if (child == nullptr)
            {
                return this;
            }
            if (child == m_popupLayer.Get())
            {
                return ViewGroup::AddView(child, Move(lp));
            } // the popup layer itself
            usize insertIndex = ChildCount();
            if (ChildCount() > 0 && GetChildAt(ChildCount() - 1) == m_popupLayer.Get())
            {
                insertIndex = ChildCount() - 1;
            }
            InsertView(child, insertIndex, Move(lp));
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
    // UIContext - central coordinator (managers deferred as nullable seams).
    // ===================================================================================
    class UIContext
    {
    public:
        enum class Phase
        {
            Idle,
            Layout,
            Drawing
        };

        UIContext()
            : m_inputManager(this), m_focusManager(this), m_shortcutManager(this),
              m_tooltipManager(this), m_dragDropManager(this)
        {
        }
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

        [[nodiscard]] Phase CurrentPhase() const noexcept { return m_phase; }
        [[nodiscard]] bool NeedsRedraw() const noexcept { return m_needsRedraw; }
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

        // Owned managers (Input/Focus/Shortcut/Tooltip). DragDrop/Animation stay deferred.
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
        void SetStyleSheet(RefPtr<StyleSheet> sheet) { m_styleSheet = Move(sheet); }

        // Clipboard adapter, set by the application / ui.shell bridge (non-owning, nullable). The core
        // stays platform-agnostic; EditText reads it through ITextEditHost.
        [[nodiscard]] IClipboard* Clipboard() const noexcept { return m_clipboard; }
        void SetClipboard(IClipboard* clipboard) noexcept { m_clipboard = clipboard; }

        // Font service, set by the application (non-owning, nullable). Controls resolve fonts through it
        // for measuring (Context->FontService()) and DrawRootView feeds it into the draw context + VG.
        [[nodiscard]] fonts::IFontService* FontService() const noexcept { return m_fontService; }
        void SetFontService(fonts::IFontService* fontService) noexcept
        {
            m_fontService = fontService;
        }

        /// Draw a root view's tree into `vg`. Builds a UIDrawContext over the (caller-supplied) VG and
        /// the font service, then walks the tree via ViewGroup::OnDraw. The caller constructs the VG with
        /// the same font service (VGContext takes it at construction; there is no setter). Ported faithfully
        /// from Sedulous UIContext.DrawRootView.
        void DrawRootView(RootView* root, vg::VGContext& vg)
        {
            if (root == nullptr)
            {
                return;
            }
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
        void BeginFrame(f32 deltaTime)
        {
            m_deltaTime = deltaTime;
            m_totalTime += deltaTime;
            m_mutationQueue.Drain();
            m_tooltipManager.Update(deltaTime);
            m_animationManager.Update(deltaTime);
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
            Unregister(view);
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
        InputManager m_inputManager;
        FocusManager m_focusManager;
        ShortcutManager m_shortcutManager;
        TooltipManager m_tooltipManager;
        DragDropManager m_dragDropManager;
        AnimationManager m_animationManager;
        RefPtr<StyleSheet> m_styleSheet;
        IClipboard* m_clipboard = nullptr;
        fonts::IFontService* m_fontService = nullptr;
        Phase m_phase = Phase::Idle;
        bool m_needsRedraw = true;
        f32 m_deltaTime = 0.0f;
        f32 m_totalTime = 0.0f;
    };

    DRACONIC_DEFINE_OBJECT(View, "draconic::ui")
    DRACONIC_DEFINE_OBJECT(ViewGroup, "draconic::ui")
    DRACONIC_DEFINE_OBJECT(RootView, "draconic::ui")

    // Out-of-line method definitions for the cluster (View::Invalidate/Root/ResolveStyle/...,
    // ViewGroup::AddView/RemoveView/..., MutationQueue::QueueDelete) live in the module implementation
    // unit Core/UIClusterImpl.cpp, together with StyleSelector::Matches / StyleSheet::Resolve. Keeping
    // bodies out of this interface partition slims its BMI (and the styling two must be there anyway to
    // avoid a :style_selector/:style_sheet -> :view module cycle).
}
