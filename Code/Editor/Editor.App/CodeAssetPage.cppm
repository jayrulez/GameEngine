// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor App - :code_asset_page partition
//
// CodeAssetPage: what a page over an asset written as code does whatever its code is
// (Documentation/Specs/editor-gui.md, 5.4, layer 6): a script, a shader, a GUI document or sheet.
// The asset names its source file (under the project's Sources); the page reads it into the code
// editor of its markup (`page-code`), coloured by the language the page names (CodeLanguages), and
// checks it a moment after each edit, the problems (its own the page's to find) shown in the
// problems list of its markup (`page-problems`) and on their lines; choosing one takes the code
// there. Save writes the source when it changed, then the asset, and asks for a cook; a check after
// it says whether the game takes the new code or keeps the last that built.
//
// One history: each step the code's document keeps (typing merges as the document merges it) is a
// command on the page's stack, so the bar's Undo and Redo, the Edit menu and the code's own Ctrl+Z
// walk one history with the page's other edits (a shader's entry points), and Discard Changes
// puts the code back with the rest. A source that is not there is a problem in the list, the code
// read only.

module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"

export module editor.app:code_asset_page;

import foundation.core;
import foundation.content;
import foundation.runtime.client;
import foundation.gui;
import foundation.gui.code;
import pipeline.core; // Asset: its source file
import editor.core;
import :asset_page;
import :asset_object_page;
import :code_languages;
import :problems_list;

using namespace foundation::core;

export namespace editor::app
{
    namespace gui = foundation::gui;

    class CodeAssetPage : public AssetObjectPage
    {
    public:
        /// How long the code rests after an edit before it is checked, in seconds.
        static constexpr f32 kCheckDelay = 0.6f;

        CodeAssetPage(EditorContext& context, AssetPageServices services, foundation::content::Instance& instance, const TypeInfo& type,
                      StringView markupPath)
            : AssetObjectPage(context, Move(services), instance, type, markupPath), m_problemsFound(context.Allocator())
        {
            m_code = Find<gui::CodeEditor>(u8"page-code");
            if (auto* host = Find<gui::UIWidget>(u8"page-problems"))
            {
                m_problems = MakeUnique<ProblemsList>(Allocator(), Allocator(), *host);
                CodeAssetPage* self = this;
                m_problems->SetOnChoose([self](const CodeProblem& problem) { self->GoTo(problem); });
            }
            const auto* asset = ObjectAs<pipeline::Asset>();
            if (asset != nullptr && !asset->fileName.IsEmpty() && context.Project() != nullptr)
            {
                m_fileName = String(asset->fileName.View());
                m_sourcePath = PathJoin(context.Project()->SourcesRoot().AsView(), m_fileName.AsView());
                if (Result<Array<byte>> bytes = ReadFile(m_sourcePath.AsView()); bytes.HasValue())
                {
                    m_saved = String(StringView(reinterpret_cast<const utf8char*>(bytes.Value().Data()), bytes.Value().Size()));
                    m_read = true;
                }
            }
            if (m_code == nullptr)
                return;
            m_code->SetText(m_saved.AsView());
            m_code->SetReadOnly(!m_read);
            gui::CodeDocument& doc = m_code->GetDocument();
            doc.SetUndoLimit(kUndoLimit); // the page's stack holds every step: none dropped under it
            m_steps = doc.UndoCount();
            CodeAssetPage* self = this;
            m_listener = doc.AddChangeListener([self](const gui::CodeChange&) { self->CodeChanged(); });
            // The code's own undo keys walk the page's one history.
            (void)m_code->AddEventListener(gui::EventType::KeyDown, [self](const gui::Event& e)
                                           {
                                               const auto& key = static_cast<const gui::KeyEvent&>(e);
                                               const bool ctrl = (key.Modifiers & gui::KeyModCtrl) != 0;
                                               const bool shift = (key.Modifiers & gui::KeyModShift) != 0;
                                               const auto code = static_cast<gui::KeyCode>(key.KeyCode);
                                               if (!ctrl || (key.Modifiers & (gui::KeyModAlt | gui::KeyModSuper)) != 0)
                                                   return;
                                               if (code == gui::KeyCode::Z && !shift)
                                                   self->Commands().Undo();
                                               else if ((code == gui::KeyCode::Z && shift) || (code == gui::KeyCode::Y && !shift))
                                                   self->Commands().Redo();
                                               else
                                                   return;
                                               e.Handled = true;
                                           },
                                           true);
        }
        ~CodeAssetPage() override
        {
            if (m_code != nullptr)
                m_code->GetDocument().RemoveChangeListener(m_listener);
        }

        void OnUpdate(foundation::runtime::IApplicationHost& host, f32 dt) override
        {
            AssetObjectPage::OnUpdate(host, dt);
            if (m_checkIn > 0.0f)
            {
                m_checkIn -= dt;
                if (m_checkIn <= 0.0f)
                    CheckNow();
            }
        }

        /// The source written when it changed, then the asset; the code checked again, and the
        /// user told whether the game takes it or keeps the last that built.
        [[nodiscard]] Status Save() override
        {
            if (!m_read)
            {
                Context().Notify(NoticeKind::Error, Format(u8"Not saved: {} is not there", m_fileName.IsEmpty() ? StringView(u8"its source") : m_fileName.AsView()).AsView());
                return Status{ErrorCode::NotFound};
            }
            const String text = Text();
            if (text.AsView() != m_saved.AsView())
            {
                const Status written = WriteFile(m_sourcePath.AsView(), Span<const byte>(reinterpret_cast<const byte*>(text.Data()), text.Size()));
                if (!written.IsOk())
                {
                    Context().Notify(NoticeKind::Error, Format(u8"Could not write {}", m_fileName).AsView());
                    return written;
                }
                m_saved = text;
            }
            const Status saved = AssetObjectPage::Save(); // the asset, and a cook asked for
            if (!saved.IsOk())
                return saved;
            CheckNow();
            const bool clean = ErrorCount() == 0;
            Context().Notify(clean ? NoticeKind::Success : NoticeKind::Warning,
                             clean ? StringView(u8"Saved: it cooks again now") : StringView(u8"Saved with errors: the game keeps the last version that built"));
            return saved;
        }

        /// Checks the code now (the page's own check), the problems shown in the list and on their lines.
        void CheckNow()
        {
            m_checkIn = 0.0f;
            m_problemsFound = m_read ? CheckCode(Text().AsView()) : Array<CodeProblem>{};
            if (!m_read)
                m_problemsFound.PushBack(CodeProblem{gui::CodeSeverity::Error,
                                                     m_fileName.IsEmpty() ? String(u8"The asset names no source file")
                                                                          : Format(u8"The source file {} is not there (the code cannot be edited)", m_fileName),
                                                     String{}, 0, 0, false});
            Array<gui::CodeDiagnostic> marks;
            for (const CodeProblem& p : m_problemsFound)
                if (p.file.IsEmpty() && m_read && m_code != nullptr)
                    marks.PushBack(gui::CodeDiagnostic{RangeOf(p), p.severity, p.message});
            if (m_code != nullptr)
                m_code->SetDiagnostics(Span<const gui::CodeDiagnostic>(marks.Data(), marks.Size()));
            if (m_problems)
            {
                m_problems->SetClean(m_read ? CleanSummary() : String{});
                m_problems->Show(Span<const CodeProblem>(m_problemsFound.Data(), m_problemsFound.Size()));
            }
            OnCodeChecked();
        }

        /// Takes the code to `problem` (its own code's: an include's has no place here).
        void GoTo(const CodeProblem& problem)
        {
            if (m_code == nullptr || !problem.file.IsEmpty())
                return;
            m_code->SetCaret(gui::CodePosition{problem.line, problem.atColumn ? problem.column : 0});
            m_code->ScrollToCaret();
            m_code->RequestFocus();
        }

        // === For tests ===
        [[nodiscard]] gui::CodeEditor* Code() const noexcept { return m_code; }
        [[nodiscard]] ProblemsList* Problems() const noexcept { return m_problems.Get(); }
        [[nodiscard]] String Text() const { return m_code != nullptr ? m_code->GetText() : m_saved; }
        [[nodiscard]] bool SourceRead() const noexcept { return m_read; }
        [[nodiscard]] StringView SourceFileName() const noexcept { return m_fileName.AsView(); }
        [[nodiscard]] Span<const CodeProblem> Found() const noexcept { return Span<const CodeProblem>(m_problemsFound.Data(), m_problemsFound.Size()); }
        [[nodiscard]] usize ErrorCount() const
        {
            usize errors = 0;
            for (const CodeProblem& p : m_problemsFound)
                errors += p.severity == gui::CodeSeverity::Error ? 1u : 0u;
            return errors;
        }

    protected:
        /// Colours the code as `languageId` (the derived page's, once it knows it) and checks it,
        /// so the page opens with its problems.
        void OpenCode(StringView languageId)
        {
            if (m_code != nullptr && Services().languages != nullptr)
                m_code->SetLanguage(Services().languages->Find(languageId));
            CheckNow();
        }
        /// What the page's check of `text` finds (a compile's errors).
        [[nodiscard]] virtual Array<CodeProblem> CheckCode(StringView text) = 0;
        /// After each check (a status, a preview following the code).
        virtual void OnCodeChecked() {}
        /// What the problems line says when the check found none (empty: "No problems"), asked
        /// after CheckCode.
        [[nodiscard]] virtual String CleanSummary() const { return String{}; }
        [[nodiscard]] StringView SourcePath() const noexcept { return m_sourcePath.AsView(); }
        /// A check a moment from now, as after an edit (a page's other code changed: a sheet's
        /// preview markup).
        void CheckSoon() noexcept { m_checkIn = kCheckDelay; }

    private:
        static constexpr usize kUndoLimit = 100000;

        // One of the code's steps on the page's stack: done when pushed (the code did it), its
        // undo and redo the code's.
        class CodeStepCommand final : public IEditorCommand
        {
        public:
            explicit CodeStepCommand(CodeAssetPage& page) : m_page(&page) {}
            [[nodiscard]] bool Execute() override
            {
                if (m_done)
                    m_page->Step(gui::TextEditCommand::Redo);
                m_done = true;
                return true;
            }
            void Undo() override { m_page->Step(gui::TextEditCommand::Undo); }
            [[nodiscard]] StringView TypeId() const override { return u8"code_text"; }

        private:
            CodeAssetPage* m_page;
            bool m_done = false;
        };

        [[nodiscard]] static bool IsWordByte(char8_t c) noexcept
        {
            return (c >= u8'a' && c <= u8'z') || (c >= u8'A' && c <= u8'Z') || (c >= u8'0' && c <= u8'9') || c == u8'_' || c >= 0x80;
        }
        // Where a problem is marked: the word at its place, or one character; its line's text
        // when the check named no column.
        [[nodiscard]] gui::CodeRange RangeOf(const CodeProblem& p) const
        {
            const gui::CodeDocument& doc = m_code->GetDocument();
            const u32 l = doc.LineCount() == 0 ? 0 : Min(p.line, doc.LineCount() - 1);
            const StringView text = doc.LineText(l);
            if (!p.atColumn)
            {
                u32 first = 0;
                while (first < text.Size() && (text[first] == u8' ' || text[first] == u8'\t'))
                    ++first;
                return gui::CodeRange{gui::CodePosition{l, first}, gui::CodePosition{l, static_cast<u32>(text.Size())}};
            }
            const u32 c = Min(p.column, static_cast<u32>(text.Size()));
            u32 end = c;
            while (end < text.Size() && IsWordByte(text[end]))
                ++end;
            if (end == c && end < text.Size())
                ++end;
            return gui::CodeRange{gui::CodePosition{l, c}, gui::CodePosition{l, end}};
        }

        void CodeChanged()
        {
            MarkDirty(); // typing merged into a step already on the stack changes the code too
            m_checkIn = kCheckDelay;
            if (m_stepping)
                return;
            const usize steps = m_code->GetDocument().UndoCount();
            if (steps > m_steps)
                (void)Commands().Execute(UniquePtr<IEditorCommand>(EditorRootAllocator().New<CodeStepCommand>(*this), EditorRootAllocator()));
            m_steps = steps;
        }
        void Step(gui::TextEditCommand command)
        {
            if (m_code == nullptr)
                return;
            m_stepping = true;
            (void)m_code->Run(command);
            m_stepping = false;
            m_code->GetDocument().BreakMerge(); // typing after an undo starts a step of its own
            m_steps = m_code->GetDocument().UndoCount();
            m_checkIn = kCheckDelay;
        }

        gui::CodeEditor* m_code = nullptr; // the markup's
        UniquePtr<ProblemsList> m_problems;
        String m_fileName;
        String m_sourcePath;
        String m_saved; // the source as read or last written
        bool m_read = false;
        u32 m_listener = 0;
        usize m_steps = 0;       // the code's steps the page's stack holds
        bool m_stepping = false; // the page walking the code's history: no new step
        f32 m_checkIn = 0.0f;
        Array<CodeProblem> m_problemsFound;
    };
}
