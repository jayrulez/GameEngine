// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI - :theme_icon_set partition.
//
// The SHARED, bake-ready chrome glyph set behind the built-in themes. The audit found the
// Godot-style crisp icons were EDITOR-ONLY by wiring, not by design: the editor baked its own
// icon set into the pixel-snapped atlas while the themes' chrome glyphs (close/chevrons/
// checkmark...) stayed live-vector renders - the editor's dock close button was crisp, the
// same button in UISandbox was not.
//
// Themes acquire glyphs through Acquire(): when a host has INITIALIZED the set (UIHost does,
// in its constructor), every theme shares ONE BakedSVGDrawable per glyph and the host bakes
// them (UIHost::BakeThemeIcons -> the supersample+downsample atlas recipe); until the bake
// lands they draw as live vectors, so init/bake ordering is free. Headless and test contexts
// never initialize the set - Acquire hands out fresh unbaked SVGDrawables, exactly the old
// behavior, and no static-lifetime teardown games exist (Shutdown is explicit, from UIHost).

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

    /// The builtin glyph's kebab-case stylesheet name -> enum (the `svg(name)` factory's
    /// fallback vocabulary; also used by the UI cook to validate names). None = not builtin.
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
        // Process-global icon cache (explicit Shutdown from UIHost) - glyphs live on
        // the process allocator by design, like the global job/parser slots.
    public:
        /// Tripwire: bump when ThemeIcon gains a glyph (Initialize materializes ALL of them).
        static constexpr usize kGlyphCount = 10;

        // NON-inline (UiRegistryStateImpl.cpp): shared glyph RefPtrs, one set per
        // process (shared-libraries.md rendezvous rule).
        [[nodiscard]] static ThemeIconSet& Get();

        /// Materialize the shared glyphs (idempotent). Called by UIHost's constructor - BEFORE
        /// apps build their themes - so every theme references the shared instances.
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

        /// Drop the shared refs (sheets holding their own refs keep instances alive). Baked
        /// variants must be cleared FIRST when the atlas owner is going away - see UIHost.
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
        /// (headless/tests - unchanged behavior). Themes call this instead of
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

        /// The TINTED shared variant (LightTheme/TexturedTheme preset a tint color at parse).
        /// One shared instance per (glyph, tint) - a handful per theme - materialized on first
        /// acquire and included in the bake pass. Uninitialized set: fresh unbaked, as above.
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

        /// Every live glyph (base + tinted variants), for the host's bake pass.
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

        /// Detach baked variants (they BORROW the baker's atlas - call before the atlas owner
        /// dies or before a re-bake at a new scale; glyphs fall back to live vector meanwhile).
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
