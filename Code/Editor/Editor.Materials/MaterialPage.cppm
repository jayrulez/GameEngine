// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Materials - :page partition.
//
// MaterialEditorPage: edits a MaterialAsset. On the GUI from the preview pages' markup
// (Data/Editor/PreviewPage.gml; Documentation/Specs/editor-gui.md, 5.4, layer 4): the material on
// a preview shape lit by a default sun and procedural sky (its clock running, so a shader that
// moves with Time moves), beside its rows. The authored form IS the runtime source
// (MaterialSource), so Save writes the object back and asks for a cook; every live proxy bound to
// the cooked product then hot-swaps.
//
// The rows are the material's, not its reflection's (a source in parallel arrays: names, offsets,
// a uniform blob): under Material its shader (a shader asset, or a built-in, PBR or Unlit) and its
// pipeline state (blend, depth, cull); under Properties each value its shader declares, by its
// type (a number, with the shader's range; two, three or four; an integer; a colour; a colour
// that may pass white, with its intensity); under Textures each texture slot, a slot taking a
// texture or a render texture. A shader switched, undone or recooked lays the rows out again. What
// the preview shows the material on (a shape, or a mesh from the project) is on the preview's bar,
// not among the material's rows, and kept per material with the project.
//
// Edits are BLOB-SNAPSHOT commands: each captures the whole serialized MaterialSource before and
// after (it is tiny), exact undo whatever the layout, and consecutive edits of one row merge. Every
// apply rebuilds the preview's material, so a scrub reads live on the shape.

module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"
#include "Core/Reflection/Reflect.h"

export module editor.materials:page;

import foundation.core;
import foundation.content;
import foundation.graphics;
import foundation.runtime;
import foundation.runtime.client;
import foundation.scene;
import foundation.geometry;
import foundation.materials;
import foundation.materials.resource; // MaterialSource
import materials.pipeline;
import foundation.texture.resource;
import foundation.resource;
import foundation.shaders;
import foundation.shaders.resource;
import foundation.rhi;
import foundation.gui;
import foundation.gui.toolkit;
import editor.core;
import editor.app;
import editor.preview;

using namespace foundation::core;

export namespace editor
{
    namespace runtime = foundation::runtime;
    namespace scene = foundation::scene;
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

    class MaterialEditorPage final : public app::AssetObjectPage
    {
    public:
        static constexpr StringView kPath = u8"Editor/PreviewPage.gml";
        /// The shapes the preview shows the material on, in the bar's order.
        static constexpr StringView kShapes[] = {u8"Sphere", u8"Cube", u8"Plane", u8"Cylinder", u8"Torus", u8"Cone"};

        MaterialEditorPage(EditorContext& context, app::AssetPageServices services, foundation::content::Instance& instance);
        ~MaterialEditorPage() override;

        void OnUpdate(runtime::IApplicationHost& host, f32 dt) override;
        void OnRenderWindow(runtime::IApplicationHost&, foundation::graphics::FrameContext& frame) override;
        void OnClose() override;

        /// The rows' sections: the material's (shader, pipeline state), its properties, its textures.
        [[nodiscard]] foundation::gui::PropertyBag& MaterialRows() noexcept { return *m_materialRows; }
        [[nodiscard]] foundation::gui::PropertyBag& PropertyRows() noexcept { return *m_propertyRows; }
        [[nodiscard]] foundation::gui::PropertyBag& TextureRows() noexcept { return *m_textureRows; }
        /// Writes `value` to row `path` of `rows` as the grid commits an edit (one undo step).
        void EditRow(foundation::gui::PropertyBag& rows, StringView path, const foundation::gui::Variant& value);
        /// The rows shown again from the source (after an undo), or laid out again (a shader switched).
        void Refresh() override;

        /// The preview's shape (an index into kShapes) or mesh (nil: the shape), kept per material.
        void SetPreviewShape(u32 shape);
        void SetPreviewMesh(const Guid& mesh);
        [[nodiscard]] u32 PreviewShape() const noexcept { return m_previewShape; }
        [[nodiscard]] const Guid& PreviewMesh() const noexcept { return m_previewMeshGuid; }

        // === For tests ===
        [[nodiscard]] const materials::MaterialSource* Source() const;
        [[nodiscard]] app::PreviewPane* Pane() const noexcept { return m_pane.Get(); }

        // Deserialize a source blob into the live asset + refresh the preview (the command
        // stack's apply path - Execute and Undo both land here).
        void ApplySourceBlob(const Array<byte>& blob);
        [[nodiscard]] Array<byte> SnapshotSource() const;

    private:
        // Whole-source snapshot command: before/after blobs + a merge key (consecutive scrubs
        // of the same row collapse; the FIRST command keeps the original `before`).
        class EditMaterialCommand final : public IEditorCommand
        {
        public:
            EditMaterialCommand(MaterialEditorPage& page, StringView mergeKey, Array<byte> before, Array<byte> after)
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
                    return false;
                prev.m_after = Move(m_after);
                return true;
            }

        private:
            MaterialEditorPage* m_page;
            String m_mergeKey;
            Array<byte> m_before;
            Array<byte> m_after;
        };

        [[nodiscard]] pipeline::MaterialAsset* Asset() const { return ObjectAs<pipeline::MaterialAsset>(); }
        // Run one edit as an undoable command: snapshot -> mutate -> snapshot -> push.
        void ApplyEdit(StringView mergeKey, Function<void(materials::MaterialSource&)> mutate);
        /// A gesture given up: the source as it was before its previews.
        void CancelGesture();

        // The rows from the source: made again when its layout changed, else their values shown.
        void BuildRows();
        void ShowValues();
        void Committed(foundation::gui::PropertyBag& rows, StringView path, const foundation::gui::Variant& value);
        void ShowNotices();

        // Preview world: the material shape + the standard seeded sun + default environment.
        void BuildPreviewScene();
        // Build the runtime preview Material from the CURRENT source and swap it onto the shape.
        void RebuildPreviewMaterial();
        void LoadPreviewPref();
        void SavePreviewPref();
        void ApplyPreviewMesh();
        void FramePreview();
        void AddPreviewControls();

        // The shader asset's cooked source (its property table), or null before it has cooked.
        [[nodiscard]] RefPtr<foundation::shaders::ShaderSource> ReadShader(const Guid& shader) const;
        // Moves the material onto a shader asset, or back to a built-in (nil), as one undoable
        // edit: re-laid by the new shader's table, every value that still fits kept.
        void SwitchShader(const Guid& shader);
        void SwitchToBuiltIn(foundation::materials::BuiltinShading shading);
        // Binds the material's shader asset and re-lays the material by its current table.
        void BindShader();

        // Uniform blob access by property name (offset/size from the source's tables).
        void ReadUniform(StringView name, void* out, usize bytes) const;
        void WriteUniform(materials::MaterialSource& source, StringView name, const void* value, usize bytes);

        UniquePtr<app::PreviewPane> m_pane;
        RefPtr<foundation::gui::ComboBox> m_shapeChoice; // the preview bar's
        RefPtr<app::AssetPickerSlot> m_meshSlot;          // the preview bar's
        UniquePtr<foundation::gui::PropertyBag> m_materialRows;
        UniquePtr<foundation::gui::PropertyBag> m_propertyRows;
        UniquePtr<foundation::gui::PropertyBag> m_textureRows;
        bool m_rowsDirty = false; // the layout changed: built again on the next Refresh
        bool m_reshow = false;    // the values changed under the rows: shown again on the next Refresh
        // A gesture under way (a drag, a scrub, a picker open): its previews are written to the
        // source as they go, so the preview shows them, with no undo step; the source as it was
        // before them, what the gesture's one step starts from, or what a cancel puts back.
        bool m_previewing = false;
        bool m_inGesture = false;
        Array<byte> m_gestureBefore;

        scene::EntityHandle m_shape;
        RefPtr<foundation::geometry::StaticMesh> m_previewMesh;
        RefPtr<materials::Material> m_previewMaterial;
        u32 m_previewShape = 0;
        Guid m_previewMeshGuid; // nil = primitive shape
        Array<foundation::resource::Proxy<foundation::texture::Texture>> m_previewTextures;
        Array<foundation::rhi::TextureView*> m_previewTextureViews; // views captured into the material

        // The material's shader asset, when it is on one: its resource (the version says when it
        // recooked) and its property table (the rows' ranges).
        foundation::resource::Proxy<foundation::shaders::ShaderResource> m_shader;
        u64 m_shaderVersion = 0;
        Array<foundation::shaders::ShaderProperty> m_shaderTable;
    };

    class MaterialEditorPageFactory final : public IEditorPageFactory
    {
    public:
        explicit MaterialEditorPageFactory(const app::AssetPageServicesSource& services) : m_services(&services) {}
        [[nodiscard]] const TypeInfo* PrimaryType() const override;
        [[nodiscard]] UniquePtr<EditorPage> CreatePage(EditorContext& context, foundation::content::Instance& instance) override;

    private:
        const app::AssetPageServicesSource* m_services = nullptr;
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

    /// The material page (asking for its services when it is made), the asset's types and the
    /// preview prefs' section (registered before the app loads the per-project store).
    inline void RegisterMaterialEditor(EditorContext& context, const app::AssetPageServicesSource& services)
    {
        GlobalTypeRegistry().Register(pipeline::MaterialAsset::StaticType(), TypeDomain(u8"Editor"));
        RegisterSerializable<pipeline::MaterialAsset>();
        GlobalTypeRegistry().Register(MaterialPreviewSettings::StaticType(), TypeDomain(u8"Editor"));
        RegisterSerializable<MaterialPreviewSettings>();
        context.Pages().Register(UniquePtr<IEditorPageFactory>(
            editor::EditorRootAllocator().New<MaterialEditorPageFactory>(services), editor::EditorRootAllocator()));
    }

    RTTI_DEFINE_OBJECT_VERSIONED(MaterialPreviewSettings, "rtti::editor::editor", 1)
}
