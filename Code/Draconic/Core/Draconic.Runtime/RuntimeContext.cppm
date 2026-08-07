// Draconic Runtime - :context partition
//
// Context: owns the engine's subsystems, looks them up by type, and drives
// their lifecycle and per-frame phases in UpdateOrder. Type identity uses the
// reflection TypeOf<T>() (Draconic has -fno-rtti, so no std::type_index).

module;
#include "Draconic.Foundation/Prelude.h"
#include <type_traits>

export module draconic.runtime:context;

import draconic.foundation;
import :subsystem;

namespace foundation = draconic::foundation;

export namespace draconic::runtime
{
    class Context
    {
    public:
        explicit Context(foundation::IAllocator& allocator = foundation::DefaultAllocator()) noexcept
            : m_allocator(&allocator)
        {
        }

        ~Context() { Dispose(); }

        Context(const Context&) = delete;
        Context& operator=(const Context&) = delete;

        [[nodiscard]] bool IsRunning() const noexcept { return m_running; }

        // Constructs and registers a subsystem of type T (one per type). Returns
        // a borrowed pointer; the Context owns it. If the Context is already
        // running, the subsystem is brought up (Init + Ready) immediately.
        template <typename T, typename... Args>
        T* AddSubsystem(Args&&... args)
        {
            static_assert(std::is_base_of_v<Subsystem, T>, "T must derive from Subsystem");
            T* subsystem = m_allocator->New<T>(foundation::Forward<Args>(args)...);
            m_owned.PushBack(
                foundation::UniquePtr<Subsystem>(static_cast<Subsystem*>(subsystem), *m_allocator));
            RegisterInternal(&foundation::TypeOf<T>(), static_cast<Subsystem*>(subsystem));
            return subsystem;
        }

        // Registers a subsystem the caller owns (e.g. a plugin owns its own
        // subsystems). The Context tracks it for lookup and per-frame phases but
        // never destroys it. Brings it up immediately if the Context is running.
        template <typename T>
        T* RegisterSubsystem(T* subsystem)
        {
            static_assert(std::is_base_of_v<Subsystem, T>, "T must derive from Subsystem");
            RegisterInternal(&foundation::TypeOf<T>(), static_cast<Subsystem*>(subsystem));
            return subsystem;
        }

        // Removes the subsystem of type T (shutting it down first if running) and,
        // if the Context owns it, destroys it. No-op if not registered.
        template <typename T>
        void RemoveSubsystem()
        {
            RemoveByType(&foundation::TypeOf<T>());
        }

        template <typename T>
        [[nodiscard]] T* GetSubsystem() noexcept
        {
            Subsystem* const* found = m_byType.Find(&foundation::TypeOf<T>());
            return (found != nullptr) ? static_cast<T*>(*found) : nullptr;
        }

        template <typename T>
        [[nodiscard]] bool HasSubsystem() const noexcept
        {
            return m_byType.Contains(&foundation::TypeOf<T>());
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
        void SetTimeScale(foundation::f32 scale) noexcept { m_timeScale = scale < 0.0f ? 0.0f : scale; }
        [[nodiscard]] foundation::f32 TimeScale() const noexcept { return m_timeScale; }

        // Fixed-lane timing, published by the host each frame AFTER the fixed steps ran:
        // subsystems interpolating fixed-rate state (physics poses) blend with FixedAlpha().
        void SetFixedTiming(foundation::f32 step, foundation::f32 alpha) noexcept
        {
            m_fixedStep = step;
            m_fixedAlpha = alpha;
        }
        [[nodiscard]] foundation::f32 FixedTimeStep() const noexcept { return m_fixedStep; }
        [[nodiscard]] foundation::f32 FixedAlpha() const noexcept { return m_fixedAlpha; }

        void BeginFrame(foundation::f32 dt)
        {
            for (Subsystem* s : m_sorted)
            {
                s->BeginFrame(dt);
            }
        }
        void FixedUpdate(foundation::f32 dt)
        {
            for (Subsystem* s : m_sorted)
            {
                s->FixedUpdate(dt);
            }
        }
        void Update(foundation::f32 dt)
        {
            for (Subsystem* s : m_sorted)
            {
                s->Update(dt);
            }
        }
        void PostUpdate(foundation::f32 dt)
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
            for (foundation::usize i = m_sorted.Size(); i-- > 0;)
            {
                m_sorted[i]->PrepareShutdown();
            }
            for (foundation::usize i = m_sorted.Size(); i-- > 0;)
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
        void RegisterInternal(const foundation::TypeInfo* type, Subsystem* subsystem)
        {
            m_byType.InsertOrAssign(type, subsystem);
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
        void RemoveByType(const foundation::TypeInfo* type)
        {
            Subsystem* const* found = m_byType.Find(type);
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

            for (foundation::usize i = 0; i < m_sorted.Size(); ++i)
            {
                if (m_sorted[i] == subsystem)
                {
                    m_sorted.RemoveAt(i);
                    break;
                }
            }
            m_byType.Remove(type);

            // If the Context owns it, destroying the UniquePtr frees the object.
            for (foundation::usize i = 0; i < m_owned.Size(); ++i)
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
            foundation::usize i = m_sorted.Size() - 1;
            while (i > 0 && m_sorted[i - 1]->UpdateOrder() > subsystem->UpdateOrder())
            {
                Subsystem* prev = m_sorted[i - 1];
                m_sorted[i - 1] = m_sorted[i];
                m_sorted[i] = prev;
                --i;
            }
        }

        foundation::IAllocator* m_allocator;
        foundation::HashMap<const foundation::TypeInfo*, Subsystem*> m_byType;
        foundation::Array<Subsystem*> m_sorted;                // non-owning, UpdateOrder-sorted
        foundation::Array<foundation::UniquePtr<Subsystem>> m_owned; // ownership
        bool m_running = false;
        foundation::f32 m_fixedStep = 1.0f / 60.0f;
        foundation::f32 m_fixedAlpha = 0.0f;
        foundation::f32 m_timeScale = 1.0f;
        bool m_disposed = false;
    };
}
