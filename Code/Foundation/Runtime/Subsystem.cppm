// Runtime - :subsystem partition
//
// Subsystem: the unit of engine functionality the Context owns and drives.
// Lifecycle: OnRegister -> Init/Ready -> (frames) -> PrepareShutdown/Shutdown.
// Frame phases run in UpdateOrder() order within the Context.

module;
#include "Core/Prelude.h"

export module foundation.runtime:subsystem;

import foundation.core;

namespace core = foundation::core;

export namespace foundation::runtime
{
    class Context;

    class Subsystem
    {
    public:
        virtual ~Subsystem() = default;

        [[nodiscard]] Context* GetContext() const noexcept { return m_context; }
        [[nodiscard]] bool IsInitialized() const noexcept { return m_initialized; }

        // Lower runs earlier in each frame phase (and in Init/Ready order).
        [[nodiscard]] virtual core::i32 UpdateOrder() const noexcept { return 0; }

        // --- registration (called by Context) ---
        virtual void OnRegister(Context* context) { m_context = context; }
        virtual void OnUnregister() { m_context = nullptr; }

        // --- lifecycle (called by Context; guard once) ---
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
        Context* m_context = nullptr;
        bool m_initialized = false;
    };
}
