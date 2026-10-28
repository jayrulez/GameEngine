// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Profiler.GUI - :widget partition
//
// What every profiler view shares: the focus it reads (joined while set), its colours from the
// sheet's parts (each view's tag, Profiler.GUI's sheet giving them through the theme's tokens), a
// name's colour (one of eight hues picked by the name, so a scope keeps its colour across frames
// and lanes) and one text drawn in turn for its labels.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.profiler.gui:widget;

import foundation.core;
import foundation.vg;
import foundation.gui;
import foundation.profiler;
import :focus;

using namespace foundation::core;
using namespace foundation::gui;

export namespace foundation::profiler::gui
{
    class ProfilerWidget : public UIWidget, public IProfileFocusClient
    {
        RTTI_OBJECT(ProfilerWidget, UIWidget)
    public:
        /// A name's colours: this many hues, the sheet's `hue-0` to `hue-7` parts.
        static constexpr u32 kHueCount = 8;

        ProfilerWidget() = default;
        ~ProfilerWidget() override
        {
            if (m_focus)
                m_focus->Leave(this);
        }

        /// What it shows (shared: every view on one page holds the same focus).
        void SetProfileFocus(RefPtr<ProfileFocus> focus)
        {
            if (m_focus)
                m_focus->Leave(this);
            m_focus = core::Move(focus);
            if (m_focus)
                m_focus->Join(this);
            OnProfileSourceChanged();
            Invalidate();
        }
        [[nodiscard]] ProfileFocus* GetProfileFocus() const noexcept { return m_focus.Get(); }
        [[nodiscard]] const IProfileSource* Source() const noexcept { return m_focus ? m_focus->Source() : nullptr; }

        /// The hue a name is drawn in (0 to kHueCount - 1): the same for its every scope.
        [[nodiscard]] static u32 HueOf(StringView name) noexcept
        {
            u32 hash = 0;
            for (usize i = 0; i < name.Size(); ++i)
                hash = hash * 31u + static_cast<u32>(name[i]);
            return hash % kHueCount;
        }

        /// The parts every view draws: a name's hues. Each view adds its own; a part the sheet is
        /// to style must be named here, or the sheet never reaches it.
        void CollectStyleParts(Array<StringView>& out) const override
        {
            static constexpr const char8_t* kHues[kHueCount] = {u8"hue-0", u8"hue-1", u8"hue-2", u8"hue-3",
                                                               u8"hue-4", u8"hue-5", u8"hue-6", u8"hue-7"};
            for (const char8_t* hue : kHues)
                out.PushBack(StringView(hue));
        }

        void OnProfileSourceChanged() override { Invalidate(); }
        void OnProfileFramesChanged() override { Invalidate(); }
        void OnProfileChoiceChanged() override { Invalidate(); }

    protected:
        /// A part's background colour (times its opacity), or `fallback` where the sheet gives none.
        [[nodiscard]] Color PartColor(StringView part, Color fallback) const
        {
            const PartStyle& style = GetPartStyle(part);
            Color color = style.HasBackground ? style.Box.BackgroundColor : fallback;
            color.a *= style.Opacity;
            return color;
        }
        /// A part's text colour, or the view's own.
        [[nodiscard]] Color PartTextColor(StringView part) const
        {
            const PartStyle& style = GetPartStyle(part);
            return style.HasColor ? style.TextColor : GetTextColor();
        }
        /// A name's colour: its hue's part.
        [[nodiscard]] Color NameColor(StringView name) const
        {
            static constexpr const char8_t* kHues[kHueCount] = {u8"hue-0", u8"hue-1", u8"hue-2", u8"hue-3",
                                                               u8"hue-4", u8"hue-5", u8"hue-6", u8"hue-7"};
            return PartColor(StringView(kHues[HueOf(name)]), Color{0.55f, 0.65f, 0.80f, 1.0f});
        }

        void OnTextStyleChange() override
        {
            m_text.SetFont(GetFont());
            m_text.SetFontSelection(GetFontService(), GetTextStyle().FontFamily.AsView(), GetTextStyle().Selection);
            m_text.SetWhiteSpace(TextWhiteSpace::NoWrap);
            m_text.SetOverflow(TextOverflow::Ellipsis);
        }
        /// `text` in `bounds` (one line, cut with an ellipsis), placed by `align`, in `color`.
        void DrawLabel(DrawContext& ctx, StringView text, const Rect& bounds, Color color,
                       TextHAlign align = TextHAlign::Start) const
        {
            if (!m_text.HasFont() || bounds.width < 4.0f || text.IsEmpty())
                return;
            m_text.SetString(text);
            m_text.SetColor(color);
            m_text.SetAlignment(align, TextVAlign::Middle);
            m_text.Draw(ctx, bounds);
        }
        /// How wide `text` is, in the view's font.
        [[nodiscard]] f32 LabelWidth(StringView text) const
        {
            if (!m_text.HasFont())
                return 0.0f;
            m_text.SetString(text);
            return m_text.GetWidth();
        }
        /// A point of an event (screen space) in its own space.
        [[nodiscard]] core::Float2 Local(core::Float2 screen) const { return ConvertToNodeSpace(screen); }

        RefPtr<ProfileFocus> m_focus;
        mutable Text m_text; // each label in turn
    };
}
