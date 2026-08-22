// Engine::Audio - the `engine.audio` module.
//
// Scene integration (docs/design/audio.md §6): an AudioSceneSystem per scene owns a
// per-scene voice group (open question 1: YES - pause/stop-all per scene falls out of
// the engine's group graph naturally, which play-in-editor needs). The PostTransform
// tick resolves clip refs, autoplays on simulation start, syncs position + velocity
// (previous-frame delta - the doppler feed) to live voices, reaps finished one-shots,
// and computes the scene's listener pose from the first active AudioListenerComponent.
// The subsystem owns the ONE AudioEngine, pushes the winning listener (component first,
// active camera fallback - camera contact lives in SubsystemImpl.cpp, the render import
// stays out of this interface), and exposes the engine-global one-shot API.

module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"
#include "Core/Reflection/Reflect.h"
#include "Profiler/Profiler.h" // PROFILE_SCOPE (compiles to nothing when disabled)

export module engine.audio;

export import :components;

import foundation.core;
import foundation.profiler;
import foundation.runtime;
import foundation.scene;
import engine.scene;
import foundation.audio;
import foundation.script;   // ExposeToScript + the Audio facade's service seam
import foundation.script.facades; // script::Entity/Scene + CurrentRunResources (the SceneAudio handle)
import foundation.settings; // AudioUserSettings section (persisted volumes)
import foundation.resource; // ResourceManager (script content-path playback)
import foundation.content;  // Instance lookup by content path
// NOTE: no render imports HERE - the camera-fallback listener lives in SubsystemImpl.cpp
// (a module implementation unit), keeping heavyweight imports out of the interface for
// GCC's -fno-module-lazy consumers.

using namespace foundation::core;
using namespace foundation::audio;

export namespace engine::audio
{
    // Foundation aliases (sibling engine::* namespaces would otherwise shadow these).
    namespace scene = foundation::scene;
    namespace script = foundation::script;

    namespace scene = foundation::scene;

    class AudioSubsystem; // forward (the script binding carries it)

    /// The service key ExposeToScript binds and the scripting Audio facade resolves.
    /// Payload = AudioScriptBinding (the physics-binding precedent): ONE service
    /// carries the engine (bus/music control) plus the subsystem + resource manager
    /// (content-path playback), instead of audio growing a second generic service.
    inline constexpr StringView kAudioScriptService = u8"audio.runtime";

    struct AudioScriptBinding
    {
        AudioEngine* engine = nullptr;
        AudioSubsystem* subsystem = nullptr;                      // cue state + helpers
        foundation::resource::ResourceManager* resources = nullptr; // path -> product
    };

    /// One listener's world pose this frame (multi-listener collection).
    struct ListenerPose
    {
        Float3 position{0.0f, 0.0f, 0.0f};
        Float3 forward{0.0f, 0.0f, -1.0f};
        Float3 up{0.0f, 1.0f, 0.0f};
        Float3 velocity{0.0f, 0.0f, 0.0f};
    };

    // ---- persisted user volumes (P2): a foundation.settings SECTION ----
    // Bus volumes/mutes as the USER's mixer state (options-menu sliders). Applied AFTER
    // any project bus layout - the layout is the artistic baseline, the user's setting
    // is absolute (the way options menus behave). Hosts load it at startup and capture
    // + save it at shutdown; scripts change volumes through the Audio facade.
    class AudioUserSettings final : public ISerializable
    {
        RTTI_OBJECT(AudioUserSettings, ISerializable)
    public:
        f32 volumes[static_cast<usize>(AudioBus::Count)] = {1.0f, 1.0f, 1.0f, 1.0f};
        bool muted[static_cast<usize>(AudioBus::Count)] = {};

        void Serialize(ISerializer& ar) override
        {
            u32 busCount = static_cast<u32>(AudioBus::Count);
            foundation::core::Serialize(ar, "busCount", busCount);
            const u32 buses = Min(busCount, static_cast<u32>(AudioBus::Count));
            for (u32 bus = 0; bus < buses; ++bus)
            {
                foundation::core::Serialize(ar, "volume", volumes[bus]);
                foundation::core::Serialize(ar, "muted", muted[bus]);
            }
        }
    };

    inline void ApplyAudioUserSettings(AudioEngine& engine, const AudioUserSettings& settings)
    {
        for (usize bus = 0; bus < static_cast<usize>(AudioBus::Count); ++bus)
        {
            engine.SetBusVolume(static_cast<AudioBus>(bus), settings.volumes[bus]);
            engine.SetBusMuted(static_cast<AudioBus>(bus), settings.muted[bus]);
        }
    }

    inline void CaptureAudioUserSettings(const AudioEngine& engine, AudioUserSettings& settings)
    {
        for (usize bus = 0; bus < static_cast<usize>(AudioBus::Count); ++bus)
        {
            settings.volumes[bus] = engine.BusVolume(static_cast<AudioBus>(bus));
            settings.muted[bus] = engine.BusMuted(static_cast<AudioBus>(bus));
        }
    }

    inline void RegisterAudioSettingsTypes()
    {
        GlobalTypeRegistry().Register(AudioUserSettings::StaticType());
        RegisterSerializable<AudioUserSettings>();
    }

    class AudioSceneSystem final : public scene::SceneSystem
    {
    public:
        void OnSceneCreate(scene::Scene& scene) override { m_scene = &scene; }

        /// The subsystem wires its engine in right after AddSystem (tests may inject a
        /// headless engine directly).
        void SetEngine(AudioEngine* engine) noexcept { m_engine = engine; }
        [[nodiscard]] AudioEngine* Engine() const noexcept { return m_engine; }
        [[nodiscard]] u64 SceneGroup() const noexcept { return m_sceneGroup; }
        [[nodiscard]] scene::Scene* ScenePtr() const noexcept { return m_scene; }
        [[nodiscard]] bool Started() const noexcept { return m_started; }

        // ---- play lifecycle ----

        void OnSceneStarted() override
        {
            m_started = true;
            m_wasSimulating = true;
            if (m_engine == nullptr)
            {
                return;
            }
            m_sceneGroup = m_engine->CreateSceneGroup();

            // Autoplay: world matrices are current before this fires (Scene::Start
            // guarantee) - positional voices start where they were authored.
            if (auto* sources = m_scene->GetSystem<AudioSourceComponentManager>())
            {
                sources->ForEach(
                    [&](AudioSourceComponent& c, scene::EntityHandle e)
                    {
                        if (c.autoPlay)
                        {
                            if (m_scene->IsEffectivelyActive(e))
                            {
                                PlayComponent(c, e);
                            }
                            else
                            {
                                // Starts-inactive: arm the latch so the ACTIVATION edge
                                // plays it (entity-active-state.md P3).
                                c.activeSuspended = true;
                            }
                        }
                    });
            }
        }

        void OnSceneStopped() override
        {
            m_started = false;
            if (auto* sources = m_scene->GetSystem<AudioSourceComponentManager>())
            {
                sources->ForEach(
                    [](AudioSourceComponent& c, scene::EntityHandle)
                    {
                        c.voice = VoiceHandle{};
                        c.hasPreviousPosition = false;
                        c.activeSuspended = false;
                    });
            }
            if (m_engine != nullptr && m_sceneGroup != 0)
            {
                m_engine->DestroySceneGroup(m_sceneGroup); // stops the scene's voices
            }
            m_sceneGroup = 0;
            m_listenerValid = false;
        }

        // ---- component control surface (the runtime controls Sedulous never had) ----

        /// Starts (or restarts) the entity's source. Returns the voice handle.
        VoiceHandle Play(scene::EntityHandle entity)
        {
            auto* sources =
                m_scene != nullptr ? m_scene->GetSystem<AudioSourceComponentManager>() : nullptr;
            AudioSourceComponent* component = sources != nullptr ? sources->Get(entity) : nullptr;
            if (component == nullptr)
            {
                return {};
            }
            return PlayComponent(*component, entity);
        }

        void Stop(scene::EntityHandle entity)
        {
            if (AudioSourceComponent* component = Component(entity))
            {
                if (m_engine != nullptr)
                {
                    m_engine->Stop(component->voice);
                }
                component->voice = VoiceHandle{};
                component->hasPreviousPosition = false;
            }
        }

        void SetPaused(scene::EntityHandle entity, bool paused)
        {
            if (AudioSourceComponent* component = Component(entity))
            {
                if (m_engine != nullptr)
                {
                    m_engine->SetPaused(component->voice, paused);
                }
            }
        }

        [[nodiscard]] bool IsPlaying(scene::EntityHandle entity)
        {
            AudioSourceComponent* component = Component(entity);
            return component != nullptr && m_engine != nullptr &&
                   m_engine->IsPlaying(component->voice);
        }

        // ---- per-frame sync (PostTransform: final transforms are ready) ----

        void OnUpdate(scene::ScenePhase phase, f32 deltaTime) override
        {
            if (phase != scene::ScenePhase::PostTransform)
            {
                return;
            }
            if (m_engine == nullptr || m_scene == nullptr || !m_started)
            {
                return;
            }
            PROFILE_SCOPE("Audio.Sources");

            // Scene simulation pause/resume maps onto the per-scene group (fade both ways).
            const bool simulating = m_scene->SimulationEnabled();
            if (simulating != m_wasSimulating && m_sceneGroup != 0)
            {
                m_engine->SetSceneGroupPaused(m_sceneGroup, !simulating);
            }
            m_wasSimulating = simulating;
            if (!simulating)
            {
                return;
            }

            if (auto* sources = m_scene->GetSystem<AudioSourceComponentManager>())
            {
                sources->ForEach(
                    [&](AudioSourceComponent& c, scene::EntityHandle e)
                    {
                        // Entity-active edges: deactivation STOPS the voice (silence, not a
                        // skipped update); reactivation restarts autoplay sources. One-shots
                        // stopped this way do not resume mid-buffer (v1).
                        const bool eff = m_scene->IsEffectivelyActive(e);
                        if (!eff)
                        {
                            if (c.voice.IsValid())
                            {
                                m_engine->Stop(c.voice);
                                c.voice = VoiceHandle{};
                                c.hasPreviousPosition = false;
                                c.activeSuspended = true;
                            }
                            return;
                        }
                        if (c.activeSuspended)
                        {
                            c.activeSuspended = false;
                            if (c.autoPlay)
                            {
                                (void)PlayComponent(c, e);
                            }
                        }
                        if (!c.voice.IsValid())
                        {
                            return;
                        }
                        if (!m_engine->IsValidHandle(c.voice))
                        {
                            c.voice = VoiceHandle{}; // finished one-shot: reap the handle
                            c.hasPreviousPosition = false;
                            return;
                        }
                        if (!c.spatial)
                        {
                            return;
                        }
                        const Float3 position = EntityPosition(e);
                        const Float3 velocity =
                            (c.hasPreviousPosition && deltaTime > 0.0f)
                                ? Float3{(position.x - c.previousPosition.x) / deltaTime,
                                         (position.y - c.previousPosition.y) / deltaTime,
                                         (position.z - c.previousPosition.z) / deltaTime}
                                : Float3{0.0f, 0.0f, 0.0f};
                        m_engine->SetVoicePosition(c.voice, position, velocity);
                        c.previousPosition = position;
                        c.hasPreviousPosition = true;
                    });
            }

            UpdateListenerPose(deltaTime);
            UpdateReverbZones();
        }

        // The scene's listener pose this frame (component-driven). The SUBSYSTEM pushes
        // the winning scene's pose to the engine (camera fallback handled there).
        [[nodiscard]] bool ListenerValid() const noexcept { return m_listenerValid; }
        [[nodiscard]] Float3 ListenerPosition() const noexcept { return m_listenerPosition; }
        [[nodiscard]] Float3 ListenerForward() const noexcept { return m_listenerForward; }
        [[nodiscard]] Float3 ListenerUp() const noexcept { return m_listenerUp; }
        [[nodiscard]] Float3 ListenerVelocity() const noexcept { return m_listenerVelocity; }
        [[nodiscard]] Span<const ListenerPose> ListenerPoses() const noexcept
        {
            return Span<const ListenerPose>{m_listenerPoses.Data(), m_listenerPoses.Size()};
        }

    private:
        [[nodiscard]] AudioSourceComponent* Component(scene::EntityHandle entity)
        {
            auto* sources =
                m_scene != nullptr ? m_scene->GetSystem<AudioSourceComponentManager>() : nullptr;
            return sources != nullptr ? sources->Get(entity) : nullptr;
        }

        [[nodiscard]] Float3 EntityPosition(scene::EntityHandle entity) const
        {
            const Float4x4 world = m_scene->GetWorldMatrix(entity);
            return TransformPoint(Float3{0.0f, 0.0f, 0.0f}, world);
        }

        VoiceHandle PlayComponent(AudioSourceComponent& c, scene::EntityHandle e)
        {
            if (m_engine == nullptr)
            {
                return {};
            }
            if (m_scene != nullptr && !m_scene->IsEffectivelyActive(e))
            {
                return {}; // inactive entities make no sound (entity-active-state.md P3)
            }
            // The sourceType discriminant decides: Cue resolves one weighted variant + this
            // trigger's jitter; Clip plays the single clip. (An empty Cue stays a silent no-op -
            // pick.variantIndex < 0 leaves clip null; see the empty-cue ruling.)
            AudioClip* clip = nullptr;
            f32 cuePitch = 1.0f;
            f32 cueVolume = 1.0f;
            if (c.sourceType == AudioSourceType::Cue)
            {
                if (const SoundCue* cue = c.cue.Get())
                {
                    const SoundCuePick pick =
                        ResolveSoundCue(*cue, m_cueRandom, c.lastCueVariant, c.cueSequentialCursor);
                    if (pick.variantIndex >= 0)
                    {
                        c.lastCueVariant = pick.variantIndex;
                        clip = cue->variants[static_cast<usize>(pick.variantIndex)].clip.Get();
                        cuePitch = pick.pitch;
                        cueVolume = pick.volume;
                    }
                }
            }
            else
            {
                clip = c.clip.Get();
            }
            if (clip == nullptr)
            {
                LOG_WARNING(u8"Audio", u8"'{}': audio source has no clip or cue",
                                     m_scene->GetEntityName(e));
                return {};
            }
            if (m_engine->IsValidHandle(c.voice))
            {
                m_engine->Stop(c.voice);
            }

            AudioPlayParams params;
            params.bus = c.bus;
            params.busName = String(c.busName.AsView());
            params.volume = c.volume * cueVolume;
            params.pitch = c.pitch * cuePitch;
            params.loop = c.loop;
            params.priority = c.priority;
            params.sceneGroup = m_sceneGroup;
            // Persistent authored source: NEVER dedupe-merged (several sources sharing a
            // clip - four torches - are distinct voices; autoplay starts them in the
            // same instant, which the one-shot window would otherwise collapse).
            params.allowDedupe = false;
            params.reverbSend = c.reverbSend;
            params.spatial = c.spatial;
            if (c.spatial)
            {
                params.distanceLowpassHz = c.distanceLowpassHz;
                params.position = EntityPosition(e);
                params.minDistance = c.minDistance;
                params.maxDistance = c.maxDistance;
                params.attenuationModel = c.attenuationModel;
                params.rolloff = c.rolloff;
                params.dopplerFactor = c.dopplerFactor;
                params.coneInnerAngleDegrees = c.coneInnerAngleDegrees;
                params.coneOuterAngleDegrees = c.coneOuterAngleDegrees;
                params.coneOuterGain = c.coneOuterGain;
            }
            c.voice = m_engine->Play(RefPtr<AudioClip>(clip), params);
            c.previousPosition = params.position;
            c.hasPreviousPosition = c.spatial;
            return c.voice;
        }

        // Environmental reverb (P3 zones): the WETTEST zone containing the listener
        // drives the scene's Effects reverb; wet fades across each zone's edge band.
        // No listener / no zone = wet 0 (the node bypasses; the tail decays naturally).
        void UpdateReverbZones()
        {
            if (m_engine == nullptr || m_sceneGroup == 0)
            {
                return;
            }
            AudioReverbParams best;
            best.wet = 0.0f;
            auto* zones = m_scene->GetSystem<AudioReverbZoneComponentManager>();
            if (m_listenerValid && zones != nullptr)
            {
                zones->ForEach(
                    [&](AudioReverbZoneComponent& zone, scene::EntityHandle e)
                    {
                        if (!zone.enabled || zone.radius <= 0.0f || zone.wetLevel <= 0.0f)
                        {
                            return;
                        }
                        const Float3 center = EntityPosition(e);
                        const Float3 delta{m_listenerPosition.x - center.x,
                                           m_listenerPosition.y - center.y,
                                           m_listenerPosition.z - center.z};
                        const f32 distance =
                            Sqrt(delta.x * delta.x + delta.y * delta.y + delta.z * delta.z);
                        if (distance >= zone.radius)
                        {
                            return;
                        }
                        const f32 fadeWidth = Max(zone.edgeFade * zone.radius, 0.001f);
                        const f32 blend = Clamp((zone.radius - distance) / fadeWidth, 0.0f, 1.0f);
                        const f32 wet = zone.wetLevel * blend;
                        if (wet > best.wet)
                        {
                            best.wet = wet;
                            best.roomSize = zone.roomSize;
                            best.damping = zone.damping;
                        }
                    });
            }
            m_engine->SetSceneReverb(m_sceneGroup, best);
        }

        void UpdateListenerPose(f32 deltaTime)
        {
            // ALL active listeners collect (multi-listener, P3: split-screen ears);
            // the FIRST one stays the scene's primary (zones + legacy accessors).
            m_listenerValid = false;
            m_listenerPoses.Clear();
            auto* listeners = m_scene->GetSystem<AudioListenerComponentManager>();
            if (listeners == nullptr)
            {
                return;
            }
            listeners->ForEach(
                [&](AudioListenerComponent& c, scene::EntityHandle e)
                {
                    if (!c.isActive)
                    {
                        return;
                    }
                    const Float4x4 world = m_scene->GetWorldMatrix(e);
                    ListenerPose pose;
                    pose.position = TransformPoint(Float3{0.0f, 0.0f, 0.0f}, world);
                    // Forward is -Z (row 2 negated), up is +Y (row 1) - row-vector convention.
                    pose.forward =
                        Normalized(Float3{-world.m[2][0], -world.m[2][1], -world.m[2][2]});
                    pose.up = Normalized(Float3{world.m[1][0], world.m[1][1], world.m[1][2]});
                    pose.velocity =
                        (c.hasPreviousPosition && deltaTime > 0.0f)
                            ? Float3{(pose.position.x - c.previousPosition.x) / deltaTime,
                                     (pose.position.y - c.previousPosition.y) / deltaTime,
                                     (pose.position.z - c.previousPosition.z) / deltaTime}
                            : Float3{0.0f, 0.0f, 0.0f};
                    c.previousPosition = pose.position;
                    c.hasPreviousPosition = true;
                    if (!m_listenerValid)
                    {
                        m_listenerPosition = pose.position;
                        m_listenerForward = pose.forward;
                        m_listenerUp = pose.up;
                        m_listenerVelocity = pose.velocity;
                        m_listenerValid = true;
                    }
                    m_listenerPoses.PushBack(pose);
                });
        }

        scene::Scene* m_scene = nullptr;
        AudioEngine* m_engine = nullptr;
        u64 m_sceneGroup = 0;
        bool m_started = false;
        bool m_wasSimulating = true;
        Random m_cueRandom; // cue variant selection (per scene system)
        bool m_listenerValid = false;
        Float3 m_listenerPosition{0.0f, 0.0f, 0.0f};
        Float3 m_listenerForward{0.0f, 0.0f, -1.0f};
        Float3 m_listenerUp{0.0f, 1.0f, 0.0f};
        Float3 m_listenerVelocity{0.0f, 0.0f, 0.0f};
        Array<ListenerPose> m_listenerPoses;
    };

    // A scene-bound AUDIO handle (SceneAudio.of(scene)): runtime WORLD ops on audio sources that need
    // the scene's AudioEngine / resource manager, which the component data cannot reach - play/stop/
    // pause a source, and swap its clip by resource id. Keyed by entity, mirroring ScenePhysics /
    // SceneRender (component = auto-reflected DATA: volume/pitch/loop; scene-handle = world ops). The
    // pure playback intent (autoPlay, loop, volume) stays on AudioSourceComponent; starting a voice
    // NOW is an engine op, so it lives here.
    struct SceneAudio
    {
        scene::Scene* scene = nullptr;

        [[nodiscard]] AudioSceneSystem* System() const
        {
            return scene != nullptr ? scene->GetSystem<AudioSceneSystem>() : nullptr;
        }

        // Start (or restart) the entity's source voice now. No-op if it has no AudioSourceComponent.
        void play(foundation::script::Entity entity) const
        {
            if (AudioSceneSystem* sys = System())
            {
                (void)sys->Play(entity.Handle());
            }
        }
        // Stop the entity's source voice (releases the voice handle).
        void stop(foundation::script::Entity entity) const
        {
            if (AudioSceneSystem* sys = System())
            {
                sys->Stop(entity.Handle());
            }
        }
        // Pause/resume the entity's source voice.
        void pause(foundation::script::Entity entity, bool paused) const
        {
            if (AudioSceneSystem* sys = System())
            {
                sys->SetPaused(entity.Handle(), paused);
            }
        }
        // True while the entity's source voice is audibly playing.
        [[nodiscard]] bool isPlaying(foundation::script::Entity entity) const
        {
            AudioSceneSystem* sys = System();
            return sys != nullptr && sys->IsPlaying(entity.Handle());
        }
        // Swap the entity's AudioSource clip to resource `id`, binding it through the run's resource
        // manager (the next play uses it). No-op if the entity has no AudioSourceComponent.
        void setClip(foundation::script::Entity entity, Guid id) const
        {
            if (scene == nullptr)
            {
                return;
            }
            AudioSourceComponentManager* sources = scene->GetSystem<AudioSourceComponentManager>();
            AudioSourceComponent* c = (sources != nullptr) ? sources->Get(entity.Handle()) : nullptr;
            if (c == nullptr)
            {
                return;
            }
            c->clip.SetId(id);
            if (auto* resources = foundation::script::CurrentRunResources())
            {
                c->clip.Bind(*resources);
            }
        }

        [[nodiscard]] static SceneAudio of(foundation::script::Scene sceneHandle)
        {
            return SceneAudio{sceneHandle.scene};
        }
    };

    // The runtime subsystem: owns the ONE AudioEngine, injects the managers + system
    // into every scene (via the composition), pushes the winning listener, and exposes the
    // engine-global one-shot API (docs/design/audio.md §6).
    // THE audio manager set for a scene - injected by the subsystem at runtime AND by headless
    // scene consumers (Engine.SceneSurface). Runtime-only wiring (SetEngine) stays with the
    // subsystem; the AudioSceneSystem is engine-less (silent) until it. Add a manager => bump
    // the SceneSurface tripwire (engine::kSceneSystemCount).
    inline void AddAudioSceneManagers(scene::Scene& scene)
    {
        scene.AddSystem<AudioSourceComponentManager>();
        scene.AddSystem<AudioListenerComponentManager>();
        scene.AddSystem<AudioReverbZoneComponentManager>();
        scene.AddSystem<AudioSceneSystem>();
    }

    class AudioSubsystem final : public foundation::runtime::Subsystem, public scene::ISceneObserver
    {
    public:
        explicit AudioSubsystem(const AudioEngineSettings& engineSettings = {})
            : m_engineSettings(engineSettings)
        {
        }

        [[nodiscard]] AudioEngine* Engine() const noexcept { return m_engine.Get(); }

        // AFTER the scene subsystem (-500): voice/listener sync ran during the scene's
        // PostTransform phase; here the engine reaps + pumps and the listener lands.
        [[nodiscard]] i32 UpdateOrder() const noexcept override { return -100; }

        void OnSystemsReady(scene::Scene& scene) override
        {
            AudioSceneSystem* system = scene.GetSystem<AudioSceneSystem>();
            system->SetEngine(m_engine.Get());
            m_systems.PushBack(SceneEntry{&scene, system});
        }
        void OnDestroying(scene::Scene& scene) override
        {
            for (usize i = 0; i < m_systems.Size(); ++i)
            {
                if (m_systems[i].scene == &scene)
                {
                    m_systems.RemoveAt(i);
                    return;
                }
            }
        }

        // Defined in SubsystemImpl.cpp: listener push (active camera fallback = render
        // dep) + the engine tick.
        void Update(f32 deltaTime) override;

        // ---- engine-global one-shots (docs/design/audio.md §6) ----

        [[nodiscard]] VoiceHandle PlayOneShot(const RefPtr<AudioClip>& clip,
                                              AudioBus bus = AudioBus::Effects, f32 volume = 1.0f,
                                              f32 pitch = 1.0f)
        {
            if (m_engine.Get() == nullptr)
            {
                return {};
            }
            AudioPlayParams params;
            params.bus = bus;
            params.volume = volume;
            params.pitch = pitch;
            return m_engine->Play(clip, params);
        }

        [[nodiscard]] VoiceHandle PlayOneShot3D(const RefPtr<AudioClip>& clip, Float3 position,
                                                const AudioPlayParams& baseParams = {})
        {
            if (m_engine.Get() == nullptr)
            {
                return {};
            }
            AudioPlayParams params = baseParams;
            params.spatial = true;
            params.position = position;
            return m_engine->Play(clip, params);
        }

        /// One cue TRIGGER as a one-shot: weighted variant + jitter through the same
        /// resolution the components use; no-repeat state tracked per cue product.
        [[nodiscard]] VoiceHandle PlayCueOneShot(const RefPtr<SoundCue>& cue,
                                                 AudioBus bus = AudioBus::Effects)
        {
            AudioPlayParams params;
            params.bus = bus;
            return PlayCueResolved(cue, params);
        }
        [[nodiscard]] VoiceHandle PlayCueOneShot3D(const RefPtr<SoundCue>& cue, Float3 position,
                                                   const AudioPlayParams& baseParams = {})
        {
            AudioPlayParams params = baseParams;
            params.spatial = true;
            params.position = position;
            return PlayCueResolved(cue, params);
        }

        /// Binds THIS subsystem's script binding as `context`'s audio service - the
        /// scripting facade (class Audio below) resolves it per context. Call once per
        /// context, AFTER init. `resources` (optional) enables content-path playback
        /// (Audio.playOneShot("Sounds/laser") etc.); without it those calls no-op.
        void ExposeToScript(foundation::script::IScriptContext& context,
                            foundation::resource::ResourceManager* resources = nullptr)
        {
            m_scriptBinding.engine = m_engine.Get();
            m_scriptBinding.subsystem = this;
            m_scriptBinding.resources = resources;
            context.SetService(kAudioScriptService, &m_scriptBinding);
        }

        // ---- content-path playback (the facade's resource addressing) ----
        // `path` = the source-DB content path shown in the editor; the cook mirrors
        // group paths AND guids into the cooked DB, so the same string resolves against
        // the runtime manager's database. Missing/uncooked/mistyped content warns ONCE
        // per path and no-ops - scripts never fault on content problems. Paths resolve
        // by TYPE: a clip path plays the clip, a cue path resolves one weighted trigger
        // (a cue handed to playOneShot behaves like the component's cue-wins rule).

        VoiceHandle PlayOneShotByPath(foundation::resource::ResourceManager& resources,
                                      StringView path, AudioBus bus = AudioBus::Effects)
        {
            const ResolvedPathContent content = ResolveContentPath(resources, path);
            if (content.cue.Get() != nullptr)
            {
                AudioPlayParams params;
                params.bus = bus;
                return PlayCueResolved(content.cue, params);
            }
            if (content.clip.Get() == nullptr)
            {
                return {};
            }
            return PlayOneShot(content.clip, bus);
        }

        VoiceHandle PlayOneShot3DByPath(foundation::resource::ResourceManager& resources,
                                        StringView path, Float3 position)
        {
            const ResolvedPathContent content = ResolveContentPath(resources, path);
            if (content.cue.Get() != nullptr)
            {
                AudioPlayParams params;
                params.spatial = true;
                params.position = position;
                return PlayCueResolved(content.cue, params);
            }
            if (content.clip.Get() == nullptr)
            {
                return {};
            }
            return PlayOneShot3D(content.clip, position);
        }

        VoiceHandle PlayCueByPath(foundation::resource::ResourceManager& resources, StringView path,
                                  AudioBus bus = AudioBus::Effects)
        {
            const ResolvedPathContent content = ResolveContentPath(resources, path);
            if (content.cue.Get() != nullptr)
            {
                return PlayCueOneShot(content.cue, bus);
            }
            if (content.clip.Get() != nullptr)
            {
                return PlayOneShot(content.clip, bus);
            }
            return {};
        }

        VoiceHandle PlayMusicByPath(foundation::resource::ResourceManager& resources, StringView path,
                                    f32 crossFadeSeconds = 1.0f)
        {
            const ResolvedPathContent content = ResolveContentPath(resources, path);
            RefPtr<AudioClip> clip = content.clip;
            if (clip.Get() == nullptr && content.cue.Get() != nullptr)
            {
                // Music from a cue: resolve ONE variant and cross-fade to it.
                CueOneShotState* found = m_cueOneShotState.Find(content.cue.Get());
                CueOneShotState& state =
                    found != nullptr
                        ? *found
                        : m_cueOneShotState.InsertOrAssign(content.cue.Get(), CueOneShotState{});
                const SoundCuePick pick = ResolveSoundCue(
                    *content.cue, m_cueRandom, state.lastVariant, state.sequentialCursor);
                if (pick.variantIndex >= 0)
                {
                    state.lastVariant = pick.variantIndex;
                    clip = content.cue->variants[static_cast<usize>(pick.variantIndex)].clip;
                }
            }
            if (clip.Get() == nullptr)
            {
                return {};
            }
            return PlayMusic(clip, crossFadeSeconds);
        }

        // ---- music (scene-less, survives scene swaps; audio.md P2) ----
        VoiceHandle PlayMusic(const RefPtr<AudioClip>& clip, f32 crossFadeSeconds = 1.0f,
                              f32 volume = 1.0f)
        {
            return m_engine.Get() != nullptr ? m_engine->PlayMusic(clip, crossFadeSeconds, volume)
                                             : VoiceHandle{};
        }
        void StopMusic(f32 fadeSeconds = 1.0f)
        {
            if (m_engine.Get() != nullptr)
            {
                m_engine->StopMusic(fadeSeconds);
            }
        }

        void Stop(VoiceHandle handle)
        {
            if (m_engine.Get() != nullptr)
            {
                m_engine->Stop(handle);
            }
        }
        [[nodiscard]] bool IsPlaying(VoiceHandle handle) const
        {
            return m_engine.Get() != nullptr && m_engine->IsPlaying(handle);
        }

        void SetBusVolume(AudioBus bus, f32 volume)
        {
            if (m_engine.Get() != nullptr)
            {
                m_engine->SetBusVolume(bus, volume);
            }
        }
        [[nodiscard]] f32 BusVolume(AudioBus bus) const
        {
            return m_engine.Get() != nullptr ? m_engine->BusVolume(bus) : 0.0f;
        }

    protected:
        void OnInit() override
        {
            m_engine = MakeUnique<AudioEngine>(DefaultAllocator(), m_engineSettings);
            for (const SceneEntry& entry : m_systems)
            {
                entry.system->SetEngine(m_engine.Get()); // scenes created pre-init
            }
            RegisterAudioComponentReflection();
        }
        void OnReady() override
        {
            if (foundation::runtime::Context* context = GetContext())
            {
            if (auto* scenes = context->GetSubsystem<engine::scene::SceneSubsystem>())
            {
                scenes->RegisterObserver(this, scene::SceneLifecycleStage::SystemsReady);
                scenes->RegisterObserver(this, scene::SceneLifecycleStage::Destroying);
            }
            }
        }
        void OnShutdown() override
        {
            if (foundation::runtime::Context* context = GetContext())
            {
            if (auto* scenes = context->GetSubsystem<engine::scene::SceneSubsystem>())
            {
                scenes->UnregisterObserver(this);
            }
            }
            m_engine = nullptr;
        }

        struct SceneEntry
        {
            scene::Scene* scene = nullptr;
            AudioSceneSystem* system = nullptr;
        };
        [[nodiscard]] Span<const SceneEntry> Systems() const noexcept
        {
            return Span<const SceneEntry>{m_systems.Data(), m_systems.Size()};
        }

    private:
        struct ResolvedPathContent
        {
            RefPtr<AudioClip> clip;
            RefPtr<SoundCue> cue;
        };

        // Path -> cooked product, sniffed by the instance's TYPE name. Binding through
        // the manager caches the product exactly like component refs do.
        [[nodiscard]] ResolvedPathContent
        ResolveContentPath(foundation::resource::ResourceManager& resources, StringView path)
        {
            ResolvedPathContent result;
            foundation::content::Instance* instance = resources.Database().GetInstance(path);
            if (instance == nullptr)
            {
                WarnPathOnce(path, u8"no content at this path");
                return result;
            }
            if (instance->TypeName() == u8"SoundCueSource")
            {
                result.cue = RefPtr<SoundCue>(resources.Bind<SoundCue>(instance->Id()).Get());
                if (result.cue.Get() == nullptr)
                {
                    WarnPathOnce(path, u8"sound cue failed to load (uncooked?)");
                }
            }
            else if (instance->TypeName() == u8"AudioClipSource")
            {
                result.clip = RefPtr<AudioClip>(resources.Bind<AudioClip>(instance->Id()).Get());
                if (result.clip.Get() == nullptr)
                {
                    WarnPathOnce(path, u8"audio clip failed to load (uncooked?)");
                }
            }
            else
            {
                WarnPathOnce(path, u8"not an audio clip or sound cue");
            }
            return result;
        }

        void WarnPathOnce(StringView path, StringView reason)
        {
            String key(path);
            if (m_warnedScriptPaths.Find(key) != nullptr)
            {
                return;
            }
            m_warnedScriptPaths.InsertOrAssign(Move(key), true);
            LOG_WARNING(u8"Audio",
                                 u8"script audio play '{}': {} - call ignored "
                                 u8"(warned once per path)",
                                 path, reason);
        }

        [[nodiscard]] VoiceHandle PlayCueResolved(const RefPtr<SoundCue>& cue,
                                                  AudioPlayParams params)
        {
            if (m_engine.Get() == nullptr || cue.Get() == nullptr)
            {
                return {};
            }
            CueOneShotState* found = m_cueOneShotState.Find(cue.Get());
            CueOneShotState& state =
                found != nullptr ? *found
                                 : m_cueOneShotState.InsertOrAssign(cue.Get(), CueOneShotState{});
            const SoundCuePick pick =
                ResolveSoundCue(*cue, m_cueRandom, state.lastVariant, state.sequentialCursor);
            if (pick.variantIndex < 0)
            {
                return {};
            }
            state.lastVariant = pick.variantIndex;
            params.pitch *= pick.pitch;
            params.volume *= pick.volume;
            params.allowDedupe = false; // distinct triggers, never merged
            return m_engine->Play(cue->variants[static_cast<usize>(pick.variantIndex)].clip,
                                  params);
        }

        struct CueOneShotState
        {
            i32 lastVariant = -1;
            u32 sequentialCursor = 0;
        };

        AudioEngineSettings m_engineSettings;
        UniquePtr<AudioEngine> m_engine;
        Array<SceneEntry> m_systems;
        Random m_cueRandom;
        HashMap<const SoundCue*, CueOneShotState> m_cueOneShotState;
        AudioScriptBinding m_scriptBinding;        // the bound script service payload
        HashMap<String, bool> m_warnedScriptPaths; // warn-once per content path
    };
    // The scripting facade (the Input facade's twin): statics on a foreign class
    // resolving the CURRENT script context's bound AudioScriptBinding. Bus addressing
    // by name ("master"/"effects"/"music"/"ui" + the layout's custom buses; unknown =
    // no-op / neutral read). Clip/cue PLAYBACK addresses content by its source-DB
    // path (the string shown in the editor) - resource addressing, no entity handles
    // needed; missing content warns once per path and no-ops.
    class Audio final : public Object
    {
        RTTI_OBJECT(Audio, Object)
    public:
        [[nodiscard]] static AudioScriptBinding* ResolveBinding()
        {
            foundation::script::IScriptContext* context = foundation::script::CurrentScriptContext();
            return context != nullptr
                       ? static_cast<AudioScriptBinding*>(context->GetService(kAudioScriptService))
                       : nullptr;
        }

        [[nodiscard]] static AudioEngine* Resolve()
        {
            AudioScriptBinding* binding = ResolveBinding();
            return binding != nullptr ? binding->engine : nullptr;
        }

        // Playback binding: subsystem + resource manager both required (the binding
        // carries them when the host wired a manager into ExposeToScript).
        [[nodiscard]] static bool
        ResolvePlayback(AudioSubsystem*& outSubsystem,
                        foundation::resource::ResourceManager*& outResources)
        {
            AudioScriptBinding* binding = ResolveBinding();
            if (binding == nullptr || binding->subsystem == nullptr ||
                binding->resources == nullptr)
            {
                return false;
            }
            outSubsystem = binding->subsystem;
            outResources = binding->resources;
            return true;
        }

        // ---- content-path playback (returns whether a voice actually started) ----
        static bool playOneShot(String path)
        {
            AudioSubsystem* subsystem = nullptr;
            foundation::resource::ResourceManager* resources = nullptr;
            if (!ResolvePlayback(subsystem, resources))
            {
                return false;
            }
            return subsystem->PlayOneShotByPath(*resources, path.AsView()).IsValid();
        }
        static bool playOneShot3D(String path, f32 x, f32 y, f32 z)
        {
            AudioSubsystem* subsystem = nullptr;
            foundation::resource::ResourceManager* resources = nullptr;
            if (!ResolvePlayback(subsystem, resources))
            {
                return false;
            }
            return subsystem->PlayOneShot3DByPath(*resources, path.AsView(), Float3{x, y, z})
                .IsValid();
        }
        static bool playCue(String path)
        {
            AudioSubsystem* subsystem = nullptr;
            foundation::resource::ResourceManager* resources = nullptr;
            if (!ResolvePlayback(subsystem, resources))
            {
                return false;
            }
            return subsystem->PlayCueByPath(*resources, path.AsView()).IsValid();
        }
        static bool playMusic(String path, f32 fadeSeconds)
        {
            AudioSubsystem* subsystem = nullptr;
            foundation::resource::ResourceManager* resources = nullptr;
            if (!ResolvePlayback(subsystem, resources))
            {
                return false;
            }
            return subsystem->PlayMusicByPath(*resources, path.AsView(), Max(fadeSeconds, 0.0f))
                .IsValid();
        }

        [[nodiscard]] static bool BusFromName(StringView name, AudioBus& out)
        {
            return AudioBusFromName(name, out); // the shared case-insensitive seam
        }

        // Bus addressing: the four fixed names first, then the applied layout's NAMED
        // custom buses (item: named bus trees) - unknown = no-op / neutral read.
        static void setBusVolume(String bus, f32 volume)
        {
            AudioEngine* engine = Resolve();
            if (engine == nullptr)
            {
                return;
            }
            AudioBus which{};
            const f32 clamped = Clamp(volume, 0.0f, 4.0f);
            if (BusFromName(bus.AsView(), which))
            {
                engine->SetBusVolume(which, clamped);
            }
            else
            {
                engine->SetNamedBusVolume(bus.AsView(), clamped);
            }
        }
        [[nodiscard]] static f32 busVolume(String bus)
        {
            AudioEngine* engine = Resolve();
            if (engine == nullptr)
            {
                return 1.0f;
            }
            AudioBus which{};
            if (BusFromName(bus.AsView(), which))
            {
                return engine->BusVolume(which);
            }
            return engine->HasNamedBus(bus.AsView()) ? engine->NamedBusVolume(bus.AsView()) : 1.0f;
        }
        static void setBusMuted(String bus, bool muted)
        {
            AudioEngine* engine = Resolve();
            if (engine == nullptr)
            {
                return;
            }
            AudioBus which{};
            if (BusFromName(bus.AsView(), which))
            {
                engine->SetBusMuted(which, muted);
            }
            else
            {
                engine->SetNamedBusMuted(bus.AsView(), muted);
            }
        }
        [[nodiscard]] static bool busMuted(String bus)
        {
            AudioEngine* engine = Resolve();
            if (engine == nullptr)
            {
                return false;
            }
            AudioBus which{};
            if (BusFromName(bus.AsView(), which))
            {
                return engine->BusMuted(which);
            }
            return engine->NamedBusMuted(bus.AsView());
        }
        static void stopMusic(f32 fadeSeconds)
        {
            if (AudioEngine* engine = Resolve())
            {
                engine->StopMusic(fadeSeconds);
            }
        }
    };

    void RegisterAudioScriptFacade();

}
