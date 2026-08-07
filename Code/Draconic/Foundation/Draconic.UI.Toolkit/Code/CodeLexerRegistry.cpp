// Draconic UI Toolkit - CodeLexerRegistry implementation (declared in :code_lexer).

module;
#include "Draconic.Foundation/Prelude.h"

module draconic.ui.toolkit;

import draconic.foundation;

using namespace draconic::foundation;

namespace draconic::ui::toolkit
{
    namespace
    {
        [[nodiscard]] bool EqualsIgnoreAsciiCase(StringView a, StringView b) noexcept
        {
            if (a.Size() != b.Size())
            {
                return false;
            }
            for (usize i = 0; i < a.Size(); ++i)
            {
                char8_t x = a[i];
                char8_t y = b[i];
                if (x >= u8'A' && x <= u8'Z')
                {
                    x = static_cast<char8_t>(x + 32);
                }
                if (y >= u8'A' && y <= u8'Z')
                {
                    y = static_cast<char8_t>(y + 32);
                }
                if (x != y)
                {
                    return false;
                }
            }
            return true;
        }
    }

    CodeLexerRegistry& CodeLexerRegistry::Get()
    {
        static CodeLexerRegistry instance;
        return instance;
    }

    void CodeLexerRegistry::Register(StringView languageId,
                                     Function<UniquePtr<ICodeLexer>()> factory)
    {
        for (usize i = 0; i < m_entries.Size(); ++i)
        {
            if (EqualsIgnoreAsciiCase(m_entries[i].id.AsView(), languageId))
            {
                m_entries[i].factory = Move(factory);
                return;
            }
        }
        m_entries.PushBack(Entry{String(languageId), Move(factory)});
    }

    UniquePtr<ICodeLexer> CodeLexerRegistry::Create(StringView languageId) const
    {
        for (usize i = 0; i < m_entries.Size(); ++i)
        {
            if (EqualsIgnoreAsciiCase(m_entries[i].id.AsView(), languageId))
            {
                return m_entries[i].factory();
            }
        }
        return UniquePtr<ICodeLexer>();
    }
}
