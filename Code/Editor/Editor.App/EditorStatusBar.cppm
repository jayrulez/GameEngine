// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::App - :status_bar partition.
//
// EditorStatusBar: what the frame's status bar shows, through its items by id: the message
// (`status-message`, the context's status line), the job at the end (`status-job`: its name, a
// progress bar and its count, or a sweep when its length is unknown, as a cook's is) and the agent
// (`status-agent`, shown while the MCP host listens). An item with nothing to show is hidden.
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
            m_agent = Cast<gui::Label>(Find(frame, u8"status-agent-text"));
            ClearJob();
            SetAgent({});
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

        /// The agent's line ("MCP on 127.0.0.1:7405"); empty hides it.
        void SetAgent(StringView text)
        {
            if (m_agent != nullptr)
                m_agent->SetText(text);
            if (m_agentItem != nullptr)
                m_agentItem->SetDisplayed(!text.IsEmpty());
        }
        [[nodiscard]] bool ShowsAgent() const { return m_agentItem != nullptr && m_agentItem->IsDisplayed(); }

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
        gui::Label* m_agent = nullptr;
    };
}
