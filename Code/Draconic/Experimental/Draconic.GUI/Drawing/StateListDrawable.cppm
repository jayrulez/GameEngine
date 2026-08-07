// Draconic GUI - :state_list_drawable partition
//
// StateListDrawable: maps a ControlState to a child Drawable and dispatches Draw to it,
// falling back to Normal when a state has no entry. Derived from eepp's StateListDrawable
// (used by UISkinState); the eepp bitmask best-match is simplified to keyed lookup +
// Normal fallback for now (grows with the skin phase).

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.gui:state_list_drawable;

import draconic.foundation; // RefPtr, HashMap, Move
import :rect;
import :draw_context;
import :drawable;
import :control_state;

using namespace draconic::foundation;

export namespace draconic::gui
{
    class StateListDrawable : public Drawable
    {
        DRACONIC_OBJECT(StateListDrawable, Drawable)
    public:
        [[nodiscard]] bool IsStateful() const override { return true; }

        void Set(ControlState state, RefPtr<Drawable> drawable)
        {
            m_states.InsertOrAssign(static_cast<u32>(state), drawable);
        }

        // The drawable for a state, or the Normal fallback, or nullptr.
        [[nodiscard]] Drawable* Get(ControlState state) const
        {
            if (const RefPtr<Drawable>* found = m_states.Find(static_cast<u32>(state)))
                return found->Get();
            if (const RefPtr<Drawable>* normal =
                    m_states.Find(static_cast<u32>(ControlState::Normal)))
                return normal->Get();
            return nullptr;
        }

        void Draw(DrawContext& ctx, const Rect& dest) override
        {
            if (Drawable* d = Get(ControlState::Normal))
                d->Draw(ctx, dest);
        }

        void Draw(DrawContext& ctx, const Rect& dest, ControlState state) override
        {
            if (Drawable* d = Get(state))
                d->Draw(ctx, dest, state);
        }

    private:
        HashMap<u32, RefPtr<Drawable>> m_states;
    };

    DRACONIC_DEFINE_OBJECT(StateListDrawable, "draconic::gui")
}
