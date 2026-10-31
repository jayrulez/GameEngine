// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - :mutation_queue partition
//
// MutationQueue: deferred operations drained at a safe sync point (in GUIContext::Update, after
// the actions and the bindings). Tree edits made mid-traversal (Node::Close among them) are
// applied this way, so the tree is never mutated while it is being walked.

module;
#include "Core/Prelude.h"

export module foundation.gui:mutation_queue;

import foundation.core;

using namespace foundation::core;
namespace core = foundation::core;

export namespace foundation::gui
{
    class MutationQueue
    {
    public:
        void Enqueue(core::Function<void()> op) { m_ops.PushBack(core::Move(op)); }

        [[nodiscard]] bool IsEmpty() const noexcept { return m_ops.Size() == 0; }

        // Run and clear all queued ops. Ops enqueued during draining run on the next drain.
        void Drain()
        {
            Array<core::Function<void()>> batch = core::Move(m_ops);
            m_ops = Array<core::Function<void()>>{};
            for (core::Function<void()>& op : batch)
                op();
        }

    private:
        Array<core::Function<void()>> m_ops;
    };
}
