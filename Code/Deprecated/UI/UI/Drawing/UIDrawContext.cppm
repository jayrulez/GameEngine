// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI - :draw_context partition
//
// Drawing context passed to View.OnDraw(). Wraps a VGContext with clip stacking, plus
// the font service and DPI scale. Stores a VGContext* (VG() returns a reference).

module;
#include "Core/Prelude.h"

export module foundation.ui:draw_context;

import foundation.core;  // Rectangle
import foundation.vg;    // VGContext
import foundation.fonts; // IFontService
import :debug_settings;
import :control_state;

using namespace foundation::core;
namespace vg = foundation::vg;
namespace fonts = foundation::fonts;

export namespace foundation::ui
{
    class Drawable;

    /// What a running transition asks the drawable draw path to blend, set per child by
    /// ViewGroup::DrawChildren from the child's active transitions (see View::CurrentDrawBlend).
    /// Consumed once by Drawable::Draw (cleared for the nested drawables it draws).
    struct DrawBlend
    {
        /// A `background` drawable change mid-flight: `From` is drawn under `To` at `DrawableT`.
        Drawable* FromDrawable = nullptr;
        Drawable* ToDrawable = nullptr;
        f32 DrawableT = 1.0f;
        /// A control-state change mid-flight: a state-aware drawable draws `FromState` under
        /// `ToState` at `StateT`.
        bool StateActive = false;
        ControlState FromState = ControlState::Normal;
        ControlState ToState = ControlState::Normal;
        f32 StateT = 1.0f;

        [[nodiscard]] bool IsActive() const noexcept
        {
            return StateActive || FromDrawable != nullptr;
        }
    };

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

        /// Where a drawable's `var(--name)` colours resolve: the view being drawn's custom
        /// properties, through `lookup`. DrawChildren sets it around each view's drawing and puts
        /// the previous one back.
        using CustomColorLookup = Optional<core::Color> (*)(const void* source, StringView name);
        struct CustomColors
        {
            const void* source = nullptr;
            CustomColorLookup lookup = nullptr;
        };
        CustomColors SetCustomColors(CustomColors colors) noexcept
        {
            const CustomColors previous = m_customColors;
            m_customColors = colors;
            return previous;
        }
        /// A drawable's colour: the custom property `variable` names, if the view being drawn has
        /// it as a colour, else `value` (the var()'s fallback, or the colour written).
        [[nodiscard]] core::Color ColorOf(core::Color value, StringView variable) const
        {
            if (variable.IsEmpty() || m_customColors.lookup == nullptr)
            {
                return value;
            }
            const Optional<core::Color> found = m_customColors.lookup(m_customColors.source, variable);
            return found.HasValue() ? found.Value() : value;
        }

        /// Pushes a clip rectangle (in current local coordinates).
        void PushClip(const Rectangle& rect) { m_vg->PushClipRect(rect); }
        /// Pops the last pushed clip.
        void PopClip() { m_vg->PopClip(); }
        /// Clips to `rect` rounded by `radii` (current local coordinates), inside any rounded clip
        /// already pushed. Balance with PopClipRounded.
        void PushClipRounded(const Rectangle& rect, const vg::CornerRadii& radii)
        {
            vg::PathBuilder builder;
            vg::ShapeBuilder::BuildRoundedRect(rect, radii, builder);
            m_vg->PushClipPath(builder.ToPath());
        }
        /// Pops the last rounded clip.
        void PopClipRounded() { m_vg->PopClipPath(); }
        /// Whether a rect (current local coordinates) can contribute pixels under the active
        /// clip. True when no clip is active. The child-draw loop culls with this so clipped-out
        /// subtrees cost no tessellation (viewport-sized draw lists, not content-sized).
        [[nodiscard]] bool IsRectVisible(const Rectangle& rect) const
        {
            return m_vg->IsRectVisible(rect);
        }

        /// The blend the next Drawable::Draw applies (see DrawBlend). Returns the previous
        /// one so the caller can restore it.
        DrawBlend SetBlend(const DrawBlend& blend) noexcept
        {
            const DrawBlend previous = m_blend;
            m_blend = blend;
            return previous;
        }
        [[nodiscard]] const DrawBlend& Blend() const noexcept { return m_blend; }

    private:
        vg::VGContext* m_vg;
        DrawBlend m_blend{};
        f32 m_dpiScale;
        CustomColors m_customColors{};
        fonts::IFontService* m_fontService;
        UIDebugDrawSettings m_debugSettings;
    };
}
