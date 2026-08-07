// Draconic UI Toolkit - :markup_completion partition
//
// Completion provider for draconic.ui MARKUP documents, fed by the MarkupRegistry's real
// vocabulary (the same tables the loader validates against): element names right after
// `<` / `</`, attribute names inside a tag (the element's registered properties + the
// layout-param union - which layout params apply depends on the parent container, so the
// union is offered). Line-local context; plain text between tags offers nothing.
//
// Lives in toolkit (not an editor page): MarkupRegistry is part of draconic.ui itself, the
// same generic tier as the XmlLexer - unlike script-language providers, whose knowledge
// belongs to the per-language editor-UI modules.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.ui.toolkit:markup_completion;

import draconic.foundation;
import draconic.ui;
import :code_document;
import :code_edit_view;

using namespace draconic::foundation;

export namespace draconic::ui::toolkit
{

    class MarkupCompletionProvider final : public ICompletionProvider
    {
    public:
        void Collect(const CodeDocument& document, CodePosition cursor, StringView prefix,
                     Array<CompletionCandidate>& out) override
        {
            const StringView line = document.Line(cursor.line);
            const i32 anchorColumn = cursor.column - static_cast<i32>(Utf8Length(prefix));
            const usize anchorByte = document.ColumnToByte(cursor.line, anchorColumn);

            // Line-local tag context: the last unclosed '<' before the prefix.
            i32 tagOpen = -1;
            for (usize i = 0; i < anchorByte && i < line.Size(); ++i)
            {
                if (line[i] == u8'<')
                {
                    tagOpen = static_cast<i32>(i);
                }
                else if (line[i] == u8'>')
                {
                    tagOpen = -1;
                }
            }
            if (tagOpen < 0)
            {
                return; // plain text between tags
            }

            usize nameBegin = static_cast<usize>(tagOpen) + 1;
            if (nameBegin < line.Size() && line[nameBegin] == u8'/')
            {
                ++nameBegin;
            }
            usize nameEnd = nameBegin;
            while (nameEnd < line.Size() && IsNameChar(line[nameEnd]))
            {
                ++nameEnd;
            }

            Array<String> names;
            if (anchorByte >= nameBegin && anchorByte <= nameEnd)
            {
                // Typing the element name itself.
                MarkupRegistry::CollectElementNames(names);
            }
            else
            {
                // Past the name: attribute position for this element.
                MarkupRegistry::CollectAttributeNames(
                    line.SubStr(nameBegin, nameEnd - nameBegin), names);
            }
            for (usize i = 0; i < names.Size(); ++i)
            {
                out.PushBack(CompletionCandidate{String(names[i].AsView()),
                                                 String(names[i].AsView())});
            }
        }

    private:
        [[nodiscard]] static bool IsNameChar(char8_t c) noexcept
        {
            return (c >= u8'a' && c <= u8'z') || (c >= u8'A' && c <= u8'Z') ||
                   (c >= u8'0' && c <= u8'9') || c == u8'_' || c == u8':' || c == u8'-';
        }
    };
}
