// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Materials - :page partition.
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
// RegisterMaterialEditor is the module's RegisterEditor entry point: registers the
// MaterialAsset page factory and the "PBR Material" / "Unlit Material" creators (presets).

module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"
#include "Core/Reflection/Reflect.h"

export module editor.materials:page;

import foundation.core;
import foundation.vfs;
import foundation.settings;
import foundation.content;
import foundation.rhi;
import foundation.graphics;
import foundation.shell;
import foundation.runtime;
import foundation.runtime.client;
import foundation.scene;
import engine.scene;
import foundation.geometry;
import foundation.materials;
import foundation.materials.resource;
import materials.pipeline;
import foundation.texture.resource;
import foundation.resource;
import foundation.shaders;
import foundation.shaders.resource; // a material on a shader asset: its resource and property table
import foundation.render;
import engine.render;
import foundation.ui;
import foundation.ui.toolkit;
import foundation.ui.runtime;
import foundation.ui.viewport;
import foundation.vg.renderer;
import editor.core;
import editor.app;
import editor.preview; // PreviewViewport (shared viewport + preview scene + camera + render loop)

using namespace foundation::core;
namespace rhi = foundation::rhi;

export namespace editor
{
    namespace runtime = foundation::runtime;
    namespace ui = foundation::ui;
    namespace vg = foundation::vg;
    namespace scene = foundation::scene;
    namespace render = foundation::render;
    namespace materials = foundation::materials;

    // The row a material property gets on the page, by its declared type: a colour picker only
    // for a declared colour (which shows the sRGB value as entered), a picker plus an intensity
    // for a colour that may pass white, and plain numbers for any other Float4. Pure, so the
    // choice is testable without a live host.
    enum class MaterialPropertyRow : u8
    {
        None, // not shown (samplers, matrices)
        Number,
        Numbers2,
        Numbers3,
        Numbers4,
        Integer,
        Color,
        ColorWithIntensity,
        Texture,
    };

    [[nodiscard]] inline MaterialPropertyRow MaterialPropertyRowFor(materials::MaterialPropertyType type)
    {
        switch (type)
        {
        case materials::MaterialPropertyType::Float:
            return MaterialPropertyRow::Number;
        case materials::MaterialPropertyType::Float2:
            return MaterialPropertyRow::Numbers2;
        case materials::MaterialPropertyType::Float3:
            return MaterialPropertyRow::Numbers3;
        case materials::MaterialPropertyType::Float4:
            return MaterialPropertyRow::Numbers4;
        case materials::MaterialPropertyType::Int:
            return MaterialPropertyRow::Integer;
        case materials::MaterialPropertyType::Color:
            return MaterialPropertyRow::Color;
        case materials::MaterialPropertyType::ColorHdr:
            return MaterialPropertyRow::ColorWithIntensity;
        case materials::MaterialPropertyType::Texture2D:
        case materials::MaterialPropertyType::TextureCube:
            return MaterialPropertyRow::Texture;
        default:
            return MaterialPropertyRow::None;
        }
    }

    class MaterialEditorPage final : public app::UIEditorPage
    {
    public:
        MaterialEditorPage(EditorContext& context, runtime::IApplicationHost& host,
                           ui::runtime::UIHost& uiHost, foundation::content::Instance& instance);
        ~MaterialEditorPage();

        // === UIEditorPage ===

        [[nodiscard]] foundation::ui::View* ContentView() override { return m_content.Get(); }
        [[nodiscard]] StringView Title() const override { return m_title.AsView(); }

        void OnUpdate(runtime::IApplicationHost&, f32 dt) override;

        void OnRenderWindow(runtime::IApplicationHost&,
                            foundation::graphics::FrameContext& frame) override;

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
        void FramePreview(const foundation::geometry::StaticMesh* mesh);

        void RebuildGrid();

        // === the material's shader ===

        // The shader asset's cooked source (its property table), or null before it has cooked.
        [[nodiscard]] RefPtr<foundation::shaders::ShaderSource> ReadShader(const Guid& shader) const;
        // Moves the material onto a shader asset, or back to a built-in (nil), as one undoable
        // edit: re-laid by the new shader's table, every value that still fits kept.
        void SwitchShader(const Guid& shader);
        void SwitchToBuiltIn(bool unlit);
        // Binds the material's shader asset (its resource registers it for the preview) and
        // re-lays the material by its current table: on opening, and whenever it recooks.
        void BindShader();

        // A pipeline-state dropdown writing one of the source's u8 mode fields.
        void AddPipelineEnumRow(StringView label, Span<const StringView> items,
                                u8& (*field)(materials::MaterialSource&));

        // A texture-slot picker row (TextureAsset picker; [Clear] unbinds the slot).
        void AddTextureRow(const String& slot);

        [[nodiscard]] foundation::ui::UIContext* Context() const noexcept { return m_grid->Context; }

        [[nodiscard]] StringView AssetNameFor(const Guid& target);

        void AddEditor(foundation::ui::toolkit::PropertyEditor* editor, Function<void()> refresher);

        // Uniform blob access by property name (offset/size from the source's tables).
        void ReadUniform(StringView name, void* out, usize bytes) const;
        void WriteUniform(StringView name, const void* value, usize bytes);

        EditorContext* m_context;
        runtime::IApplicationHost* m_host;
        ui::runtime::UIHost* m_uiHost;
        String m_title;

        RefPtr<pipeline::MaterialAsset> m_asset;

        UniquePtr<PreviewViewport> m_preview; // shared viewport + preview scene + camera
        scene::EntityHandle m_sphere;         // the preview shape entity (in m_preview->Scene())
        RefPtr<foundation::geometry::StaticMesh> m_previewMesh;
        RefPtr<materials::Material> m_previewMaterial;
        u32 m_previewShape = 0; // index into the Shape enum row
        Guid m_previewMeshGuid; // nil = primitive shape
        Array<foundation::resource::Proxy<foundation::texture::Texture>> m_previewTextures;
        Array<rhi::TextureView*> m_previewTextureViews; // views captured into the material

        // The material's shader asset, when it is on one: its resource (the version says when it
        // recooked) and its property table (the rows' ranges).
        foundation::resource::Proxy<foundation::shaders::ShaderResource> m_shader;
        u64 m_shaderVersion = 0;
        Array<foundation::shaders::ShaderProperty> m_shaderTable;
        // The rows change with the layout (a shader switched, undone or recooked): rebuilt on the
        // next update, never inside the grid callback that caused it.
        bool m_gridDirty = false;

        RefPtr<foundation::ui::toolkit::PropertyGrid> m_grid;
        RefPtr<foundation::ui::View> m_content;
        RefPtr<app::PageToolbar> m_toolbar;
        Array<Function<void()>> m_refreshers;
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
        CreatePage(EditorContext& context, foundation::content::Instance& instance) override;

    private:
        runtime::IApplicationHost* m_host;
        ui::runtime::UIHost* m_uiHost;
    };

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
            foundation::core::Serialize(ar, "shape", shape);
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
        RTTI_OBJECT(MaterialPreviewSettings, ISerializable)
    public:
        Array<MaterialPreviewPref> prefs;

        void Serialize(ISerializer& ar) override
        {
            foundation::core::Serialize(ar, "prefs", prefs);
        }
    };

    inline void RegisterMaterialEditor(EditorContext& context, runtime::IApplicationHost& host,
                                       ui::runtime::UIHost& uiHost)
    {
        GlobalTypeRegistry().Register(pipeline::MaterialAsset::StaticType(), TypeDomain(u8"Editor"));
        RegisterSerializable<pipeline::MaterialAsset>();
        // The preview-prefs section (registered before the app loads the per-project store).
        GlobalTypeRegistry().Register(MaterialPreviewSettings::StaticType(), TypeDomain(u8"Editor"));
        RegisterSerializable<MaterialPreviewSettings>();

        context.Pages().Register(UniquePtr<IEditorPageFactory>(
            editor::EditorRootAllocator().New<MaterialEditorPageFactory>(host, uiHost), editor::EditorRootAllocator()));

    }

    RTTI_DEFINE_OBJECT_VERSIONED(MaterialPreviewSettings, "rtti::editor::editor", 1)
}
