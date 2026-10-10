// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::App - :export_dialog partition (implementation). See ExportDialog.cppm.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

module editor.app;

import foundation.core;
import foundation.ui;
import engine.project; // the display enums (ProjectSettings' renderFit and windowMode)
import editor.core;
import :editor_icons;
import :category_tabs;

using namespace foundation::core;
namespace ui = foundation::ui;

namespace editor::app
{
    namespace
    {
        constexpr Color kDimText{0.62f, 0.62f, 0.62f, 1.0f};
        constexpr Color kWarningText{0.91f, 0.69f, 0.29f, 1.0f};
        constexpr StringView kDot = u8" · ";

        /// The extra files as the field shows them, ";"-separated (an EditText holds no list).
        String JoinFiles(const Array<String>& files)
        {
            String out;
            for (const String& file : files)
            {
                out += out.IsEmpty() ? u8"" : u8"; ";
                out += file.AsView();
            }
            return out;
        }

        void SplitFiles(StringView text, Array<String>& out)
        {
            out.Clear();
            usize start = 0;
            for (usize i = 0; i <= text.Size(); ++i)
            {
                if (i != text.Size() && text[i] != utf8char(';'))
                {
                    continue;
                }
                usize s = start, e = i;
                while (s < e && (text[s] == utf8char(' ') || text[s] == utf8char('\t')))
                {
                    ++s;
                }
                while (e > s && (text[e - 1] == utf8char(' ') || text[e - 1] == utf8char('\t')))
                {
                    --e;
                }
                if (e > s)
                {
                    out.PushBack(String(text.SubStr(s, e - s)));
                }
                start = i + 1;
            }
        }

        [[nodiscard]] StringView ConfigOf(const editor::ExportPreset& preset)
        {
            return preset.config.IsEmpty() ? StringView(u8"Release") : preset.config.AsView();
        }

        /// The enum type of one of ProjectSettings' properties (the display choices).
        [[nodiscard]] const TypeInfo& SettingEnum(const char* property)
        {
            return *FindProperty(engine::project::ProjectSettings::StaticType(), property)->type;
        }

        [[nodiscard]] RefPtr<ui::ComboBox> EnumCombo(IAllocator& allocator, const TypeInfo& type)
        {
            auto combo = MakeRef<ui::ComboBox>(allocator);
            for (usize i = 0; i < EnumeratorCount(type); ++i)
            {
                (void)combo->AddItem(StringView(reinterpret_cast<const utf8char*>(EnumeratorAt(type, i).name)));
            }
            return combo;
        }

        void SelectEnum(ui::ComboBox& combo, const TypeInfo& type, i64 value)
        {
            for (usize i = 0; i < EnumeratorCount(type); ++i)
            {
                if (EnumeratorAt(type, i).value == value)
                {
                    combo.SetSelectedIndex(static_cast<i32>(i));
                }
            }
        }

        [[nodiscard]] i64 ChosenEnum(const ui::ComboBox& combo, const TypeInfo& type)
        {
            const i32 index = combo.SelectedIndex();
            return (index >= 0 && static_cast<usize>(index) < EnumeratorCount(type))
                       ? EnumeratorAt(type, static_cast<usize>(index)).value
                       : 0;
        }

        [[nodiscard]] RefPtr<ui::NumericField> SizeField(IAllocator& allocator, f64 least)
        {
            auto field = MakeRef<ui::NumericField>(allocator);
            field->SetDecimalPlaces(0);
            field->SetStep(1.0);
            field->SetMin(least);
            field->SetMax(16384.0);
            return field;
        }
    }

    // A preset's row: the icon of the template it resolves to (a warning when none does), its name
    // over its platform and config.
    class ExportDialog::Adapter final : public ui::ListAdapterBase
    {
    public:
        explicit Adapter(ExportDialog& owner) : m_owner(&owner) {}

        [[nodiscard]] i32 ItemCount() const override { return static_cast<i32>(m_owner->m_presets->Count()); }

        [[nodiscard]] RefPtr<ui::View> CreateView(i32) override
        {
            IAllocator& allocator = m_owner->MemoryAllocator();
            auto row = MakeRef<ui::FlexLayout>(allocator);
            row->Direction = ui::Orientation::Horizontal;
            row->Spacing = 8;
            row->Padding = ui::Thickness{6, 4};
            auto icon = MakeRef<ui::DrawableView>(allocator);
            icon->DesiredWidth.SetValue(Optional<f32>(24.0f));
            icon->DesiredHeight.SetValue(Optional<f32>(24.0f));
            ui::LayoutStyle centred;
            centred.AlignSelf = ui::Align::Center;
            row->AddView(icon.Get(), centred);
            auto text = MakeRef<ui::FlexLayout>(allocator);
            text->Direction = ui::Orientation::Vertical;
            text->Spacing = 1;
            auto name = MakeRef<ui::Label>(allocator);
            name->FontSize.SetValue(12.5f);
            text->AddView(name.Get());
            auto summary = MakeRef<ui::Label>(allocator);
            summary->FontSize.SetValue(11.0f);
            text->AddView(summary.Get());
            ui::LayoutStyle grow;
            grow.FlexGrow = 1.0f;
            grow.AlignSelf = ui::Align::Center;
            row->AddView(text.Get(), grow);
            return RefPtr<ui::View>(row.Get());
        }

        void BindView(ui::View* view, i32 position) override
        {
            auto* row = Cast<ui::FlexLayout>(view);
            if (row == nullptr || row->ChildCount() < 2 || position < 0 ||
                static_cast<usize>(position) >= m_owner->m_presets->Count())
            {
                return;
            }
            const editor::ExportPreset& preset = m_owner->m_presets->At(static_cast<usize>(position));
            const editor::ExportTemplate* tmpl = m_owner->m_registry.Resolve(preset);
            Cast<ui::DrawableView>(row->GetChildAt(0))->Drawable =
                tmpl != nullptr ? m_owner->IconFor(*tmpl) : ui::DrawablePtr(EditorIcons::Get().warning.Get());
            auto* text = Cast<ui::FlexLayout>(row->GetChildAt(1));
            Cast<ui::Label>(text->GetChildAt(0))->SetText(preset.name.AsView());
            String summary(preset.platform.IsEmpty() ? StringView(u8"?") : preset.platform.AsView());
            summary += kDot;
            summary += ConfigOf(preset);
            if (tmpl == nullptr)
            {
                summary += kDot;
                summary += u8"no template here";
            }
            auto* line = Cast<ui::Label>(text->GetChildAt(1));
            line->SetText(summary.AsView());
            line->TextColor.SetValue(Optional<Color>(tmpl != nullptr ? kDimText : kWarningText));
        }

    private:
        ExportDialog* m_owner;
    };

    ExportDialog::ExportDialog(editor::EditorContext& context, editor::ExportPresetsController& presets,
                               ExportDialogSeams seams)
        : ui::Dialog(u8"Export"), m_context(&context), m_presets(&presets), m_seams(Move(seams))
    {
        // A fixed size: the tabs measure only the page they show.
        MinWidth.SetValue(820.0f);
        MaxWidth.SetValue(820.0f);
        MinHeight.SetValue(520.0f);
        MaxHeight.SetValue(520.0f);
        if (m_seams.refresh)
        {
            m_seams.refresh(m_registry);
        }
        IAllocator& allocator = MemoryAllocator();
        ExportDialog* self = this;

        auto body = MakeRef<ui::FlexLayout>(allocator);
        body->Direction = ui::Orientation::Horizontal;

        // --- The presets ---
        auto side = MakeRef<ui::FlexLayout>(allocator);
        side->Direction = ui::Orientation::Vertical;
        side->Spacing = 4;
        side->Padding = ui::Thickness{6, 6};
        {
            auto bar = MakeRef<ui::FlexLayout>(allocator);
            bar->Direction = ui::Orientation::Horizontal;
            bar->Spacing = 2;
            auto heading = MakeRef<ui::Label>(allocator, StringView(u8"Presets"));
            heading->FontSize.SetValue(12.0f);
            heading->TextColor.SetValue(Optional<Color>(kDimText));
            ui::LayoutStyle grow;
            grow.FlexGrow = 1.0f;
            grow.AlignSelf = ui::Align::Center;
            bar->AddView(heading.Get(), grow);
            EditorIcons& icons = EditorIcons::Get();
            const auto iconButton = [&](ui::SVGDrawable* icon, StringView tip)
            {
                auto button = MakeRef<ui::IconButton>(allocator, icon, 14.0f);
                button->TooltipText = String(tip);
                ui::LayoutStyle centred;
                centred.AlignSelf = ui::Align::Center;
                bar->AddView(button.Get(), centred);
                return button;
            };
            iconButton(icons.add.Get(), u8"Add a preset for this platform")
                ->OnClick.Add([self](ui::ButtonBase*) { self->AddPreset(); });
            auto duplicate = iconButton(icons.copy.Get(), u8"Duplicate the preset");
            duplicate->OnClick.Add([self](ui::ButtonBase*) { self->DuplicateSelected(); });
            m_duplicate = duplicate.Get();
            auto remove = iconButton(icons.remove.Get(), u8"Remove the preset");
            remove->OnClick.Add([self](ui::ButtonBase*) { self->RemoveSelected(); });
            m_remove = remove.Get();
            ui::LayoutStyle match;
            match.Width = ui::SizeSpec::Match();
            side->AddView(bar.Get(), match);
        }
        m_adapter = MakeUnique<Adapter>(allocator, *this);
        m_list = MakeRef<ui::ListView>(allocator);
        m_list->ItemHeight.SetValue(40.0f);
        m_list->SetAdapter(m_adapter.Get());
        m_list->Selection.OnSelectionChanged.Add(
            [self]()
            {
                self->Save(); // what was edited stays edited
                self->LoadFields();
            });
        {
            ui::LayoutStyle grow;
            grow.FlexGrow = 1.0f;
            grow.Width = ui::SizeSpec::Match();
            side->AddView(m_list.Get(), grow);
        }
        if (!m_seams.outputRoot.IsEmpty())
        {
            auto where = MakeRef<ui::Label>(allocator, Format(u8"Exports go to {}", m_seams.outputRoot.AsView()).AsView());
            where->FontSize.SetValue(10.5f);
            where->WordWrap.SetValue(true);
            where->TextColor.SetValue(Optional<Color>(kDimText));
            ui::LayoutStyle match;
            match.Width = ui::SizeSpec::Match();
            side->AddView(where.Get(), match);
        }
        {
            ui::LayoutStyle fixed;
            fixed.Width = ui::SizeSpec::Fixed(ui::Unit::Dp(250.0f));
            fixed.Height = ui::SizeSpec::Match();
            body->AddView(side.Get(), fixed);
        }
        {
            auto rule = MakeRef<ui::Separator>(allocator);
            rule->Orientation.SetValue(ui::Orientation::Vertical);
            ui::LayoutStyle line;
            line.Width = ui::SizeSpec::Fixed(ui::Unit::Dp(1.0f));
            line.Height = ui::SizeSpec::Match();
            body->AddView(rule.Get(), line);
        }

        // --- The selected preset: its template card over its settings' tabs ---
        auto detail = MakeRef<ui::FlexLayout>(allocator);
        detail->Direction = ui::Orientation::Vertical;
        detail->Spacing = 6;
        detail->Padding = ui::Thickness{10, 8};
        BuildResolveCard(*detail);
        m_tabs = MakeUnique<CategoryTabs>(allocator, allocator);
        BuildGeneral(m_tabs->Column(u8"General"));
        BuildContent(m_tabs->Column(u8"Content"));
        BuildDisplay(m_tabs->Column(u8"Display"));
        {
            ui::LayoutStyle grow;
            grow.FlexGrow = 1.0f;
            grow.Width = ui::SizeSpec::Match();
            detail->AddView(&m_tabs->View(), grow);
        }
        {
            ui::LayoutStyle grow;
            grow.FlexGrow = 1.0f;
            grow.Height = ui::SizeSpec::Match();
            body->AddView(detail.Get(), grow);
        }
        SetContent(body.Get());

        ui::Button* templates = AddButton(u8"Export Templates...", ui::DialogResult::None);
        templates->OnClick.Add(
            [self](ui::ButtonBase*)
            {
                if (!self->m_seams.openTemplates)
                {
                    return;
                }
                if (ui::Dialog* opened = self->m_seams.openTemplates())
                {
                    // What it installed or removed shows here once it closes.
                    opened->OnClosed.Add([self](ui::Dialog*, ui::DialogResult) { self->RefreshTemplates(); });
                }
            });
        ui::Button* all = AddButton(u8"Export All", ui::DialogResult::None);
        all->OnClick.Add([self](ui::ButtonBase*) { self->ExportAll(); });
        m_export = AddButton(u8"Export", ui::DialogResult::None);
        m_export->OnClick.Add([self](ui::ButtonBase*) { self->ExportSelected(); });
        AddButton(u8"Close", ui::DialogResult::Cancel);
        OnClosed.Add([self](ui::Dialog*, ui::DialogResult) { self->Save(); });

        if (m_presets->Count() > 0)
        {
            Select(0);
        }
        LoadFields();
    }

    ExportDialog::~ExportDialog()
    {
        m_list->SetAdapter(nullptr);
    }

    ui::FlexLayout* ExportDialog::AddRow(ui::FlexLayout& column, StringView label, ui::View* field)
    {
        IAllocator& allocator = MemoryAllocator();
        auto row = MakeRef<ui::FlexLayout>(allocator);
        row->Direction = ui::Orientation::Horizontal;
        row->Spacing = 8;
        auto text = MakeRef<ui::Label>(allocator, label);
        text->FontSize.SetValue(12.0f);
        ui::LayoutStyle fixed;
        fixed.Width = ui::SizeSpec::Fixed(ui::Unit::Dp(110.0f));
        fixed.AlignSelf = ui::Align::Center;
        row->AddView(text.Get(), fixed);
        ui::LayoutStyle grow;
        grow.FlexGrow = 1.0f;
        grow.AlignSelf = ui::Align::Center;
        row->AddView(field, grow);
        ui::LayoutStyle match;
        match.Width = ui::SizeSpec::Match();
        column.AddView(row.Get(), match);
        return row.Get();
    }

    void ExportDialog::BuildResolveCard(ui::FlexLayout& column)
    {
        IAllocator& allocator = MemoryAllocator();
        auto card = MakeRef<ui::FlexLayout>(allocator);
        card->Direction = ui::Orientation::Horizontal;
        card->Spacing = 10;
        card->Padding = ui::Thickness{10, 8};
        auto icon = MakeRef<ui::DrawableView>(allocator);
        icon->DesiredWidth.SetValue(Optional<f32>(34.0f));
        icon->DesiredHeight.SetValue(Optional<f32>(34.0f));
        m_resolveIcon = icon.Get();
        ui::LayoutStyle centred;
        centred.AlignSelf = ui::Align::Center;
        card->AddView(icon.Get(), centred);
        auto text = MakeRef<ui::FlexLayout>(allocator);
        text->Direction = ui::Orientation::Vertical;
        text->Spacing = 2;
        auto title = MakeRef<ui::Label>(allocator);
        title->FontSize.SetValue(13.0f);
        m_resolveTitle = title.Get();
        text->AddView(title.Get());
        auto line = MakeRef<ui::Label>(allocator);
        line->FontSize.SetValue(11.0f);
        line->WordWrap.SetValue(true);
        m_resolveDetail = line.Get();
        ui::LayoutStyle match;
        match.Width = ui::SizeSpec::Match();
        text->AddView(line.Get(), match);
        ui::LayoutStyle grow;
        grow.FlexGrow = 1.0f;
        grow.AlignSelf = ui::Align::Center;
        card->AddView(text.Get(), grow);
        // A quiet rounded panel behind it (a FlexLayout draws no background of its own).
        auto panel = MakeRef<ui::Panel>(allocator);
        panel->SetStyle(ui::StyleProperty::Background,
                        ui::DrawablePtr(MakeRef<ui::RoundedRectDrawable>(
                                            allocator, Color{1.0f, 1.0f, 1.0f, 0.04f}, 6.0f,
                                            Color{1.0f, 1.0f, 1.0f, 0.10f}, 1.0f)
                                            .Get()));
        panel->AddView(card.Get());
        column.AddView(panel.Get(), match);
    }

    void ExportDialog::BuildGeneral(ui::FlexLayout& column)
    {
        IAllocator& allocator = MemoryAllocator();
        ExportDialog* self = this;
        const auto stored = [self]() { self->StoreFields(); };

        auto name = MakeRef<ui::EditText>(allocator);
        m_name = name.Get();
        name->OnTextChanged.Add([stored](ui::EditText*) { stored(); });
        AddRow(column, u8"Name", name.Get());

        auto tmpl = MakeRef<ui::ComboBox>(allocator);
        m_template = tmpl.Get();
        tmpl->OnSelectionChanged.Add([stored](ui::ComboBox*, i32) { stored(); });
        AddRow(column, u8"Template", tmpl.Get());

        auto platform = MakeRef<ui::ComboBox>(allocator);
        m_platform = platform.Get();
        platform->OnSelectionChanged.Add([stored](ui::ComboBox*, i32) { stored(); });
        AddRow(column, u8"Platform", platform.Get());

        auto config = MakeRef<ui::ComboBox>(allocator);
        m_config = config.Get();
        config->OnSelectionChanged.Add([stored](ui::ComboBox*, i32) { stored(); });
        AddRow(column, u8"Config", config.Get());
        CategoryTabs::AddNote(column, u8"A template picked by name decides the platform and config; "
                                      u8"\"Any\" takes the best installed for the platform and config.");

        auto player = MakeRef<ui::EditText>(allocator);
        player->SetPlaceholder(u8"the template's (Engine.Player)");
        m_player = player.Get();
        player->OnTextChanged.Add([stored](ui::EditText*) { stored(); });
        AddRow(column, u8"Player name", player.Get());

        auto subdir = MakeRef<ui::EditText>(allocator);
        subdir->SetPlaceholder(u8"the preset's name");
        m_subdir = subdir.Get();
        subdir->OnTextChanged.Add([stored](ui::EditText*) { stored(); });
        AddRow(column, u8"Output folder", subdir.Get());
    }

    void ExportDialog::BuildContent(ui::FlexLayout& column)
    {
        IAllocator& allocator = MemoryAllocator();
        ExportDialog* self = this;
        const auto stored = [self]() { self->StoreFields(); };

        auto files = MakeRef<ui::EditText>(allocator);
        files->SetPlaceholder(u8"none (separate files with ;)");
        m_files = files.Get();
        files->OnTextChanged.Add([stored](ui::EditText*) { stored(); });
        ui::FlexLayout* row = AddRow(column, u8"Extra files", files.Get());
        auto browse = MakeRef<ui::Button>(allocator, StringView(u8"Add Files..."));
        browse->OnClick.Add(
            [self](ui::ButtonBase*)
            {
                if (!self->m_seams.pickFiles)
                {
                    return;
                }
                self->m_seams.pickFiles(
                    [self](Array<String> paths)
                    {
                        Array<String> files;
                        SplitFiles(self->m_files->Text(), files);
                        for (String& path : paths)
                        {
                            files.PushBack(Move(path));
                        }
                        self->m_files->SetText(JoinFiles(files).AsView());
                        self->StoreFields();
                    });
            });
        ui::LayoutStyle centred;
        centred.AlignSelf = ui::Align::Center;
        row->AddView(browse.Get(), centred);
        CategoryTabs::AddNote(column, u8"Copied beside the player, as they are (a licence, a readme).");

        auto symbols = MakeRef<ui::CheckBox>(allocator, StringView(u8"Ship the template's debug symbols"), false);
        m_symbols = symbols.Get();
        symbols->OnCheckedChanged.Add([stored](ui::CheckBox*, bool) { stored(); });
        column.AddView(symbols.Get());
        auto prune = MakeRef<ui::CheckBox>(allocator, StringView(u8"Ship only the content the game reaches"), false);
        prune->TooltipText = String(u8"Pack the closure of the default scene, the startup script and the other "
                                    u8"settings' assets, not every cooked asset");
        m_prune = prune.Get();
        prune->OnCheckedChanged.Add([stored](ui::CheckBox*, bool) { stored(); });
        column.AddView(prune.Get());
    }

    void ExportDialog::BuildDisplay(ui::FlexLayout& column)
    {
        IAllocator& allocator = MemoryAllocator();
        ExportDialog* self = this;
        const auto stored = [self]() { self->StoreFields(); };
        const auto pair = [&allocator](ui::View* a, ui::View* b)
        {
            auto row = MakeRef<ui::FlexLayout>(allocator);
            row->Direction = ui::Orientation::Horizontal;
            row->Spacing = 6;
            ui::LayoutStyle fixed;
            fixed.Width = ui::SizeSpec::Fixed(ui::Unit::Dp(90));
            row->AddView(a, fixed);
            row->AddView(MakeRef<ui::Label>(allocator, StringView(u8"x")).Get());
            row->AddView(b, fixed);
            return row;
        };
        CategoryTabs::AddNote(column, u8"This platform's own display, over the project's (a handheld's "
                                      u8"panel, fullscreen on a device).");

        auto overridesRender =
            MakeRef<ui::CheckBox>(allocator, StringView(u8"Draw at its own render size"), false);
        m_overridesRender = overridesRender.Get();
        overridesRender->OnCheckedChanged.Add([stored](ui::CheckBox*, bool) { stored(); });
        column.AddView(overridesRender.Get());
        auto renderWidth = SizeField(allocator, 0.0);
        auto renderHeight = SizeField(allocator, 0.0);
        m_renderWidth = renderWidth.Get();
        m_renderHeight = renderHeight.Get();
        renderWidth->OnValueChanged.Add([stored](ui::NumericField*, f64) { stored(); });
        renderHeight->OnValueChanged.Add([stored](ui::NumericField*, f64) { stored(); });
        AddRow(column, u8"Render size", pair(renderWidth.Get(), renderHeight.Get()).Get());
        auto renderFit = EnumCombo(allocator, SettingEnum("renderFit"));
        m_renderFit = renderFit.Get();
        renderFit->OnSelectionChanged.Add([stored](ui::ComboBox*, i32) { stored(); });
        AddRow(column, u8"Render fit", renderFit.Get());

        auto overridesWindow = MakeRef<ui::CheckBox>(allocator, StringView(u8"Open its own window"), false);
        m_overridesWindow = overridesWindow.Get();
        overridesWindow->OnCheckedChanged.Add([stored](ui::CheckBox*, bool) { stored(); });
        column.AddView(overridesWindow.Get());
        auto windowWidth = SizeField(allocator, 1.0);
        auto windowHeight = SizeField(allocator, 1.0);
        m_windowWidth = windowWidth.Get();
        m_windowHeight = windowHeight.Get();
        windowWidth->OnValueChanged.Add([stored](ui::NumericField*, f64) { stored(); });
        windowHeight->OnValueChanged.Add([stored](ui::NumericField*, f64) { stored(); });
        AddRow(column, u8"Window size", pair(windowWidth.Get(), windowHeight.Get()).Get());
        auto windowMode = EnumCombo(allocator, SettingEnum("windowMode"));
        m_windowMode = windowMode.Get();
        windowMode->OnSelectionChanged.Add([stored](ui::ComboBox*, i32) { stored(); });
        AddRow(column, u8"Window mode", windowMode.Get());
        auto resizable = MakeRef<ui::CheckBox>(allocator, StringView(u8"Resizable"), false);
        m_windowResizable = resizable.Get();
        resizable->OnCheckedChanged.Add([stored](ui::CheckBox*, bool) { stored(); });
        column.AddView(resizable.Get());
    }

    ui::DrawablePtr ExportDialog::IconFor(const editor::ExportTemplate& tmpl)
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

    i32 ExportDialog::SelectedIndex() const
    {
        const i32 index = m_list->Selection.FirstSelected();
        return (index >= 0 && static_cast<usize>(index) < m_presets->Count()) ? index : -1;
    }

    void ExportDialog::Select(usize index)
    {
        if (index < m_presets->Count())
        {
            m_list->Selection.Select(static_cast<i32>(index));
            m_list->ScrollToPosition(static_cast<i32>(index));
        }
    }

    const editor::ExportTemplate* ExportDialog::Resolved() const
    {
        const i32 index = SelectedIndex();
        return index >= 0 ? m_registry.Resolve(m_presets->At(static_cast<usize>(index))) : nullptr;
    }

    StringView ExportDialog::ResolveText() const
    {
        return m_resolveTitle->Text.Value().AsView();
    }

    void ExportDialog::FillChoices(ui::ComboBox& combo, StringView current, StringView fallback, bool platforms)
    {
        // The values the templates here offer, the preset's own, and the fallback (the host's
        // platform, Release): a platform with no template stays choosable, the card says so.
        Array<String> values;
        const auto add = [&values](StringView value)
        {
            if (value.IsEmpty())
            {
                return;
            }
            for (const String& known : values)
            {
                if (known.AsView() == value)
                {
                    return;
                }
            }
            values.PushBack(String(value));
        };
        add(current);
        add(fallback);
        for (usize i = 0; i < m_registry.Count(); ++i)
        {
            add(platforms ? m_registry.At(i)->platform.AsView() : m_registry.At(i)->EffectiveConfig());
        }
        combo.ClearItems();
        i32 selected = 0;
        for (const String& value : values)
        {
            const i32 index = combo.AddItem(value.AsView());
            selected = value.AsView() == (current.IsEmpty() ? fallback : current) ? index : selected;
        }
        combo.SetSelectedIndex(selected);
    }

    void ExportDialog::FillTemplateChoices()
    {
        m_template->ClearItems();
        m_templateIds.Clear();
        (void)m_template->AddItem(u8"Any for its platform and config");
        for (usize i = 0; i < m_registry.Count(); ++i)
        {
            const editor::ExportTemplate* tmpl = m_registry.At(i);
            String item(tmpl->name.AsView());
            if (tmpl->isHost)
            {
                item += u8" (this editor's own)";
            }
            (void)m_template->AddItem(item.AsView());
            m_templateIds.PushBack(String(tmpl->id.AsView()));
        }
    }

    void ExportDialog::LoadFields()
    {
        const i32 index = SelectedIndex();
        m_loading = true;
        const bool any = index >= 0;
        m_tabs->View().IsEnabled = any;
        m_export->IsEnabled = any;
        m_duplicate->IsEnabled = any;
        m_remove->IsEnabled = any;
        FillTemplateChoices();
        if (any)
        {
            const editor::ExportPreset& preset = m_presets->At(static_cast<usize>(index));
            m_name->SetText(preset.name.AsView());
            i32 chosen = 0;
            for (usize i = 0; i < m_templateIds.Size(); ++i)
            {
                chosen = m_templateIds[i] == preset.templateId ? static_cast<i32>(i) + 1 : chosen;
            }
            m_template->SetSelectedIndex(chosen);
            FillChoices(*m_platform, preset.platform.AsView(), m_seams.hostPlatform.AsView(), true);
            FillChoices(*m_config, preset.config.AsView(), u8"Release", false);
            // A template picked by name decides both.
            m_platform->IsEnabled = chosen == 0;
            m_config->IsEnabled = chosen == 0;
            m_player->SetText(preset.playerName.AsView());
            m_subdir->SetText(preset.outputSubdir.AsView());
            m_files->SetText(JoinFiles(preset.additionalFiles).AsView());
            m_symbols->IsChecked.SetValue(preset.stageSymbols);
            m_prune->IsChecked.SetValue(preset.pruneToReachable);
            m_overridesRender->IsChecked.SetValue(preset.overridesRender);
            m_renderWidth->SetValue(static_cast<f64>(preset.renderWidth));
            m_renderHeight->SetValue(static_cast<f64>(preset.renderHeight));
            SelectEnum(*m_renderFit, SettingEnum("renderFit"), static_cast<i64>(preset.renderFit));
            m_overridesWindow->IsChecked.SetValue(preset.overridesWindow);
            m_windowWidth->SetValue(static_cast<f64>(preset.windowWidth));
            m_windowHeight->SetValue(static_cast<f64>(preset.windowHeight));
            SelectEnum(*m_windowMode, SettingEnum("windowMode"), static_cast<i64>(preset.windowMode));
            m_windowResizable->IsChecked.SetValue(preset.windowResizable);
        }
        m_loading = false;
        UpdateResolve();
    }

    void ExportDialog::StoreFields()
    {
        const i32 index = SelectedIndex();
        if (m_loading || index < 0)
        {
            return;
        }
        editor::ExportPreset preset = m_presets->At(static_cast<usize>(index));
        preset.name = String(m_name->Text());
        const i32 chosen = m_template->SelectedIndex();
        if (chosen > 0 && static_cast<usize>(chosen) <= m_templateIds.Size())
        {
            // A template by name: its platform and config, shown in their (now fixed) fields.
            const editor::ExportTemplate* tmpl = m_registry.FindById(m_templateIds[static_cast<usize>(chosen) - 1].AsView());
            preset.templateId = m_templateIds[static_cast<usize>(chosen) - 1];
            if (tmpl != nullptr)
            {
                preset.platform = tmpl->platform;
                preset.config = String(tmpl->EffectiveConfig());
            }
        }
        else
        {
            preset.templateId = String();
            preset.platform = String(m_platform->SelectedText());
            preset.config = String(m_config->SelectedText());
        }
        preset.playerName = String(m_player->Text());
        preset.outputSubdir = String(m_subdir->Text());
        SplitFiles(m_files->Text(), preset.additionalFiles);
        preset.stageSymbols = m_symbols->IsChecked.Value();
        preset.pruneToReachable = m_prune->IsChecked.Value();
        preset.overridesRender = m_overridesRender->IsChecked.Value();
        preset.renderWidth = static_cast<u32>(m_renderWidth->Value());
        preset.renderHeight = static_cast<u32>(m_renderHeight->Value());
        preset.renderFit = static_cast<FitMode>(ChosenEnum(*m_renderFit, SettingEnum("renderFit")));
        preset.overridesWindow = m_overridesWindow->IsChecked.Value();
        preset.windowWidth = static_cast<u32>(m_windowWidth->Value());
        preset.windowHeight = static_cast<u32>(m_windowHeight->Value());
        preset.windowMode =
            static_cast<engine::project::WindowMode>(ChosenEnum(*m_windowMode, SettingEnum("windowMode")));
        preset.windowResizable = m_windowResizable->IsChecked.Value();
        m_presets->Update(static_cast<usize>(index), preset);
        m_dirty = true;

        // The platform and config follow a template picked by name.
        m_loading = true;
        const editor::ExportPreset& stored = m_presets->At(static_cast<usize>(index));
        FillChoices(*m_platform, stored.platform.AsView(), m_seams.hostPlatform.AsView(), true);
        FillChoices(*m_config, stored.config.AsView(), u8"Release", false);
        m_platform->IsEnabled = chosen <= 0;
        m_config->IsEnabled = chosen <= 0;
        m_loading = false;
        m_list->OnItemRangeChanged(index, 1);
        UpdateResolve();
    }

    void ExportDialog::UpdateResolve()
    {
        const i32 index = SelectedIndex();
        if (index < 0)
        {
            m_resolveIcon->Drawable = ui::DrawablePtr{};
            m_resolveTitle->SetText(u8"No presets yet");
            m_resolveDetail->SetText(u8"Add one with + for this platform.");
            m_resolveDetail->TextColor.SetValue(Optional<Color>(kDimText));
            return;
        }
        const editor::ExportPreset& preset = m_presets->At(static_cast<usize>(index));
        const editor::ExportTemplate* tmpl = m_registry.Resolve(preset);
        if (tmpl == nullptr)
        {
            m_resolveIcon->Drawable = ui::DrawablePtr(EditorIcons::Get().warning.Get());
            if (!preset.templateId.IsEmpty() && m_registry.FindById(preset.templateId.AsView()) == nullptr)
            {
                m_resolveTitle->SetText(
                    Format(u8"Template '{}' is not installed here", preset.templateId.AsView()).AsView());
            }
            else
            {
                m_resolveTitle->SetText(Format(u8"No template for {} {} here",
                                               preset.platform.IsEmpty() ? StringView(u8"?") : preset.platform.AsView(),
                                               ConfigOf(preset))
                                            .AsView());
            }
            m_resolveDetail->SetText(u8"Install or create one in Export Templates, or pick another platform.");
            m_resolveDetail->TextColor.SetValue(Optional<Color>(kWarningText));
            m_export->IsEnabled = false;
            return;
        }
        m_resolveIcon->Drawable = IconFor(*tmpl);
        m_resolveTitle->SetText(Format(u8"Exports with {}", tmpl->name.AsView()).AsView());
        String detail(tmpl->platform.AsView());
        detail += kDot;
        detail += tmpl->EffectiveConfig();
        if (!tmpl->engineVersion.IsEmpty())
        {
            detail += kDot;
            detail += tmpl->engineVersion.AsView();
        }
        detail += kDot;
        detail += tmpl->isHost ? StringView(u8"this editor's own player") : StringView(u8"installed");
        const bool matches = editor::TemplateEngineMatches(*tmpl);
        if (!matches)
        {
            detail += u8". Built for another engine version; the export still runs.";
        }
        m_resolveDetail->SetText(detail.AsView());
        m_resolveDetail->TextColor.SetValue(Optional<Color>(matches ? kDimText : kWarningText));
        m_export->IsEnabled = true;
    }

    void ExportDialog::Save()
    {
        if (m_dirty && m_seams.save)
        {
            m_seams.save();
        }
        m_dirty = false;
    }

    void ExportDialog::AddPreset()
    {
        editor::ExportPreset fresh;
        fresh.name = String(u8"New Preset");
        fresh.platform = m_seams.hostPlatform;
        const usize index = m_presets->Add(fresh);
        m_dirty = true;
        Save();
        m_list->NotifyDataChanged();
        Select(index);
        LoadFields();
    }

    void ExportDialog::DuplicateSelected()
    {
        const i32 index = SelectedIndex();
        if (index < 0)
        {
            return;
        }
        const usize copy = m_presets->Duplicate(static_cast<usize>(index));
        m_dirty = true;
        Save();
        m_list->NotifyDataChanged();
        Select(copy);
        LoadFields();
    }

    void ExportDialog::RemoveSelected()
    {
        const i32 index = SelectedIndex();
        if (index < 0)
        {
            return;
        }
        m_presets->Remove(static_cast<usize>(index));
        m_dirty = true;
        Save();
        m_list->Selection.ClearSelection();
        m_list->NotifyDataChanged();
        if (m_presets->Count() > 0)
        {
            Select(Min(static_cast<usize>(index), m_presets->Count() - 1));
        }
        LoadFields();
    }

    void ExportDialog::ExportSelected()
    {
        const i32 index = SelectedIndex();
        if (index < 0 || !m_seams.runExport)
        {
            return;
        }
        Save();
        const String name(m_presets->At(static_cast<usize>(index)).name.AsView());
        m_seams.runExport(name.AsView(), false);
        Close(ui::DialogResult::OK);
    }

    void ExportDialog::ExportAll()
    {
        if (!m_seams.runExport || m_presets->Count() == 0)
        {
            return;
        }
        Save();
        m_seams.runExport(StringView{}, true);
        Close(ui::DialogResult::OK);
    }

    void ExportDialog::RefreshTemplates()
    {
        if (m_seams.refresh)
        {
            m_seams.refresh(m_registry);
        }
        m_icons.Clear();
        m_list->NotifyDataChanged();
        LoadFields();
    }
}
