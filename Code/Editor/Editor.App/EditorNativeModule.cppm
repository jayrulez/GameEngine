// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::App - :native_module partition.
//
// EditorNativeModule: a project's native code (game-native-code.md) in the editor, out of the
// application (Documentation/Specs/editor-gui.md, 5.1, layer 3). It loads the module the project
// declares into the EMBEDDED runtime when the project opens, reloads it on request (Project >
// Reload Native Module) with every running game stopped and every live scene carried across, and
// unloads it when the project closes, before the runtime and the resources go.
//
// Always through a staged copy under the project's cache, never the declared path: on Windows
// LoadLibraryW holds the image open against writes, so mapping Native/<Target>.dll directly would
// stop "Build Native Module" relinking it; and a reload needs a fresh path anyway (dlopen
// refcounts by path).
module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"

export module editor.app:native_module;

import foundation.core;
import foundation.runtime;
import foundation.scene;
import foundation.scene.resource; // SceneSnapshot (the reload's scene bracket)
import foundation.resource;
import engine.project; // kProjectCacheDir
import engine.scene;   // SceneSubsystem, GlobalSceneContributionRecorder
import editor.core;

using namespace foundation::core;

export namespace editor::app
{
    class EditorNativeModule
    {
    public:
        explicit EditorNativeModule(IAllocator& allocator) : m_allocator(allocator) {}
        EditorNativeModule(const EditorNativeModule&) = delete;
        EditorNativeModule& operator=(const EditorNativeModule&) = delete;

        /// The project opened: its declared module loads into `runtime` (nothing when it declares
        /// none). A failure says so and the project opens without it (scripts work regardless).
        void Load(editor::EditorProject& project, editor::EditorContext& context, foundation::runtime::Context& runtime)
        {
            if (project.Settings().nativeModule.IsEmpty())
                return;
            m_plugins = MakeUnique<foundation::runtime::PluginHost>(m_allocator, runtime);
            m_plugins->AddRecorder(&engine::scene::GlobalSceneContributionRecorder());
            const String modulePath = StageCopy(project);
            bool ok = false;
            if (!modulePath.IsEmpty())
            {
                auto loaded = m_plugins->Load(modulePath.AsView());
                ok = loaded.HasValue();
                if (ok)
                    LOG_INFO(u8"Editor", u8"native game module '{}' loaded ('{}')", project.Settings().nativeModule, loaded.Value()->Name());
            }
            if (!ok)
            {
                context.Notify(editor::NoticeKind::Error, u8"Native game module failed to load (see Console).");
                LOG_ERROR(u8"Editor",
                          u8"native game module '{}' failed to load - continuing without it (a static editor build cannot load native "
                          u8"modules; use a shared build, or check the path)",
                          project.Settings().nativeModule);
                m_plugins.Reset();
            }
        }

        /// Reloads the project's module (it was rebuilt): every game run stops, every live scene on
        /// `runtime` is captured, the old module unloads (and stays mapped: no page is freed under
        /// a pointer the teardown missed), the new one loads, and the scenes are restored, its
        /// components rehydrating through its registrations. The scenes are restored even when the
        /// load fails, never left stripped.
        void Reload(editor::EditorProject* project, editor::EditorContext& context, foundation::runtime::Context& runtime,
                    foundation::resource::ResourceManager* resources)
        {
            if (project == nullptr || project->Settings().nativeModule.IsEmpty())
            {
                context.Notify(editor::NoticeKind::Info, u8"No native module declared (Project Settings > Native module).");
                return;
            }
            for (const UniquePtr<editor::EditorPage>& open : context.OpenPages())
                if (editor::IGameRunPage* run = editor::ServiceOf<editor::IGameRunPage>(open.Get()))
                    run->StopRun();
            struct SceneBracket
            {
                foundation::scene::Scene* scene;
                UniquePtr<foundation::scene::SceneSnapshot> snapshot;
            };
            Array<SceneBracket> brackets;
            if (auto* scenes = runtime.GetSubsystem<engine::scene::SceneSubsystem>())
                scenes->Registry().ForEachScene(
                    [&](foundation::scene::Scene& scene)
                    {
                        SceneBracket b{&scene, foundation::scene::SceneSnapshot::Capture(scene)};
                        if (b.snapshot)
                            brackets.PushBack(Move(b));
                    });
            const auto restoreScenes = [&]()
            {
                for (SceneBracket& b : brackets)
                    if (!b.snapshot->Restore(*b.scene, resources).IsOk())
                        LOG_ERROR(u8"Editor", u8"native reload: scene '{}' failed to restore", b.scene->Name());
                brackets.Clear();
            };
            if (m_plugins)
            {
                m_plugins->UnloadAll(/*closeLibraries*/ false);
                m_plugins.Reset();
            }
            const String staged = StageCopy(*project);
            if (staged.IsEmpty())
            {
                context.Notify(editor::NoticeKind::Error, u8"Native module reload failed - build it first (see Console).");
                restoreScenes();
                return;
            }
            m_plugins = MakeUnique<foundation::runtime::PluginHost>(m_allocator, runtime);
            m_plugins->AddRecorder(&engine::scene::GlobalSceneContributionRecorder());
            auto loaded = m_plugins->Load(staged.AsView());
            restoreScenes(); // after the contribution, so plugin components rehydrate
            if (loaded.HasValue())
            {
                context.Notify(editor::NoticeKind::Success, u8"Native module reloaded.");
                LOG_INFO(u8"Editor", u8"native game module reloaded ('{}', copy {})", loaded.Value()->Name(), static_cast<u64>(m_reloads));
            }
            else
            {
                context.Notify(editor::NoticeKind::Error, u8"Native module reload failed to load (see Console).");
                LOG_ERROR(u8"Editor", u8"native module reload: load failed for '{}'", staged);
                m_plugins.Reset();
            }
        }

        /// The project closes: the module's managers and systems withdraw while the runtime and
        /// the resources are still alive.
        void Unload()
        {
            if (m_plugins)
            {
                m_plugins->UnloadAll();
                m_plugins.Reset();
            }
        }

        [[nodiscard]] bool IsLoaded() const noexcept { return m_plugins.Get() != nullptr; }

        /// The module's staged copy (Library/native-hot/reload-<n>-<name>), from the declared path
        /// or, when it is absent, this platform's spelling of the same target (a project opened
        /// on Linux and Windows names one of Native/libX.so and Native/X.dll); empty (logged) when
        /// it cannot be read or written.
        [[nodiscard]] String StageCopy(const editor::EditorProject& project)
        {
            const StringView declared = project.Settings().nativeModule.AsView();
            String source = PathJoin(project.Directory(), declared);
            if (!FileExists(source.AsView()))
            {
                const String target = editor::detail::NativeTargetFromModulePath(declared);
                if (!target.IsEmpty())
                {
#if PLATFORM_WINDOWS
                    const String alternative = Format(u8"Native/{}.dll", target);
#else
                    const String alternative = Format(u8"Native/lib{}.so", target);
#endif
                    const String alternativePath = PathJoin(project.Directory(), alternative.AsView());
                    if (FileExists(alternativePath.AsView()))
                    {
                        LOG_INFO(u8"Editor", u8"native module '{}' not present; using this platform's '{}'", declared, alternative);
                        source = alternativePath;
                    }
                }
            }
            auto bytes = ReadFile(source.AsView(), m_allocator);
            if (!bytes.HasValue())
            {
                LOG_ERROR(u8"Editor", u8"native module: cannot read '{}'", source);
                return String{};
            }
            const String hotDir = PathJoin(project.Directory(), Format(u8"{}/native-hot", engine::project::kProjectCacheDir).AsView());
            (void)CreateDirectory(hotDir.AsView());
            // The resolved path's name, so the staged copy keeps the extension actually loaded.
            const String staged = Format(u8"{}/reload-{}-{}", hotDir, ++m_reloads, PathFilename(source.AsView()));
            if (!WriteFile(staged.AsView(), Span<const byte>{bytes.Value().Data(), bytes.Value().Size()}).IsOk())
            {
                LOG_ERROR(u8"Editor", u8"native module: cannot stage '{}'", staged);
                return String{};
            }
            return staged;
        }

    private:
        IAllocator& m_allocator;
        UniquePtr<foundation::runtime::PluginHost> m_plugins;
        u32 m_reloads = 0; // versions the staged copies
    };
}
