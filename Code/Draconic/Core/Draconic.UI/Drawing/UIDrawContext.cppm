// Draconic UI - :draw_context partition
//
// Drawing context passed to View.OnDraw(). Wraps a VGContext with clip stacking, plus
// the font service and DPI scale. Ported from Sedulous.UI/src/Drawing/UIDrawContext.bf.
// Beef `VGContext mVG` (class = by ref) -> stored VGContext* (VG() returns the ref).

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.ui:draw_context;

import draconic.foundation;  // Rectangle
import draconic.vg;    // VGContext
import draconic.fonts; // IFontService
import :debug_settings;

using namespace draconic::foundation;
namespace vg = draconic::vg;
namespace fonts = draconic::fonts;

export namespace draconic::ui
{
    class UIDrawContext
    {
    public:
        UIDrawContext(vg::VGContext& context, f32 dpiScale,
                      fonts::IFontService* fontService = nullptr,
                      UIDebugDrawSettings debugSettings = {}) noexcept
            : m_vg(&context), m_dpiScale(dpiScale), m_fontService(fontService),
              m_debugSettings(debugSettings)
        {
        }

        /// The underlying vector-graphics context.
        [[nodiscard]] vg::VGContext& VG() const noexcept { return *m_vg; }
        /// Current DPI scale.
        [[nodiscard]] f32 DpiScale() const noexcept { return m_dpiScale; }
        /// Font service for text rendering (may be null).
        [[nodiscard]] fonts::IFontService* FontService() const noexcept { return m_fontService; }
        /// Debug overlay settings.
        [[nodiscard]] const UIDebugDrawSettings& DebugSettings() const noexcept
        {
            return m_debugSettings;
        }

        /// Pushes a clip rectangle (in current local coordinates).
        void PushClip(const Rectangle& rect) { m_vg->PushClipRect(rect); }
        /// Pops the last pushed clip.
        void PopClip() { m_vg->PopClip(); }

    private:
        vg::VGContext* m_vg;
        f32 m_dpiScale;
        fonts::IFontService* m_fontService;
        UIDebugDrawSettings m_debugSettings;
    };
}
