// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Scene - :inspector partition, its body. See InspectorView.cppm.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

module editor.scene;

import foundation.core;
import foundation.scene;
import foundation.scene.resource;
import foundation.gui;
import foundation.gui.toolkit;
import editor.core;
import editor.app;
import :edit;
import :inspector;
import :entity_ref_editor;
import :bone_name_editor;
import :collision_matrix_editor;
import :inspector_extras;
import :scene_settings_source;
import :settings_profiles;

using namespace foundation::core;
namespace gui = foundation::gui;
namespace scene = foundation::scene;

namespace editor
{
    namespace
    {
        constexpr StringView kActiveKey = u8"e";

        // A component's source as the inspector reads it and an undo writes it back: its text
        // format, EntityRef as a reference by its id, and a bone name as its own row.
        void ConfigureComponentSource(gui::ReflectedPropertySource& source, gui::IPropertyTextFormat& format)
        {
            source.SetTextFormat(&format);
            source.RegisterReferenceType(EntityRefEditor::kHint, gui::ReflectedReferenceParts{String(u8"id")});
            source.RegisterHintAttribute(BoneNameEditor::kHint);
        }
        constexpr StringView kTransformKey = u8"t";

        // A component edit as the reflected source reports it: each entity's value before and after
        // as text, restored onto the components found again by entity and type (a pool may have
        // moved them since). Its first Execute writes the after values over what the edit already
        // wrote.
        class InspectorEditCommand final : public IEditorCommand
        {
        public:
            InspectorEditCommand(SceneEditContext& edit, Array<Guid> entities, const TypeInfo& type, gui::ReflectedChange change,
                                 gui::IPropertyTextFormat& format)
                : m_edit(&edit), m_entities(Move(entities)), m_type(&type), m_change(Move(change)), m_format(&format)
            {
            }
            [[nodiscard]] bool Execute() override { return Restore(false); }
            void Undo() override { (void)Restore(true); }
            [[nodiscard]] StringView TypeId() const override { return u8"inspector_edit"; }

        private:
            bool Restore(bool before)
            {
                Array<Instance> objects;
                for (const Guid& entity : m_entities)
                {
                    scene::ComponentManagerBase* manager = m_edit->FindManager(m_type);
                    const scene::EntityHandle e = m_edit->Resolve(entity);
                    if (manager == nullptr || !e.IsAssigned() || !manager->HasComponent(e))
                        return false;
                    objects.PushBack(manager->GetComponentInstance(e));
                }
                gui::ReflectedPropertySource source(EditorRootAllocator());
                ConfigureComponentSource(source, *m_format);
                source.SetObjects(Span<const Instance>(objects.Data(), objects.Size()));
                const bool ok = source.Restore(m_change, before);
                // A reference's id is written; what it names is bound now (the mesh shows at once).
                for (const Guid& entity : m_entities)
                    m_edit->ResolveComponentResources(entity, m_type);
                return ok;
            }

            SceneEditContext* m_edit;
            Array<Guid> m_entities;
            const TypeInfo* m_type;
            gui::ReflectedChange m_change;
            gui::IPropertyTextFormat* m_format;
        };

        u64 Mix(u64 hash, u64 value) { return (hash ^ value) * 1099511628211ull; }
    }

    class SceneInspectorView::Extras final : public InspectorExtras
    {
    public:
        Extras(SceneInspectorView& view, IAllocator& allocator, usize objects) : bag(allocator, objects), m_view(view), m_notices(allocator), m_actions(allocator)
        {
            Extras* self = this;
            bag.SetOnCommit([self](const gui::PropertyBag::Commit& commit)
                            {
                                for (const Action& action : self->m_actions)
                                    if (action.name == commit.Path && action.run)
                                        action.run();
                            });
        }
        [[nodiscard]] SceneEditContext& Edit() override { return *m_view.m_edit; }
        [[nodiscard]] EditorContext* Context() override { return m_view.m_services.context; }
        [[nodiscard]] Span<const Guid> Entities() const override { return m_view.m_entities.AsSpan(); }
        void AddNotice(StringView name, Function<String()> message) override
        {
            gui::PropertyInfo& info = bag.Add(name, gui::PropertyKind::String, gui::Variant(StringView{}));
            info.Hint = String(app::kNoticeHint);
            info.ReadOnly = true;
            info.Visible = false; // until it says something
            m_notices.PushBack(Notice{String(name), Move(message), String{}});
        }
        void AddAction(StringView name, StringView label, StringView description, Function<void()> run) override
        {
            gui::PropertyInfo& info = bag.Add(name, gui::PropertyKind::String, gui::Variant(label));
            info.Hint = String(app::kActionHint);
            info.Description = String(description);
            for (usize i = 0; i < bag.ObjectCount(); ++i)
                bag.SetValue(name, i, gui::Variant(label));
            m_actions.PushBack(Action{String(name), Move(run), {}});
        }
        /// An action shown only while `shown` says so.
        void AddShownAction(StringView name, StringView label, StringView description, Function<void()> run, Function<bool()> shown)
        {
            AddAction(name, label, description, Move(run));
            m_actions.Back().shown = Move(shown);
            if (gui::PropertyInfo* info = bag.FindInfo(name))
                info->Visible = !m_actions.Back().shown || m_actions.Back().shown();
        }
        [[nodiscard]] bool IsShown(StringView name)
        {
            const gui::PropertyInfo* info = bag.FindInfo(name);
            return info != nullptr && info->Visible;
        }
        void AddSection(UniquePtr<gui::IPropertySource> source, Function<void()> refresh) override
        {
            m_sources.PushBack(Section{Move(source), Move(refresh)});
        }
        [[nodiscard]] bool HasRows() const noexcept { return !m_notices.IsEmpty() || !m_actions.IsEmpty(); }
        [[nodiscard]] bool IsEmpty() const noexcept { return !HasRows() && m_sources.IsEmpty(); }
        [[nodiscard]] usize SourceCount() const noexcept { return m_sources.Size(); }
        [[nodiscard]] gui::IPropertySource& SourceAt(usize i) const { return *m_sources[i].source; }
        /// Asks each notice for its current text; a notice shows while the text is not empty.
        void Refresh()
        {
            for (const Section& section : m_sources)
                if (section.refresh)
                    section.refresh();
            bool shown = false;
            for (Notice& notice : m_notices)
            {
                const String message = notice.message ? notice.message() : String{};
                if (message != notice.said)
                {
                    notice.said = message;
                    for (usize i = 0; i < bag.ObjectCount(); ++i)
                        bag.SetValue(notice.name.AsView(), i, gui::Variant(message.AsView()));
                }
                gui::PropertyInfo* info = bag.FindInfo(notice.name.AsView());
                if (info != nullptr && info->Visible == message.IsEmpty())
                {
                    info->Visible = !message.IsEmpty();
                    shown = true;
                }
            }
            for (const Action& action : m_actions)
            {
                if (!action.shown)
                    continue;
                const bool visible = action.shown();
                gui::PropertyInfo* info = bag.FindInfo(action.name.AsView());
                if (info != nullptr && info->Visible != visible)
                {
                    info->Visible = visible;
                    shown = true;
                }
            }
            if (shown)
                bag.PropertiesChanged();
        }
        [[nodiscard]] String Text(StringView name) const
        {
            for (const Notice& notice : m_notices)
                if (notice.name.AsView() == name)
                    return notice.said;
            for (const Action& action : m_actions)
                if (action.name.AsView() == name)
                    return String(const_cast<gui::PropertyBag&>(bag).GetValue(name, 0).AsString());
            return String{};
        }
        bool Run(StringView name)
        {
            for (const Action& action : m_actions)
                if (action.name.AsView() == name && action.run)
                {
                    action.run();
                    return true;
                }
            return false;
        }

        gui::PropertyBag bag;

    private:
        struct Notice
        {
            String name;
            Function<String()> message;
            String said;
        };
        struct Action
        {
            String name;
            Function<void()> run;
            Function<bool()> shown; // empty: always
        };
        struct Section
        {
            UniquePtr<gui::IPropertySource> source;
            Function<void()> refresh;
        };
        SceneInspectorView& m_view;
        Array<Section> m_sources;
        Array<Notice> m_notices;
        Array<Action> m_actions;
    };

    SceneInspectorView::SceneInspectorView(IAllocator& allocator, SceneEditContext& edit, Services services, gui::Node& pane)
        : m_allocator(allocator), m_edit(&edit), m_services(Move(services)), m_pane(&pane), m_sections(allocator), m_format(allocator),
          m_entities(allocator), m_transforms(allocator), m_components(allocator)
    {
        SceneInspectorView* self = this;
        m_grid = Find<gui::PropertyGrid>(u8"inspector-grid");
        if (m_grid != nullptr)
        {
            if (m_services.refs != nullptr)
                app::ResourceRefEditor::Register(m_grid->GetEditors(), *m_services.refs);
            // An entity reference: a slot over this scene's entities, chosen before the asset slot.
            scene::RegisterEntityRefReflection();
            m_entityRefs.edit = m_edit;
            m_entityRefs.icons = m_services.icons;
            if (m_services.pickEntity)
                m_entityRefs.pick = [self](EntityPickRequest request) { self->m_services.pickEntity(Move(request)); };
            EntityRefEditor::Register(m_grid->GetEditors(), m_entityRefs);
            BoneNameEditor::Register(m_grid->GetEditors(), m_edit);
            CollisionMatrixEditor::Register(m_grid->GetEditors()); // the physics block's groups, a grid
            app::PropertyExtras::Register(m_grid->GetEditors()); // a component's notices and actions
            m_grid->SetSource(&m_sections);
            m_grid->SetOnRowMenu([self](gui::Menu& menu, const gui::PropertyRow& row) { self->HeaderMenu(menu, row); });
        }
        if (auto* search = Find<gui::TextField>(u8"inspector-search"))
            search->SetOnTextChanging([self](StringView text) { self->Search(text); });
        if (auto* paste = Find<gui::Button>(u8"inspector-paste"))
        {
            paste->SetOnClick([self]() { (void)self->PasteComponent(); });
            paste->SetDisplayed(false);
        }
        if (auto* add = Find<gui::Button>(u8"inspector-add"))
            add->SetOnClick([self]() { self->OpenAddComponent(); });
        if (auto* name = Find<gui::TextField>(u8"inspector-name"))
            // Enter, or the focus leaving with a new name, renames the entity (only with one
            // selected).
            name->SetOnTextChanged([self](StringView text)
                                   {
                                       if (self->m_entities.Size() == 1 && !text.IsEmpty())
                                           self->m_edit->RenameEntity(self->m_entities[0], text);
                                   });
        if (auto* note = Find<gui::Node>(u8"inspector-note"))
            note->SetDisplayed(false);
        Refresh();
    }

    SceneInspectorView::~SceneInspectorView()
    {
        if (m_grid != nullptr)
            m_grid->SetSource(nullptr);
        Clear();
    }

    u64 SceneInspectorView::Signature() const
    {
        u64 hash = 14695981039346656037ull;
        hash = Mix(hash, m_edit->Scene().Revision());
        hash = Mix(hash, ShowsScene() ? 1u : 0u);
        scene::Scene& scene = m_edit->Scene();
        for (const Guid& entity : m_edit->EntitySelection().Items())
        {
            hash = Mix(hash, Hash<Guid>{}(entity));
            const scene::EntityHandle e = scene.FindEntity(entity);
            u64 bit = 1;
            scene.ForEachManager([&](scene::ComponentManagerBase& manager)
                                 {
                                     if (e.IsAssigned() && manager.HasComponent(e))
                                         hash = Mix(hash, bit);
                                     bit <<= 1;
                                 });
        }
        return hash;
    }

    void SceneInspectorView::Refresh()
    {
        const bool readOnly = m_services.simulating && m_services.simulating();
        if (readOnly != m_readOnly)
        {
            m_readOnly = readOnly;
            m_sections.SetReadOnly(readOnly);
            if (auto* note = Find<gui::Node>(u8"inspector-note"))
                note->SetDisplayed(readOnly);
        }
        const u64 signature = Signature();
        if (signature != m_signature)
        {
            m_signature = signature;
            Build();
            return;
        }
        // The values as they are now (after a gizmo's drag, an undo): the transforms are read
        // again, the notices asked again and the grid told, except while it is editing one (that
        // row belongs to the gesture).
        ShowPaste();
        PullTransforms();
        RefreshExtras();
        if (m_grid != nullptr && !m_grid->IsEditing())
            for (const UniquePtr<ComponentSection>& section : m_components)
                section->source->Refresh();
    }

    void SceneInspectorView::Clear()
    {
        m_sections.Clear();
        m_components.Clear();
        m_settings.Clear();
        m_transform.Reset();
        m_active.Reset();
        m_transforms.Clear();
        m_entities.Clear();
    }

    void SceneInspectorView::Build()
    {
        Clear();
        scene::Scene& scene = m_edit->Scene();
        for (const Guid& entity : m_edit->EntitySelection().Items())
            if (scene.FindEntity(entity).IsAssigned())
                m_entities.PushBack(entity);
        const bool any = !m_entities.IsEmpty();
        const bool sceneShown = !any && ShowsScene();
        if (auto* state = Find<gui::Node>(u8"inspector-state"))
            state->SetDisplayed(!any && !sceneShown);
        if (m_grid != nullptr)
            m_grid->SetDisplayed(any || sceneShown);
        if (auto* add = Find<gui::Node>(u8"inspector-add"))
            add->SetDisplayed(any); // a scene has settings, not components
        ShowPaste();
        BuildHead();
        if (sceneShown)
        {
            BuildScene();
            return;
        }
        if (!any)
            return;

        // Active: a bag of one bool, a value per entity; a change is each entity's command.
        m_active = MakeUnique<gui::PropertyBag>(m_allocator, m_allocator, m_entities.Size());
        gui::PropertyInfo& active = m_active->Add(u8"active", gui::PropertyKind::Bool, gui::Variant(true));
        active.DisplayName = String(u8"Active");
        active.Description = String(u8"Off: the entity and everything under it neither show nor run");
        for (usize i = 0; i < m_entities.Size(); ++i)
            m_active->SetValue(u8"active", i, gui::Variant(scene.IsActive(scene.FindEntity(m_entities[i]))));
        SceneInspectorView* self = this;
        m_active->SetOnCommit([self](const gui::PropertyBag::Commit& commit)
                              {
                                  self->m_edit->Commands().BeginGroup(u8"inspector.active");
                                  for (usize i = 0; i < self->m_entities.Size() && i < commit.After.Size(); ++i)
                                      self->m_edit->SetEntityActive(self->m_entities[i], commit.After[i].AsBool());
                                  self->m_edit->Commands().EndGroup();
                              });
        m_sections.Add(kActiveKey, u8"Entity", *m_active);
        BuildTransform();
        BuildComponents();
        if (m_grid != nullptr && !m_filter.IsEmpty())
            m_grid->SetFilter(m_filter.AsView());
    }

    void SceneInspectorView::BuildHead()
    {
        scene::Scene& scene = m_edit->Scene();
        auto* name = Find<gui::TextField>(u8"inspector-name");
        auto* kind = Find<gui::Label>(u8"inspector-kind");
        auto* icon = Find<gui::Image>(u8"inspector-icon");
        const usize count = m_entities.Size();
        if (count == 0 && ShowsScene())
        {
            if (name != nullptr)
            {
                name->SetText(m_services.sceneName ? m_services.sceneName() : scene.Name());
                name->SetEnabled(false); // the name is the asset's; Save As gives another
            }
            if (kind != nullptr)
                kind->SetText(u8"Scene");
            if (icon != nullptr)
                icon->SetSource(String(m_services.sceneIcon.IsEmpty() ? StringView(u8"Editor/Icons/Types/SceneDocument.svg") : m_services.sceneIcon));
            return;
        }
        if (name != nullptr)
        {
            name->SetText(count == 1 ? scene.GetEntityName(scene.FindEntity(m_entities[0])) : StringView{});
            name->SetEnabled(count == 1);
        }
        if (kind != nullptr)
            kind->SetText(count == 0 ? StringView{} : count == 1 ? StringView(u8"Entity") : Format(u8"{} entities", count).AsView());
        if (icon != nullptr)
        {
            String source(u8"Editor/Icons/cube.svg");
            if (count == 1 && m_services.icons != nullptr)
            {
                const scene::EntityHandle e = scene.FindEntity(m_entities[0]);
                bool found = false;
                scene.ForEachManager([&](scene::ComponentManagerBase& manager)
                                     {
                                         if (found || manager.ComponentType() == nullptr || !manager.HasComponent(e))
                                             return;
                                         const TypeIcon typeIcon = m_services.icons->For(*manager.ComponentType(), StringView{});
                                         if (!typeIcon.Source.IsEmpty())
                                         {
                                             source = typeIcon.Source;
                                             found = true;
                                         }
                                     });
            }
            icon->SetSource(source);
        }
    }

    void SceneInspectorView::BuildTransform()
    {
        // A copy of each entity's local transform is the source's object; an edit sets them back
        // through the edit context, one undo step for the selection.
        PullTransforms();
        Array<Instance> objects;
        for (Transform& transform : m_transforms)
            objects.PushBack(Instance(&transform, &TypeOf<Transform>()));
        m_transform = MakeUnique<gui::ReflectedPropertySource>(m_allocator, m_allocator);
        m_transform->SetTextFormat(&m_format);
        m_transform->SetObjects(Span<const Instance>(objects.Data(), objects.Size()));
        SceneInspectorView* self = this;
        m_transform->SetOnChange([self](const gui::ReflectedChange&)
                                 {
                                     const String group = Format(u8"inspector.transform.{}", ++self->m_edits); // never coalesced with the last
                                     self->m_edit->Commands().BeginGroup(group.AsView());
                                     for (usize i = 0; i < self->m_entities.Size(); ++i)
                                         self->m_edit->SetLocalTransform(self->m_entities[i], self->m_transforms[i]);
                                     self->m_edit->Commands().EndGroup();
                                 });
        m_sections.Add(kTransformKey, u8"Transform", *m_transform);
    }

    void SceneInspectorView::PullTransforms()
    {
        scene::Scene& scene = m_edit->Scene();
        if (m_transforms.Size() != m_entities.Size())
            m_transforms.Resize(m_entities.Size());
        if (m_grid != nullptr && m_grid->IsEditing())
            return; // the row being edited belongs to the gesture
        for (usize i = 0; i < m_entities.Size(); ++i)
            m_transforms[i] = scene.GetLocalTransform(scene.FindEntity(m_entities[i]));
        if (m_transform)
            m_transform->Refresh();
    }

    void SceneInspectorView::BuildComponents()
    {
        // Each component every selected entity has, in the scene's order of managers.
        scene::Scene& scene = m_edit->Scene();
        scene.ForEachManager([&](scene::ComponentManagerBase& manager)
                             {
                                 const TypeInfo* type = manager.ComponentType();
                                 if (type == nullptr)
                                     return;
                                 for (const Guid& entity : m_entities)
                                     if (!manager.HasComponent(scene.FindEntity(entity)))
                                         return;
                                 auto section = MakeUnique<ComponentSection>(m_allocator);
                                 section->key = Format(u8"c{}", m_components.Size());
                                 section->title = TitleOf(*type);
                                 section->type = type;
                                 section->source = MakeUnique<gui::ReflectedPropertySource>(m_allocator, m_allocator);
                                 ConfigureComponentSource(*section->source, m_format);
                                 const Array<Instance> objects = InstancesOf(*type);
                                 section->source->SetObjects(Span<const Instance>(objects.Data(), objects.Size()));
                                 SceneInspectorView* self = this;
                                 section->source->SetOnChange([self, type](const gui::ReflectedChange& change) { self->ComponentChanged(type, change); });
                                 m_sections.Add(section->key.AsView(), section->title.AsView(), *section->source);
                                 section->extras = BuildExtras(*section->type, section->key.AsView(), section->title.AsView(), m_entities.Size());
                                 m_components.PushBack(Move(section));
                             });
    }

    UniquePtr<SceneInspectorView::Extras> SceneInspectorView::BuildExtras(const TypeInfo& type, StringView key, StringView title, usize objects)
    {
        auto extras = MakeUnique<Extras>(m_allocator, *this, m_allocator, objects);
        InspectorExtrasRegistry::Get().ForEach(type, [&](const InspectorExtrasProvider& provider) { provider(*extras); });
        if (extras->IsEmpty())
            return nullptr;
        extras->Refresh();
        // Under the section's header: the grid groups a title's rows together.
        if (extras->HasRows())
            m_sections.Add(Format(u8"x{}", key).AsView(), title, extras->bag);
        for (usize i = 0; i < extras->SourceCount(); ++i)
            m_sections.Add(Format(u8"y{}{}", key, i).AsView(), title, extras->SourceAt(i));
        return extras;
    }

    void SceneInspectorView::RefreshExtras()
    {
        for (const UniquePtr<SettingsSection>& section : m_settings)
        {
            section->source->Refresh();
            if (section->profile)
                section->profile->Refresh();
            if (section->extras)
                section->extras->Refresh();
        }
        for (const UniquePtr<ComponentSection>& section : m_components)
            if (section->extras)
                section->extras->Refresh();
    }

    String SceneInspectorView::ExtraText(StringView title, StringView name) const
    {
        for (const UniquePtr<ComponentSection>& section : m_components)
            if (section->title.AsView() == title && section->extras)
                return section->extras->Text(name);
        for (const UniquePtr<SettingsSection>& section : m_settings)
            if (section->title.AsView() == title && section->extras)
                return section->extras->Text(name);
        return String{};
    }

    gui::IPropertySource* SceneInspectorView::ExtraSource(StringView title, usize index) const
    {
        const Extras* extras = nullptr;
        for (const UniquePtr<ComponentSection>& section : m_components)
            if (section->title.AsView() == title)
                extras = section->extras.Get();
        for (const UniquePtr<SettingsSection>& section : m_settings)
            if (section->title.AsView() == title)
                extras = section->extras.Get();
        return extras != nullptr && index < extras->SourceCount() ? &extras->SourceAt(index) : nullptr;
    }

    bool SceneInspectorView::RunExtra(StringView title, StringView name)
    {
        for (const UniquePtr<ComponentSection>& section : m_components)
            if (section->title.AsView() == title && section->extras)
                return section->extras->Run(name);
        for (const UniquePtr<SettingsSection>& section : m_settings)
            if (section->title.AsView() == title && section->extras)
                return section->extras->Run(name);
        return false;
    }

    bool SceneInspectorView::ShowsScene() const
    {
        return m_services.sceneSelected && m_services.sceneSelected();
    }

    void SceneInspectorView::BuildScene()
    {
        EditorContext* context = m_services.context;
        SceneEditContext* edit = m_edit;
        Array<const TypeInfo*> types;
        m_edit->Scene().ForEachSystem([&](foundation::scene::SceneSystem& system)
                                      {
                                          const TypeInfo* type = system.SettingsType();
                                          if (type != nullptr && type->name != nullptr && type->name[0] != '<')
                                              types.PushBack(type);
                                      });
        for (const TypeInfo* type : types)
        {
            auto section = MakeUnique<SettingsSection>(m_allocator);
            section->title = TitleOf(*type);
            if (section->title.AsView().EndsWith(u8" Settings"))
                section->title = String(section->title.AsView().SubStr(0, section->title.Size() - 9));
            section->source = MakeUnique<SceneSettingsSource>(m_allocator, m_allocator, *m_edit, *type, m_format);
            const String key = Format(u8"s{}", m_settings.Size());
            section->key = key;
            // A block that can use a profile: where its values are, and the profile's verbs.
            foundation::scene::SceneSystem* system = m_edit->FindSystemBySettingsType(type);
            if (system != nullptr && system->SettingsProfileType() != nullptr && context != nullptr)
            {
                auto profile = MakeUnique<Extras>(m_allocator, *this, m_allocator, 1u);
                const auto inUse = [edit, type]() -> Guid
                {
                    foundation::scene::SceneSystem* s = edit->FindSystemBySettingsType(type);
                    return s != nullptr ? s->SettingsProfile() : Guid{};
                };
                profile->AddShownAction(u8"Open Profile", u8"Open Profile", u8"The values below are this profile's: an edit changes every scene using it (written to the profile on save).",
                                        [context, inUse]()
                                        {
                                            if (context->OpenAsset && !inUse().IsNil())
                                                context->OpenAsset(inUse());
                                        },
                                        [inUse]() { return !inUse().IsNil(); });
                const String profileName = Format(u8"{} {}", m_edit->Scene().Name(), section->title);
                profile->AddShownAction(u8"Make Profile", u8"Make Profile", u8"Saves these values as a new profile asset, and this scene uses it.",
                                        [context, edit, type, profileName]()
                                        {
                                            if (foundation::content::Instance* made = MakeSettingsProfile(*context, *edit, type, profileName.AsView()))
                                                context->Notify(NoticeKind::Success, Format(u8"Made profile '{}'; this scene uses it.", made->Name()).AsView());
                                        },
                                        [inUse]() { return inUse().IsNil(); });
                profile->AddShownAction(u8"Copy Into Scene", u8"Copy Into Scene", u8"Copies the profile's values into this scene, which then uses its own (the profile is unchanged).",
                                        [edit, type]() { (void)edit->MutateSceneSettings(type, [](foundation::scene::SceneSystem& s) { s.CopySettingsProfileIntoScene(); }); },
                                        [inUse]() { return !inUse().IsNil(); });
                m_sections.Add(Format(u8"p{}", key).AsView(), section->title.AsView(), profile->bag);
                section->profile = Move(profile);
            }
            m_sections.Add(key.AsView(), section->title.AsView(), *section->source);
            section->extras = BuildExtras(*type, key.AsView(), section->title.AsView(), 1u);
            m_settings.PushBack(Move(section));
        }
    }

    SceneSettingsSource* SceneInspectorView::SettingsOf(StringView title) const
    {
        for (const UniquePtr<SettingsSection>& section : m_settings)
            if (section->title.AsView() == title)
                return section->source.Get();
        return nullptr;
    }

    bool SceneInspectorView::ShowsProfileAction(StringView title, StringView name) const
    {
        for (const UniquePtr<SettingsSection>& section : m_settings)
            if (section->title.AsView() == title && section->profile)
                return section->profile->IsShown(name);
        return false;
    }

    bool SceneInspectorView::RunProfileAction(StringView title, StringView name)
    {
        for (const UniquePtr<SettingsSection>& section : m_settings)
            if (section->title.AsView() == title && section->profile)
                return section->profile->Run(name);
        return false;
    }

    Array<Instance> SceneInspectorView::InstancesOf(const TypeInfo& type) const
    {
        Array<Instance> out;
        scene::ComponentManagerBase* manager = m_edit->FindManager(&type);
        if (manager == nullptr)
            return out;
        for (const Guid& entity : m_entities)
            out.PushBack(manager->GetComponentInstance(m_edit->Resolve(entity)));
        return out;
    }

    void SceneInspectorView::ComponentChanged(const TypeInfo* type, const gui::ReflectedChange& change)
    {
        // The source has already written the value; the command keeps it and restores each entity's
        // on undo.
        Array<Guid> entities;
        for (const Guid& entity : m_entities)
            entities.PushBack(entity);
        (void)m_edit->Commands().Execute(UniquePtr<IEditorCommand>(
            EditorRootAllocator().New<InspectorEditCommand>(*m_edit, Move(entities), *type, change, m_format), EditorRootAllocator()));
    }

    String SceneInspectorView::TitleOf(const TypeInfo& type)
    {
        const StringView named = TypeAttrString(type, "displayName", StringView{});
        if (!named.IsEmpty())
            return String(named);
        StringView name(reinterpret_cast<const utf8char*>(type.name));
        if (name.EndsWith(u8"Component") && name.Size() > 9)
            name = name.SubStr(0, name.Size() - 9);
        return gui::ReadablePropertyName(name);
    }

    bool SceneInspectorView::CanPasteComponent() const
    {
        return !m_readOnly && !m_entities.IsEmpty() && m_services.context != nullptr && !m_services.context->ClipboardData(u8"component").IsEmpty();
    }

    void SceneInspectorView::ShowPaste()
    {
        if (auto* paste = Find<gui::Node>(u8"inspector-paste"))
            paste->SetDisplayed(CanPasteComponent());
    }

    bool SceneInspectorView::PasteComponent()
    {
        if (!CanPasteComponent())
            return false;
        // Pasting over a component an entity already has overwrites it, so it asks first.
        const Span<const byte> clip = m_services.context->ClipboardData(u8"component");
        scene::Scene& scene = m_edit->Scene();
        scene::ComponentManagerBase* manager = scene.FindManagerBySerializationId(SceneEditContext::PeekComponentTypeId(clip).AsView());
        bool overwrites = false;
        for (const Guid& entity : m_entities)
            overwrites = overwrites || (manager != nullptr && manager->HasComponent(scene.FindEntity(entity)));
        gui::Node* root = m_pane != nullptr ? m_pane->GetRootNode() : nullptr;
        if (!overwrites || manager == nullptr || root == nullptr)
            return PasteComponentNow();
        const String title = manager->ComponentType() != nullptr ? TitleOf(*manager->ComponentType()) : String(u8"component");
        m_pasteConfirm = MakeRef<gui::MessageBox>(m_allocator);
        m_pasteConfirm->Configure(u8"Overwrite Component?",
                                  Format(u8"{} has a {} component already. Pasting overwrites it (you can undo).",
                                         m_entities.Size() == 1 ? StringView(u8"This entity") : StringView(u8"An entity selected"), title)
                                      .AsView(),
                                  gui::MessageBox::Buttons::Ok);
        m_pasteConfirm->ClearButtons();
        (void)m_pasteConfirm->AddButton(u8"Cancel", u8"cancel", gui::DialogRole::Cancel);
        (void)m_pasteConfirm->AddButton(u8"Overwrite", u8"overwrite", gui::DialogRole::Primary);
        SceneInspectorView* self = this;
        gui::MessageBox* raw = m_pasteConfirm.Get();
        (void)m_pasteConfirm->AddEventListener(gui::EventType::Changed, [self, raw](const gui::Event&)
                                               {
                                                   if (raw->GetResultName().AsView() == StringView(u8"overwrite"))
                                                       (void)self->PasteComponentNow();
                                               });
        m_pasteConfirm->OpenModal(*root);
        return true;
    }

    bool SceneInspectorView::PasteComponentNow()
    {
        if (!CanPasteComponent())
            return false;
        const Span<const byte> clip = m_services.context->ClipboardData(u8"component");
        m_edit->Commands().BeginGroup(u8"inspector.paste");
        bool pasted = false;
        for (const Guid& entity : m_entities)
            pasted = m_edit->PasteComponent(entity, clip) || pasted;
        m_edit->Commands().EndGroup();
        return pasted;
    }

    gui::MessageBox* SceneInspectorView::PasteConfirm() const
    {
        return m_pasteConfirm && m_pasteConfirm->IsOpen() ? m_pasteConfirm.Get() : nullptr;
    }

    void SceneInspectorView::HeaderMenu(gui::Menu& menu, const gui::PropertyRow& row)
    {
        if (!row.Category)
            return;
        const TypeInfo* type = ComponentOf(row.Path.AsView());
        if (type == nullptr || m_readOnly)
            return;
        SceneInspectorView* self = this;
        EditorContext* context = m_services.context;
        // Copy takes the first entity's component; paste goes over each selected entity's.
        if (context != nullptr)
        {
            (void)menu.AddItem(u8"Copy Component", [self, type, context]()
                               {
                                   if (self->m_entities.IsEmpty())
                                       return;
                                   Array<byte> blob = self->m_edit->CopyComponent(self->m_entities[0], type);
                                   if (!blob.IsEmpty())
                                       context->CopyToEditorClipboard(u8"component", Move(blob), u8"component");
                               });
            gui::MenuItem* paste = menu.AddItem(u8"Paste Component Values", [self, context]()
                                                {
                                                    const Span<const byte> clip = context->ClipboardData(u8"component");
                                                    self->m_edit->Commands().BeginGroup(u8"inspector.paste");
                                                    for (const Guid& entity : self->m_entities)
                                                        (void)self->m_edit->PasteComponent(entity, clip);
                                                    self->m_edit->Commands().EndGroup();
                                                });
            paste->SetEnabled(!context->ClipboardData(u8"component").IsEmpty());
            menu.AddSeparator();
        }
        // Reset: back to the type's defaults, one undo step.
        for (const UniquePtr<ComponentSection>& section : m_components)
            if (section->type == type)
            {
                gui::ReflectedPropertySource* source = section->source.Get();
                (void)menu.AddItem(u8"Reset", [source]() { (void)source->Revert(StringView{}); });
            }
        // Revert to Prefab: shown while a prefab's member overrides it.
        if (m_entities.Size() == 1)
        {
            scene::PrefabMemberInfo member;
            scene::ComponentManagerBase* manager = m_edit->FindManager(type);
            if (manager != nullptr && scene::FindPrefabMember(m_edit->Scene(), m_entities[0], member))
            {
                gui::MenuItem* revert = menu.AddItem(u8"Revert to Prefab", [self, type]() { (void)self->m_edit->RevertComponentToBaseline(self->m_entities[0], type); });
                revert->SetEnabled(scene::IsPrefabComponentOverridden(m_edit->Scene(), member, *manager));
            }
        }
        menu.AddSeparator();
        (void)menu.AddItem(u8"Remove Component", [self, type]()
                           {
                               self->m_edit->Commands().BeginGroup(u8"inspector.remove");
                               for (const Guid& entity : self->m_entities)
                                   self->m_edit->RemoveComponent(entity, type);
                               self->m_edit->Commands().EndGroup();
                           });
    }

    Array<String> SceneInspectorView::AddableComponents() const
    {
        Array<String> out;
        for (const TypeInfo* type : AddableTypes())
            out.PushBack(String(StringView(reinterpret_cast<const utf8char*>(type->name))));
        return out;
    }

    // The types themselves, as the scene's managers hold them (a type is registered under its
    // namespace, so its bare name finds none).
    Array<const TypeInfo*> SceneInspectorView::AddableTypes() const
    {
        Array<const TypeInfo*> out;
        scene::Scene& scene = m_edit->Scene();
        scene.ForEachManager([&](scene::ComponentManagerBase& manager)
                             {
                                 const TypeInfo* type = manager.ComponentType();
                                 if (type == nullptr || type->name == nullptr || type->name[0] == '<')
                                     return; // not reflected: nothing to name or show
                                 // What an entity of the selection lacks and the manager can make.
                                 bool lacking = false;
                                 for (const Guid& entity : m_entities)
                                     lacking = lacking || !manager.HasComponent(scene.FindEntity(entity));
                                 if (lacking)
                                     out.PushBack(type);
                             });
        return out;
    }

    bool SceneInspectorView::AddComponent(StringView typeName)
    {
        const TypeInfo* type = nullptr;
        m_edit->Scene().ForEachManager([&](scene::ComponentManagerBase& manager)
                                       {
                                           const TypeInfo* t = manager.ComponentType();
                                           if (t != nullptr && t->name != nullptr && StringView(reinterpret_cast<const utf8char*>(t->name)) == typeName)
                                               type = t;
                                       });
        if (type == nullptr || m_entities.IsEmpty() || m_readOnly)
            return false;
        m_edit->Commands().BeginGroup(u8"inspector.add");
        for (const Guid& entity : m_entities)
            if (!m_edit->FindManager(type)->HasComponent(m_edit->Resolve(entity)))
                m_edit->AddComponent(entity, type);
        m_edit->Commands().EndGroup();
        return true;
    }

    void SceneInspectorView::OpenAddComponent()
    {
        if (!m_services.pick || m_entities.IsEmpty())
            return;
        app::PickerRequest request;
        request.Placeholder = String(u8"Search components");
        // By category (in their names' order), each component by its display name with its
        // description (one with no category goes under Other).
        Array<app::PickerItem> items;
        for (const TypeInfo* type : AddableTypes())
        {
            app::PickerItem item;
            item.Key = String(StringView(reinterpret_cast<const utf8char*>(type->name)));
            item.Name = TitleOf(*type);
            item.Section = String(TypeAttrString(*type, "category", u8"Other"));
            item.Description = String(TypeAttrString(*type, "description", StringView{})); // a sentence, shown in the picker's foot rather than the row
            if (m_services.icons != nullptr)
            {
                const TypeIcon icon = m_services.icons->For(*type, u8"Component");
                item.Glyph = icon.Source;
                item.GlyphClass = icon.Class;
            }
            items.PushBack(Move(item));
        }
        // A section shows where its first item is, so sort to keep each category's rows together.
        for (usize i = 1; i < items.Size(); ++i)
            for (usize j = i; j > 0; --j)
            {
                const app::PickerItem& a = items[j - 1];
                const app::PickerItem& b = items[j];
                const i32 section = b.Section.AsView().Compare(a.Section.AsView());
                const bool before = section < 0 || (section == 0 && b.Name.AsView().Compare(a.Name.AsView()) < 0);
                if (!before)
                    break;
                app::PickerItem swap = Move(items[j - 1]);
                items[j - 1] = Move(items[j]);
                items[j] = Move(swap);
            }
        for (app::PickerItem& item : items)
            request.Items.PushBack(Move(item));
        request.EmptyTitle = String(u8"Nothing more to add");
        request.EmptyText = String(u8"Every component the scene knows is on the selection.");
        SceneInspectorView* self = this;
        request.OnPicked = [self](StringView key) { (void)self->AddComponent(key); };
        m_services.pick(Move(request));
    }

    void SceneInspectorView::Search(StringView text)
    {
        m_filter = String(text);
        if (m_grid != nullptr)
            m_grid->SetFilter(text);
    }

    String SceneInspectorView::SectionTitles() const
    {
        String out;
        if (m_active)
            out = String(u8"Entity");
        if (m_transform)
            out += u8" | Transform";
        for (const UniquePtr<ComponentSection>& section : m_components)
        {
            out += u8" | ";
            out += section->title;
        }
        for (const UniquePtr<SettingsSection>& section : m_settings)
        {
            if (!out.IsEmpty())
                out += u8" | ";
            out += section->title;
        }
        return out;
    }

    String SceneInspectorView::PathOf(StringView title, StringView property) const
    {
        if (title == StringView(u8"Entity"))
            return m_sections.PathIn(kActiveKey, property);
        if (title == StringView(u8"Transform"))
            return m_sections.PathIn(kTransformKey, property);
        for (const UniquePtr<ComponentSection>& section : m_components)
            if (section->title.AsView() == title)
                return m_sections.PathIn(section->key.AsView(), property);
        // A settings block's own rows, or else an extra source's (the collision groups).
        const auto holds = [property](const gui::IPropertySource& source)
        {
            Array<gui::PropertyInfo> top;
            source.GetProperties(StringView{}, top);
            for (const gui::PropertyInfo& info : top)
                if (info.Name.AsView() == property)
                    return true;
            return false;
        };
        for (const UniquePtr<SettingsSection>& section : m_settings)
        {
            if (section->title.AsView() != title)
                continue;
            if (holds(*section->source))
                return m_sections.PathIn(section->key.AsView(), property);
            for (usize i = 0; section->extras && i < section->extras->SourceCount(); ++i)
                if (holds(section->extras->SourceAt(i)))
                    return m_sections.PathIn(Format(u8"y{}{}", section->key, i).AsView(), property);
        }
        return String{};
    }

    const TypeInfo* SceneInspectorView::ComponentOf(StringView headerPath) const
    {
        const StringView key = m_sections.SectionOf(headerPath);
        for (const UniquePtr<ComponentSection>& section : m_components)
            if (section->key.AsView() == key)
                return section->type;
        return nullptr;
    }

    bool SceneInspectorView::OpenHeaderMenu(StringView headerPath)
    {
        if (m_grid == nullptr)
            return false;
        const i32 row = m_grid->FindRow(headerPath);
        return row >= 0 && m_grid->OpenRowMenu(row, Float2{10.0f, 10.0f});
    }

    StringView SceneInspectorView::Kind() const
    {
        const auto* kind = Find<gui::Label>(u8"inspector-kind");
        return kind != nullptr ? kind->GetText() : StringView{};
    }

    template <typename T>
    T* SceneInspectorView::Find(StringView id) const
    {
        Array<gui::Node*> pending(m_allocator);
        pending.PushBack(m_pane);
        while (!pending.IsEmpty())
        {
            gui::Node* node = pending.Back();
            pending.PopBack();
            if (auto* widget = Cast<gui::UIWidget>(node); widget != nullptr && widget->GetId() == id)
                return Cast<T>(node);
            for (usize i = 0; i < node->ChildCount(); ++i)
                pending.PushBack(node->GetChildAt(i));
        }
        return nullptr;
    }
}
