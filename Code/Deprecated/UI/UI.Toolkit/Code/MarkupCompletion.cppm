// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI Toolkit - :markup_completion partition
//
// Completion provider for foundation.ui markup documents, fed by the MarkupRegistry's vocabulary
// (the same tables the loader validates against). It offers element names right after `<` or
// `</`, and attribute names inside a tag: the element's registered properties plus the union of
// all layout params, since which ones apply depends on the parent container. Context is
// line-local; plain text between tags offers nothing.
//
// It lives in the toolkit rather than an editor page because MarkupRegistry is part of
// foundation.ui itself, as generic as the XmlLexer. Script-language providers differ: their
// knowledge belongs to the modules that own each language.

module;
#include "Core/Prelude.h"

export module foundation.ui.toolkit:markup_completion;

import foundation.core;
import foundation.ui;
import :code_document;
import :code_edit_view;

using namespace foundation::core;

export namespace foundation::ui::toolkit
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
