// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Render - :settings_profile_page implementation (see SettingsProfilePage.cppm).

module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"
#include "Core/Reflection/Reflect.h"

module editor.render;

import foundation.core;
import foundation.content;
import foundation.graphics;
import foundation.runtime;
import foundation.runtime.client;
import foundation.scene;
import foundation.scene.resource; // SceneDocument, what the preview's scene slot takes
import foundation.settings; // the project's editor store (where the preview scene choice is kept)
import foundation.resource;
import foundation.geometry;
import foundation.materials;
import engine.render;
import render.pipeline; // SettingsProfileAsset
import foundation.gui;
import foundation.gui.toolkit;
import editor.core;
import editor.app;
import editor.preview;
import editor.scene; // LoadEditorScene, SceneWorldBounds, LoadSceneViewPref

using namespace foundation::core;
namespace scene = foundation::scene;
namespace gui = foundation::gui;

namespace editor
{
    namespace
    {
        [[nodiscard]] pipeline::SettingsProfileAsset* AssetOf(ISerializable* object)
        {
            return Cast<pipeline::SettingsProfileAsset>(object);
        }
    }

    bool ApplySettingsProfileToScene(ISerializable& profileAsset, scene::Scene& scene,
                                     foundation::resource::ResourceManager* resources)
    {
        auto* asset = Cast<pipeline::SettingsProfileAsset>(&profileAsset);
        if (asset == nullptr)
        {
            return false;
        }
        scene::SceneSystem* block = nullptr;
        scene.ForEachSystem(
            [&](scene::SceneSystem& system)
            {
                if (block == nullptr && system.SettingsType() == asset->ValuesType())
                {
                    block = &system;
                }
            });
        if (block == nullptr)
        {
            return false;
        }
        asset->CopyValuesInto(block->SettingsInstance());
        if (resources != nullptr)
        {
            block->ResolveResources(*resources); // the sky texture, the grading LUT
        }
        return true;
    }

    bool PreviewSettingsProfileInScene(ISerializable& profileAsset, scene::Scene& target,
                                       foundation::resource::ResourceManager* resources)
    {
        auto* asset = Cast<pipeline::SettingsProfileAsset>(&profileAsset);
        if (asset == nullptr)
        {
            return false;
        }
        // A scene that takes its values from a profile renders that profile's cooked values; the
        // preview shows the ones being edited, so the block goes back to its own first.
        target.ForEachSystem(
            [&](scene::SceneSystem& system)
            {
                if (system.SettingsType() == asset->ValuesType())
                {
                    system.UseSettingsProfile(Guid{});
                }
            });
        return ApplySettingsProfileToScene(profileAsset, target, resources);
    }

    // The profile's values as the grid shows them: the settings type's reflection without the
    // block's own fields (nested ones, and those marked `sceneOnly`, such as the source and the
    // profile reference), which belong to a scene rather than a profile.
    class SettingsProfilePage::ValuesSource final : public gui::IPropertySource, private gui::IPropertySourceClient
    {
    public:
        ValuesSource(IAllocator& allocator, gui::IPropertyTextFormat& format) : m_inner(allocator), m_skipped(allocator)
        {
            m_inner.SetTextFormat(&format);
            m_inner.SetClient(this);
        }
        ~ValuesSource() override { m_inner.SetClient(nullptr); }

        /// Over `values` of `type` (null: none).
        void Bind(void* values, const TypeInfo* type)
        {
            m_skipped.Clear();
            if (values == nullptr || type == nullptr)
            {
                m_inner.SetObjects(Span<const Instance>{});
                return;
            }
            for (const PropertyInfo& property : foundation::core::Properties(*type)) // not the page's Properties()
                if (IsNested(property) || FindAttribute(property, u8"sceneOnly") != nullptr)
                    m_skipped.PushBack(String(StringView(reinterpret_cast<const utf8char*>(property.name))));
            const Instance object(values, type);
            m_inner.SetObjects(Span<const Instance>(&object, 1u));
        }
        [[nodiscard]] gui::ReflectedPropertySource& Inner() noexcept { return m_inner; }
        /// Called whenever the values change under it: a gesture's previews (written in place as it
        /// goes), its cancel, a commit or an undo. The page's preview follows them.
        Function<void()> OnValuesWritten;
        void Refresh() { m_inner.Refresh(); }

        [[nodiscard]] usize ObjectCount() const override { return m_inner.ObjectCount(); }
        void GetProperties(StringView parent, Array<gui::PropertyInfo>& out) const override
        {
            if (!parent.IsEmpty())
            {
                m_inner.GetProperties(parent, out);
                return;
            }
            Array<gui::PropertyInfo> all;
            m_inner.GetProperties(parent, all);
            for (gui::PropertyInfo& info : all)
            {
                bool skipped = false;
                for (const String& name : m_skipped)
                    skipped = skipped || name == info.Name;
                if (!skipped)
                    out.PushBack(Move(info));
            }
        }
        [[nodiscard]] gui::Variant GetValue(StringView path, usize object) const override { return m_inner.GetValue(path, object); }
        void BeginEdit(StringView path) override { m_inner.BeginEdit(path); }
        void PreviewEdit(StringView path, const gui::PropertyWrite& write) override { m_inner.PreviewEdit(path, write); }
        void CommitEdit(StringView path, const gui::PropertyWrite& write) override { m_inner.CommitEdit(path, write); }
        void CancelEdit(StringView path) override { m_inner.CancelEdit(path); }
        bool EditList(StringView path, const gui::ListEdit& edit) override { return m_inner.EditList(path, edit); }
        [[nodiscard]] gui::Variant GetDefault(StringView path, usize object) const override { return m_inner.GetDefault(path, object); }
        bool Revert(StringView path) override { return m_inner.Revert(path); }
        [[nodiscard]] String GetRevertText(StringView path) const override { return m_inner.GetRevertText(path); }
        [[nodiscard]] String CopyValue(StringView path) const override { return m_inner.CopyValue(path); }
        bool PasteValue(StringView path, StringView text, String& reason) override { return m_inner.PasteValue(path, text, reason); }

    private:
        void OnPropertiesChanged() override { NotifyPropertiesChanged(); }
        void OnValuesChanged() override
        {
            NotifyValuesChanged();
            if (OnValuesWritten)
                OnValuesWritten();
        }

        mutable gui::ReflectedPropertySource m_inner;
        Array<String> m_skipped;
    };

    SettingsProfilePage::SettingsProfilePage(EditorContext& context, app::AssetPageServices services, foundation::content::Instance& instance)
        : app::AssetObjectPage(context, Move(services), instance, pipeline::SettingsProfileAsset::StaticType(), kPath), m_format(context.Allocator())
    {
        // The asset's own reflection is its wrapper; the rows show its values, as a scene's
        // settings do.
        HideAssetRows();
        m_values = MakeUnique<ValuesSource>(Allocator(), Allocator(), m_format);
        SettingsProfilePage* self = this;
        m_values->Inner().SetOnChange([self](const gui::ReflectedChange& change)
                                      {
                                          pipeline::SettingsProfileAsset* asset = AssetOf(self->Object());
                                          if (asset == nullptr)
                                              return;
                                          (void)self->Commands().Execute(UniquePtr<IEditorCommand>(
                                              EditorRootAllocator().New<app::ReflectedEditCommand>(
                                                  *asset->ValuesType(),
                                                  [self]() -> void*
                                                  {
                                                      pipeline::SettingsProfileAsset* a = AssetOf(self->Object());
                                                      return a != nullptr ? a->Values() : nullptr;
                                                  },
                                                  change, self->m_format, u8"edit_settings_profile",
                                                  [self]()
                                                  {
                                                      self->m_values->Refresh(); // an undo wrote it behind the source
                                                      self->ApplyPreview();
                                                  }),
                                              EditorRootAllocator()));
                                      });
        // The preview scene follows the values as they change, including during a slider's drag,
        // not only on the commit at its release.
        m_values->OnValuesWritten = [self]() { self->ApplyPreview(); };
        BindValues();
        if (pipeline::SettingsProfileAsset* asset = AssetOf(Object()))
            AddSection(u8"values", TypeAttrString(*asset->ValuesType(), "displayName", u8"Values"), *m_values);
        if (Content())
        {
            m_pane = MakeUnique<app::PreviewPane>(Allocator(), *Content(), Services(), u8"profile.preview");
            m_pane->OnFrame = [self]()
            {
                AABB bounds;
                if (self->Preview() != nullptr && self->Preview()->Scene() != nullptr && SceneWorldBounds(*self->Preview()->Scene(), bounds))
                    self->Preview()->Frame(bounds.Center(), Length(bounds.Extents()));
            };
            // The scene the profile is previewed on: the built-in one, or one of the project's.
            m_sceneSlot = MakeRef<app::AssetPickerSlot>(Allocator());
            m_sceneSlot->AddClass(u8"preview-slot");
            m_sceneSlot->SetTooltip(u8"A scene of the project to preview the profile in (the built-in one when none); kept for this profile");
            const app::ResourceRefServices* refs = Services().refs;
            if (refs != nullptr && refs->AcceptedTypes)
                m_sceneSlot->SetAcceptedTypes(refs->AcceptedTypes(scene::SceneDocument::StaticType()));
            if (refs != nullptr && refs->Pick)
                m_sceneSlot->SetOnPick([self, refs]()
                                       {
                                           refs->Pick(self->m_sceneSlot->AcceptedTypes(), self->m_previewSceneId,
                                                      [self](const Guid& picked) { self->SetPreviewScene(picked); });
                                       });
            m_sceneSlot->SetOnClear([self]() { self->SetPreviewScene(Guid{}); });
            m_sceneSlot->SetOnDropped([self](const Guid& dropped) { self->SetPreviewScene(dropped); });
            m_pane->AddControl(*m_sceneSlot);
        }
        LoadPreviewPref();
        LoadPreviewContent();
        ShowScene();
        if (m_pane && !LoadFailed())
            m_pane->Show();
    }

    SettingsProfilePage::~SettingsProfilePage() = default;

    gui::IPropertySource& SettingsProfilePage::Rows() noexcept
    {
        return *m_values;
    }

    void SettingsProfilePage::EditValue(StringView path, const gui::PropertyWrite& write)
    {
        m_values->BeginEdit(path);
        m_values->CommitEdit(path, write);
    }

    void SettingsProfilePage::BindValues()
    {
        pipeline::SettingsProfileAsset* asset = AssetOf(Object());
        m_values->Bind(asset != nullptr ? asset->Values() : nullptr, asset != nullptr ? asset->ValuesType() : nullptr);
    }

    void SettingsProfilePage::OnObjectChanged()
    {
        BindValues(); // read again: the values live in another object now
        ApplyPreview();
    }

    void SettingsProfilePage::OnUpdate(foundation::runtime::IApplicationHost& host, f32 dt)
    {
        app::AssetObjectPage::OnUpdate(host, dt);
        if (m_pane)
            m_pane->Update(dt);
    }

    void SettingsProfilePage::OnRenderWindow(foundation::runtime::IApplicationHost&, foundation::graphics::FrameContext& frame)
    {
        if (m_pane)
            m_pane->Render(frame);
    }

    void SettingsProfilePage::OnClose()
    {
        if (m_pane)
            m_pane->Close();
    }

    void SettingsProfilePage::OnAssetExternallyModified()
    {
        // A scene's save wrote a profile-mode edit to the asset: show what is stored now.
        (void)ReadAgain();
    }

    void SettingsProfilePage::BuildPreviewScene()
    {
        scene::Scene* preview = Preview() != nullptr ? Preview()->Scene() : nullptr;
        auto* meshes =
            preview != nullptr ? preview->GetSystem<engine::render::MeshComponentManager>() : nullptr;
        if (meshes == nullptr)
        {
            return;
        }
        namespace geometry = foundation::geometry;
        namespace materials = foundation::materials;
        // Runtime-built content, kept alive by the page (the components hold direct overrides).
        auto place = [&](StringView name, const RefPtr<geometry::StaticMesh>& mesh,
                         const RefPtr<materials::Material>& material, Float3 position)
        {
            const scene::EntityHandle e = preview->CreateEntity(name);
            Transform t;
            t.position = position;
            preview->SetLocalTransform(e, t);
            engine::render::MeshComponent& mc = meshes->Add(e);
            mc.mesh = mesh.Get();
            mc.SetMaterial(material);
            m_previewContent.PushBack(RefPtr<RefCounted>(mesh.Get()));
            m_previewContent.PushBack(RefPtr<RefCounted>(material.Get()));
        };
        // Colours are sRGB, as entered everywhere.
        place(u8"Ground", geometry::Primitives::Plane(Allocator(), 14.0f, 14.0f),
              materials::CreatePBR(u8"preview.ground", Float4{0.55f, 0.55f, 0.55f, 1}, 0.0f, 0.9f),
              Float3{0.0f, 0.0f, 0.0f});
        const RefPtr<geometry::StaticMesh> sphere =
            geometry::Primitives::Sphere(Allocator(), 0.5f, 48, 24);
        place(u8"Matte", sphere,
              materials::CreatePBR(u8"preview.matte", Float4{0.9f, 0.9f, 0.9f, 1}, 0.0f, 0.6f),
              Float3{-1.2f, 0.5f, 0.0f});
        place(u8"Metal", sphere,
              materials::CreatePBR(u8"preview.metal", Float4{0.95f, 0.8f, 0.45f, 1}, 1.0f, 0.3f),
              Float3{0.0f, 0.5f, 0.0f});
        place(u8"Gloss", sphere,
              materials::CreatePBR(u8"preview.gloss", Float4{0.8f, 0.15f, 0.12f, 1}, 0.0f, 0.1f),
              Float3{1.2f, 0.5f, 0.0f});
        place(u8"Cube", geometry::Primitives::Cube(Allocator(), 0.8f),
              materials::CreatePBR(u8"preview.cube", Float4{0.3f, 0.45f, 0.75f, 1}, 0.0f, 0.7f),
              Float3{2.6f, 0.4f, -0.8f});

        const scene::EntityHandle sun = preview->CreateEntity(u8"Sun");
        Transform t;
        t.rotation = Quaternion::FromAxisAngle(Float3{0, 1, 0}, 0.6f) *
                     Quaternion::FromAxisAngle(Float3{1, 0, 0}, -0.9f);
        preview->SetLocalTransform(sun, t);
        if (auto* lights = preview->GetSystem<engine::render::LightComponentManager>())
        {
            engine::render::LightComponent& light = lights->Add(sun);
            light.castsShadows = true; // the profile's shadow reach and fade show on the ground
        }
    }

    void SettingsProfilePage::ApplyPreview()
    {
        ++m_previewsApplied;
        scene::Scene* preview = Preview() != nullptr ? Preview()->Scene() : nullptr;
        if (preview != nullptr && Object() != nullptr)
            (void)PreviewSettingsProfileInScene(*Object(), *preview, Context().Resources());
    }

    void SettingsProfilePage::SetPreviewScene(const Guid& sceneId)
    {
        if (sceneId == m_previewSceneId)
            return;
        m_previewSceneId = sceneId;
        LoadPreviewContent();
        SavePreviewPref();
        ShowScene();
    }

    void SettingsProfilePage::ShowScene()
    {
        if (!m_sceneSlot)
            return;
        app::ResourceRefShown shown;
        const app::ResourceRefServices* refs = Services().refs;
        if (!m_previewSceneId.IsNil() && refs != nullptr && refs->Describe && refs->Describe(m_previewSceneId, shown))
        {
            m_sceneSlot->SetValue(shown.Name.AsView());
            m_sceneSlot->SetGlyph(shown.Glyph.AsView(), shown.GlyphClass.AsView());
            return;
        }
        m_sceneSlot->SetValue(StringView{}); // the built-in scene
    }

    void SettingsProfilePage::LoadPreviewContent()
    {
        if (Preview() == nullptr)
        {
            return;
        }
        m_previewContent.Clear();
        scene::Scene* preview = Preview()->ResetScene();
        if (preview == nullptr)
        {
            return;
        }
        foundation::content::Instance* sceneDocument =
            (!m_previewSceneId.IsNil() && Context().Project() != nullptr)
                ? Context().Project()->SourceDb().GetInstance(m_previewSceneId)
                : nullptr;
        bool loaded = false;
        if (sceneDocument != nullptr)
        {
            loaded = LoadEditorScene(Context(), *sceneDocument, *preview).IsOk();
            if (!loaded)
            {
                LOG_WARNING(u8"Editor", u8"profile '{}': preview scene '{}' did not load - showing the built-in one",
                            Title(), sceneDocument->Name());
                preview = Preview()->ResetScene();
            }
        }
        EditorCamera& camera = Preview()->Camera();
        if (loaded)
        {
            // From where that scene's page left its camera, else the whole scene in view.
            const SceneViewPref view =
                LoadSceneViewPref(Context().ProjectEditorSettings(), m_previewSceneId, SceneViewPref{});
            AABB bounds;
            if (view.hasCamera)
            {
                camera.position = view.cameraPosition;
                camera.yaw = view.cameraYaw;
                camera.pitch = view.cameraPitch;
                camera.focusDistance = view.cameraFocusDistance;
            }
            else if (SceneWorldBounds(*preview, bounds))
            {
                camera.FrameSphere(bounds.Center(), Length(bounds.Extents()), 1.0472f, /*ease*/ false);
            }
        }
        else
        {
            camera.position = Float3{0.0f, 1.7f, 5.0f};
            camera.LookAt(Float3{0.0f, 0.5f, 0.0f});
            BuildPreviewScene();
        }
        ApplyPreview();
    }

    void SettingsProfilePage::LoadPreviewPref()
    {
        foundation::settings::Settings* store = Context().ProjectEditorSettings();
        if (store == nullptr)
        {
            return;
        }
        if (const ProfilePreviewSettings* section = store->Find<ProfilePreviewSettings>())
        {
            for (const ProfilePreviewPref& pref : section->prefs)
            {
                if (pref.profile == InstanceId())
                {
                    m_previewSceneId = pref.scene;
                    return;
                }
            }
        }
    }

    void SettingsProfilePage::SavePreviewPref()
    {
        foundation::settings::Settings* store = Context().ProjectEditorSettings();
        if (store == nullptr)
        {
            return;
        }
        ProfilePreviewSettings& section = store->Section<ProfilePreviewSettings>();
        bool found = false;
        for (ProfilePreviewPref& pref : section.prefs)
        {
            if (pref.profile == InstanceId())
            {
                pref.scene = m_previewSceneId;
                found = true;
                break;
            }
        }
        if (!found)
        {
            section.prefs.PushBack(ProfilePreviewPref{InstanceId(), m_previewSceneId});
        }
        store->MarkChanged<ProfilePreviewSettings>();
        Context().RequestProjectEditorSettingsSave();
    }

    const TypeInfo* SettingsProfilePageFactory::PrimaryType() const
    {
        return &pipeline::SettingsProfileAsset::StaticType();
    }

    UniquePtr<EditorPage> SettingsProfilePageFactory::CreatePage(EditorContext& context, foundation::content::Instance& instance)
    {
        auto* page = editor::EditorRootAllocator().New<SettingsProfilePage>(context, m_services->Make(), instance);
        return UniquePtr<EditorPage>(page, editor::EditorRootAllocator());
    }

    void RegisterSettingsProfileEditor(EditorContext& context, const app::AssetPageServicesSource& services)
    {
        // The preview-choice section (registered before the app loads the per-project store).
        GlobalTypeRegistry().Register(ProfilePreviewSettings::StaticType(), TypeDomain(u8"Editor"));
        RegisterSerializable<ProfilePreviewSettings>();
        context.Pages().Register(UniquePtr<IEditorPageFactory>(
            editor::EditorRootAllocator().New<SettingsProfilePageFactory>(services), editor::EditorRootAllocator()));
    }
}
