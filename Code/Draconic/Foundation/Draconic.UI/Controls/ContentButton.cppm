// Draconic UI - :content_button partition
//
// Button with arbitrary View content - icons, icon+text combos, or any custom content layout. Ported
// from Sedulous.UI/src/Controls/ContentButton.bf. Content is RefPtr-owned (Beef raw owned + manual
// delete); it is drawn manually in OnDraw (not a logical/visual child), and attached to the context in
// OnMeasure so it can resolve fonts - same content pattern as ToggleButton.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.ui:content_button;

import draconic.foundation;
import draconic.vg;
import :button_base;
import :view;
import :control_state;
import :style_property;
import :thickness;
import :box_constraints;
import :draw_context;

using namespace draconic::foundation;
namespace foundation = draconic::foundation;

export namespace draconic::ui
{
    class ContentButton : public ButtonBase
    {
        DRACONIC_OBJECT(ContentButton, ButtonBase)
    public:
        ContentButton() = default;
        explicit ContentButton(RefPtr<View> content) : m_content(Move(content)) {}

        [[nodiscard]] View* Content() const noexcept { return m_content.Get(); }
        void SetContent(RefPtr<View> content)
        {
            m_content = Move(content);
            Invalidate();
        }

    protected:
        void OnMeasure(BoxConstraints constraints) override
        {
            const Thickness pad = ResolveStyleThickness(StyleProperty::Padding, Thickness{12, 8});
            const BoxConstraints inner = constraints.Deflate(pad).Loosen();

            f32 contentW = 0, contentH = 0;
            if (m_content)
            {
                // Pass the context down so content can resolve fonts during measure.
                if (m_content->Context == nullptr && Context != nullptr)
                {
                    Context->AttachView(m_content.Get());
                }
                m_content->Measure(inner);
                contentW = m_content->MeasuredSize.x;
                contentH = m_content->MeasuredSize.y;
            }
            MeasuredSize = Float2{constraints.ConstrainWidth(contentW + pad.TotalHorizontal()),
                                  constraints.ConstrainHeight(contentH + pad.TotalVertical())};
        }

        void OnLayout(f32 left, f32 top, f32 width, f32 height) override
        {
            (void)left;
            (void)top;
            if (!m_content)
            {
                return;
            }
            const Thickness pad = ResolveStyleThickness(StyleProperty::Padding, Thickness{12, 8});
            const f32 contentW = width - pad.TotalHorizontal();
            const f32 contentH = height - pad.TotalVertical();
            const f32 cw = m_content->MeasuredSize.x;
            const f32 ch = m_content->MeasuredSize.y;
            m_content->Layout(pad.Left + (contentW - cw) * 0.5f, pad.Top + (contentH - ch) * 0.5f,
                              cw, ch);
        }

        void OnDraw(UIDrawContext& ctx) override
        {
            const Rectangle bounds{0, 0, Width(), Height()};
            DrawButtonBackground(ctx, bounds, GetControlState());
            if (m_content)
            {
                ctx.VG().PushState();
                ctx.VG().Translate(m_content->Bounds.x, m_content->Bounds.y);
                m_content->OnDraw(ctx);
                ctx.VG().PopState();
            }
        }

    private:
        RefPtr<View> m_content;
    };

    DRACONIC_DEFINE_OBJECT(ContentButton, "draconic::ui")
}
