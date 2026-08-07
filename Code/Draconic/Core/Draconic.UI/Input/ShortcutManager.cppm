// Draconic UI - :shortcut_manager partition
//
// Manages global and scoped keyboard shortcuts, owned by UIContext. Dispatch order: focused-view key
// handlers -> ShortcutManager -> IAcceleratorHandler. Ported from Sedulous.UI/src/Input/ShortcutManager.bf.
// View/UIContext-touching bodies (TryDispatch/IsInScope) live in the module impl unit.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.ui:shortcut_manager;

import draconic.foundation; // Array, RefPtr, Function
import :input_enums;  // KeyCode, KeyModifiers
import :shortcut;

using namespace draconic::foundation;

export namespace draconic::ui
{
    class View;
    class UIContext;

    class ShortcutManager
    {
    public:
        explicit ShortcutManager(UIContext* context) : m_context(context) {}

        /// Register a shortcut (takes ownership).
        void Add(RefPtr<Shortcut> shortcut) { m_shortcuts.PushBack(Move(shortcut)); }

        /// Register a global shortcut (fires regardless of focus). Returns a borrowed pointer.
        Shortcut* AddGlobal(KeyCode key, KeyModifiers modifiers, Function<void()> action)
        {
            RefPtr<Shortcut> s = MakeRef<Shortcut>(DefaultAllocator(), key, modifiers, Move(action),
                                                   static_cast<View*>(nullptr));
            Shortcut* ptr = s.Get();
            m_shortcuts.PushBack(Move(s));
            return ptr;
        }

        /// Register a scoped shortcut (fires only when `scope` or a descendant has focus).
        Shortcut* AddScoped(KeyCode key, KeyModifiers modifiers, Function<void()> action,
                            View* scope)
        {
            RefPtr<Shortcut> s =
                MakeRef<Shortcut>(DefaultAllocator(), key, modifiers, Move(action), scope);
            Shortcut* ptr = s.Get();
            m_shortcuts.PushBack(Move(s));
            return ptr;
        }

        /// Remove a shortcut.
        void Remove(Shortcut* shortcut)
        {
            for (usize i = 0; i < m_shortcuts.Size(); ++i)
            {
                if (m_shortcuts[i].Get() == shortcut)
                {
                    m_shortcuts.RemoveAt(i);
                    return;
                }
            }
        }

        /// Remove all shortcuts scoped to a view (called when the view is deleted).
        void RemoveScopedTo(View* view)
        {
            for (usize i = m_shortcuts.Size(); i-- > 0;)
            {
                if (m_shortcuts[i]->Scope == view)
                {
                    m_shortcuts.RemoveAtSwap(i);
                }
            }
        }

        [[nodiscard]] usize Count() const noexcept { return m_shortcuts.Size(); }

        /// Try to dispatch a key event (scoped first, then global). Defined in the impl unit.
        bool TryDispatch(KeyCode key, KeyModifiers modifiers);

    private:
        static bool IsInScope(View* view, View* scope); // defined in impl unit

        UIContext* m_context = nullptr;
        Array<RefPtr<Shortcut>> m_shortcuts;
    };
}
