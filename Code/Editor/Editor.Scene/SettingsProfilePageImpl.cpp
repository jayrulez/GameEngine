// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Scene - :settings_profile_page implementation (see SettingsProfilePage.cppm).

module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"

module editor.scene;

import foundation.core;
import foundation.content;
import foundation.graphics;
import foundation.runtime;
import foundation.runtime.client;
import foundation.scene;
import foundation.resource;
import foundation.geometry;
import foundation.materials;
import engine.render;
import render.pipeline; // SettingsProfileAsset
import foundation.ui;
import foundation.ui.toolkit;
import foundation.ui.runtime;
import editor.core;
import editor.app;
import editor.preview;
import :inspector; // SettingsRows (the rows a scene's settings section shows)
import :scene_loading; // LoadEditorScene, SceneWorldBounds (a project scene to preview on)
import :view_settings; // LoadSceneViewPref (where that scene's page left its camera)

using namespace foundation::core;
namespace scene = foundation::scene;
namespace ui = foundation::ui;

namespace editor
{
    namespace
    {
        [[nodiscard]] pipeline::SettingsProfileAsset* AssetOf(const RefPtr<ISerializable>& object)
        {
            return Cast<pipeline::SettingsProfileAsset>(object.Get());
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
        // A scene that takes its values from a profile renders that profile's COOKED values; the
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

    SettingsProfilePage::SettingsProfilePage(EditorContext& context,
                                             foundation::runtime::IApplicationHost& host,
                                             ui::runtime::UIHost& uiHost,
                                             foundation::content::Instance& instance)
        : app::UIEditorPage(context.Allocator()), m_context(&context), m_title(instance.Name())
    {
        m_asset = instance.ReadObject();
        if (AssetOf(m_asset) == nullptr)
        {
            LOG_ERROR(u8"Editor", u8"profile '{}' failed to read - page opens empty", m_title);
            m_asset = {};
        }
        SetInstanceId(instance.Id());

        m_preview = MakeUnique<PreviewViewport>(Allocator(), host, uiHost, u8"profile.preview");
        LoadPreviewPref();
        LoadPreviewContent();

        m_grid = MakeRef<ui::toolkit::PropertyGrid>(Allocator());
        RebuildGrid();

        auto gridColumn = MakeRef<ui::FlexLayout>(Allocator());
        gridColumn->Direction = ui::Orientation::Vertical;
        gridColumn->Padding = ui::Thickness{8, 6}; // inset like the scene inspector
        {
            ui::LayoutStyle grow;
            grow.FlexGrow = 1.0f;
            gridColumn->AddView(m_grid.Get(), grow);
        }
        auto split = MakeRef<ui::toolkit::SplitView>(Allocator());
        split->SetSplitRatio(0.62f);
        split->SetPanes(m_preview->View(), gridColumn.Get());
        m_toolbar = MakeRef<app::PageToolbar>(Allocator(), *this, m_context->Actions());
        m_content = app::PageToolbar::Frame(Allocator(), *m_toolbar, *split);
    }

    SettingsProfilePage::~SettingsProfilePage() = default;

    void SettingsProfilePage::OnUpdate(foundation::runtime::IApplicationHost&, f32 dt)
    {
        if (m_toolbar.Get() != nullptr)
        {
            m_toolbar->Refresh();
        }
        if (m_preview)
        {
            m_preview->Update(dt);
        }
        for (const Function<void()>& refresher : m_refreshers)
        {
            refresher();
        }
    }

    void SettingsProfilePage::OnRenderWindow(foundation::runtime::IApplicationHost&,
                                             foundation::graphics::FrameContext& frame)
    {
        if (m_preview)
        {
            m_preview->RenderFrame(frame);
        }
    }

    Status SettingsProfilePage::Save()
    {
        if (m_asset.Get() == nullptr || m_context->Project() == nullptr)
        {
            return Status{ErrorCode::NotFound};
        }
        foundation::content::Instance* instance =
            m_context->Project()->SourceDb().GetInstance(InstanceId());
        if (instance == nullptr)
        {
            return Status{ErrorCode::NotFound};
        }
        const Status saved = instance->WriteObject(*m_asset);
        if (saved.IsOk())
        {
            ClearDirty();
            m_context->RequestCook(false); // every scene using the profile takes the new values
            LOG_INFO(u8"Editor", u8"saved profile '{}'", m_title);
        }
        return saved;
    }

    void SettingsProfilePage::OnClose()
    {
        if (m_preview)
        {
            m_preview->Shutdown();
        }
    }

    void SettingsProfilePage::OnAssetExternallyModified()
    {
        // A scene's save wrote a profile-mode edit to the asset: show what is stored now.
        foundation::content::Instance* instance =
            m_context->Project() != nullptr ? m_context->Project()->SourceDb().GetInstance(InstanceId())
                                            : nullptr;
        RefPtr<ISerializable> object = instance != nullptr ? instance->ReadObject() : RefPtr<ISerializable>{};
        if (AssetOf(object) == nullptr)
        {
            return;
        }
        m_asset = Move(object);
        Commands().Clear();
        ClearDirty();
        ApplyPreview();
    }

    void SettingsProfilePage::ApplyAssetBlob(const Array<byte>& blob)
    {
        if (m_asset.Get() == nullptr)
        {
            return;
        }
        MemoryStream stream;
        (void)stream.Write(blob.Data(), blob.Size());
        (void)stream.Seek(0, SeekOrigin::Begin);
        BinarySerializer ar(stream, SerializeMode::Read);
        m_asset->Serialize(ar);
        ApplyPreview();
    }

    Array<byte> SettingsProfilePage::SnapshotAsset() const
    {
        Array<byte> blob;
        if (m_asset.Get() == nullptr)
        {
            return blob;
        }
        MemoryStream stream;
        BinarySerializer ar(stream, SerializeMode::Write);
        m_asset->Serialize(ar);
        const Span<const byte> bytes = stream.Bytes();
        blob.Reserve(bytes.Size());
        for (byte b : bytes)
        {
            blob.PushBack(b);
        }
        return blob;
    }

    void SettingsProfilePage::ApplyEdit(StringView mergeKey, Function<void(void* values)> mutate)
    {
        pipeline::SettingsProfileAsset* asset = AssetOf(m_asset);
        if (asset == nullptr)
        {
            return;
        }
        Array<byte> before = SnapshotAsset();
        mutate(asset->Values());
        Array<byte> after = SnapshotAsset();
        // The mutation already ran; Execute re-applies `after` (and refreshes the preview).
        (void)Commands().Execute(UniquePtr<IEditorCommand>(
            Allocator().New<EditProfileCommand>(*this, mergeKey, Move(before), Move(after)),
            Allocator()));
    }

    void SettingsProfilePage::BuildPreviewScene()
    {
        scene::Scene* preview = m_preview ? m_preview->Scene() : nullptr;
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
        scene::Scene* preview = m_preview ? m_preview->Scene() : nullptr;
        if (preview != nullptr && m_asset.Get() != nullptr)
        {
            (void)PreviewSettingsProfileInScene(*m_asset, *preview, m_context->Resources());
        }
    }

    void SettingsProfilePage::SetPreviewScene(const Guid& sceneId)
    {
        if (sceneId == m_previewSceneId)
        {
            return;
        }
        m_previewSceneId = sceneId;
        LoadPreviewContent();
        SavePreviewPref();
    }

    void SettingsProfilePage::LoadPreviewContent()
    {
        if (!m_preview)
        {
            return;
        }
        m_previewContent.Clear();
        scene::Scene* preview = m_preview->ResetScene();
        if (preview == nullptr)
        {
            return;
        }
        foundation::content::Instance* sceneDocument =
            (!m_previewSceneId.IsNil() && m_context->Project() != nullptr)
                ? m_context->Project()->SourceDb().GetInstance(m_previewSceneId)
                : nullptr;
        bool loaded = false;
        if (sceneDocument != nullptr)
        {
            loaded = LoadEditorScene(*m_context, *sceneDocument, *preview).IsOk();
            if (!loaded)
            {
                LOG_WARNING(u8"Editor", u8"profile '{}': preview scene '{}' did not load - showing the built-in one",
                            m_title, sceneDocument->Name());
                preview = m_preview->ResetScene();
            }
        }
        EditorCamera& camera = m_preview->Camera();
        if (loaded)
        {
            // From where that scene's page left its camera, else the whole scene in view.
            const SceneViewPref view =
                LoadSceneViewPref(m_context->ProjectEditorSettings(), m_previewSceneId, SceneViewPref{});
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
        foundation::settings::Settings* store = m_context->ProjectEditorSettings();
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
        foundation::settings::Settings* store = m_context->ProjectEditorSettings();
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
        m_context->RequestProjectEditorSettingsSave();
    }

    void SettingsProfilePage::RebuildGrid()
    {
        m_grid->Clear();
        m_refreshers.Clear();
        pipeline::SettingsProfileAsset* asset = AssetOf(m_asset);
        if (asset == nullptr)
        {
            return;
        }
        SettingsProfilePage* page = this;
        const TypeInfo* type = asset->ValuesType();
        auto access = MakeRef<SettingsAccess>(Allocator());
        access->type = type;
        access->values = [page](const char*) -> void*
        {
            pipeline::SettingsProfileAsset* a = AssetOf(page->m_asset);
            return a != nullptr ? a->Values() : nullptr;
        };
        access->set = [page, type](const char* property, const Variant& value)
        {
            page->ApplyEdit(StringView(reinterpret_cast<const utf8char*>(property)),
                            [type, property, value](void* values)
                            {
                                if (const PropertyInfo* p = FindProperty(*type, property))
                                {
                                    (void)SetProperty(*p, Instance{values, type}, value);
                                }
                            });
        };
        access->setRaw = [page, type](const char* property, i64 raw)
        {
            page->ApplyEdit(StringView(reinterpret_cast<const utf8char*>(property)),
                            [type, property, raw](void* values)
                            {
                                const PropertyInfo* p = FindProperty(*type, property);
                                if (p != nullptr && p->address != nullptr)
                                {
                                    WriteEnumValue(p->address(Instance{values, type}), *p->type, raw);
                                }
                            });
        };
        access->setReference = [page, type](const char* property, const Guid& id)
        {
            page->ApplyEdit(StringView(reinterpret_cast<const utf8char*>(property)),
                            [type, property, id](void* values)
                            {
                                const PropertyInfo* p = FindProperty(*type, property);
                                if (p != nullptr && p->address != nullptr &&
                                    p->type->reference != nullptr)
                                {
                                    p->type->reference->SetId(p->address(Instance{values, type}), id);
                                }
                            });
        };

        // The scene the profile is previewed on (page-local: not the profile's data, remembered
        // per profile in the project's editor store).
        {
            const StringView sceneTypes[] = {u8"SceneDocument"};
            auto sceneRow = MakeRef<ResourceRefEditor>(Allocator(), StringView(u8"Scene"), StringView(u8"(built-in)"),
                                                       StringView(u8"Preview"), Span<const StringView>{sceneTypes, 1});
            sceneRow->SetEmptyText(u8"(built-in)");
            sceneRow->BindAsset(*m_context, [page]() { return page->m_previewSceneId; },
                                [page](const Guid& picked) { page->SetPreviewScene(picked); });
            m_grid->AddProperty(RefPtr<ui::toolkit::PropertyEditor>(sceneRow.Get()));
        }

        SettingsRows rows(*m_context, *m_grid, m_refreshers);
        const StringView category = SettingsCategoryName(*type);
        for (const PropertyInfo& prop : Properties(*type))
        {
            // A scene's own fields (its source, its profile) are not a profile's.
            if (IsNested(prop) || FindAttribute(prop, u8"sceneOnly") != nullptr)
            {
                continue;
            }
            rows.Build(access, prop, category);
        }
    }

    const TypeInfo* SettingsProfilePageFactory::PrimaryType() const
    {
        return &pipeline::SettingsProfileAsset::StaticType();
    }

    UniquePtr<EditorPage> SettingsProfilePageFactory::CreatePage(EditorContext& context,
                                                                 foundation::content::Instance& instance)
    {
        SettingsProfilePage* page = editor::EditorRootAllocator().New<SettingsProfilePage>(
            context, *m_host, *m_uiHost, instance);
        return UniquePtr<EditorPage>(page, editor::EditorRootAllocator());
    }

    void RegisterSettingsProfileEditor(EditorContext& context,
                                       foundation::runtime::IApplicationHost& host,
                                       ui::runtime::UIHost& uiHost)
    {
        // The preview-choice section (registered before the app loads the per-project store).
        GlobalTypeRegistry().Register(ProfilePreviewSettings::StaticType(), TypeDomain(u8"Editor"));
        RegisterSerializable<ProfilePreviewSettings>();
        context.Pages().Register(UniquePtr<IEditorPageFactory>(
            editor::EditorRootAllocator().New<SettingsProfilePageFactory>(host, uiHost),
            editor::EditorRootAllocator()));
    }
}
