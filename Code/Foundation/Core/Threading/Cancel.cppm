// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Core - :cancel partition
//
// Cooperative cancelling (Specs/task-layer.md D1). Whoever may cancel holds a CancelSource; the
// work it started holds a CancelToken, a cheap copy that asks IsCancelled() at its checks and
// returns ErrorCode::Cancelled when told. Work that runs where its canceller cannot reach (a cook in
// the pipeline, started by an editor window) takes a token, so the canceller needs no knowledge of
// the work and the work none of the canceller.
//
// The two share one small reference-counted state from the source's allocator, so a token stays
// valid after its source is gone. Not std::stop_token: its state comes from global new, past the
// owner's allocator.

module;
#include "Core/Prelude.h"
#include <atomic>

export module foundation.core:cancel;

import :base;
import :allocator;
import :ref_counted;

export namespace foundation::core
{
    namespace detail
    {
        /// What a source and its tokens share: the one flag.
        class CancelState final : public RefCounted
        {
        public:
            std::atomic<bool> cancelled{false};
        };
    }

    /// Handed to work so it can be told to stop. A default token never cancels, so code with
    /// nothing to cancel passes CancelToken{}.
    class CancelToken
    {
    public:
        CancelToken() = default;

        /// Whether the source has cancelled: the work stops at its next check.
        [[nodiscard]] bool IsCancelled() const noexcept { return m_state && m_state->cancelled.load(std::memory_order_acquire); }

        /// Whether a source stands behind it (false for a default token, which never cancels).
        [[nodiscard]] bool CanBeCancelled() const noexcept { return static_cast<bool>(m_state); }

    private:
        friend class CancelSource;
        explicit CancelToken(RefPtr<detail::CancelState> state) noexcept : m_state(static_cast<RefPtr<detail::CancelState>&&>(state)) {}

        RefPtr<detail::CancelState> m_state;
    };

    /// Held by whoever may cancel. Its tokens see Cancel() from any thread; cancelling twice is
    /// cancelling once, and nothing undoes it.
    class CancelSource
    {
    public:
        explicit CancelSource(IAllocator& allocator) : m_state(MakeRef<detail::CancelState>(allocator)) {}

        void Cancel() noexcept { m_state->cancelled.store(true, std::memory_order_release); }
        [[nodiscard]] bool IsCancelled() const noexcept { return m_state->cancelled.load(std::memory_order_acquire); }

        /// A token for the work, cancelled when this is.
        [[nodiscard]] CancelToken Token() const noexcept { return CancelToken(m_state); }

    private:
        RefPtr<detail::CancelState> m_state;
    };
}
