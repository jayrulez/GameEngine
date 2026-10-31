// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI - :theme_icon_set partition.
//
// The shared, bake-ready chrome glyph set (close, chevrons, checkmark, ...) behind the built-in
// themes, so every host draws them pixel-snapped from the same baked atlas.
//
// Themes get glyphs through Acquire(). When a host has initialized the set (UIHost does, in its
// constructor), every theme shares one BakedSVGDrawable per glyph and the host bakes them
// (UIHost::BakeThemeIcons, which supersamples and downsamples into an atlas). Until the bake
// finishes they draw as live vectors, so initialization and baking can happen in any order.
// Headless and test contexts never initialize the set; Acquire then hands out fresh unbaked
// SVGDrawables. Shutdown is explicit, from UIHost, so nothing depends on static teardown order.

module;
#include "Core/Prelude.h"

export module foundation.ui:theme_icon_set;

import foundation.core;
import :drawable;
import :svg_drawable;
import :theme_icons;

using namespace foundation::core;

export namespace foundation::ui
{
    enum class ThemeIcon : u8
    {
        Checkmark,
        ArrowDown,
        ArrowUp,
        ChevronRight,
        ChevronDown,
        Close,
        Plus,
        Minus,
        RadioMarkSquare,
        RadioMarkRound,
    };

    /// The built-in glyph for a kebab-case stylesheet name (the `svg(name)` factory's fallback
    /// vocabulary, also used by the UI cook to validate names). None when it is not built in.
    [[nodiscard]] inline Optional<ThemeIcon> ThemeIconFromName(StringView name)
    {
        struct Entry
        {
            StringView name;
            ThemeIcon icon;
        };
        static constexpr Entry kNames[] = {
            {u8"checkmark", ThemeIcon::Checkmark},
            {u8"arrow-down", ThemeIcon::ArrowDown},
            {u8"arrow-up", ThemeIcon::ArrowUp},
            {u8"chevron-right", ThemeIcon::ChevronRight},
            {u8"chevron-down", ThemeIcon::ChevronDown},
            {u8"close", ThemeIcon::Close},
            {u8"plus", ThemeIcon::Plus},
            {u8"minus", ThemeIcon::Minus},
            {u8"radio-mark-square", ThemeIcon::RadioMarkSquare},
            {u8"radio-mark-round", ThemeIcon::RadioMarkRound},
        };
        for (const Entry& entry : kNames)
        {
            if (entry.name == name)
            {
                return Optional<ThemeIcon>(entry.icon);
            }
        }
        return {};
    }

    class ThemeIconSet
    {
        // Process-global icon cache, shut down explicitly by UIHost. The glyphs live on the
        // process allocator, like the global job and parser slots.
    public:
        /// Bump when ThemeIcon gains a glyph (Initialize materializes all of them).
        static constexpr usize kGlyphCount = 10;

        // Defined out of line in UiRegistryStateImpl.cpp, so there is one set of shared glyph
        // RefPtrs per process rather than one per shared library.
        [[nodiscard]] static ThemeIconSet& Get();

        /// Materialize the shared glyphs (idempotent). UIHost's constructor calls this before
        /// apps build their themes, so every theme references the shared instances.
        void Initialize()
        {
            if (m_initialized)
            {
                return;
            }
            for (usize i = 0; i < kGlyphCount; ++i)
            {
                m_glyphs[i] = BakedSVGDrawable::FromString(DefaultAllocator(), Svg(static_cast<ThemeIcon>(i)));
            }
            m_initialized = true;
        }

        /// Drop the shared references (sheets holding their own keep instances alive). Clear
        /// baked variants first when the atlas owner is going away; see UIHost.
        void Shutdown()
        {
            for (RefPtr<BakedSVGDrawable>& glyph : m_glyphs)
            {
                glyph = {};
            }
            m_tinted.Clear();
            m_initialized = false;
        }

        [[nodiscard]] bool IsInitialized() const noexcept { return m_initialized; }

        /// The shared baked glyph when the set is live, else a fresh unbaked SVGDrawable
        /// (headless runs and tests). Themes call this instead of
        /// SVGDrawable::FromString(DefaultAllocator(), ThemeIcons::...).
        [[nodiscard]] static RefPtr<Drawable> Acquire(ThemeIcon icon)
        {
            ThemeIconSet& set = Get();
            if (set.m_initialized)
            {
                if (const RefPtr<BakedSVGDrawable>& shared = set.m_glyphs[static_cast<usize>(icon)])
                {
                    return RefPtr<Drawable>(shared.Get());
                }
            }
            RefPtr<SVGDrawable> fallback = SVGDrawable::FromString(DefaultAllocator(), Svg(icon));
            return RefPtr<Drawable>(fallback.Get());
        }

        /// The tinted shared variant (LightTheme and TexturedTheme preset a tint color at parse).
        /// There is one shared instance per (glyph, tint), a handful per theme, materialized on
        /// first acquire and included in the bake pass. An uninitialized set hands out a fresh
        /// unbaked drawable, as above.
        [[nodiscard]] static RefPtr<Drawable> Acquire(ThemeIcon icon, Color tint)
        {
            ThemeIconSet& set = Get();
            if (set.m_initialized)
            {
                for (const TintedGlyph& entry : set.m_tinted)
                {
                    if (entry.icon == icon && entry.tint.r == tint.r && entry.tint.g == tint.g &&
                        entry.tint.b == tint.b && entry.tint.a == tint.a)
                    {
                        return RefPtr<Drawable>(entry.drawable.Get());
                    }
                }
                if (RefPtr<BakedSVGDrawable> made = BakedSVGDrawable::FromString(DefaultAllocator(), Svg(icon)))
                {
                    made->TintColor = Optional<Color>(tint);
                    set.m_tinted.PushBack(TintedGlyph{icon, tint, made});
                    return RefPtr<Drawable>(made.Get());
                }
            }
            RefPtr<SVGDrawable> fallback = SVGDrawable::FromString(DefaultAllocator(), Svg(icon), tint);
            return RefPtr<Drawable>(fallback.Get());
        }

        /// Every live glyph (base and tinted variants), for the host's bake pass.
        void CollectBakeable(Array<BakedSVGDrawable*>& out)
        {
            for (const RefPtr<BakedSVGDrawable>& glyph : m_glyphs)
            {
                if (glyph)
                {
                    out.PushBack(glyph.Get());
                }
            }
            for (const TintedGlyph& entry : m_tinted)
            {
                if (entry.drawable)
                {
                    out.PushBack(entry.drawable.Get());
                }
            }
        }

        /// Detach baked variants. They borrow the baker's atlas, so call this before the atlas
        /// owner dies or before a re-bake at a new scale; glyphs draw as live vectors meanwhile.
        void ClearBakedVariants()
        {
            for (const RefPtr<BakedSVGDrawable>& glyph : m_glyphs)
            {
                if (glyph)
                {
                    glyph->ClearBakedVariants();
                }
            }
            for (const TintedGlyph& entry : m_tinted)
            {
                if (entry.drawable)
                {
                    entry.drawable->ClearBakedVariants();
                }
            }
        }

    private:
        [[nodiscard]] static StringView Svg(ThemeIcon icon)
        {
            switch (icon)
            {
            case ThemeIcon::Checkmark:
                return ThemeIcons::Checkmark();
            case ThemeIcon::ArrowDown:
                return ThemeIcons::ArrowDown();
            case ThemeIcon::ArrowUp:
                return ThemeIcons::ArrowUp();
            case ThemeIcon::ChevronRight:
                return ThemeIcons::ChevronRight();
            case ThemeIcon::ChevronDown:
                return ThemeIcons::ChevronDown();
            case ThemeIcon::Close:
                return ThemeIcons::Close();
            case ThemeIcon::Plus:
                return ThemeIcons::Plus();
            case ThemeIcon::Minus:
                return ThemeIcons::Minus();
            case ThemeIcon::RadioMarkSquare:
                return ThemeIcons::RadioMarkSquare();
            case ThemeIcon::RadioMarkRound:
                return ThemeIcons::RadioMarkRound();
            }
            return {};
        }

        struct TintedGlyph
        {
            ThemeIcon icon{};
            Color tint{};
            RefPtr<BakedSVGDrawable> drawable;
        };

        RefPtr<BakedSVGDrawable> m_glyphs[kGlyphCount];
        Array<TintedGlyph> m_tinted;
        bool m_initialized = false;
    };
}
