// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor App - :code_languages partition
//
// CodeLanguages: the code editors' languages by id, for the code pages. A language is a data file
// in Data/Editor/Code, shared by the editor and the GUI sandbox, named by its id and read through
// the XML serialization the first time a page asks for it (a script asset's language, "hlsl",
// "css"). No list of languages is written anywhere; adding one means adding a file. The editor's
// application owns one instance and hands it to the pages with their services.

module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"

export module editor.app:code_languages;

import foundation.core;
import foundation.xml;
import foundation.xml.serialization;
import foundation.gui.code;

using namespace foundation::core;

export namespace editor::app
{
    class CodeLanguages
    {
    public:
        /// The folder of language files under the data root, one <id>.xml per language.
        static constexpr StringView kFolder = u8"Editor/Code/";

        /// `read` gives a file's text by its path under the data root (false: it is not there).
        CodeLanguages(IAllocator& allocator, Function<bool(StringView path, String& out)> read)
            : m_allocator(&allocator), m_read(Move(read)), m_entries(allocator)
        {
        }
        CodeLanguages(const CodeLanguages&) = delete;
        CodeLanguages& operator=(const CodeLanguages&) = delete;

        /// The language `id` names, read the first time it is asked for and then cached; null when
        /// it has no file or the file fails to read (logged once).
        [[nodiscard]] const foundation::gui::CodeLanguage* Find(StringView id)
        {
            if (id.IsEmpty())
                return nullptr;
            for (const Entry& entry : m_entries)
                if (entry.id.AsView() == id)
                    return entry.language.Get();
            Entry entry{String(id), Read(id)};
            const foundation::gui::CodeLanguage* found = entry.language.Get();
            m_entries.PushBack(Move(entry));
            return found;
        }

    private:
        struct Entry
        {
            String id;
            UniquePtr<foundation::gui::CodeLanguage> language; // null: no file, or it failed to read
        };

        [[nodiscard]] UniquePtr<foundation::gui::CodeLanguage> Read(StringView id) const
        {
            const String path = Format(u8"{}{}.xml", kFolder, id);
            String text;
            if (!m_read || !m_read(path.AsView(), text))
            {
                LOG_WARNING(u8"Editor", u8"no code language '{}' ({} is not there): its code shows uncoloured", id, path);
                return {};
            }
            foundation::xml::XmlDocument doc(*m_allocator);
            auto language = MakeUnique<foundation::gui::CodeLanguage>(*m_allocator);
            if (doc.Parse(text.AsView()) == foundation::xml::XmlResult::Ok)
            {
                foundation::xml::XmlSerializer reader(doc);
                foundation::gui::Serialize(reader, *language);
                if (reader.IsOk() && !language->Contexts.IsEmpty())
                    return language;
            }
            LOG_WARNING(u8"Editor", u8"the code language {} does not read: its code shows uncoloured", path);
            return {};
        }

        IAllocator* m_allocator;
        Function<bool(StringView path, String& out)> m_read;
        Array<Entry> m_entries;
    };
}
