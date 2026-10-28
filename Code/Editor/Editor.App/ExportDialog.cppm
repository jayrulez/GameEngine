// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor App - :export_dialog partition
//
// Export (Documentation/Specs/editor-gui.md, 5.2, layer 6): Project > Export, a window over its
// markup (Data/Editor/Export.xml). The project's export presets on the left (add, duplicate,
// remove after asking), each by the icon of the template it resolves to here, warned of when none
// does; the selected one on the right under a card naming the template it exports with, or saying
// why there is none, its settings on the property grid as Project Settings' are (ruling 20): a
// section per category (General: its name, template, platform, config, player and output folder;
// Content: the extra files as a list, symbols, pruning, shader source; Display: its own render
// size, window and orientation, each shown only when the preset overrides it). A template picked
// by name decides the platform and config. Edits apply as they are made and are saved when the
// selection moves, an export starts or it closes. Export runs the selected preset, Export All
// every one; Export Templates opens that window over this one, and the card follows what it
// changed. What it needs from outside comes through ExportDialogSeams, so a test drives it
// without a window system.
module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"
#include <initializer_list>

export module editor.app:export_dialog;

import foundation.core;
import foundation.gui;
import foundation.gui.toolkit;
import engine.project; // the display enums (ProjectSettings' own)
import editor.core;
import :templates_dialog; // TemplateRow

using namespace foundation::core;

export namespace editor::app
{
    namespace gui = foundation::gui;

    /// What the export dialog asks of the application.
    struct ExportDialogSeams
    {
        /// Fill `registry` with the templates this host sees.
        Function<void(editor::TemplateRegistry&)> refresh;
        /// Persist the presets (the controller the dialog edits) to the project.
        Function<void()> save;
        /// Run the export of one preset by name, or of every preset (`all`).
        Function<void(StringView preset, bool all)> runExport;
        /// Open Export Templates over this one; `closed` hears when it closes.
        Function<void(Function<void()> closed)> openTemplates;
        /// Ask for files; `chosen` hears their paths, or nothing when the user cancels.
        Function<void(Function<void(Array<String>)> chosen)> pickFiles;
        /// The host's platform, a new preset's.
        String hostPlatform;
        /// Where exports are written, said under the presets.
        String outputRoot;
    };

    /// ExportDialog::kPath, at namespace scope for another library to read (MSVC exports no class's
    /// static constexpr member from a shared library).
    inline constexpr const char8_t* kExportDialogPath = u8"Editor/Export.xml";

    class ExportDialog
    {
    public:
        /// Its markup, under the data root.
        static constexpr const char8_t* kPath = kExportDialogPath;

        ExportDialog(IAllocator& allocator, editor::ExportPresetsController& presets, ExportDialogSeams seams,
                     RefPtr<gui::Node> body)
            : m_allocator(allocator), m_presets(&presets), m_seams(Move(seams)), m_body(Move(body)), m_icons(allocator),
              m_model(*this)
        {
            if (!m_body)
                return;
            m_window = MakeRef<gui::Window>(m_allocator);
            m_window->SetTitle(u8"Export");
            m_window->AddClass(u8"export-window");
            m_window->SetSize(Float2{kWidth, kHeight});
            m_window->GetContent()->AddChild(m_body.Get());
            ExportDialog* self = this;
            m_list = Find<gui::ListView>(u8"export-list");
            if (m_list != nullptr)
            {
                gui::RowFactory rows;
                rows.Create = [self](u32) -> RefPtr<gui::Node> { return MakeRef<TemplateRow>(self->m_allocator); };
                rows.Bind = [self](gui::Node& row, const gui::ModelIndex& index) { self->BindRow(row, index.Row); };
                m_list->SetRowFactory(Move(rows));
                m_list->SetModel(&m_model);
                m_list->SetOnSelectionChanged([self](gui::ModelIndex)
                                               {
                                                   self->Save(); // the one left is kept
                                                   self->Load();
                                               });
            }
            m_grid = Find<gui::PropertyGrid>(u8"export-grid");
            m_export = Find<gui::Button>(u8"export-run");
            m_duplicate = Find<gui::Button>(u8"export-duplicate");
            m_remove = Find<gui::Button>(u8"export-remove");
            const auto click = [this](StringView id, Function<void()> run)
            {
                if (auto* button = Find<gui::Button>(id))
                    button->SetOnClick(Move(run));
            };
            click(u8"export-add", [self]() { self->AddPreset(); });
            click(u8"export-duplicate", [self]() { self->DuplicateSelected(); });
            click(u8"export-remove", [self]() { self->RemoveSelected(); });
            click(u8"export-run", [self]() { self->ExportSelected(); });
            click(u8"export-all", [self]() { self->ExportAll(); });
            click(u8"export-close", [self]() { self->Close(); });
            click(u8"export-add-files", [self]() { self->AddFiles(); });
            click(u8"export-templates", [self]()
                  {
                      if (self->m_seams.openTemplates)
                          self->m_seams.openTemplates([self]() { self->RefreshTemplates(); });
                  });
            if (auto* where = Find<gui::Label>(u8"export-where"))
                where->SetText(Format(u8"Exports go to {}", m_seams.outputRoot).AsView());
            // However it closes, what was edited is kept.
            m_window->SetOnClose([self]() { self->Save(); });
        }
        ExportDialog(const ExportDialog&) = delete;
        ExportDialog& operator=(const ExportDialog&) = delete;
        ~ExportDialog()
        {
            Close();
            if (m_grid != nullptr)
                m_grid->SetSource(nullptr);
            if (m_list != nullptr)
                m_list->SetModel(nullptr);
        }

        /// Opens it over `root`, the templates read again and the first preset selected; false
        /// with no markup or one open.
        bool Open(gui::Node& root)
        {
            if (!m_window || m_window->IsOpen())
                return false;
            m_root = &root;
            if (m_seams.refresh)
                m_seams.refresh(m_registry);
            m_icons.Clear();
            m_model.Reset();
            Select(0);
            m_window->OpenModal(root);
            return true;
        }
        [[nodiscard]] bool IsOpen() const noexcept { return m_window && m_window->IsOpen(); }
        void Close()
        {
            if (IsOpen())
                m_window->Close();
        }

        /// The presets' count and the selected one's index (-1 when there are none).
        [[nodiscard]] usize PresetCount() const noexcept { return m_presets->Count(); }
        [[nodiscard]] i32 SelectedIndex() const
        {
            const i32 index = m_list != nullptr ? m_list->GetSelectedItem() : -1;
            return index >= 0 && static_cast<usize>(index) < m_presets->Count() ? index : -1;
        }
        void Select(usize index)
        {
            Save(); // the one left is kept
            if (m_list != nullptr)
            {
                if (index < m_presets->Count())
                    m_list->SetSelectedItem(static_cast<i32>(index));
                else
                    m_list->ClearSelection();
            }
            Load();
        }

        /// The template the selected preset resolves to here, or null.
        [[nodiscard]] const editor::ExportTemplate* Resolved() const
        {
            const i32 index = SelectedIndex();
            return index >= 0 ? m_registry.Resolve(m_presets->At(static_cast<usize>(index))) : nullptr;
        }
        /// The card's title: the template it exports with, or why there is none.
        [[nodiscard]] StringView ResolveText() const
        {
            const auto* title = Find<gui::Label>(u8"export-card-title");
            return title != nullptr ? title->GetText() : StringView{};
        }

        void AddPreset()
        {
            editor::ExportPreset fresh;
            fresh.name = String(u8"New Preset");
            fresh.platform = m_seams.hostPlatform;
            const usize index = m_presets->Add(fresh);
            m_dirty = true;
            Save();
            m_model.Reset();
            Select(index);
        }
        void DuplicateSelected()
        {
            const i32 index = SelectedIndex();
            if (index < 0)
                return;
            const usize copy = m_presets->Duplicate(static_cast<usize>(index));
            m_dirty = true;
            Save();
            m_model.Reset();
            Select(copy);
        }
        /// Removes the selected preset once the user confirms (`confirmed` skips the question).
        void RemoveSelected(bool confirmed = false)
        {
            const i32 index = SelectedIndex();
            if (index < 0)
                return;
            const String name(m_presets->At(static_cast<usize>(index)).name);
            if (!confirmed)
            {
                if (m_root == nullptr)
                    return;
                auto box = MakeRef<gui::MessageBox>(m_allocator);
                box->Configure(u8"Remove Preset", Format(u8"Remove the preset '{}'? Its settings go; what it exported stays.", name).AsView(),
                               gui::MessageBox::Buttons::Ok);
                box->ClearButtons();
                (void)box->AddButton(u8"Cancel", u8"cancel", gui::DialogRole::Cancel);
                (void)box->AddButton(u8"Remove", u8"remove", gui::DialogRole::Destructive);
                ExportDialog* self = this;
                gui::MessageBox* raw = box.Get();
                (void)box->AddEventListener(gui::EventType::Changed, [self, raw, index, name](const gui::Event&)
                                            {
                                                if (raw->GetResultName().AsView() == StringView(u8"remove") && self->SelectedIndex() == index &&
                                                    self->m_presets->At(static_cast<usize>(index)).name == name)
                                                    self->RemoveSelected(true);
                                            });
                box->OpenModal(*m_root);
                return;
            }
            m_presets->Remove(static_cast<usize>(index));
            m_dirty = true;
            Save();
            m_model.Reset();
            Select(m_presets->Count() > 0 ? Min(static_cast<usize>(index), m_presets->Count() - 1) : m_presets->Count());
        }
        void ExportSelected()
        {
            const i32 index = SelectedIndex();
            if (index < 0 || !m_seams.runExport || Resolved() == nullptr)
                return;
            Save();
            const String name(m_presets->At(static_cast<usize>(index)).name);
            Close();
            m_seams.runExport(name.AsView(), false);
        }
        void ExportAll()
        {
            if (!m_seams.runExport || m_presets->Count() == 0)
                return;
            Save();
            Close();
            m_seams.runExport(StringView{}, true);
        }
        /// Re-reads the templates (after Export Templates changed them).
        void RefreshTemplates()
        {
            if (m_seams.refresh)
                m_seams.refresh(m_registry);
            m_icons.Clear();
            m_model.Reset();
            Load();
        }
        /// Adds picked files to the selected preset's extra files.
        void AddFiles()
        {
            if (!m_seams.pickFiles || SelectedIndex() < 0)
                return;
            ExportDialog* self = this;
            const i32 index = SelectedIndex();
            m_seams.pickFiles([self, index](Array<String> paths)
                              {
                                  if (self->SelectedIndex() != index)
                                      return;
                                  editor::ExportPreset preset = self->m_presets->At(static_cast<usize>(index));
                                  for (String& path : paths)
                                      preset.additionalFiles.PushBack(Move(path));
                                  self->m_presets->Update(static_cast<usize>(index), preset);
                                  self->m_dirty = true;
                                  self->Load();
                              });
        }

        /// The rows (the selected preset's, the grid's source; null with none) and the window, for tests.
        [[nodiscard]] gui::PropertyBag* Rows() const noexcept { return m_bag.Get(); }
        [[nodiscard]] gui::PropertyGrid* Grid() const noexcept { return m_grid; }
        [[nodiscard]] gui::Window* Window() const noexcept { return m_window.Get(); }
        [[nodiscard]] gui::Button* ExportButton() const noexcept { return m_export; }

    private:
        static constexpr f32 kWidth = 820.0f;
        static constexpr f32 kHeight = 540.0f;
        static constexpr StringView kDot = u8" · ";
        static constexpr StringView kGeneral = u8"General";
        static constexpr StringView kContent = u8"Content";
        static constexpr StringView kDisplay = u8"Display";

        // The presets as the list's model.
        class Model final : public gui::IModel
        {
        public:
            explicit Model(ExportDialog& dialog) : m_dialog(dialog) {}
            [[nodiscard]] usize RowCount(const gui::ModelIndex& parent = {}) const override { return parent.IsValid() ? 0 : m_dialog.m_presets->Count(); }
            [[nodiscard]] usize ColumnCount() const override { return 1; }
            [[nodiscard]] gui::Variant Data(const gui::ModelIndex& index, gui::ModelRole role = gui::ModelRole::Display) const override
            {
                if (role != gui::ModelRole::Display || index.Row < 0 || static_cast<usize>(index.Row) >= m_dialog.m_presets->Count())
                    return gui::Variant{};
                return gui::Variant(m_dialog.m_presets->At(static_cast<usize>(index.Row)).name.AsView());
            }
            void Reset() { NotifyReset(); }
            void Changed(i32 row) { NotifyRowsChanged(gui::ModelIndex{}, row, 1); }

        private:
            ExportDialog& m_dialog;
        };

        [[nodiscard]] static StringView ConfigOf(const editor::ExportPreset& preset)
        {
            return preset.config.IsEmpty() ? StringView(u8"Release") : preset.config.AsView();
        }
        /// The enum type of one of ProjectSettings' properties (the display choices).
        [[nodiscard]] static const TypeInfo& SettingEnum(const char* property)
        {
            return *FindProperty(engine::project::ProjectSettings::StaticType(), property)->type;
        }
        [[nodiscard]] RefPtr<gui::Drawable> IconFor(const editor::ExportTemplate& tmpl)
        {
            String key(tmpl.id.AsView());
            key += u8"|";
            key += tmpl.directory.AsView();
            if (const RefPtr<gui::Drawable>* known = m_icons.Find(key))
                return *known;
            const String svg = editor::TemplateIconSvg(tmpl, m_allocator);
            RefPtr<gui::Drawable> icon(gui::SvgDrawable::FromText(m_allocator, svg.AsView()).Get());
            m_icons.InsertOrAssign(Move(key), icon);
            return icon;
        }
        void BindRow(gui::Node& node, i32 row)
        {
            auto* shown = Cast<TemplateRow>(&node);
            if (shown == nullptr || row < 0 || static_cast<usize>(row) >= m_presets->Count())
                return;
            const editor::ExportPreset& preset = m_presets->At(static_cast<usize>(row));
            const editor::ExportTemplate* tmpl = m_registry.Resolve(preset);
            String summary(preset.platform.IsEmpty() ? StringView(u8"?") : preset.platform.AsView());
            summary += kDot;
            summary += ConfigOf(preset);
            shown->Show(tmpl != nullptr ? IconFor(*tmpl) : RefPtr<gui::Drawable>{}, preset.name.AsView(), summary.AsView(),
                        tmpl != nullptr ? StringView{} : StringView(u8"No template for it here"));
        }

        // The values a choice offers: the preset's own, the fallback (the host's platform,
        // Release), then the templates' here; a platform with no template stays choosable.
        void FillChoices(gui::PropertyInfo& row, Array<String>& values, StringView current, StringView fallback, bool platforms) const
        {
            values.Clear();
            const auto add = [&values](StringView value)
            {
                if (value.IsEmpty())
                    return;
                for (const String& known : values)
                    if (known.AsView() == value)
                        return;
                values.PushBack(String(value));
            };
            add(current);
            add(fallback);
            for (usize i = 0; i < m_registry.Count(); ++i)
                add(platforms ? m_registry.At(i)->platform.AsView() : m_registry.At(i)->EffectiveConfig());
            for (usize i = 0; i < values.Size(); ++i)
                row.Choices.PushBack(gui::PropertyChoice{values[i], static_cast<i64>(i)});
        }
        [[nodiscard]] static i64 IndexOf(const Array<String>& values, StringView value)
        {
            for (usize i = 0; i < values.Size(); ++i)
                if (values[i].AsView() == value)
                    return static_cast<i64>(i);
            return 0;
        }

        gui::PropertyInfo& Row(StringView path, gui::PropertyKind kind, const gui::Variant& value, StringView label, StringView category,
                               StringView description = {})
        {
            gui::PropertyInfo& row = m_bag->Add(path, kind, value);
            row.DisplayName = String(label);
            row.Category = String(category);
            row.Description = String(description);
            return row;
        }
        gui::PropertyInfo& EnumRow(StringView path, const TypeInfo& type, i64 value, StringView label, StringView category)
        {
            gui::PropertyInfo& row = Row(path, gui::PropertyKind::Enum, gui::Variant(value), label, category);
            for (usize i = 0; i < EnumeratorCount(type); ++i)
            {
                const EnumValue& enumerator = EnumeratorAt(type, i);
                row.Choices.PushBack(gui::PropertyChoice{String(StringView(reinterpret_cast<const utf8char*>(enumerator.name))), enumerator.value});
            }
            return row;
        }
        gui::PropertyInfo& SizeRow(StringView path, u32 value, StringView label, f64 least)
        {
            gui::PropertyInfo& row = Row(path, gui::PropertyKind::Integer, gui::Variant(static_cast<i64>(value)), label, kDisplay);
            row.HasRange = true;
            row.Min = least;
            row.Max = 16384.0;
            row.Step = 1.0;
            return row;
        }

        // The selected preset's rows, the card and the buttons.
        void Load()
        {
            if (m_grid != nullptr)
                m_grid->SetSource(nullptr);
            m_bag.Reset();
            const i32 index = SelectedIndex();
            if (m_export != nullptr)
                m_export->SetEnabled(index >= 0 && Resolved() != nullptr);
            if (m_duplicate != nullptr)
                m_duplicate->SetEnabled(index >= 0);
            if (m_remove != nullptr)
                m_remove->SetEnabled(index >= 0);
            ShowCard();
            if (index < 0)
                return;
            const editor::ExportPreset& preset = m_presets->At(static_cast<usize>(index));
            m_bag = MakeUnique<gui::PropertyBag>(m_allocator, m_allocator);
            Row(u8"name", gui::PropertyKind::String, gui::Variant(preset.name.AsView()), u8"Name", kGeneral);
            gui::PropertyInfo& tmpl = Row(u8"template", gui::PropertyKind::Enum, gui::Variant(i64{0}), u8"Template", kGeneral,
                                          u8"A template picked by name decides the platform and config; Any takes the best installed for them.");
            tmpl.Choices.PushBack(gui::PropertyChoice{String(u8"Any for its platform and config"), 0});
            i64 chosen = 0;
            for (usize i = 0; i < m_registry.Count(); ++i)
            {
                const editor::ExportTemplate* t = m_registry.At(i);
                tmpl.Choices.PushBack(gui::PropertyChoice{t->isHost ? Format(u8"{} (this editor's own)", t->name) : t->name, static_cast<i64>(i) + 1});
                if (!preset.templateId.IsEmpty() && t->id == preset.templateId)
                    chosen = static_cast<i64>(i) + 1;
            }
            m_bag->SetValue(u8"template", 0, gui::Variant(chosen));
            (void)Row(u8"platform", gui::PropertyKind::Enum, gui::Variant(i64{0}), u8"Platform", kGeneral);
            (void)Row(u8"config", gui::PropertyKind::Enum, gui::Variant(i64{0}), u8"Config", kGeneral);
            ShowChoices(preset, chosen != 0);
            Row(u8"playerName", gui::PropertyKind::String, gui::Variant(preset.playerName.AsView()), u8"Player name", kGeneral,
                u8"Empty: the template's (Engine.Player).");
            Row(u8"outputSubdir", gui::PropertyKind::String, gui::Variant(preset.outputSubdir.AsView()), u8"Output folder", kGeneral,
                u8"Empty: the preset's name.");

            gui::PropertyInfo& files = m_bag->AddList(u8"additionalFiles", gui::PropertyKind::String, gui::Variant(StringView{}), preset.additionalFiles.Size());
            files.DisplayName = String(u8"Extra files");
            files.Category = String(kContent);
            files.Description = String(u8"Copied beside the player, as they are (a licence, a readme).");
            for (usize i = 0; i < preset.additionalFiles.Size(); ++i)
                m_bag->SetValue(Format(u8"additionalFiles.{}", i).AsView(), 0, gui::Variant(preset.additionalFiles[i].AsView()));
            Row(u8"stageSymbols", gui::PropertyKind::Bool, gui::Variant(preset.stageSymbols), u8"Ship debug symbols", kContent,
                u8"Stage the template's debug symbols into the export.");
            Row(u8"pruneToReachable", gui::PropertyKind::Bool, gui::Variant(preset.pruneToReachable), u8"Only what is reached", kContent,
                u8"Pack the closure of the default scene, the startup script and the other settings' assets, not every cooked asset.");
            Row(u8"includeShaderSource", gui::PropertyKind::Bool, gui::Variant(preset.includeShaderSource), u8"Shader source", kContent,
                u8"Ship the project's shaders as HLSL too, beside the cooked bytecode (for modders to read).");

            Row(u8"overridesRender", gui::PropertyKind::Bool, gui::Variant(preset.overridesRender), u8"Own render size", kDisplay,
                u8"This platform draws at its own resolution rather than the project's.");
            SizeRow(u8"renderWidth", preset.renderWidth, u8"Render width", 0.0);
            SizeRow(u8"renderHeight", preset.renderHeight, u8"Render height", 0.0);
            EnumRow(u8"renderFit", SettingEnum("renderFit"), static_cast<i64>(preset.renderFit), u8"Render fit", kDisplay);
            Row(u8"overridesWindow", gui::PropertyKind::Bool, gui::Variant(preset.overridesWindow), u8"Own window", kDisplay,
                u8"This platform's window differs from the project's (fullscreen on a device).");
            SizeRow(u8"windowWidth", preset.windowWidth, u8"Window width", 1.0);
            SizeRow(u8"windowHeight", preset.windowHeight, u8"Window height", 1.0);
            EnumRow(u8"windowMode", SettingEnum("windowMode"), static_cast<i64>(preset.windowMode), u8"Window mode", kDisplay);
            Row(u8"windowResizable", gui::PropertyKind::Bool, gui::Variant(preset.windowResizable), u8"Resizable", kDisplay);
            Row(u8"overridesOrientation", gui::PropertyKind::Bool, gui::Variant(preset.overridesOrientation), u8"Own orientation", kDisplay,
                u8"This platform is held its own way.");
            EnumRow(u8"orientation", SettingEnum("orientation"), static_cast<i64>(preset.orientation), u8"Orientation", kDisplay);
            ShowOverrides(preset);
            ExportDialog* self = this;
            m_bag->SetOnCommit([self](const gui::PropertyBag::Commit&) { self->Store(); });
            if (m_grid != nullptr)
                m_grid->SetSource(m_bag.Get());
        }
        // The platform and config rows as the preset has them: their choices, the preset's, and
        // read only while a template picked by name decides them. In place: this runs inside the
        // rows' own commit.
        void ShowChoices(const editor::ExportPreset& preset, bool byTemplate)
        {
            gui::PropertyInfo* platform = m_bag->FindInfo(u8"platform");
            gui::PropertyInfo* config = m_bag->FindInfo(u8"config");
            if (platform == nullptr || config == nullptr)
                return;
            platform->Choices.Clear();
            config->Choices.Clear();
            FillChoices(*platform, m_platforms, preset.platform.AsView(), m_seams.hostPlatform.AsView(), true);
            FillChoices(*config, m_configs, preset.config.AsView(), u8"Release", false);
            platform->ReadOnly = byTemplate;
            config->ReadOnly = byTemplate;
            m_bag->SetValue(u8"platform", 0, gui::Variant(IndexOf(m_platforms, preset.platform.IsEmpty() ? m_seams.hostPlatform.AsView() : preset.platform.AsView())));
            m_bag->SetValue(u8"config", 0, gui::Variant(IndexOf(m_configs, ConfigOf(preset))));
            m_bag->PropertiesChanged();
        }
        // A display override's rows show only while the preset overrides it.
        void ShowOverrides(const editor::ExportPreset& preset)
        {
            const auto show = [this](StringView path, bool shown)
            {
                if (gui::PropertyInfo* info = m_bag->FindInfo(path))
                    info->Visible = shown;
            };
            for (StringView path : {StringView(u8"renderWidth"), StringView(u8"renderHeight"), StringView(u8"renderFit")})
                show(path, preset.overridesRender);
            for (StringView path : {StringView(u8"windowWidth"), StringView(u8"windowHeight"), StringView(u8"windowMode"), StringView(u8"windowResizable")})
                show(path, preset.overridesWindow);
            show(u8"orientation", preset.overridesOrientation);
            m_bag->PropertiesChanged();
        }
        // The rows into the selected preset, then what shows it.
        void Store()
        {
            const i32 index = SelectedIndex();
            if (index < 0 || !m_bag)
                return;
            editor::ExportPreset preset = m_presets->At(static_cast<usize>(index));
            const auto text = [this](StringView path) { return String(m_bag->GetValue(path, 0).AsString()); };
            const auto flag = [this](StringView path) { return m_bag->GetValue(path, 0).AsBool(); };
            const auto number = [this](StringView path) { return static_cast<u32>(Max<i64>(0, m_bag->GetValue(path, 0).AsInt())); };
            const auto choice = [this](StringView path) { return m_bag->GetValue(path, 0).AsInt(); };
            preset.name = text(u8"name");
            const i64 chosen = choice(u8"template");
            const editor::ExportTemplate* tmpl = chosen > 0 && static_cast<usize>(chosen) <= m_registry.Count() ? m_registry.At(static_cast<usize>(chosen) - 1) : nullptr;
            if (tmpl != nullptr)
            {
                // A template by name: its platform and config.
                preset.templateId = tmpl->id;
                preset.platform = tmpl->platform;
                preset.config = String(tmpl->EffectiveConfig());
            }
            else
            {
                preset.templateId = String{};
                const i64 platform = choice(u8"platform");
                const i64 config = choice(u8"config");
                preset.platform = platform >= 0 && static_cast<usize>(platform) < m_platforms.Size() ? m_platforms[static_cast<usize>(platform)] : String{};
                preset.config = config >= 0 && static_cast<usize>(config) < m_configs.Size() ? m_configs[static_cast<usize>(config)] : String{};
            }
            preset.playerName = text(u8"playerName");
            preset.outputSubdir = text(u8"outputSubdir");
            preset.additionalFiles.Clear();
            for (usize i = 0; i < m_bag->ElementCount(u8"additionalFiles", 0); ++i)
            {
                String file = text(Format(u8"additionalFiles.{}", i).AsView());
                if (!Trim(file.AsView()).IsEmpty())
                    preset.additionalFiles.PushBack(Move(file));
            }
            preset.stageSymbols = flag(u8"stageSymbols");
            preset.pruneToReachable = flag(u8"pruneToReachable");
            preset.includeShaderSource = flag(u8"includeShaderSource");
            preset.overridesRender = flag(u8"overridesRender");
            preset.renderWidth = number(u8"renderWidth");
            preset.renderHeight = number(u8"renderHeight");
            preset.renderFit = static_cast<FitMode>(choice(u8"renderFit"));
            preset.overridesWindow = flag(u8"overridesWindow");
            preset.windowWidth = number(u8"windowWidth");
            preset.windowHeight = number(u8"windowHeight");
            preset.windowMode = static_cast<engine::project::WindowMode>(choice(u8"windowMode"));
            preset.windowResizable = flag(u8"windowResizable");
            preset.overridesOrientation = flag(u8"overridesOrientation");
            preset.orientation = static_cast<ScreenOrientation>(choice(u8"orientation"));
            m_presets->Update(static_cast<usize>(index), preset);
            m_dirty = true;
            m_model.Changed(index);
            ShowChoices(preset, tmpl != nullptr); // the platform and config follow a template picked by name
            ShowOverrides(preset);
            ShowCard();
            if (m_export != nullptr)
                m_export->SetEnabled(Resolved() != nullptr);
        }

        void ShowCard()
        {
            auto* icon = Find<gui::Image>(u8"export-card-icon");
            auto* title = Find<gui::Label>(u8"export-card-title");
            auto* detail = Find<gui::Label>(u8"export-card-detail");
            if (icon == nullptr || title == nullptr || detail == nullptr)
                return;
            const i32 index = SelectedIndex();
            detail->RemoveClass(u8"warning");
            if (index < 0)
            {
                icon->SetDrawable({});
                title->SetText(u8"No presets yet");
                detail->SetText(u8"Add one with + for this platform.");
                return;
            }
            const editor::ExportPreset& preset = m_presets->At(static_cast<usize>(index));
            const editor::ExportTemplate* tmpl = m_registry.Resolve(preset);
            if (tmpl == nullptr)
            {
                icon->SetDrawable({});
                icon->SetSource(String(u8"Editor/Icons/warning.svg"));
                if (!preset.templateId.IsEmpty() && m_registry.FindById(preset.templateId.AsView()) == nullptr)
                    title->SetText(Format(u8"Template '{}' is not installed here", preset.templateId).AsView());
                else
                    title->SetText(Format(u8"No template for {} {} here", preset.platform.IsEmpty() ? StringView(u8"?") : preset.platform.AsView(), ConfigOf(preset)).AsView());
                detail->SetText(u8"Install or create one in Export Templates, or pick another platform.");
                detail->AddClass(u8"warning");
                return;
            }
            icon->SetDrawable(IconFor(*tmpl));
            title->SetText(Format(u8"Exports with {}", tmpl->name).AsView());
            String text(tmpl->platform.AsView());
            text += kDot;
            text += tmpl->EffectiveConfig();
            if (!tmpl->engineVersion.IsEmpty())
            {
                text += kDot;
                text += tmpl->engineVersion.AsView();
            }
            text += kDot;
            text += tmpl->isHost ? StringView(u8"this editor's own player") : StringView(u8"installed");
            if (!editor::TemplateEngineMatches(*tmpl))
            {
                text += u8". Built for another engine version; the export still runs.";
                detail->AddClass(u8"warning");
            }
            detail->SetText(text.AsView());
        }
        void Save()
        {
            if (m_dirty && m_seams.save)
                m_seams.save();
            m_dirty = false;
        }

        template <typename T>
        [[nodiscard]] T* Find(StringView id) const
        {
            return m_body ? Cast<T>(FindIn(*m_body, id)) : nullptr;
        }
        static gui::Node* FindIn(gui::Node& node, StringView id)
        {
            if (const auto* widget = Cast<gui::UIWidget>(&node); widget != nullptr && widget->GetId() == id)
                return &node;
            for (usize i = 0; i < node.ChildCount(); ++i)
                if (gui::Node* found = FindIn(*node.GetChildAt(i), id))
                    return found;
            return nullptr;
        }

        IAllocator& m_allocator;
        editor::ExportPresetsController* m_presets;
        ExportDialogSeams m_seams;
        RefPtr<gui::Node> m_body;
        RefPtr<gui::Window> m_window;
        gui::Node* m_root = nullptr; // what it opened over (its confirms open there too)
        editor::TemplateRegistry m_registry;
        HashMap<String, RefPtr<gui::Drawable>> m_icons; // by template id and folder
        UniquePtr<gui::PropertyBag> m_bag;              // the selected preset's rows
        Array<String> m_platforms;                      // the platform row's choices, by index
        Array<String> m_configs;                        // the config row's
        gui::ListView* m_list = nullptr;
        gui::PropertyGrid* m_grid = nullptr;
        gui::Button* m_export = nullptr;
        gui::Button* m_duplicate = nullptr;
        gui::Button* m_remove = nullptr;
        bool m_dirty = false; // edited since the last save
        Model m_model;
    };
}
