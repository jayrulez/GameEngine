// Draconic GUI - module implementation unit for draconic.gui.
//
// Holds Node method bodies that call into the EventDispatcher. They cannot be inline in
// the :node interface partition without making it import :event_dispatcher, which imports
// :node back (a module-partition cycle). An implementation unit sees the whole module (it
// implicitly imports the primary interface) and is outside the interface dependency graph
// - the same pattern as draconic.ui's UIClusterImpl.cpp.

module;
#include "Draconic.Foundation/Prelude.h"

module draconic.gui;

namespace draconic::gui
{
    void Node::RequestFocus()
    {
        if (EventDispatcher* dispatcher = GetEventDispatcher())
            dispatcher->SetFocusNode(this);
    }

    void Node::ReleaseFocus()
    {
        if (EventDispatcher* dispatcher = GetEventDispatcher())
            if (dispatcher->GetFocusNode() == this)
                dispatcher->SetFocusNode(nullptr);
    }
}
