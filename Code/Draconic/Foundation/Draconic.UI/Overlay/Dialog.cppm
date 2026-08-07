// Draconic UI - :dialog partition
//
// Modal dialog with title, content, and a right-aligned button row. Shown via PopupLayer as a centered
// modal popup. Ported from Sedulous.UI/src/Overlay/Dialog.bf. Ownership: Beef `new FlexLayout` + manual
// delete -> RefPtr<FlexLayout> m_layout (VisualChild); the title Label / button-row FlexLayout / content
// View are owned by m_layout (AddView), held here as borrowed raw pointers. Beef get/set Property<float>
// -> Value()/SetValue(); `delegate void(Dialog, DialogResult)` OnClosed event -> Event<void(Dialog*,
// DialogResult)>. The dialog is one VisualChild (m_layout); measure/layout/draw delegate to it.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.ui:dialog;

import draconic.foundation;
import draconic.vg;
import :view;
import :property;
import :event;
import :thickness;
import :box_constraints;
import :unit;
import :size_spec;
import :draw_context;
import :drawable;
import :control_state;
import :style_property;
import :event_args;
import :input_enums;
import :popup_layer;
import :flex_layout;
import :label;
import :button_base;
import :button;

using namespace draconic::foundation;

export namespace draconic::ui
{
    enum class DialogResult
    {
        None,
        OK,
        Cancel
    };

    /// Modal dialog with title, content, and button row. Shown via PopupLayer as a centered modal popup.
    class Dialog : public ViewGroup
    {
        DRACONIC_OBJECT(Dialog, ViewGroup)
    public:
        String Title;
        DialogResult Result = DialogResult::None;
        Event<void(Dialog*, DialogResult)> OnClosed;

        /// Minimum dialog width.
        Property<f32> MinWidth{250.0f};
        /// Minimum dialog height.
        Property<f32> MinHeight{120.0f};
        /// Maximum dialog width. Clamped to 80% of viewport if larger.
        Property<f32> MaxWidth{400.0f};
        /// Maximum dialog height. Clamped to 80% of viewport if larger.
        Property<f32> MaxHeight{300.0f};

        explicit Dialog(StringView title)
        {
            ClipsContent = true;
            MinWidth.SetOwner(this);
            MinHeight.SetOwner(this);
            MaxWidth.SetOwner(this);
            MaxHeight.SetOwner(this);
            Title = String(title);

            m_layout = MakeRef<FlexLayout>(DefaultAllocator());
            m_layout->Direction = Orientation::Vertical;
            m_layout->Spacing = 10;
            m_layout->Padding = Thickness{12, 10};
            m_layout->Parent = this;

            // Title
            RefPtr<Label> titleLabel = MakeRef<Label>(DefaultAllocator(), title);
            m_titleLabel = titleLabel.Get();
            {
                RefPtr<FlexLayoutParams> lp = MakeRef<FlexLayoutParams>(DefaultAllocator());
                lp->Width = SizeSpec::Match();
                lp->Height = SizeSpec::Fixed(Unit::Px(24));
                m_layout->AddView(titleLabel.Get(), lp);
            }

            // Button row (right-aligned)
            m_buttonRow = MakeRef<FlexLayout>(DefaultAllocator());
            m_buttonRow->Direction = Orientation::Horizontal;
            m_buttonRow->Spacing = 8;
            m_buttonRow->JustifyContent = Justify::End;
            {
                RefPtr<FlexLayoutParams> lp = MakeRef<FlexLayoutParams>(DefaultAllocator());
                lp->Width = SizeSpec::Match();
                lp->Height = SizeSpec::Fixed(Unit::Px(36));
                m_layout->AddView(m_buttonRow.Get(), lp);
            }
        }

        /// Set the content view (between title and buttons).
        void SetContent(View* content)
        {
            if (m_content != nullptr)
            {
                m_layout->RemoveView(m_content, true);
            }

            m_content = content;
            m_layout->RemoveView(m_buttonRow.Get(), false);
            {
                RefPtr<FlexLayoutParams> lp = MakeRef<FlexLayoutParams>(DefaultAllocator());
                lp->Width = SizeSpec::Match();
                lp->Grow = 1;
                m_layout->AddView(content, lp);
            }
            {
                RefPtr<FlexLayoutParams> lp = MakeRef<FlexLayoutParams>(DefaultAllocator());
                lp->Width = SizeSpec::Match();
                lp->Height = SizeSpec::Fixed(Unit::Px(36));
                m_layout->AddView(m_buttonRow.Get(), lp);
            }
        }

        /// Add a button to the button row. A result of None makes the button CALLER-MANAGED:
        /// its click does NOT auto-close - wire OnClick and call Close() yourself, so a
        /// validation failure can keep the dialog up. Any other result closes with it.
        Button* AddButton(StringView text, DialogResult result)
        {
            RefPtr<Button> btn = MakeRef<Button>(DefaultAllocator(), text);
            if (result != DialogResult::None)
            {
                Dialog* self = this;
                const DialogResult dialogResult = result;
                btn->OnClick.Add(Event<void(ButtonBase*)>::Handler{[self, dialogResult](ButtonBase*)
                                                                   { self->Close(dialogResult); }});
            }
            Button* raw = btn.Get();
            m_buttonRow->AddView(btn.Get());
            return raw;
        }

        /// Show as a centered modal dialog.
        void Show(UIContext* ctx, bool ownsView = true)
        {
            RootView* root = ctx->ActiveInputRoot();
            if (root == nullptr)
            {
                return;
            }

            // Show at (0,0) first so dialog gets context-attached for measurement. AttachView recurses
            // through VisualChildren, so the internal FlexLayout and its buttons attach automatically.
            root->GetPopupLayer()->ShowPopup(this, nullptr, 0, 0, false, true, ownsView);

            // Now measure with context available, then reposition to center. Use logical coordinates
            // (physical / DpiScale) to match layout space.
            const f32 dpi = Max(root->DpiScale, 0.01f);
            const f32 viewportW = root->ViewportSize.x / dpi;
            const f32 viewportH = root->ViewportSize.y / dpi;
            const f32 maxW = Min(MaxWidth.Value(), viewportW * 0.8f);
            const f32 maxH = Min(MaxHeight.Value(), viewportH * 0.8f);
            Measure(BoxConstraints(MinWidth.Value(), maxW, MinHeight.Value(), maxH));

            const f32 finalW = MeasuredSize.x;
            const f32 finalH = MeasuredSize.y;

            const f32 x = (viewportW - finalW) * 0.5f;
            const f32 y = (viewportH - finalH) * 0.5f;

            Layout(x, y, finalW, finalH);
            root->GetPopupLayer()->UpdatePopupPosition(this, x, y);
        }

        /// Close the dialog with a result. Deferred via MutationQueue.
        void Close(DialogResult result = DialogResult::None)
        {
            if (result != DialogResult::None)
            {
                Result = result;
            }
            OnClosed.Invoke(this, Result);
            UIContext* ctx = Context;
            if (ctx != nullptr)
            {
                Dialog* self = this;
                ctx->MutationQueueRef().QueueAction(
                    Function<void()>{[ctx, self]()
                                     {
                                         if (RootView* root = ctx->ActiveInputRoot())
                                         {
                                             root->GetPopupLayer()->ClosePopup(self);
                                         }
                                     }});
            }
        }

        // === Visual children: the internal layout ===

        [[nodiscard]] usize VisualChildCount() const override { return 1; }
        [[nodiscard]] View* GetVisualChild(usize index) const override
        {
            return (index == 0) ? m_layout.Get() : nullptr;
        }

        // === Drawing ===

        void OnDraw(UIDrawContext& ctx) override
        {
            const Rectangle bounds{0, 0, Width(), Height()};
            Drawable* bg = ResolveStyleDrawable(StyleProperty::Background);
            if (bg != nullptr)
            {
                bg->Draw(ctx, bounds, GetControlState());
            }
            else
            {
                ctx.VG().FillRoundedRect(
                    bounds, 6.0f, Color{50.0f / 255.0f, 52.0f / 255.0f, 62.0f / 255.0f, 1.0f});
                ctx.VG().StrokeRoundedRect(
                    bounds, 6.0f, Color{80.0f / 255.0f, 85.0f / 255.0f, 100.0f / 255.0f, 1.0f},
                    1.0f);
            }
            DrawChildren(ctx);
        }

        // === Escape to close ===

        void OnKeyDown(KeyEventArgs& e) override
        {
            if (e.Key == KeyCode::Escape)
            {
                Close(DialogResult::Cancel);
                e.Handled = true;
            }
        }

        // === Static factories ===

        /// Create a simple alert dialog with an OK button.
        static RefPtr<Dialog> Alert(StringView title, StringView message)
        {
            RefPtr<Dialog> dialog = MakeRef<Dialog>(DefaultAllocator(), title);
            RefPtr<Label> label = MakeRef<Label>(DefaultAllocator(), message);
            label->WordWrap.SetValue(true); // long messages wrap inside the dialog width
            dialog->SetContent(label.Get());
            dialog->AddButton(u8"OK", DialogResult::OK);
            return dialog;
        }

        /// Create a confirm dialog with OK and Cancel buttons.
        static RefPtr<Dialog> Confirm(StringView title, StringView message)
        {
            RefPtr<Dialog> dialog = MakeRef<Dialog>(DefaultAllocator(), title);
            RefPtr<Label> label = MakeRef<Label>(DefaultAllocator(), message);
            label->WordWrap.SetValue(true);
            dialog->SetContent(label.Get());
            dialog->AddButton(u8"OK", DialogResult::OK);
            dialog->AddButton(u8"Cancel", DialogResult::Cancel);
            return dialog;
        }

    protected:
        // === Layout ===

        void OnMeasure(BoxConstraints constraints) override
        {
            // Apply Dialog's own min/max, then intersect with input constraints.
            const f32 effMinW = Max(MinWidth.Value(), constraints.MinWidth);
            const f32 effMaxW =
                Min(MaxWidth.Value() > 0 ? MaxWidth.Value() : kFloatMax, constraints.MaxWidth);
            const f32 effMinH = Max(MinHeight.Value(), constraints.MinHeight);
            const f32 effMaxH =
                Min(MaxHeight.Value() > 0 ? MaxHeight.Value() : kFloatMax, constraints.MaxHeight);

            // First pass: measure with unconstrained height so simple content (text labels) wraps to its
            // natural size.
            const BoxConstraints inner(0, effMaxW, 0, kFloatMax);
            m_layout->Measure(inner);

            // Clamp to [min, max].
            const f32 finalW = Clamp(m_layout->MeasuredSize.x, effMinW, effMaxW);
            const f32 finalH = Clamp(m_layout->MeasuredSize.y, effMinH, effMaxH);

            // Second pass: if height was clamped (content was larger or smaller than bounds), re-measure
            // with the final bounded height so Grow children distribute space correctly.
            if (finalH != m_layout->MeasuredSize.y)
            {
                m_layout->Measure(BoxConstraints(finalW, finalW, finalH, finalH));
            }

            MeasuredSize = Float2{finalW, finalH};
        }

        void OnLayout(f32 left, f32 top, f32 width, f32 height) override
        {
            (void)left;
            (void)top;
            m_layout->Layout(0, 0, width, height);
        }

    private:
        RefPtr<FlexLayout> m_layout;
        Label* m_titleLabel = nullptr;
        // The button row is detached (RemoveView) then re-added in SetContent; our RemoveView drops the
        // tree's ref, so an independent RefPtr keeps it alive across that window (Beef's raw field relied
        // on detach-not-delete semantics).
        RefPtr<FlexLayout> m_buttonRow;
        View* m_content = nullptr;
    };

    DRACONIC_DEFINE_OBJECT(Dialog, "draconic::ui")
}
