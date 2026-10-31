// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::App - :problems_list partition.
//
// ProblemsList: what a code page's check found, shown under its code: a line saying how many
// errors and warnings there are ("No problems" when none), then a row for each problem with its
// severity's icon, its place ("12:4", with the file before it when the problem is in another file,
// such as a shader's include) and its message, errors first. Choosing a row, by a click or the
// arrow keys, tells the page, which moves the cursor there. It fills a panel of the page's markup,
// and lives in the editor until something outside the editor needs it.

module;
#include "Core/Prelude.h"

export module editor.app:problems_list;

import foundation.core;
import foundation.gui;
import foundation.gui.code;

using namespace foundation::core;

export namespace editor::app
{
    namespace gui = foundation::gui;

    /// One thing a check found.
    struct CodeProblem
    {
        gui::CodeSeverity severity = gui::CodeSeverity::Error;
        String message;
        String file; ///< empty: the page's own code; else the file it is in (an include)
        u32 line = 0;   ///< from 0
        u32 column = 0; ///< from 0
        bool atColumn = true; ///< false: the check named only the line (the whole line is marked)
    };

    /// Converts a code editor's diagnostics (in its own code) into problems.
    [[nodiscard]] inline Array<CodeProblem> ProblemsOf(Span<const gui::CodeDiagnostic> diagnostics)
    {
        Array<CodeProblem> out;
        for (const gui::CodeDiagnostic& d : diagnostics)
            out.PushBack(CodeProblem{d.Severity, d.Message, String{}, d.Range.Start.Line, d.Range.Start.Column, true});
        return out;
    }

    class ProblemsList
    {
    public:
        /// Fills `host` (a panel of the page's markup): its summary, then its list.
        ProblemsList(IAllocator& allocator, gui::UIWidget& host)
            : m_allocator(allocator), m_problems(allocator), m_model(*this)
        {
            host.AddClass(u8"problems");
            m_summary = MakeRef<gui::Label>(m_allocator);
            m_summary->AddClass(u8"problems-summary");
            host.AddChild(m_summary.Get());
            m_list = MakeRef<gui::ListView>(m_allocator);
            m_list->AddClass(u8"problems-list");
            ProblemsList* self = this;
            gui::RowFactory rows;
            rows.Create = [self](u32) { return self->MakeRow(); };
            rows.Bind = [self](gui::Node& row, const gui::ModelIndex& index) { self->BindRow(row, index); };
            m_list->SetRowFactory(Move(rows));
            m_list->SetModel(&m_model);
            // A row chosen by the keys or a click is navigated to; so is a second click on the
            // chosen row.
            m_list->SetOnSelectionChanged([self](gui::ModelIndex index) { self->Choose(index); });
            m_list->SetOnActivated([self](gui::ModelIndex index) { self->Choose(index); });
            (void)m_list->AddEventListener(gui::EventType::MouseClick, [self](const gui::Event&)
                                           { self->Choose(gui::ModelIndex{self->m_list->GetCurrentItem(), 0}); });
            host.AddChild(m_list.Get());
            Show(Span<const CodeProblem>{});
        }
        ~ProblemsList() { m_list->SetModel(nullptr); }
        ProblemsList(const ProblemsList&) = delete;
        ProblemsList& operator=(const ProblemsList&) = delete;

        /// Shows `problems`, errors first, each severity sorted by place.
        void Show(Span<const CodeProblem> problems)
        {
            m_problems.Clear();
            for (const CodeProblem& p : problems)
                m_problems.PushBack(p);
            // Errors, then warnings, then the rest; by file (its own first), line and column.
            for (usize i = 1; i < m_problems.Size(); ++i)
                for (usize j = i; j > 0 && Before(m_problems[j], m_problems[j - 1]); --j)
                {
                    CodeProblem t = Move(m_problems[j]);
                    m_problems[j] = Move(m_problems[j - 1]);
                    m_problems[j - 1] = Move(t);
                }
            m_summary->SetText(Summary().AsView());
            m_model.Reset();
        }
        /// Called with the chosen problem, so the page can move the cursor to it.
        void SetOnChoose(Function<void(const CodeProblem&)> onChoose) { m_onChoose = Move(onChoose); }

        /// What the summary says with no problems (empty: "No problems"), naming what the page's
        /// check confirmed (the class a script compiles). Takes effect at the next Show.
        void SetClean(String text) { m_clean = Move(text); }
        /// The summary: "No problems" (or the SetClean text), "1 error", "2 errors, 1 warning".
        [[nodiscard]] String Summary() const
        {
            usize errors = 0, warnings = 0, others = 0;
            for (const CodeProblem& p : m_problems)
                (p.severity == gui::CodeSeverity::Error ? errors : p.severity == gui::CodeSeverity::Warning ? warnings : others)++;
            if (m_problems.IsEmpty())
                return m_clean.IsEmpty() ? String(u8"No problems") : m_clean;
            String out;
            const auto add = [&out](usize count, StringView one, StringView many)
            {
                if (count == 0)
                    return;
                if (!out.IsEmpty())
                    out += u8", ";
                out += Format(u8"{} {}", count, count == 1 ? one : many);
            };
            add(errors, u8"error", u8"errors");
            add(warnings, u8"warning", u8"warnings");
            add(others, u8"note", u8"notes");
            return out;
        }

        // === For tests ===
        [[nodiscard]] Span<const CodeProblem> Problems() const noexcept { return Span<const CodeProblem>(m_problems.Data(), m_problems.Size()); }
        [[nodiscard]] gui::ListView& List() noexcept { return *m_list; }
        /// The place a row shows for a problem ("12:4", "Common.hlsli 3:1").
        [[nodiscard]] static String PlaceOf(const CodeProblem& p)
        {
            const String at = p.atColumn ? Format(u8"{}:{}", p.line + 1, p.column + 1) : Format(u8"{}", p.line + 1);
            return p.file.IsEmpty() ? at : Format(u8"{} {}", PathFilename(p.file.AsView()), at);
        }
        void Choose(gui::ModelIndex index)
        {
            if (index.Row < 0 || static_cast<usize>(index.Row) >= m_problems.Size() || !m_onChoose)
                return;
            m_onChoose(m_problems[static_cast<usize>(index.Row)]);
        }

    private:
        class Model final : public gui::IModel
        {
        public:
            explicit Model(ProblemsList& list) : m_list(list) {}
            [[nodiscard]] usize RowCount(const gui::ModelIndex& parent = {}) const override { return parent.IsValid() ? 0 : m_list.m_problems.Size(); }
            [[nodiscard]] usize ColumnCount() const override { return 1; }
            [[nodiscard]] gui::Variant Data(const gui::ModelIndex& index, gui::ModelRole role = gui::ModelRole::Display) const override
            {
                if (role != gui::ModelRole::Display || index.Row < 0 || static_cast<usize>(index.Row) >= m_list.m_problems.Size())
                    return gui::Variant{};
                return gui::Variant(m_list.m_problems[static_cast<usize>(index.Row)].message.AsView());
            }
            void Reset() { NotifyReset(); }

        private:
            ProblemsList& m_list;
        };

        [[nodiscard]] static i32 Rank(gui::CodeSeverity severity)
        {
            return severity == gui::CodeSeverity::Error ? 0 : severity == gui::CodeSeverity::Warning ? 1 : 2;
        }
        [[nodiscard]] static bool Before(const CodeProblem& a, const CodeProblem& b)
        {
            if (Rank(a.severity) != Rank(b.severity))
                return Rank(a.severity) < Rank(b.severity);
            if (a.file.IsEmpty() != b.file.IsEmpty())
                return a.file.IsEmpty();
            if (a.file.AsView() != b.file.AsView())
                return a.file.AsView().Compare(b.file.AsView()) < 0;
            return a.line != b.line ? a.line < b.line : a.column < b.column;
        }

        [[nodiscard]] RefPtr<gui::Node> MakeRow()
        {
            RefPtr<gui::Panel> row = MakeRef<gui::Panel>(m_allocator);
            row->AddClass(u8"problem-row");
            RefPtr<gui::Image> icon = MakeRef<gui::Image>(m_allocator);
            icon->AddClass(u8"problem-icon");
            RefPtr<gui::Label> place = MakeRef<gui::Label>(m_allocator);
            place->AddClass(u8"problem-place");
            place->AddClass(u8"mono");
            RefPtr<gui::Label> message = MakeRef<gui::Label>(m_allocator);
            message->AddClass(u8"problem-message");
            row->AddChild(icon.Get());
            row->AddChild(place.Get());
            row->AddChild(message.Get());
            return RefPtr<gui::Node>(row);
        }
        void BindRow(gui::Node& node, const gui::ModelIndex& index)
        {
            auto* row = Cast<gui::UIWidget>(&node);
            if (row == nullptr || index.Row < 0 || static_cast<usize>(index.Row) >= m_problems.Size() || row->ChildCount() < 3)
                return;
            const CodeProblem& p = m_problems[static_cast<usize>(index.Row)];
            const StringView severity = gui::CodeSeverityName(p.severity);
            if (auto* icon = Cast<gui::Image>(row->GetChildAt(0)))
                icon->SetSource(Format(u8"Editor/Icons/{}.svg", p.severity == gui::CodeSeverity::Error ? StringView(u8"error")
                                                                : p.severity == gui::CodeSeverity::Warning ? StringView(u8"warning")
                                                                                                          : StringView(u8"info")));
            if (auto* place = Cast<gui::Label>(row->GetChildAt(1)))
                place->SetText(PlaceOf(p).AsView());
            if (auto* message = Cast<gui::Label>(row->GetChildAt(2)))
            {
                message->SetText(p.message.AsView());
                message->SetTooltip(p.message.AsView()); // a long message is cut where the row ends
            }
            static constexpr StringView kSeverities[] = {u8"error", u8"warning", u8"info", u8"hint"};
            for (StringView s : kSeverities)
                row->RemoveClass(s);
            row->AddClass(severity);
        }

        IAllocator& m_allocator;
        Array<CodeProblem> m_problems;
        String m_clean; // the text set by SetClean
        Model m_model;
        RefPtr<gui::Label> m_summary;
        RefPtr<gui::ListView> m_list;
        Function<void(const CodeProblem&)> m_onChoose;
    };
}
