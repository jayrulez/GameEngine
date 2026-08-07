// Draconic GUI - :progress_bar partition
//
// ProgressBar: a display-only [0,1] fill bar (track + fill). A lean Draconic-native control
// modeled on eepp's UIProgressBar (role only). No interaction; drive it with SetProgress.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.gui:progress_bar;

import draconic.foundation; // Color, Max, Min
import draconic.vg;   // CornerRadii
import :rect;
import :draw_context;
import :ui_widget;

using namespace draconic::foundation;
namespace foundation = draconic::foundation;
namespace vg = draconic::vg;

export namespace draconic::gui
{
    class ProgressBar : public UIWidget
    {
        DRACONIC_OBJECT(ProgressBar, UIWidget)
    public:
        ProgressBar() { SetTag(foundation::StringView(u8"progressbar")); }

        [[nodiscard]] f32 GetProgress() const noexcept { return m_progress; }
        void SetProgress(f32 progress)
        {
            progress = foundation::Max(0.0f, foundation::Min(1.0f, progress));
            if (progress == m_progress)
                return;
            m_progress = progress;
            Invalidate();
        }

        void SetTrackColor(Color color)
        {
            m_trackColor = color;
            Invalidate();
        }
        void SetFillColor(Color color)
        {
            m_fillColor = color;
            Invalidate();
        }

        // Theming parts: progressbar::track / ::fill.
        void CollectStyleParts(foundation::Array<foundation::StringView>& out) const override
        {
            out.PushBack(foundation::StringView(u8"track"));
            out.PushBack(foundation::StringView(u8"fill"));
        }
        void SetThemePartColor(foundation::StringView part, Color color) override
        {
            if (part == foundation::StringView(u8"track"))
                SetTrackColor(color);
            else if (part == foundation::StringView(u8"fill"))
                SetFillColor(color);
        }

    protected:
        void OnDraw(DrawContext& ctx, const Rect& localBounds) override
        {
            (void)localBounds;
            const Rect b = GetContentBounds();
            const f32 radius = b.height * 0.5f;
            ctx.VG().FillRoundedRect(b.ToRectangle(), vg::CornerRadii(radius), m_trackColor);
            if (m_progress > 0.0f)
            {
                const Rect fill{b.x, b.y, b.width * m_progress, b.height};
                ctx.VG().FillRoundedRect(fill.ToRectangle(), vg::CornerRadii(radius), m_fillColor);
            }
        }

        f32 m_progress = 0.0f;
        Color m_trackColor{0.24f, 0.26f, 0.30f, 1.0f};
        Color m_fillColor{0.31f, 0.63f, 0.85f, 1.0f};
    };

    DRACONIC_DEFINE_OBJECT(ProgressBar, "draconic::gui")
}
