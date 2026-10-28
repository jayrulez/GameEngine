// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI Toolkit - :property_grid partition
//
// Property inspector grid: a ScrollView of PropertyEditors grouped by category into Expanders, each shown
// as a label + editor row (an EditableLabel when the editor has OnLabelRenamed set). Ported from
// Sedulous.UI.Toolkit/src/PropertyGrid/PropertyGrid.bf.
//
// Beef owned `List<PropertyEditor> mEditors` (deletes each) -> Array<RefPtr<PropertyEditor>>. The Beef
// Dictionary<String,List> category grouping -> parallel Arrays (categoryOrder + per-category editor lists),
// preserving first-seen order like the original. `new FlexLayout.LayoutParams()` -> LayoutStyle
// / LayoutStyle; `.Value =` on properties -> SetValue(...); `.Match` -> SizeSpec::Match().

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.ui.toolkit:property_grid;

import foundation.core;
import foundation.vg;
import foundation.fonts;
import foundation.ui;
import :property_editor;

using namespace foundation::core;
namespace fonts = foundation::fonts;

export namespace foundation::ui::toolkit
{
    /// Property inspector grid. Displays PropertyEditors grouped by category into Expanders.
    class PropertyGrid : public ViewGroup
    {
        RTTI_OBJECT(PropertyGrid, ViewGroup)
    public:
        /// Ratio of label width to total width (0.1 - 0.9).
        f32 LabelWidthRatio = 0.4f;

        /// Height of each property row.
        f32 RowHeight = 26.0f;

        /// Vertical spacing between property rows.
        f32 RowSpacing = 6.0f;

        PropertyGrid()
        {
            RefPtr<ScrollView> scrollView = MakeRef<ScrollView>(MemoryAllocator());
            scrollView->VScrollBarPolicy.SetValue(ScrollBarPolicy::Auto);
            scrollView->HScrollBarPolicy.SetValue(ScrollBarPolicy::Never);
            scrollView->ScrollBarMode.SetValue(ScrollBarModeValue::Reserved);
            m_scrollView = scrollView.Get();
            AddView(scrollView.Get());

            RefPtr<FlexLayout> content = MakeRef<FlexLayout>(MemoryAllocator());
            content->Direction = Orientation::Vertical;
            m_content = content.Get();
            // `LayoutParams` names View's shadowing member here, so the type is spelled foundation::ui::LayoutParams.
            foundation::ui::LayoutStyle lp;
            lp.Width = SizeSpec::Match();
            m_scrollView->AddView(content.Get(), lp);
        }

        /// Add a property editor (takes ownership).
        void AddProperty(RefPtr<PropertyEditor> editor)
        {
            m_editors.PushBack(Move(editor));
            m_needsRebuild = true;
            Invalidate();
        }

        /// Right-aligned action widgets for a CATEGORY's expander header (e.g. a component's copy /
        /// remove icons). Set before/with the properties; applied when the category expander builds.
        /// Pass null to clear. Re-registered each inspector rebuild (Clear() drops them).
        void SetCategoryHeaderActions(StringView category, RefPtr<View> actions)
        {
            for (usize i = 0; i < m_actionCategories.Size(); ++i)
            {
                if (StringView(m_actionCategories[i]) == category)
                {
                    m_actionViews[i] = Move(actions);
                    m_needsRebuild = true;
                    Invalidate();
                    return;
                }
            }
            m_actionCategories.PushBack(String(category));
            m_actionViews.PushBack(Move(actions));
            m_needsRebuild = true;
            Invalidate();
        }

        /// The actions registered for `category`'s header (null when none): what a test drives a
        /// section's icons through.
        [[nodiscard]] View* GetCategoryHeaderActions(StringView category) const
        {
            for (usize i = 0; i < m_actionCategories.Size(); ++i)
            {
                if (StringView(m_actionCategories[i]) == category)
                {
                    return m_actionViews[i].Get();
                }
            }
            return nullptr;
        }

        /// Places `category`'s section INSIDE `parent`'s body, after the parent's own rows, rather
        /// than beside it (a script component's behaviours inside the component). A parent that
        /// never appears, or a nesting that would loop, leaves the section at the top level.
        void SetCategoryParent(StringView category, StringView parent)
        {
            for (usize i = 0; i < m_nestedCategories.Size(); ++i)
            {
                if (StringView(m_nestedCategories[i]) == category)
                {
                    m_nestedParents[i] = String(parent);
                    m_needsRebuild = true;
                    Invalidate();
                    return;
                }
            }
            m_nestedCategories.PushBack(String(category));
            m_nestedParents.PushBack(String(parent));
            m_needsRebuild = true;
            Invalidate();
        }

        /// The category `category` nests in, or empty.
        [[nodiscard]] StringView CategoryParent(StringView category) const
        {
            for (usize i = 0; i < m_nestedCategories.Size(); ++i)
            {
                if (StringView(m_nestedCategories[i]) == category)
                {
                    return m_nestedParents[i].AsView();
                }
            }
            return {};
        }

        /// Remove a property by name.
        void RemoveProperty(StringView name)
        {
            for (usize i = 0; i < m_editors.Size(); ++i)
            {
                if (m_editors[i]->Name() == name)
                {
                    m_editors.RemoveAt(i);
                    m_needsRebuild = true;
                    Invalidate();
                    return;
                }
            }
        }

        /// Get a property editor by name.
        [[nodiscard]] PropertyEditor* GetProperty(StringView name)
        {
            for (usize i = 0; i < m_editors.Size(); ++i)
            {
                if (m_editors[i]->Name() == name)
                {
                    return m_editors[i].Get();
                }
            }
            return nullptr;
        }

        /// Remove all properties.
        /// Build the named category's expander COLLAPSED (content Gone - costing no layout or
        /// draw until opened). For bulk sections a user rarely edits (the generic asset form's
        /// per-array groups: ~1300 rows measured every damaged frame made interaction crawl).
        /// Set before/with the properties; applied when the expander builds.
        void SetCategoryDefaultCollapsed(StringView category)
        {
            for (usize i = 0; i < m_collapsedCategories.Size(); ++i)
            {
                if (StringView(m_collapsedCategories[i]) == category)
                {
                    return;
                }
            }
            m_collapsedCategories.PushBack(String(category));
            m_needsRebuild = true;
            Invalidate();
        }

        void Clear()
        {
            m_editors.Clear();
            m_actionCategories.Clear();
            m_actionViews.Clear();
            m_collapsedCategories.Clear();
            m_nestedCategories.Clear();
            m_nestedParents.Clear();
            m_needsRebuild = true;
            Invalidate();
        }

        /// Number of properties.
        [[nodiscard]] usize PropertyCount() const noexcept { return m_editors.Size(); }

        /// Editor at index (for iteration, e.g. to subscribe to per-editor events).
        [[nodiscard]] PropertyEditor* PropertyAt(usize index) const
        {
            return m_editors[index].Get();
        }

        void OnDraw(UIDrawContext& ctx) override
        {
            if (Drawable* bgDrawable = ResolveStyleDrawable(StyleProperty::Background))
            {
                bgDrawable->Draw(ctx, Rectangle{0, 0, Width(), Height()});
            }
            else
            {
                const Color bgColor =
                    ResolveStyleColor(StyleProperty::Background,
                                      Color{42.0f / 255.0f, 44.0f / 255.0f, 54.0f / 255.0f, 1.0f});
                ctx.VG().FillRect(Rectangle{0, 0, Width(), Height()}, bgColor);
            }
            DrawChildren(ctx);
        }

    protected:
        void OnMeasure(BoxConstraints constraints) override
        {
            if (m_needsRebuild)
            {
                RebuildLayout();
            }
            m_scrollView->Measure(constraints);
            MeasuredSize = Float2{constraints.ConstrainWidth(m_scrollView->MeasuredSize.x),
                                  constraints.ConstrainHeight(m_scrollView->MeasuredSize.y)};
        }

        void OnLayout(f32 left, f32 top, f32 width, f32 height) override
        {
            (void)left;
            (void)top;
            m_scrollView->Layout(0, 0, width, height);
        }

    private:
        void RebuildLayout()
        {
            m_needsRebuild = false;
            m_content->Spacing = RowSpacing;

            // Remember each category's CURRENT expansion before tearing the expanders down: a
            // rebuild (undo/redo, shape change) must not slam shut a group the user opened - or
            // reopen one they collapsed. The remembered state wins over the default-collapsed
            // list; the default applies only to categories seen for the first time.
            RememberExpansions(*m_content);

            // Clear existing content.
            while (m_content->ChildCount() > 0)
            {
                m_content->RemoveView(m_content->GetChildAt(0), true);
            }

            // Group by category, preserving first-seen order (uncategorized first).
            Array<PropertyEditor*> uncategorized;
            Array<String> categoryOrder;
            Array<Array<PropertyEditor*>> categoryLists;

            for (usize i = 0; i < m_editors.Size(); ++i)
            {
                PropertyEditor* editor = m_editors[i].Get();
                const StringView category = editor->Category();
                if (category.IsEmpty())
                {
                    uncategorized.PushBack(editor);
                }
                else
                {
                    usize catIndex = categoryOrder.Size();
                    for (usize c = 0; c < categoryOrder.Size(); ++c)
                    {
                        if (StringView(categoryOrder[c]) == category)
                        {
                            catIndex = c;
                            break;
                        }
                    }
                    if (catIndex == categoryOrder.Size())
                    {
                        categoryOrder.PushBack(String(category));
                        categoryLists.PushBack(Array<PropertyEditor*>());
                    }
                    categoryLists[catIndex].PushBack(editor);
                }
            }

            // Add uncategorized first.
            for (usize i = 0; i < uncategorized.Size(); ++i)
            {
                AddEditorRowTo(m_content, uncategorized[i]);
            }

            // Every section is built first, then placed: inside its parent's body when it names a
            // parent that is here (after the parent's own rows), else at the top level.
            Array<RefPtr<Expander>> expanders;
            Array<FlexLayout*> bodies;
            for (usize c = 0; c < categoryOrder.Size(); ++c)
            {
                RefPtr<Expander> expander = MakeRef<Expander>(MemoryAllocator());
                expander->SetHeaderText(categoryOrder[c]);
                for (usize a = 0; a < m_actionCategories.Size(); ++a)
                {
                    if (StringView(m_actionCategories[a]) == StringView(categoryOrder[c]) &&
                        m_actionViews[a].Get() != nullptr)
                    {
                        expander->SetHeaderActions(m_actionViews[a].Get());
                        break;
                    }
                }

                RefPtr<FlexLayout> catContent = MakeRef<FlexLayout>(MemoryAllocator());
                catContent->Direction = Orientation::Vertical;
                catContent->Spacing = RowSpacing;

                for (usize e = 0; e < categoryLists[c].Size(); ++e)
                {
                    AddEditorRowTo(catContent.Get(), categoryLists[c][e]);
                }

                LayoutStyle contentLp;
                contentLp.Width = SizeSpec::Match();
                expander->SetContent(catContent.Get(), contentLp);

                bool remembered = false;
                for (usize k = 0; k < m_expansionNames.Size(); ++k)
                {
                    if (StringView(m_expansionNames[k]) == StringView(categoryOrder[c]))
                    {
                        expander->SetIsExpanded(m_expansionStates[k]); // the user's last state
                        remembered = true;
                        break;
                    }
                }
                for (usize k = 0; !remembered && k < m_collapsedCategories.Size(); ++k)
                {
                    if (StringView(m_collapsedCategories[k]) == StringView(categoryOrder[c]))
                    {
                        expander->SetIsExpanded(false); // content Gone: no layout/draw until opened
                        break;
                    }
                }

                bodies.PushBack(catContent.Get());
                expanders.PushBack(Move(expander));
            }
            for (usize c = 0; c < categoryOrder.Size(); ++c)
            {
                LayoutStyle expLp;
                expLp.Width = SizeSpec::Match();
                const i32 parent = NestingParent(categoryOrder, c);
                if (parent >= 0)
                {
                    bodies[static_cast<usize>(parent)]->AddView(expanders[c].Get(), expLp);
                }
                else
                {
                    m_content->AddView(expanders[c].Get(), expLp);
                }
            }
        }

        /// The index of the section category `c` nests in, or -1 for the top level: no parent, a
        /// parent that is not here, or a chain that loops back to `c`.
        [[nodiscard]] i32 NestingParent(const Array<String>& categoryOrder, usize c) const
        {
            const auto indexOf = [&categoryOrder](StringView name) -> i32
            {
                for (usize i = 0; i < categoryOrder.Size(); ++i)
                {
                    if (StringView(categoryOrder[i]) == name)
                    {
                        return static_cast<i32>(i);
                    }
                }
                return -1;
            };
            const StringView parentName = CategoryParent(categoryOrder[c].AsView());
            if (parentName.IsEmpty())
            {
                return -1;
            }
            const i32 parent = indexOf(parentName);
            if (parent < 0 || static_cast<usize>(parent) == c)
            {
                return -1;
            }
            // Walk up from the parent: meeting `c` again is a loop, and the section stays on top.
            i32 at = parent;
            for (usize guard = 0; guard < categoryOrder.Size(); ++guard)
            {
                const StringView up = CategoryParent(categoryOrder[static_cast<usize>(at)].AsView());
                if (up.IsEmpty())
                {
                    return parent;
                }
                const i32 next = indexOf(up);
                if (next < 0)
                {
                    return parent;
                }
                if (static_cast<usize>(next) == c)
                {
                    return -1;
                }
                at = next;
            }
            return -1;
        }

        /// What every expander, nested ones included, has open, before the tree is torn down.
        void RememberExpansions(ViewGroup& container)
        {
            for (usize i = 0; i < container.ChildCount(); ++i)
            {
                if (auto* expander = Cast<Expander>(container.GetChildAt(i)))
                {
                    RememberExpansion(expander->HeaderText(), expander->IsExpanded());
                    if (auto* body = Cast<ViewGroup>(expander->Content()))
                    {
                        RememberExpansions(*body);
                    }
                }
            }
        }

        void AddEditorRowTo(FlexLayout* container, PropertyEditor* editor)
        {
            RefPtr<FlexLayout> row = MakeRef<FlexLayout>(MemoryAllocator());
            row->Direction = Orientation::Horizontal;
            row->Spacing = 6.0f; // gap between the (ellipsized) label column and the value editor

            // Label - editable if editor has OnLabelRenamed set.
            if (editor->OnLabelRenamed)
            {
                RefPtr<EditableLabel> editableLabel = MakeRef<EditableLabel>(MemoryAllocator());
                editableLabel->SetText(editor->DisplayName());
                editableLabel->FontSize.SetValue(12.0f);
                editableLabel->Ellipsis.SetValue(
                    true); // truncate instead of overflowing into the value when narrow
                PropertyEditor* boundEditor = editor;
                editableLabel->OnRenameCommitted.Add(
                    [boundEditor](EditableLabel*, StringView newName)
                    {
                        if (boundEditor->OnLabelRenamed)
                        {
                            boundEditor->OnLabelRenamed(newName);
                        }
                    });
                editor->BindDisplayNameSink([raw = editableLabel.Get()](StringView text)
                                            { raw->SetText(text); });
                LayoutStyle lp;
                lp.FlexGrow = LabelWidthRatio;
                row->AddView(editableLabel.Get(), lp);
            }
            else
            {
                RefPtr<Label> label = MakeRef<Label>(MemoryAllocator());
                label->SetText(editor->DisplayName());
                label->FontSize.SetValue(12.0f);
                label->VAlign.SetValue(fonts::VerticalAlignment::Middle);
                label->Ellipsis.SetValue(
                    true); // truncate instead of overflowing into the value when narrow
                editor->BindDisplayNameSink([raw = label.Get()](StringView text)
                                            { raw->SetText(text); });
                LayoutStyle lp;
                lp.FlexGrow = LabelWidthRatio;
                row->AddView(label.Get(), lp);
            }

            // Editor view.
            View* editorView = editor->EditorView();
            if (editorView != nullptr)
            {
                LayoutStyle lp;
                lp.FlexGrow = 1.0f - LabelWidthRatio;
                row->AddView(editorView, lp);
            }

            // Row-level presentation carried by the editor: tooltip + conditional visibility.
            if (!editor->Tooltip().IsEmpty())
            {
                row->TooltipText = String(editor->Tooltip());
            }
            editor->SetRowView(row.Get());

            LayoutStyle rowLp;
            rowLp.Width = SizeSpec::Match();
            container->AddView(row.Get(), rowLp);
        }

        ScrollView* m_scrollView = nullptr; // borrowed; the ViewGroup child tree owns it
        FlexLayout* m_content = nullptr;    // borrowed; the ScrollView tree owns it
        Array<RefPtr<PropertyEditor>> m_editors;
        Array<String> m_actionCategories;   // parallel: category -> header-action view
        Array<RefPtr<View>> m_actionViews;
        Array<String> m_collapsedCategories; // categories whose expanders build collapsed
        // Parallel: a category and the category whose body it sits in (SetCategoryParent).
        Array<String> m_nestedCategories;
        Array<String> m_nestedParents;
        bool m_needsRebuild = true;
        // Per-category expansion memory across rebuilds (parallel arrays, keyed by header text).
        Array<String> m_expansionNames;
        Array<bool> m_expansionStates;

        void RememberExpansion(StringView category, bool expanded)
        {
            for (usize i = 0; i < m_expansionNames.Size(); ++i)
            {
                if (StringView(m_expansionNames[i]) == category)
                {
                    m_expansionStates[i] = expanded;
                    return;
                }
            }
            m_expansionNames.PushBack(String(category));
            m_expansionStates.PushBack(expanded);
        }
    };

    RTTI_DEFINE_OBJECT(PropertyGrid, "rtti::ui::toolkit")
}
