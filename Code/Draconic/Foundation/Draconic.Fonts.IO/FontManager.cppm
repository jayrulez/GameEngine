// Draconic::FontsIO - draconic.fonts.io:manager partition
//
// Thread-safe font cache keyed by (path, pixel height). Loads source-format
// fonts through the parser/baker factories on a miss, caches the resulting
// CachedFont, and hands out ref-counted shared instances. Ported from
// Sedulous.Fonts.IO (FontManager.bf); Beef's Monitor + delegate become a
// Mutex/ScopedLock + Function.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.fonts.io:manager;

import draconic.foundation;
import draconic.fonts;
import :factories;

using namespace draconic::foundation;

export namespace draconic::fonts
{
    // Caches fonts at multiple sizes; thread-safe for concurrent access.
    class FontManager
    {
    public:
        explicit FontManager(FontLoadOptions defaultOptions = FontLoadOptions::Default())
            : m_defaultOptions(defaultOptions)
        {
        }

        ~FontManager() { DeleteCache(); }

        FontManager(const FontManager&) = delete;
        FontManager& operator=(const FontManager&) = delete;

        // Set a factory function for creating text shapers attached to loaded
        // fonts. Replaces any previously set factory.
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

            Result<IFont*, FontLoadResult> parsed = FontParserFactory::ParseFromFile(path, options);
            if (!parsed.HasValue())
                return nullptr;
            IFont* font = parsed.Value();

            Result<IFontAtlas*, FontLoadResult> baked = FontAtlasBakerFactory::Bake(*font, options);
            if (!baked.HasValue())
            {
                DefaultAllocator().Delete(font);
                return nullptr;
            }
            IFontAtlas* atlas = baked.Value();

            ITextShaper* shaper = m_shaperFactory ? m_shaperFactory() : nullptr;

            CachedFont* entry = DefaultAllocator().New<CachedFont>(font, atlas, shaper);

            // Insert with a double-check: another thread may have loaded it.
            {
                ScopedLock<Mutex> lock(m_lock);
                if (CachedFont** existing = m_cache.Find(lookupKey))
                {
                    DefaultAllocator().Delete(entry); // frees font/atlas/shaper too
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

        // Evict + free any cached fonts with no outstanding references.
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
                    DefaultAllocator().Delete(*found);
                m_cache.Remove(key);
            }
        }

        // Evict + free everything (use with caution).
        void ClearAll()
        {
            ScopedLock<Mutex> lock(m_lock);
            for (const auto& entry : m_cache)
                DefaultAllocator().Delete(entry.value);
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
                DefaultAllocator().Delete(entry.value);
            m_cache.Clear();
        }

        Mutex m_lock;
        HashMap<FontCacheKey, CachedFont*> m_cache;
        FontLoadOptions m_defaultOptions;
        Function<ITextShaper*()> m_shaperFactory;
    };
}
