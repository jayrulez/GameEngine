// Draconic::EditorGeneric - the `draconic.editor.generic` module.
//
// GenericAssetEditorPage (editor-pages-gap.md, the LAST page of the bespoke track): the fallback
// property-form editor for EVERY asset without a dedicated page. Registered against
// ISerializable's TypeInfo, so the page registry's nearest-base dispatch routes every bespoke
// page first (distance 0) and everything else lands here instead of the hard "No editor
// registered" failure.
//
// The assets are NOT reflected (their fields exist only in Serialize), so the form is
// SERIALIZE-DRIVEN: a scanning serializer runs the object's own Serialize in write mode and
// records every named value (scalars, strings, guids, blobs, array counts) as an ordinal field
// list; edits replay Serialize in read mode feeding the recorded values back with ONE field
// patched. The version scope is pushed manually with the type's CURRENT data-version chain, so
// `ar.Version() >= N` gated fields appear in the form (the bus-layout lesson). A patch that
// changes Serialize's control flow (a conditional branch) desyncs the replay fail-safe (the
// object keeps its live values from that point) and triggers a full re-scan + grid rebuild.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Log/Log.h"

export module draconic.editor.generic;

import draconic.foundation;
import draconic.content;
import draconic.ui;
import draconic.ui.toolkit;
import draconic.editor.core;
import draconic.editor.app;

using namespace draconic::foundation;

export namespace draconic::editor
{
    namespace ui = draconic::ui;

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

    // Run `object`'s Serialize (write mode) into a field list. The type's CURRENT data-version
    // chain is pushed so version-gated fields are included.
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
        GenericAssetEditorPage(EditorContext& context, draconic::content::Instance& instance);

        [[nodiscard]] StringView Title() const override { return m_title.AsView(); }
        [[nodiscard]] ui::View* ContentView() override { return m_content.Get(); }
        [[nodiscard]] Status Save() override;

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

        void BuildGrid();
        // Patch one field + undo; re-scan, and rebuild the grid only on a SHAPE change
        // (deferred through the mutation queue - never mid-scrub).
        void ApplyFieldEdit(usize index, const AssetFormField& newValue);

        [[nodiscard]] Array<byte> Snapshot() const;
        void ApplyBlob(const Array<byte>& blob);
        [[nodiscard]] ui::UIContext* Ctx() const;

        EditorContext* m_context = nullptr;
        String m_title;
        RefPtr<ISerializable> m_object;
        Array<AssetFormField> m_fields;

        RefPtr<ui::View> m_content;
        RefPtr<ui::Label> m_info;
        RefPtr<ui::toolkit::PropertyGrid> m_grid;
        Array<byte> m_undoBaseline;
    };

    class GenericAssetPageFactory final : public IEditorPageFactory
    {
    public:
        [[nodiscard]] const TypeInfo* PrimaryType() const override;
        [[nodiscard]] UniquePtr<EditorPage>
        CreatePage(EditorContext& context, draconic::content::Instance& instance) override;
    };

    // Registers the fallback factory. Bespoke pages always win (nearest-base dispatch); this
    // catches everything else, replacing the hard "No editor registered" failure.
    inline void RegisterGenericAssetEditor(EditorContext& context)
    {
        context.Pages().Register(UniquePtr<IEditorPageFactory>(
            DefaultAllocator().New<GenericAssetPageFactory>(), DefaultAllocator()));
    }
}
