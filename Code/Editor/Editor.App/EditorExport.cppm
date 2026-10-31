// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::App - :export partition.
//
// EditorExport: Project > Export in the editor, kept out of the application. A requested export
// waits for a background cook, then runs as a job. Two steps run on the main thread first because
// they load scenes: transcoding the scenes' text sources to the wire format and, when a preset
// prunes to what is reachable, finding the reachable closure. The cook-free pack then runs off the
// main thread. It keeps the export presets the Export dialog edits, finds the templates (the
// user's override, else the environment's, else the user data's), and reports the resource
// manager's live products to the log.
module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"

export module editor.app:export_service;

import foundation.core;
import foundation.content;
import foundation.vfs;
import foundation.resource;
import foundation.settings;
import pipeline.core;
import editor.core;

using namespace foundation::core;

export namespace editor::app
{
    class EditorExport
    {
    public:
        /// The services it works with: the per-user settings (the templates root), the data root
        /// (the shader cook reads its Shaders from it), and the callback for a written export.
        struct Services
        {
            editor::EditorContext* context = nullptr;
            editor::EditorCookService* cook = nullptr;
            editor::EditorJobService* jobs = nullptr;
            pipeline::BuilderRegistry* builders = nullptr;
            foundation::settings::Settings* userSettings = nullptr;
            String dataRoot;
            /// Called when the export was written to `folder` (the application offers to open it).
            Function<void(StringView folder)> onExported;
        };

        EditorExport(IAllocator& allocator, Services services) : m_allocator(allocator), m_services(Move(services)) {}
        EditorExport(const EditorExport&) = delete;
        EditorExport& operator=(const EditorExport&) = delete;

        /// Sets the project it exports (null between projects) and reads its presets from it (its
        /// export_presets.xml, else the default set), so presets saved earlier survive a restart.
        void SetProject(editor::EditorProject* project)
        {
            m_project = project;
            if (m_project == nullptr)
                return;
            foundation::vfs::NativeFileSystem projectFs(m_project->Directory(), m_allocator);
            m_presetsController.Load(projectFs);
        }

        /// Exports `presetName` (or every preset): a background cook first, then the job.
        void Run(StringView presetName, bool all)
        {
            editor::EditorContext& context = *m_services.context;
            if (m_project == nullptr)
            {
                context.Notify(editor::NoticeKind::Info, u8"Open a project first.");
                return;
            }
            if (m_services.jobs->IsBusy() || m_pending.active)
            {
                context.Notify(editor::NoticeKind::Info, u8"An export is already in progress.");
                return;
            }
            m_pending = Pending{String(presetName), all, /*waitingCook*/ true, /*active*/ true};
            context.Notify(editor::NoticeKind::Info, u8"Cooking before export...");
            m_services.cook->RequestCook(false); // safe background cook; Pump submits the job after it
        }
        [[nodiscard]] bool IsPending() const noexcept { return m_pending.active; }

        /// Called each frame: submits an export waiting on its cook to the jobs once the cook is done.
        void Pump()
        {
            if (m_pending.active && m_pending.waitingCook && !m_services.cook->IsCooking())
            {
                m_pending.waitingCook = false;
                SubmitJob(m_pending.presetName, m_pending.all);
                m_pending.active = false; // the job owns it now
            }
        }

        /// The templates' folder: the user's override (Preferences), else $ENV_TEMPLATES_DIR, else
        /// <user-data>/templates.
        [[nodiscard]] String TemplatesRoot() const
        {
            StringView overrideRoot;
            if (m_services.userSettings != nullptr)
                if (const editor::EditorExportSettings* s = m_services.userSettings->Find<editor::EditorExportSettings>())
                    overrideRoot = s->templatesRoot.AsView();
            return editor::ResolveTemplatesRoot(overrideRoot);
        }
        /// The templates as the main thread sees them (the Export dialog's list).
        void BuildTemplateRegistry(editor::TemplateRegistry& out)
        {
            const String toolDir = GetExecutableDirectory();
            const String templatesRoot = TemplatesRoot();
            foundation::vfs::NativeFileSystem toolFs(toolDir.AsView(), m_allocator);
            foundation::vfs::NativeFileSystem rootFs(templatesRoot.AsView(), m_allocator);
            out.Refresh(templatesRoot.AsView(), &rootFs, toolDir.AsView(), &toolFs);
        }

        /// The presets the Export dialog edits, which also saves them to the project.
        [[nodiscard]] editor::ExportPresetsController& Presets() noexcept { return m_presetsController; }
        void SavePresets()
        {
            if (m_project == nullptr)
                return;
            foundation::vfs::NativeFileSystem projectFs(m_project->Directory(), m_allocator);
            if (!m_presetsController.Save(*projectFs.AsWritable()).IsOk())
                m_services.context->Notify(editor::NoticeKind::Error, u8"Saving export presets FAILED (see console).");
        }

        /// Logs the resource manager's live products by type (Project > Report Resource Memory).
        static void ReportResourceMemory(foundation::resource::ResourceManager* resources)
        {
            if (resources == nullptr)
            {
                LOG_INFO(u8"Editor", u8"resource report: no resource manager (no project open)");
                return;
            }
            Array<foundation::resource::ResourceManager::LiveProductRow> rows;
            resources->ReportLiveProducts(rows);
            usize totalLive = 0;
            usize totalUnreferenced = 0;
            for (const auto& row : rows)
            {
                const char* name = row.type != nullptr ? row.type->name : "<unresolved>";
                LOG_INFO(u8"Editor", u8"resource report: {} - live {}, pending {}, failed {}, unreferenced {}",
                         StringView(reinterpret_cast<const char8_t*>(name)), static_cast<u64>(row.live), static_cast<u64>(row.pending),
                         static_cast<u64>(row.failed), static_cast<u64>(row.unreferenced));
                totalLive += row.live;
                totalUnreferenced += row.unreferenced;
            }
            LOG_INFO(u8"Editor",
                     u8"resource report: {} live products total, {} unreferenced (cache-only - what a purge would release); sizes "
                     u8"arrive with allocator tagging (I5)",
                     static_cast<u64>(totalLive), static_cast<u64>(totalUnreferenced));
        }

    private:
        struct Pending
        {
            String presetName;
            bool all = false;
            bool waitingCook = false;
            bool active = false;
        };

        [[nodiscard]] static String Absolutize(StringView path)
        {
            return PathIsAbsolute(path) ? String(path) : PathJoin(GetCurrentDirectory().AsView(), path);
        }
        [[nodiscard]] bool AnyPresetPrunes(bool all, StringView presetName) const
        {
            if (all)
            {
                for (const editor::ExportPreset& p : m_presets.presets)
                    if (p.pruneToReachable)
                        return true;
                return false;
            }
            const editor::ExportPreset* p = m_presets.Find(presetName);
            return p != nullptr && p->pruneToReachable;
        }

        void SubmitJob(String presetName, bool all)
        {
            editor::EditorProject* project = m_project;
            editor::EditorContext& context = *m_services.context;
            pipeline::BuilderRegistry* builders = m_services.builders;
            // The main thread's part: transcode the scenes' text sources to the binary wire format
            // (the stager needs the scene subsystem), held here for the job (one export at a time).
            m_sceneStreams.Clear();
            if (context.SceneStreamStager)
                editor::CollectSceneStreams(*project->SourceDb().RootGroup(), context.SceneStreamStager, m_sceneStreams);
            const HashMap<Guid, Array<byte>>* sceneStreams = &m_sceneStreams;
            // Read the presets here: the reachability scan below asks whether one prunes.
            m_presets.presets.Clear();
            {
                foundation::vfs::NativeFileSystem projectFs(project->Directory(), m_allocator);
                if (!editor::LoadExportPresets(projectFs, m_presets).IsOk())
                    editor::DefaultExportPresets(m_presets);
            }
            // Pruning needs the scenes' references, which means loading scenes, so the reachable
            // set is found here and the job only cooks and packs it. Without a scanner the job packs
            // everything.
            m_reachableRoots.Clear();
            m_reachableValid = false;
            if (AnyPresetPrunes(all, presetName.AsView()) && context.SceneRefScanner)
            {
                editor::EditorContext* contextPtr = &context;
                const editor::SceneReferenceScanner adapter =
                    [contextPtr](foundation::content::Instance& inst, foundation::content::ContentDatabase& db, editor::SceneReferences& refs)
                { contextPtr->SceneRefScanner(inst, db, refs.resources, refs.prefabs); };
                const Array<editor::ExportRoot> seeds = editor::CollectExportRoots(*project);
                m_reachableRoots = editor::ExpandReachableRoots(*project, seeds, adapter);
                m_reachableValid = true;
            }
            const Array<Guid>* reachableRoots = m_reachableValid ? &m_reachableRoots : nullptr;
            const editor::ExportPresetSet* presetsPtr = &m_presets;

            const String toolDir = GetExecutableDirectory();
            const String templatesRoot = TemplatesRoot();
            const String dataRoot = m_services.dataRoot;
            const String outRoot = Absolutize(PathJoin(project->Directory(), u8"Dist").AsView());
            const String title(all ? StringView(u8"Export All") : StringView(u8"Export"));
            EditorExport* self = this;
            m_services.jobs->Submit(
                title.AsView(),
                [project, builders, toolDir, templatesRoot, dataRoot, presetName, all, outRoot, sceneStreams, reachableRoots,
                 presetsPtr](editor::JobContext& ctx) -> Status
                {
                    foundation::vfs::NativeFileSystem toolFs(toolDir.AsView(), editor::EditorRootAllocator());
                    foundation::vfs::NativeFileSystem rootFs(templatesRoot.AsView(), editor::EditorRootAllocator());
                    editor::TemplateRegistry registry;
                    registry.Refresh(templatesRoot.AsView(), &rootFs, toolDir.AsView(), &toolFs);
                    const editor::ExportPresetSet& presets = *presetsPtr;
                    const editor::ExportProgress onProgress = [&ctx](StringView step, f32 fraction)
                    {
                        ctx.SetStep(step);
                        ctx.SetFraction(fraction);
                    };
                    if (all)
                    {
                        const Span<const editor::ExportPreset> span(presets.presets.Data(), presets.presets.Size());
                        return editor::ExportAll(*project, span, registry, *builders, outRoot.AsView(), dataRoot.AsView(), /*rebuild*/ false,
                                                 onProgress, /*cook*/ false, sceneStreams, /*scanner*/ nullptr, reachableRoots);
                    }
                    const editor::ExportPreset* preset = presets.Find(presetName.AsView());
                    if (preset == nullptr)
                        return Status{ErrorCode::NotFound};
                    editor::ExportResult result;
                    return editor::ExportOne(*project, *preset, registry, *builders, outRoot.AsView(), dataRoot.AsView(), /*rebuild*/ false,
                                             &result, onProgress, /*cook*/ false, sceneStreams, /*scanner*/ nullptr, reachableRoots);
                },
                [self, outRoot](Status s) // main thread
                {
                    if (!s.IsOk())
                    {
                        self->m_services.context->Notify(editor::NoticeKind::Error, u8"Export failed (see Console).");
                        return;
                    }
                    self->m_services.context->SetStatus(u8"Export complete.");
                    if (self->m_services.onExported)
                        self->m_services.onExported(outRoot.AsView());
                });
        }

        IAllocator& m_allocator;
        Services m_services;
        editor::EditorProject* m_project = nullptr; // borrowed (the session's)
        Pending m_pending;
        editor::ExportPresetsController m_presetsController; // backs the Export dialog
        HashMap<Guid, Array<byte>> m_sceneStreams;           // the running job's transcoded scenes
        editor::ExportPresetSet m_presets;                   // the running job's presets
        Array<Guid> m_reachableRoots;                        // the running job's reachable closure
        bool m_reachableValid = false;
    };
}
