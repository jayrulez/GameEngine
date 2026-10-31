// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI Toolkit - :property_editor partition
//
// Abstract base for the typed property editors used by PropertyGrid (a label and editing-control row).
// Supports transactional editing for undo and redo: OnEditBegin, OnValueChanged (which may fire many
// times), then OnEditEnd or OnEditCancelled.
//
// PropertyEditor is an Object with RTTI_OBJECT: abstract (CreateEditorView and RefreshView are pure
// virtual) but still carrying a type identity, like the core abstract Drawable. The editor holds its
// view as a RefPtr<View> so its borrowed child pointers stay valid across PropertyGrid rebuilds. Empty
// String fields mean unset.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.ui.toolkit:property_editor;

import foundation.core;
import foundation.vg;
import foundation.ui;

using namespace foundation::core;

export namespace foundation::ui::toolkit
{
    /// Abstract base for typed property editors used by PropertyGrid. Subclasses implement
    /// CreateEditorView() to return the editing control and RefreshView() to update it from external state.
    class PropertyEditor : public Object
    {
        RTTI_OBJECT(PropertyEditor, Object)
    public:
        /// Fired each time the value changes (may fire multiple times per edit gesture).
        Event<void(PropertyEditor*)> OnValueChanged;

        /// Fired once when an edit gesture begins (drag start, text field focus, etc.).
        Event<void(PropertyEditor*)> OnEditBegin;

        /// Fired once when an edit gesture completes successfully.
        Event<void(PropertyEditor*)> OnEditEnd;

        /// Fired if an edit gesture is cancelled (Escape key, etc.).
        Event<void(PropertyEditor*)> OnEditCancelled;

        /// Optional callback for making the label editable. When set, PropertyGrid renders the label as an
        /// EditText. The delegate receives the new name.
        Function<void(StringView)> OnLabelRenamed;

        explicit PropertyEditor(StringView name, StringView category = {}) : m_name(name)
        {
            if (category.Size() > 0)
            {
                m_category = String(category);
            }
        }

        ~PropertyEditor() override = default;

        /// Property identity name. Stable, machine-readable (e.g., "CastsShadows").
        [[nodiscard]] StringView Name() const { return m_name; }

        /// Display label shown in the inspector UI. Falls back to Name if not set.
        [[nodiscard]] StringView DisplayName() const
        {
            return m_displayName.IsEmpty() ? StringView(m_name) : StringView(m_displayName);
        }

        /// Sets the display label (for pretty names like "Casts Shadows"). It applies
        /// live: a built row's label follows it (PropertyGrid binds the sink as it binds row visibility).
        void SetDisplayName(StringView displayName)
        {
            m_displayName = String(displayName);
            if (m_displayNameSink)
            {
                m_displayNameSink(DisplayName());
            }
        }

        /// Called by PropertyGrid each time it (re)builds this editor's row, to keep the row's
        /// label view in sync with later SetDisplayName calls.
        void BindDisplayNameSink(Function<void(StringView)> sink)
        {
            m_displayNameSink = static_cast<Function<void(StringView)>&&>(sink);
        }

        /// Plain-text tooltip for the whole row (empty for none). PropertyGrid copies it onto
        /// the row view's TooltipText when it builds the row.
        [[nodiscard]] StringView Tooltip() const { return m_tooltip; }
        void SetTooltip(StringView tooltip) { m_tooltip = String(tooltip); }

        /// Conditional row visibility (e.g. spot-only light angles). Toggling flips the live
        /// row (PropertyGrid wires it via SetRowView) without a structural rebuild.
        [[nodiscard]] bool RowVisible() const noexcept { return m_rowVisible; }
        void SetRowVisible(bool visible)
        {
            if (m_rowVisible == visible)
            {
                return;
            }
            m_rowVisible = visible;
            ApplyRowVisibility();
        }

        /// Called by PropertyGrid each time it (re)builds this editor's row (borrowed; the
        /// grid's content tree owns the row). Applies the current visibility to the new row.
        void SetRowView(View* row)
        {
            m_rowView = row;
            ApplyRowVisibility();
        }

        [[nodiscard]] StringView Category() const { return m_category; }

        /// Whether an edit gesture is currently in progress.
        [[nodiscard]] bool IsEditing() const noexcept { return m_isEditing; }

        /// Get or create the editor view (lazy).
        [[nodiscard]] View* EditorView()
        {
            if (m_editorView.Get() == nullptr)
            {
                m_editorView = CreateEditorView();
            }
            return m_editorView.Get();
        }

        /// Refresh the view from the current value (for external state changes).
        virtual void RefreshView() = 0;

    protected:
        /// Create the editing control. Called once, lazily.
        virtual RefPtr<View> CreateEditorView() = 0;

        /// Notify that the value changed (call from subclasses).
        void NotifyValueChanged() { OnValueChanged.Invoke(this); }

        /// Call when an edit gesture begins.
        void BeginEdit()
        {
            if (!m_isEditing)
            {
                m_isEditing = true;
                OnEditBegin.Invoke(this);
            }
        }

        /// Call when an edit gesture completes successfully.
        void EndEdit()
        {
            if (m_isEditing)
            {
                m_isEditing = false;
                OnEditEnd.Invoke(this);
            }
        }

        /// Call when an edit gesture is cancelled (Escape pressed, etc.).
        void CancelEdit()
        {
            if (m_isEditing)
            {
                m_isEditing = false;
                OnEditCancelled.Invoke(this);
            }
        }

    private:
        void ApplyRowVisibility()
        {
            if (m_rowView == nullptr)
            {
                return;
            }
            m_rowView->Visibility = m_rowVisible ? VisibilityValue::Visible : VisibilityValue::Gone;
            m_rowView->Invalidate();
        }

        String m_name; // machine-readable identity
        String m_displayName; // empty means use Name
        Function<void(StringView)>
            m_displayNameSink;     // row label binding (PropertyGrid)
        String m_category;         // empty == uncategorized
        String m_tooltip;          // empty == none
        RefPtr<View> m_editorView; // owned; the view tree also refs it once added
        View* m_rowView = nullptr; // borrowed; PropertyGrid's content tree owns it
        bool m_isEditing = false;
        bool m_rowVisible = true;
    };

    RTTI_DEFINE_OBJECT(PropertyEditor, "rtti::ui::toolkit")
}
