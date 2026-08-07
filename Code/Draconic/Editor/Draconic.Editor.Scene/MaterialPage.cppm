// Draconic::EditorScene - :material_page partition.
//
// MaterialEditorPage (Sedulous MaterialEditorPage shape): edits a MaterialAsset - a preview
// sphere lit by a default sun + procedural sky on the left, the material's parameters on the
// right. The authored form IS the runtime source (MaterialSource), so Save just writes the
// object back and requests a re-cook; every live proxy bound to the cooked product then
// hot-swaps (the same reload path texture/mesh edits ride).
//
// Edits are BLOB-SNAPSHOT commands: each edit captures the whole serialized MaterialSource
// before/after (it is tiny) - robust against the source's parallel-array layout, exact undo,
// and consecutive scrubs of the same field merge into one entry. Every apply rebuilds the
// preview's runtime material in place, so scrubbing reads live on the sphere.
//
// RegisterMaterialEditor is the module's RegisterEditor entry point (§3.1): registers the
// MaterialAsset page factory and the "PBR Material" / "Unlit Material" creators (presets;
// custom shader-backed materials come later with the shader-asset story).

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Log/Log.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.editor.scene:material_page;

import draconic.foundation;
import draconic.vfs;
import draconic.settings;
import draconic.content;
import draconic.rhi;
import draconic.graphics;
import draconic.shell;
import draconic.runtime;
import draconic.runtime.client;
import draconic.scene;
import draconic.engine.scene;
import draconic.geometry;
import draconic.materials;
import draconic.materials.resource;
import draconic.materials.editor;
import draconic.texture.resource;
import draconic.resource;
import draconic.shaders;
import draconic.render;
import draconic.engine.render;
import draconic.ui;
import draconic.ui.toolkit;
import draconic.ui.runtime;
import draconic.ui.viewport;
import draconic.vg.renderer;
import draconic.editor.core;
import draconic.editor.app;
import :camera;    // EditorCamera (fly camera on the preview viewport)
import :inspector; // ResourceRefEditor (the picker row)

using namespace draconic::foundation;

export namespace draconic::editor
{
    namespace runtime = draconic::runtime;
    namespace ui = draconic::ui;
    namespace vg = draconic::vg;
    namespace scene = draconic::scene;
    namespace render = draconic::render;
    namespace materials = draconic::materials;

    class MaterialEditorPage final : public app::UIEditorPage
    {
    public:
        MaterialEditorPage(EditorContext& context, runtime::IApplicationHost& host,
                           ui::runtime::UIHost& uiHost, draconic::content::Instance& instance)
            : m_context(&context), m_host(&host), m_uiHost(&uiHost), m_title(instance.Name())
        {
            // Fly camera on the preview viewport (hover/focus-gated devices, like scene pages).
            m_router =
                MakeUnique<draconic::shell::InputRouter>(DefaultAllocator(), host.Shell()->Input());
            m_camera.position = Float3{0.0f, 0.9f, 2.6f};
            m_camera.LookAt(Float3{0.0f, 0.0f, 0.0f});

            // The edited object: the instance's MaterialAsset (kept live; Save writes it back).
            RefPtr<ISerializable> object = instance.ReadObject();
            m_asset =
                RefPtr<materials::MaterialAsset>(Cast<materials::MaterialAsset>(object.Get()));
            if (m_asset.Get() != nullptr)
            {
                // Pre-emissive assets gain the factor in memory (black default); saving the
                // page persists the upgraded table (the load-time upgrade covers unsaved ones).
                materials::UpgradeForwardMaterialSource(m_asset->source);
            }
            if (m_asset.Get() == nullptr)
            {
                DRACONIC_LOG_ERROR(u8"Editor", u8"material '{}' failed to read - page opens empty",
                                   m_title);
            }

            m_scenes = host.Ctx().GetSubsystem<scene::SceneSubsystem>();
            m_render = host.Ctx().GetSubsystem<render::RenderSubsystem>();

            BuildPreviewScene();

            // The context assigns the page's instance id AFTER construction (OpenPage), but
            // the preview-pref restore below keys on it - set it from the instance now
            // (the context's later SetInstanceId writes the same value).
            SetInstanceId(instance.Id());

            // Restore this material's saved preview choice (shape or mesh asset) before the
            // grid builds its rows, so the Shape/Mesh rows show the persisted state.
            LoadPreviewPref();
            if (m_previewShape != 0 || !m_previewMeshGuid.IsNil())
            {
                ApplyPreviewMesh();
            }

            m_viewport = MakeRef<ui::viewport::ViewportView>(DefaultAllocator());
            m_viewport->ClearColor = rhi::ClearColor{0.10f, 0.11f, 0.13f, 1.0f};

            m_grid = MakeRef<draconic::ui::toolkit::PropertyGrid>(DefaultAllocator());
            RebuildGrid();

            // Inset the property grid off the pane edge (matches the scene inspector / hierarchy).
            auto gridColumn = MakeRef<draconic::ui::FlexLayout>(DefaultAllocator());
            gridColumn->Direction = draconic::ui::Orientation::Vertical;
            gridColumn->Padding = draconic::ui::Thickness{8, 6};
            {
                auto grow = MakeRef<draconic::ui::FlexLayoutParams>(DefaultAllocator());
                grow->Grow = 1.0f;
                gridColumn->AddView(m_grid.Get(), grow);
            }

            m_content = MakeRef<draconic::ui::toolkit::SplitView>(DefaultAllocator());
            m_content->SetSplitRatio(0.62f);
            m_content->SetPanes(m_viewport.Get(), gridColumn.Get());

            RebuildPreviewMaterial();
        }

        // === UIEditorPage ===

        [[nodiscard]] draconic::ui::View* ContentView() override { return m_content.Get(); }
        [[nodiscard]] StringView Title() const override { return m_title.AsView(); }

        void OnUpdate(runtime::IApplicationHost&, f32 dt) override;

        void OnRenderWindow(runtime::IApplicationHost&,
                            draconic::graphics::FrameContext& frame) override;

        [[nodiscard]] Status Save() override;

        void OnClose() override;

        // Deserialize a source blob into the live asset + refresh the preview (the command
        // stack's apply path - Execute and Undo both land here).
        void ApplySourceBlob(const Array<byte>& blob);

        [[nodiscard]] Array<byte> SnapshotSource() const;

    private:
        // Whole-source snapshot command: before/after blobs + a merge key (consecutive scrubs
        // of the same field collapse; the FIRST command keeps the original `before`).
        class EditMaterialCommand final : public IEditorCommand
        {
        public:
            EditMaterialCommand(MaterialEditorPage& page, StringView mergeKey, Array<byte> before,
                                Array<byte> after)
                : m_page(&page), m_mergeKey(mergeKey), m_before(Move(before)), m_after(Move(after))
            {
            }

            [[nodiscard]] bool Execute() override
            {
                m_page->ApplySourceBlob(m_after);
                return true;
            }
            void Undo() override { m_page->ApplySourceBlob(m_before); }
            [[nodiscard]] StringView TypeId() const override { return u8"edit_material"; }
            [[nodiscard]] bool MergeInto(IEditorCommand& previous) override
            {
                auto& prev = static_cast<EditMaterialCommand&>(previous);
                if (prev.m_page != m_page || prev.m_mergeKey.AsView() != m_mergeKey.AsView())
                {
                    return false;
                }
                prev.m_after = Move(m_after);
                return true;
            }

        private:
            MaterialEditorPage* m_page;
            String m_mergeKey;
            Array<byte> m_before;
            Array<byte> m_after;
        };

        // Run one edit as an undoable command: snapshot -> mutate -> snapshot -> push.
        void ApplyEdit(StringView mergeKey, Function<void(materials::MaterialSource&)> mutate);

        // Preview world: the material sphere + the standard seeded sun + default environment
        // (the render subsystem injects EnvironmentSystem on CreateScene - procedural sky/IBL).
        void BuildPreviewScene();

        // Build the runtime preview Material from the CURRENT source (the factory's conversion,
        // with textures resolved through the editor's cooked-DB resources) and swap it onto the
        // sphere. Runs on every applied edit, so scrubs read live.
        void RebuildPreviewMaterial();

        // === the parameter grid ===

        // Swap the preview geometry: a built-in primitive, or any mesh asset from the
        // project (imported models show the material with their real UVs).
        // === Preview prefs (a section in the per-project editor-settings store) ===
        // Editor state, NOT on the MaterialAsset: the preview choice is a per-user pref,
        // never a build input (the Sedulous editor kept these in its asset-cache sidecar).
        // Lives in EditorContext::ProjectEditorSettings() alongside layout/favorites/pages.

        void LoadPreviewPref();

        void SavePreviewPref();

        void ApplyPreviewMesh();

        // Reframe the fly camera to fit `mesh` (asset meshes vary wildly in size).
        void FramePreview(const draconic::geometry::StaticMesh* mesh);

        void RebuildGrid();

        // A pipeline-state dropdown writing one of the source's u8 mode fields.
        void AddPipelineEnumRow(StringView label, Span<const StringView> items,
                                u8& (*field)(materials::MaterialSource&));

        // A texture-slot picker row (TextureAsset picker; [Clear] unbinds the slot).
        void AddTextureRow(const String& slot);

        [[nodiscard]] draconic::ui::UIContext* Context() const noexcept { return m_grid->Context; }

        [[nodiscard]] StringView AssetNameFor(const Guid& target);

        void AddEditor(draconic::ui::toolkit::PropertyEditor* editor, Function<void()> refresher);

        // Uniform blob access by property name (offset/size from the source's tables).
        void ReadUniform(StringView name, void* out, usize bytes) const;
        void WriteUniform(StringView name, const void* value, usize bytes);

        void EnsureViewportBound();

        EditorContext* m_context;
        runtime::IApplicationHost* m_host;
        ui::runtime::UIHost* m_uiHost;
        String m_title;

        RefPtr<materials::MaterialAsset> m_asset;

        scene::SceneSubsystem* m_scenes = nullptr;
        scene::SceneManager
            m_sceneManager; // this page's OWN preview scene group (registered with m_scenes)
        render::RenderSubsystem* m_render = nullptr;
        scene::Scene* m_scene = nullptr;
        scene::EntityHandle m_sphere;
        RefPtr<draconic::geometry::StaticMesh> m_previewMesh;
        RefPtr<materials::Material> m_previewMaterial;
        EditorCamera m_camera;
        UniquePtr<draconic::shell::InputRouter> m_router;
        u32 m_previewShape = 0; // index into the Shape enum row
        Guid m_previewMeshGuid; // nil = primitive shape
        Array<draconic::resource::Proxy<draconic::texture::Texture>> m_previewTextures;
        Array<rhi::TextureView*> m_previewTextureViews; // views captured into the material

        RefPtr<ui::viewport::ViewportView> m_viewport;
        RefPtr<draconic::ui::toolkit::PropertyGrid> m_grid;
        RefPtr<draconic::ui::toolkit::SplitView> m_content;
        Array<Function<void()>> m_refreshers;
        draconic::graphics::RenderWindow* m_hostWindow = nullptr;
    };

    class MaterialEditorPageFactory final : public IEditorPageFactory
    {
    public:
        MaterialEditorPageFactory(runtime::IApplicationHost& host, ui::runtime::UIHost& uiHost)
            : m_host(&host), m_uiHost(&uiHost)
        {
        }

        [[nodiscard]] const TypeInfo* PrimaryType() const override;

        [[nodiscard]] UniquePtr<EditorPage>
        CreatePage(EditorContext& context, draconic::content::Instance& instance) override;

    private:
        runtime::IApplicationHost* m_host;
        ui::runtime::UIHost* m_uiHost;
    };

    // Create a preset material instance in `group` (or Materials/ from the File menu).
    inline draconic::content::Instance*
    CreateMaterialInstance(EditorContext& context, draconic::content::Group* group, bool unlit)
    {
        if (context.Project() == nullptr)
        {
            return nullptr;
        }
        draconic::content::Group* target = group;
        if (target == nullptr)
        {
            draconic::content::Group* root = context.Project()->SourceDb().RootGroup();
            target = root->GetGroup(u8"Materials");
            if (target == nullptr)
            {
                target = root->CreateGroup(u8"Materials");
            }
        }
        if (target == nullptr)
        {
            return nullptr;
        }

        const String name = target->UniqueInstanceName(u8"Material");

        draconic::content::Instance* instance =
            target->CreateInstance(name.AsView(), materials::MaterialAsset::StaticType());
        if (instance == nullptr)
        {
            return nullptr;
        }

        RefPtr<materials::Material> built =
            unlit ? materials::CreateUnlit(name.AsView()) : materials::CreatePBR(name.AsView());
        materials::MaterialAsset asset;
        materials::MaterialImporter::Import(*built, Guid{}, asset);
        if (!instance->WriteObject(asset).IsOk())
        {
            return nullptr;
        }
        DRACONIC_LOG_INFO(u8"Editor", u8"created {} material '{}'", unlit ? u8"unlit" : u8"PBR",
                          instance->Path());
        context.RequestCook(false); // pickable as soon as the product lands
        return instance;
    }

    // Per-asset material-preview prefs: {assetGuid -> (shape, meshGuid)} - a section in the
    // per-project editor-settings store (rewritten whole; the page reads/writes its row).
    struct MaterialPreviewPref
    {
        Guid asset;
        u32 shape = 0;
        Guid mesh;

        void Serialize(ISerializer& ar)
        {
            ar.Key("asset");
            ar.GuidValue(asset);
            draconic::foundation::Serialize(ar, "shape", shape);
            ar.Key("mesh");
            ar.GuidValue(mesh);
        }
    };

    inline void Serialize(ISerializer& ar, MaterialPreviewPref& p)
    {
        ar.BeginObject();
        p.Serialize(ar);
        ar.EndObject();
    }

    class MaterialPreviewSettings final : public ISerializable
    {
        DRACONIC_OBJECT(MaterialPreviewSettings, ISerializable)
    public:
        Array<MaterialPreviewPref> prefs;

        void Serialize(ISerializer& ar) override
        {
            draconic::foundation::Serialize(ar, "prefs", prefs);
        }
    };

    inline void RegisterMaterialEditor(EditorContext& context, runtime::IApplicationHost& host,
                                       ui::runtime::UIHost& uiHost)
    {
        GlobalTypeRegistry().Register(materials::MaterialAsset::StaticType(), TypeDomain(u8"Editor"));
        RegisterSerializable<materials::MaterialAsset>();
        // The preview-prefs section (registered before the app loads the per-project store).
        GlobalTypeRegistry().Register(MaterialPreviewSettings::StaticType(), TypeDomain(u8"Editor"));
        RegisterSerializable<MaterialPreviewSettings>();

        context.Pages().Register(UniquePtr<IEditorPageFactory>(
            DefaultAllocator().New<MaterialEditorPageFactory>(host, uiHost), DefaultAllocator()));

        EditorContext::AssetCreator pbr;
        pbr.label = String(u8"PBR Material");
        pbr.category = String(u8"Materials");
        pbr.create = [](EditorContext& ctx, draconic::content::Group* group)
        { return CreateMaterialInstance(ctx, group, /*unlit*/ false); };
        context.RegisterCreator(Move(pbr));

        EditorContext::AssetCreator unlit;
        unlit.label = String(u8"Unlit Material");
        unlit.category = String(u8"Materials");
        unlit.create = [](EditorContext& ctx, draconic::content::Group* group)
        { return CreateMaterialInstance(ctx, group, /*unlit*/ true); };
        context.RegisterCreator(Move(unlit));
    }

    DRACONIC_DEFINE_OBJECT_VERSIONED(MaterialPreviewSettings, "draconic::editor", 1)
}
