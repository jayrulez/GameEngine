// Draconic::EditorApp - :import_dialog partition.
//
// The pre-import options dialog (Sedulous ImportDialog lineage, lean v1): dropping a file on
// the asset browser pops this up when the routed importer has options. Shows the source file,
// the destination group, and one checkbox per ImportOptions::Toggle (each writes straight into
// the options object), then Import/Cancel. Import is a caller-managed button so a future
// validation step can keep the dialog up; today it always proceeds.
//
// (Sedulous also previews the individual items with per-item rename - that per-item list is a
// later nicety; the seam is the options object, which already travels through the importer.)

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.editor.app:import_dialog;

import draconic.foundation;
import draconic.ui;
import draconic.editor.core;

using namespace draconic::foundation;

export namespace draconic::editor::app
{
    namespace ui = draconic::ui;

    class ImportOptionsDialog final : public ui::Dialog
    {
        DRACONIC_OBJECT(ImportOptionsDialog, ui::Dialog)
    public:
        /// Fired when the user confirms; the options object carries their checkbox edits.
        Function<void()> OnImport;

        ImportOptionsDialog(StringView sourcePath, StringView destination,
                            RefPtr<draconic::editor::ImportOptions> options)
            : ui::Dialog(u8"Import"), m_options(Move(options))
        {
            MaxWidth.SetValue(480.0f);
            MaxHeight.SetValue(420.0f);

            auto column = MakeRef<ui::FlexLayout>(DefaultAllocator());
            column->Direction = ui::Orientation::Vertical;
            column->Spacing = 6.0f;

            AddInfoRow(*column, u8"Source:", draconic::editor::FileNameOf(sourcePath));
            AddInfoRow(*column, u8"Into:", destination);

            if (m_options.Get() != nullptr)
            {
                for (const draconic::editor::ImportOptions::Toggle& toggle : m_options->Toggles())
                {
                    if (toggle.value == nullptr)
                    {
                        continue;
                    }
                    auto check =
                        MakeRef<ui::CheckBox>(DefaultAllocator(), toggle.label, *toggle.value);
                    check->FontSize.SetValue(12.0f);
                    if (!toggle.description.IsEmpty())
                    {
                        check->TooltipText = String(toggle.description);
                    }
                    bool* value = toggle.value;
                    check->OnCheckedChanged.Add([value](ui::CheckBox*, bool checked)
                                                { *value = checked; });
                    auto lp = MakeRef<ui::FlexLayoutParams>(DefaultAllocator());
                    lp->Width = ui::SizeSpec::Match();
                    lp->Height = ui::SizeSpec::Fixed(ui::Unit::Px(22.0f));
                    column->AddView(check.Get(), lp);
                }
            }

            SetContent(column.Get());

            ImportOptionsDialog* self = this;
            ui::Button* import = AddButton(u8"Import", ui::DialogResult::None);
            import->OnClick.Add(
                [self](ui::ButtonBase*)
                {
                    if (self->OnImport)
                    {
                        self->OnImport();
                    }
                    self->Close(ui::DialogResult::OK);
                });
            AddButton(u8"Cancel", ui::DialogResult::Cancel);
        }

        [[nodiscard]] draconic::editor::ImportOptions* Options() const noexcept
        {
            return m_options.Get();
        }

    private:
        void AddInfoRow(ui::FlexLayout& column, StringView label, StringView value)
        {
            auto row = MakeRef<ui::FlexLayout>(DefaultAllocator());
            row->Direction = ui::Orientation::Horizontal;
            row->Spacing = 6.0f;

            auto name = MakeRef<ui::Label>(DefaultAllocator(), label);
            name->FontSize.SetValue(11.0f);
            {
                auto lp = MakeRef<ui::FlexLayoutParams>(DefaultAllocator());
                lp->Width = ui::SizeSpec::Fixed(ui::Unit::Px(52.0f));
                lp->Height = ui::SizeSpec::Match();
                row->AddView(name.Get(), lp);
            }
            auto text = MakeRef<ui::Label>(DefaultAllocator(), value);
            text->FontSize.SetValue(11.0f);
            text->Ellipsis.SetValue(true);
            {
                auto lp = MakeRef<ui::FlexLayoutParams>(DefaultAllocator());
                lp->Grow = 1.0f;
                lp->Height = ui::SizeSpec::Match();
                row->AddView(text.Get(), lp);
            }
            auto lp = MakeRef<ui::FlexLayoutParams>(DefaultAllocator());
            lp->Width = ui::SizeSpec::Match();
            lp->Height = ui::SizeSpec::Fixed(ui::Unit::Px(18.0f));
            column.AddView(row.Get(), lp);
        }

        RefPtr<draconic::editor::ImportOptions> m_options;
    };

    DRACONIC_DEFINE_OBJECT(ImportOptionsDialog, "draconic::editor::app")
}
