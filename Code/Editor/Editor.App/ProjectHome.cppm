// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::App - :project_home partition.
//
// ProjectHomeView: the Project panel with a project open. Its frame is markup
// (Data/Editor/ProjectHome.xml); it fills in the project's picture, name, engine and folder; a
// Start column (open the default scene, the Game menu's actions, every scene in the project); and
// a Project column (every setting the project's settings reflect, by category, each asset a link
// that opens it, then the Project menu's actions). Nothing here keeps a list of its own: the
// settings come from ProjectSettings' reflection, the actions from the registry's Game and Project
// menus, and the scenes are the assets of the default-scene setting's type.
//
// Refresh (each frame) fills it again when the settings changed (MarkChanged, from the settings'
// save), the cook revision moved (assets added, renamed or removed) or the thumbnail changed.
module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module editor.app:project_home;

import foundation.core;
import foundation.content;
import foundation.gui;
import foundation.image;
import engine.project;
import engine.render; // the MSAA levels' labels (as the settings dialog shows them)
import editor.core;
import :project_picture;

using namespace foundation::core;

export namespace editor::app
{
    namespace gui = foundation::gui;
    namespace content = foundation::content;

    /// ProjectHomeView::kPath, at namespace scope for another library to read (MSVC exports no class's
    /// static constexpr member from a shared library).
    inline constexpr const char8_t* kProjectHomeViewPath = u8"Editor/ProjectHome.xml";

    class ProjectHomeView
    {
    public:
        /// Its frame's markup path, under the data root.
        static constexpr const char8_t* kPath = kProjectHomeViewPath;

        /// Builds the view over `context`'s project in the frame `content` (from the markup);
        /// `cook` (nullable) tells it when the project's assets changed.
        ProjectHomeView(IAllocator& allocator, editor::EditorContext& context, editor::EditorCookService* cook, RefPtr<gui::Node> content)
            : m_allocator(allocator), m_context(&context), m_cook(cook), m_content(Move(content)), m_pictures(allocator), m_scenes(allocator),
              m_rows(allocator), m_actions(allocator)
        {
            Rebuild();
        }

        /// Opens the asset with this guid in its page (the application's open path).
        void SetOnOpenAsset(Function<void(const Guid&)> open) { m_onOpenAsset = Move(open); }

        [[nodiscard]] RefPtr<gui::Node> Content() const { return m_content; }

        /// Marks the settings as changed, so the view is filled again at the next Refresh.
        void MarkChanged() noexcept { m_changed = true; }

        /// Fills the view again when something it shows changed (see the file header).
        void Refresh()
        {
            const u64 revision = m_cook != nullptr ? m_cook->Revision() : 0;
            if (m_changed || revision != m_cookRevision || ThumbnailTime() != m_thumbnailTime)
                Rebuild();
        }

        void Rebuild()
        {
            m_changed = false;
            m_cookRevision = m_cook != nullptr ? m_cook->Revision() : 0;
            m_thumbnailTime = ThumbnailTime();
            m_scenes.Clear();
            m_rows.Clear();
            m_actions.Clear();
            editor::EditorProject* project = m_context->Project();
            if (!m_content || project == nullptr)
                return;
            Header(*project);
            StartColumn(*project);
            ProjectColumn(*project);
        }

        // What it shows, for tests: the scenes listed (in order), each setting row as
        // "Label: value", and the actions offered (ids).
        [[nodiscard]] Span<const Guid> Scenes() const noexcept { return m_scenes.AsSpan(); }
        [[nodiscard]] Span<const String> SettingRows() const noexcept { return m_rows.AsSpan(); }
        [[nodiscard]] Span<const String> ActionIds() const noexcept { return m_actions.AsSpan(); }

    private:
        template <typename T>
        [[nodiscard]] T* Find(StringView id) const
        {
            return m_content ? Cast<T>(FindIn(*m_content, id)) : nullptr;
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
        static void Clear(gui::Node& node)
        {
            while (node.ChildCount() > 0)
                node.RemoveChild(node.GetChildAt(node.ChildCount() - 1));
        }

        [[nodiscard]] i64 ThumbnailTime() const
        {
            const editor::EditorProject* project = m_context->Project();
            u64 size = 0;
            i64 time = 0;
            if (project == nullptr || !FileStat(ProjectThumbnailPath(project->Directory()).AsView(), size, time))
                return 0;
            return time;
        }

        [[nodiscard]] RefPtr<gui::Label> Text(StringView text, StringView classes = {})
        {
            RefPtr<gui::Label> label = MakeRef<gui::Label>(m_allocator);
            label->SetText(text);
            if (!classes.IsEmpty())
                label->AddClasses(classes);
            return label;
        }
        // Makes a link: a flat button with the text, at its own width.
        [[nodiscard]] RefPtr<gui::Button> Link(StringView text, StringView tooltip, Function<void()> onClick)
        {
            RefPtr<gui::Button> link = MakeRef<gui::Button>(m_allocator);
            link->AddClass(u8"link");
            link->SetText(text);
            if (!tooltip.IsEmpty())
                link->SetTooltip(tooltip);
            link->SetOnClick(Move(onClick));
            return link;
        }
        void OpenAsset(const Guid& id)
        {
            if (m_onOpenAsset)
                m_onOpenAsset(id);
        }
        [[nodiscard]] RefPtr<gui::Node> AssetLink(const Guid& id, StringView emptyText)
        {
            if (id.IsNil())
                return RefPtr<gui::Node>(Text(emptyText, u8"home-value dim"));
            const content::Instance* instance = m_context->Project()->SourceDb().GetInstance(id);
            if (instance == nullptr)
                return RefPtr<gui::Node>(Text(u8"(missing)", u8"home-value missing"));
            ProjectHomeView* self = this;
            return RefPtr<gui::Node>(Link(instance->Name(), instance->Path(), [self, id]() { self->OpenAsset(id); }));
        }

        // Fills the header: the picture, the name, the engine and the folder.
        void Header(const editor::EditorProject& project)
        {
            const engine::project::ProjectSettings& settings = project.Settings();
            if (gui::Node* slot = Find<gui::Node>(u8"home-picture"))
            {
                Clear(*slot);
                m_pictures.Clear();
                slot->AddChild(ProjectPicture::Make(m_allocator, project.Directory(), settings.name.AsView(), u8"home-picture", m_pictures).Get());
            }
            if (auto* name = Find<gui::Label>(u8"home-name"))
                name->SetText(settings.name.IsEmpty() ? StringView(u8"(unnamed)") : settings.name.AsView());
            if (auto* engine = Find<gui::Label>(u8"home-engine"))
                engine->SetText(Format(u8"AssiduousEngine {}", settings.engineVersion).AsView());
            if (auto* folder = Find<gui::Label>(u8"home-folder"))
            {
                folder->SetText(project.Directory());
                folder->SetTooltip(project.Directory());
            }
        }

        // Adds the actions under `menu` ("Game/") to `into` as buttons, in menu order.
        void MenuActions(StringView menu, gui::Node& into)
        {
            Clear(into);
            Array<const editor::EditorActionDeclaration*> found;
            for (const editor::EditorActionDeclaration& action : m_context->Actions().Actions())
                if (action.menuPath.AsView().StartsWith(menu))
                    found.PushBack(&action);
            found.Sort([](const editor::EditorActionDeclaration* a, const editor::EditorActionDeclaration* b) { return a->menuOrder < b->menuOrder; });
            editor::EditorContext* context = m_context;
            for (const editor::EditorActionDeclaration* action : found)
            {
                RefPtr<gui::Button> button = MakeRef<gui::Button>(m_allocator);
                button->SetText(action->label.AsView());
                button->SetTooltip(action->description.AsView());
                button->SetEnabled(editor::EditorActionRegistry::IsEnabled(*action, nullptr));
                const String id = action->id;
                button->SetOnClick([context, id]() { (void)context->Actions().Execute(id.AsView(), nullptr); });
                into.AddChild(button.Get());
                m_actions.PushBack(action->id);
            }
        }

        // Fills the Start column: open the default scene, play, and every scene in the project.
        void StartColumn(editor::EditorProject& project)
        {
            const engine::project::ProjectSettings& settings = project.Settings();
            ProjectHomeView* self = this;
            if (auto* open = Find<gui::Button>(u8"home-open-default"))
            {
                const content::Instance* scene = settings.defaultSceneId.IsNil() ? nullptr : project.SourceDb().GetInstance(settings.defaultSceneId);
                open->SetText(scene != nullptr ? Format(u8"Open {}", scene->Name()).AsView() : StringView(u8"No default scene"));
                open->SetEnabled(scene != nullptr);
                const Guid id = settings.defaultSceneId;
                open->SetOnClick([self, id]() { self->OpenAsset(id); });
            }
            if (gui::Node* game = Find<gui::Node>(u8"home-game-actions"))
                MenuActions(u8"Game/", *game);

            // The scenes: every asset of the type the default-scene setting names.
            gui::Node* list = Find<gui::Node>(u8"home-scenes");
            auto* heading = Find<gui::Label>(u8"home-scenes-heading");
            if (list == nullptr)
                return;
            Clear(*list);
            const PropertyInfo* defaultScene = FindProperty(engine::project::ProjectSettings::StaticType(), "defaultSceneId");
            const String* sceneType = defaultScene != nullptr ? engine::project::SettingAttribute(*defaultScene, engine::project::kSettingAssetTypeAttribute) : nullptr;
            Array<const content::Instance*> scenes;
            if (sceneType != nullptr)
                // (A group keeps its instances and its groups in name order, so the walk is in order.)
                CollectInstances(*project.SourceDb().RootGroup(), sceneType->AsView(), scenes);
            if (heading != nullptr)
                heading->SetText(Format(u8"Scenes ({})", scenes.Size()).AsView());
            for (const content::Instance* scene : scenes)
            {
                const Guid id = scene->Id();
                const bool isDefault = id == settings.defaultSceneId;
                list->AddChild(Link(isDefault ? Format(u8"{} (default)", scene->Name()).AsView() : scene->Name(), scene->Path(),
                                    [self, id]() { self->OpenAsset(id); })
                                   .Get());
                m_scenes.PushBack(id);
            }
            if (scenes.IsEmpty())
                list->AddChild(Text(u8"No scenes yet.", u8"home-value dim").Get());
        }

        static void CollectInstances(const content::Group& group, StringView typeName, Array<const content::Instance*>& out)
        {
            for (const content::Instance* instance : group.Instances())
                if (instance->TypeName() == typeName)
                    out.PushBack(instance);
            for (const content::Group* child : group.Groups())
                CollectInstances(*child, typeName, out);
        }

        // A setting's value as text: a choice by its name, a flag as On or Off, a number, a string
        // ("Not set" when empty). Empty for a kind it does not show.
        [[nodiscard]] static String SettingText(const PropertyInfo& property, const void* address)
        {
            if (IsEnum(*property.type))
            {
                const i64 current = ReadEnumValue(address, *property.type);
                for (usize i = 0; i < EnumeratorCount(*property.type); ++i)
                {
                    const EnumValue& value = EnumeratorAt(*property.type, i);
                    if (value.value == current)
                        return String(StringView(reinterpret_cast<const utf8char*>(value.name)));
                }
                return {};
            }
            if (property.type == &TypeOf<bool>())
                return String(*static_cast<const bool*>(address) ? u8"On" : u8"Off");
            if (property.type == &TypeOf<u32>())
                return Format(u8"{}", *static_cast<const u32*>(address));
            if (property.type == &TypeOf<String>())
            {
                const String& text = *static_cast<const String*>(address);
                return text.IsEmpty() ? String(u8"Not set") : text;
            }
            return {};
        }

        // Fills the Project column: every reflected setting by category, then the Project menu's actions.
        void ProjectColumn(editor::EditorProject& project)
        {
            namespace proj = engine::project;
            gui::Node* column = Find<gui::Node>(u8"home-settings");
            if (column == nullptr)
                return;
            Clear(*column);
            const TypeInfo& type = proj::ProjectSettings::StaticType();
            const Instance settings(&project.Settings(), &type);
            const PropertyInfo* msaa = FindProperty(type, "renderMsaaSamples");

            // The categories in the order the settings first name them.
            Array<String> categories;
            for (const PropertyInfo& property : Properties(type))
            {
                if (proj::SettingAttribute(property, proj::kSettingLabelAttribute) == nullptr)
                    continue;
                const StringView category = proj::SettingCategory(property);
                bool seen = false;
                for (const String& known : categories)
                    seen = seen || known.AsView() == category;
                if (!seen)
                    categories.PushBack(String(category));
            }
            for (const String& category : categories)
            {
                column->AddChild(Text(category.AsView(), u8"home-category").Get());
                for (const PropertyInfo& property : Properties(type))
                {
                    const String* label = proj::SettingAttribute(property, proj::kSettingLabelAttribute);
                    if (label == nullptr || proj::SettingCategory(property) != category.AsView())
                        continue;
                    const void* address = property.address(settings);
                    RefPtr<gui::Node> value;
                    String text;
                    if (proj::IsAssetSetting(property))
                    {
                        const Guid& id = *static_cast<const Guid*>(address);
                        const String* empty = proj::SettingAttribute(property, proj::kSettingEmptyTextAttribute);
                        const StringView emptyText = empty != nullptr ? empty->AsView() : StringView(u8"Not set");
                        value = AssetLink(id, emptyText);
                        const content::Instance* instance = id.IsNil() ? nullptr : project.SourceDb().GetInstance(id);
                        text = id.IsNil() ? String(emptyText) : instance != nullptr ? String(instance->Name()) : String(u8"(missing)");
                    }
                    else if (proj::IsAssetListSetting(property))
                    {
                        const Array<Guid>& ids = *static_cast<const Array<Guid>*>(address);
                        RefPtr<gui::Panel> list = MakeRef<gui::Panel>(m_allocator);
                        list->AddClass(u8"home-links");
                        for (const Guid& id : ids)
                        {
                            list->AddChild(AssetLink(id, u8"Not set").Get());
                            const content::Instance* instance = project.SourceDb().GetInstance(id);
                            text += text.IsEmpty() ? u8"" : u8", ";
                            text += instance != nullptr ? instance->Name() : StringView(u8"(missing)");
                        }
                        if (ids.IsEmpty())
                        {
                            text = String(u8"Not set");
                            list->AddChild(Text(text.AsView(), u8"home-value dim").Get());
                        }
                        value = RefPtr<gui::Node>(list);
                    }
                    else
                    {
                        if (&property == msaa)
                        {
                            const u32 samples = *static_cast<const u32*>(address);
                            text = String(engine::render::kMsaaLevels[engine::render::MsaaIndexForSamples(samples)].label);
                        }
                        else
                            text = SettingText(property, address);
                        if (text.IsEmpty())
                            continue; // a kind this page does not show
                        value = RefPtr<gui::Node>(Text(text.AsView(), u8"home-value"));
                    }
                    m_rows.PushBack(Format(u8"{}: {}", label->AsView(), text.AsView()));
                    RefPtr<gui::Panel> row = MakeRef<gui::Panel>(m_allocator);
                    row->AddClass(u8"home-setting");
                    row->AddChild(Text(label->AsView(), u8"home-label").Get());
                    row->AddChild(value.Get());
                    column->AddChild(row.Get());
                }
            }
            if (gui::Node* actions = Find<gui::Node>(u8"home-project-actions"))
                MenuActions(u8"Project/", *actions);
        }

        IAllocator& m_allocator;
        editor::EditorContext* m_context;
        editor::EditorCookService* m_cook;
        RefPtr<gui::Node> m_content;
        Function<void(const Guid&)> m_onOpenAsset;
        Array<UniquePtr<foundation::image::Image>> m_pictures; // what the header's picture shows
        bool m_changed = false;
        u64 m_cookRevision = 0;
        i64 m_thumbnailTime = 0;
        Array<Guid> m_scenes;
        Array<String> m_rows;
        Array<String> m_actions;
    };
}
