// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// foundation.run - implementation: a run's sessions and services, and the registry of runs.

module;
#include "Core/Prelude.h"

module foundation.run;

import foundation.core;
import foundation.messaging;

using namespace foundation::core;

namespace foundation::run
{
    namespace
    {
        // Never reused in the process. The game thread makes runs; one library holds the counter.
        RunId g_lastRunId = 0;
    }

    RunBase::RunBase(IAllocator& allocator, StringView name)
        : m_allocator(&allocator), m_id(++g_lastRunId), m_name(name, allocator), m_services(allocator)
    {
    }

    RunBase::~RunBase() { Dispose(); }

    void* RunBase::FindService(TypeId type) const noexcept
    {
        for (const Entry& entry : m_services)
            if (entry.type == type)
                return entry.object;
        return nullptr;
    }

    void RunBase::Insert(Entry entry)
    {
        RemoveType(entry.type); // one per type: the new one replaces
        m_services.PushBack(entry);
    }

    void RunBase::Destroy(Entry& entry)
    {
        if (entry.destroy != nullptr)
            entry.destroy(*m_allocator, entry.object);
        entry.object = nullptr;
    }

    void RunBase::RemoveType(TypeId type)
    {
        for (usize i = 0; i < m_services.Size(); ++i)
            if (m_services[i].type == type)
            {
                Destroy(m_services[i]);
                m_services.RemoveAt(i);
                return;
            }
    }

    void RunBase::Start()
    {
        if (m_state != RunState::Idle)
            return;
        m_state = RunState::Running;
        if (m_listener != nullptr)
            m_listener->OnRunStarted(*this);
    }

    void RunBase::SetPaused(bool paused)
    {
        const bool now = m_state == RunState::Paused;
        if ((m_state != RunState::Running && m_state != RunState::Paused) || now == paused)
            return;
        m_state = paused ? RunState::Paused : RunState::Running;
        for (usize i = m_services.Size(); i > 0; --i)
            if (IRunService* hooks = m_services[i - 1].hooks)
                hooks->OnRunPaused(*this, paused);
        if (m_listener != nullptr)
            m_listener->OnRunPaused(*this, paused);
    }

    void RunBase::Stop()
    {
        if (m_state != RunState::Running && m_state != RunState::Paused)
            return;
        m_state = RunState::Stopping;
        for (usize i = m_services.Size(); i > 0; --i)
            if (IRunService* hooks = m_services[i - 1].hooks)
                hooks->OnRunStopping(*this);
        if (m_listener != nullptr)
            m_listener->OnRunStopping(*this);
        // This session's services go, last added first.
        for (usize i = m_services.Size(); i > 0; --i)
            if (m_services[i - 1].session)
            {
                Destroy(m_services[i - 1]);
                m_services.RemoveAt(i - 1);
            }
        m_state = RunState::Idle;
    }

    void RunBase::Dispose()
    {
        if (m_state == RunState::Disposed)
            return;
        Stop();
        for (usize i = m_services.Size(); i > 0; --i)
            Destroy(m_services[i - 1]);
        m_services.Clear();
        m_state = RunState::Disposed;
        if (IRunListener* listener = m_listener)
        {
            m_listener = nullptr;
            listener->OnRunDestroyed(*this);
        }
    }

    // ---- RunRegistry ---------------------------------------------------------------------

    RunRegistry::~RunRegistry()
    {
        // Runs still here outlive the registry: they stop telling it anything.
        for (RunBase* run : m_runs)
            run->SetLifecycleListener(nullptr);
    }

    void RunRegistry::Add(RunBase& run)
    {
        for (RunBase* existing : m_runs)
            if (existing == &run)
                return;
        m_runs.PushBack(&run);
        run.SetLifecycleListener(this);
        Announce([&](IRunListener& l) { l.OnRunCreated(run); });
        if (run.State() == RunState::Running || run.State() == RunState::Paused)
            Announce([&](IRunListener& l) { l.OnRunStarted(run); });
    }

    void RunRegistry::ForEachRun(const Function<void(IRun&)>& visit)
    {
        Array<RunBase*> runs = m_runs; // a visit may dispose one
        for (RunBase* run : runs)
            visit(*run);
    }

    IRun* RunRegistry::Find(RunId id) noexcept
    {
        for (RunBase* run : m_runs)
            if (run->Id() == id)
                return run;
        return nullptr;
    }

    void RunRegistry::SetFocused(IRun* run)
    {
        if (run == m_focused)
            return;
        m_focused = run;
        Announce([&](IRunListener& l) { l.OnFocusChanged(run); });
    }

    void RunRegistry::Subscribe(IRunListener& listener)
    {
        for (IRunListener* existing : m_listeners)
            if (existing == &listener)
                return;
        m_listeners.PushBack(&listener);
    }

    void RunRegistry::Unsubscribe(IRunListener& listener)
    {
        for (usize i = 0; i < m_listeners.Size(); ++i)
            if (m_listeners[i] == &listener)
            {
                m_listeners.RemoveAt(i);
                return;
            }
    }

    void RunRegistry::OnRunStarted(IRun& run)
    {
        Announce([&](IRunListener& l) { l.OnRunStarted(run); });
    }

    void RunRegistry::OnRunPaused(IRun& run, bool paused)
    {
        Announce([&](IRunListener& l) { l.OnRunPaused(run, paused); });
    }

    void RunRegistry::OnRunStopping(IRun& run)
    {
        Announce([&](IRunListener& l) { l.OnRunStopping(run); });
    }

    void RunRegistry::OnRunDestroyed(IRun& run)
    {
        if (m_focused == &run)
            SetFocused(nullptr);
        Announce([&](IRunListener& l) { l.OnRunDestroyed(run); });
        for (usize i = 0; i < m_runs.Size(); ++i)
            if (m_runs[i] == &run)
            {
                m_runs.RemoveAt(i);
                return;
            }
    }
}
