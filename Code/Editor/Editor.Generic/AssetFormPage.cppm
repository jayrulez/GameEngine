// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Generic - the `editor.generic` module.
//
// GenericAssetEditorPage: the fallback
// property-form editor for EVERY asset without a dedicated page, on the GUI from its markup
// (Data/Editor/AssetForm.xml; Documentation/Specs/editor-gui.md, 5.4, layer 1). Registered against
// ISerializable's TypeInfo, so the page registry's nearest-base dispatch routes every bespoke
// page first (distance 0) and everything else lands here instead of the hard "No editor
// registered" failure.
//
// The assets are NOT reflected (their fields exist only in Serialize), so the form is
// SERIALIZE-DRIVEN: a scanning serializer runs the object's own Serialize in write mode and
// records every named value (scalars, strings, guids, blobs, array counts) as an ordinal field
// list; edits replay Serialize in read mode feeding the recorded values back with ONE field
// patched. The version scope is pushed manually with the type's CURRENT data-version chain
// (the same scope a real envelope carries). A patch that
// changes Serialize's control flow (a conditional branch) desyncs the replay fail-safe (the
// object keeps its live values from that point) and triggers a full re-scan + grid rebuild.
//
// On the grid the fields are a property bag: each value a row named by its key made readable (the
// key itself its tooltip), the fields inside an array a group under the outermost array's key,
// folded until opened (a model manifest's thousand node fields cost nothing until then), a guid one
// asset reference taking any asset, a blob its size, read only. An array's count is not a row: the
// replay cannot resize one. Every edit is one undo step, a snapshot of the whole object.

module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"

export module editor.generic;

import foundation.core;
import foundation.content;
import foundation.runtime.client; // IApplicationHost (OnUpdate)
import foundation.gui;
import foundation.gui.toolkit;
import editor.core;
import editor.app;

using namespace foundation::core;

export namespace editor
{
    namespace gui = foundation::gui;

    // ---- the serialize-driven form model -----------------------------------------------------

    enum class AssetFormFieldKind : u8
    {
        Scalar,    // bool / ints / floats (see scalarKind)
        Text,      // String
        Guid,      // Guid (canonical string + untyped asset picker in the UI)
        Blob,      // opaque bytes - shown read-only, replayed verbatim
        ArrayCount // BeginArray's count - hidden from the UI, replayed verbatim
    };

    struct AssetFormField
    {
        String label; // the last Key() before the value ("key[i]" inside unkeyed arrays)
        // The OUTERMOST enclosing array's key ("" for top-level fields). BuildGrid groups each
        // array's rows under a DEFAULT-COLLAPSED expander - bulk sections (a model manifest's
        // ~1300 decomposed node fields) cost no layout/draw until opened.
        String category;
        AssetFormFieldKind kind = AssetFormFieldKind::Scalar;
        ScalarKind scalarKind = ScalarKind::Float32; // when kind == Scalar / ArrayCount
        i64 intValue = 0;                            // bool + integer scalars, array counts
        f64 floatValue = 0.0;                        // float scalars
        String textValue;
        Guid guidValue{};
        Array<u8> blobValue;

        [[nodiscard]] bool Editable() const noexcept
        {
            return kind != AssetFormFieldKind::Blob && kind != AssetFormFieldKind::ArrayCount;
        }
    };

    // Run `object`'s Serialize (write mode) into a field list under the type's data-version
    // scope.
    [[nodiscard]] Status ScanAssetForm(ISerializable& object, Array<AssetFormField>& outFields);

    // Replay `fields` back through Serialize (read mode) with fields[index] replaced by
    // `newValue`. On a shape mismatch the replay goes inert from that point (the object keeps
    // its live values) - callers should re-scan afterwards regardless.
    [[nodiscard]] Status ApplyAssetFormField(ISerializable& object,
                                             const Array<AssetFormField>& fields, usize index,
                                             const AssetFormField& newValue);

    // True when two scans have the same SHAPE (count, kinds, labels) - a shape change after a
    // patch means a conditional Serialize branch flipped and the grid must rebuild.
    [[nodiscard]] bool AssetFormShapeEquals(const Array<AssetFormField>& a,
                                            const Array<AssetFormField>& b);

    // ---- the page ----------------------------------------------------------------------------

    class GenericAssetEditorPage final : public app::UIEditorPage
    {
    public:
        static constexpr StringView kPath = u8"Editor/AssetForm.xml";

        GenericAssetEditorPage(EditorContext& context, app::AssetPageServices services, foundation::content::Instance& instance);
        ~GenericAssetEditorPage() override;

        [[nodiscard]] StringView Title() const override { return m_title.AsView(); }
        [[nodiscard]] RefPtr<gui::Node> Content() override { return m_content; }
        [[nodiscard]] Status Save() override;
        void OnUpdate(foundation::runtime::IApplicationHost& host, f32 dt) override;

        /// The bar as the actions are over the page, and the rows built again after a change of
        /// shape (a branch of Serialize flipped) or shown again after an undo; once a frame.
        void Refresh();

        // === For tests ===
        [[nodiscard]] Span<const AssetFormField> Fields() const noexcept { return m_fields.AsSpan(); }
        /// The bag's path of field `index` (empty: it has no row).
        [[nodiscard]] StringView PathOf(usize index) const;
        [[nodiscard]] gui::PropertyBag* Bag() const noexcept { return m_bag.Get(); }
        [[nodiscard]] gui::PropertyGrid* Grid() const noexcept { return m_grid; }
        /// Writes `value` to the row at `path` as the grid commits an edit.
        void Edit(StringView path, const gui::Variant& value);
        [[nodiscard]] bool LoadFailed() const noexcept { return m_object.Get() == nullptr; }

    private:
        class EditGenericCommand final : public IEditorCommand
        {
        public:
            EditGenericCommand(GenericAssetEditorPage& page, StringView mergeKey,
                               Array<byte> before, Array<byte> after)
                : m_page(&page), m_mergeKey(mergeKey), m_before(Move(before)), m_after(Move(after))
            {
            }

            [[nodiscard]] bool Execute() override
            {
                m_page->ApplyBlob(m_after);
                return true;
            }
            void Undo() override { m_page->ApplyBlob(m_before); }
            [[nodiscard]] StringView TypeId() const override { return u8"edit_generic_asset"; }
            [[nodiscard]] bool MergeInto(IEditorCommand& previous) override
            {
                auto& prev = static_cast<EditGenericCommand&>(previous);
                if (prev.m_page != m_page || prev.m_mergeKey.AsView() != m_mergeKey.AsView())
                {
                    return false;
                }
                prev.m_after = Move(m_after);
                return true;
            }

        private:
            GenericAssetEditorPage* m_page;
            String m_mergeKey;
            Array<byte> m_before;
            Array<byte> m_after;
        };

        /// The bag made anew from the fields, given to the grid.
        void BuildBag();
        /// Each row's value from the fields, where it differs.
        void ShowValues();
        void Committed(const gui::PropertyBag::Commit& commit);
        // Patch one field + undo; re-scan, and rebuild the rows only on a SHAPE change (on the
        // next Refresh, never inside the grid's commit).
        void ApplyFieldEdit(usize index, const AssetFormField& newValue);

        [[nodiscard]] Array<byte> Snapshot() const;
        void ApplyBlob(const Array<byte>& blob);
        template <typename T>
        [[nodiscard]] T* Find(StringView id) const
        {
            return m_content ? app::FindById<T>(*m_content, id) : nullptr;
        }

        EditorContext* m_context = nullptr;
        app::AssetPageServices m_services;
        String m_title;
        RefPtr<ISerializable> m_object;
        Array<AssetFormField> m_fields;
        Array<String> m_paths; // by field: its row's path in the bag (empty: none)

        RefPtr<gui::Node> m_content;
        gui::PropertyGrid* m_grid = nullptr; // the markup's
        UniquePtr<gui::PropertyBag> m_bag;
        UniquePtr<app::PageActionBar> m_bar;
        Array<byte> m_undoBaseline;
        bool m_rebuild = false; // the shape changed: the bag is built again on Refresh
        bool m_reshow = false;  // the values changed under the rows (an undo): shown again on Refresh
    };

    class GenericAssetPageFactory final : public IEditorPageFactory
    {
    public:
        explicit GenericAssetPageFactory(const app::AssetPageServicesSource& services) : m_services(&services) {}
        [[nodiscard]] const TypeInfo* PrimaryType() const override;
        [[nodiscard]] UniquePtr<EditorPage>
        CreatePage(EditorContext& context, foundation::content::Instance& instance) override;

    private:
        const app::AssetPageServicesSource* m_services = nullptr;
    };

    // Registers the fallback factory. Bespoke pages always win (nearest-base dispatch); this
    // catches everything else, replacing the hard "No editor registered" failure. A page asks for
    // its services when it is made (the window exists by then).
    inline void RegisterGenericAssetEditor(EditorContext& context, const app::AssetPageServicesSource& services)
    {
        context.Pages().Register(UniquePtr<IEditorPageFactory>(
            editor::EditorRootAllocator().New<GenericAssetPageFactory>(services), editor::EditorRootAllocator()));
    }
}
