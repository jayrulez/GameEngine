// Draconic::EditorApp - :path_picker_dialog partition.
//
// PathPickerDialog: the AssetPickerDialog's sibling for SOURCE FILES - a modal picker over
// the files under a project directory (typically the Sources root), constrained to a set of
// extensions. Bespoke pages whose assets reference a raw file (FontAsset.fileName) use this
// instead of a hand-typed string: the user can only pick paths that exist inside the
// project, so cooked references never dangle.
//
//   - the walk is recursive from `rootPath`; rows show root-relative paths
//   - `extensions` filter (".ttf", ...) is case-insensitive; empty = every file
//   - the filter box matches a substring of the relative path
//   - double-click or [Select] confirms; [Cancel]/Escape dismisses
//
// The result is delivered through OnPicked(rootRelativePath), fired BEFORE the dialog
// closes itself. Cancel fires nothing.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.editor.app:path_picker_dialog;

import draconic.foundation;
import draconic.vfs;
import draconic.ui;

using namespace draconic::foundation;

export namespace draconic::editor::app
{
    namespace ui = draconic::ui;

    class PathPickerDialog final : public ui::Dialog
    {
        DRACONIC_OBJECT(PathPickerDialog, ui::Dialog)
    public:
        /// The pick result: a root-relative path. Fired once, before close.
        Function<void(StringView)> OnPicked;

        PathPickerDialog(StringView title, StringView rootPath, Array<String> extensions)
            : ui::Dialog(title), m_extensions(Move(extensions))
        {
            MinWidth.SetValue(460.0f);
            MinHeight.SetValue(340.0f);
            MaxWidth.SetValue(560.0f);
            MaxHeight.SetValue(420.0f);

            auto column = MakeRef<ui::FlexLayout>(DefaultAllocator());
            column->Direction = ui::Orientation::Vertical;
            column->Spacing = 6;

            m_filterEdit = MakeRef<ui::EditText>(DefaultAllocator());
            m_filterEdit->SetPlaceholder(u8"Filter...");
            {
                PathPickerDialog* self = this;
                m_filterEdit->OnTextChanged.Add(
                    [self](ui::EditText* edit)
                    {
                        self->m_filter = String(edit->Text());
                        self->RebuildRows();
                    });
                auto lp = MakeRef<ui::FlexLayoutParams>(DefaultAllocator());
                lp->Width = ui::SizeSpec::Match();
                column->AddView(m_filterEdit.Get(), lp);
            }

            m_adapter = MakeUnique<RowAdapter>(DefaultAllocator(), *this);
            m_list = MakeRef<ui::ListView>(DefaultAllocator());
            m_list->ItemHeight.SetValue(20.0f);
            m_list->SetAdapter(m_adapter.Get());
            {
                PathPickerDialog* self = this;
                m_list->OnItemClicked.Add(
                    [self](i32 position, i32 clickCount, f32, f32)
                    {
                        if (clickCount >= 2)
                        {
                            self->ConfirmAt(position);
                        }
                    });
                auto lp = MakeRef<ui::FlexLayoutParams>(DefaultAllocator());
                lp->Width = ui::SizeSpec::Match();
                lp->Grow = 1.0f;
                column->AddView(m_list.Get(), lp);
            }
            SetContent(column.Get());

            {
                PathPickerDialog* self = this;
                ui::Button* select = AddButton(u8"Select", ui::DialogResult::None);
                select->OnClick.Add([self](ui::ButtonBase*)
                                    { self->ConfirmAt(self->m_list->Selection.FirstSelected()); });
                AddButton(u8"Cancel", ui::DialogResult::Cancel);
            }

            CollectFiles(rootPath);
            RebuildRows();
        }

        ~PathPickerDialog() override { m_list->SetAdapter(nullptr); }

    private:
        class RowAdapter final : public ui::ListAdapterBase
        {
        public:
            explicit RowAdapter(PathPickerDialog& owner) : m_owner(&owner) {}
            [[nodiscard]] i32 ItemCount() const override
            {
                return static_cast<i32>(m_owner->m_rows.Size());
            }
            [[nodiscard]] RefPtr<ui::View> CreateView(i32) override
            {
                auto row = MakeRef<ui::FlexLayout>(DefaultAllocator());
                row->Padding = ui::Thickness{6, 2};
                auto label = MakeRef<ui::Label>(DefaultAllocator());
                label->FontSize.SetValue(12.0f);
                auto grow = MakeRef<ui::FlexLayoutParams>(DefaultAllocator());
                grow->Grow = 1.0f;
                row->AddView(label.Get(), grow);
                return RefPtr<ui::View>(row.Get());
            }
            void BindView(ui::View* view, i32 position) override
            {
                auto* row = Cast<ui::FlexLayout>(view);
                if (row == nullptr || row->ChildCount() == 0 || position < 0 ||
                    position >= static_cast<i32>(m_owner->m_rows.Size()))
                {
                    return;
                }
                auto* label = Cast<ui::Label>(row->GetChildAt(0));
                if (label == nullptr)
                {
                    return;
                }
                label->SetText(m_owner->m_rows[static_cast<usize>(position)].AsView());
            }

        private:
            PathPickerDialog* m_owner;
        };

        [[nodiscard]] bool ExtensionMatches(StringView name) const
        {
            if (m_extensions.IsEmpty())
            {
                return true;
            }
            for (const String& extension : m_extensions)
            {
                const StringView ext = extension.AsView();
                if (name.Size() < ext.Size())
                {
                    continue;
                }
                bool equal = true;
                const usize offset = name.Size() - ext.Size();
                for (usize i = 0; i < ext.Size(); ++i)
                {
                    utf8char a = name[offset + i];
                    utf8char b = ext[i];
                    if (a >= u8'A' && a <= u8'Z')
                    {
                        a = static_cast<utf8char>(a - u8'A' + u8'a');
                    }
                    if (b >= u8'A' && b <= u8'Z')
                    {
                        b = static_cast<utf8char>(b - u8'A' + u8'a');
                    }
                    if (a != b)
                    {
                        equal = false;
                        break;
                    }
                }
                if (equal)
                {
                    return true;
                }
            }
            return false;
        }

        // Recursive walk from the root; `m_files` keeps root-relative paths.
        void CollectFiles(StringView rootPath)
        {
            draconic::vfs::NativeFileSystem fs(rootPath);
            auto* enumerable = fs.AsEnumerable();
            if (enumerable == nullptr)
            {
                return;
            }
            Array<String> pending;
            pending.PushBack(String(u8""));
            while (!pending.IsEmpty())
            {
                const String folder = Move(pending[pending.Size() - 1]);
                pending.RemoveAt(pending.Size() - 1);
                Array<draconic::vfs::DirEntry> entries;
                if (!enumerable->Enumerate(folder.AsView(), entries).IsOk())
                {
                    continue;
                }
                for (const draconic::vfs::DirEntry& entry : entries)
                {
                    String path = folder.IsEmpty()
                                      ? String(entry.name.AsView())
                                      : PathJoin(folder.AsView(), entry.name.AsView());
                    if (entry.isDirectory)
                    {
                        pending.PushBack(Move(path));
                    }
                    else if (ExtensionMatches(path.AsView()))
                    {
                        m_files.PushBack(Move(path));
                    }
                }
            }
        }

        void RebuildRows()
        {
            m_rows.Clear();
            for (const String& file : m_files)
            {
                if (m_filter.IsEmpty() || MatchesFilter(file.AsView(), m_filter.AsView()))
                {
                    m_rows.PushBack(String(file.AsView()));
                }
            }
            m_list->Selection.ClearSelection();
            m_list->NotifyDataChanged();
        }

        [[nodiscard]] static bool MatchesFilter(StringView name, StringView filter)
        {
            if (filter.Size() > name.Size())
            {
                return false;
            }
            auto lower = [](utf8char c)
            { return (c >= u8'A' && c <= u8'Z') ? static_cast<utf8char>(c - u8'A' + u8'a') : c; };
            for (usize start = 0; start + filter.Size() <= name.Size(); ++start)
            {
                bool match = true;
                for (usize i = 0; i < filter.Size(); ++i)
                {
                    if (lower(name[start + i]) != lower(filter[i]))
                    {
                        match = false;
                        break;
                    }
                }
                if (match)
                {
                    return true;
                }
            }
            return false;
        }

        void ConfirmAt(i32 position)
        {
            if (position < 0 || position >= static_cast<i32>(m_rows.Size()))
            {
                return;
            }
            if (OnPicked)
            {
                OnPicked(m_rows[static_cast<usize>(position)].AsView());
            }
            Close(ui::DialogResult::OK);
        }

        RefPtr<ui::ListView> m_list;
        RefPtr<ui::EditText> m_filterEdit;
        UniquePtr<RowAdapter> m_adapter;
        Array<String> m_extensions;
        Array<String> m_files; // every match under the root (root-relative)
        Array<String> m_rows;  // the filtered view of m_files
        String m_filter;
    };

    DRACONIC_DEFINE_OBJECT(PathPickerDialog, "draconic::editor::app")
}
