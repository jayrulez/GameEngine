// AudioPlayground - the audio P1 consumer proof: an ambient chord loop on the Music bus
// plus four looping 3D emitters around the origin (distinct pitches of the same clip -
// REAL pitch resampling), all authored as AudioSource COMPONENTS with autoplay on a
// scene, the LISTENER driven by an AudioListenerComponent on the fly-camera entity, and
// click-to-fire positional one-shots (random pitch + position) through the engine-global
// PlayOneShot3D. Fly around (WASD/RMB-look) to hear attenuation + pan + doppler as you
// strafe past emitters; the ImGui panel has Master/Effects/Music bus sliders and live
// voice counts. Emitters draw as debug wire spheres. Runs fine without an audio device
// (Null mode - a warning logs and everything else still works, just silently).

#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Log/Log.h"
#include "Draconic.Runtime.Client/AppMain.h"
#include "imgui.h"
#include <cmath>

import draconic.foundation;
import draconic.runtime;
import draconic.runtime.client;
import draconic.engine.defaultapp;
import draconic.shell;
import draconic.runtime.desktop;
import draconic.shell.desktop;
import draconic.graphics;
import draconic.graphics.gpu;
import draconic.scene;
import draconic.engine.scene;
import draconic.engine.render;
import draconic.imgui;
import draconic.audio;
import draconic.engine.audio;

#include "../Common/FlyCamera.h" // after the imports: uses draconic::foundation/runtime types

namespace foundation = draconic::foundation;
namespace runtime = draconic::runtime;
namespace graphics = draconic::graphics;
namespace shell = draconic::shell;
namespace scene = draconic::scene;
namespace render = draconic::render;
namespace audio = draconic::audio;
namespace imgui = draconic::imgui;

using foundation::f32;

namespace
{
    [[nodiscard]] foundation::RefPtr<audio::AudioClip> LoadClipFromFile(foundation::StringView path,
                                                                  bool loop = false)
    {
        foundation::Result<foundation::Array<foundation::byte>> bytes = foundation::ReadFile(path);
        if (!bytes.HasValue())
        {
            DRACONIC_LOG_ERROR(u8"AudioPlayground", u8"missing sample data: {}", path);
            return {};
        }
        audio::AudioClipMetadata metadata;
        if (!audio::ProbeAudioClipMetadata(
                foundation::Span<const foundation::byte>(bytes.Value().Data(), bytes.Value().Size()), metadata))
        {
            return {};
        }
        foundation::RefPtr<audio::AudioClip> clip =
            foundation::MakeRef<audio::AudioClip>(foundation::DefaultAllocator());
        clip->channels = metadata.channels;
        clip->sampleRate = metadata.sampleRate;
        clip->frameCount = metadata.frameCount;
        clip->durationSeconds = metadata.durationSeconds;
        clip->loop = loop;
        clip->encodedData = foundation::Move(bytes.Value());
        return clip;
    }

    class PlaygroundApp final : public runtime::DefaultApplication
    {
    public:
        void Configure(runtime::IApplicationHost& host) override
        {
            runtime::DefaultApplication::Configure(host); // registers AudioSubsystem
            if (auto* gfx = host.Graphics(); gfx != nullptr && gfx->Raw() != nullptr)
            {
                host.Ctx().AddSubsystem<imgui::ImguiSubsystem>(*gfx->Raw(), gfx->FramesInFlight());
            }
        }

        void OnLaunch(runtime::IApplicationHost& host) override
        {
            auto* scenes = host.Ctx().GetSubsystem<scene::SceneSubsystem>();
            if (scenes == nullptr || Audio() == nullptr)
            {
                return;
            }
            m_scene = PrimaryScenes().CreateScene(u8"audio-playground");

            const foundation::String dataDir(u8"" DRACONIC_AUDIO_SAMPLE_DATA_DIR);
            m_ambient =
                LoadClipFromFile(foundation::PathJoin(dataDir.AsView(), u8"ambient_loop.wav"), true);
            m_beepHigh = LoadClipFromFile(foundation::PathJoin(dataDir.AsView(), u8"beep_high.wav"));
            m_beepLow = LoadClipFromFile(foundation::PathJoin(dataDir.AsView(), u8"beep_low.wav"), true);
            m_click = LoadClipFromFile(foundation::PathJoin(dataDir.AsView(), u8"click.wav"));

            // Camera entity = the LISTENER (AudioListenerComponent drives the engine).
            m_camera = m_scene->CreateEntity(u8"camera");
            if (auto* cameras = m_scene->GetSystem<render::CameraComponentManager>())
            {
                cameras->Add(m_camera);
            }
            m_scene->GetSystem<audio::AudioListenerComponentManager>()->Add(m_camera);
            m_fly.position = foundation::Float3{0.0f, 2.0f, 14.0f};
            m_fly.moveSpeed = 8.0f;
            m_fly.fastSpeed = 25.0f;

            // Ambient pad: a looping streamless clip on the MUSIC bus (autoplay, 2D).
            {
                scene::EntityHandle e = m_scene->CreateEntity(u8"ambient");
                audio::AudioSourceComponent& c =
                    m_scene->GetSystem<audio::AudioSourceComponentManager>()->Add(e);
                c.clip = m_ambient;
                c.bus = audio::AudioBus::Music;
                c.spatial = false;
                c.loop = true;
                c.autoPlay = true;
                // Quiet bed: positional sources must read OVER it (masking lesson -
                // the pad shares the beeps' spectrum, so level is the separator).
                c.volume = 0.35f;
            }

            // Four 3D emitters around the origin: the SAME low-beep clip at four pitches
            // (real resampling), looping, positioned - fly past to hear pan/attenuation
            // and strafe quickly for doppler.
            // Pitches keep every emitter >= ~250 Hz (laptop-speaker floor) and distinct.
            const f32 pitches[4] = {0.75f, 1.0f, 1.5f, 2.0f};
            for (int i = 0; i < 4; ++i)
            {
                const f32 angle = 3.14159265f * 0.5f * static_cast<f32>(i);
                scene::EntityHandle e = m_scene->CreateEntity(u8"emitter");
                m_scene->SetLocalPosition(
                    e, foundation::Float3{10.0f * std::cos(angle), 1.5f, 10.0f * std::sin(angle)});
                audio::AudioSourceComponent& c =
                    m_scene->GetSystem<audio::AudioSourceComponentManager>()->Add(e);
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

            // Reverb zone (P3): stand near the origin to hear the 'cave' - the tail
            // fades in across the zone's edge band and dries out as you fly away.
            {
                scene::EntityHandle zone = m_scene->CreateEntity(u8"cave-zone");
                auto& reverb =
                    m_scene->GetSystem<audio::AudioReverbZoneComponentManager>()->Add(zone);
                reverb.radius = 12.0f;
                reverb.edgeFade = 0.4f;
                reverb.roomSize = 0.8f;
                reverb.damping = 0.2f;
                reverb.wetLevel = 0.6f;
            }

            // LMB one-shots fire through a CUE (P3): three weighted variants with
            // pitch jitter - no two consecutive shots pick the same clip.
            m_shotCue = foundation::MakeRef<audio::SoundCue>(foundation::DefaultAllocator());
            m_shotCue->variants.PushBack(audio::SoundCueVariant{m_beepHigh, 3.0f});
            m_shotCue->variants.PushBack(audio::SoundCueVariant{m_click, 2.0f});
            m_shotCue->variants.PushBack(audio::SoundCueVariant{m_beepLow, 1.0f});
            m_shotCue->pitchMin = 0.85f;
            m_shotCue->pitchMax = 1.25f;

            m_scene->Start();
            m_scene->SetSimulationEnabled(true);
            if (Audio()->Engine() != nullptr && Audio()->Engine()->IsHeadless())
            {
                foundation::ConsoleWrite(
                    u8"AudioPlayground: NO audio device - running silent (Null mode).\n");
            }
            foundation::ConsoleWrite(
                u8"AudioPlayground: WASD/RMB-look fly. LMB = positional one-shot at the\n"
                u8"crosshair distance (random pitch). Space = UI click one-shot. T = pause\n"
                u8"scene simulation (fades the scene's voices). Esc quits.\n");
        }

        void OnUpdate(runtime::IApplicationHost& host, f32 deltaTime) override
        {
            runtime::DefaultApplication::OnUpdate(host, deltaTime);
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

            // LMB: a positional one-shot ahead of the camera with random pitch/offset -
            // engine-global PlayOneShot3D on the Effects bus.
            if (input->Mouse()->IsButtonPressed(shell::MouseButton::Left) && Audio() != nullptr)
            {
                const foundation::Float3 forward = m_fly.Forward();
                const f32 distance = 4.0f + 8.0f * Random01();
                const foundation::Float3 position{
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

            // P: scene-simulation pause - the per-scene voice group fades and halts.
            if (input->Keyboard()->IsKeyPressed(shell::KeyCode::T))
            {
                m_scene->SetSimulationEnabled(!m_scene->SimulationEnabled());
            }

            DrawEmitterGizmos(host);
            if (auto* gui = host.Ctx().GetSubsystem<imgui::ImguiSubsystem>())
            {
                gui->NewFrame(input, deltaTime);
                DrawHud();
            }
        }

        void OnRenderWindow(runtime::IApplicationHost& host, graphics::FrameContext& frame) override
        {
            if (m_scene != nullptr && frame.height > 0)
            {
                if (auto* cameras = m_scene->GetSystem<render::CameraComponentManager>())
                {
                    if (render::CameraComponent* camera = cameras->Get(m_camera))
                    {
                        camera->aspect =
                            static_cast<f32>(frame.width) / static_cast<f32>(frame.height);
                    }
                }
            }
            runtime::DefaultApplication::OnRenderWindow(host, frame);
            if (auto* gui = host.Ctx().GetSubsystem<imgui::ImguiSubsystem>())
            {
                gui->Render(frame);
            }
        }

    private:
        [[nodiscard]] f32 Random01()
        {
            m_randomState = m_randomState * 1664525u + 1013904223u;
            return static_cast<f32>((m_randomState >> 8) & 0xFFFFFF) / 16777215.0f;
        }

        void DrawEmitterGizmos(runtime::IApplicationHost& host)
        {
            auto* renderer = host.Ctx().GetSubsystem<render::RenderSubsystem>();
            if (renderer == nullptr || m_scene == nullptr)
            {
                return;
            }
            auto& draw = renderer->DebugScene(*m_scene);
            for (scene::EntityHandle e : m_emitters)
            {
                const foundation::Float3 position = m_scene->GetWorldPosition(e);
                draw.DrawWireSphere(position, 0.5f, foundation::Color{0.3f, 0.9f, 1.0f, 1.0f});
            }
            if (m_haveOneShot && Audio() != nullptr && Audio()->IsPlaying(m_lastOneShot))
            {
                draw.DrawWireSphere(m_lastOneShotPosition, 0.35f,
                                    foundation::Color{1.0f, 0.8f, 0.2f, 1.0f});
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
            foundation::Transform t = m_scene->GetLocalTransform(m_camera);
            t.position = m_fly.position;
            t.rotation = m_fly.Rotation();
            m_scene->SetLocalTransform(m_camera, t);
        }

        scene::Scene* m_scene = nullptr;
        scene::EntityHandle m_camera;
        foundation::Array<scene::EntityHandle> m_emitters;
        foundation::RefPtr<audio::AudioClip> m_ambient;
        foundation::RefPtr<audio::SoundCue> m_shotCue;
        foundation::RefPtr<audio::AudioClip> m_beepHigh;
        foundation::RefPtr<audio::AudioClip> m_beepLow;
        foundation::RefPtr<audio::AudioClip> m_click;
        audio::VoiceHandle m_lastOneShot;
        foundation::Float3 m_lastOneShotPosition{0, 0, 0};
        bool m_haveOneShot = false;
        foundation::u32 m_randomState = 0x12345678u;
        draconic::samples::FlyCamera m_fly;
    };
}

DRACONIC_APP_MAIN(PlaygroundApp)
