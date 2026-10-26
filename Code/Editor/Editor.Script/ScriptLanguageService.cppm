// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Script - :script_language_service partition.
//
// ScriptLanguageService: a script page's completion and hover for its code editor
// (Documentation/Specs/editor-gui.md, 5.4, layer 6, part 3), over the page's API surface (what the
// runtime binds for its language) and its language's words. After `x.` or `x::` it offers the
// members of what x is (a type or namespace by name, or the type a variable named x is declared
// with in the script); elsewhere the language's words, the bound types and namespaces (an
// editor-only one marked) and the script's own names. Hover says what a name is, or, while a Game
// run is paused in this script, its value (the page's probe). Adapted from the GUISandbox's Code
// page, which proved it; that copy stays the sandbox's.

module;
#include "Core/Prelude.h"

export module editor.script:script_language_service;

import foundation.core;
import foundation.script;
import foundation.gui.code;
import :script_api;

using namespace foundation::core;

export namespace editor
{
    namespace gui = foundation::gui;
    namespace script = foundation::script;

    class ScriptLanguageService final : public gui::ICodeLanguageService
    {
    public:
        /// Over `surface` (the page's, outliving this) and the words of `language` (may be null).
        ScriptLanguageService(const ScriptApiSurface& surface, const gui::CodeLanguage* language) : m_surface(&surface), m_language(language) {}

        /// While a run is paused in the page's script, the value of a name (empty: none); the page's.
        void SetValueProbe(Function<String(StringView name)> probe) { m_probe = Move(probe); }

        [[nodiscard]] StringView CompletionTriggers() const override { return u8".:"; }

        void Complete(const gui::CodeDocument& document, const gui::CodeCompletionRequest& request, Array<gui::CodeCompletionItem>& out) override
        {
            const StringView line = document.LineText(request.WordStart.Line);
            const u32 start = request.WordStart.Column;
            // After `x.`, `x:` or `x::`, the members of what x is: a type's (or a namespace's)
            // statics after its name, a value's own after a variable's (as every language reads
            // it: AngelScript's `Float3::Dot`, Luau's `Float3.Dot`, both a value's `v.x`); else the
            // words a script may begin with.
            const StringView qualifier = QualifierBefore(line, start);
            if (!qualifier.IsEmpty())
            {
                bool statics = false;
                if (const script::ScriptApiType* type = Resolve(document, qualifier, statics))
                    AddMembers(*type, statics, out);
                return;
            }
            if (start > 0 && line[start - 1] == u8'.')
                return; // a member of something we cannot name
            AddLanguageWords(out);
            for (const script::ScriptApiType& type : m_surface->Types())
                out.PushBack(gui::CodeCompletionItem{type.scriptName, type.isNamespace ? gui::CodeCompletionKind::Module : gui::CodeCompletionKind::Type,
                                                     String(IsEditorOnlyBinding(type.typeId) ? (type.isNamespace ? StringView(u8"namespace [editor]") : StringView(u8"type [editor]"))
                                                                                             : (type.isNamespace ? StringView(u8"namespace") : StringView(u8"type"))),
                                                     String{}});
            AddDocumentWords(document, request, out);
        }

        bool Hover(const gui::CodeDocument& document, gui::CodePosition at, String& out) override
        {
            if (at.Line >= document.LineCount())
                return false;
            const StringView line = document.LineText(at.Line);
            u32 begin = at.Column, end = at.Column;
            while (begin > 0 && IsWordByte(line[begin - 1]))
                --begin;
            while (end < line.Size() && IsWordByte(line[end]))
                ++end;
            if (begin == end)
                return false;
            const StringView word = line.SubStr(begin, end - begin);
            // Paused in this script: what the name holds.
            if (m_probe)
            {
                out = m_probe(word);
                if (!out.IsEmpty())
                    return true;
            }
            const StringView qualifier = QualifierBefore(line, begin);
            if (!qualifier.IsEmpty())
            {
                bool statics = false;
                const script::ScriptApiType* type = Resolve(document, qualifier, statics);
                if (type == nullptr)
                    return false;
                usize shown = 0;
                for (const script::ScriptApiMember& member : type->members)
                    if (member.name.AsView() == word && shown < 3)
                    {
                        if (shown++ > 0)
                            out += u8"\n";
                        out += Format(u8"{}.{}", type->scriptName, member.signature.IsEmpty() ? member.name : member.signature);
                    }
                return shown > 0;
            }
            if (const script::ScriptApiType* type = m_surface->FindType(word))
            {
                out = Format(u8"{} {} ({} members){}", type->isNamespace ? StringView(u8"namespace") : StringView(u8"type"), type->scriptName,
                             type->members.Size(), IsEditorOnlyBinding(type->typeId) ? StringView(u8", the editor's alone: not in a built game") : StringView{});
                return true;
            }
            return false;
        }

    private:
        [[nodiscard]] static bool IsWordByte(char8_t c) noexcept
        {
            return (c >= u8'a' && c <= u8'z') || (c >= u8'A' && c <= u8'Z') || (c >= u8'0' && c <= u8'9') || c == u8'_' || c >= 0x80;
        }

        /// The name before `x.`, `x:` or `x::` ending at `start`; empty when none.
        [[nodiscard]] static StringView QualifierBefore(StringView line, u32 start)
        {
            u32 end = start;
            if (end >= 2 && line[end - 1] == u8':' && line[end - 2] == u8':')
                end -= 2;
            else if (end >= 1 && (line[end - 1] == u8'.' || line[end - 1] == u8':'))
                end -= 1;
            else
                return {};
            u32 begin = end;
            while (begin > 0 && IsWordByte(line[begin - 1]))
                --begin;
            return line.SubStr(begin, end - begin);
        }

        /// A type or namespace by its name (`statics`), or the type a variable of that name is
        /// declared with in the script (`Node@ card`, `Float3 at`), the last declaration found.
        [[nodiscard]] const script::ScriptApiType* Resolve(const gui::CodeDocument& document, StringView name, bool& statics) const
        {
            statics = false;
            if (const script::ScriptApiType* type = m_surface->FindType(name))
            {
                statics = true;
                return type;
            }
            const script::ScriptApiType* found = nullptr;
            for (u32 l = 0; l < document.LineCount(); ++l)
            {
                const StringView text = document.LineText(l);
                for (usize at = 0; at + name.Size() <= text.Size(); ++at)
                {
                    if (text.SubStr(at, name.Size()) != name || (at > 0 && IsWordByte(text[at - 1])) ||
                        (at + name.Size() < text.Size() && IsWordByte(text[at + name.Size()])))
                        continue;
                    // Back over spaces, a handle or reference mark, and spaces, to the type's name.
                    usize p = at;
                    while (p > 0 && text[p - 1] == u8' ')
                        --p;
                    while (p > 0 && (text[p - 1] == u8'@' || text[p - 1] == u8'&'))
                        --p;
                    while (p > 0 && text[p - 1] == u8' ')
                        --p;
                    usize q = p;
                    while (q > 0 && IsWordByte(text[q - 1]))
                        --q;
                    if (q == p || p == at)
                        continue;
                    if (const script::ScriptApiType* type = m_surface->FindType(text.SubStr(q, p - q)))
                        found = type;
                }
            }
            return found;
        }

        static void AddMembers(const script::ScriptApiType& type, bool statics, Array<gui::CodeCompletionItem>& out)
        {
            for (const script::ScriptApiMember& member : type.members)
            {
                if (member.kind == script::ScriptApiMemberKind::Operator || (!type.isNamespace && member.isStatic != statics))
                    continue;
                bool seen = false; // an overload shows once, with its first signature
                for (const gui::CodeCompletionItem& item : out)
                    seen = seen || item.Label == member.name;
                if (seen)
                    continue;
                gui::CodeCompletionKind kind = gui::CodeCompletionKind::Method;
                if (member.kind == script::ScriptApiMemberKind::Property)
                    kind = gui::CodeCompletionKind::Property;
                else if (member.kind == script::ScriptApiMemberKind::Constant)
                    kind = gui::CodeCompletionKind::Constant;
                else if (type.isNamespace || member.isStatic)
                    kind = gui::CodeCompletionKind::Function;
                out.PushBack(gui::CodeCompletionItem{member.name, kind, member.signature, String{}});
            }
        }

        void AddLanguageWords(Array<gui::CodeCompletionItem>& out) const
        {
            if (m_language == nullptr)
                return;
            for (const gui::CodeWords& words : m_language->Words)
            {
                gui::CodeCompletionKind kind = gui::CodeCompletionKind::Keyword;
                if (words.Token == gui::CodeToken::Type)
                    kind = gui::CodeCompletionKind::Type;
                else if (words.Token == gui::CodeToken::Constant)
                    kind = gui::CodeCompletionKind::Constant;
                const StringView list = words.List.AsView();
                usize begin = 0;
                for (usize i = 0; i <= list.Size(); ++i)
                    if (i == list.Size() || list[i] == u8' ')
                    {
                        if (i > begin)
                            out.PushBack(gui::CodeCompletionItem{String(list.SubStr(begin, i - begin)), kind, String{}, String{}});
                        begin = i + 1;
                    }
            }
        }

        /// The script's own names (its functions, variables), as an editor offers the words of the
        /// file: each once, not the word being typed, not what the list holds already.
        static void AddDocumentWords(const gui::CodeDocument& document, const gui::CodeCompletionRequest& request, Array<gui::CodeCompletionItem>& out)
        {
            for (u32 l = 0; l < document.LineCount(); ++l)
            {
                const StringView text = document.LineText(l);
                usize at = 0;
                while (at < text.Size())
                {
                    if (!IsWordByte(text[at]))
                    {
                        ++at;
                        continue;
                    }
                    usize end = at;
                    while (end < text.Size() && IsWordByte(text[end]))
                        ++end;
                    const bool typing = l == request.WordStart.Line && at == request.WordStart.Column;
                    if (end - at >= 3 && !typing && !(text[at] >= u8'0' && text[at] <= u8'9'))
                    {
                        const StringView word = text.SubStr(at, end - at);
                        bool seen = false;
                        for (const gui::CodeCompletionItem& item : out)
                            seen = seen || item.Label.AsView() == word;
                        if (!seen)
                            out.PushBack(gui::CodeCompletionItem{String(word), gui::CodeCompletionKind::Text, String{}, String{}});
                    }
                    at = end;
                }
            }
        }

        const ScriptApiSurface* m_surface;
        const gui::CodeLanguage* m_language;
        Function<String(StringView)> m_probe;
    };
}
