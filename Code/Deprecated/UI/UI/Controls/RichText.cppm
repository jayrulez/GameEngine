// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI - :rich_text partition
//
// RichText: a paragraph of styled runs, the sibling of Label (which keeps its single-run contract
// and measure cache). Its text is markup (see :text_runs) or
// runs set directly; runs take the control's font family, size and text colour where they leave
// them unset; it wraps at its width unless told not to; a click on a link raises OnLink with the
// link's id. Markup problems are logged once per text and the offending text drawn as written.

module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"
#include "Core/Reflection/Reflect.h"

export module foundation.ui:rich_text;

import foundation.core;
import foundation.fonts;
import foundation.image;
import foundation.vg;
import :view;
import :event;
import :property;
import :box_constraints;
import :style_property;
import :draw_context;
import :palette;
import :event_args;
import :input_enums;
import :enums;
import :text_runs;
import :text_layout;

using namespace foundation::core;
namespace core = foundation::core;
namespace fonts = foundation::fonts;
namespace vg = foundation::vg;

export namespace foundation::ui
{
    class RichText : public View
    {
        RTTI_OBJECT(RichText, View)
    public:
        /// The text, as markup ([b], [color=], [link=], ...). Setting it replaces runs set directly.
        Property<String> Markup;
        Property<fonts::TextAlignment> HAlign{fonts::TextAlignment::Left};
        Property<fonts::VerticalAlignment> VAlign{fonts::VerticalAlignment::Top};
        Property<Optional<bool>> WordWrap; ///< unset: the style's word-wrap when set, else wrapped
        Property<Optional<f32>> FontSize;
        Property<String> FontFamily;
        Property<Optional<core::Color>> TextColor;

        /// A link was clicked: the control and the link's id.
        Event<void(RichText*, StringView)> OnLink;

        RichText()
        {
            Markup.SetOwner(this);
            HAlign.SetOwner(this, InvalidationKind::Visual);
            VAlign.SetOwner(this, InvalidationKind::Visual);
            WordWrap.SetOwner(this);
            FontSize.SetOwner(this);
            FontFamily.SetOwner(this);
            TextColor.SetOwner(this, InvalidationKind::Visual);
        }
        explicit RichText(StringView markup) : RichText() { Markup.SetSilent(String(markup)); }

        /// Set the markup and return this for chaining.
        RichText* SetMarkup(StringView markup)
        {
            Markup.SetValue(String(markup));
            Invalidate();
            return this;
        }

        /// Set the runs directly (a script building a paragraph without markup). Markup set later
        /// replaces them.
        void SetRuns(Array<TextRun> runs)
        {
            m_document = TextDocument{};
            m_document.runs = Move(runs);
            m_runsSet = true;
            Markup.SetSilent(String()); // markup set after replaces them, whatever it was before
            m_parsedMarkup.Clear();
            ++m_documentVersion;
            Invalidate();
        }

        /// Append a run, as SetRuns would with it on the end. When there is markup, its runs are
        /// kept and the markup is cleared.
        void AppendRun(TextRun run)
        {
            Array<TextRun> runs = Document().runs;
            runs.PushBack(Move(run));
            SetRuns(Move(runs));
        }

        /// The id of the link last clicked: what an OnLink handler that runs later (a deferred script
        /// handler) reads.
        [[nodiscard]] StringView LastLink() const noexcept { return m_lastLink.AsView(); }

        /// Raise OnLink as a click on the link `id` does (keyboard activation, tools, tests).
        void ActivateLink(StringView id)
        {
            m_lastLink = String(id); // copied, since the handler may change the text
            const String held = m_lastLink;
            OnLink.Invoke(this, held.AsView());
        }

        /// The runs the control draws (parsed from its markup, or set directly).
        [[nodiscard]] const TextDocument& Document()
        {
            SyncDocument();
            return m_document;
        }

        /// The last layout (the control's own coordinates), for tests and tools.
        [[nodiscard]] const TextLayout& Layout() const noexcept { return m_layout; }

        /// The link id under a point in the control's coordinates, or empty.
        [[nodiscard]] StringView LinkAt(Float2 local) const
        {
            const i32 run = m_layout.LinkAt(Float2{local.x - m_drawOffset.x, local.y - m_drawOffset.y});
            if (run < 0 || static_cast<usize>(run) >= m_document.runs.Size())
                return {};
            return m_document.runs[static_cast<usize>(run)].link.AsView();
        }

        [[nodiscard]] CursorType CursorAt(Float2 localPoint) const override
        {
            return LinkAt(localPoint).IsEmpty() ? Cursor : CursorType::Hand;
        }

        [[nodiscard]] Color ResolvedTextColor()
        {
            return TextColor.Value().HasValue()
                       ? TextColor.Value().Value()
                       : ResolveStyleColor(StyleProperty::TextColor,
                                           Color{220.0f / 255.0f, 225.0f / 255.0f, 235.0f / 255.0f, 1.0f});
        }

        [[nodiscard]] bool IsWordWrapped()
        {
            if (WordWrap.Value().HasValue())
                return WordWrap.Value().Value();
            const Optional<bool> styled = ResolveStyle(StyleProperty::WordWrap).AsBool();
            return !styled.HasValue() || styled.Value();
        }

    protected:
        void OnMeasure(BoxConstraints constraints) override
        {
            f32 width = 0.0f, height = 0.0f;
            if (Context != nullptr && Context->FontService() != nullptr)
            {
                const bool wrap = IsWordWrapped();
                const f32 maxWidth = constraints.MaxWidth < kFloatMax ? constraints.MaxWidth : 0.0f;
                const TextLayout& layout = LayOut(*Context->FontService(), wrap ? maxWidth : 0.0f);
                width = layout.width; // as wide as its widest line (within maxWidth when wrapped)
                height = layout.height;
            }
            MeasuredSize = Float2{constraints.ConstrainWidth(width), constraints.ConstrainHeight(height)};
        }

        // One run's glyphs, slanted when its italic is synthesized and twice when its bold is.
        void DrawRunGlyphs(UIDrawContext& ctx, const LaidOutGlyphs& run, Color color)
        {
            const bool slanted = run.syntheticItalic;
            if (slanted)
            {
                // Lean the glyphs about their baseline: x moves right by a fifth of the height
                // above it.
                ctx.VG().PushState();
                Float4x4 lean = Float4x4::Identity();
                const f32 baseline = m_drawOffset.y + run.baseline;
                lean.m[1][0] = -0.2f;
                lean.m[3][0] = 0.2f * baseline;
                ctx.VG().SetTransform(lean * ctx.VG().GetTransform());
            }
            ctx.VG().DrawPositionedGlyphs(run.glyphs, run.font, m_drawOffset.x, m_drawOffset.y, color);
            if (run.syntheticBold && !ctx.VG().TextEffectsOnly()) // effects once, under both strokes
            {
                ctx.VG().DrawPositionedGlyphs(run.glyphs, run.font, m_drawOffset.x + Max(0.5f, run.size / 24.0f),
                                              m_drawOffset.y, color);
            }
            if (slanted)
                ctx.VG().PopState();
        }

        void OnDraw(UIDrawContext& ctx) override
        {
            if (ctx.FontService() == nullptr)
                return;
            const bool wrap = IsWordWrapped();
            const TextLayout& layout = LayOut(*ctx.FontService(), wrap ? Width() : 0.0f, Width());
            const fonts::VerticalAlignment v = VAlign.Value();
            m_drawOffset = Float2{0.0f, v == fonts::VerticalAlignment::Middle   ? (Height() - layout.height) * 0.5f
                                        : v == fonts::VerticalAlignment::Bottom ? Height() - layout.height
                                                                                : 0.0f};
            const bool dim = !IsEffectivelyEnabled();
            const auto tone = [dim](Color c) { return dim ? Palette::ComputeDisabled(c) : c; };
            // All runs' effects (the view's effects, replaced where the markup sets its own) draw
            // under all runs' glyphs, so one run's outline or shadow never covers its neighbour.
            const vg::TextEffects inherited = ctx.VG().GetTextEffects();
            ctx.VG().PushState();
            ctx.VG().SetTextEffectsOnly(true);
            for (const LaidOutGlyphs& run : layout.glyphRuns)
            {
                const vg::TextEffects effects = m_document.runs[static_cast<usize>(run.run)].EffectsOver(inherited);
                if (!effects.Any())
                    continue;
                ctx.VG().SetTextEffects(effects);
                DrawRunGlyphs(ctx, run, tone(run.color));
            }
            ctx.VG().PopState();
            ctx.VG().PushState();
            ctx.VG().ClearTextEffects();
            for (const LaidOutGlyphs& run : layout.glyphRuns)
                DrawRunGlyphs(ctx, run, tone(run.color));
            ctx.VG().PopState();
            for (const LaidOutBox& underline : layout.underlines)
            {
                const Rectangle r = underline.rect;
                ctx.VG().FillRect(Rectangle{r.x + m_drawOffset.x, r.y + m_drawOffset.y, r.width, r.height},
                                  tone(underline.color));
            }
            IResourceProvider* provider = Context != nullptr ? Context->ResourceProvider() : nullptr;
            for (const LaidOutBox& icon : layout.icons)
            {
                const TextRun& run = m_document.runs[static_cast<usize>(icon.run)];
                const image::ImageData* image = provider != nullptr ? provider->LoadImage(run.icon.AsView()) : nullptr;
                if (image == nullptr)
                {
                    InvalidateVisual(); // not loaded yet: ask again next frame
                    continue;
                }
                const Rectangle r = icon.rect;
                ctx.VG().DrawImage(image, Rectangle{r.x + m_drawOffset.x, r.y + m_drawOffset.y, r.width, r.height});
            }
        }

        void OnMouseUp(MouseEventArgs& e) override
        {
            if (e.Button != MouseButton::Left)
                return;
            const StringView link = LinkAt(e.Position());
            if (link.IsEmpty())
                return;
            ActivateLink(link);
            e.Handled = true;
        }

    private:
        // The document from the markup, parsed again only when it changed; problems logged once.
        void SyncDocument()
        {
            const StringView markup = Markup.Value();
            if (m_runsSet && m_parsedMarkup.AsView() == markup)
                return;
            if (!m_runsSet && m_parsedMarkup.AsView() == markup && m_parsed)
                return;
            m_document = ParseTextMarkup(markup);
            for (const TextMarkupDiagnostic& diagnostic : m_document.diagnostics)
            {
                LOG_WARNING(u8"UI", u8"RichText markup at byte {}: {} (in \"{}\")", diagnostic.offset,
                            diagnostic.message.AsView(), markup);
            }
            m_parsedMarkup = String(markup);
            m_parsed = true;
            m_runsSet = false;
            ++m_documentVersion;
        }

        // Lay out against the style and width, again only when either or the document changed.
        const TextLayout& LayOut(fonts::IFontService& service, f32 maxWidth, f32 alignWidth = -1.0f)
        {
            SyncDocument();
            TextLayoutStyle style;
            style.family = ResolveStyleFontFamily(FontFamily.Value());
            style.size = FontSize.Value().HasValue() ? FontSize.Value().Value()
                                                     : ResolveStyleFloat(StyleProperty::FontSize, 16.0f);
            style.color = ResolvedTextColor();
            style.align = HAlign.Value();
            style.wrap = maxWidth > 0.0f;
            style.maxWidth = maxWidth > 0.0f ? maxWidth : Max(0.0f, alignWidth);
            if (m_layoutVersion == m_documentVersion && m_layoutFamily == style.family &&
                m_layoutSize == style.size && m_layoutColor == style.color && m_layoutAlign == style.align &&
                m_layoutWrap == style.wrap && m_layoutWidth == style.maxWidth && m_layoutService == &service)
            {
                return m_layout;
            }
            m_layout = LayOutText(m_document, style, service);
            m_layoutVersion = m_documentVersion;
            m_layoutFamily = style.family;
            m_layoutSize = style.size;
            m_layoutColor = style.color;
            m_layoutAlign = style.align;
            m_layoutWrap = style.wrap;
            m_layoutWidth = style.maxWidth;
            m_layoutService = &service;
            return m_layout;
        }

        TextDocument m_document;
        String m_lastLink;
        String m_parsedMarkup;
        bool m_parsed = false;
        bool m_runsSet = false;
        u64 m_documentVersion = 1;

        TextLayout m_layout;
        u64 m_layoutVersion = 0;
        String m_layoutFamily;
        f32 m_layoutSize = -1.0f;
        Color m_layoutColor{};
        fonts::TextAlignment m_layoutAlign = fonts::TextAlignment::Left;
        bool m_layoutWrap = false;
        f32 m_layoutWidth = -1.0f;
        const fonts::IFontService* m_layoutService = nullptr; // identity only, never dereferenced
        Float2 m_drawOffset{0.0f, 0.0f};
    };

    RTTI_DEFINE_OBJECT(RichText, "rtti::ui")
}
