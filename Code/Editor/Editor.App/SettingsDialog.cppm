// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor App - :settings_dialog partition
//
// Project Settings: the project's manifest (Project.xml) on the property grid, in a window over its
// markup (Data/Editor/ProjectSettings.xml). The rows are a bag made from ProjectSettings'
// reflection, with a section per category the settings name and each row by its label: text, a
// number within its range, a check box, an enum's choices by name, and for each asset setting an
// asset reference to the source type it names (a list of them for a list setting), so the slot
// picks, drops and clears as an inspector's does. MSAA's choices are the render subsystem's
// levels. The engine version the project was last saved with is shown read-only. The project's
// plugins follow in their own section, a group per entry in its order: an Enabled check box, its
// state in the process's plugin host, and for a loaded plugin its settings, rows made the same way
// from its settings type; an entry whose plugin is not loaded keeps its settings as written. After
// them, what the project could enable (engine::project::DiscoverPlugins, read from its Plugins
// folder each time the dialog opens): each package no entry names and each linked plugin no entry
// enables, unchecked, or with why it cannot be enabled. Save writes the rows back (dropping a
// list's empty and repeated entries), keeps the path mirrors beside the guids, writes each loaded
// plugin's settings into its entry, removes the entries left unchecked (a package's folder stays)
// and adds the plugins checked (on every platform their manifests support), persists the manifest
// and notifies the editor; a changed plugin list takes effect at the next Build Project Plugins and
// Reload Project Plugins. Cancel or closing leaves the manifest as it was.
module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"
#include "Core/Reflection/Reflect.h"

export module editor.app:settings_dialog;

import foundation.core;
import foundation.content;
import foundation.gui;
import foundation.gui.toolkit;
import engine.render;  // the MSAA levels
import engine.project; // ProjectSettings' reflected settings
import foundation.runtime; // the plugins' entries and host
import editor.core;
import :resource_ref_editor;

using namespace foundation::core;

export namespace editor::app
{
    namespace gui = foundation::gui;

    /// ProjectSettingsDialog::kEngineVersionRow, at namespace scope for another library to read (MSVC exports no class's
    /// static constexpr member from a shared library).
    inline constexpr StringView kProjectSettingsDialogEngineVersionRow = u8"engineVersion";

    /// ProjectSettingsDialog::kMsaaSetting, at namespace scope for another library to read (MSVC exports no class's
    /// static constexpr member from a shared library).
    inline constexpr StringView kProjectSettingsDialogMsaaSetting = u8"renderMsaaSamples";

    /// ProjectSettingsDialog::kPluginsCategory, at namespace scope for another library to read.
    inline constexpr StringView kProjectSettingsDialogPluginsCategory = u8"Plugins";

    /// ProjectSettingsDialog::kPath, at namespace scope for another library to read (MSVC exports no class's
    /// static constexpr member from a shared library).
    inline constexpr const char8_t* kProjectSettingsDialogPath = u8"Editor/ProjectSettings.xml";

    class ProjectSettingsDialog
    {
    public:
        /// Its markup's path, under the data root.
        static constexpr const char8_t* kPath = kProjectSettingsDialogPath;

        /// Builds the dialog over `context`'s project; `refs` (the asset slots' table) outlives it, as
        /// `linked` does (the plugins the editor links, which the project may enable).
        ProjectSettingsDialog(IAllocator& allocator, editor::EditorContext& context, const ResourceRefServices& refs,
                              Span<const foundation::runtime::LinkedPlugin> linked, RefPtr<gui::Node> body)
            : m_allocator(allocator), m_context(&context), m_linked(linked), m_pluginSettings(allocator),
              m_available(allocator), m_body(Move(body))
        {
            if (!m_body)
                return;
            m_grid = Find<gui::PropertyGrid>(u8"settings-grid");
            if (m_grid != nullptr)
                ResourceRefEditor::Register(m_grid->GetEditors(), refs);
            m_window = MakeRef<gui::Window>(m_allocator);
            m_window->SetTitle(u8"Project Settings");
            m_window->AddClass(u8"project-settings-window");
            m_window->SetSize(Float2{kWidth, kHeight});
            m_window->GetContent()->AddChild(m_body.Get());
            ProjectSettingsDialog* self = this;
            if (auto* save = Find<gui::Button>(u8"settings-save"))
                save->SetOnClick([self]() { (void)self->Save(); });
            if (auto* cancel = Find<gui::Button>(u8"settings-cancel"))
                cancel->SetOnClick([self]() { self->Close(); });
        }
        ProjectSettingsDialog(const ProjectSettingsDialog&) = delete;
        ProjectSettingsDialog& operator=(const ProjectSettingsDialog&) = delete;
        ~ProjectSettingsDialog()
        {
            Close();
            if (m_grid != nullptr)
                m_grid->SetSource(nullptr);
        }

        /// Opens it over `root` with the manifest as it is; false with no markup or no project.
        bool Open(gui::Node& root)
        {
            if (!m_window || m_window->IsOpen() || m_context->Project() == nullptr)
                return false;
            if (m_grid != nullptr)
                m_grid->SetSource(nullptr); // the rows are rebuilt from the manifest as it is
            Fill(m_context->Project()->Settings());
            if (m_grid != nullptr)
            {
                m_grid->SetSource(m_bag.Get());
                OpenPluginGroups();
            }
            m_window->OpenModal(root);
            return true;
        }
        [[nodiscard]] bool IsOpen() const noexcept { return m_window && m_window->IsOpen(); }
        void Close()
        {
            if (m_window && m_window->IsOpen())
                m_window->Close();
        }

        /// Writes the rows into the manifest, persists it and closes; false when it could not be
        /// written (the dialog stays open and the editor is told).
        bool Save()
        {
            editor::EditorProject* project = m_context->Project();
            if (project == nullptr)
            {
                Close();
                return false;
            }
            engine::project::ProjectSettings& settings = project->Settings();
            WriteBack(settings);
            settings.RefreshPathMirrors([project](const Guid& id)
                                        {
                                            foundation::content::Instance* instance = project->SourceDb().GetInstance(id);
                                            return instance != nullptr ? instance->Path() : String();
                                        });
            if (!project->SaveSettings().IsOk())
            {
                m_context->Notify(editor::NoticeKind::Error, u8"Project settings save FAILED (see Console).");
                return false;
            }
            m_context->SetStatus(m_pluginsChanged ? StringView(u8"Project settings saved. The plugins changed: Build Project "
                                                               u8"Plugins, then Reload Project Plugins.")
                                                  : StringView(u8"Project settings saved."));
            m_context->NotifyProjectSettingsChanged(); // the session's settings-derived state follows
            Close();
            return true;
        }

        /// The rows (the bag the grid shows) and the grid, for a test.
        [[nodiscard]] gui::PropertyBag* Rows() const noexcept { return m_bag.Get(); }
        [[nodiscard]] gui::PropertyGrid* Grid() const noexcept { return m_grid; }
        [[nodiscard]] gui::Window* Window() const noexcept { return m_window.Get(); }

        /// The path of the read-only row showing the engine version the project was saved with.
        static constexpr StringView kEngineVersionRow = kProjectSettingsDialogEngineVersionRow;
        /// The section the project's plugins show in; plugin `id`'s group is `plugins.<id>`, its
        /// state `plugins.<id>.state` and each setting `plugins.<id>.<name>`.
        static constexpr StringView kPluginsCategory = kProjectSettingsDialogPluginsCategory;
        /// The MSAA setting, whose choices are the render subsystem's levels.
        static constexpr StringView kMsaaSetting = kProjectSettingsDialogMsaaSetting;
        /// The row the entries' groups sit under (`plugins.<id>`), and the one the plugins the
        /// project could enable sit under (`available.<n>`).
        static constexpr StringView kPluginEntriesGroup = u8"plugins";
        static constexpr StringView kAvailablePluginsGroup = u8"available";

    private:
        static constexpr f32 kWidth = 620.0f;
        static constexpr f32 kHeight = 480.0f;

        [[nodiscard]] static const TypeInfo& SettingsType() { return engine::project::ProjectSettings::StaticType(); }
        [[nodiscard]] static StringView NameOf(const PropertyInfo& property) { return StringView(reinterpret_cast<const utf8char*>(property.name)); }
        [[nodiscard]] static String GuidText(const Guid& id)
        {
            if (id.IsNil())
                return String{};
            utf8char chars[37];
            id.ToChars(chars);
            return String(StringView(chars, 36));
        }
        // The source asset type a setting names, looked up by name (the settings name it that way).
        [[nodiscard]] static const TypeInfo* SourceTypeNamed(StringView name)
        {
            for (const TypeInfo* type : GlobalTypeRegistry().All())
                if (type != nullptr && type->name != nullptr && StringView(reinterpret_cast<const utf8char*>(type->name)) == name)
                    return type;
            return nullptr;
        }

        // Makes a row per labelled setting, in its category, from the manifest's values.
        void Fill(const engine::project::ProjectSettings& settings)
        {
            m_bag = MakeUnique<gui::PropertyBag>(m_allocator, m_allocator);
            const Instance instance(const_cast<engine::project::ProjectSettings*>(&settings), &SettingsType());
            for (const PropertyInfo& property : Properties(SettingsType()))
            {
                const String* label = engine::project::SettingAttribute(property, engine::project::kSettingLabelAttribute);
                if (label == nullptr || property.address == nullptr)
                    continue;
                const void* address = property.address(instance);
                gui::PropertyInfo* row = AddRow(NameOf(property), property, address);
                if (row == nullptr)
                    continue;
                row->DisplayName = *label;
                row->Category = String(engine::project::SettingCategory(property));
            }
            // The engine version that last saved it, under the first category.
            gui::PropertyInfo& version = m_bag->Add(kEngineVersionRow, gui::PropertyKind::String, gui::Variant(settings.engineVersion.AsView()));
            version.DisplayName = String(u8"Engine version");
            version.Category = String(engine::project::kSettingDefaultCategory);
            version.Description = String(u8"The engine this project was last saved with; a save stamps the running one.");
            version.ReadOnly = true;
            FillPlugins(settings);
        }
        // A group per plugin entry: whether it stays enabled, its state, and a loaded plugin's
        // settings; then a group per plugin the project could enable.
        void FillPlugins(const engine::project::ProjectSettings& settings)
        {
            m_pluginSettings.Clear();
            foundation::runtime::PluginHost* host = m_context->Plugins();
            // The entries' groups sit under one row of their own, as the grid reaches a row through its
            // parent's.
            if (!settings.plugins.IsEmpty())
            {
                gui::PropertyInfo& entries = m_bag->Add(kPluginEntriesGroup, gui::PropertyKind::Group);
                entries.DisplayName = String(u8"In this project");
                entries.Category = String(kPluginsCategory);
            }
            for (const foundation::runtime::EnabledPlugin& entry : settings.plugins)
            {
                const String group = Format(u8"plugins.{}", entry.id);
                const foundation::runtime::PluginManifest* manifest = host != nullptr ? host->Manifest(entry.id.AsView()) : nullptr;
                gui::PropertyInfo& header = m_bag->Add(group.AsView(), gui::PropertyKind::Group);
                header.DisplayName = manifest != nullptr && !manifest->name.IsEmpty() ? manifest->name : entry.id;
                header.Category = String(kPluginsCategory);
                gui::PropertyInfo& enabled = m_bag->Add(Format(u8"{}.enabled", group).AsView(), gui::PropertyKind::Bool, gui::Variant(true));
                enabled.DisplayName = String(u8"Enabled");
                enabled.Category = String(kPluginsCategory);
                enabled.Description = String(u8"Unchecked, Save removes it and its settings from the project; a package's folder stays.");
                const engine::project::PluginStatus status = engine::project::PluginStatusOf(entry, host, m_allocator);
                String state(engine::project::PluginStateName(status.state));
                if (!status.reason.IsEmpty())
                    state = Format(u8"{}: {}", state, status.reason);
                for (const String& platform : status.unsupportedPlatforms)
                    state.Append(Format(u8"; {} is not a platform it supports", platform).AsView());
                gui::PropertyInfo& stateRow = m_bag->Add(Format(u8"{}.state", group).AsView(), gui::PropertyKind::String, gui::Variant(state.AsView()));
                stateRow.DisplayName = String(u8"State");
                stateRow.Category = String(kPluginsCategory);
                stateRow.ReadOnly = true;
                ISerializable* object = host != nullptr ? host->Settings(entry.id.AsView()) : nullptr;
                if (object == nullptr || !host->SettingsRead(entry.id.AsView()))
                    continue; // kept as written
                m_pluginSettings.PushBack(PluginSettingsRows{String(entry.id.AsView(), m_allocator), object});
                const Instance instance(object, object->GetType());
                for (const PropertyInfo& property : Properties(*object->GetType()))
                {
                    const String* label = engine::project::SettingAttribute(property, engine::project::kSettingLabelAttribute);
                    if (label == nullptr || property.address == nullptr)
                        continue;
                    const String path = Format(u8"{}.{}", group, NameOf(property));
                    gui::PropertyInfo* row = AddRow(path.AsView(), property, property.address(instance));
                    if (row == nullptr)
                        continue;
                    row->DisplayName = *label;
                    row->Category = String(kPluginsCategory);
                }
            }
            FillAvailablePlugins(settings);
        }
        // Opens the plugins' groups (groups start folded), so each one's Enabled box shows.
        void OpenPluginGroups()
        {
            m_grid->SetOpen(kPluginEntriesGroup, true);
            m_grid->SetOpen(kAvailablePluginsGroup, true);
            if (editor::EditorProject* project = m_context->Project())
                for (const foundation::runtime::EnabledPlugin& entry : project->Settings().plugins)
                    m_grid->SetOpen(Format(u8"{}.{}", kPluginEntriesGroup, entry.id).AsView(), true);
            for (usize i = 0; i < m_available.Size(); ++i)
                m_grid->SetOpen(Format(u8"{}.{}", kAvailablePluginsGroup, i).AsView(), true);
        }
        // A group per plugin the project could enable (`available.<n>`): unchecked, with what it is,
        // or with why it cannot be enabled.
        void FillAvailablePlugins(const engine::project::ProjectSettings& settings)
        {
            m_available.Clear();
            editor::EditorProject* project = m_context->Project();
            if (project == nullptr)
                return;
            m_available = engine::project::DiscoverPlugins(project->Directory(), settings, m_linked, m_allocator);
            if (!m_available.IsEmpty())
            {
                gui::PropertyInfo& available = m_bag->Add(kAvailablePluginsGroup, gui::PropertyKind::Group);
                available.DisplayName = String(u8"Available");
                available.Category = String(kPluginsCategory);
            }
            for (usize i = 0; i < m_available.Size(); ++i)
            {
                const engine::project::AvailablePlugin& plugin = m_available[i];
                const String group = Format(u8"available.{}", i);
                gui::PropertyInfo& header = m_bag->Add(group.AsView(), gui::PropertyKind::Group);
                // Its name and version, then where it is: its folder, or linked; a package whose manifest
                // does not read is only its folder.
                if (plugin.id.IsEmpty())
                    header.DisplayName = String(plugin.path.AsView(), m_allocator);
                else
                {
                    header.DisplayName = String(!plugin.name.IsEmpty() ? plugin.name.AsView() : plugin.id.AsView(), m_allocator);
                    if (!plugin.version.IsEmpty())
                        header.DisplayName.Append(Format(u8" {}", plugin.version).AsView());
                    header.DisplayName.Append(plugin.path.IsEmpty() ? String(u8" (linked)").AsView()
                                                                    : Format(u8" ({})", plugin.path).AsView());
                }
                header.Category = String(kPluginsCategory);
                String state;
                if (plugin.problem.IsEmpty())
                {
                    gui::PropertyInfo& enabled =
                        m_bag->Add(Format(u8"{}.enabled", group).AsView(), gui::PropertyKind::Bool, gui::Variant(false));
                    enabled.DisplayName = String(u8"Enabled");
                    enabled.Category = String(kPluginsCategory);
                    enabled.Description = String(u8"Checked, Save enables it in the project. A package's code is built by Build "
                                                 u8"Project Plugins and then runs in the editor.");
                    state = String(u8"available:");
                    for (usize k = 0; k < plugin.parts.Size(); ++k)
                    {
                        state.Append(k == 0 ? StringView(u8" ") : StringView(u8", "));
                        state.Append(plugin.parts[k].AsView());
                    }
                    state.Append(plugin.parts.Size() == 1 ? StringView(u8" part on") : StringView(u8" parts on"));
                    for (usize k = 0; k < plugin.platforms.Size(); ++k)
                    {
                        state.Append(k == 0 ? StringView(u8" ") : StringView(u8", "));
                        state.Append(plugin.platforms[k].AsView());
                    }
                }
                else
                {
                    state = Format(u8"not offered: {}", plugin.problem);
                }
                gui::PropertyInfo& stateRow = m_bag->Add(Format(u8"{}.state", group).AsView(), gui::PropertyKind::String,
                                                         gui::Variant(state.AsView()));
                stateRow.DisplayName = String(u8"State");
                stateRow.Category = String(kPluginsCategory);
                stateRow.ReadOnly = true;
            }
        }
        // Adds the row for a setting at `name`, or returns null for one no row shows.
        gui::PropertyInfo* AddRow(StringView name, const PropertyInfo& property, const void* address)
        {
            const TypeInfo& type = *property.type;
            const String* assetType = engine::project::SettingAttribute(property, engine::project::kSettingAssetTypeAttribute);
            const String* emptyText = engine::project::SettingAttribute(property, engine::project::kSettingEmptyTextAttribute);
            if (engine::project::IsAssetSetting(property) && assetType != nullptr)
            {
                gui::PropertyInfo& row = m_bag->Add(name, gui::PropertyKind::Reference, gui::Variant(GuidText(*static_cast<const Guid*>(address)).AsView()));
                row.ReferenceType = SourceTypeNamed(assetType->AsView());
                if (emptyText != nullptr)
                    row.Description = Format(u8"None is {}", *emptyText);
                return &row;
            }
            if (engine::project::IsAssetListSetting(property) && assetType != nullptr)
            {
                const Array<Guid>& ids = *static_cast<const Array<Guid>*>(address);
                (void)m_bag->AddList(name, gui::PropertyKind::Reference, gui::Variant(StringView{}), ids.Size());
                for (usize i = 0; i < ids.Size(); ++i)
                    m_bag->SetValue(Format(u8"{}.{}", name, i).AsView(), 0, gui::Variant(GuidText(ids[i]).AsView()));
                if (gui::PropertyInfo* element = m_bag->FindElementInfo(name))
                    element->ReferenceType = SourceTypeNamed(assetType->AsView());
                return m_bag->FindInfo(name);
            }
            if (name == kMsaaSetting)
            {
                gui::PropertyInfo& row = m_bag->Add(name, gui::PropertyKind::Enum, gui::Variant(static_cast<i64>(*static_cast<const u32*>(address))));
                for (u32 i = 0; i < engine::render::MsaaLevelCount(); ++i)
                    row.Choices.PushBack(gui::PropertyChoice{String(engine::render::kMsaaLevels[i].label), static_cast<i64>(engine::render::kMsaaLevels[i].samples)});
                return &row;
            }
            if (IsEnum(type))
            {
                gui::PropertyInfo& row = m_bag->Add(name, gui::PropertyKind::Enum, gui::Variant(ReadEnumValue(address, type)));
                for (usize i = 0; i < EnumeratorCount(type); ++i)
                {
                    const EnumValue& value = EnumeratorAt(type, i);
                    row.Choices.PushBack(gui::PropertyChoice{String(StringView(reinterpret_cast<const utf8char*>(value.name))), value.value});
                }
                return &row;
            }
            if (&type == &TypeOf<String>())
                return &m_bag->Add(name, gui::PropertyKind::String, gui::Variant(static_cast<const String*>(address)->AsView()));
            if (&type == &TypeOf<bool>())
                return &m_bag->Add(name, gui::PropertyKind::Bool, gui::Variant(*static_cast<const bool*>(address)));
            if (&type == &TypeOf<u32>())
            {
                gui::PropertyInfo& row = m_bag->Add(name, gui::PropertyKind::Integer, gui::Variant(static_cast<i64>(*static_cast<const u32*>(address))));
                if (const Attribute* range = FindAttribute(property, u8"range"))
                    if (const Float4* bounds = range->value.TryGet<Float4>())
                    {
                        row.HasRange = true;
                        row.Min = bounds->x;
                        row.Max = bounds->y;
                        row.Step = bounds->z;
                    }
                return &row;
            }
            return nullptr;
        }
        // Writes the rows into `settings`, and each loaded plugin's into its object and its entry.
        void WriteBack(engine::project::ProjectSettings& settings)
        {
            if (!m_bag)
                return;
            WriteRows(Instance(&settings, &SettingsType()), SettingsType(), StringView{});
            for (const PluginSettingsRows& plugin : m_pluginSettings)
            {
                foundation::runtime::EnabledPlugin* entry = settings.FindPlugin(plugin.id.AsView());
                if (entry == nullptr)
                    continue;
                const String group = Format(u8"plugins.{}", plugin.id);
                WriteRows(Instance(plugin.object, plugin.object->GetType()), *plugin.object->GetType(), group.AsView());
                if (!foundation::runtime::WritePluginSettings(*plugin.object, entry->settings, m_allocator).IsOk())
                    LOG_ERROR(u8"Editor", u8"plugin '{}': its settings did not write into the project", plugin.id);
            }
            // The entries left unchecked go, with their settings; the plugins checked join, on every
            // platform their manifests support.
            m_pluginsChanged = false;
            for (usize i = settings.plugins.Size(); i-- > 0;)
            {
                const String path = Format(u8"plugins.{}.enabled", settings.plugins[i].id);
                if (m_bag->FindInfo(path) != nullptr && !m_bag->GetValue(path, 0).AsBool())
                {
                    settings.plugins.RemoveAt(i);
                    m_pluginsChanged = true;
                }
            }
            for (usize i = 0; i < m_available.Size(); ++i)
            {
                const engine::project::AvailablePlugin& plugin = m_available[i];
                const String path = Format(u8"available.{}.enabled", i);
                if (!plugin.problem.IsEmpty() || m_bag->FindInfo(path) == nullptr || !m_bag->GetValue(path, 0).AsBool() ||
                    settings.FindPlugin(plugin.id.AsView()) != nullptr)
                    continue;
                foundation::runtime::EnabledPlugin entry;
                entry.id = String(plugin.id.AsView(), m_allocator);
                entry.path = String(plugin.path.AsView(), m_allocator);
                settings.plugins.PushBack(Move(entry));
                m_pluginsChanged = true;
            }
        }
        // Writes the rows under `group` (top level when empty) into `instance`, a `type`.
        void WriteRows(const Instance& instance, const TypeInfo& type, StringView group)
        {
            for (const PropertyInfo& property : Properties(type))
            {
                const String name = group.IsEmpty() ? String(NameOf(property)) : Format(u8"{}.{}", group, NameOf(property));
                if (m_bag->FindInfo(name) == nullptr || property.address == nullptr || m_bag->FindInfo(name)->ReadOnly)
                    continue;
                void* address = property.address(instance);
                const gui::Variant value = m_bag->GetValue(name, 0);
                const TypeInfo& type = *property.type;
                if (engine::project::IsAssetSetting(property))
                {
                    Guid id;
                    (void)Guid::TryParse(Trim(value.AsString()), id); // empty: none
                    *static_cast<Guid*>(address) = id;
                }
                else if (engine::project::IsAssetListSetting(property))
                {
                    Array<Guid>& ids = *static_cast<Array<Guid>*>(address);
                    ids.Clear();
                    for (usize i = 0; i < m_bag->ElementCount(name, 0); ++i)
                    {
                        Guid id;
                        if (!Guid::TryParse(Trim(m_bag->GetValue(Format(u8"{}.{}", name, i).AsView(), 0).AsString()), id) || id.IsNil())
                            continue; // a slot never picked
                        bool listed = false;
                        for (const Guid& other : ids)
                            listed = listed || other == id;
                        if (!listed)
                            ids.PushBack(id);
                    }
                }
                else if (IsEnum(type))
                    WriteEnumValue(address, type, value.AsInt());
                else if (&type == &TypeOf<String>())
                    *static_cast<String*>(address) = String(value.AsString());
                else if (&type == &TypeOf<bool>())
                    *static_cast<bool*>(address) = value.AsBool();
                else if (&type == &TypeOf<u32>())
                    *static_cast<u32*>(address) = static_cast<u32>(Max<i64>(0, value.AsInt()));
            }
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

        // A loaded plugin whose settings the rows show, and the object they write into.
        struct PluginSettingsRows
        {
            String id;
            ISerializable* object = nullptr; // the plugin host's, alive while the plugin is loaded
        };

        IAllocator& m_allocator;
        editor::EditorContext* m_context;
        Span<const foundation::runtime::LinkedPlugin> m_linked; // the plugins the editor links
        Array<PluginSettingsRows> m_pluginSettings; // rebuilt at each open
        Array<engine::project::AvailablePlugin> m_available; // what the project could enable, at each open
        bool m_pluginsChanged = false; // the last Save added or removed an entry
        RefPtr<gui::Node> m_body;
        RefPtr<gui::Window> m_window;
        gui::PropertyGrid* m_grid = nullptr;
        UniquePtr<gui::PropertyBag> m_bag; // rebuilt at each open
    };
}
