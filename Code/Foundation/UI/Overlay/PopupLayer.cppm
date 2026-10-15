// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI - :popup_layer partition
//
// Central overlay manager: always the last child of RootView (topmost for drawing, first for hit-test).
// Manages popup lifecycle, modal backdrops, and click-outside dismissal. Popups are NOT regular children
// - they are tracked via PopupEntry (RefPtr<View>-held) and positioned/drawn/hit-tested independently.
// Ported from Sedulous.UI/src/Overlay/PopupLayer.bf. Ownership: popups are RefPtr-held (RAII), so the
// Beef destructor's manual detach/delete is unnecessary; `ownsView` is stored for parity (drop-on-close
// destroys iff no other ref). The factory ShowPopup's Beef `delegate Vector2?(int32)` -> Function<
// Optional<Float2>(i32)>.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.ui:popup_layer;

import foundation.core;
import foundation.vg;
import :view;
import :box_constraints;
import :draw_context;
import :event_args;
import :ipopup_owner;
import :popup_entry;
import :modal_backdrop;

using namespace foundation::core;

export namespace foundation::ui
{
    class PopupLayer : public ViewGroup
    {
        RTTI_OBJECT(PopupLayer, ViewGroup)
    public:
        ~PopupLayer() override
        {
            // Popups live in m_entries, NOT m_children - so ~ViewGroup's back-pointer clearing
            // never sees them. A popup view held elsewhere (a MenuBar's persistent menu, a
            // ComboBox dropdown) would keep a dangling Parent into this freed layer, and the
            // next ShowPopup would dereference it. No OnPopupClosed / focus-restore here: the
            // whole layer is dying with its root.
            for (PopupEntry& entry : m_entries)
            {
                View* popup = entry.Popup.Get();
                if (popup == nullptr)
                {
                    continue;
                }
                if (popup->Context != nullptr)
                {
                    popup->Context->DetachView(popup);
                }
                if (popup->Parent == this)
                {
                    popup->Parent = nullptr;
                }
            }
        }

        [[nodiscard]] bool HasModalPopup() const
        {
            for (const PopupEntry& e : m_entries)
            {
                if (e.IsModal)
                {
                    return true;
                }
            }
            return false;
        }
        [[nodiscard]] usize PopupCount() const noexcept { return m_entries.Size(); }
        [[nodiscard]] View* TopmostModalPopup() const
        {
            for (usize i = m_entries.Size(); i-- > 0;)
            {
                if (m_entries[i].IsModal)
                {
                    return m_entries[i].Popup.Get();
                }
            }
            return nullptr;
        }

        /// The topmost popup that took focus on open (menus, dialogs - not tooltips). While one is
        /// open it IS the keyboard focus scope: FocusManager confines Tab/arrow traversal to it,
        /// which both traps the keyboard inside modals and makes popup content Tab-reachable
        /// (popups are attached but not ViewGroup children, so the root walk never finds them).
        [[nodiscard]] View* TopmostFocusScopePopup() const
        {
            for (usize i = m_entries.Size(); i-- > 0;)
            {
                if (m_entries[i].PushedFocus)
                {
                    return m_entries[i].Popup.Get();
                }
            }
            return nullptr;
        }

        // === Show / Close ===
        void ShowPopup(View* popup, IPopupOwner* owner, f32 x, f32 y,
                       bool closeOnClickOutside = true, bool isModal = false, bool ownsView = true,
                       bool takesFocus = true)
        {
            ShowPopupInternal(popup, owner, x, y, closeOnClickOutside, isModal, ownsView,
                              takesFocus);
        }

        /// Show a popup using a position factory: called with attempt 0,1,2,... returning candidate
        /// positions (None to stop). The popup is attached + measured first, then candidates are tried
        /// until one fits the viewport; otherwise the last is clamped.
        void ShowPopup(View* popup, IPopupOwner* owner,
                       Function<Optional<Float2>(i32)> positionFactory,
                       bool closeOnClickOutside = true, bool isModal = false, bool ownsView = true,
                       bool takesFocus = true)
        {
            ShowPopupInternal(popup, owner, 0, 0, closeOnClickOutside, isModal, ownsView,
                              takesFocus);

            popup->Measure(BoxConstraints::Loose(Width(), Height()));
            const Float2 popupSize = popup->MeasuredSize;

            f32 bestX = 0, bestY = 0;
            bool placed = false;
            for (i32 attempt = 0; attempt < 16; ++attempt)
            {
                Optional<Float2> candidate = positionFactory(attempt);
                if (!candidate.HasValue())
                {
                    break;
                }
                const Float2 pos = candidate.Value();
                bestX = pos.x;
                bestY = pos.y;
                if (pos.x >= 0 && pos.y >= 0 && pos.x + popupSize.x <= Width() &&
                    pos.y + popupSize.y <= Height())
                {
                    placed = true;
                    break;
                }
            }
            if (!placed)
            {
                bestX = Clamp(bestX, 0.0f, Max(0.0f, Width() - popupSize.x));
                bestY = Clamp(bestY, 0.0f, Max(0.0f, Height() - popupSize.y));
            }
            UpdatePopupPosition(popup, bestX, bestY);
        }

        /// Close EVERY open popup (topmost first; ClosePopup cascades dependents). Used when
        /// a root view is detached from its window: a detached root receives no input and no
        /// ticks, so an open menu would freeze and still be showing when the root is
        /// re-attached later (the editor's mode swap hit exactly this with the File menu).
        void CloseAllPopups()
        {
            while (!m_entries.IsEmpty())
            {
                ClosePopup(m_entries[m_entries.Size() - 1].Popup.Get());
            }
        }

        /// Close a specific popup.
        void ClosePopup(View* popup)
        {
            // Cascade: any popup whose owner lives inside the popup being closed must close first, else its
            // Owner pointer would dangle on the next layout.
            CloseDependentPopups(popup);

            for (usize i = 0; i < m_entries.Size(); ++i)
            {
                if (m_entries[i].Popup.Get() == popup)
                {
                    PopupEntry entry = Move(m_entries[i]);
                    m_entries.RemoveAt(i);

                    if (popup->Context != nullptr)
                    {
                        popup->Context->DetachView(popup);
                    }
                    popup->Parent = nullptr;
                    if (entry.Owner != nullptr)
                    {
                        entry.Owner->OnPopupClosed(popup);
                    }
                    // entry (and its RefPtr) drops here -> popup destroyed iff no other ref (ownsView).

                    if (entry.PushedFocus && Context != nullptr)
                    {
                        // Restore what THIS popup displaced (validated: no-op if the target died,
                        // was disabled, or hidden meanwhile). Restores with the original
                        // FocusSource - a pointer-focused button comes back ringless.
                        Context->GetFocusManager()->RestoreFocus(entry.SavedFocusEntry);
                    }
                    if (!HasModalPopup() && m_backdrop && m_backdrop->Parent != nullptr)
                    {
                        RemoveView(m_backdrop.Get(), false);
                    }
                    Invalidate();
                    return;
                }
            }
        }

        /// Close popups stacked above the topmost popup containing `hitView` (topmost first). When hitView
        /// is null or outside every popup, every CloseOnClickOutside popup is closed. Returns true if any
        /// popup was closed AND button is LMB (0) - i.e. the click was consumed.
        bool HandleClickOutside(View* hitView, i32 button)
        {
            i32 hitPopupIndex = -1;
            for (View* v = hitView; v != nullptr; v = v->Parent)
            {
                if (v->Parent == this)
                {
                    for (usize i = 0; i < m_entries.Size(); ++i)
                    {
                        if (m_entries[i].Popup.Get() == v)
                        {
                            hitPopupIndex = static_cast<i32>(i);
                            break;
                        }
                    }
                    break;
                }
            }

            bool closed = false;
            while (true)
            {
                bool found = false;
                for (i32 i = static_cast<i32>(m_entries.Size()) - 1; i > hitPopupIndex; --i)
                {
                    if (m_entries[static_cast<usize>(i)].CloseOnClickOutside)
                    {
                        ClosePopup(m_entries[static_cast<usize>(i)].Popup.Get());
                        closed = true;
                        found = true;
                        break;
                    }
                }
                if (!found)
                {
                    break;
                }
            }
            return closed && button == 0;
        }

        void UpdatePopupPosition(View* popup, f32 x, f32 y)
        {
            for (PopupEntry& entry : m_entries)
            {
                if (entry.Popup.Get() == popup)
                {
                    entry.X = x;
                    entry.Y = y;
                    Invalidate();
                    return;
                }
            }
        }

        // === Hit testing (three-state: popup hit / modal-block / pass-through) ===
        View* HitTest(Float2 localPoint) override
        {
            if (m_entries.Size() == 0 && (!m_backdrop || m_backdrop->Parent == nullptr))
            {
                return nullptr;
            }

            for (usize i = m_entries.Size(); i-- > 0;)
            {
                const PopupEntry& entry = m_entries[i];
                View* popup = entry.Popup.Get();
                const Float2 popupLocal{localPoint.x - entry.X, localPoint.y - entry.Y};
                if (popupLocal.x >= 0 && popupLocal.y >= 0 && popupLocal.x < popup->Width() &&
                    popupLocal.y < popup->Height())
                {
                    if (View* hit = popup->HitTest(popupLocal))
                    {
                        return hit;
                    }
                }
            }
            if (HasModalPopup())
            {
                return this;
            }
            return nullptr;
        }

        void OnDraw(UIDrawContext& ctx) override
        {
            if (m_backdrop && m_backdrop->Parent != nullptr &&
                m_backdrop->Visibility == VisibilityValue::Visible)
            {
                ctx.VG().PushState();
                ctx.VG().Translate(m_backdrop->Bounds.x, m_backdrop->Bounds.y);
                m_backdrop->OnDraw(ctx);
                ctx.VG().PopState();
            }
            for (const PopupEntry& entry : m_entries)
            {
                View* popup = entry.Popup.Get();
                if (popup->Visibility != VisibilityValue::Visible)
                {
                    continue;
                }
                ctx.VG().PushState();
                ctx.VG().Translate(entry.X, entry.Y);
                // A fading popup fades as one (VGContext::PushLayer), as a faded view does.
                const bool faded = popup->Opacity < 1.0f;
                if (faded)
                {
                    ctx.VG().PushLayer(popup->Opacity);
                }
                popup->OnDraw(ctx);
                if (faded)
                {
                    ctx.VG().PopLayer();
                }
                ctx.VG().PopState();
            }
        }

    protected:
        void OnMeasure(BoxConstraints constraints) override
        {
            MeasuredSize = Float2{constraints.ConstrainWidth(constraints.MaxWidth),
                                  constraints.ConstrainHeight(constraints.MaxHeight)};
        }
        void OnLayout(f32 left, f32 top, f32 width, f32 height) override
        {
            (void)left;
            (void)top;
            if (m_backdrop && m_backdrop->Parent != nullptr)
            {
                m_backdrop->Layout(0, 0, width, height);
            }
            for (const PopupEntry& entry : m_entries)
            {
                View* popup = entry.Popup.Get();
                popup->Measure(BoxConstraints::Loose(width, height));
                // Clamp to the space below the anchor: a popup placed low would otherwise get
                // its full measured height and run off the layer, with the tail unreachable.
                // Scroll-aware popups (ContextMenu) see the smaller Height() and scroll.
                const f32 availableH = Max(0.0f, height - entry.Y);
                popup->Layout(entry.X, entry.Y, popup->MeasuredSize.x,
                              Min(popup->MeasuredSize.y, availableH));
            }
        }

    private:
        void ShowPopupInternal(View* popup, IPopupOwner* owner, f32 x, f32 y,
                               bool closeOnClickOutside, bool isModal, bool ownsView,
                               bool takesFocus)
        {
            PopupEntry entry;
            entry.Popup = RefPtr<View>(popup);
            entry.Owner = owner;
            entry.CloseOnClickOutside = closeOnClickOutside;
            entry.IsModal = isModal;
            entry.OwnsView = ownsView;
            entry.PushedFocus = takesFocus;
            entry.X = x;
            entry.Y = y;

            // Tooltips (takesFocus=false) must never disturb focus - a tooltip appearing
            // mid-typing would otherwise clear the editor's focus and kill its completion popup.
            if (takesFocus && Context != nullptr)
            {
                entry.SavedFocusEntry = Context->GetFocusManager()->SaveAndClearFocus();
            }

            const bool hadModal = HasModalPopup();
            m_entries.PushBack(Move(entry));

            if (isModal && !hadModal)
            {
                if (!m_backdrop)
                {
                    m_backdrop = MakeRef<ModalBackdrop>(MemoryAllocator());
                }
                if (m_backdrop->Parent == nullptr)
                {
                    AddView(m_backdrop.Get());
                }
            }

            popup->Parent = this;
            if (Context != nullptr)
            {
                Context->AttachView(popup);
            }
            Invalidate();
        }

        /// Close popups whose owner lives inside `parent`'s subtree (restart after each - list mutates).
        void CloseDependentPopups(View* parent)
        {
            while (true)
            {
                View* toClose = nullptr;
                for (const PopupEntry& entry : m_entries)
                {
                    if (entry.Owner == nullptr)
                    {
                        continue;
                    }
                    View* ownerView = entry.Owner->OwnerView();
                    if (ownerView != nullptr && IsDescendant(ownerView, parent))
                    {
                        toClose = entry.Popup.Get();
                        break;
                    }
                }
                if (toClose == nullptr)
                {
                    break;
                }
                ClosePopup(toClose);
            }
        }

        [[nodiscard]] static bool IsDescendant(View* child, View* ancestor)
        {
            for (View* p = child; p != nullptr; p = p->Parent)
            {
                if (p == ancestor)
                {
                    return true;
                }
            }
            return false;
        }

        Array<PopupEntry> m_entries;
        RefPtr<ModalBackdrop> m_backdrop;
    };

    RTTI_DEFINE_OBJECT(PopupLayer, "rtti::ui")
}
