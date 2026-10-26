// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The Game tab from its markup (Data/Editor/Game.gml) on a headless host, no game running: the
// kit's transport (Play while stopped; Pause and Stop while it runs or waits; the state named;
// a Play waiting for the cook said so and cancelled by Stop), the resolutions in the dropdown, the
// phone kept with the project, and the debugger opened by its toggle.

#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.runtime;
import foundation.runtime.client; // IApplicationHost
import foundation.shell;
import foundation.graphics;
import foundation.settings;
import foundation.gui;
import foundation.gui.toolkit;
import editor.core;
import editor.play;

using namespace foundation::core;
using namespace editor;
namespace runtime = foundation::runtime;
namespace gui = foundation::gui;

namespace
{
    // A host with an empty context: no renderer, no window, no game.
    class BareHost final : public runtime::IApplicationHost
    {
    public:
        ~BareHost() override { m_context.Shutdown(); }
        runtime::Context& Ctx() noexcept override { return m_context; }
        foundation::shell::IShell* Shell() noexcept override { return nullptr; }
        foundation::graphics::GraphicsDevice* Graphics() noexcept override { return nullptr; }
        foundation::graphics::RenderWindow* MainRenderWindow() noexcept override { return nullptr; }
        foundation::graphics::RenderWindow* OpenWindow(const foundation::shell::WindowSettings&, const foundation::graphics::RenderWindowDesc&) override
        {
            return nullptr;
        }
        void CloseWindow(foundation::graphics::RenderWindow*) override {}
        void RequestExit(int) override {}

    private:
        runtime::Context m_context{DefaultAllocator()};
    };

    RefPtr<gui::Node> LoadMarkup(StringView relative)
    {
        const String path = PathJoin(PathJoin(StringView(reinterpret_cast<const char8_t*>(RAPTOR_SOURCE_DIR)), u8"Data").AsView(), relative);
        Result<Array<byte>> bytes = ReadFile(path.AsView());
        REQUIRE(bytes.HasValue());
        gui::RegisterGuiElements();
        gui::RegisterToolkitElements();
        static const gui::ElementSet elements = gui::ElementSet::All();
        gui::MarkupLoader loader(elements, DefaultAllocator());
        return loader.LoadFromString(StringView(reinterpret_cast<const utf8char*>(bytes.Value().Data()), bytes.Value().Size()));
    }

    template <typename T>
    T* FindId(gui::Node& node, StringView id)
    {
        if (auto* widget = Cast<gui::UIWidget>(&node); widget != nullptr && widget->GetId() == id)
            return Cast<T>(&node);
        for (usize i = 0; i < node.ChildCount(); ++i)
            if (T* found = FindId<T>(*node.GetChildAt(i), id))
                return found;
        return nullptr;
    }

    struct Bed
    {
        String dir = String(u8"scratch_game_page_project");
        UniquePtr<EditorProject> project;
        foundation::settings::Settings store{DefaultAllocator()};
        EditorContext context{DefaultAllocator()};
        BareHost host;
        UniquePtr<GameEditorPage> page;

        Bed()
        {
            (void)RemoveDirectoryRecursive(dir.AsView());
            REQUIRE(EditorProject::Create(DefaultAllocator(), dir.AsView(), u8"P").IsOk());
            project = EditorProject::Open(DefaultAllocator(), dir.AsView());
            REQUIRE(static_cast<bool>(project));
            context.SetProject(project.Get());
            context.SetProjectEditorSettings(&store);
            GameEditorPage::Services services;
            services.loadMarkup = [](StringView path) { return LoadMarkup(path); };
            page = MakeUnique<GameEditorPage>(DefaultAllocator(), context, host, Move(services), nullptr, nullptr, u8"game-page");
        }
        ~Bed()
        {
            page->OnClose();
            page.Reset();
            context.SetProjectEditorSettings(nullptr);
            context.SetProject(nullptr);
            project.Reset();
            (void)RemoveDirectoryRecursive(dir.AsView());
        }
        template <typename T>
        T* Find(StringView id) { return FindId<T>(*page->Content(), id); }
    };
}

TEST_CASE("game tab: the kit's transport over a stopped run; a Play waiting for the cook says so and Stop cancels it")
{
    Bed bed;
    auto* play = bed.Find<gui::ButtonBase>(u8"game-play");
    auto* pause = bed.Find<gui::ButtonBase>(u8"game-pause");
    auto* stop = bed.Find<gui::ButtonBase>(u8"game-stop");
    auto* state = bed.Find<gui::Label>(u8"game-state");
    auto* cook = bed.Find<gui::Label>(u8"game-cook");
    REQUIRE(play != nullptr);
    REQUIRE(pause != nullptr);
    REQUIRE(stop != nullptr);
    REQUIRE(state != nullptr);
    REQUIRE(cook != nullptr);
    CHECK(play->IsEnabled());
    CHECK_FALSE(pause->IsEnabled());
    CHECK_FALSE(stop->IsEnabled());
    CHECK(state->GetText() == StringView(u8"Stopped"));
    CHECK_FALSE(cook->IsDisplayed());

    // The cook is busy: Play waits for it, and says so.
    bed.context.CookBusy = []() { return true; };
    (void)play->HandleActivate();
    bed.page->OnUpdate(bed.host, 1.0f / 60.0f);
    CHECK(bed.page->IsStarting());
    CHECK(cook->IsDisplayed());
    CHECK_FALSE(play->IsEnabled());
    CHECK(stop->IsEnabled());
    (void)stop->HandleActivate();
    bed.page->OnUpdate(bed.host, 1.0f / 60.0f);
    CHECK_FALSE(bed.page->IsStarting());
    CHECK_FALSE(cook->IsDisplayed());
    CHECK(play->IsEnabled());
}

TEST_CASE("game tab: the resolutions in the dropdown, a pick and the phone kept with the project; the debugger by its toggle")
{
    Bed bed;
    auto* resolution = bed.Find<gui::ComboBox>(u8"game-resolution");
    REQUIRE(resolution != nullptr);
    const Span<const GameResolutionChoice> choices = bed.page->ResolutionChoices();
    REQUIRE(choices.Size() >= 2u);
    CHECK(resolution->ItemCount() == choices.Size());
    CHECK(resolution->GetSelectedText() == choices[0].label.AsView()); // the project's
    resolution->SelectByUser(1);
    CHECK(bed.page->ResolutionKey() == choices[1].key.AsView());
    const GamePageSettings* saved = bed.store.Find<GamePageSettings>();
    REQUIRE(saved != nullptr);
    CHECK(saved->resolutionKey == choices[1].key);

    auto* phone = bed.Find<gui::ButtonBase>(u8"game-phone");
    REQUIRE(phone != nullptr);
    bed.page->SelectPhonePreview(true);
    CHECK(phone->IsChecked());
    CHECK(bed.page->PhonePreview());
    CHECK(bed.store.Find<GamePageSettings>()->phonePreview);

    auto* debugger = bed.Find<gui::Node>(u8"game-debugger");
    auto* toggle = bed.Find<gui::ButtonBase>(u8"game-debugger-toggle");
    auto* debuggerState = bed.Find<gui::Label>(u8"game-debugger-state");
    REQUIRE(debugger != nullptr);
    REQUIRE(toggle != nullptr);
    REQUIRE(debuggerState != nullptr);
    CHECK_FALSE(debugger->IsDisplayed()); // it takes room the game wants
    CHECK(debuggerState->GetText() == StringView(u8"Not running"));
    (void)toggle->HandleActivate();
    CHECK(toggle->IsChecked());
    CHECK(debugger->IsDisplayed());
}
