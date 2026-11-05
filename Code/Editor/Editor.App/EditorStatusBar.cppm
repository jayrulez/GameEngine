// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::App - :status_bar partition.
//
// EditorStatusBar: what the frame's status bar shows, through its items by id: the message
// (`status-message`, the context's status line), the job at the end (`status-job`: its name, a
// progress bar and its count, or a sweep when its length is unknown, as a cook's is; hidden when
// none runs) and the MCP host (`status-agent`, always shown: a dot lit while it listens, its port or
// "off", the tool of a call still running beside it; clicked, it shows the MCP panel).
module;
#include "Core/Prelude.h"

export module editor.app:status_bar;

import foundation.core;
import foundation.gui;

using namespace foundation::core;

export namespace editor::app
{
    namespace gui = foundation::gui;

    class EditorStatusBar
    {
    public:
        /// Binds `frame`'s items; any it lacks is skipped.
        explicit EditorStatusBar(gui::Node& frame)
        {
            m_message = Cast<gui::Label>(Find(frame, u8"status-message"));
            m_job = Find(frame, u8"status-job");
            m_jobName = Cast<gui::Label>(Find(frame, u8"status-job-name"));
            m_jobProgress = Cast<gui::ProgressBar>(Find(frame, u8"status-job-progress"));
            m_jobCount = Cast<gui::Label>(Find(frame, u8"status-job-count"));
            m_agentItem = Find(frame, u8"status-agent");
            m_agent = Cast<gui::Button>(Find(frame, u8"status-agent-text"));
            m_agentDot = Cast<gui::UIWidget>(Find(frame, u8"status-agent-dot"));
            m_agentCall = Cast<gui::Label>(Find(frame, u8"status-agent-call"));
            ClearJob();
            SetAgent(false, u8"MCP off", {}, u8"MCP: stopped. Show the MCP panel to start it");
        }

        void SetMessage(StringView text)
        {
            if (m_message != nullptr)
                m_message->SetText(text);
        }
        [[nodiscard]] StringView Message() const { return m_message != nullptr ? m_message->GetText() : StringView{}; }

        /// Shows a running job: its name, its progress (0 to 1; below 0 when its length is unknown)
        /// and its count ("12 of 40"; empty for none).
        void SetJob(StringView name, f32 fraction, StringView count)
        {
            if (m_job != nullptr)
                m_job->SetDisplayed(true);
            if (m_jobName != nullptr)
                m_jobName->SetText(name);
            if (m_jobProgress != nullptr)
            {
                m_jobProgress->SetIndeterminate(fraction < 0.0f);
                if (fraction >= 0.0f)
                    m_jobProgress->SetValue(Clamp(fraction, 0.0f, 1.0f));
            }
            if (m_jobCount != nullptr)
            {
                m_jobCount->SetText(count);
                m_jobCount->SetDisplayed(!count.IsEmpty());
            }
        }
        /// Clears the job display: no job is running.
        void ClearJob()
        {
            if (m_job != nullptr)
                m_job->SetDisplayed(false);
            if (m_jobProgress != nullptr)
                m_jobProgress->SetIndeterminate(false); // so no sweep animates while hidden
        }
        [[nodiscard]] bool ShowsJob() const { return m_job != nullptr && m_job->IsDisplayed(); }

        /// The MCP host's item: whether it listens (the dot lit), its text ("MCP 7405", "MCP off"),
        /// the tool of a call still running (empty: none) and the item's tooltip.
        void SetAgent(bool listening, StringView text, StringView runningCall, StringView tooltip)
        {
            if (m_agent != nullptr)
            {
                if (m_agent->GetText() != text)
                    m_agent->SetText(text);
                if (m_agent->GetTooltipText() != tooltip)
                    m_agent->SetTooltip(tooltip);
            }
            if (m_agentDot != nullptr && m_agentDot->HasClass(u8"on") != listening)
                listening ? m_agentDot->AddClass(u8"on") : m_agentDot->RemoveClass(u8"on");
            if (m_agentCall != nullptr)
            {
                if (m_agentCall->GetText() != runningCall)
                    m_agentCall->SetText(runningCall);
                m_agentCall->SetDisplayed(!runningCall.IsEmpty());
            }
        }
        /// What a click on the MCP item does (shows the MCP panel).
        void SetOnAgentClicked(Function<void()> clicked)
        {
            if (m_agent != nullptr)
                m_agent->SetOnClick(Move(clicked));
        }
        [[nodiscard]] bool ShowsAgent() const { return m_agentItem != nullptr && m_agentItem->IsDisplayed(); }
        [[nodiscard]] StringView AgentText() const { return m_agent != nullptr ? m_agent->GetText() : StringView{}; }
        [[nodiscard]] bool AgentListening() const { return m_agentDot != nullptr && m_agentDot->HasClass(u8"on"); }
        [[nodiscard]] StringView AgentCall() const
        {
            return m_agentCall != nullptr && m_agentCall->IsDisplayed() ? m_agentCall->GetText() : StringView{};
        }

    private:
        static gui::Node* Find(gui::Node& node, StringView id)
        {
            if (const auto* widget = Cast<gui::UIWidget>(&node); widget != nullptr && widget->GetId() == id)
                return &node;
            for (usize i = 0; i < node.ChildCount(); ++i)
                if (gui::Node* found = Find(*node.GetChildAt(i), id))
                    return found;
            return nullptr;
        }

        gui::Label* m_message = nullptr;
        gui::Node* m_job = nullptr;
        gui::Label* m_jobName = nullptr;
        gui::ProgressBar* m_jobProgress = nullptr;
        gui::Label* m_jobCount = nullptr;
        gui::Node* m_agentItem = nullptr;
        gui::Button* m_agent = nullptr;
        gui::UIWidget* m_agentDot = nullptr;
        gui::Label* m_agentCall = nullptr;
    };
}
