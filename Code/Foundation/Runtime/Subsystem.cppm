// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Runtime - :subsystem partition
//
// Subsystem: the unit of engine functionality the SubsystemManager owns and drives.
// Lifecycle: OnRegister -> Init/Ready -> (frames) -> PrepareShutdown/Shutdown.
// Frame phases run in UpdateOrder() order within the SubsystemManager.

module;
#include "Core/Prelude.h"

export module foundation.runtime:subsystem;

import foundation.core;

namespace core = foundation::core;

export namespace foundation::runtime
{
    class SubsystemManager;

    class Subsystem
    {
    public:
        virtual ~Subsystem() = default;

        [[nodiscard]] SubsystemManager* Manager() const noexcept { return m_manager; }
        [[nodiscard]] bool IsInitialized() const noexcept { return m_initialized; }

        // Lower runs earlier in each frame phase (and in Init/Ready order).
        [[nodiscard]] virtual core::i32 UpdateOrder() const noexcept { return 0; }

        // --- registration (called by SubsystemManager) ---
        virtual void OnRegister(SubsystemManager* manager) { m_manager = manager; }
        virtual void OnUnregister() { m_manager = nullptr; }

        // --- lifecycle (called by SubsystemManager; guard once) ---
        void Init()
        {
            if (!m_initialized)
            {
                OnInit();
                m_initialized = true;
            }
        }
        void Ready() { OnReady(); }
        void PrepareShutdown() { OnPrepareShutdown(); }
        void Shutdown()
        {
            if (m_initialized)
            {
                OnShutdown();
                m_initialized = false;
            }
        }

        // --- per-frame phases ---
        virtual void BeginFrame(core::f32 /*deltaTime*/) {}
        virtual void Update(core::f32 /*deltaTime*/) {}
        virtual void PostUpdate(core::f32 /*deltaTime*/) {}
        virtual void EndFrame() {}

    protected:
        virtual void OnInit() {}
        virtual void OnReady() {}
        virtual void OnPrepareShutdown() {}
        virtual void OnShutdown() {}

    private:
        SubsystemManager* m_manager = nullptr;
        bool m_initialized = false;
    };
}
