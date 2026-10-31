// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI - :popup_entry partition
//
// Entry tracking a single popup in the PopupLayer. The popup is held as a RefPtr<View>, sharing
// ownership while it is open; the Owner is borrowed. OwnsView is recorded, but lifetime follows the
// reference count: dropping the entry on close destroys the popup only if nothing else holds it.

module;
#include "Core/Prelude.h"

export module foundation.ui:popup_entry;

import foundation.core;
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
        bool OwnsView = true;   ///< The caller asked PopupLayer to own the view.
        bool PushedFocus = false; ///< This popup took focus on open (restored on close).
        /// The focus this popup displaced, restored on close. It is kept per popup rather than
        /// on a stack in the focus manager, so each popup restores only what it saved and closing
        /// popups out of order cannot restore another popup's focus.
        FocusManager::SavedFocus SavedFocusEntry{};
        f32 X = 0.0f, Y = 0.0f; ///< Position in PopupLayer coordinates.
    };
}
