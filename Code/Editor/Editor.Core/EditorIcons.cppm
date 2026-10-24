// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell
// Editor::Core - :editor_icons partition.
//
// A type's icon (Documentation/Specs/editor-gui.md, 3.2; catalog 2.7), found by asking the type,
// never from a list of type names: the icon named after it (Editor/Icons/Types/<Name>.svg), else
// its base's, up the chain, else a generic one the caller names (Component, Asset). Its tint is a
// class a sheet styles, from the category the type declares (the reflected `category` attribute,
// its own or its nearest base's): "Rendering" is `cat-rendering`. A type with no category keeps
// the plain icon colour.
module;
#include "Core/Prelude.h"

export module editor.core:editor_icons;

import foundation.core;

using namespace foundation::core;

export namespace editor
{
    /// A type's icon as a list row shows it: the image's source and the class that tints it.
    struct TypeIcon
    {
        String Source; ///< "Editor/Icons/Types/MeshComponent.svg"
        String Class;  ///< "cat-rendering", or empty: no category
    };

    class EditorIcons
    {
    public:
        /// The folder type icons are in, relative to the editor's resources.
        static constexpr const char8_t* kTypeIconFolder = u8"Editor/Icons/Types/";

        /// `exists` says whether a file is there (the editor's resources answer it).
        explicit EditorIcons(IAllocator& allocator, Function<bool(StringView path)> exists)
            : m_exists(Move(exists)), m_cache(allocator)
        {
        }

        /// The icon of `type`: its own, else its nearest base's, else `fallback`'s (a generic
        /// type name: Component, Asset), else none; its class from the nearest category.
        [[nodiscard]] TypeIcon For(const TypeInfo& type, StringView fallback)
        {
            const u64 key = reinterpret_cast<u64>(&type) ^ Hash<StringView>{}(fallback);
            if (const TypeIcon* known = m_cache.Find(key))
                return *known;
            TypeIcon icon;
            for (const TypeInfo* t = &type; t != nullptr && icon.Source.IsEmpty(); t = t->base)
                if (t->name != nullptr)
                {
                    String path = IconPath(StringView(reinterpret_cast<const utf8char*>(t->name)));
                    if (m_exists && m_exists(path.AsView()))
                        icon.Source = Move(path);
                }
            if (icon.Source.IsEmpty() && !fallback.IsEmpty())
            {
                String path = IconPath(fallback);
                if (m_exists && m_exists(path.AsView()))
                    icon.Source = Move(path);
            }
            for (const TypeInfo* t = &type; t != nullptr && icon.Class.IsEmpty(); t = t->base)
                if (const Variant* category = FindAttribute(*t, "category"))
                    if (const String* name = category->TryGet<String>())
                        icon.Class = CategoryClass(name->AsView());
            m_cache.InsertOrAssign(key, icon);
            return icon;
        }

        /// Forgets what it found (the icon files changed on disk).
        void Forget() { m_cache.Clear(); }

        /// The class a category tints by: "cat-" and its letters and digits in lower case
        /// ("Rendering": cat-rendering; "Visual Effects": cat-visualeffects).
        [[nodiscard]] static String CategoryClass(StringView category)
        {
            String out(u8"cat-");
            for (const utf8char c : category)
            {
                if (c >= u8'A' && c <= u8'Z')
                    out.PushBack(static_cast<utf8char>(c - u8'A' + u8'a'));
                else if ((c >= u8'a' && c <= u8'z') || (c >= u8'0' && c <= u8'9'))
                    out.PushBack(c);
            }
            return out.Size() > 4 ? out : String{};
        }

    private:
        [[nodiscard]] static String IconPath(StringView typeName)
        {
            String path(kTypeIconFolder);
            path += typeName;
            path += u8".svg";
            return path;
        }

        Function<bool(StringView)> m_exists;
        HashMap<u64, TypeIcon> m_cache; // by the type (and the fallback asked with it)
    };
}
