// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI - :popup_entry partition
//
// Entry tracking a single popup in the PopupLayer. Ported from Sedulous.UI/src/Overlay/PopupEntry.bf.
// The popup is held as a RefPtr<View> (RAII co-ownership while open); the Owner is borrowed. OwnsView is
// kept for API parity - lifetime is governed by ref-counting (drop-on-close destroys iff no other ref).

module;
#include "Core/Prelude.h"

export module foundation.ui:popup_entry;

import foundation.core; // RefPtr
import :view;
import :ipopup_owner;
import :focus_manager; // FocusManager::SavedFocus

using namespace foundation::core;

export namespace foundation::ui
{
    struct PopupEntry
    {
        RefPtr<View> Popup;               ///< The popup view.
        IPopupOwner* Owner = nullptr;     ///< Notified when this popup closes (borrowed).
        bool CloseOnClickOutside = false; ///< Clicking outside dismisses it.
        bool IsModal = false;             ///< Blocks input to underlying content.
        bool OwnsView = true;   ///< PopupLayer is the primary owner (delete-on-close semantics).
        bool PushedFocus = false; ///< This popup took focus on open (restored on close).
        /// The focus this popup displaced, restored on close. Owned HERE (not a manager-global
        /// stack) so each popup restores only what IT saved - out-of-LIFO-order closes can never
        /// cross-restore another popup's focus.
        FocusManager::SavedFocus SavedFocusEntry{};
        f32 X = 0.0f, Y = 0.0f; ///< Position in PopupLayer coordinates.
    };
}
