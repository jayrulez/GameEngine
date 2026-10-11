// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::App - :project_home partition.
//
// ProjectHomeView: the Project page, the panel that holds the document area's place (with every
// page closed, a newly opened one docks beside it rather than among the tool panels). It shows
// the open project: its picture, name, engine and folder; a Start column (open the default
// scene, the Game menu's actions, every scene in the project); and a Project column (every
// setting the project's settings reflect, by category, each asset a link that opens it, then
// the Project menu's actions). Nothing here is a list of its own: the settings are
// ProjectSettings' reflection, the actions the registry's Game and Project menus, the scenes the
// assets of the default-scene setting's type.
//
// Refresh (each frame) rebuilds it when the settings changed (MarkChanged, from the settings'
// save), the cook revision moved (assets added, renamed or removed) or the thumbnail changed.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module editor.app:project_home;

import foundation.core;
import foundation.content;
import foundation.ui;
import foundation.image;
import engine.project;
import engine.render; // the MSAA level labels (as the settings dialog shows them)
import editor.core;
import :project_picture;

using namespace foundation::core;

export namespace editor::app
{
    namespace ui = foundation::ui;
    namespace content = foundation::content;

    class ProjectHomeView
    {
    public:
        /// `cook` (nullable) tells when the project's assets changed.
        ProjectHomeView(IAllocator& allocator, editor::EditorContext& context, editor::EditorCookService* cook)
            : m_allocator(&allocator), m_context(&context), m_cook(cook)
        {
            m_scroll = MakeRef<ui::ScrollView>(allocator);
            m_scroll->VScrollBarPolicy.SetValue(ui::ScrollBarPolicy::Auto);
            m_scroll->HScrollBarPolicy.SetValue(ui::ScrollBarPolicy::Never);
            m_page = MakeRef<ui::FlexLayout>(allocator);
            m_page->Direction = ui::Orientation::Vertical;
            m_page->Spacing = 22;
            m_page->Padding = ui::Thickness{28, 24};
            ui::LayoutStyle match;
            match.Width = ui::SizeSpec::Match();
            m_scroll->AddView(m_page.Get(), match);
            Rebuild();
        }

        /// Open the asset with this guid in its page (the app's open path).
        Function<void(const Guid&)> OnOpenAsset;

        [[nodiscard]] ui::View& View() const noexcept { return *m_scroll; }

        /// The settings changed: rebuild at the next Refresh.
        void MarkChanged() noexcept { m_changed = true; }

        /// Rebuild when something it shows changed (see the header).
        void Refresh()
        {
            const u64 revision = m_cook != nullptr ? m_cook->Revision() : 0;
            const i64 thumbnail = ThumbnailTime();
            if (m_changed || revision != m_cookRevision || thumbnail != m_thumbnailTime)
            {
                Rebuild();
            }
        }

        void Rebuild()
        {
            m_changed = false;
            m_cookRevision = m_cook != nullptr ? m_cook->Revision() : 0;
            m_thumbnailTime = ThumbnailTime();
            m_pictures.Clear();
            m_scenes.Clear();
            m_rows.Clear();
            m_actions.Clear();

            ui::FlexLayout* page = m_page.Get();
            page->RemoveAllViews();
            editor::EditorProject* project = m_context->Project();
            if (project == nullptr)
            {
                auto none = MakeRef<ui::Label>(Allocator(), StringView(u8"No project is open."));
                none->TextColor.SetValue(Optional<Color>(kDim));
                page->AddView(none.Get());
                return;
            }
            page->AddView(Header(*project).Get());

            auto columns = MakeRef<ui::FlexLayout>(Allocator());
            columns->Direction = ui::Orientation::Horizontal;
            columns->Wrap = true;
            columns->Spacing = 40;
            columns->LineSpacing = 22;
            ui::LayoutStyle start;
            start.Width = ui::SizeSpec::Fixed(ui::Unit::Dp(280.0f));
            columns->AddView(StartColumn(*project).Get(), start);
            ui::LayoutStyle settings;
            settings.Width = ui::SizeSpec::Fixed(ui::Unit::Dp(440.0f));
            columns->AddView(ProjectColumn(*project).Get(), settings);
            page->AddView(columns.Get());
        }

        // What it shows, for tests: the scenes listed (in order), each setting row as
        // "Label: value", and the actions offered (ids).
        [[nodiscard]] Span<const Guid> Scenes() const noexcept { return m_scenes.AsSpan(); }
        [[nodiscard]] Span<const String> SettingRows() const noexcept { return m_rows.AsSpan(); }
        [[nodiscard]] Span<const String> ActionIds() const noexcept { return m_actions.AsSpan(); }

    private:
        static constexpr Color kDim{0.55f, 0.55f, 0.57f, 1.0f};
        static constexpr Color kMissing{0.95f, 0.6f, 0.35f, 1.0f};
        static constexpr f32 kLinkInsetX = 6.0f; // a link's padding: its hover fill around the text
        static constexpr f32 kLinkInsetY = 3.0f;

        [[nodiscard]] IAllocator& Allocator() const noexcept { return *m_allocator; }

        [[nodiscard]] i64 ThumbnailTime() const
        {
            const editor::EditorProject* project = m_context->Project();
            u64 size = 0;
            i64 time = 0;
            if (project == nullptr ||
                !FileStat(ProjectThumbnailPath(project->Directory()).AsView(), size, time))
            {
                return 0;
            }
            return time;
        }

        [[nodiscard]] RefPtr<ui::Label> Text(StringView text, f32 size, Optional<Color> color = {})
        {
            auto label = MakeRef<ui::Label>(Allocator(), text);
            label->FontSize.SetValue(size);
            if (color.HasValue())
            {
                label->TextColor.SetValue(color);
            }
            return label;
        }

        // A setting's value as plain text, inset as a link's text is, so the column lines up.
        [[nodiscard]] RefPtr<ui::View> Value(StringView text, Optional<Color> color = {})
        {
            auto label = Text(text, 13.0f, color);
            label->Ellipsis.SetValue(true);
            ui::LayoutStyle inset;
            inset.Margin = ui::Thickness{kLinkInsetX, kLinkInsetY};
            label->SetLayout(inset);
            return RefPtr<ui::View>(label.Get());
        }

        [[nodiscard]] RefPtr<ui::Label> Heading(StringView text)
        {
            return Text(text, 11.5f, Optional<Color>(kDim));
        }

        // A quiet button as wide as its text: a link to an asset or a scene. (A content button
        // centres its content, so a link is placed at its own width, at the start of its line.)
        [[nodiscard]] RefPtr<ui::ContentButton> Link(StringView text, Function<void()> onClick)
        {
            auto label = Text(text, 13.0f);
            label->Ellipsis.SetValue(true);
            auto button = MakeRef<ui::ContentButton>(Allocator(), RefPtr<ui::View>(label.Get()));
            button->SetStyle(ui::StyleProperty::Padding, ui::Thickness{kLinkInsetX, kLinkInsetY});
            auto look = MakeRef<ui::StateListDrawable>(Allocator());
            const auto fill = [this](f32 alpha)
            {
                return ui::DrawablePtr(
                    MakeRef<ui::RoundedRectDrawable>(Allocator(), Color{1.0f, 1.0f, 1.0f, alpha}, 4.0f).Get());
            };
            look->Set(ui::ControlState::Normal, fill(0.0f));
            look->Set(ui::ControlState::Hover, fill(0.07f));
            look->Set(ui::ControlState::Pressed, fill(0.11f));
            button->SetStyle(ui::StyleProperty::Background, ui::DrawablePtr(look.Get()));
            button->OnClick.Add([onClick = Move(onClick)](ui::ButtonBase*) { onClick(); });
            return button;
        }

        void OpenAsset(const Guid& id)
        {
            if (OnOpenAsset)
            {
                OnOpenAsset(id);
            }
        }

        [[nodiscard]] RefPtr<ui::View> AssetLink(const Guid& id, StringView emptyText)
        {
            if (id.IsNil())
            {
                return Value(emptyText, Optional<Color>(kDim));
            }
            const content::Instance* instance = m_context->Project()->SourceDb().GetInstance(id);
            if (instance == nullptr)
            {
                return Value(u8"(missing)", Optional<Color>(kMissing));
            }
            ProjectHomeView* self = this;
            auto link = Link(instance->Name(), [self, id]() { self->OpenAsset(id); });
            link->TooltipText = instance->Path();
            return RefPtr<ui::View>(link.Get());
        }

        // The picture, the name, the engine and the folder.
        [[nodiscard]] RefPtr<ui::View> Header(const editor::EditorProject& project)
        {
            const engine::project::ProjectSettings& settings = project.Settings();
            auto header = MakeRef<ui::FlexLayout>(Allocator());
            header->Direction = ui::Orientation::Horizontal;
            header->AlignItems = ui::Align::Center;
            header->Spacing = 18;
            header->AddView(MakeProjectPicture(Allocator(), project.Directory(), settings.name.AsView(), 192.0f,
                                               108.0f, 8.0f, m_pictures)
                                .Get());
            auto text = MakeRef<ui::FlexLayout>(Allocator());
            text->Direction = ui::Orientation::Vertical;
            text->Spacing = 4;
            auto name = Text(settings.name.IsEmpty() ? StringView(u8"(unnamed)") : settings.name.AsView(), 22.0f);
            name->Ellipsis.SetValue(true);
            ui::LayoutStyle match;
            match.Width = ui::SizeSpec::Match();
            text->AddView(name.Get(), match);
            text->AddView(Text(Format(u8"AssiduousEngine {}", settings.engineVersion.AsView()).AsView(), 12.5f,
                               Optional<Color>(kDim))
                              .Get());
            auto folder = Text(project.Directory(), 12.0f, Optional<Color>(kDim));
            folder->Ellipsis.SetValue(true);
            folder->TooltipText = String(project.Directory());
            text->AddView(folder.Get(), match);
            ui::LayoutStyle grow;
            grow.FlexBasis = ui::Unit::Dp(0.0f);
            grow.FlexGrow = 1.0f;
            header->AddView(text.Get(), grow);
            return RefPtr<ui::View>(header.Get());
        }

        // The actions under `menu` ("Game/"), by their menu order, as a row of buttons.
        [[nodiscard]] RefPtr<ui::View> MenuActions(StringView menu)
        {
            Array<const editor::EditorActionDeclaration*> found;
            for (const editor::EditorActionDeclaration& action : m_context->Actions().Actions())
            {
                if (action.menuPath.AsView().StartsWith(menu))
                {
                    found.PushBack(&action);
                }
            }
            found.Sort([](const editor::EditorActionDeclaration* a, const editor::EditorActionDeclaration* b)
                       { return a->menuOrder < b->menuOrder; });
            auto row = MakeRef<ui::FlexLayout>(Allocator());
            row->Direction = ui::Orientation::Horizontal;
            row->Wrap = true;
            row->Spacing = 6;
            row->LineSpacing = 6;
            editor::EditorContext* context = m_context;
            for (const editor::EditorActionDeclaration* action : found)
            {
                auto button = MakeRef<ui::Button>(Allocator(), action->label.AsView());
                button->TooltipText = action->description;
                button->IsEnabled = editor::EditorActionRegistry::IsEnabled(*action, nullptr);
                button->OnClick.Add([context, id = action->id](ui::ButtonBase*)
                                    { (void)context->Actions().Execute(id.AsView(), nullptr); });
                ui::LayoutStyle height;
                height.Height = ui::SizeSpec::Fixed(ui::Unit::Dp(28.0f));
                row->AddView(button.Get(), height);
                m_actions.PushBack(action->id);
            }
            return RefPtr<ui::View>(row.Get());
        }

        // Open the default scene, play, and every scene in the project.
        [[nodiscard]] RefPtr<ui::View> StartColumn(editor::EditorProject& project)
        {
            const engine::project::ProjectSettings& settings = project.Settings();
            auto column = MakeRef<ui::FlexLayout>(Allocator());
            column->Direction = ui::Orientation::Vertical;
            column->Spacing = 8;
            column->AddView(Heading(u8"START").Get());
            {
                const content::Instance* scene =
                    settings.defaultSceneId.IsNil() ? nullptr : project.SourceDb().GetInstance(settings.defaultSceneId);
                auto open = MakeRef<ui::Button>(Allocator(), scene != nullptr
                                                                 ? Format(u8"Open {}", scene->Name()).AsView()
                                                                 : StringView(u8"No default scene"));
                open->IsEnabled = scene != nullptr;
                ProjectHomeView* self = this;
                const Guid id = settings.defaultSceneId;
                open->OnClick.Add([self, id](ui::ButtonBase*) { self->OpenAsset(id); });
                ui::LayoutStyle height;
                height.Height = ui::SizeSpec::Fixed(ui::Unit::Dp(30.0f));
                column->AddView(open.Get(), height);
            }
            column->AddView(MenuActions(u8"Game/").Get());

            // The scenes: every asset of the type the default-scene setting names.
            const PropertyInfo* defaultScene =
                FindProperty(engine::project::ProjectSettings::StaticType(), "defaultSceneId");
            const String* sceneType = defaultScene != nullptr
                                          ? engine::project::SettingAttribute(
                                                *defaultScene, engine::project::kSettingAssetTypeAttribute)
                                          : nullptr;
            if (sceneType == nullptr)
            {
                return RefPtr<ui::View>(column.Get());
            }
            Array<const content::Instance*> scenes;
            // (A group keeps its instances and its groups in name order, so the walk is in order.)
            CollectInstances(*project.SourceDb().RootGroup(), sceneType->AsView(), scenes);
            auto spacer = MakeRef<ui::FlexLayout>(Allocator());
            spacer->Padding = ui::Thickness{0, 8, 0, 0};
            spacer->AddView(Heading(Format(u8"SCENES ({})", scenes.Size()).AsView()).Get());
            column->AddView(spacer.Get());
            ProjectHomeView* self = this;
            for (const content::Instance* scene : scenes)
            {
                const Guid id = scene->Id();
                const bool isDefault = id == settings.defaultSceneId;
                auto link = Link(isDefault ? Format(u8"{}  (default)", scene->Name()).AsView() : scene->Name(),
                                 [self, id]() { self->OpenAsset(id); });
                link->TooltipText = scene->Path();
                ui::LayoutStyle start;
                start.AlignSelf = ui::Align::Start;
                column->AddView(link.Get(), start);
                m_scenes.PushBack(id);
            }
            if (scenes.IsEmpty())
            {
                column->AddView(Text(u8"No scenes yet.", 13.0f, Optional<Color>(kDim)).Get());
            }
            return RefPtr<ui::View>(column.Get());
        }

        static void CollectInstances(const content::Group& group, StringView typeName,
                                     Array<const content::Instance*>& out)
        {
            for (const content::Instance* instance : group.Instances())
            {
                if (instance->TypeName() == typeName)
                {
                    out.PushBack(instance);
                }
            }
            for (const content::Group* child : group.Groups())
            {
                CollectInstances(*child, typeName, out);
            }
        }

        // A setting's value as text: a choice by its name, a flag as On or Off, a number, a
        // string ("(none)" when empty). Empty for a kind it does not show.
        [[nodiscard]] static String SettingText(const PropertyInfo& property, const void* address)
        {
            if (IsEnum(*property.type))
            {
                const i64 current = ReadEnumValue(address, *property.type);
                for (usize i = 0; i < EnumeratorCount(*property.type); ++i)
                {
                    const EnumValue& value = EnumeratorAt(*property.type, i);
                    if (value.value == current)
                    {
                        return String(StringView(reinterpret_cast<const utf8char*>(value.name)));
                    }
                }
                return {};
            }
            if (property.type == &TypeOf<bool>())
            {
                return String(*static_cast<const bool*>(address) ? u8"On" : u8"Off");
            }
            if (property.type == &TypeOf<u32>())
            {
                return Format(u8"{}", *static_cast<const u32*>(address));
            }
            if (property.type == &TypeOf<String>())
            {
                const String& text = *static_cast<const String*>(address);
                return text.IsEmpty() ? String(u8"(none)") : text;
            }
            return {};
        }

        // Every reflected setting by category, then the Project menu's actions.
        [[nodiscard]] RefPtr<ui::View> ProjectColumn(editor::EditorProject& project)
        {
            namespace proj = engine::project;
            const TypeInfo& type = proj::ProjectSettings::StaticType();
            const Instance settings(&project.Settings(), &type);
            const PropertyInfo* msaa = FindProperty(type, "renderMsaaSamples");
            auto column = MakeRef<ui::FlexLayout>(Allocator());
            column->Direction = ui::Orientation::Vertical;
            column->Spacing = 4;
            column->AddView(Heading(u8"PROJECT").Get());

            // The categories in the order the settings first name them.
            Array<String> categories;
            for (const PropertyInfo& property : Properties(type))
            {
                if (proj::SettingAttribute(property, proj::kSettingLabelAttribute) == nullptr)
                {
                    continue;
                }
                const StringView category = proj::SettingCategory(property);
                bool seen = false;
                for (const String& known : categories)
                {
                    seen = seen || known.AsView() == category;
                }
                if (!seen)
                {
                    categories.PushBack(String(category));
                }
            }
            for (const String& category : categories)
            {
                auto title = Text(category.AsView(), 13.0f, Optional<Color>(Color{0.8f, 0.8f, 0.82f, 1.0f}));
                auto pad = MakeRef<ui::FlexLayout>(Allocator());
                pad->Padding = ui::Thickness{0, 8, 0, 2};
                pad->AddView(title.Get());
                column->AddView(pad.Get());
                for (const PropertyInfo& property : Properties(type))
                {
                    const String* label = proj::SettingAttribute(property, proj::kSettingLabelAttribute);
                    if (label == nullptr || proj::SettingCategory(property) != category.AsView())
                    {
                        continue;
                    }
                    const void* address = property.address(settings);
                    RefPtr<ui::View> value;
                    String text;
                    if (proj::IsAssetSetting(property))
                    {
                        const Guid& id = *static_cast<const Guid*>(address);
                        const String* empty = proj::SettingAttribute(property, proj::kSettingEmptyTextAttribute);
                        value = AssetLink(id, empty != nullptr ? empty->AsView() : StringView(u8"(none)"));
                        const content::Instance* instance =
                            id.IsNil() ? nullptr : project.SourceDb().GetInstance(id);
                        text = id.IsNil() ? String(empty != nullptr ? empty->AsView() : StringView(u8"(none)"))
                               : instance != nullptr ? String(instance->Name())
                                                     : String(u8"(missing)");
                    }
                    else if (proj::IsAssetListSetting(property))
                    {
                        const Array<Guid>& ids = *static_cast<const Array<Guid>*>(address);
                        auto list = MakeRef<ui::FlexLayout>(Allocator());
                        list->Direction = ui::Orientation::Vertical;
                        list->AlignItems = ui::Align::Start;
                        for (const Guid& id : ids)
                        {
                            list->AddView(AssetLink(id, u8"(none)").Get());
                            const content::Instance* instance = project.SourceDb().GetInstance(id);
                            text += text.IsEmpty() ? u8"" : u8", ";
                            text += instance != nullptr ? instance->Name() : StringView(u8"(missing)");
                        }
                        if (ids.IsEmpty())
                        {
                            text = String(u8"(none)");
                            list->AddView(Value(text.AsView(), Optional<Color>(kDim)).Get());
                        }
                        value = RefPtr<ui::View>(list.Get());
                    }
                    else
                    {
                        if (&property == msaa)
                        {
                            const u32 samples = *static_cast<const u32*>(address);
                            text = String(engine::render::kMsaaLevels[engine::render::MsaaIndexForSamples(samples)].label);
                        }
                        else
                        {
                            text = SettingText(property, address);
                        }
                        if (text.IsEmpty())
                        {
                            continue; // a kind this page does not show
                        }
                        value = Value(text.AsView());
                    }
                    m_rows.PushBack(Format(u8"{}: {}", label->AsView(), text.AsView()));
                    column->AddView(SettingRow(label->AsView(), value).Get());
                }
            }

            auto actions = MakeRef<ui::FlexLayout>(Allocator());
            actions->Padding = ui::Thickness{0, 14, 0, 0};
            actions->AddView(MenuActions(u8"Project/").Get());
            column->AddView(actions.Get());
            return RefPtr<ui::View>(column.Get());
        }

        [[nodiscard]] RefPtr<ui::View> SettingRow(StringView label, const RefPtr<ui::View>& value)
        {
            auto row = MakeRef<ui::FlexLayout>(Allocator());
            row->Direction = ui::Orientation::Horizontal;
            row->AlignItems = ui::Align::Start;
            row->Spacing = 10;
            auto name = Text(label, 13.0f, Optional<Color>(kDim));
            ui::LayoutStyle fixed;
            fixed.Width = ui::SizeSpec::Fixed(ui::Unit::Dp(150.0f));
            fixed.Margin = ui::Thickness{0, kLinkInsetY, 0, 0};
            row->AddView(name.Get(), fixed);
            // The value at the start of the room left (a link keeps its own width).
            auto cell = MakeRef<ui::FlexLayout>(Allocator());
            cell->Direction = ui::Orientation::Vertical;
            cell->AlignItems = ui::Align::Start;
            cell->AddView(value.Get());
            ui::LayoutStyle grow;
            grow.FlexBasis = ui::Unit::Dp(0.0f);
            grow.FlexGrow = 1.0f;
            row->AddView(cell.Get(), grow);
            return RefPtr<ui::View>(row.Get());
        }

        IAllocator* m_allocator;
        editor::EditorContext* m_context;
        editor::EditorCookService* m_cook;
        RefPtr<ui::ScrollView> m_scroll;
        RefPtr<ui::FlexLayout> m_page; // the scroll's one child, refilled by Rebuild
        Array<UniquePtr<foundation::image::Image>> m_pictures; // what the header's picture borrows
        bool m_changed = false;
        u64 m_cookRevision = 0;
        i64 m_thumbnailTime = 0;
        Array<Guid> m_scenes;
        Array<String> m_rows;
        Array<String> m_actions;
    };
}
