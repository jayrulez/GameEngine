// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::App - :project_home partition.
//
// ProjectHomeView: the Project panel with a project open. Its frame is markup
// (Data/Editor/ProjectHome.xml), in two columns. On the left it fills in the project: its picture,
// name, location and engine version, the settings that sum it up (those whose reflection carries
// `summary`, by that label, each asset a link that opens it), and the player's window; Show in
// Folder and Project Settings are under them. On the right are the project's scenes (the assets of
// the default-scene setting's type), each a card with its thumbnail once the thumbnail service has
// one (its type's icon meanwhile) that opens it, or, with none, a note with New Scene. Nothing here
// keeps a list of its own: the rows come from ProjectSettings' reflection.
//
// Refresh (each frame) fills it again when the settings changed (MarkChanged, from the settings'
// save), the cook revision moved (assets added, renamed or removed) or the project's picture
// changed, and asks again for the thumbnails the cards still lack.
module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module editor.app:project_home;

import foundation.core;
import foundation.content;
import foundation.gui;
import foundation.image;
import engine.project;
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
    /// ProjectHomeView::kFirstProjectPage, at namespace scope for the same reason.
    inline constexpr const char8_t* kProjectHomeFirstProjectPage = u8"getting-started/first-project/";

    class ProjectHomeView
    {
    public:
        /// Its frame's markup path, under the data root.
        static constexpr const char8_t* kPath = kProjectHomeViewPath;
        /// The documentation page the note for a project with no scenes points at, under the site.
        static constexpr const char8_t* kFirstProjectPage = kProjectHomeFirstProjectPage;

        /// Builds the view over `context`'s project in the frame `content` (from the markup);
        /// `cook` (nullable) tells it when the project's assets changed.
        ProjectHomeView(IAllocator& allocator, editor::EditorContext& context, editor::EditorCookService* cook, RefPtr<gui::Node> content)
            : m_allocator(allocator), m_context(&context), m_cook(cook), m_content(Move(content)), m_home(GetHomeDirectory()), m_pictures(allocator),
              m_scenes(allocator), m_rows(allocator), m_cards(allocator)
        {
            ProjectHomeView* self = this;
            const auto wire = [this](StringView id, Function<void()> onClick)
            {
                if (auto* button = Find<gui::Button>(id))
                    button->SetOnClick(Move(onClick));
            };
            wire(u8"home-show-folder", [self]()
                 {
                     if (self->m_onShowFolder && self->m_context->Project() != nullptr)
                         self->m_onShowFolder(self->m_context->Project()->Directory());
                 });
            wire(u8"home-settings", [self]()
                 {
                     if (self->m_onProjectSettings)
                         self->m_onProjectSettings();
                 });
            const char8_t* const creators[] = {u8"home-new-scene", u8"home-first-scene"};
            for (const char8_t* id : creators)
                wire(id, [self]()
                     {
                         if (self->m_onNewScene)
                             self->m_onNewScene();
                     });
            wire(u8"home-first-docs", [self]()
                 {
                     if (self->m_onOpenDocs)
                         self->m_onOpenDocs(kFirstProjectPage);
                 });
            Rebuild();
        }

        /// Opens the asset with this guid in its page (the application's open path).
        void SetOnOpenAsset(Function<void(const Guid&)> open) { m_onOpenAsset = Move(open); }
        /// Makes a scene and opens it (File > New Scene).
        void SetOnNewScene(Function<void()> create) { m_onNewScene = Move(create); }
        /// Shows the project's folder in the system's file manager.
        void SetOnShowFolder(Function<void(StringView folder)> show) { m_onShowFolder = Move(show); }
        /// Opens the Project Settings dialog.
        void SetOnProjectSettings(Function<void()> open) { m_onProjectSettings = Move(open); }
        /// Opens a page of the documentation site (its path under the site, as kFirstProjectPage).
        void SetOnOpenDocs(Function<void(StringView page)> open) { m_onOpenDocs = Move(open); }

        [[nodiscard]] RefPtr<gui::Node> Content() const { return m_content; }

        /// Marks the settings as changed, so the view is filled again at the next Refresh.
        void MarkChanged() noexcept { m_changed = true; }

        /// Fills the view again when something it shows changed (see the file header), and shows the
        /// scene thumbnails that have become ready.
        void Refresh()
        {
            const u64 revision = m_cook != nullptr ? m_cook->Revision() : 0;
            if (m_changed || revision != m_cookRevision || ThumbnailTime() != m_thumbnailTime)
                Rebuild();
            ShowThumbnails();
        }

        void Rebuild()
        {
            m_changed = false;
            m_cookRevision = m_cook != nullptr ? m_cook->Revision() : 0;
            m_thumbnailTime = ThumbnailTime();
            m_scenes.Clear();
            m_rows.Clear();
            m_cards.Clear();
            editor::EditorProject* project = m_context->Project();
            if (!m_content || project == nullptr)
                return;
            Side(*project);
            SceneCards(*project);
            ShowThumbnails();
        }

        // What it shows, for tests: the scenes (in order), each summary row as "Label: value", and
        // how many cards show their thumbnail.
        [[nodiscard]] Span<const Guid> Scenes() const noexcept { return m_scenes.AsSpan(); }
        [[nodiscard]] Span<const String> SummaryRows() const noexcept { return m_rows.AsSpan(); }
        [[nodiscard]] usize ThumbnailsShown() const noexcept
        {
            usize shown = 0;
            for (const Card& card : m_cards)
                shown += card.shown ? 1 : 0;
            return shown;
        }

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
        void OpenAsset(const Guid& id)
        {
            if (m_onOpenAsset)
                m_onOpenAsset(id);
        }
        // An asset setting's value: a link that opens the asset, its empty text when unset, or a
        // warning when the asset is gone. `text` receives what it says.
        [[nodiscard]] RefPtr<gui::Node> AssetValue(const Guid& id, StringView emptyText, String& text)
        {
            if (id.IsNil())
            {
                text = String(emptyText);
                return RefPtr<gui::Node>(Text(emptyText, u8"home-value dim"));
            }
            const content::Instance* instance = m_context->Project()->SourceDb().GetInstance(id);
            if (instance == nullptr)
            {
                text = String(u8"(missing)");
                return RefPtr<gui::Node>(Text(text.AsView(), u8"home-value missing"));
            }
            text = String(instance->Name());
            ProjectHomeView* self = this;
            RefPtr<gui::Button> link = MakeRef<gui::Button>(m_allocator);
            link->AddClass(u8"link");
            link->SetText(instance->Name());
            link->SetTooltip(instance->Path());
            link->SetOnClick([self, id]() { self->OpenAsset(id); });
            return RefPtr<gui::Node>(link);
        }
        void AddRow(gui::Node& summary, StringView label, RefPtr<gui::Node> value, StringView text)
        {
            RefPtr<gui::Panel> row = MakeRef<gui::Panel>(m_allocator);
            row->AddClass(u8"home-row");
            row->AddChild(Text(label, u8"home-label").Get());
            row->AddChild(value.Get());
            summary.AddChild(row.Get());
            m_rows.PushBack(Format(u8"{}: {}", label, text));
        }

        // Fills the left column: the picture, the name, and the summary.
        void Side(editor::EditorProject& project)
        {
            namespace proj = engine::project;
            proj::ProjectSettings& settings = project.Settings();
            if (gui::Node* slot = Find<gui::Node>(u8"home-picture"))
            {
                Clear(*slot);
                m_pictures.Clear();
                slot->AddChild(ProjectPicture::Make(m_allocator, project.Directory(), settings.name.AsView(), u8"home-picture", m_pictures).Get());
            }
            if (auto* name = Find<gui::Label>(u8"home-name"))
                name->SetText(settings.name.IsEmpty() ? StringView(u8"(unnamed)") : settings.name.AsView());
            gui::Node* summary = Find<gui::Node>(u8"home-summary");
            if (summary == nullptr)
                return;
            Clear(*summary);
            const String location = RecentProjects::ShortenHomePath(project.Directory(), m_home.AsView());
            RefPtr<gui::Label> where = Text(location.AsView(), u8"home-value");
            where->SetTooltip(project.Directory());
            AddRow(*summary, u8"Location", RefPtr<gui::Node>(where), location.AsView());
            AddRow(*summary, u8"Engine version", RefPtr<gui::Node>(Text(settings.engineVersion.AsView(), u8"home-value")), settings.engineVersion.AsView());
            const TypeInfo& type = proj::ProjectSettings::StaticType();
            const Instance instance(&settings, &type);
            for (const PropertyInfo& property : Properties(type))
            {
                const String* label = proj::SettingAttribute(property, proj::kSettingSummaryAttribute);
                if (label == nullptr || !proj::IsAssetSetting(property))
                    continue; // the summary names assets; the window has its own row
                const String* empty = proj::SettingAttribute(property, proj::kSettingEmptyTextAttribute);
                String text;
                RefPtr<gui::Node> value = AssetValue(*static_cast<const Guid*>(property.address(instance)),
                                                     empty != nullptr ? empty->AsView() : StringView(u8"Not set"), text);
                AddRow(*summary, label->AsView(), Move(value), text.AsView());
            }
            const String window = settings.WindowSummary();
            AddRow(*summary, u8"Window", RefPtr<gui::Node>(Text(window.AsView(), u8"home-value")), window.AsView());
        }

        // Fills the right column: a card per scene, or the note for a project with none.
        void SceneCards(editor::EditorProject& project)
        {
            const engine::project::ProjectSettings& settings = project.Settings();
            gui::UIWidget* grid = Find<gui::UIWidget>(u8"home-scenes");
            if (grid == nullptr)
                return;
            Clear(*grid);
            const PropertyInfo* defaultScene = FindProperty(engine::project::ProjectSettings::StaticType(), "defaultSceneId");
            const String* sceneType = defaultScene != nullptr ? engine::project::SettingAttribute(*defaultScene, engine::project::kSettingAssetTypeAttribute) : nullptr;
            Array<const content::Instance*> scenes;
            if (sceneType != nullptr)
                // (A group keeps its instances and its groups in name order, so the walk is in order.)
                CollectInstances(*project.SourceDb().RootGroup(), sceneType->AsView(), scenes);
            if (auto* count = Find<gui::Label>(u8"home-scene-count"))
                count->SetText(Format(u8"{}", scenes.Size()).AsView());
            const bool any = !scenes.IsEmpty();
            grid->SetDisplayed(any);
            if (auto* none = Find<gui::UIWidget>(u8"home-no-scenes"))
                none->SetDisplayed(!any);
            if (auto* create = Find<gui::UIWidget>(u8"home-new-scene"))
                create->SetDisplayed(any); // with none, the note has its own
            const String icon = Format(u8"Editor/Icons/Types/{}.svg", sceneType != nullptr ? sceneType->AsView() : StringView(u8"Asset"));
            ProjectHomeView* self = this;
            for (const content::Instance* scene : scenes)
            {
                const Guid id = scene->Id();
                RefPtr<gui::Button> card = MakeRef<gui::Button>(m_allocator);
                card->AddClass(u8"home-scene");
                card->SetTooltip(scene->Path());
                card->SetOnClick([self, id]() { self->OpenAsset(id); });
                RefPtr<gui::Panel> picture = MakeRef<gui::Panel>(m_allocator);
                picture->AddClass(u8"home-scene-picture");
                picture->SetHitTestVisible(false);
                RefPtr<gui::Image> stand = MakeRef<gui::Image>(m_allocator);
                stand->AddClass(u8"home-scene-icon");
                stand->SetSource(icon);
                picture->AddChild(stand.Get());
                if (id == settings.defaultSceneId)
                    picture->AddChild(Text(u8"Default", u8"badge info").Get());
                card->AddChild(picture.Get());
                RefPtr<gui::Panel> body = MakeRef<gui::Panel>(m_allocator);
                body->AddClass(u8"home-scene-body");
                body->SetHitTestVisible(false);
                body->AddChild(Text(scene->Name(), u8"home-scene-name").Get());
                body->AddChild(Text(scene->Path(), u8"home-scene-path").Get());
                card->AddChild(body.Get());
                grid->AddChild(card.Get());
                m_scenes.PushBack(id);
                m_cards.PushBack(Card{id, picture.Get(), stand.Get(), false});
            }
        }

        // Puts each ready thumbnail in its card, in place of the type's icon.
        void ShowThumbnails()
        {
            editor::ThumbnailService* thumbs = m_context->Thumbnails();
            if (thumbs == nullptr)
                return;
            for (Card& card : m_cards)
            {
                if (card.shown)
                    continue;
                RefPtr<gui::Drawable> drawable = thumbs->Get(card.id);
                if (!drawable)
                    continue;
                RefPtr<gui::Image> thumb = MakeRef<gui::Image>(m_allocator);
                thumb->AddClass(u8"home-scene-thumb");
                thumb->SetFit(gui::ObjectFit::Cover);
                thumb->SetDrawable(drawable);
                card.icon->SetDisplayed(false);
                (void)card.picture->AddChildAt(thumb.Get(), 0);
                card.shown = true;
            }
        }

        static void CollectInstances(const content::Group& group, StringView typeName, Array<const content::Instance*>& out)
        {
            for (const content::Instance* instance : group.Instances())
                if (instance->TypeName() == typeName)
                    out.PushBack(instance);
            for (const content::Group* child : group.Groups())
                CollectInstances(*child, typeName, out);
        }

        // A scene's card: where its thumbnail goes, the icon shown until then.
        struct Card
        {
            Guid id;
            gui::Panel* picture = nullptr;
            gui::Image* icon = nullptr;
            bool shown = false;
        };

        IAllocator& m_allocator;
        editor::EditorContext* m_context;
        editor::EditorCookService* m_cook;
        RefPtr<gui::Node> m_content;
        String m_home; // shortens the location to ~
        Function<void(const Guid&)> m_onOpenAsset;
        Function<void()> m_onNewScene;
        Function<void(StringView)> m_onShowFolder;
        Function<void()> m_onProjectSettings;
        Function<void(StringView)> m_onOpenDocs;
        Array<UniquePtr<foundation::image::Image>> m_pictures; // what the picture shows
        bool m_changed = false;
        u64 m_cookRevision = 0;
        i64 m_thumbnailTime = 0;
        Array<Guid> m_scenes;
        Array<String> m_rows;
        Array<Card> m_cards;
    };
}
