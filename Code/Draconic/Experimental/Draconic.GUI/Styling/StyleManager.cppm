// Draconic GUI - :style_manager partition
//
// StyleManager: the glue that makes a StyleSheet live on a widget tree. It resolves each
// UIWidget against the sheet (+ MediaContext), applies the result, and - by caching the
// last-applied ResolvedStyle per widget - animates transitioned changes on the next apply.
// Re-applying after a state change (e.g. :hover) therefore drives CSS transitions live.
//
// The per-widget cache lives here (not on UIWidget) to avoid a :ui_widget <-> :style_sheet
// partition cycle. v1 re-resolves the whole subtree on ApplyTree; dirty-tracking (only
// re-resolve invalidated widgets) is a later optimization. Cache entries are keyed by raw
// Node* - call Forget()/Clear() when widgets are destroyed (lifecycle wiring deferred).

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.gui:style_manager;

import draconic.foundation;  // HashMap, Cast, Move, Array, StringView, Float2, Duration, MakeRef
import draconic.fonts; // IFontService
import :node;
import :ui_node;
import :ui_widget;
import :style_rule; // StyleProperty
import :style_sheet;
import :media_query;
import :style_applier;
import :transition;
import :resource_provider;
import :css_values; // ParseLength
import :actions;    // KeyframeAction

using namespace draconic::foundation;
namespace foundation = draconic::foundation;
namespace fonts = draconic::fonts;

export namespace draconic::gui
{
    class StyleManager
    {
    public:
        StyleManager() = default;
        explicit StyleManager(StyleSheet sheet) : m_sheet(foundation::Move(sheet)) {}

        void SetStyleSheet(StyleSheet sheet)
        {
            m_sheet = foundation::Move(sheet);
            m_cache = {};
        } // hot-reload: drop cache
        [[nodiscard]] const StyleSheet& GetStyleSheet() const noexcept { return m_sheet; }

        void SetMediaContext(const MediaContext& context) { m_context = context; }
        [[nodiscard]] const MediaContext& GetMediaContext() const noexcept { return m_context; }

        // Root font size for CSS `rem` units (default 16px).
        void SetRootFontSize(f32 pixels) noexcept { m_rootFontSize = pixels; }
        [[nodiscard]] f32 GetRootFontSize() const noexcept { return m_rootFontSize; }

        // Loads background-image assets referenced by the sheet. Null = background-image skipped.
        void SetResourceProvider(IResourceProvider* resources) noexcept { m_resources = resources; }
        [[nodiscard]] IResourceProvider* GetResourceProvider() const noexcept
        {
            return m_resources;
        }

        // Resolves font-family/-size in the sheet (the app's font service - VFS-backed or not;
        // the GUI is agnostic). Null = font-family skipped.
        void SetFontService(fonts::IFontService* fontService) noexcept
        {
            m_fontService = fontService;
        }
        [[nodiscard]] fonts::IFontService* GetFontService() const noexcept { return m_fontService; }

        // Resolve + apply one widget; animate any transitioned change versus its last apply.
        void ApplyTo(UIWidget& widget)
        {
            ResolvedStyle resolved = m_sheet.Resolve(widget, m_context);

            // Build the length-resolution context: viewport from the media context, root font
            // size configurable, element font size from this widget's resolved font-size (for em).
            LengthContext lengths;
            lengths.RootFontSize = m_rootFontSize;
            lengths.ViewportWidth = m_context.Width;
            lengths.ViewportHeight = m_context.Height;
            lengths.ElementFontSize =
                ParseLength(resolved.Get(foundation::StringView(u8"font-size"), foundation::StringView(u8"")))
                    .ValueOr(m_rootFontSize);

            if (const ResolvedStyle* previous = m_cache.Find(&widget))
                ApplyStyleAnimated(widget, *previous, resolved, m_resources, m_fontService,
                                   lengths);
            else
                ApplyStyle(widget, resolved, m_resources, m_fontService, lengths);
            m_cache.InsertOrAssign(&widget, foundation::Move(resolved));

            // Pseudo-element parts (tag::part): resolve + apply each part the widget declares.
            Array<foundation::StringView> parts;
            widget.CollectStyleParts(parts);
            for (const foundation::StringView part : parts)
            {
                const ResolvedStyle partStyle = m_sheet.Resolve(widget, m_context, true, part);
                ApplyPartStyle(widget, part, partStyle);
            }

            // @keyframes animation: spawn a KeyframeAction when the `animation` property names
            // one, and only once per (widget, animation-name) so ApplyTree doesn't re-spawn it.
            ApplyAnimation(widget, resolved);
        }

        // Apply to every UIWidget in the subtree.
        void ApplyTree(Node& root)
        {
            if (UIWidget* widget = foundation::Cast<UIWidget>(&root))
                ApplyTo(*widget);
            for (usize i = 0; i < root.ChildCount(); ++i)
                if (Node* child = root.GetChildAt(i))
                    ApplyTree(*child);
        }

        // Drop a widget's cached style (call before it is destroyed).
        void Forget(Node* widget)
        {
            m_cache.Remove(widget);
            m_animations.Remove(widget);
        }
        void Clear()
        {
            m_cache = {};
            m_animations = {};
        }

    private:
        // Spawn the @keyframes animation named by the `animation` property (if any), once.
        void ApplyAnimation(UIWidget& widget, const ResolvedStyle& style)
        {
            if (!style.Has(foundation::StringView(u8"animation")))
            {
                m_animations.Remove(
                    &widget); // (a running loop keeps going; re-appearance re-spawns)
                return;
            }
            foundation::String name;
            f32 durationSecs = 0.0f;
            bool loop = false;
            if (!ParseAnimation(style.Get(foundation::StringView(u8"animation")), name, durationSecs,
                                loop))
                return;

            const foundation::String* running = m_animations.Find(&widget);
            if (running != nullptr && running->AsView() == name.AsView())
                return; // already running this one

            const Keyframes* kf = m_sheet.FindKeyframes(name.AsView());
            if (kf == nullptr)
                return;
            widget.RunAction(foundation::MakeRef<KeyframeAction>(
                foundation::DefaultAllocator(), ExtractOpacityTrack(*kf), ExtractColorTrack(*kf),
                foundation::Duration::FromSeconds(static_cast<f64>(durationSecs)), loop));
            m_animations.InsertOrAssign(&widget, foundation::Move(name));
        }

        // Parse `animation: name duration [infinite]` (timing/direction/etc. ignored for v1).
        [[nodiscard]] static bool ParseAnimation(foundation::StringView value, foundation::String& name,
                                                 f32& durationSecs, bool& loop)
        {
            bool haveName = false;
            usize start = 0;
            const usize n = value.Size();
            for (usize i = 0; i <= n; ++i)
            {
                if (i == n || IsWhiteSpace(value[i]))
                {
                    if (i > start)
                    {
                        const foundation::StringView tok = value.SubStr(start, i - start);
                        if (!haveName)
                        {
                            name = foundation::String(tok);
                            haveName = true;
                        }
                        else if (tok == foundation::StringView(u8"infinite"))
                            loop = true;
                        else if (Optional<f32> d = ParseLength(tok); d.HasValue())
                            durationSecs = d.Value();
                    }
                    start = i + 1;
                }
            }
            return haveName;
        }

        // Pull the opacity track {offset, opacity} from a keyframes definition, sorted by offset.
        [[nodiscard]] static Array<foundation::Float2> ExtractOpacityTrack(const Keyframes& kf)
        {
            Array<foundation::Float2> track;
            for (const KeyframeStop& stop : kf.Stops)
                for (const StyleProperty& p : stop.Properties)
                    if (p.Name.AsView() == foundation::StringView(u8"opacity"))
                        if (Optional<f32> o = ParseLength(p.Value.AsView()); o.HasValue())
                            track.PushBack(foundation::Float2{stop.Offset, o.Value()});

            for (usize a = 1; a < track.Size(); ++a) // insertion sort by offset
            {
                const foundation::Float2 key = track[a];
                usize b = a;
                while (b > 0 && track[b - 1].x > key.x)
                {
                    track[b] = track[b - 1];
                    --b;
                }
                track[b] = key;
            }
            return track;
        }

        // Pull the background-color track from a keyframes definition, sorted by offset.
        [[nodiscard]] static Array<ColorKey> ExtractColorTrack(const Keyframes& kf)
        {
            Array<ColorKey> track;
            for (const KeyframeStop& stop : kf.Stops)
                for (const StyleProperty& p : stop.Properties)
                    if (p.Name.AsView() == foundation::StringView(u8"background-color"))
                        if (Optional<Color> c = ParseColor(p.Value.AsView()); c.HasValue())
                            track.PushBack(ColorKey{stop.Offset, c.Value()});

            for (usize a = 1; a < track.Size(); ++a) // insertion sort by offset
            {
                const ColorKey key = track[a];
                usize b = a;
                while (b > 0 && track[b - 1].Offset > key.Offset)
                {
                    track[b] = track[b - 1];
                    --b;
                }
                track[b] = key;
            }
            return track;
        }

        StyleSheet m_sheet;
        MediaContext m_context;
        IResourceProvider* m_resources = nullptr;     // non-owning; loads background-image assets
        fonts::IFontService* m_fontService = nullptr; // non-owning; resolves font-family
        f32 m_rootFontSize = 16.0f;                   // CSS `rem` base
        HashMap<Node*, ResolvedStyle> m_cache; // last-applied style per widget (non-owning keys)
        HashMap<Node*, foundation::String> m_animations; // widget -> running @keyframes animation name
    };
}
