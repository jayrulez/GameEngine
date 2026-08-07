// Draconic GUI - :mutation_queue partition
//
// MutationQueue: deferred operations drained at a safe sync point (end of the SceneNode
// update). This is how tree edits made mid-traversal (notably Node::Close) are applied
// without mutating the tree while it is being walked - eepp's SceneNode close-queue role,
// generalized to arbitrary deferred ops (same shape as draconic.ui's MutationQueue).

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.gui:mutation_queue;

import draconic.foundation; // Function, Array, Move

using namespace draconic::foundation;
namespace foundation = draconic::foundation;

export namespace draconic::gui
{
    class MutationQueue
    {
    public:
        void Enqueue(foundation::Function<void()> op) { m_ops.PushBack(foundation::Move(op)); }

        [[nodiscard]] bool IsEmpty() const noexcept { return m_ops.Size() == 0; }

        // Run and clear all queued ops. Ops enqueued during draining run on the next drain.
        void Drain()
        {
            Array<foundation::Function<void()>> batch = foundation::Move(m_ops);
            m_ops = Array<foundation::Function<void()>>{};
            for (foundation::Function<void()>& op : batch)
                op();
        }

    private:
        Array<foundation::Function<void()>> m_ops;
    };
}
