// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Foundation::Run - the `foundation.run` module (Documentation/Specs/run-context.md).
///
/// A run is one running game: a player's game, each editor Game tab's game, an in-editor
/// dedicated server. `IRun` is how anything reaches one (its id, name, state, event bus and the
/// services it owns); `RunBase` implements it once, for a run to derive from (the engine's
/// GameInstance does). `IRunManager` lists the live runs, holds the focused one and announces each
/// run's lifecycle to its `IRunListener`s; `RunRegistry` implements it, owned by whatever owns the
/// runs (the engine's DefaultApplication), and the subsystem manager holds it for every subsystem.
///
/// A run is not a scheduler: it holds services and runs their lifecycle hooks; its owner ticks
/// what it ticks. Plays are sessions inside a run: Idle -> Running <-> Paused -> Stopping -> Idle,
/// as many times as the host plays, then Disposed.
///
/// Layering: Core and Messaging only. The runtime and the scenes depend on this module; it never
/// depends on them.

module;
#include "Core/Prelude.h"
#include <type_traits>

export module foundation.run;

import foundation.core;
import foundation.messaging;

using namespace foundation::core;

export namespace foundation::run
{
    namespace messaging = foundation::messaging;

    /// A run's id: never reused in the process; 0 is no run.
    using RunId = u64;

    enum class RunState : u8
    {
        Idle,     ///< made, or between sessions
        Running,  ///< a session is playing
        Paused,   ///< the host paused it (the toolbar, a debugger break): not the game's own pause
        Stopping, ///< a session is ending: its services' and listeners' stop hooks are running
        Disposed, ///< gone; nothing reaches it after its destruction is announced
    };

    class IRun;

    /// What a run's lifecycle is told to: a subsystem keeping per-run state (through the run
    /// manager), and the run manager itself (from each run).
    class IRunListener
    {
    public:
        virtual ~IRunListener() = default;
        virtual void OnRunCreated(IRun&) {}
        virtual void OnRunStarted(IRun&) {}
        virtual void OnRunPaused(IRun&, bool /*paused*/) {}
        virtual void OnRunStopping(IRun&) {}
        virtual void OnRunDestroyed(IRun&) {}
        virtual void OnFocusChanged(IRun* /*focused*/) {}
    };

    /// A service a run owns may implement this to hear its run pause and stop (in reverse order
    /// of registration).
    class IRunService
    {
    public:
        virtual ~IRunService() = default;
        virtual void OnRunPaused(IRun&, bool /*paused*/) {}
        virtual void OnRunStopping(IRun&) {}
    };

    /// A running game, however it is implemented.
    class IRun
    {
    public:
        virtual ~IRun() = default;
        [[nodiscard]] virtual RunId Id() const noexcept = 0;
        [[nodiscard]] virtual StringView Name() const noexcept = 0;
        [[nodiscard]] virtual RunState State() const noexcept = 0;
        /// The run's event bus: its scenes' and its scripts' (`run.events()`).
        [[nodiscard]] virtual messaging::EventBus& Events() noexcept = 0;

        /// The service of type `T` the run holds, or null.
        template <typename T>
        [[nodiscard]] T* Get() const noexcept
        {
            return static_cast<T*>(FindService(TypeOf<T>().id));
        }
        template <typename T>
        [[nodiscard]] bool Has() const noexcept
        {
            return FindService(TypeOf<T>().id) != nullptr;
        }

    protected:
        [[nodiscard]] virtual void* FindService(TypeId type) const noexcept = 0;
    };

    /// The live runs, the focused one, and every run's lifecycle announced.
    class IRunManager
    {
    public:
        virtual ~IRunManager() = default;
        virtual void ForEachRun(const Function<void(IRun&)>& visit) = 0;
        [[nodiscard]] virtual IRun* Find(RunId id) noexcept = 0;
        /// The run the user is playing; null is no preference.
        [[nodiscard]] virtual IRun* Focused() noexcept = 0;
        virtual void SetFocused(IRun* run) = 0;
        /// A listener subscribing after runs exist finds them with ForEachRun.
        virtual void Subscribe(IRunListener& listener) = 0;
        virtual void Unsubscribe(IRunListener& listener) = 0;
    };

    /// IRun implemented once: the id, name, state machine, event bus and service table. A run
    /// derives from it. One that registers its own members as services calls Dispose() in its own
    /// destructor, so their stop hooks run while they still exist.
    class RunBase : public IRun
    {
    public:
        RunBase(IAllocator& allocator, StringView name);
        ~RunBase() override;
        RunBase(const RunBase&) = delete;
        RunBase& operator=(const RunBase&) = delete;

        [[nodiscard]] RunId Id() const noexcept override { return m_id; }
        [[nodiscard]] StringView Name() const noexcept override { return m_name.AsView(); }
        [[nodiscard]] RunState State() const noexcept override { return m_state; }
        [[nodiscard]] messaging::EventBus& Events() noexcept override { return m_events; }
        void SetName(StringView name) { m_name = String(name, *m_allocator); }

        /// Constructs a service the run owns, for the run's life. A type held already is
        /// replaced (an owned predecessor destroyed).
        template <typename T, typename... Args>
        T& Add(Args&&... args)
        {
            return AddOwned<T>(false, Forward<Args>(args)...);
        }
        /// As Add, for this session only: removed (and destroyed) at Stop.
        template <typename T, typename... Args>
        T& AddForSession(Args&&... args)
        {
            return AddOwned<T>(true, Forward<Args>(args)...);
        }
        /// Holds a service someone else owns (never destroyed by the run).
        template <typename T>
        T& Register(T& service)
        {
            Insert(Entry{TypeOf<T>().id, &service, HooksOf(&service), nullptr, false});
            return service;
        }
        template <typename T>
        void Remove()
        {
            RemoveType(TypeOf<T>().id);
        }

        /// A session starts (Idle -> Running). Nothing when one is under way.
        void Start();
        /// The host's pause (Running <-> Paused); repeating the same value does nothing.
        void SetPaused(bool paused);
        /// The session ends: services' stop hooks (reverse order), then the listener's, then the
        /// per-session services go. Back to Idle. Nothing when no session is under way.
        void Stop();
        /// Stops a session under way, destroys the owned services, announces the destruction.
        /// Idempotent; the destructor calls it.
        void Dispose();

        /// Who hears this run's transitions (its manager); null for none.
        void SetLifecycleListener(IRunListener* listener) noexcept { m_listener = listener; }

    protected:
        [[nodiscard]] void* FindService(TypeId type) const noexcept override;

    private:
        struct Entry
        {
            TypeId type = 0;
            void* object = nullptr;
            IRunService* hooks = nullptr;
            void (*destroy)(IAllocator&, void*) = nullptr; // null: borrowed
            bool session = false;
        };

        template <typename T>
        static IRunService* HooksOf(T* service) noexcept
        {
            if constexpr (std::is_base_of_v<IRunService, T>)
                return static_cast<IRunService*>(service);
            else
                return nullptr;
        }

        template <typename T, typename... Args>
        T& AddOwned(bool session, Args&&... args)
        {
            T* service = m_allocator->New<T>(Forward<Args>(args)...);
            Insert(Entry{TypeOf<T>().id, service, HooksOf(service),
                         [](IAllocator& allocator, void* object) { allocator.Delete(static_cast<T*>(object)); }, session});
            return *service;
        }

        void Insert(Entry entry);
        void RemoveType(TypeId type);
        void Destroy(Entry& entry);

        IAllocator* m_allocator;
        RunId m_id;
        String m_name;
        RunState m_state = RunState::Idle;
        messaging::EventBus m_events;
        Array<Entry> m_services; // registration order
        IRunListener* m_listener = nullptr;
    };

    /// IRunManager implemented once: the runs added to it, the focus, the subscribers. Each run
    /// added announces its transitions here, and they go to every subscriber, in subscription
    /// order; a run's destruction removes it (clearing the focus when it held it).
    class RunRegistry final : public IRunManager, private IRunListener
    {
    public:
        explicit RunRegistry(IAllocator& allocator) : m_runs(allocator), m_listeners(allocator) {}
        ~RunRegistry() override;
        RunRegistry(const RunRegistry&) = delete;
        RunRegistry& operator=(const RunRegistry&) = delete;

        /// `run` joins the live runs (announced as created); it leaves when it is disposed.
        void Add(RunBase& run);
        [[nodiscard]] usize Count() const noexcept { return m_runs.Size(); }

        void ForEachRun(const Function<void(IRun&)>& visit) override;
        [[nodiscard]] IRun* Find(RunId id) noexcept override;
        [[nodiscard]] IRun* Focused() noexcept override { return m_focused; }
        void SetFocused(IRun* run) override;
        void Subscribe(IRunListener& listener) override;
        void Unsubscribe(IRunListener& listener) override;

    private:
        void OnRunStarted(IRun& run) override;
        void OnRunPaused(IRun& run, bool paused) override;
        void OnRunStopping(IRun& run) override;
        void OnRunDestroyed(IRun& run) override;

        // The listeners as they are now: one may unsubscribe while it is being told.
        template <typename Fn>
        void Announce(Fn&& tell)
        {
            Array<IRunListener*> listeners = m_listeners;
            for (IRunListener* listener : listeners)
                tell(*listener);
        }

        Array<RunBase*> m_runs;
        Array<IRunListener*> m_listeners;
        IRun* m_focused = nullptr;
    };
}
