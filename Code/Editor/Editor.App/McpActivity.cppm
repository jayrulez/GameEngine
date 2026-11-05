// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::App - :mcp_activity partition.
//
// McpActivity: what agents did through the editor's MCP host in this run, for the MCP panel and
// the status bar. Each tools/call is one entry, added when the host reports it and updated as it
// finishes, with its arguments, its answer, how long it took and whether its tool changes authored
// data; between them sit the host's own events (it started, it stopped, a client connected). It is
// kept in memory only, outlives the host's restarts, and is capped (the oldest trimmed).
module;
#include "Core/Prelude.h"

export module editor.app:mcp_activity;

import foundation.core;
import foundation.json;
import foundation.mcp;

using namespace foundation::core;

export namespace editor::app
{
    /// McpActivity::kMaxEntries, at namespace scope for another library to read (MSVC exports no
    /// class's static constexpr member from a shared library).
    inline constexpr usize kMcpActivityMaxEntries = 500;

    class McpActivity
    {
    public:
        static constexpr usize kMaxEntries = kMcpActivityMaxEntries;

        enum class Kind : u8
        {
            Call,  ///< a tools/call
            Event, ///< the host's own: started, stopped, a client connected
        };
        enum class Outcome : u8
        {
            Running,
            Done,
            Failed,
            Abandoned, ///< its caller went away, or the host stopped, before it answered
            Refused,   ///< no tool ran: an unknown tool, or arguments its schema refuses
        };

        struct Entry
        {
            u64 id = 0; ///< this record's number, unique for the editor's run
            Kind kind = Kind::Call;
            Outcome outcome = Outcome::Done;
            String tool;      ///< a call's tool; an event's text
            String arguments; ///< compact JSON
            String result;    ///< the answer's text: its JSON, or the error message
            bool changes = false; ///< its tool creates, overwrites or deletes authored data
            i64 at = 0;           ///< when it started, in local seconds since 1970 (CivilTimeOf reads it)
            Duration elapsed;     ///< how long it took (so far, while running)

            [[nodiscard]] bool IsCall() const noexcept { return kind == Kind::Call; }
        };

        explicit McpActivity(IAllocator& allocator) : m_entries(allocator), m_inFlight(allocator) {}

        /// Records one report of the host's server. `at` is the local time it arrived. Started and
        /// an at-once Finished add an entry; a later Finished or Abandoned updates its own.
        void Record(const foundation::mcp::ToolCallReport& report, i64 at)
        {
            Entry* entry = report.phase == foundation::mcp::ToolCallPhase::Started ? nullptr : FindRunning(report.serial);
            if (entry == nullptr)
            {
                if (report.phase == foundation::mcp::ToolCallPhase::Abandoned)
                    return; // never heard Started: nothing to close
                Entry added;
                added.kind = Kind::Call;
                added.tool = String(report.tool);
                added.arguments = report.arguments != nullptr ? report.arguments->ToString() : String(u8"{}");
                added.changes = report.phase != foundation::mcp::ToolCallPhase::Refused && report.annotations.ChangesAuthoredData();
                added.at = at - static_cast<i64>(report.elapsed.AsSeconds());
                entry = &Add(Move(added));
                m_inFlight.PushBack(InFlight{entry->id, report.serial});
            }
            entry->elapsed = report.elapsed;
            switch (report.phase)
            {
            case foundation::mcp::ToolCallPhase::Started:
                entry->outcome = Outcome::Running;
                return; // stays running
            case foundation::mcp::ToolCallPhase::Finished:
                entry->outcome = report.isError ? Outcome::Failed : Outcome::Done;
                entry->result = String(report.result);
                break;
            case foundation::mcp::ToolCallPhase::Abandoned:
                entry->outcome = Outcome::Abandoned;
                break;
            case foundation::mcp::ToolCallPhase::Refused:
                entry->outcome = Outcome::Refused;
                entry->result = String(report.result);
                break;
            }
            ForgetRunning(entry->id);
            ++m_revision;
        }

        /// Records one of the host's own events ("Listening on 127.0.0.1:7405", "Stopped").
        void RecordEvent(StringView text, i64 at)
        {
            Entry added;
            added.kind = Kind::Event;
            added.tool = String(text);
            added.at = at;
            (void)Add(Move(added));
        }

        /// The host stopped: its calls still running end unanswered, and the stop is recorded. The
        /// server's serials start again with the next host, so none is kept.
        void HostStopped(i64 at)
        {
            for (const InFlight& call : m_inFlight)
                if (Entry* entry = FindEntry(call.id))
                    entry->outcome = Outcome::Abandoned;
            m_inFlight.Clear();
            RecordEvent(u8"Stopped", at);
        }

        void Clear()
        {
            m_entries.Clear();
            m_inFlight.Clear();
            ++m_revision;
        }

        [[nodiscard]] usize Size() const noexcept { return m_entries.Size(); }
        /// The entries oldest first.
        [[nodiscard]] const Entry& At(usize index) const { return m_entries[index]; }
        [[nodiscard]] const Entry* FindById(u64 id) const
        {
            for (const Entry& entry : m_entries)
                if (entry.id == id)
                    return &entry;
            return nullptr;
        }
        /// The newest call still running, or null.
        [[nodiscard]] const Entry* RunningCall() const
        {
            return m_inFlight.IsEmpty() ? nullptr : FindById(m_inFlight.Back().id);
        }
        /// The newest call, or null.
        [[nodiscard]] const Entry* LastCall() const
        {
            for (usize i = m_entries.Size(); i > 0; --i)
                if (m_entries[i - 1].IsCall())
                    return &m_entries[i - 1];
            return nullptr;
        }
        /// Changes with every record, so a view knows when to refresh.
        [[nodiscard]] u64 Revision() const noexcept { return m_revision; }

        /// One line saying what a call was about, for its row: a failure's or a refusal's first line, else
        /// its arguments as `name: value` pairs (strings bare, the rest as JSON), cut at `maxLength`.
        [[nodiscard]] static String Summary(const Entry& entry, usize maxLength = 160)
        {
            String text;
            if (!entry.IsCall())
                text = entry.tool;
            else if (entry.outcome == Outcome::Failed || entry.outcome == Outcome::Refused)
            {
                const StringView error = entry.result.AsView();
                const usize end = error.Find(StringView(u8"\n"));
                text = String(end == StringView::kNotFound ? error : error.SubStr(0, end));
            }
            else
            {
                const foundation::json::ParseResult parsed = foundation::json::Parse(entry.arguments.AsView());
                if (parsed.ok && parsed.value.IsObject())
                    for (const String& key : parsed.value.Keys())
                    {
                        const foundation::json::JsonValue value = parsed.value.Get(key);
                        if (!text.IsEmpty())
                            text += u8", ";
                        text += key.AsView();
                        text += u8": ";
                        text += (value.IsString() ? value.AsString() : value.ToString()).AsView();
                        if (text.Size() > maxLength)
                            break;
                    }
            }
            if (text.Size() > maxLength)
            {
                usize cut = maxLength;
                while (cut > 0 && (static_cast<u8>(text.Data()[cut]) & 0xC0u) == 0x80u)
                    --cut; // never inside a character
                text = String(text.AsView().SubStr(0, cut));
                text += u8"...";
            }
            return text;
        }

    private:
        struct InFlight
        {
            u64 id = 0;     ///< the entry's
            u64 serial = 0; ///< the server's
        };

        Entry& Add(Entry entry)
        {
            entry.id = m_nextId++;
            m_entries.PushBack(Move(entry));
            while (m_entries.Size() > kMaxEntries)
            {
                ForgetRunning(m_entries[0].id);
                m_entries.RemoveAt(0);
            }
            ++m_revision;
            return m_entries.Back();
        }
        [[nodiscard]] Entry* FindEntry(u64 id)
        {
            for (Entry& entry : m_entries)
                if (entry.id == id)
                    return &entry;
            return nullptr;
        }
        [[nodiscard]] Entry* FindRunning(u64 serial)
        {
            for (const InFlight& call : m_inFlight)
                if (call.serial == serial)
                    return FindEntry(call.id);
            return nullptr;
        }
        void ForgetRunning(u64 id)
        {
            for (usize i = 0; i < m_inFlight.Size(); ++i)
                if (m_inFlight[i].id == id)
                {
                    m_inFlight.RemoveAt(i);
                    return;
                }
        }

        Array<Entry> m_entries;
        Array<InFlight> m_inFlight; // the calls still running, oldest first
        u64 m_nextId = 1;
        u64 m_revision = 0;
    };
}
