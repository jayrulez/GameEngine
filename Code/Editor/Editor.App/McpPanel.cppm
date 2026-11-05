// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::App - :mcp_panel partition.
//
// McpPanel: the MCP panel. Its frame is markup (Data/Editor/Mcp.xml). The bar shows the host's
// state (a dot lit while it listens, where it listens, the client that connected), Start or Stop,
// Copy Connect Command (the command that wires Claude Code to the host, with its token) and
// whether the host starts with each project (the MCP preference); then a chip per filter (All;
// Edits, the calls whose tool creates, overwrites or deletes authored data; Failed), each with its
// count, a search over a call's tool, summary and arguments, Clear and the preferences. Under it
// the activity (McpActivity), newest first: each call's time, outcome, tool, summary, an Edit badge
// and how long it took, with the host's own events between; beside it the chosen call's arguments
// and answer, read only, and Copy as JSON. It is refreshed once a frame from the activity.
module;
#include "Core/Prelude.h"

export module editor.app:mcp_panel;

import foundation.core;
import foundation.json;
import foundation.gui;
import :mcp_activity;

using namespace foundation::core;

export namespace editor::app
{
    namespace gui = foundation::gui;

    /// McpPanel::kPath, at namespace scope for another library to read (MSVC exports no class's
    /// static constexpr member from a shared library).
    inline constexpr const char8_t* kMcpPanelPath = u8"Editor/Mcp.xml";

    /// What the MCP panel shows and does, from the application.
    struct McpPanelSeams
    {
        Function<McpActivity*()> activity; ///< the service's (null: no service yet)
        Function<bool()> listening;
        Function<u16()> port;           ///< where it listens (0: stopped)
        Function<u32()> configuredPort; ///< where it starts
        Function<String()> client;      ///< the client that connected ("claude-code 2.1.0"; empty: none yet)
        Function<bool()> start;         ///< the user's Start; false when it could not listen
        Function<void()> stop;          ///< the user's Stop
        Function<String()> connectCommand;
        Function<bool()> startsWithProject;
        Function<void(bool)> setStartsWithProject;
        /// Puts `text` on the clipboard; `what` names it for the user ("the connect command").
        Function<void(StringView text, StringView what)> copy;
        Function<void()> preferences;
    };

    class McpPanel
    {
    public:
        static constexpr const char8_t* kPath = kMcpPanelPath;

        enum class Filter : u8
        {
            All,
            Edits,
            Failed,
        };

        McpPanel(IAllocator& allocator, McpPanelSeams seams, RefPtr<gui::Node> content)
            : m_allocator(allocator), m_seams(Move(seams)), m_content(Move(content)), m_shown(allocator), m_model(*this)
        {
            if (!m_content)
                return;
            McpPanel* self = this;
            m_list = Find<gui::ListView>(u8"mcp-list");
            if (m_list != nullptr)
            {
                gui::RowFactory rows;
                rows.Create = [self](u32) { return self->MakeRow(); };
                rows.Bind = [self](gui::Node& row, const gui::ModelIndex& index) { self->BindRow(row, index); };
                m_list->SetRowFactory(Move(rows));
                m_list->SetModel(&m_model);
                m_list->SetOnSelectionChanged([self](gui::ModelIndex) { self->ShowDetail(); });
            }
            const char8_t* const chips[3] = {u8"mcp-all", u8"mcp-edits", u8"mcp-failed"};
            for (usize i = 0; i < 3; ++i)
                if ((m_chips[i] = Find<gui::Button>(StringView(chips[i]))) != nullptr)
                {
                    const Filter filter = static_cast<Filter>(i);
                    m_chips[i]->SetOnClick([self, filter]() { self->SetFilter(filter); });
                }
            if (auto* search = Find<gui::TextField>(u8"mcp-search"))
                search->SetOnTextChanging([self](StringView text) { self->SetSearch(text); });
            if (auto* clearSearch = Find<gui::Button>(u8"mcp-clear-search"))
                clearSearch->SetOnClick([self]()
                                        {
                                            if (auto* field = self->Find<gui::TextField>(u8"mcp-search"))
                                                field->SetText(StringView{});
                                            self->SetSearch(StringView{});
                                        });
            if (auto* clear = Find<gui::Button>(u8"mcp-clear"))
                clear->SetOnClick([self]() { self->Clear(); });
            if (auto* start = Find<gui::Button>(u8"mcp-start"))
                start->SetOnClick([self]() { self->Start(); });
            if (auto* stop = Find<gui::Button>(u8"mcp-stop"))
                stop->SetOnClick([self]() { self->Stop(); });
            if (auto* copy = Find<gui::Button>(u8"mcp-copy-connect"))
                copy->SetOnClick([self]() { self->CopyConnectCommand(); });
            if (auto* preferences = Find<gui::Button>(u8"mcp-preferences"))
                preferences->SetOnClick([self]()
                                        {
                                            if (self->m_seams.preferences)
                                                self->m_seams.preferences();
                                        });
            if (auto* withProject = Find<gui::CheckBox>(u8"mcp-with-project"))
                withProject->SetOnToggled([self](bool on)
                                          {
                                              if (self->m_seams.setStartsWithProject)
                                                  self->m_seams.setStartsWithProject(on);
                                          });
            if (auto* copyCall = Find<gui::Button>(u8"mcp-detail-copy"))
                copyCall->SetOnClick([self]() { self->CopySelectedCall(); });
            static constexpr StringView kAreas[] = {u8"mcp-arguments", u8"mcp-result"};
            for (StringView id : kAreas)
                if (auto* area = Find<gui::TextArea>(id))
                    area->SetReadOnly(true);
            Update();
        }
        McpPanel(const McpPanel&) = delete;
        McpPanel& operator=(const McpPanel&) = delete;
        ~McpPanel()
        {
            if (m_list != nullptr)
                m_list->SetModel(nullptr);
        }

        [[nodiscard]] RefPtr<gui::Node> Content() const { return m_content; }

        /// Called each frame: the bar follows the host, the list and the detail the activity.
        void Update()
        {
            ShowState();
            const u64 revision = Activity() != nullptr ? Activity()->Revision() : 0;
            if (revision != m_revision || !m_listed)
            {
                m_revision = revision;
                m_listed = true;
                Refilter();
                ShowChrome();
                ShowDetail();
            }
        }

        void Start()
        {
            m_startFailed = m_seams.start && !m_seams.start();
            ShowState();
        }
        void Stop()
        {
            m_startFailed = false;
            if (m_seams.stop)
                m_seams.stop();
            ShowState();
        }
        void CopyConnectCommand()
        {
            const String command = m_seams.connectCommand ? m_seams.connectCommand() : String();
            if (!command.IsEmpty() && m_seams.copy)
                m_seams.copy(command.AsView(), u8"the connect command");
        }
        void Clear()
        {
            if (McpActivity* activity = Activity())
                activity->Clear();
            Update();
        }

        [[nodiscard]] Filter GetFilter() const noexcept { return m_filter; }
        void SetFilter(Filter filter)
        {
            if (m_filter == filter)
                return;
            m_filter = filter;
            Refilter();
            ShowChrome();
            ShowDetail();
        }
        /// Shows only the calls whose tool, summary or arguments hold `text`, ignoring case; empty
        /// shows them all.
        void SetSearch(StringView text)
        {
            if (m_search.AsView() == text)
                return;
            m_search = String(text);
            Refilter();
            ShowChrome();
            ShowDetail();
        }

        /// The rows shown, newest first.
        [[nodiscard]] usize ShownCount() const noexcept { return m_shown.Size(); }
        [[nodiscard]] const McpActivity::Entry* ShownEntry(usize row) const
        {
            const McpActivity* activity = Activity();
            return row < m_shown.Size() && activity != nullptr ? &activity->At(m_shown[row]) : nullptr;
        }
        /// Chooses a shown row, so the detail shows it.
        void Select(usize row)
        {
            if (m_list != nullptr && row < m_shown.Size())
                m_list->SetSelectedItem(static_cast<i32>(row));
            ShowDetail();
        }
        /// The chosen call (null: none, or an event).
        [[nodiscard]] const McpActivity::Entry* Selected() const
        {
            if (m_list == nullptr)
                return nullptr;
            const i32 item = m_list->GetSelectedItem();
            const McpActivity::Entry* entry = item >= 0 ? ShownEntry(static_cast<usize>(item)) : nullptr;
            return entry != nullptr && entry->IsCall() ? entry : nullptr;
        }
        /// The chosen call as JSON: its tool, arguments, outcome, how long it took and its answer.
        [[nodiscard]] String SelectedCallJson() const
        {
            const McpActivity::Entry* entry = Selected();
            if (entry == nullptr)
                return {};
            foundation::json::JsonValue call = foundation::json::JsonValue::MakeObject();
            call.Set(u8"tool", foundation::json::JsonValue::MakeString(entry->tool));
            call.Set(u8"arguments", ParsedOrText(entry->arguments.AsView()));
            call.Set(u8"outcome", foundation::json::JsonValue::MakeString(String(OutcomeName(entry->outcome))));
            call.Set(u8"milliseconds", foundation::json::JsonValue::MakeNumber(static_cast<f64>(static_cast<i64>(entry->elapsed.AsSeconds() * 1000.0 + 0.5))));
            if (entry->outcome == McpActivity::Outcome::Done || entry->outcome == McpActivity::Outcome::Failed)
                call.Set(entry->outcome == McpActivity::Outcome::Failed ? StringView(u8"error") : StringView(u8"result"),
                         ParsedOrText(entry->result.AsView()));
            return call.ToString(true);
        }
        void CopySelectedCall()
        {
            const String json = SelectedCallJson();
            if (!json.IsEmpty() && m_seams.copy)
                m_seams.copy(json.AsView(), u8"the call");
        }

        /// A time of day as hh:mm:ss, from local seconds since 1970.
        [[nodiscard]] static String TimeText(i64 at)
        {
            const CivilTime time = CivilTimeOf(at);
            return Format(u8"{}{}:{}{}:{}{}", time.hour < 10 ? u8"0" : u8"", time.hour, time.minute < 10 ? u8"0" : u8"", time.minute,
                          time.second < 10 ? u8"0" : u8"", time.second);
        }
        /// How long a call took: whole milliseconds under a second, tenths of a second above.
        [[nodiscard]] static String TookText(Duration elapsed)
        {
            const f64 seconds = elapsed.AsSeconds();
            if (seconds < 1.0)
                return Format(u8"{} ms", static_cast<i64>(seconds * 1000.0 + 0.5));
            const i64 tenths = static_cast<i64>(seconds * 10.0 + 0.5);
            return Format(u8"{}.{} s", tenths / 10, tenths % 10);
        }

    private:
        class Model final : public gui::IModel
        {
        public:
            explicit Model(McpPanel& panel) : m_panel(panel) {}
            [[nodiscard]] usize RowCount(const gui::ModelIndex& parent = {}) const override { return parent.IsValid() ? 0 : m_panel.m_shown.Size(); }
            [[nodiscard]] usize ColumnCount() const override { return 1; }
            [[nodiscard]] gui::Variant Data(const gui::ModelIndex& index, gui::ModelRole role = gui::ModelRole::Display) const override
            {
                const McpActivity::Entry* entry = index.Row >= 0 ? m_panel.ShownEntry(static_cast<usize>(index.Row)) : nullptr;
                if (role != gui::ModelRole::Display || entry == nullptr)
                    return gui::Variant{};
                return gui::Variant(entry->tool.AsView());
            }
            [[nodiscard]] gui::ItemId IdOf(const gui::ModelIndex& index) const override
            {
                const McpActivity::Entry* entry = index.Row >= 0 ? m_panel.ShownEntry(static_cast<usize>(index.Row)) : nullptr;
                return entry != nullptr ? entry->id : 0;
            }
            [[nodiscard]] gui::ModelIndex IndexOfId(gui::ItemId id) const override
            {
                for (usize row = 0; row < m_panel.m_shown.Size(); ++row)
                    if (m_panel.ShownEntry(row)->id == id)
                        return gui::ModelIndex{static_cast<i32>(row), 0};
                return {};
            }
            void Reset() { NotifyReset(); }

        private:
            McpPanel& m_panel;
        };

        template <typename T>
        [[nodiscard]] T* Find(StringView id) const
        {
            return m_content ? Cast<T>(FindIn(*m_content, id)) : nullptr;
        }
        static gui::Node* FindIn(gui::Node& node, StringView id)
        {
            if (const auto* widget = Cast<gui::UIWidget>(&node); widget != nullptr && widget->GetId() == id)
                return &node;
            for (usize i = 0; i < node.ChildCount(); ++i)
                if (gui::Node* found = FindIn(*node.GetChildAt(i), id))
                    return found;
            return nullptr;
        }
        [[nodiscard]] McpActivity* Activity() const { return m_seams.activity ? m_seams.activity() : nullptr; }
        static void SetText(gui::Label* label, StringView text)
        {
            if (label != nullptr && label->GetText() != text)
                label->SetText(text);
        }
        static void SetClass(gui::UIWidget* widget, StringView name, bool on)
        {
            if (widget != nullptr && widget->HasClass(name) != on)
                on ? widget->AddClass(name) : widget->RemoveClass(name);
        }

        static constexpr StringView kOutcomeClasses[] = {u8"running", u8"done", u8"failed", u8"abandoned"};
        static constexpr StringView kRowClasses[] = {u8"running", u8"done", u8"failed", u8"abandoned", u8"event"};

        [[nodiscard]] static StringView OutcomeName(McpActivity::Outcome outcome)
        {
            switch (outcome)
            {
            case McpActivity::Outcome::Running:
                return u8"running";
            case McpActivity::Outcome::Failed:
                return u8"failed";
            case McpActivity::Outcome::Abandoned:
                return u8"abandoned";
            default:
                return u8"done";
            }
        }
        // JSON text as a value, or the text itself as a string when it is not JSON (an error message).
        [[nodiscard]] static foundation::json::JsonValue ParsedOrText(StringView text)
        {
            foundation::json::ParseResult parsed = foundation::json::Parse(text);
            return parsed.ok ? Move(parsed.value) : foundation::json::JsonValue::MakeString(String(text));
        }
        [[nodiscard]] static String Pretty(StringView text)
        {
            const foundation::json::ParseResult parsed = foundation::json::Parse(text);
            return parsed.ok ? parsed.value.ToString(true) : String(text);
        }

        [[nodiscard]] bool Passes(const McpActivity::Entry& entry) const
        {
            if (m_filter == Filter::Edits && !(entry.IsCall() && entry.changes))
                return false;
            if (m_filter == Filter::Failed && !(entry.IsCall() && entry.outcome == McpActivity::Outcome::Failed))
                return false;
            if (m_search.IsEmpty())
                return true;
            const StringView search = m_search.AsView();
            return entry.tool.AsView().ContainsIgnoreCase(search) || entry.arguments.AsView().ContainsIgnoreCase(search) ||
                   McpActivity::Summary(entry).AsView().ContainsIgnoreCase(search);
        }
        void Refilter()
        {
            m_shown.Clear();
            for (usize i = 0; i < 3; ++i)
                m_counts[i] = 0;
            if (const McpActivity* activity = Activity())
                for (usize i = activity->Size(); i > 0; --i)
                {
                    const McpActivity::Entry& entry = activity->At(i - 1);
                    if (entry.IsCall())
                    {
                        ++m_counts[static_cast<usize>(Filter::All)];
                        m_counts[static_cast<usize>(Filter::Edits)] += entry.changes ? 1 : 0;
                        m_counts[static_cast<usize>(Filter::Failed)] += entry.outcome == McpActivity::Outcome::Failed ? 1 : 0;
                    }
                    if (Passes(entry))
                        m_shown.PushBack(i - 1);
                }
            m_model.Reset();
        }

        // The bar: the host's state, the controls that apply to it, the preference.
        void ShowState()
        {
            const bool listening = m_seams.listening && m_seams.listening();
            if (listening)
                m_startFailed = false;
            const u16 port = m_seams.port ? m_seams.port() : 0;
            const u32 configured = m_seams.configuredPort ? m_seams.configuredPort() : 0;
            SetClass(Find<gui::UIWidget>(u8"mcp-dot"), u8"on", listening);
            SetText(Find<gui::Label>(u8"mcp-state"), listening ? Format(u8"Listening on 127.0.0.1:{}", port).AsView()
                                                     : m_startFailed ? StringView(u8"Could not start")
                                                                     : StringView(u8"Stopped"));
            String client;
            if (listening)
            {
                client = m_seams.client ? m_seams.client() : String();
                if (client.IsEmpty())
                    client = String(u8"no client yet");
            }
            else
                client = m_startFailed ? Format(u8"127.0.0.1:{} is taken, by another editor most likely; change the port in the preferences", configured)
                                       : String(u8"Agents cannot reach the editor");
            SetText(Find<gui::Label>(u8"mcp-client"), client.AsView());
            if (auto* start = Find<gui::Button>(u8"mcp-start"))
            {
                start->SetDisplayed(!listening);
                const String tooltip = Format(u8"Start the MCP host on 127.0.0.1:{}", configured);
                if (start->GetTooltipText() != tooltip.AsView())
                    start->SetTooltip(tooltip.AsView());
            }
            if (auto* stop = Find<gui::Button>(u8"mcp-stop"))
                stop->SetDisplayed(listening);
            if (auto* copy = Find<gui::Button>(u8"mcp-copy-connect"))
                copy->SetDisplayed(listening);
            if (auto* withProject = Find<gui::CheckBox>(u8"mcp-with-project"))
            {
                const bool on = m_seams.startsWithProject && m_seams.startsWithProject();
                if (withProject->IsChecked() != on)
                    withProject->SetChecked(on);
            }
        }

        // The chips' counts and states, the search's x, the empty line.
        void ShowChrome()
        {
            static constexpr StringView kNames[3] = {u8"All", u8"Edits", u8"Failed"};
            for (usize i = 0; i < 3; ++i)
                if (gui::Button* chip = m_chips[i])
                {
                    const String text = m_counts[i] == 0 ? String(kNames[i]) : Format(u8"{}  {}", kNames[i], m_counts[i]);
                    if (chip->GetText() != text.AsView())
                        chip->SetText(text.AsView());
                    SetClass(chip, u8"checked", static_cast<usize>(m_filter) == i);
                }
            if (auto* clearSearch = Find<gui::Button>(u8"mcp-clear-search"))
                clearSearch->SetVisible(!m_search.IsEmpty()); // kept in place so the field does not jump
            if (auto* empty = Find<gui::Label>(u8"mcp-empty"))
            {
                empty->SetDisplayed(m_shown.IsEmpty());
                const bool none = Activity() == nullptr || Activity()->Size() == 0;
                SetText(empty, none ? StringView(u8"No calls yet. Agents' calls show here as they arrive.") : StringView(u8"No call matches."));
            }
        }

        // The chosen call's head, arguments and answer; the placeholder when none is chosen.
        void ShowDetail()
        {
            const McpActivity::Entry* entry = Selected();
            if (auto* detail = Find<gui::UIWidget>(u8"mcp-detail"))
                detail->SetDisplayed(entry != nullptr);
            if (auto* none = Find<gui::UIWidget>(u8"mcp-no-detail"))
                none->SetDisplayed(entry == nullptr);
            if (entry == nullptr)
            {
                m_detailId = 0;
                return;
            }
            // Rewritten only when the call or its outcome changed, so a selection in the text stays.
            if (m_detailId == entry->id && m_detailOutcome == entry->outcome)
                return;
            m_detailId = entry->id;
            m_detailOutcome = entry->outcome;
            SetText(Find<gui::Label>(u8"mcp-detail-tool"), entry->tool.AsView());
            if (auto* outcome = Find<gui::Label>(u8"mcp-detail-outcome"))
            {
                static constexpr StringView kTexts[] = {u8"Running", u8"Done", u8"Failed", u8"Abandoned"};
                SetText(outcome, kTexts[static_cast<usize>(entry->outcome)]);
                for (StringView name : kOutcomeClasses)
                    SetClass(outcome, name, name == OutcomeName(entry->outcome));
            }
            const String when = entry->outcome == McpActivity::Outcome::Running ? TimeText(entry->at)
                                                                                 : Format(u8"{}, {}", TimeText(entry->at), TookText(entry->elapsed));
            SetText(Find<gui::Label>(u8"mcp-detail-when"), when.AsView());
            if (auto* arguments = Find<gui::TextArea>(u8"mcp-arguments"))
            {
                arguments->SetText(Pretty(entry->arguments.AsView()).AsView());
                arguments->SetCaret(0); // read from the top
            }
            SetText(Find<gui::Label>(u8"mcp-result-title"), entry->outcome == McpActivity::Outcome::Failed ? StringView(u8"Error") : StringView(u8"Result"));
            if (auto* result = Find<gui::TextArea>(u8"mcp-result"))
                result->SetText(entry->outcome == McpActivity::Outcome::Running     ? StringView(u8"(still running)")
                                : entry->outcome == McpActivity::Outcome::Abandoned ? StringView(u8"(no answer: the caller left or the host stopped)")
                                                                                    : Pretty(entry->result.AsView()).AsView());
            if (auto* result = Find<gui::TextArea>(u8"mcp-result"))
                result->SetCaret(0);
        }

        [[nodiscard]] RefPtr<gui::Node> MakeRow()
        {
            RefPtr<gui::Panel> row = MakeRef<gui::Panel>(m_allocator);
            row->AddClass(u8"mcp-row");
            const auto label = [&](StringView classes, bool mono)
            {
                RefPtr<gui::Label> made = MakeRef<gui::Label>(m_allocator);
                made->AddClass(classes);
                if (mono)
                    made->AddClass(u8"mono");
                row->AddChild(made.Get());
            };
            label(u8"mcp-time", true);
            RefPtr<gui::Panel> mark = MakeRef<gui::Panel>(m_allocator);
            mark->AddClass(u8"mcp-mark");
            row->AddChild(mark.Get());
            label(u8"mcp-tool", true);
            label(u8"mcp-summary", false);
            label(u8"mcp-badge", false);
            label(u8"mcp-took", true);
            return RefPtr<gui::Node>(row);
        }
        void BindRow(gui::Node& node, const gui::ModelIndex& index)
        {
            auto* row = Cast<gui::UIWidget>(&node);
            const McpActivity::Entry* entry = index.Row >= 0 ? ShownEntry(static_cast<usize>(index.Row)) : nullptr;
            if (row == nullptr || entry == nullptr || row->ChildCount() < 6)
                return;
            const bool call = entry->IsCall();
            const StringView outcome = call ? OutcomeName(entry->outcome) : StringView(u8"event");
            for (StringView name : kRowClasses)
            {
                SetClass(row, name, name == outcome);
                SetClass(Cast<gui::UIWidget>(row->GetChildAt(1)), name, name == outcome);
            }
            SetText(Cast<gui::Label>(row->GetChildAt(0)), TimeText(entry->at).AsView());
            auto* tool = Cast<gui::Label>(row->GetChildAt(2));
            SetText(tool, call ? entry->tool.AsView() : StringView{});
            if (tool != nullptr)
                tool->SetDisplayed(call);
            SetText(Cast<gui::Label>(row->GetChildAt(3)), McpActivity::Summary(*entry).AsView());
            auto* badge = Cast<gui::Label>(row->GetChildAt(4));
            if (badge != nullptr)
            {
                badge->SetDisplayed(call && entry->changes);
                SetText(badge, u8"Edit");
                SetClass(badge, u8"edit", true);
            }
            SetText(Cast<gui::Label>(row->GetChildAt(5)), !call ? StringView{}
                                                          : entry->outcome == McpActivity::Outcome::Running ? StringView(u8"running")
                                                                                                            : TookText(entry->elapsed).AsView());
        }

        IAllocator& m_allocator;
        McpPanelSeams m_seams;
        RefPtr<gui::Node> m_content;
        gui::ListView* m_list = nullptr;
        gui::Button* m_chips[3] = {};
        Array<usize> m_shown; // the entries shown, newest first, by index into the activity
        usize m_counts[3] = {};
        Filter m_filter = Filter::All;
        String m_search;
        u64 m_revision = 0;
        bool m_listed = false;
        bool m_startFailed = false;
        u64 m_detailId = 0;
        McpActivity::Outcome m_detailOutcome = McpActivity::Outcome::Done;
        Model m_model;
    };
}
