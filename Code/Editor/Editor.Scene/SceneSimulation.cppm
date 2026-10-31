// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Scene - :scene_simulation partition.
//
// Edit-mode Simulate over a page's scene. Starting takes a snapshot, then starts and runs the
// scene, with the page's command stack locked meanwhile (runtime mutations do not belong in the
// edit history, and undoing into entities the restore recreates would tangle their guids). Paused,
// the simulation holds. Stopped, the scene stops and the snapshot is restored into the same scene,
// so borrowed scene pointers stay valid and the guid-keyed selection resolves again (what the run
// spawned drops out). The game's UI stays non-interactive meanwhile: Simulate's pointer serves
// selection and the camera, and the Game tab is the interactive run.

module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"

export module editor.scene:scene_simulation;

import foundation.core;
import foundation.resource;
import foundation.scene;
import foundation.scene.resource;
import editor.core;

using namespace foundation::core;

export namespace editor
{
    class SceneSimulation
    {
    public:
        [[nodiscard]] bool IsRunning() const noexcept { return m_running; }
        [[nodiscard]] bool IsPaused() const noexcept { return m_paused; }

        /// Snapshot `scene` and run it with `commands` locked; false when it is already running or
        /// the snapshot failed (reported in the log).
        bool Start(foundation::scene::Scene& scene, EditorCommandStack& commands)
        {
            if (m_running)
                return false;
            m_snapshot = foundation::scene::SceneSnapshot::Capture(scene);
            if (!m_snapshot)
            {
                LOG_ERROR(u8"Editor", u8"Simulate: scene snapshot capture failed");
                return false;
            }
            scene.Start();
            scene.SetSimulationEnabled(true);
            commands.SetLocked(true);
            m_running = true;
            m_paused = false;
            return true;
        }
        /// Holds or resumes a running simulation (the per-frame freeze, not the start and stop
        /// callbacks the systems get on the big transitions).
        void Pause(foundation::scene::Scene& scene, bool paused)
        {
            if (!m_running)
                return;
            m_paused = paused;
            scene.SetSimulationEnabled(!paused);
        }
        /// Stops and restores the snapshot into `scene`, its resources resolved through `resources`
        /// (null: none), `commands` unlocked.
        void Stop(foundation::scene::Scene& scene, EditorCommandStack& commands, foundation::resource::ResourceManager* resources)
        {
            if (!m_running)
                return;
            scene.Stop();
            if (m_snapshot)
            {
                if (!m_snapshot->Restore(scene, resources).IsOk())
                    LOG_ERROR(u8"Editor", u8"Simulate: snapshot restore failed");
                m_snapshot = nullptr;
            }
            scene.SetSimulationEnabled(false);
            commands.SetLocked(false);
            m_running = false;
            m_paused = false;
        }

    private:
        UniquePtr<foundation::scene::SceneSnapshot> m_snapshot;
        bool m_running = false;
        bool m_paused = false;
    };
}
