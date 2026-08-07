// Draconic UI - :focus_manager partition
//
// Manages keyboard focus and mouse capture (tracked by ViewId for deletion safety), tab navigation,
// and directional/spatial focus. Ported from Sedulous.UI/src/Input/FocusManager.bf. All View-touching
// bodies live in the module impl unit. (PopupLayer-constrained focus root is deferred - GetFocusRoot
// returns the full root until the Overlay subsystem lands.)

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.ui:focus_manager;

import draconic.foundation; // Array, ViewId-compatible
import :view_id;
import :input_enums; // FocusDirection

using namespace draconic::foundation;

export namespace draconic::ui
{
    class View;
    class UIContext;

    class FocusManager
    {
    public:
        explicit FocusManager(UIContext* context) : m_context(context) {}

        // === Focus ===
        [[nodiscard]] View* FocusedView() const; // impl unit
        [[nodiscard]] ViewId FocusedId() const noexcept { return m_focusedId; }
        void SetFocus(View* view); // impl unit
        void ClearFocus();         // impl unit

        // === Focus stack (for popups) ===
        void PushFocus(); // impl unit
        void PopFocus();  // impl unit
        [[nodiscard]] usize FocusStackDepth() const noexcept { return m_focusStack.Size(); }

        // === Mouse capture ===
        [[nodiscard]] View* CapturedView() const; // impl unit
        [[nodiscard]] bool HasCapture() const;    // impl unit
        void SetCapture(View* view);              // impl unit
        void ReleaseCapture() { m_capturedId = ViewId::Invalid; }

        // === Navigation ===
        void FocusNext();                         // impl unit
        void FocusPrev();                         // impl unit
        bool MoveFocus(FocusDirection direction); // impl unit

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
        Array<ViewId> m_focusStack;
    };
}
