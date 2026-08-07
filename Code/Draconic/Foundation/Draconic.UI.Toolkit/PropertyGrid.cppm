// Draconic UI Toolkit - :property_grid partition
//
// Property inspector grid: a ScrollView of PropertyEditors grouped by category into Expanders, each shown
// as a label + editor row (an EditableLabel when the editor has OnLabelRenamed set). Ported from
// Sedulous.UI.Toolkit/src/PropertyGrid/PropertyGrid.bf.
//
// Beef owned `List<PropertyEditor> mEditors` (deletes each) -> Array<RefPtr<PropertyEditor>>. The Beef
// Dictionary<String,List> category grouping -> parallel Arrays (categoryOrder + per-category editor lists),
// preserving first-seen order like the original. `new FlexLayout.LayoutParams()` -> RefPtr<FlexLayoutParams>
// / RefPtr<LayoutParams>; `.Value =` on properties -> SetValue(...); `.Match` -> SizeSpec::Match().

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.ui.toolkit:property_grid;

import draconic.foundation;
import draconic.vg;
import draconic.fonts;
import draconic.ui;
import :property_editor;

using namespace draconic::foundation;
namespace fonts = draconic::fonts;

export namespace draconic::ui::toolkit
{
    /// Property inspector grid. Displays PropertyEditors grouped by category into Expanders.
    class PropertyGrid : public ViewGroup
    {
        DRACONIC_OBJECT(PropertyGrid, ViewGroup)
    public:
        /// Ratio of label width to total width (0.1 - 0.9).
        f32 LabelWidthRatio = 0.4f;

        /// Height of each property row.
        f32 RowHeight = 26.0f;

        /// Vertical spacing between property rows.
        f32 RowSpacing = 6.0f;

        PropertyGrid()
        {
            RefPtr<ScrollView> scrollView = MakeRef<ScrollView>(DefaultAllocator());
            scrollView->VScrollBarPolicy.SetValue(ScrollBarPolicy::Auto);
            scrollView->HScrollBarPolicy.SetValue(ScrollBarPolicy::Never);
            scrollView->ScrollBarMode.SetValue(ScrollBarModeValue::Reserved);
            m_scrollView = scrollView.Get();
            AddView(scrollView.Get());

            RefPtr<FlexLayout> content = MakeRef<FlexLayout>(DefaultAllocator());
            content->Direction = Orientation::Vertical;
            m_content = content.Get();
            // `LayoutParams` names View's shadowing member here, so the type is spelled draconic::ui::LayoutParams.
            RefPtr<draconic::ui::LayoutParams> lp =
                MakeRef<draconic::ui::LayoutParams>(DefaultAllocator());
            lp->Width = SizeSpec::Match();
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
        void Clear()
        {
            m_editors.Clear();
            m_actionCategories.Clear();
            m_actionViews.Clear();
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

            // Add categorized in Expanders.
            for (usize c = 0; c < categoryOrder.Size(); ++c)
            {
                RefPtr<Expander> expander = MakeRef<Expander>(DefaultAllocator());
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

                RefPtr<FlexLayout> catContent = MakeRef<FlexLayout>(DefaultAllocator());
                catContent->Direction = Orientation::Vertical;
                catContent->Spacing = RowSpacing;

                for (usize e = 0; e < categoryLists[c].Size(); ++e)
                {
                    AddEditorRowTo(catContent.Get(), categoryLists[c][e]);
                }

                RefPtr<FlexLayoutParams> contentLp = MakeRef<FlexLayoutParams>(DefaultAllocator());
                contentLp->Width = SizeSpec::Match();
                expander->SetContent(catContent.Get(), contentLp);

                RefPtr<FlexLayoutParams> expLp = MakeRef<FlexLayoutParams>(DefaultAllocator());
                expLp->Width = SizeSpec::Match();
                m_content->AddView(expander.Get(), expLp);
            }
        }

        void AddEditorRowTo(FlexLayout* container, PropertyEditor* editor)
        {
            RefPtr<FlexLayout> row = MakeRef<FlexLayout>(DefaultAllocator());
            row->Direction = Orientation::Horizontal;
            row->Spacing = 6.0f; // gap between the (ellipsized) label column and the value editor

            // Label - editable if editor has OnLabelRenamed set.
            if (editor->OnLabelRenamed)
            {
                RefPtr<EditableLabel> editableLabel = MakeRef<EditableLabel>(DefaultAllocator());
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
                RefPtr<FlexLayoutParams> lp = MakeRef<FlexLayoutParams>(DefaultAllocator());
                lp->Grow = LabelWidthRatio;
                row->AddView(editableLabel.Get(), lp);
            }
            else
            {
                RefPtr<Label> label = MakeRef<Label>(DefaultAllocator());
                label->SetText(editor->DisplayName());
                label->FontSize.SetValue(12.0f);
                label->VAlign.SetValue(fonts::VerticalAlignment::Middle);
                label->Ellipsis.SetValue(
                    true); // truncate instead of overflowing into the value when narrow
                editor->BindDisplayNameSink([raw = label.Get()](StringView text)
                                            { raw->SetText(text); });
                RefPtr<FlexLayoutParams> lp = MakeRef<FlexLayoutParams>(DefaultAllocator());
                lp->Grow = LabelWidthRatio;
                row->AddView(label.Get(), lp);
            }

            // Editor view.
            View* editorView = editor->EditorView();
            if (editorView != nullptr)
            {
                RefPtr<FlexLayoutParams> lp = MakeRef<FlexLayoutParams>(DefaultAllocator());
                lp->Grow = 1.0f - LabelWidthRatio;
                row->AddView(editorView, lp);
            }

            // Row-level presentation carried by the editor: tooltip + conditional visibility.
            if (!editor->Tooltip().IsEmpty())
            {
                row->TooltipText = String(editor->Tooltip());
            }
            editor->SetRowView(row.Get());

            RefPtr<FlexLayoutParams> rowLp = MakeRef<FlexLayoutParams>(DefaultAllocator());
            rowLp->Width = SizeSpec::Match();
            container->AddView(row.Get(), rowLp);
        }

        ScrollView* m_scrollView = nullptr; // borrowed; the ViewGroup child tree owns it
        FlexLayout* m_content = nullptr;    // borrowed; the ScrollView tree owns it
        Array<RefPtr<PropertyEditor>> m_editors;
        Array<String> m_actionCategories;   // parallel: category -> header-action view
        Array<RefPtr<View>> m_actionViews;
        bool m_needsRebuild = true;
    };

    DRACONIC_DEFINE_OBJECT(PropertyGrid, "draconic::ui::toolkit")
}
