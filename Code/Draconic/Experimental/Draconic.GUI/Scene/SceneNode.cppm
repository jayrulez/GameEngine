// Draconic GUI - :scene_node partition
//
// SceneNode: the root of a widget tree AND its coordinator (eepp Scene::SceneNode). A Node
// subclass that sits at the top; it owns the ActionManager and the MutationQueue, and its
// Update ticks running actions then drains deferred tree edits. Drawing is inherited from
// Node::Draw (walks the subtree). UISceneNode (CSS/stylesheet root) lands with the UI phase.
//
// Nodes reach the coordinator through Node's virtual GetActionManager()/GetMutationQueue(),
// which walk up the parent chain; SceneNode overrides them to return its own.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.gui:scene_node;

import draconic.foundation; // Duration
import :node;
import :action_manager;
import :mutation_queue;
import :event_dispatcher;

using namespace draconic::foundation;
namespace foundation = draconic::foundation;

export namespace draconic::gui
{
    class SceneNode : public Node
    {
        DRACONIC_OBJECT(SceneNode, Node)
    public:
        SceneNode() = default;

        // Advance the tree by `elapsed`: tick actions, run the per-frame hook, then drain
        // deferred tree edits (e.g. queued Node::Close removals).
        void Update(foundation::Duration elapsed)
        {
            m_actionManager.Update(elapsed);
            OnUpdate(elapsed);
            m_mutationQueue.Drain();
        }

        [[nodiscard]] ActionManager* GetActionManager() override { return &m_actionManager; }
        [[nodiscard]] MutationQueue* GetMutationQueue() override { return &m_mutationQueue; }
        [[nodiscard]] EventDispatcher* GetEventDispatcher() override { return &m_eventDispatcher; }

        // Per-frame hook for subclasses (layout, timers, ...).
        virtual void OnUpdate(foundation::Duration elapsed) { (void)elapsed; }

    private:
        ActionManager m_actionManager;
        MutationQueue m_mutationQueue;
        EventDispatcher m_eventDispatcher{this}; // root = this SceneNode
    };

    DRACONIC_DEFINE_OBJECT(SceneNode, "draconic::gui")
}
