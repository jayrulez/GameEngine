// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Fonts - foundation.fonts:font_manager partition
//
// Thread-safe font cache keyed by (path, pixel height). Loads source-format
// fonts through the parser/baker factories on a miss, caches the resulting
// CachedFont, and hands out ref-counted shared instances.

module;
#include "Core/Prelude.h"

export module foundation.fonts:font_manager;

import foundation.core;
import :types;
import :interfaces;
import :backend_interfaces;
import :factories;

using namespace foundation::core;

export namespace foundation::fonts
{
    // Caches fonts at multiple sizes; thread-safe for concurrent access.
    class FontManager
    {
    public:
        // The allocator (required; the owner decides) backs every loaded font,
        // atlas, shaper aggregate, and the cache entries.
        explicit FontManager(IAllocator& allocator,
                             FontLoadOptions defaultOptions = FontLoadOptions::Default())
            : m_allocator(&allocator), m_defaultOptions(defaultOptions)
        {
        }

        ~FontManager() { DeleteCache(); }

        FontManager(const FontManager&) = delete;
        FontManager& operator=(const FontManager&) = delete;

        // Set a factory function for creating text shapers attached to loaded
        // fonts. Replaces any factory currently set.
        void SetShaperFactory(Function<ITextShaper*()> factory) { m_shaperFactory = Move(factory); }

        // Get or load a font at the given pixel height. Returns null on failure.
        [[nodiscard]] CachedFont* GetFont(StringView path, f32 pixelHeight)
        {
            const FontCacheKey lookupKey(path, pixelHeight);

            // Check cache first.
            {
                ScopedLock<Mutex> lock(m_lock);
                if (CachedFont** cached = m_cache.Find(lookupKey))
                {
                    (*cached)->refCount++;
                    return *cached;
                }
            }

            // Load outside the lock so other threads aren't blocked.
            FontLoadOptions options = m_defaultOptions;
            options.pixelHeight = pixelHeight;

            Result<IFont*, FontLoadResult> parsed =
                FontParserFactory::ParseFromFile(path, options, *m_allocator);
            if (!parsed.HasValue())
                return nullptr;
            IFont* font = parsed.Value();

            Result<IFontAtlas*, FontLoadResult> baked =
                FontAtlasBakerFactory::Bake(*font, options, *m_allocator);
            if (!baked.HasValue())
            {
                m_allocator->Delete(font);
                return nullptr;
            }
            IFontAtlas* atlas = baked.Value();

            ITextShaper* shaper = m_shaperFactory ? m_shaperFactory() : nullptr;

            CachedFont* entry = m_allocator->New<CachedFont>(*m_allocator, font, atlas, shaper);

            // Insert with a double-check: another thread may have loaded it.
            {
                ScopedLock<Mutex> lock(m_lock);
                if (CachedFont** existing = m_cache.Find(lookupKey))
                {
                    m_allocator->Delete(entry); // frees font/atlas/shaper too
                    (*existing)->refCount++;
                    return *existing;
                }
                m_cache.InsertOrAssign(FontCacheKey(path, pixelHeight), entry);
                return entry;
            }
        }

        // Get a font at the default pixel height from the options.
        [[nodiscard]] CachedFont* GetFont(StringView path)
        {
            return GetFont(path, m_defaultOptions.pixelHeight);
        }

        // Release a reference. The font stays cached for potential reuse.
        void ReleaseFont(CachedFont* font)
        {
            if (font == nullptr)
                return;
            ScopedLock<Mutex> lock(m_lock);
            font->refCount--;
        }

        // Evict and free any cached fonts with no outstanding references.
        void ClearUnused()
        {
            ScopedLock<Mutex> lock(m_lock);
            Array<FontCacheKey> toRemove;
            for (const auto& entry : m_cache)
                if (entry.value->refCount <= 0)
                    toRemove.PushBack(entry.key);

            for (const FontCacheKey& key : toRemove)
            {
                if (CachedFont** found = m_cache.Find(key))
                    m_allocator->Delete(*found);
                m_cache.Remove(key);
            }
        }

        // Evict and free everything (use with caution).
        void ClearAll()
        {
            ScopedLock<Mutex> lock(m_lock);
            for (const auto& entry : m_cache)
                m_allocator->Delete(entry.value);
            m_cache.Clear();
        }

        [[nodiscard]] usize CacheCount()
        {
            ScopedLock<Mutex> lock(m_lock);
            return m_cache.Size();
        }

        [[nodiscard]] bool IsCached(StringView path, f32 pixelHeight)
        {
            const FontCacheKey key(path, pixelHeight);
            ScopedLock<Mutex> lock(m_lock);
            return m_cache.Contains(key);
        }

    private:
        void DeleteCache()
        {
            for (const auto& entry : m_cache)
                m_allocator->Delete(entry.value);
            m_cache.Clear();
        }

        Mutex m_lock;
        HashMap<FontCacheKey, CachedFont*> m_cache;
        IAllocator* m_allocator;
        FontLoadOptions m_defaultOptions;
        Function<ITextShaper*()> m_shaperFactory;
    };
}
