// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor App - :preview_pane partition
//
// PreviewPane: binds the 3D preview of a preview page's markup (Data/Editor/PreviewPage.xml):
// Editor.Preview's PreviewViewport in `preview-stage`; Frame (`preview-frame`, and F over the
// preview), which tells the page to frame what it shows; the page's own controls, placed after
// `preview-own`; and what shows while there is nothing to preview (`preview-empty`, with its text
// in `preview-empty-title` and `preview-empty-text`). Without a host (a test) it has no viewport:
// the page still builds and shows the empty state.

module;
#include "Core/Prelude.h"

export module editor.app:preview_pane;

import foundation.core;
import foundation.runtime.client;
import foundation.graphics;
import foundation.gui;
import foundation.gui.hosting;
import editor.preview;
import :asset_page; // FindById, AssetPageServices

using namespace foundation::core;

export namespace editor::app
{
    namespace gui = foundation::gui;

    class PreviewPane
    {
    public:
        PreviewPane(gui::Node& root, const AssetPageServices& services, StringView sceneName)
        {
            if (services.host != nullptr)
                m_viewport = MakeUnique<PreviewViewport>(EditorRootAllocator(), EditorRootAllocator(), *services.host, services.gui, sceneName);
            m_stage = FindById<gui::Node>(root, u8"preview-stage");
            m_empty = FindById<gui::Node>(root, u8"preview-empty");
            m_emptyTitle = FindById<gui::Label>(root, u8"preview-empty-title");
            m_emptyText = FindById<gui::Label>(root, u8"preview-empty-text");
            m_own = FindById<gui::Node>(root, u8"preview-own");
            if (m_stage != nullptr && m_viewport)
                m_stage->AddChild(m_viewport->View().Get());
            PreviewPane* self = this;
            if (auto* frame = FindById<gui::ButtonBase>(root, u8"preview-frame"))
                frame->SetOnClick([self]()
                                  {
                                      if (self->OnFrame)
                                          self->OnFrame();
                                  });
            ShowEmpty(StringView{}, StringView{});
        }
        PreviewPane(const PreviewPane&) = delete;
        PreviewPane& operator=(const PreviewPane&) = delete;

        /// The preview (null without a host).
        [[nodiscard]] PreviewViewport* Viewport() const noexcept { return m_viewport.Get(); }
        /// Called when Frame is clicked or F pressed over the preview, so the page frames what it shows.
        Function<void()> OnFrame;

        /// Shows the preview (its stage) and hides the empty state.
        void Show()
        {
            m_showing = true;
            if (m_stage != nullptr)
                m_stage->SetDisplayed(m_viewport.Get() != nullptr);
            if (m_empty != nullptr)
                m_empty->SetDisplayed(m_viewport.Get() == nullptr);
        }
        /// Shows the empty state: `title` and `why` in place of the preview (empty: the markup's text).
        void ShowEmpty(StringView title, StringView why)
        {
            m_showing = false;
            if (m_stage != nullptr)
                m_stage->SetDisplayed(false);
            if (m_empty != nullptr)
                m_empty->SetDisplayed(true);
            if (m_emptyTitle != nullptr && !title.IsEmpty())
                m_emptyTitle->SetText(title);
            if (m_emptyText != nullptr && !why.IsEmpty())
                m_emptyText->SetText(why);
        }
        [[nodiscard]] bool Showing() const noexcept { return m_showing; }

        /// Adds `control` to the preview's own controls, after those added before it.
        void AddControl(gui::Node& control)
        {
            gui::Node* bar = m_own != nullptr ? m_own->GetParent() : nullptr;
            if (bar == nullptr)
                return;
            bar->AddChildAt(&control, bar->GetChildIndex(m_own) + 1 + m_controls);
            ++m_controls;
        }

        /// Called once a frame from the page's OnUpdate: updates the camera and handles F.
        void Update(f32 dt)
        {
            if (m_viewport && m_viewport->Update(dt) && OnFrame)
                OnFrame();
        }
        /// Called from the page's OnRenderWindow.
        void Render(foundation::graphics::FrameContext& frame)
        {
            if (m_viewport && m_showing)
                m_viewport->RenderFrame(frame);
        }
        /// Called from the page's OnClose: the preview's scene is released while the device lives.
        void Close()
        {
            if (m_viewport)
                m_viewport->Shutdown();
        }

    private:
        UniquePtr<PreviewViewport> m_viewport;
        gui::Node* m_stage = nullptr;
        gui::Node* m_empty = nullptr;
        gui::Label* m_emptyTitle = nullptr;
        gui::Label* m_emptyText = nullptr;
        gui::Node* m_own = nullptr;
        usize m_controls = 0;
        bool m_showing = false;
    };
}
