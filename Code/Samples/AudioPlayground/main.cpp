// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// AudioPlayground - an audio consumer: an ambient chord loop on the Music bus plus four looping 3D
// emitters around the origin (distinct pitches of the same clip, through real pitch resampling),
// all authored as AudioSource components with autoplay on a scene. The listener is an
// AudioListenerComponent on the fly-camera entity, and clicks fire positional one-shots (random
// pitch and position) through the engine-global PlayOneShot3D. Fly around (WASD, RMB look) to hear
// attenuation, pan and doppler as you strafe past emitters; the ImGui panel has Master, Effects and
// Music bus sliders and live voice counts. Emitters draw as debug wire spheres. It runs without an
// audio device too (Null mode: a warning logs and everything else works, silently).

#include "Core/Prelude.h"
#include "Core/Log/Log.h"
#include "Runtime.Client/AppMain.h"
#include "imgui.h"
#include <cmath>

import foundation.core;
import foundation.vfs; // DataPath (engine data lives under the app-resolved data root)
import foundation.runtime;
import foundation.runtime.client;
import engine.defaultapp;
import foundation.shell;
import foundation.runtime.desktop;
import foundation.shell.desktop;
import foundation.graphics;
import foundation.graphics.gpu;
import foundation.scene;
import engine.scene;
import engine.render;
import extensions.imgui;
import foundation.audio;
import engine.audio;

#include "../Common/FlyCamera.h" // after the imports: uses foundation::core/runtime types

namespace core = foundation::core;
namespace runtime = foundation::runtime;
namespace graphics = foundation::graphics;
namespace shell = foundation::shell;
namespace scene = foundation::scene;
namespace audio = foundation::audio;
namespace imgui = extensions::imgui;

using core::f32;

namespace
{
    [[nodiscard]] core::RefPtr<audio::AudioClip> LoadClipFromFile(core::StringView path,
                                                                  bool loop = false)
    {
        core::Result<core::Array<core::byte>> bytes = core::ReadFile(path);
        if (!bytes.HasValue())
        {
            LOG_ERROR(u8"AudioPlayground", u8"missing sample data: {}", path);
            return {};
        }
        audio::AudioClipMetadata metadata;
        if (!audio::ProbeAudioClipMetadata(
                core::Span<const core::byte>(bytes.Value().Data(), bytes.Value().Size()), metadata))
        {
            return {};
        }
        core::RefPtr<audio::AudioClip> clip =
            core::MakeRef<audio::AudioClip>(foundation::core::DefaultAllocator());
        clip->channels = metadata.channels;
        clip->sampleRate = metadata.sampleRate;
        clip->frameCount = metadata.frameCount;
        clip->durationSeconds = metadata.durationSeconds;
        clip->loop = loop;
        clip->encodedData = core::Move(bytes.Value());
        return clip;
    }

    class PlaygroundApp final : public engine::runtime::DefaultApplication
    {
    public:
        void Configure(runtime::IApplicationHost& host) override
        {
            engine::runtime::DefaultApplication::Configure(host); // registers AudioSubsystem
            if (auto* gfx = host.Graphics(); gfx != nullptr && gfx->Raw() != nullptr)
            {
                host.Subsystems().AddSubsystem<imgui::ImguiSubsystem>(*gfx->Raw(), gfx->FramesInFlight(),
                                                                    DataFileSystem());
            }
        }

        void OnLaunch(runtime::IApplicationHost& host) override
        {
            auto* scenes = host.Subsystems().GetSubsystem<engine::scene::SceneSubsystem>();
            if (scenes == nullptr || Audio() == nullptr)
            {
                return;
            }
            m_scene = PrimaryScenes().CreateScene(u8"audio-playground");

            const core::String dataDir =
                foundation::vfs::DataPath(DataRoot(), u8"Samples/Audio/playground");
            m_ambient =
                LoadClipFromFile(core::PathJoin(dataDir.AsView(), u8"ambient_loop.wav"), true);
            m_beepHigh = LoadClipFromFile(core::PathJoin(dataDir.AsView(), u8"beep_high.wav"));
            m_beepLow = LoadClipFromFile(core::PathJoin(dataDir.AsView(), u8"beep_low.wav"), true);
            m_click = LoadClipFromFile(core::PathJoin(dataDir.AsView(), u8"click.wav"));

            // The camera entity is the listener (its AudioListenerComponent drives the engine).
            m_camera = m_scene->CreateEntity(u8"camera");
            if (auto* cameras = m_scene->GetSystem<engine::render::CameraComponentManager>())
            {
                cameras->Add(m_camera);
            }
            m_scene->GetSystem<engine::audio::AudioListenerComponentManager>()->Add(m_camera);
            m_fly.position = core::Float3{0.0f, 2.0f, 14.0f};
            m_fly.moveSpeed = 8.0f;
            m_fly.fastSpeed = 25.0f;

            // Ambient pad: a looping streamless clip on the Music bus (autoplay, 2D).
            {
                scene::EntityHandle e = m_scene->CreateEntity(u8"ambient");
                engine::audio::AudioSourceComponent& c =
                    m_scene->GetSystem<engine::audio::AudioSourceComponentManager>()->Add(e);
                c.clip = m_ambient;
                c.bus = audio::AudioBus::Music;
                c.spatial = false;
                c.loop = true;
                c.autoPlay = true;
                // A quiet bed, so positional sources read over it: the pad shares the beeps'
                // spectrum, so level is what separates them.
                c.volume = 0.35f;
            }

            // Four 3D emitters around the origin: the same low-beep clip at four pitches
            // (real resampling), looping and positioned. Fly past to hear pan and attenuation,
            // and strafe quickly for doppler.
            // Pitches keep every emitter >= ~250 Hz (laptop-speaker floor) and distinct.
            const f32 pitches[4] = {0.75f, 1.0f, 1.5f, 2.0f};
            for (int i = 0; i < 4; ++i)
            {
                const f32 angle = 3.14159265f * 0.5f * static_cast<f32>(i);
                scene::EntityHandle e = m_scene->CreateEntity(u8"emitter");
                m_scene->SetLocalPosition(
                    e, core::Float3{10.0f * std::cos(angle), 1.5f, 10.0f * std::sin(angle)});
                engine::audio::AudioSourceComponent& c =
                    m_scene->GetSystem<engine::audio::AudioSourceComponentManager>()->Add(e);
                c.clip = m_beepLow;
                c.loop = true;
                c.autoPlay = true;
                c.spatial = true;
                c.pitch = pitches[i];
                c.volume = 0.7f;
                c.minDistance = 2.0f;
                c.maxDistance = 60.0f;
                c.dopplerFactor = 1.0f;
                m_emitters.PushBack(e);
            }

            // Reverb zone: stand near the origin to hear the 'cave'. The tail
            // fades in across the zone's edge band and dries out as you fly away.
            {
                scene::EntityHandle zone = m_scene->CreateEntity(u8"cave-zone");
                auto& reverb =
                    m_scene->GetSystem<engine::audio::AudioReverbZoneComponentManager>()->Add(zone);
                reverb.radius = 12.0f;
                reverb.edgeFade = 0.4f;
                reverb.roomSize = 0.8f;
                reverb.damping = 0.2f;
                reverb.wetLevel = 0.6f;
            }

            // LMB one-shots fire through a SoundCue: three weighted variants with
            // pitch jitter, and no two consecutive shots pick the same clip.
            m_shotCue = core::MakeRef<audio::SoundCue>(foundation::core::DefaultAllocator());
            m_shotCue->variants.PushBack(audio::SoundCueVariant{m_beepHigh, 3.0f});
            m_shotCue->variants.PushBack(audio::SoundCueVariant{m_click, 2.0f});
            m_shotCue->variants.PushBack(audio::SoundCueVariant{m_beepLow, 1.0f});
            m_shotCue->pitchMin = 0.85f;
            m_shotCue->pitchMax = 1.25f;

            m_scene->Start();
            m_scene->SetSimulationEnabled(true);
            if (Audio()->Engine() != nullptr && Audio()->Engine()->IsHeadless())
            {
                core::ConsoleWrite(
                    u8"AudioPlayground: NO audio device - running silent (Null mode).\n");
            }
            core::ConsoleWrite(
                u8"AudioPlayground: WASD/RMB-look fly. LMB = positional one-shot at the\n"
                u8"crosshair distance (random pitch). Space = UI click one-shot. T = pause\n"
                u8"scene simulation (fades the scene's voices). Esc quits.\n");
        }

        void OnUpdate(runtime::IApplicationHost& host, f32 deltaTime) override
        {
            engine::runtime::DefaultApplication::OnUpdate(host, deltaTime);
            m_fly.Update(host, deltaTime);
            PushCameraToEntity();

            auto* input = host.Shell() != nullptr ? host.Shell()->Input() : nullptr;
            if (input == nullptr || m_scene == nullptr)
            {
                return;
            }
            if (input->Keyboard()->IsKeyPressed(shell::KeyCode::Escape))
            {
                host.RequestExit(0);
            }

            // LMB: a positional one-shot ahead of the camera with random pitch and offset,
            // through the engine-global PlayOneShot3D on the Effects bus.
            if (input->Mouse()->IsButtonPressed(shell::MouseButton::Left) && Audio() != nullptr)
            {
                const core::Float3 forward = m_fly.Forward();
                const f32 distance = 4.0f + 8.0f * Random01();
                const core::Float3 position{
                    m_fly.position.x + forward.x * distance + (Random01() - 0.5f) * 4.0f,
                    m_fly.position.y + forward.y * distance,
                    m_fly.position.z + forward.z * distance + (Random01() - 0.5f) * 4.0f};
                audio::AudioPlayParams params;
                params.minDistance = 1.5f;
                params.maxDistance = 50.0f;
                m_lastOneShot = Audio()->PlayCueOneShot3D(m_shotCue, position, params);
                m_lastOneShotPosition = position;
                m_haveOneShot = true;
            }

            // Space: a flat UI-bus click (non-spatial one-shot).
            if (input->Keyboard()->IsKeyPressed(shell::KeyCode::Space) && Audio() != nullptr)
            {
                (void)Audio()->PlayOneShot(m_click, audio::AudioBus::UI);
            }

            // T: scene-simulation pause; the per-scene voice group fades and halts.
            if (input->Keyboard()->IsKeyPressed(shell::KeyCode::T))
            {
                m_scene->SetSimulationEnabled(!m_scene->SimulationEnabled());
            }

            DrawEmitterGizmos(host);
            if (auto* gui = host.Subsystems().GetSubsystem<imgui::ImguiSubsystem>())
            {
                gui->NewFrame(input, deltaTime);
                DrawHud();
            }
        }

        void OnRenderWindow(runtime::IApplicationHost& host, graphics::FrameContext& frame) override
        {
            if (m_scene != nullptr && frame.height > 0)
            {
                if (auto* cameras = m_scene->GetSystem<engine::render::CameraComponentManager>())
                {
                    if (engine::render::CameraComponent* camera = cameras->Get(m_camera))
                    {
                        camera->aspect =
                            static_cast<f32>(frame.width) / static_cast<f32>(frame.height);
                    }
                }
            }
            RenderFrame(host, frame); // the scenes and window overlays; FinishFrame takes the screenshot
            if (auto* gui = host.Subsystems().GetSubsystem<imgui::ImguiSubsystem>())
            {
                gui->Render(frame);
            }
            FinishFrame(host, frame); // copies the screenshot, overlay included
        }

    private:
        [[nodiscard]] f32 Random01()
        {
            m_randomState = m_randomState * 1664525u + 1013904223u;
            return static_cast<f32>((m_randomState >> 8) & 0xFFFFFF) / 16777215.0f;
        }

        void DrawEmitterGizmos(runtime::IApplicationHost& host)
        {
            auto* renderer = host.Subsystems().GetSubsystem<engine::render::RenderSubsystem>();
            if (renderer == nullptr || m_scene == nullptr)
            {
                return;
            }
            auto& draw = renderer->DebugScene(*m_scene);
            for (scene::EntityHandle e : m_emitters)
            {
                const core::Float3 position = m_scene->GetWorldPosition(e);
                draw.DrawWireSphere(position, 0.5f, core::Color{0.3f, 0.9f, 1.0f, 1.0f});
            }
            if (m_haveOneShot && Audio() != nullptr && Audio()->IsPlaying(m_lastOneShot))
            {
                draw.DrawWireSphere(m_lastOneShotPosition, 0.35f,
                                    core::Color{1.0f, 0.8f, 0.2f, 1.0f});
            }
        }

        void DrawHud()
        {
            ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
            ImGui::Begin("Audio");
            audio::AudioEngine* engine = Audio() != nullptr ? Audio()->Engine() : nullptr;
            if (engine != nullptr)
            {
                ImGui::Text("voices: %zu%s", engine->ActiveVoiceCount(),
                            engine->IsHeadless() ? "  (NULL mode - no device)" : "");
                struct BusRow
                {
                    audio::AudioBus bus;
                    const char* label;
                };
                const BusRow rows[4] = {{audio::AudioBus::Master, "Master"},
                                        {audio::AudioBus::Effects, "Effects"},
                                        {audio::AudioBus::Music, "Music"},
                                        {audio::AudioBus::UI, "UI"}};
                for (const BusRow& row : rows)
                {
                    f32 volume = engine->BusVolume(row.bus);
                    if (ImGui::SliderFloat(row.label, &volume, 0.0f, 1.5f))
                    {
                        engine->SetBusVolume(row.bus, volume);
                    }
                }
            }
            if (m_scene != nullptr)
            {
                ImGui::Text("simulation: %s (T toggles)",
                            m_scene->SimulationEnabled() ? "running" : "PAUSED");
            }
            ImGui::Text("LMB 3D one-shot | Space UI click | Esc quit");
            ImGui::End();
        }

        void PushCameraToEntity()
        {
            if (m_scene == nullptr)
            {
                return;
            }
            core::Transform t = m_scene->GetLocalTransform(m_camera);
            t.position = m_fly.position;
            t.rotation = m_fly.Rotation();
            m_scene->SetLocalTransform(m_camera, t);
        }

        scene::Scene* m_scene = nullptr;
        scene::EntityHandle m_camera;
        core::Array<scene::EntityHandle> m_emitters;
        core::RefPtr<audio::AudioClip> m_ambient;
        core::RefPtr<audio::SoundCue> m_shotCue;
        core::RefPtr<audio::AudioClip> m_beepHigh;
        core::RefPtr<audio::AudioClip> m_beepLow;
        core::RefPtr<audio::AudioClip> m_click;
        audio::VoiceHandle m_lastOneShot;
        core::Float3 m_lastOneShotPosition{0, 0, 0};
        bool m_haveOneShot = false;
        core::u32 m_randomState = 0x12345678u;
        samples::FlyCamera m_fly;
    };
}

APP_MAIN(PlaygroundApp)
