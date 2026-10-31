// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Materials - :page partition.
//
// MaterialEditorPage: edits a MaterialAsset, built from the preview pages' markup
// (Data/Editor/PreviewPage.xml). The material shows on a preview shape lit by a default sun and
// procedural sky, with its clock running so a shader that moves with Time moves, beside its rows.
// The authored form is the runtime source (MaterialSource), so Save writes the object back and
// requests a cook; every live proxy bound to the cooked product then hot-swaps.
//
// The page builds the rows itself rather than from reflection (the source is parallel arrays:
// names, offsets, a uniform blob). Under Material are its shader (a shader asset, or a built-in:
// PBR, Unlit or Phong) and its pipeline state (blend, depth, cull). Under Properties is each value
// its shader declares, by type: a number with the shader's range; two, three or four numbers; an
// integer; a colour; or a colour that may pass white, with its intensity. Under Textures is each
// texture slot, which takes a texture or a render texture. Then comes a section per effect group
// the material carries (Wind: its Strength, Speed and Height), with rows from the effect's table
// as a custom shader's are and values from the group (`Wind.Strength`); a material whose shader
// declares no effect has none. Switching the shader, an undo or a recook lays the rows out again.
// What the preview shows the material on (a shape, or a mesh from the project) is on the
// preview's bar, not among the material's rows, and is kept per material with the project.
//
// Edits are blob-snapshot commands: each captures the whole serialized MaterialSource before and
// after (it is tiny), which gives an exact undo whatever the layout, and consecutive edits of one
// row merge. Every apply rebuilds the preview's material, so a scrub shows live on the shape.

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
import foundation.shaders.system; // the effects' tables (their ranges; a group the material lacks)
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
        static constexpr StringView kPath = u8"Editor/PreviewPage.xml";
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
        /// An effect group's section (`Wind`), or null when the material carries no such group.
        [[nodiscard]] foundation::gui::PropertyBag* EffectRows(StringView effect) noexcept;
        /// Writes `value` to row `path` of `rows` as the grid commits an edit (one undo step).
        void EditRow(foundation::gui::PropertyBag& rows, StringView path, const foundation::gui::Variant& value);
        /// Shows the rows again from the source (after an undo), or lays them out again (after a
        /// shader switch).
        void Refresh() override;

        /// The preview's shape (an index into kShapes) or mesh (nil means the shape), kept per
        /// material.
        void SetPreviewShape(u32 shape);
        void SetPreviewMesh(const Guid& mesh);
        [[nodiscard]] u32 PreviewShape() const noexcept { return m_previewShape; }
        [[nodiscard]] const Guid& PreviewMesh() const noexcept { return m_previewMeshGuid; }

        // === For tests ===
        [[nodiscard]] const materials::MaterialSource* Source() const;
        [[nodiscard]] app::PreviewPane* Pane() const noexcept { return m_pane.Get(); }

        // Deserialize a source blob into the live asset and refresh the preview (the command
        // stack's apply path: Execute and Undo both land here).
        void ApplySourceBlob(const Array<byte>& blob);
        [[nodiscard]] Array<byte> SnapshotSource() const;

    private:
        // Whole-source snapshot command: before and after blobs and a merge key (consecutive
        // scrubs of the same row collapse; the first command keeps the original `before`).
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
        // Run one edit as an undoable command: snapshot, mutate, snapshot, push.
        void ApplyEdit(StringView mergeKey, Function<void(materials::MaterialSource&)> mutate);
        /// Abandons a gesture: puts back the source as it was before its previews.
        void CancelGesture();

        // Builds the rows from the source: rebuilt when its layout changed, else only their values
        // are shown again.
        void BuildRows();
        void ShowValues();
        void Committed(foundation::gui::PropertyBag& rows, StringView path, const foundation::gui::Variant& value);
        void ShowNotices();

        // Preview world: the material shape, the standard seeded sun and the default environment.
        void BuildPreviewScene();
        // Build the runtime preview Material from the current source and swap it onto the shape.
        void RebuildPreviewMaterial();
        void LoadPreviewPref();
        void SavePreviewPref();
        void ApplyPreviewMesh();
        void FramePreview();
        void AddPreviewControls();

        // The shader asset's cooked source (its property table), or null before it has cooked.
        [[nodiscard]] RefPtr<foundation::shaders::ShaderSource> ReadShader(const Guid& shader) const;
        // Moves the material onto a shader asset, or back to a built-in (nil), as one undoable
        // edit: the rows are laid out by the new shader's table, and every value that still fits
        // is kept.
        void SwitchShader(const Guid& shader);
        void SwitchToBuiltIn(foundation::materials::BuiltinShading shading);
        // Binds the material's shader asset and re-lays the material by its current table.
        void BindShader();

        // Lays the material's effect groups on the effects' tables by what its shader's vertex
        // stage declares, adding a missing group at the effect's defaults, when the page has the
        // shader system. `source` is the live source (for a layout, not an edit) or an edit's.
        void AdoptEffects(materials::MaterialSource& source, foundation::shaders::ShaderFlags vertexVariants);
        // The vertex variants the source's shader declares: a built-in's, or its shader asset's.
        [[nodiscard]] foundation::shaders::ShaderFlags VertexVariantsOf(const materials::MaterialSource& source) const;

        // Adds a value's row by its type, shows its value and writes its edits, the same way for
        // the shader's properties and an effect group's parameters. `path` is the row's in `rows`;
        // `name` addresses the value (`Metallic`, `Wind.Strength`); `declared` gives its range.
        void AddValueRow(foundation::gui::PropertyBag& rows, StringView path, MaterialPropertyRow row,
                         const foundation::shaders::ShaderProperty* declared);
        void ShowValue(foundation::gui::PropertyBag& rows, StringView path, StringView name, MaterialPropertyRow row);
        void CommitValue(StringView name, StringView part, MaterialPropertyRow row, const foundation::gui::Variant& value);

        // Uniform blob access by property name (offset and size from the source's tables); an
        // `Effect.Property` name reaches the effect group's values instead.
        void ReadUniform(StringView name, void* out, usize bytes) const;
        void WriteUniform(materials::MaterialSource& source, StringView name, const void* value, usize bytes) const;

        UniquePtr<app::PreviewPane> m_pane;
        RefPtr<foundation::gui::ComboBox> m_shapeChoice; // on the preview bar
        RefPtr<app::AssetPickerSlot> m_meshSlot;          // on the preview bar
        UniquePtr<foundation::gui::PropertyBag> m_materialRows;
        UniquePtr<foundation::gui::PropertyBag> m_propertyRows;
        UniquePtr<foundation::gui::PropertyBag> m_textureRows;
        // A section per effect group, in the source's order, and the group each shows.
        Array<UniquePtr<foundation::gui::PropertyBag>> m_effectRows;
        Array<String> m_effectRowNames;
        bool m_rowsDirty = false; // the layout changed: built again on the next Refresh
        bool m_reshow = false;    // the values changed under the rows: shown again on the next Refresh
        // A gesture under way (a drag, a scrub, an open picker) writes its previews to the source
        // as they go, so the preview shows them, with no undo step. The source as it was before
        // them is kept: the gesture's single undo step starts from it, and a cancel puts it back.
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
        foundation::shaders::ShaderFlags m_shaderVertexVariants = foundation::shaders::ShaderFlags::None;
        // The renderer's shader system (borrowed; null without a host): the effects' tables.
        foundation::shaders::ShaderSystem* m_effectSystem = nullptr;
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

    // Per-asset material-preview prefs (asset guid -> shape and mesh guid): a section in the
    // per-project editor-settings store, rewritten whole; the page reads and writes its own row.
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

    /// Registers the material page (which asks for its services when it is made), the asset's
    /// types and the preview prefs' section (registered before the app loads the per-project
    /// store).
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
