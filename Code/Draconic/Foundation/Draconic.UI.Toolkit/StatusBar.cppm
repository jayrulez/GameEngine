// Draconic UI Toolkit - :status_bar partition
//
// Bottom status strip with text sections. Ported from Sedulous.UI.Toolkit/src/StatusBar.bf (a FlexLayout
// subclass). Beef raw-owned `mDefaultLabel` (new Label) -> a borrowed raw Label* (the flex child tree owns
// the RefPtr); `FontSize.Value = 12` -> FontSize.SetValue(12) (Property<Optional<f32>>); `.(4)` Thickness
// and `.(r,g,b,a)` Colors spelled out. The toolkit namespace nests in draconic::ui, so the core types
// (FlexLayout/Label/StyleProperty/...) resolve unqualified.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.ui.toolkit:status_bar;

import draconic.foundation;
import draconic.vg;
import draconic.ui;

using namespace draconic::foundation;

export namespace draconic::ui::toolkit
{
    /// Bottom status strip with text sections.
    class StatusBar : public FlexLayout
    {
        DRACONIC_OBJECT(StatusBar, FlexLayout)
    public:
        StatusBar()
        {
            Direction = Orientation::Horizontal;
            Spacing = 12.0f;
            Padding = Thickness(4.0f);
        }

        /// Set the default status text (creates the label on first call).
        void SetText(StringView text)
        {
            if (m_defaultLabel == nullptr)
            {
                RefPtr<Label> label = MakeRef<Label>(DefaultAllocator());
                label->FontSize.SetValue(12.0f);
                m_defaultLabel = label.Get();
                RefPtr<FlexLayoutParams> lp = MakeRef<FlexLayoutParams>(DefaultAllocator());
                lp->Width = SizeSpec::Match();
                lp->Height = SizeSpec::Match();
                lp->Grow = 1.0f;
                InsertView(label.Get(), 0, lp);
            }
            m_defaultLabel->SetText(text);
        }

        /// Add a named section label. Returns the borrowed Label for customization.
        Label* AddSection(StringView text)
        {
            RefPtr<Label> label = MakeRef<Label>(DefaultAllocator());
            label->FontSize.SetValue(12.0f);
            label->SetText(text);
            RefPtr<FlexLayoutParams> lp = MakeRef<FlexLayoutParams>(DefaultAllocator());
            lp->Height = SizeSpec::Match();
            Label* raw = label.Get();
            AddView(label.Get(), lp);
            return raw;
        }

        void OnDraw(UIDrawContext& ctx) override
        {
            // Background.
            if (Drawable* bg = ResolveStyleDrawable(StyleProperty::Background))
            {
                bg->Draw(ctx, Rectangle{0, 0, Width(), Height()});
            }
            else
            {
                ctx.VG().FillRect(Rectangle{0, 0, Width(), Height()},
                                  Color{30.0f / 255.0f, 32.0f / 255.0f, 40.0f / 255.0f, 1.0f});
            }

            // Top border.
            const Color borderColor =
                ResolveStyleColor(StyleProperty::BorderColor,
                                  Color{65.0f / 255.0f, 70.0f / 255.0f, 85.0f / 255.0f, 1.0f});
            ctx.VG().FillRect(Rectangle{0, 0, Width(), 1.0f}, borderColor);

            DrawChildren(ctx);
        }

    protected:
        void OnMeasure(BoxConstraints constraints) override
        {
            FlexLayout::OnMeasure(constraints);
            // Ensure a minimum height of 24px.
            if (MeasuredSize.y < 24.0f)
            {
                MeasuredSize = Float2{MeasuredSize.x, 24.0f};
            }
        }

    private:
        Label* m_defaultLabel = nullptr; // borrowed; the flex child tree owns the RefPtr
    };

    DRACONIC_DEFINE_OBJECT(StatusBar, "draconic::ui::toolkit")
}
