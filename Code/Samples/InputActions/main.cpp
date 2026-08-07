// InputActions - the draconic.input consumer proof, now the WHOLE stack in one app:
// named actions (WASD/stick/touch move, Jump via key/pad/touch region), an exclusive Menu
// set (suppression + held-latching), the Flax-style smoothing, the engine TIME SCALE, the
// USER REBIND OVERLAY (capture-next-input, persisted to the user settings file, reset to
// default), and TOUCH virtual sticks/regions (Steam Deck) - all inspected and driven from
// a Dear ImGui debug panel. The moving square renders as the window clear color (position
// = color); the point is the input layer, not the drawing.

#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Runtime.Client/AppMain.h"
#include "imgui.h"
#include <cstdio>

import draconic.foundation;
import draconic.runtime;
import draconic.runtime.client;
import draconic.shell;
import draconic.runtime.desktop;
import draconic.shell.desktop;
import draconic.graphics;
import draconic.graphics.gpu;
import draconic.xml;
import draconic.xml.serialization;
import draconic.settings;
import draconic.imgui;
import draconic.input;
import draconic.engine.input;

namespace foundation = draconic::foundation;
namespace runtime = draconic::runtime;
namespace graphics = draconic::graphics;
namespace shell = draconic::shell;
namespace input = draconic::input;
namespace imgui = draconic::imgui;

using foundation::f32;
using foundation::u32;

namespace
{
    [[nodiscard]] input::InputMap MakeDefaultMap()
    {
        input::InputMap map;

        input::ActionSet gameplay;
        gameplay.name = foundation::String(u8"Gameplay");
        {
            input::Action move;
            move.name = foundation::String(u8"Move");
            move.kind = input::ActionKind::Axis2D;
            input::Binding wasd;
            wasd.source = input::BindingSource::Composite2D;
            wasd.negX = static_cast<u32>(shell::KeyCode::A);
            wasd.posX = static_cast<u32>(shell::KeyCode::D);
            wasd.negY = static_cast<u32>(shell::KeyCode::S);
            wasd.posY = static_cast<u32>(shell::KeyCode::W);
            move.bindings.PushBack(wasd);
            input::Binding stick;
            stick.source = input::BindingSource::GamepadStick;
            stick.code = static_cast<u32>(input::StickCode::Left);
            stick.invert = true; // stick +Y is down; the square's +Y is up
            move.bindings.PushBack(stick);
            input::Binding touch;
            touch.source = input::BindingSource::TouchStick;
            touch.regionX = 0.0f;
            touch.regionY = 0.3f; // left side, below the debug panel
            touch.regionW = 0.45f;
            touch.regionH = 0.7f;
            touch.stickRadius = 0.12f;
            touch.invert = true;
            move.bindings.PushBack(touch);
            move.processors.sensitivity = 6.0f;
            move.processors.gravity = 10.0f;
            move.processors.snap = true;
            move.processors.timeScale = true; // slow-mo slows movement
            gameplay.actions.PushBack(static_cast<input::Action&&>(move));
        }
        {
            input::Action jump;
            jump.name = foundation::String(u8"Jump");
            jump.kind = input::ActionKind::Button;
            input::Binding space;
            space.source = input::BindingSource::Key;
            space.code = static_cast<u32>(shell::KeyCode::Space);
            jump.bindings.PushBack(space);
            input::Binding pad;
            pad.source = input::BindingSource::GamepadButton;
            pad.code = 0;
            jump.bindings.PushBack(pad);
            input::Binding touch;
            touch.source = input::BindingSource::TouchButton;
            touch.regionX = 0.55f;
            touch.regionY = 0.55f; // bottom-right quadrant
            touch.regionW = 0.45f;
            touch.regionH = 0.45f;
            jump.bindings.PushBack(touch);
            gameplay.actions.PushBack(static_cast<input::Action&&>(jump));
        }
        map.sets.PushBack(static_cast<input::ActionSet&&>(gameplay));

        input::ActionSet menu;
        menu.name = foundation::String(u8"Menu");
        menu.priority = 10;
        {
            input::Action confirm;
            confirm.name = foundation::String(u8"Confirm");
            confirm.kind = input::ActionKind::Button;
            input::Binding space;
            space.source = input::BindingSource::Key;
            space.code = static_cast<u32>(shell::KeyCode::Space);
            confirm.bindings.PushBack(space);
            menu.actions.PushBack(static_cast<input::Action&&>(confirm));
        }
        map.sets.PushBack(static_cast<input::ActionSet&&>(menu));
        return map;
    }

    [[nodiscard]] const char* SourceLabel(input::BindingSource source)
    {
        switch (source)
        {
        case input::BindingSource::Key:
            return "Key";
        case input::BindingSource::MouseButton:
            return "MouseBtn";
        case input::BindingSource::MouseAxis:
            return "MouseAxis";
        case input::BindingSource::MouseDelta:
            return "MouseDelta";
        case input::BindingSource::GamepadButton:
            return "PadBtn";
        case input::BindingSource::GamepadAxis:
            return "PadAxis";
        case input::BindingSource::GamepadStick:
            return "PadStick";
        case input::BindingSource::Composite2D:
            return "Keys4";
        case input::BindingSource::TouchButton:
            return "TouchBtn";
        case input::BindingSource::TouchStick:
            return "TouchStick";
        }
        return "?";
    }

    class InputActionsApp final : public runtime::IApplication
    {
    public:
        void Configure(runtime::IApplicationHost& host) override
        {
            m_input = host.Ctx().AddSubsystem<input::InputSubsystem>(
                host.Shell() != nullptr ? host.Shell()->Input() : nullptr);
            if (auto* gfx = host.Graphics(); gfx != nullptr && gfx->Raw() != nullptr)
            {
                host.Ctx().AddSubsystem<imgui::ImguiSubsystem>(*gfx->Raw(), gfx->FramesInFlight());
            }
        }

        void OnStartup(runtime::IApplicationHost&) override
        {
            input::RegisterInputTypes();
            m_asset = MakeDefaultMap(); // "the asset": pristine defaults
            LoadOverlay();
            ApplyEffectiveMap();
            foundation::ConsoleWrite(
                u8"InputActions: WASD/stick/touch drives the clear color;\n"
                u8"  the ImGui panel has rebinding, time scale, and the menu toggle.\n");
        }

        void OnUpdate(runtime::IApplicationHost& host, f32 dt) override
        {
            input::ActionRuntime& actions = m_input->Runtime();

            if (auto* g = host.Ctx().GetSubsystem<imgui::ImguiSubsystem>())
            {
                g->NewFrame(host.Shell() != nullptr ? host.Shell()->Input() : nullptr, dt);
                DrawDebugPanel(host);
                DrawTouchOverlay(host);
            }

            // Rebind capture (started from the panel): first matching input wins; Esc cancels.
            if (m_capturing != nullptr)
            {
                auto* shellInput = host.Shell() != nullptr ? host.Shell()->Input() : nullptr;
                if (shellInput != nullptr && shellInput->Keyboard() != nullptr &&
                    shellInput->Keyboard()->IsKeyPressed(shell::KeyCode::Escape))
                {
                    m_capturing = nullptr;
                }
                else if (shellInput != nullptr)
                {
                    input::ShellInputSource devices(shellInput);
                    input::Binding captured;
                    if (input::CaptureBinding(devices, m_captureFilter, captured))
                    {
                        foundation::Array<input::Binding> replacement;
                        replacement.PushBack(captured);
                        m_overlay.Set(
                            u8"Gameplay",
                            foundation::StringView(reinterpret_cast<const foundation::utf8char*>(m_capturing)),
                            static_cast<foundation::Array<input::Binding>&&>(replacement));
                        m_capturing = nullptr;
                        SaveOverlay();
                        ApplyEffectiveMap();
                    }
                }
            }

            const foundation::Float2 move = actions.Value2D(m_move);
            m_x = foundation::Clamp(m_x + move.x * dt * 0.6f, 0.0f, 1.0f);
            m_y = foundation::Clamp(m_y + move.y * dt * 0.6f, 0.0f, 1.0f);
            if (actions.WasPressed(m_jump))
            {
                foundation::ConsoleWrite(u8"InputActions: Jump!\n");
            }
            if (actions.WasPressed(m_confirm))
            {
                foundation::ConsoleWrite(u8"InputActions: Confirm.\n");
            }
        }

        void OnRenderWindow(runtime::IApplicationHost& host, graphics::FrameContext& frame) override
        {
            frame.Clear(0.1f + 0.8f * m_x, 0.1f + 0.8f * m_y, 0.25f, 1.0f);
            if (auto* g = host.Ctx().GetSubsystem<imgui::ImguiSubsystem>())
            {
                g->Render(frame);
            }
        }

    private:
        // ---- the effective map = pristine asset + user overlay ----
        void ApplyEffectiveMap()
        {
            input::InputMap effective = m_asset;
            input::ApplyBindingOverrides(effective, m_overlay);
            m_input->SetMap(effective);
            m_move = m_input->Runtime().Resolve(u8"Move");
            m_jump = m_input->Runtime().Resolve(u8"Jump");
            m_confirm = m_input->Runtime().Resolve(u8"Confirm");
        }

        [[nodiscard]] static foundation::String OverlayPath()
        {
            return foundation::PathJoin(foundation::GetUserDataDirectory(u8"draconic").AsView(),
                                  u8"inputactions.rebinds.xml");
        }

        void LoadOverlay()
        {
            foundation::Result<foundation::Array<foundation::byte>> bytes = foundation::ReadFile(OverlayPath().AsView());
            if (!bytes.HasValue())
            {
                return;
            }
            foundation::MemoryStream stream;
            (void)stream.Write(bytes.Value().Data(), bytes.Value().Size());
            (void)stream.Seek(0, foundation::SeekOrigin::Begin);
            draconic::settings::Settings store;
            if (store.Load(stream, draconic::xml::XmlSerializerFactory()).IsOk())
            {
                if (const auto* section = store.Find<input::InputBindingOverrides>())
                {
                    m_overlay.overrides = section->overrides;
                    foundation::ConsoleWrite(u8"InputActions: loaded user rebinds.\n");
                }
            }
        }

        void SaveOverlay()
        {
            draconic::settings::Settings store;
            store.Section<input::InputBindingOverrides>().overrides = m_overlay.overrides;
            foundation::MemoryStream stream;
            if (store.Save(stream, draconic::xml::XmlSerializerFactory()).IsOk())
            {
                if (foundation::WriteFile(OverlayPath().AsView(), stream.Bytes()).IsOk())
                {
                    foundation::ConsoleWrite(u8"InputActions: rebinds saved.\n");
                }
            }
        }

        // ---- debug panel ----
        void DrawDebugPanel(runtime::IApplicationHost& host)
        {
            input::ActionRuntime& actions = m_input->Runtime();
            ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
            ImGui::Begin("Input");

            const foundation::Float2 move = actions.Value2D(m_move);
            ImGui::Text("Move  %+.2f %+.2f", static_cast<double>(move.x),
                        static_cast<double>(move.y));
            ImGui::Text("Jump  %s", actions.IsDown(m_jump) ? "DOWN" : "up");

            // Engine time scale: flagged actions (Move) scale, Jump's press does not.
            float scale = host.Ctx().TimeScale();
            if (ImGui::SliderFloat("time scale", &scale, 0.0f, 2.0f))
            {
                host.Ctx().SetTimeScale(scale);
            }

            // Exclusive menu toggle (suppression + held latching, live).
            const bool menuOpen = actions.ExclusiveDepth() > 0;
            if (ImGui::Button(menuOpen ? "Close Menu (gameplay resumes)"
                                       : "Open Menu (gameplay suppressed)"))
            {
                if (menuOpen)
                {
                    actions.PopExclusiveSet();
                }
                else
                {
                    actions.PushExclusiveSet(u8"Menu");
                }
            }

            ImGui::Separator();
            ImGui::Text("Rebinding (overlay over the pristine defaults):");
            DrawRebindRow(host, "Jump");
            DrawRebindRow(host, "Move");
            if (ImGui::Button("Reset ALL to defaults"))
            {
                m_overlay.overrides.Clear();
                m_capturing = nullptr;
                SaveOverlay();
                ApplyEffectiveMap();
            }
            if (m_capturing != nullptr)
            {
                ImGui::TextColored(ImVec4(1, 0.8f, 0.2f, 1),
                                   "Press the new input for %s (Esc cancels)...", m_capturing);
            }
            ImGui::End();
        }

        void DrawRebindRow(runtime::IApplicationHost&, const char* actionName)
        {
            // Current EFFECTIVE first binding, for display.
            const input::InputMap& map = m_input->Runtime().Map();
            const input::Binding* first = nullptr;
            const foundation::StringView wanted(reinterpret_cast<const foundation::utf8char*>(actionName));
            for (const input::ActionSet& set : map.sets)
            {
                for (const input::Action& action : set.actions)
                {
                    if (action.name.AsView() == wanted)
                    {
                        if (!action.bindings.IsEmpty())
                        {
                            first = &action.bindings[0];
                        }
                        break;
                    }
                }
            }
            char label[96];
            std::snprintf(label, sizeof(label), "%s: %s#%u", actionName,
                          first != nullptr ? SourceLabel(first->source) : "(unbound)",
                          first != nullptr ? first->code : 0u);
            ImGui::TextUnformatted(label);
            ImGui::SameLine(180.0f);
            char id[64];
            std::snprintf(id, sizeof(id), "Rebind##%s", actionName);
            if (ImGui::Button(id))
            {
                m_capturing = actionName;
                m_captureFilter = input::CaptureFilter{};
                if (wanted == foundation::StringView(u8"Move"))
                {
                    // Axis2D: sticks only (keyboard stays on the default composite).
                    m_captureFilter.keys = false;
                    m_captureFilter.mouseButtons = false;
                    m_captureFilter.gamepadButtons = false;
                    m_captureFilter.gamepadSticks = true;
                }
            }
            ImGui::SameLine();
            std::snprintf(id, sizeof(id), "Reset##%s", actionName);
            if (ImGui::Button(id))
            {
                m_overlay.Clear(u8"Gameplay", wanted);
                SaveOverlay();
                ApplyEffectiveMap();
            }
        }

        // Touch region visualization: rectangles for the stick/button zones + live points.
        void DrawTouchOverlay(runtime::IApplicationHost& host)
        {
            ImDrawList* draw = ImGui::GetForegroundDrawList();
            const ImVec2 size = ImGui::GetIO().DisplaySize;
            auto rect = [&](f32 x, f32 y, f32 w, f32 h, ImU32 color)
            {
                draw->AddRect(ImVec2(x * size.x, y * size.y),
                              ImVec2((x + w) * size.x, (y + h) * size.y), color, 0, 0, 2.0f);
            };
            rect(0.0f, 0.3f, 0.45f, 0.7f, IM_COL32(90, 160, 255, 120));    // stick zone
            rect(0.55f, 0.55f, 0.45f, 0.45f, IM_COL32(255, 140, 90, 120)); // jump zone
            auto* shellInput = host.Shell() != nullptr ? host.Shell()->Input() : nullptr;
            if (shellInput != nullptr && shellInput->Touch() != nullptr)
            {
                shell::ITouch* touch = shellInput->Touch();
                const foundation::i32 count = touch->TouchCount();
                for (foundation::i32 i = 0; i < count; ++i)
                {
                    shell::TouchPoint point;
                    if (touch->GetTouchPoint(i, point))
                    {
                        draw->AddCircle(ImVec2(point.x * size.x, point.y * size.y), 24.0f,
                                        IM_COL32(255, 255, 255, 200), 0, 3.0f);
                    }
                }
            }
        }

        input::InputSubsystem* m_input = nullptr;
        input::InputMap m_asset;                // pristine defaults
        input::InputBindingOverrides m_overlay; // the user's rebinds (persisted)
        input::ActionRef m_move;
        input::ActionRef m_jump;
        input::ActionRef m_confirm;
        const char* m_capturing = nullptr; // action being rebound (static literal)
        input::CaptureFilter m_captureFilter;
        f32 m_x = 0.5f;
        f32 m_y = 0.5f;
    };
}

DRACONIC_APP_MAIN(InputActionsApp)
