// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GameUiSandbox - a game's screens on the engine GUI (Engine.GUI, GUI.Gamekit). A DefaultApplication
// (same host as ScriptPlayground / PhysicsPlayground: the GUI, rendering and input come with it) that
// builds two screens from markup and pushes them on the GUI's screen stack:
//   - a HUD: a menu of buttons (the arrows or the pad move through it, Enter or South presses), a
//     health bar that drains and heals with a tween, a score that rolls up through a data tween
//     (a whole number, so it rolls in whole steps), a prompt naming the key or button bound to the
//     "Deliver" action, and toasts on "Show toast";
//   - a pause menu pushed over it, blocking it, which back (Escape) pops.
// The old game kit's MenuList, Bar and Ticker have no widget here (Specs/gui-lab-evolution-3.md,
// section K): markup, navigation and commands make the menu, a progress bar and a tween the bar, a
// data tween the ticker. Escape on the HUD exits.

#include "Core/Prelude.h"
#include "Runtime.Client/AppMain.h"

import foundation.core;
import foundation.runtime;
import foundation.runtime.client;
import foundation.runtime.desktop;
import foundation.shell;
import foundation.shell.desktop;
import foundation.graphics;
import foundation.graphics.gpu;
import foundation.scene;
import engine.scene;
import engine.render;
import engine.defaultapp;
import engine.gui;            // GuiSubsystem (the screen tier: Screens(), ScreenRoot())
import foundation.gui;           // the widgets, the toasts
import foundation.gui.resources; // GuiDocument (the documents made in code)
import foundation.gui.gamekit; // Screen, ScreenStack
import foundation.input;      // InputMap

namespace core = foundation::core;
namespace runtime = foundation::runtime;
namespace shell = foundation::shell;
namespace scene = foundation::scene;
namespace gui = foundation::gui;
namespace input = foundation::input;

namespace
{
    // This binary's composition root: the ONE ambient-allocator decision here.
    [[nodiscard]] foundation::core::IAllocator& AppRoot() noexcept
    {
        return foundation::core::DefaultAllocator();
    }
}

using core::f32;
using core::i64;
using core::RefPtr;
using core::String;
using core::StringView;

namespace
{
    // The HUD: the title, the prompt, the score and the health (bound to the GUI's data), the menu.
    constexpr StringView kHudMarkup =
        u8"<Panel flex-direction=\"column\" gap=\"16\" padding=\"40 48\" flex-grow=\"1\" background-color=\"rgb(18, 20, 26)\">"
        u8"  <Label text=\"Game UI Sandbox\" font-size=\"28\"/>"
        u8"  <Panel flex-direction=\"row\" align-items=\"center\" gap=\"8\">"
        u8"    <Prompt action=\"Deliver\"/><Label text=\"Deliver\"/>"
        u8"  </Panel>"
        u8"  <Panel flex-direction=\"row\" align-items=\"center\" gap=\"8\">"
        u8"    <Label text=\"Score\" width=\"64\"/><Label text=\"{{score}}\" font-size=\"20\"/>"
        u8"  </Panel>"
        u8"  <Panel flex-direction=\"row\" align-items=\"center\" gap=\"8\">"
        u8"    <Label text=\"Health\" width=\"64\"/><ProgressBar id=\"health\" width=\"220\" value=\"{{health}}\"/>"
        u8"  </Panel>"
        u8"  <Panel flex-direction=\"column\" gap=\"6\" width=\"240\">"
        u8"    <Button id=\"menu-score\" text=\"Score +250\"/>"
        u8"    <Button id=\"menu-damage\" text=\"Take damage\"/>"
        u8"    <Button id=\"menu-heal\" text=\"Heal\"/>"
        u8"    <Button id=\"menu-toast\" text=\"Show toast\"/>"
        u8"    <Button id=\"menu-pause\" text=\"Pause menu\"/>"
        u8"    <Button id=\"menu-quit\" text=\"Quit\"/>"
        u8"  </Panel>"
        u8"</Panel>";

    // The pause menu: a scrim over the HUD, a card in its middle.
    constexpr StringView kPauseMarkup =
        u8"<Panel background-color=\"rgba(0, 0, 0, 0.55)\" justify-content=\"center\" align-items=\"center\">"
        u8"  <Panel flex-direction=\"column\" align-items=\"center\" gap=\"16\" padding=\"28 40\""
        u8"         background-color=\"rgba(31, 33, 43, 0.98)\" border-radius=\"8\">"
        u8"    <Label text=\"Paused\" font-size=\"26\"/>"
        u8"    <Button id=\"pause-resume\" text=\"Resume\" width=\"200\"/>"
        u8"    <Button id=\"pause-quit\" text=\"Quit\" width=\"200\"/>"
        u8"  </Panel>"
        u8"</Panel>";

    // The node under `node` with `id`, or null.
    [[nodiscard]] gui::Node* FindById(gui::Node& node, StringView id)
    {
        if (const auto* widget = core::Cast<gui::UIWidget>(&node); widget != nullptr && widget->GetId() == id)
            return &node;
        for (core::usize i = 0; i < node.ChildCount(); ++i)
            if (gui::Node* found = FindById(*node.GetChildAt(i), id))
                return found;
        return nullptr;
    }

    class GameUiSandbox final : public engine::runtime::DefaultApplication
    {
    public:
        void OnLaunch(runtime::IApplicationHost& host) override
        {
            if (host.Ctx().GetSubsystem<engine::scene::SceneSubsystem>() == nullptr)
                return;
            // A minimal scene and camera so a frame renders; the HUD covers it.
            m_scene = PrimaryScenes().CreateScene(u8"gameui");
            m_camera = m_scene->CreateEntity(u8"camera");
            if (auto* cameras = m_scene->GetSystem<engine::render::CameraComponentManager>())
                cameras->Add(m_camera);
            m_scene->Start();

            m_gui = host.Ctx().GetSubsystem<engine::gui::GuiSubsystem>();
            if (m_gui == nullptr || m_gui->ScreenRoot() == nullptr || m_gui->Screens() == nullptr)
                return;
            gui::SceneRoot& root = *m_gui->ScreenRoot();

            // The game's bindings, which the prompt names (the engine's GUI reads the game's runtime).
            BuildInputMap();
            Instance().InputRuntime().SetMap(m_map);

            // The values the HUD shows: a whole score (so its tween rolls in whole steps), the health.
            root.GetContext().GetData().Set(u8"score", core::Variant::From<i64>(0));
            root.GetContext().GetData().Set(u8"health", core::Variant::From<core::f64>(1.0));

            // Toasts over the screens.
            m_toasts = core::MakeRef<gui::ToastHost>(AppRoot());
            root.AddChild(m_toasts.Get());

            PushHud();
            core::ConsoleWrite(u8"GameUiSandbox: the arrows (or the pad) move through the menu, Enter or South presses. "
                               u8"Escape backs out of the pause menu, else exits.\n");
        }

        void OnUpdate(runtime::IApplicationHost& host, f32 dt) override
        {
            engine::runtime::DefaultApplication::OnUpdate(host, dt);
            if (m_quit)
            {
                host.RequestExit(0);
                return;
            }
            auto* in = host.Shell() != nullptr ? host.Shell()->Input() : nullptr;
            if (in == nullptr || m_gui == nullptr || m_gui->Screens() == nullptr)
                return;
            // Escape backs out of the pause menu first, else exits.
            if (in->Keyboard()->IsKeyPressed(shell::KeyCode::Escape) && m_gui->Screens()->Count() <= 1)
                host.RequestExit(0);
            // The "Deliver" action the prompt names: E fires it (a toast, for the demo).
            if (in->Keyboard()->IsKeyPressed(shell::KeyCode::E))
                Toast(u8"Delivered!", gui::ToastSeverity::Info, 1.5f);
        }

    private:
        void BuildInputMap()
        {
            input::ActionSet set;
            set.name = String(u8"Gameplay");
            input::Action deliver;
            deliver.name = String(u8"Deliver");
            input::Binding key;
            key.source = input::BindingSource::Key;
            key.code = static_cast<core::u32>(shell::KeyCode::E);
            deliver.bindings.PushBack(key);
            input::Binding pad;
            pad.source = input::BindingSource::GamepadButton;
            pad.code = static_cast<core::u32>(shell::GamepadButton::South);
            deliver.bindings.PushBack(pad);
            set.actions.PushBack(core::Move(deliver));
            m_map.sets.PushBack(core::Move(set));
        }

        // A screen built from `markup` and pushed: the pushed screen, or null.
        gui::Screen* PushMarkup(StringView markup, RefPtr<gui::GuiDocument>& document)
        {
            document = core::MakeRef<gui::GuiDocument>(AppRoot());
            document->markup = String(markup);
            RefPtr<gui::Node> tree = m_gui->BuildDocument(*document);
            return tree ? m_gui->Screens()->Push(*tree) : nullptr;
        }

        void OnClick(gui::Node& screen, StringView id, core::Function<void()> click)
        {
            if (auto* button = core::Cast<gui::Button>(FindById(screen, id)))
                button->SetOnClick(core::Move(click));
        }

        void PushHud()
        {
            gui::Screen* hud = PushMarkup(kHudMarkup, m_hudDocument);
            if (hud == nullptr)
                return;
            hud->SetCovers(true);  // over the scene, whole
            hud->SetBack(String(u8"none")); // back on the HUD is the host's (Escape exits)
            GameUiSandbox* self = this;
            gui::SceneRoot& root = *m_gui->ScreenRoot();
            OnClick(*hud, u8"menu-score", [self, &root]()
                    {
                        self->m_score += 250;
                        root.GetContext().AnimateData(u8"score", static_cast<core::f64>(self->m_score), 0.6f);
                    });
            OnClick(*hud, u8"menu-damage", [self, &root]()
                    {
                        self->m_health = core::Max(0.0f, self->m_health - 0.2f);
                        root.GetContext().AnimateData(u8"health", self->m_health, 0.35f);
                    });
            OnClick(*hud, u8"menu-heal", [self, &root]()
                    {
                        self->m_health = core::Min(1.0f, self->m_health + 0.3f);
                        root.GetContext().AnimateData(u8"health", self->m_health, 0.35f);
                    });
            OnClick(*hud, u8"menu-toast", [self]() { self->Toast(u8"Objective complete!", gui::ToastSeverity::Success, 3.0f); });
            OnClick(*hud, u8"menu-pause", [self]() { self->PushPause(); });
            OnClick(*hud, u8"menu-quit", [self]() { self->m_quit = true; });
        }

        void PushPause()
        {
            gui::Screen* pause = PushMarkup(kPauseMarkup, m_pauseDocument);
            if (pause == nullptr)
                return;
            pause->SetBlocks(true); // the HUD under it shows, and takes no input
            GameUiSandbox* self = this;
            OnClick(*pause, u8"pause-resume", [self]() { (void)self->m_gui->Screens()->Pop(); });
            OnClick(*pause, u8"pause-quit", [self]() { self->m_quit = true; });
        }

        void Toast(StringView message, gui::ToastSeverity severity, f32 seconds)
        {
            if (!m_toasts)
                return;
            gui::ToastRequest r;
            r.Title = String(message);
            r.Severity = severity;
            r.Duration = seconds;
            (void)m_toasts->Post(core::Move(r));
        }

        scene::Scene* m_scene = nullptr;
        scene::EntityHandle m_camera{};
        engine::gui::GuiSubsystem* m_gui = nullptr;
        RefPtr<gui::ToastHost> m_toasts;
        RefPtr<gui::GuiDocument> m_hudDocument;
        RefPtr<gui::GuiDocument> m_pauseDocument;
        input::InputMap m_map;
        i64 m_score = 0;
        f32 m_health = 1.0f;
        bool m_quit = false;
    };
}

APP_MAIN(GameUiSandbox)
