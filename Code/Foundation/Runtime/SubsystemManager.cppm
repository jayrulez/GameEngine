// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Runtime - :context partition
//
// Context: owns the engine's subsystems, looks them up by type, and drives
// their lifecycle and per-frame phases in UpdateOrder. Type identity uses the
// reflection TypeOf<T>() (We have -fno-rtti, so no std::type_index).

module;
#include "Core/Prelude.h"
#include <type_traits>

export module foundation.runtime:context;

import foundation.core;
import :subsystem;

namespace core = foundation::core;

export namespace foundation::runtime
{
    class Context
    {
    public:
        explicit Context(core::IAllocator& allocator) noexcept
            : m_allocator(&allocator)
        {
        }

        ~Context() { Dispose(); }

        Context(const Context&) = delete;
        Context& operator=(const Context&) = delete;

        [[nodiscard]] bool IsRunning() const noexcept { return m_running; }

        // The runtime's allocator authority: subsystems thread THIS (usually
        // wrapped in a per-system TaggedAllocator) instead of reaching for the
        // ambient default.
        [[nodiscard]] core::IAllocator& Allocator() const noexcept { return *m_allocator; }

        // Constructs and registers a subsystem of type T (one per type). Returns
        // a borrowed pointer; the Context owns it. If the Context is already
        // running, the subsystem is brought up (Init + Ready) immediately.
        template <typename T, typename... Args>
        T* AddSubsystem(Args&&... args)
        {
            static_assert(std::is_base_of_v<Subsystem, T>, "T must derive from Subsystem");
            T* subsystem = m_allocator->New<T>(core::Forward<Args>(args)...);
            m_owned.PushBack(
                core::UniquePtr<Subsystem>(static_cast<Subsystem*>(subsystem), *m_allocator));
            RegisterInternal(&core::TypeOf<T>(), static_cast<Subsystem*>(subsystem));
            return subsystem;
        }

        // Registers a subsystem the caller owns (e.g. a plugin owns its own
        // subsystems). The Context tracks it for lookup and per-frame phases but
        // never destroys it. Brings it up immediately if the Context is running.
        template <typename T>
        T* RegisterSubsystem(T* subsystem)
        {
            static_assert(std::is_base_of_v<Subsystem, T>, "T must derive from Subsystem");
            RegisterInternal(&core::TypeOf<T>(), static_cast<Subsystem*>(subsystem));
            return subsystem;
        }

        // Removes the subsystem of type T (shutting it down first if running) and,
        // if the Context owns it, destroys it. No-op if not registered.
        template <typename T>
        void RemoveSubsystem()
        {
            RemoveByType(&core::TypeOf<T>());
        }

        template <typename T>
        [[nodiscard]] T* GetSubsystem() noexcept
        {
            Subsystem* const* found = m_byType.Find(core::TypeOf<T>().id);
            return (found != nullptr) ? static_cast<T*>(*found) : nullptr;
        }

        template <typename T>
        [[nodiscard]] bool HasSubsystem() const noexcept
        {
            return m_byType.Contains(core::TypeOf<T>().id);
        }

        // Init then Ready, in UpdateOrder; marks the context running.
        void Startup()
        {
            for (Subsystem* s : m_sorted)
            {
                s->Init();
            }
            for (Subsystem* s : m_sorted)
            {
                s->Ready();
            }
            m_running = true;
        }

        // Engine time scale (ez-style): 1 = realtime, 0 = paused, 0.5 = slow-mo. The host
        // scales the dt feeding FixedUpdate accumulation and the Update/PostUpdate phases;
        // frame-rate-tied work (UI, app hooks) keeps the raw dt. Gameplay code and the
        // input runtime's per-action timeScale flag read it from here.
        void SetTimeScale(core::f32 scale) noexcept { m_timeScale = scale < 0.0f ? 0.0f : scale; }
        [[nodiscard]] core::f32 TimeScale() const noexcept { return m_timeScale; }

        // The app's configured fixed step - plain CONFIG (a float), not an execution lane.
        // The scene bridge reads it to seed per-scene fixed steppers; fixed-rate interpolation
        // is PER SCENE (Scene::FixedAlpha). There is no context-level fixed lane (no FixedUpdate
        // fan, no context-level FixedAlpha): no subsystem overrides FixedUpdate and no reader
        // consumes a context alpha.
        void SetFixedTimeStep(core::f32 step) noexcept { m_fixedStep = step; }
        [[nodiscard]] core::f32 FixedTimeStep() const noexcept { return m_fixedStep; }

        void BeginFrame(core::f32 dt)
        {
            for (Subsystem* s : m_sorted)
            {
                s->BeginFrame(dt);
            }
        }
        void Update(core::f32 dt)
        {
            for (Subsystem* s : m_sorted)
            {
                s->Update(dt);
            }
        }
        void PostUpdate(core::f32 dt)
        {
            for (Subsystem* s : m_sorted)
            {
                s->PostUpdate(dt);
            }
        }
        void EndFrame()
        {
            for (Subsystem* s : m_sorted)
            {
                s->EndFrame();
            }
        }

        // PrepareShutdown then Shutdown, in reverse UpdateOrder.
        void Shutdown()
        {
            m_running = false;
            for (core::usize i = m_sorted.Size(); i-- > 0;)
            {
                m_sorted[i]->PrepareShutdown();
            }
            for (core::usize i = m_sorted.Size(); i-- > 0;)
            {
                m_sorted[i]->Shutdown();
            }
        }

        // Shuts down (if running) and destroys all subsystems. Idempotent.
        void Dispose()
        {
            if (m_disposed)
            {
                return;
            }
            m_disposed = true;
            if (m_running)
            {
                Shutdown();
            }
            m_sorted.Clear();
            m_owned.Clear(); // UniquePtr<Subsystem> destroys each via the allocator
        }

    private:
        // Registers an already-constructed subsystem: index by type, insert into
        // the sorted phase list, wire the context, and bring it up if running.
        void RegisterInternal(const core::TypeInfo* type, Subsystem* subsystem)
        {
            m_byType.InsertOrAssign(type->id, subsystem);
            InsertSorted(subsystem);
            subsystem->OnRegister(this);
            if (m_running)
            {
                subsystem->Init();
                subsystem->Ready();
            }
        }

        // Detaches a subsystem by type: shut it down (if running), unregister,
        // drop from the lookup/phase lists, and destroy it if Context-owned.
        void RemoveByType(const core::TypeInfo* type)
        {
            Subsystem* const* found = m_byType.Find(type->id);
            if (found == nullptr)
            {
                return;
            }
            Subsystem* subsystem = *found;

            if (m_running)
            {
                subsystem->PrepareShutdown();
            }
            subsystem->Shutdown();
            subsystem->OnUnregister();

            for (core::usize i = 0; i < m_sorted.Size(); ++i)
            {
                if (m_sorted[i] == subsystem)
                {
                    m_sorted.RemoveAt(i);
                    break;
                }
            }
            m_byType.Remove(type->id);

            // If the Context owns it, destroying the UniquePtr frees the object.
            for (core::usize i = 0; i < m_owned.Size(); ++i)
            {
                if (m_owned[i].Get() == subsystem)
                {
                    m_owned.RemoveAt(i);
                    break;
                }
            }
        }

        // Insertion sort into m_sorted, ascending by UpdateOrder (stable).
        void InsertSorted(Subsystem* subsystem)
        {
            m_sorted.PushBack(subsystem);
            core::usize i = m_sorted.Size() - 1;
            while (i > 0 && m_sorted[i - 1]->UpdateOrder() > subsystem->UpdateOrder())
            {
                Subsystem* prev = m_sorted[i - 1];
                m_sorted[i - 1] = m_sorted[i];
                m_sorted[i] = prev;
                --i;
            }
        }

        core::IAllocator* m_allocator;
        // Keyed by TypeId, not TypeInfo*: a plugin library resolving GetSubsystem<T>()
        // holds its own copy of TypeOf<T>'s static, so addresses diverge across shared
        // library boundaries while ids do not (shared-libraries.md identity rule).
        core::HashMap<core::TypeId, Subsystem*> m_byType;
        core::Array<Subsystem*> m_sorted;                // non-owning, UpdateOrder-sorted
        core::Array<core::UniquePtr<Subsystem>> m_owned; // ownership
        bool m_running = false;
        core::f32 m_fixedStep = 1.0f / 60.0f;
        core::f32 m_timeScale = 1.0f;
        bool m_disposed = false;
    };
}
