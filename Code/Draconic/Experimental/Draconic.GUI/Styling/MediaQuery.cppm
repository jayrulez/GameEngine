// Draconic GUI - :media_query partition
//
// MediaQuery + MediaContext: `@media` condition evaluation. Ported from eepp's css/MediaQuery
// (common subset): width/height feature tests (min-/max-/exact) combined with `and`. A rule
// carries an (optional) MediaQuery; StyleSheet::Resolve skips rules whose query doesn't match
// the supplied MediaContext (viewport size / dpi). Deferred: orientation, aspect-ratio, `or`
// / comma lists, `not`, resolution units.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.gui:media_query;

import draconic.foundation; // StringView, Array, f32
import :css_values;   // ParseLength

using namespace draconic::foundation;
namespace foundation = draconic::foundation;

export namespace draconic::gui
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
        Height
    };

    struct MediaFeature
    {
        MediaFeatureType Type;
        f32 Value;
    };

    // A conjunction of feature tests (all must hold). An empty query always matches.
    class MediaQuery
    {
    public:
        MediaQuery() = default;
        explicit MediaQuery(foundation::StringView text) { Parse(text); }

        [[nodiscard]] bool IsEmpty() const noexcept { return m_features.Size() == 0; }
        [[nodiscard]] usize FeatureCount() const noexcept { return m_features.Size(); }

        [[nodiscard]] bool Evaluate(const MediaContext& context) const
        {
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
            }
            return true;
        }

        void Parse(foundation::StringView text)
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

        void ParseFeature(foundation::StringView group)
        {
            usize colon = group.Size();
            for (usize i = 0; i < group.Size(); ++i)
                if (group[i] == u8':')
                {
                    colon = i;
                    break;
                }
            if (colon >= group.Size())
                return;

            const foundation::StringView name = foundation::Trim(group.SubStr(0, colon));
            const Optional<f32> value =
                ParseLength(foundation::Trim(group.SubStr(colon + 1, group.Size() - colon - 1)));
            if (!value.HasValue())
                return;

            MediaFeatureType type;
            if (name == foundation::StringView(u8"min-width"))
                type = MediaFeatureType::MinWidth;
            else if (name == foundation::StringView(u8"max-width"))
                type = MediaFeatureType::MaxWidth;
            else if (name == foundation::StringView(u8"min-height"))
                type = MediaFeatureType::MinHeight;
            else if (name == foundation::StringView(u8"max-height"))
                type = MediaFeatureType::MaxHeight;
            else if (name == foundation::StringView(u8"width"))
                type = MediaFeatureType::Width;
            else if (name == foundation::StringView(u8"height"))
                type = MediaFeatureType::Height;
            else
                return; // unknown feature ignored

            m_features.PushBack(MediaFeature{type, value.Value()});
        }

        Array<MediaFeature> m_features;
    };
}
