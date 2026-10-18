// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - :media_query partition
//
// MediaQuery + MediaContext: `@media` condition evaluation. Ported from eepp's css/MediaQuery
// (common subset): width/height feature tests (min-/max-/exact) combined with `and`. A rule
// carries an (optional) MediaQuery; StyleSheet::Resolve skips rules whose query doesn't match
// the supplied MediaContext (viewport size / dpi); a query naming any other feature matches
// nothing. Deferred: orientation, aspect-ratio, `or`
// / comma lists, `not`.

module;
#include "Core/Prelude.h"

export module experimental.gui:media_query;

import foundation.core; // StringView, Array, f32
import :css_values;   // ParseLength

using namespace foundation::core;
namespace core = foundation::core;

export namespace experimental::gui
{
    // The environment a media query is evaluated against.
    struct MediaContext
    {
        f32 Width = 0.0f;
        f32 Height = 0.0f;
        f32 Dpi = 96.0f;
    };

    enum class MediaFeatureType
    {
        MinWidth,
        MaxWidth,
        MinHeight,
        MaxHeight,
        Width,
        Height,
        MinResolution, ///< in dpi: 1dppx (1x) is 96dpi
        MaxResolution,
        Resolution,
    };

    struct MediaFeature
    {
        MediaFeatureType Type;
        f32 Value;
    };

    // A conjunction of feature tests (all must hold). An empty query always matches; one with a
    // feature the lab does not know never does (CSS's "not all"), so a rule written for a feature
    // it cannot test is not applied everywhere.
    class MediaQuery
    {
    public:
        MediaQuery() = default;
        explicit MediaQuery(core::StringView text) { Parse(text); }

        [[nodiscard]] bool IsEmpty() const noexcept { return m_features.Size() == 0 && !m_unknown; }
        [[nodiscard]] usize FeatureCount() const noexcept { return m_features.Size(); }

        [[nodiscard]] bool Evaluate(const MediaContext& context) const
        {
            if (m_unknown)
                return false;
            for (const MediaFeature& f : m_features)
                if (!EvaluateFeature(f, context))
                    return false;
            return true;
        }

    private:
        [[nodiscard]] static bool EvaluateFeature(const MediaFeature& f,
                                                  const MediaContext& c) noexcept
        {
            switch (f.Type)
            {
            case MediaFeatureType::MinWidth:
                return c.Width >= f.Value;
            case MediaFeatureType::MaxWidth:
                return c.Width <= f.Value;
            case MediaFeatureType::MinHeight:
                return c.Height >= f.Value;
            case MediaFeatureType::MaxHeight:
                return c.Height <= f.Value;
            case MediaFeatureType::Width:
                return c.Width == f.Value;
            case MediaFeatureType::Height:
                return c.Height == f.Value;
            case MediaFeatureType::MinResolution:
                return c.Dpi >= f.Value;
            case MediaFeatureType::MaxResolution:
                return c.Dpi <= f.Value;
            case MediaFeatureType::Resolution:
                return c.Dpi == f.Value;
            }
            return true;
        }

        void Parse(core::StringView text)
        {
            // Scan parenthesized "(feature: value)" groups; `and` between them is implicit.
            usize i = 0;
            const usize n = text.Size();
            while (i < n)
            {
                while (i < n && text[i] != u8'(')
                    ++i;
                if (i >= n)
                    break;
                ++i;
                const usize start = i;
                while (i < n && text[i] != u8')')
                    ++i;
                ParseFeature(text.SubStr(start, i - start));
                if (i < n)
                    ++i; // skip ')'
            }
        }

        void ParseFeature(core::StringView group)
        {
            usize colon = group.Size();
            for (usize i = 0; i < group.Size(); ++i)
                if (group[i] == u8':')
                {
                    colon = i;
                    break;
                }
            if (colon >= group.Size())
            {
                m_unknown = true; // a boolean feature ("(orientation)"): none is known
                return;
            }

            const core::StringView name = core::Trim(group.SubStr(0, colon));
            const core::StringView text = core::Trim(group.SubStr(colon + 1, group.Size() - colon - 1));
            const bool resolution = name == core::StringView(u8"min-resolution") ||
                                    name == core::StringView(u8"max-resolution") ||
                                    name == core::StringView(u8"resolution");
            const Optional<f32> value = resolution ? ParseResolution(text) : ParseLength(text);
            if (!value.HasValue())
            {
                m_unknown = true; // a value it cannot read (`orientation: landscape`)
                return;
            }

            MediaFeatureType type;
            if (name == core::StringView(u8"min-width"))
                type = MediaFeatureType::MinWidth;
            else if (name == core::StringView(u8"max-width"))
                type = MediaFeatureType::MaxWidth;
            else if (name == core::StringView(u8"min-height"))
                type = MediaFeatureType::MinHeight;
            else if (name == core::StringView(u8"max-height"))
                type = MediaFeatureType::MaxHeight;
            else if (name == core::StringView(u8"width"))
                type = MediaFeatureType::Width;
            else if (name == core::StringView(u8"height"))
                type = MediaFeatureType::Height;
            else if (name == core::StringView(u8"min-resolution"))
                type = MediaFeatureType::MinResolution;
            else if (name == core::StringView(u8"max-resolution"))
                type = MediaFeatureType::MaxResolution;
            else if (name == core::StringView(u8"resolution"))
                type = MediaFeatureType::Resolution;
            else
            {
                m_unknown = true; // a feature it does not know
                return;
            }

            m_features.PushBack(MediaFeature{type, value.Value()});
        }

        // `2dppx`, `2x` or `192dpi`, in dpi.
        [[nodiscard]] static Optional<f32> ParseResolution(core::StringView text)
        {
            const auto number = [&](usize suffix) { return ParseLength(text.SubStr(0, text.Size() - suffix)); };
            const auto endsWith = [&](const char8_t* unit)
            {
                const core::StringView u(unit);
                return text.Size() > u.Size() && text.SubStr(text.Size() - u.Size(), u.Size()) == u;
            };
            if (endsWith(u8"dppx"))
                if (Optional<f32> n = number(4); n.HasValue())
                    return n.Value() * 96.0f;
            if (endsWith(u8"dpi"))
                return number(3);
            if (endsWith(u8"x"))
                if (Optional<f32> n = number(1); n.HasValue())
                    return n.Value() * 96.0f;
            return {};
        }

        Array<MediaFeature> m_features;
        bool m_unknown = false; // a feature it cannot test: the query matches nothing
    };
}
