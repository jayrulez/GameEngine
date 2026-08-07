// Draconic GUI - :transition partition
//
// CSS transitions wired to the Action system. Ported from eepp's css/TransitionDefinition:
// parse the `transition` shorthand (`property duration [timing] [delay]`, comma-separated),
// and ApplyStyleAnimated animates transitioned properties from their previous value instead
// of snapping. This is where the CSS engine meets the Phase-3 ActionManager: a transitioned
// `opacity` change spawns a FadeAction on the node.
//
// v1 animates `opacity` (the common case + a clean showcase); size/position/color transitions
// follow the same pattern and land as their animatable actions are needed. Timing functions
// (easing) are parsed-but-ignored for now.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.gui:transition;

import draconic.foundation;  // String, StringView, Array, Optional, f32, Duration, MakeRef, Move
import :style_sheet;   // ResolvedStyle
import :style_applier; // ApplyStyle
import :css_values;    // ParseLength
import draconic.fonts; // IFontService
import :ui_node;       // UINode
import :actions;       // FadeAction
import :resource_provider;

namespace fonts = draconic::fonts;

using namespace draconic::foundation;
namespace foundation = draconic::foundation;

export namespace draconic::gui
{
    struct TransitionDefinition
    {
        foundation::String Property;
        f32 Duration = 0.0f; // seconds
        f32 Delay = 0.0f;    // seconds
    };

    // Parse the `transition` shorthand into definitions (comma-separated list).
    [[nodiscard]] inline Array<TransitionDefinition> ParseTransitions(foundation::StringView value)
    {
        Array<TransitionDefinition> out;
        usize start = 0;
        for (usize i = 0; i <= value.Size(); ++i)
        {
            if (i == value.Size() || value[i] == u8',')
            {
                const foundation::StringView part = foundation::Trim(value.SubStr(start, i - start));
                if (part.Size() != 0)
                {
                    foundation::StringView tokens[4];
                    usize count = 0, s = 0;
                    for (usize k = 0; k <= part.Size() && count < 4; ++k)
                    {
                        const bool boundary = (k == part.Size()) || foundation::IsWhiteSpace(part[k]);
                        if (boundary)
                        {
                            if (k > s)
                                tokens[count++] = part.SubStr(s, k - s);
                            s = k + 1;
                        }
                    }
                    if (count != 0)
                    {
                        TransitionDefinition d;
                        d.Property = tokens[0];
                        d.Duration = count >= 2 ? ParseLength(tokens[1]).ValueOr(0.0f) : 0.0f;
                        d.Delay = count >= 4 ? ParseLength(tokens[3]).ValueOr(0.0f) : 0.0f;
                        out.PushBack(foundation::Move(d));
                    }
                }
                start = i + 1;
            }
        }
        return out;
    }

    [[nodiscard]] inline const TransitionDefinition*
    FindTransition(const Array<TransitionDefinition>& list, foundation::StringView property)
    {
        for (const TransitionDefinition& d : list)
            if (d.Property == property)
                return &d;
        return nullptr;
    }

    // Apply `newStyle` to the node, animating any transitioned property from its `oldStyle`
    // value. Non-transitioned properties (and any property with no running coordinator) snap.
    inline void ApplyStyleAnimated(UINode& node, const ResolvedStyle& oldStyle,
                                   const ResolvedStyle& newStyle,
                                   IResourceProvider* resources = nullptr,
                                   fonts::IFontService* fontService = nullptr,
                                   const LengthContext& lengths = {})
    {
        // Apply everything first (this also sets the final opacity); a spawned FadeAction then
        // rewinds opacity to its old value on Start() and animates back to the applied value.
        ApplyStyle(node, newStyle, resources, fontService, lengths);

        const Array<TransitionDefinition> transitions =
            ParseTransitions(newStyle.Get(foundation::StringView(u8"transition")));
        if (const TransitionDefinition* opacity =
                FindTransition(transitions, foundation::StringView(u8"opacity")))
        {
            const Optional<f32> from =
                ParseLength(oldStyle.Get(foundation::StringView(u8"opacity"), foundation::StringView(u8"1")));
            const Optional<f32> to =
                ParseLength(newStyle.Get(foundation::StringView(u8"opacity"), foundation::StringView(u8"1")));
            if (from.HasValue() && to.HasValue() && from.Value() != to.Value())
                node.RunAction(
                    foundation::MakeRef<FadeAction>(foundation::DefaultAllocator(), from.Value(), to.Value(),
                                              foundation::Duration::FromSeconds(opacity->Duration)));
        }
    }
}
