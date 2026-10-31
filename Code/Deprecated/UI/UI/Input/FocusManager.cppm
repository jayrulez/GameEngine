// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI - :focus_manager partition
//
// Manages keyboard focus and mouse capture (tracked by ViewId for deletion safety), tab navigation,
// and directional/spatial focus. All View-touching bodies live in the module impl unit. GetFocusRoot
// is scoped to the topmost focus-taking popup when one is open, so modals trap Tab/arrows and their
// content is keyboard-reachable.

module;
#include "Core/Prelude.h"

export module foundation.ui:focus_manager;

import foundation.core; // Array, ViewId-compatible
import :view_id;
import :input_enums; // FocusDirection

using namespace foundation::core;

export namespace foundation::ui
{
    class View;
    class UIContext;

    /// How the current focus was acquired. Focus is retained regardless of source (typing, list
    /// arrow-nav, and tab continuity all key off the focused view), but controls only draw their
    /// focus indicator for Keyboard-acquired focus (View::IsFocusVisible, the :focus-visible
    /// split). Pointer and Programmatic focus hold focus without a ring.
    enum class FocusSource : u8
    {
        Programmatic,
        Pointer,
        Keyboard,
    };

    class FocusManager
    {
    public:
        explicit FocusManager(UIContext* context) : m_context(context) {}

        // === Focus ===
        [[nodiscard]] View* FocusedView() const; // impl unit
        [[nodiscard]] ViewId FocusedId() const noexcept { return m_focusedId; }
        [[nodiscard]] FocusSource Source() const noexcept { return m_focusSource; }
        void SetFocus(View* view, FocusSource source = FocusSource::Programmatic); // impl unit
        void ClearFocus();                                                         // impl unit

        /// Focus the first focusable view (tab order) inside `scope`; false if none. Used for
        /// initial focus in dialogs (Programmatic: holds focus, draws no ring).
        bool FocusFirstIn(View* scope); // impl unit

        // === Focus save/restore (each focus-taking popup owns its saved entry) ===
        /// What a focus-taking popup remembers to give focus back on close.
        struct SavedFocus
        {
            ViewId id{};
            FocusSource source = FocusSource::Programmatic;
        };
        /// Save the current focus and how it was acquired, then clear it (a focus-taking popup is
        /// opening). The popup stores the returned entry, and each popup restores only what it
        /// saved, so closing popups out of LIFO order never restores another popup's focus.
        SavedFocus SaveAndClearFocus(); // impl unit
        /// Restore a saved entry. No-op unless the view still resolves and is still focusable,
        /// effectively enabled, and not Gone (it may have been disabled/hidden/destroyed while the
        /// popup was open). Restores with the original source: a pointer-focused button comes back
        /// from a modal holding focus but ringless.
        void RestoreFocus(SavedFocus saved); // impl unit
        /// Outstanding saved entries (popups currently holding a saved focus). Consumers use
        /// "depth == 0" to distinguish a real blur from focus moving into a popup.
        [[nodiscard]] usize FocusStackDepth() const noexcept { return m_savedCount; }

        // === Mouse capture ===
        [[nodiscard]] View* CapturedView() const; // impl unit
        [[nodiscard]] bool HasCapture() const;    // impl unit
        void SetCapture(View* view);              // impl unit
        void ReleaseCapture() { m_capturedId = ViewId::Invalid; }

        // === Navigation ===
        void FocusNext();                         // impl unit
        void FocusPrev();                         // impl unit
        bool MoveFocus(FocusDirection direction); // impl unit
        /// The views Tab visits, in the order it visits them (within the topmost focus-taking
        /// popup when one is open).
        void CollectTabStops(Array<View*>& out) const; // impl unit

        // === Deletion safety ===
        void OnViewDeleted(View* view); // impl unit

    private:
        // Internal helpers (impl unit).
        void CollectFocusable(View* view, Array<View*>& output) const;
        void SortByTabIndex(Array<View*>& list) const;
        [[nodiscard]] isize FindCurrentIndex(const Array<View*>& list) const;
        [[nodiscard]] View* GetFocusRoot() const;
        [[nodiscard]] static bool IsDescendantOf(View* view, View* ancestor);

        UIContext* m_context = nullptr;
        ViewId m_focusedId{};
        ViewId m_capturedId{};
        FocusSource m_focusSource = FocusSource::Programmatic;
        usize m_savedCount = 0; // outstanding SaveAndClearFocus entries (held by popups)
    };
}
