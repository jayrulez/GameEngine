// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI - :event partition
//
// Event<void(Args...)>: a minimal multicast delegate over core::Function, the port's
// equivalent of Beef's `Event<delegate void(...)>` used pervasively across Sedulous.UI
// (Property.Changed, Button.Clicked, ...). Add handlers; invoke via operator() / Invoke.

module;
#include "Core/Prelude.h"

export module foundation.ui:event;

import foundation.core;

using namespace foundation::core;

export namespace foundation::ui
{
    template <typename Signature>
    class Event; // primary template undefined; only void(Args...) is valid

    template <typename... Args>
    class Event<void(Args...)>
    {
    public:
        using Handler = Function<void(Args...)>;
        /// What Add hands back: the way to remove that one handler again. 0 is never issued.
        using Token = u64;

        /// Register a handler (move-only, like the underlying Function). The token removes it;
        /// a subscriber that can die before the event (a surface generated from a registry)
        /// removes itself in its destructor, or the event would call into freed memory.
        Token Add(Handler handler)
        {
            const Token token = ++m_lastToken;
            m_handlers.PushBack(Entry{token, Move(handler)});
            return token;
        }
        /// Remove the handler `token` names; false when none does (already removed, or 0).
        bool Remove(Token token) noexcept
        {
            for (usize i = 0; i < m_handlers.Size(); ++i)
            {
                if (m_handlers[i].token == token)
                {
                    m_handlers.RemoveAt(i);
                    return true;
                }
            }
            return false;
        }
        /// Remove all handlers.
        void Clear() noexcept { m_handlers.Clear(); }
        [[nodiscard]] usize Count() const noexcept { return m_handlers.Size(); }

        /// Invoke every handler in registration order.
        void Invoke(Args... args) const
        {
            for (const Entry& entry : m_handlers)
            {
                entry.handler(args...);
            }
        }
        void operator()(Args... args) const { Invoke(args...); }

    private:
        struct Entry
        {
            Token token;
            Handler handler;
        };
        Array<Entry> m_handlers;
        Token m_lastToken = 0;
    };
}
