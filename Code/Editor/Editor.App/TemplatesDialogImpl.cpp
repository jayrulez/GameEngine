// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::App - :templates_dialog partition (implementation). See TemplatesDialog.cppm.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

module editor.app;

import foundation.core;
import foundation.ui;
import editor.core;
import :editor_icons;
import :confirm_dialog;

using namespace foundation::core;
namespace ui = foundation::ui;

namespace editor::app
{
    namespace
    {
        constexpr Color kDimText{0.62f, 0.62f, 0.62f, 1.0f};
        constexpr Color kWarningText{0.91f, 0.69f, 0.29f, 1.0f};

        /// "Linux64 · Release · 0.1.0", the line under a template's name.
        String SummaryOf(const editor::ExportTemplate& tmpl)
        {
            String text(tmpl.platform.AsView());
            text += u8" · ";
            text += tmpl.EffectiveConfig();
            if (!tmpl.engineVersion.IsEmpty())
            {
                text += u8" · ";
                text += tmpl.engineVersion.AsView();
            }
            if (tmpl.isHost)
            {
                text += u8" · this editor's own";
            }
            return text;
        }
    }

    // A row: the template's icon, its name over its summary, and a warning when its engine is not
    // this editor's.
    class TemplatesDialog::Adapter final : public ui::ListAdapterBase
    {
    public:
        explicit Adapter(TemplatesDialog& owner) : m_owner(&owner) {}

        [[nodiscard]] i32 ItemCount() const override
        {
            return static_cast<i32>(m_owner->m_registry.Count());
        }

        [[nodiscard]] RefPtr<ui::View> CreateView(i32) override
        {
            IAllocator& allocator = m_owner->MemoryAllocator();
            auto row = MakeRef<ui::FlexLayout>(allocator);
            row->Direction = ui::Orientation::Horizontal;
            row->Spacing = 8;
            row->Padding = ui::Thickness{6, 4};
            ui::LayoutStyle centred;
            centred.AlignSelf = ui::Align::Center;
            auto icon = MakeRef<ui::DrawableView>(allocator);
            icon->DesiredWidth.SetValue(Optional<f32>(26.0f));
            icon->DesiredHeight.SetValue(Optional<f32>(26.0f));
            row->AddView(icon.Get(), centred);
            auto text = MakeRef<ui::FlexLayout>(allocator);
            text->Direction = ui::Orientation::Vertical;
            text->Spacing = 1;
            auto name = MakeRef<ui::Label>(allocator);
            name->FontSize.SetValue(12.5f);
            text->AddView(name.Get());
            auto summary = MakeRef<ui::Label>(allocator);
            summary->FontSize.SetValue(11.0f);
            summary->TextColor.SetValue(Optional<Color>(kDimText));
            text->AddView(summary.Get());
            ui::LayoutStyle grow;
            grow.FlexGrow = 1.0f;
            grow.AlignSelf = ui::Align::Center;
            row->AddView(text.Get(), grow);
            auto warning = MakeRef<ui::DrawableView>(allocator);
            warning->DesiredWidth.SetValue(Optional<f32>(14.0f));
            warning->DesiredHeight.SetValue(Optional<f32>(14.0f));
            warning->TooltipText = String(u8"Built for another engine version");
            row->AddView(warning.Get(), centred);
            return RefPtr<ui::View>(row.Get());
        }

        void BindView(ui::View* view, i32 position) override
        {
            auto* row = Cast<ui::FlexLayout>(view);
            const editor::ExportTemplate* tmpl =
                position >= 0 ? m_owner->TemplateAt(static_cast<usize>(position)) : nullptr;
            if (row == nullptr || row->ChildCount() < 3 || tmpl == nullptr)
            {
                return;
            }
            Cast<ui::DrawableView>(row->GetChildAt(0))->Drawable = m_owner->IconFor(*tmpl);
            auto* text = Cast<ui::FlexLayout>(row->GetChildAt(1));
            Cast<ui::Label>(text->GetChildAt(0))->SetText(tmpl->name.AsView());
            Cast<ui::Label>(text->GetChildAt(1))->SetText(SummaryOf(*tmpl).AsView());
            auto* warning = Cast<ui::DrawableView>(row->GetChildAt(2));
            const bool mismatch = !editor::TemplateEngineMatches(*tmpl);
            warning->Drawable = mismatch ? ui::DrawablePtr(EditorIcons::Get().warning.Get()) : ui::DrawablePtr{};
            warning->Visibility = mismatch ? ui::Visibility::Visible : ui::Visibility::Hidden;
        }

    private:
        TemplatesDialog* m_owner;
    };

    TemplatesDialog::TemplatesDialog(editor::EditorContext& context, TemplatesDialogSeams seams)
        : ui::Dialog(u8"Export Templates"), m_context(&context), m_seams(Move(seams))
    {
        // A fixed size: the details side changes with the selection, the dialog does not.
        MinWidth.SetValue(760.0f);
        MaxWidth.SetValue(760.0f);
        MinHeight.SetValue(460.0f);
        MaxHeight.SetValue(460.0f);

        auto body = MakeRef<ui::FlexLayout>(MemoryAllocator());
        body->Direction = ui::Orientation::Horizontal;
        body->Spacing = 0;

        // --- The list ---
        m_adapter = MakeUnique<Adapter>(MemoryAllocator(), *this);
        m_list = MakeRef<ui::ListView>(MemoryAllocator());
        m_list->ItemHeight.SetValue(42.0f);
        m_list->Padding = ui::Thickness{4, 4};
        m_list->SetAdapter(m_adapter.Get());
        {
            TemplatesDialog* self = this;
            m_list->Selection.OnSelectionChanged.Add([self]() { self->ShowDetails(); });
        }
        {
            ui::LayoutStyle side;
            side.Width = ui::SizeSpec::Fixed(ui::Unit::Dp(290.0f));
            side.Height = ui::SizeSpec::Match();
            body->AddView(m_list.Get(), side);
        }
        {
            auto rule = MakeRef<ui::Separator>(MemoryAllocator());
            rule->Orientation.SetValue(ui::Orientation::Vertical);
            ui::LayoutStyle line;
            line.Width = ui::SizeSpec::Fixed(ui::Unit::Dp(1.0f));
            line.Height = ui::SizeSpec::Match();
            body->AddView(rule.Get(), line);
        }

        // --- The details ---
        auto details = MakeRef<ui::FlexLayout>(MemoryAllocator());
        details->Direction = ui::Orientation::Vertical;
        details->Spacing = 8;
        details->Padding = ui::Thickness{16, 12};
        BuildDetails(*details);
        auto scroll = MakeRef<ui::ScrollView>(MemoryAllocator());
        scroll->VScrollBarPolicy.SetValue(ui::ScrollBarPolicy::Auto);
        scroll->HScrollBarPolicy.SetValue(ui::ScrollBarPolicy::Never);
        {
            ui::LayoutStyle match;
            match.Width = ui::SizeSpec::Match();
            scroll->AddView(details.Get(), match);
        }
        {
            ui::LayoutStyle grow;
            grow.FlexGrow = 1.0f;
            grow.Height = ui::SizeSpec::Match();
            body->AddView(scroll.Get(), grow);
        }
        SetContent(body.Get());

        {
            TemplatesDialog* self = this;
            ui::Button* install = AddButton(u8"Install from Folder...", ui::DialogResult::None);
            install->TooltipText = String(u8"Copy in a template bundle (a folder holding a template.xml)");
            install->OnClick.Add([self](ui::ButtonBase*) { self->InstallFromFolder(); });
            ui::Button* create = AddButton(u8"Create from Build...", ui::DialogResult::None);
            create->TooltipText =
                String(u8"Make a template from a player build folder (Bin/<Config>/<Platform>-<Compiler>)");
            create->OnClick.Add([self](ui::ButtonBase*) { self->CreateFromBuild(); });
        }
        AddButton(u8"Close", ui::DialogResult::Cancel);

        Refresh();
    }

    TemplatesDialog::~TemplatesDialog()
    {
        m_list->SetAdapter(nullptr);
    }

    void TemplatesDialog::BuildDetails(ui::FlexLayout& column)
    {
        IAllocator& allocator = MemoryAllocator();
        {
            auto note = MakeRef<ui::Label>(allocator, StringView(u8"No templates. Install one from a "
                                                                 u8"folder, or create one from a build."));
            m_emptyNote = note.Get();
            note->FontSize.SetValue(12.0f);
            note->WordWrap.SetValue(true);
            note->TextColor.SetValue(Optional<Color>(kDimText));
            ui::LayoutStyle match;
            match.Width = ui::SizeSpec::Match();
            column.AddView(note.Get(), match);
        }

        auto body = MakeRef<ui::FlexLayout>(allocator);
        body->Direction = ui::Orientation::Vertical;
        body->Spacing = 6;
        m_detailBody = body.Get();
        {
            ui::LayoutStyle match;
            match.Width = ui::SizeSpec::Match();
            column.AddView(body.Get(), match);
        }

        // The heading: the icon large, the name over the id.
        {
            auto heading = MakeRef<ui::FlexLayout>(allocator);
            heading->Direction = ui::Orientation::Horizontal;
            heading->Spacing = 12;
            ui::LayoutStyle centred;
            centred.AlignSelf = ui::Align::Center;
            auto icon = MakeRef<ui::DrawableView>(allocator);
            icon->DesiredWidth.SetValue(Optional<f32>(44.0f));
            icon->DesiredHeight.SetValue(Optional<f32>(44.0f));
            m_detailIcon = icon.Get();
            heading->AddView(icon.Get(), centred);
            auto names = MakeRef<ui::FlexLayout>(allocator);
            names->Direction = ui::Orientation::Vertical;
            names->Spacing = 2;
            auto name = MakeRef<ui::Label>(allocator);
            name->FontSize.SetValue(16.0f);
            m_detailName = name.Get();
            names->AddView(name.Get());
            auto id = MakeRef<ui::Label>(allocator);
            id->FontSize.SetValue(11.0f);
            id->TextColor.SetValue(Optional<Color>(kDimText));
            m_detailId = id.Get();
            names->AddView(id.Get());
            ui::LayoutStyle grow;
            grow.FlexGrow = 1.0f;
            grow.AlignSelf = ui::Align::Center;
            heading->AddView(names.Get(), grow);
            m_detailBody->AddView(heading.Get());
        }
        {
            auto rule = MakeRef<ui::Separator>(allocator);
            ui::LayoutStyle line;
            line.Width = ui::SizeSpec::Match();
            line.Height = ui::SizeSpec::Fixed(ui::Unit::Dp(1.0f));
            m_detailBody->AddView(rule.Get(), line);
        }

        (void)AddDetail(*m_detailBody, u8"Platform");
        (void)AddDetail(*m_detailBody, u8"Config");
        (void)AddDetail(*m_detailBody, u8"Compiler");
        {
            ui::Label* engine = AddDetail(*m_detailBody, u8"Engine");
            // The warning sits after the version when the template was built for another.
            auto* row = Cast<ui::FlexLayout>(engine->Parent);
            auto warning = MakeRef<ui::DrawableView>(allocator);
            warning->DesiredWidth.SetValue(Optional<f32>(14.0f));
            warning->DesiredHeight.SetValue(Optional<f32>(14.0f));
            warning->Drawable = ui::DrawablePtr(EditorIcons::Get().warning.Get());
            m_mismatchIcon = warning.Get();
            ui::LayoutStyle centred;
            centred.AlignSelf = ui::Align::Center;
            row->InsertView(warning.Get(), 1, centred);
        }
        (void)AddDetail(*m_detailBody, u8"Source");
        (void)AddDetail(*m_detailBody, u8"Folder");
        (void)AddDetail(*m_detailBody, u8"Player");
        (void)AddDetail(*m_detailBody, u8"Runtime files");
        (void)AddDetail(*m_detailBody, u8"Symbols");
        (void)AddDetail(*m_detailBody, u8"Notes");

        // Its actions.
        auto actions = MakeRef<ui::FlexLayout>(allocator);
        actions->Direction = ui::Orientation::Horizontal;
        actions->Spacing = 8;
        TemplatesDialog* self = this;
        auto reveal = MakeRef<ui::Button>(allocator, StringView(u8"Reveal Folder"));
        reveal->OnClick.Add(
            [self](ui::ButtonBase*)
            {
                const editor::ExportTemplate* tmpl = self->Selected();
                if (tmpl != nullptr && self->m_seams.revealFolder)
                {
                    self->m_seams.revealFolder(tmpl->directory.AsView());
                }
            });
        m_reveal = reveal.Get();
        actions->AddView(reveal.Get());
        auto remove = MakeRef<ui::Button>(allocator, StringView(u8"Remove..."));
        remove->OnClick.Add([self](ui::ButtonBase*) { self->RemoveSelected(); });
        m_remove = remove.Get();
        actions->AddView(remove.Get());
        {
            ui::LayoutStyle top;
            top.Margin = ui::Thickness{0, 6, 0, 0};
            m_detailBody->AddView(actions.Get(), top);
        }
    }

    ui::Label* TemplatesDialog::AddDetail(ui::FlexLayout& column, StringView label)
    {
        IAllocator& allocator = MemoryAllocator();
        auto row = MakeRef<ui::FlexLayout>(allocator);
        row->Direction = ui::Orientation::Horizontal;
        row->Spacing = 8;
        auto key = MakeRef<ui::Label>(allocator, label);
        key->FontSize.SetValue(12.0f);
        key->TextColor.SetValue(Optional<Color>(kDimText));
        ui::LayoutStyle fixed;
        fixed.Width = ui::SizeSpec::Fixed(ui::Unit::Dp(96.0f));
        row->AddView(key.Get(), fixed);
        auto value = MakeRef<ui::Label>(allocator);
        value->FontSize.SetValue(12.0f);
        value->WordWrap.SetValue(true);
        ui::LayoutStyle grow;
        grow.FlexGrow = 1.0f;
        row->AddView(value.Get(), grow);
        ui::LayoutStyle match;
        match.Width = ui::SizeSpec::Match();
        column.AddView(row.Get(), match);
        m_details.PushBack(Detail{String(label), value.Get()});
        return value.Get();
    }

    void TemplatesDialog::SetDetail(StringView label, StringView text)
    {
        for (Detail& detail : m_details)
        {
            if (detail.label.AsView() == label)
            {
                detail.value->SetText(text.IsEmpty() ? StringView(u8"-") : text);
                // A row with nothing to say (no compiler, no notes) folds away.
                detail.value->Parent->Visibility = text.IsEmpty() && (label == u8"Notes" || label == u8"Compiler")
                                                       ? ui::Visibility::Gone
                                                       : ui::Visibility::Visible;
                return;
            }
        }
    }

    StringView TemplatesDialog::DetailText(StringView label) const
    {
        for (const Detail& detail : m_details)
        {
            if (detail.label.AsView() == label)
            {
                return detail.value->Text.Value().AsView();
            }
        }
        return {};
    }

    ui::DrawablePtr TemplatesDialog::IconFor(const editor::ExportTemplate& tmpl)
    {
        String key(tmpl.id.AsView());
        key += u8"|";
        key += tmpl.directory.AsView();
        if (ui::DrawablePtr* known = m_icons.Find(key))
        {
            return *known;
        }
        const String svg = editor::TemplateIconSvg(tmpl, MemoryAllocator());
        ui::DrawablePtr icon(ui::SVGDrawable::FromString(MemoryAllocator(), svg.AsView()).Get());
        m_icons.InsertOrAssign(Move(key), icon);
        return icon;
    }

    const editor::ExportTemplate* TemplatesDialog::Selected() const
    {
        const i32 index = m_list->Selection.FirstSelected();
        return index >= 0 ? TemplateAt(static_cast<usize>(index)) : nullptr;
    }

    void TemplatesDialog::Select(usize index)
    {
        if (index < m_registry.Count())
        {
            m_list->Selection.Select(static_cast<i32>(index));
            m_list->ScrollToPosition(static_cast<i32>(index));
        }
    }

    void TemplatesDialog::Refresh()
    {
        String selectedId;
        if (const editor::ExportTemplate* tmpl = Selected())
        {
            selectedId = tmpl->id;
        }
        if (m_seams.refresh)
        {
            m_seams.refresh(m_registry);
        }
        m_icons.Clear(); // a reinstall may carry a new icon
        m_list->NotifyDataChanged();
        m_list->Selection.ClearSelection();
        usize select = 0;
        for (usize i = 0; i < m_registry.Count(); ++i)
        {
            if (m_registry.At(i)->id == selectedId)
            {
                select = i;
            }
        }
        Select(select);
        ShowDetails();
    }

    void TemplatesDialog::ShowDetails()
    {
        const editor::ExportTemplate* tmpl = Selected();
        m_emptyNote->Visibility = tmpl == nullptr ? ui::Visibility::Visible : ui::Visibility::Gone;
        m_detailBody->Visibility = tmpl == nullptr ? ui::Visibility::Gone : ui::Visibility::Visible;
        if (tmpl == nullptr)
        {
            return;
        }
        m_detailIcon->Drawable = IconFor(*tmpl);
        m_detailName->SetText(tmpl->name.AsView());
        m_detailId->SetText(tmpl->id.AsView());
        SetDetail(u8"Platform", tmpl->platform.AsView());
        SetDetail(u8"Config", tmpl->EffectiveConfig());
        SetDetail(u8"Compiler", tmpl->compiler.AsView());
        const bool matches = editor::TemplateEngineMatches(*tmpl);
        {
            String engine(tmpl->engineVersion.IsEmpty() ? StringView(u8"unstamped") : tmpl->engineVersion.AsView());
            if (!matches)
            {
                engine += u8" (this editor is ";
                engine += editor::kEngineVersionString;
                engine += u8"; an export still runs)";
            }
            SetDetail(u8"Engine", engine.AsView());
            for (Detail& detail : m_details)
            {
                if (detail.label.AsView() == u8"Engine")
                {
                    detail.value->TextColor.SetValue(matches ? Optional<Color>() : Optional<Color>(kWarningText));
                }
            }
            m_mismatchIcon->Visibility = matches ? ui::Visibility::Gone : ui::Visibility::Visible;
        }
        SetDetail(u8"Source", tmpl->isHost ? StringView(u8"This editor's own player build (always available)")
                                           : StringView(u8"Installed in the templates folder"));
        SetDetail(u8"Folder", tmpl->directory.AsView());
        SetDetail(u8"Player", tmpl->playerBinary.AsView());
        {
            String files;
            for (const String& sidecar : tmpl->sidecars)
            {
                files += files.IsEmpty() ? u8"" : u8", ";
                files += sidecar.AsView();
            }
            SetDetail(u8"Runtime files", files.IsEmpty() ? StringView(u8"none") : files.AsView());
        }
        SetDetail(u8"Symbols", tmpl->symbols.IsEmpty()
                                   ? StringView(u8"none")
                                   : Format(u8"{} file(s), staged when a preset asks", tmpl->symbols.Size()).AsView());
        SetDetail(u8"Notes", tmpl->notes.AsView());
        // The host build is this editor's own, never on disk as a bundle: it cannot be removed.
        m_remove->IsEnabled = !tmpl->isHost;
        m_remove->TooltipText = tmpl->isHost ? String(u8"This editor's own build cannot be removed")
                                             : String(u8"Delete this template's folder");
        m_reveal->IsEnabled = !tmpl->directory.IsEmpty();
        Invalidate();
    }

    void TemplatesDialog::InstallFromFolder()
    {
        if (!m_seams.pickFolder)
        {
            return;
        }
        TemplatesDialog* self = this;
        m_seams.pickFolder(
            [self](String folder)
            {
                const String root = self->m_seams.templatesRoot ? self->m_seams.templatesRoot() : String();
                String id;
                if (editor::ImportTemplate(folder.AsView(), root.AsView(), &id).IsOk())
                {
                    self->m_context->Notify(editor::NoticeKind::Success,
                                            Format(u8"Installed template '{}'.", id.AsView()).AsView());
                    self->Refresh();
                    for (usize i = 0; i < self->m_registry.Count(); ++i)
                    {
                        if (self->m_registry.At(i)->id == id)
                        {
                            self->Select(i);
                        }
                    }
                }
                else
                {
                    self->m_context->Notify(editor::NoticeKind::Error,
                                            u8"That folder holds no template (no valid template.xml).");
                }
            });
    }

    void TemplatesDialog::CreateFromBuild()
    {
        if (!m_seams.pickFolder)
        {
            return;
        }
        TemplatesDialog* self = this;
        m_seams.pickFolder(
            [self](String folder)
            {
                const String root = self->m_seams.templatesRoot ? self->m_seams.templatesRoot() : String();
                String id, dir;
                if (editor::CreateTemplate(folder.AsView(), root.AsView(), editor::TemplateOutput::Install, &id,
                                           &dir)
                        .IsOk())
                {
                    self->m_context->Notify(editor::NoticeKind::Success,
                                            Format(u8"Created template '{}'.", id.AsView()).AsView());
                    self->Refresh();
                    for (usize i = 0; i < self->m_registry.Count(); ++i)
                    {
                        if (self->m_registry.At(i)->id == id)
                        {
                            self->Select(i);
                        }
                    }
                }
                else
                {
                    self->m_context->Notify(editor::NoticeKind::Error,
                                            u8"No player build there: pick a Bin/<Config>/<Platform>-<Compiler> "
                                            u8"folder holding Engine.Player.");
                }
            });
    }

    void TemplatesDialog::RemoveSelected(bool confirmed)
    {
        const editor::ExportTemplate* tmpl = Selected();
        if (tmpl == nullptr || tmpl->isHost)
        {
            return;
        }
        const String id(tmpl->id.AsView());
        if (!confirmed)
        {
            if (Context == nullptr)
            {
                return;
            }
            const StringView choices[] = {u8"Remove", u8"Cancel"};
            auto confirm = MakeRef<ConfirmDialog>(
                MemoryAllocator(), StringView(u8"Remove Template"),
                Format(u8"Remove '{}'? Its folder is deleted; presets that pick it fall back to another "
                       u8"template for their platform.",
                       tmpl->name.AsView())
                    .AsView(),
                Span<const StringView>(choices, 2));
            TemplatesDialog* self = this;
            confirm->OnChosen = [self](usize choice)
            {
                if (choice == 0)
                {
                    self->RemoveSelected(true);
                }
            };
            confirm->Show(Context);
            return;
        }
        const String root = m_seams.templatesRoot ? m_seams.templatesRoot() : String();
        if (editor::RemoveTemplate(root.AsView(), id.AsView()).IsOk())
        {
            m_context->Notify(editor::NoticeKind::Success, Format(u8"Removed template '{}'.", id.AsView()).AsView());
        }
        else
        {
            m_context->Notify(editor::NoticeKind::Error, u8"Removing the template failed (see the Console).");
        }
        Refresh();
    }
}
