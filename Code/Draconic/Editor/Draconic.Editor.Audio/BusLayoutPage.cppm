// Draconic::EditorAudio - the `:bus_layout_page` partition.
//
// AudioBusLayoutPage (editor-pages-gap.md, bespoke pass #6): the mixer editor. A bus TREE on the
// left (the four fixed buses - Master with Effects/Music/UI under it - plus the used custom-bus
// slots parented by name) and a per-bus inspector on the right (volume/mute + the lowpass/
// highpass/delay/reverb effect fields; custom buses also edit name + parent and can be removed).
// "+ Add Bus" claims the first empty slot. Edits are coalesced whole-asset blob-snapshot undo
// commands; Save writes the asset + recooks (the cook validates parent cycles - the page's parent
// dropdown already refuses choices that would cycle, via the pure AudioBusWouldCycle helper).

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.editor.audio:bus_layout_page;

import draconic.foundation;
import draconic.content;
import draconic.runtime.client;
import draconic.audio;
import draconic.audio.editor;
import draconic.ui;
import draconic.ui.toolkit;
import draconic.editor.core;
import draconic.editor.app;

using namespace draconic::foundation;

export namespace draconic::editor
{
    namespace runtime = draconic::runtime;
    namespace ui = draconic::ui;
    namespace audio = draconic::audio;

    // True when re-parenting custom slot `slotIndex` under `newParent` would create a cycle
    // among the custom slots (fixed-bus parents can never cycle). Pure - the page's parent
    // dropdown filters with it and the tests pin it.
    [[nodiscard]] bool AudioBusWouldCycle(const audio::AudioBusLayoutAsset& asset, i32 slotIndex,
                                          StringView newParent);

    class BusLayoutTreeAdapter; // defined below

    class AudioBusLayoutEditorPage final : public app::UIEditorPage
    {
    public:
        AudioBusLayoutEditorPage(EditorContext& context, runtime::IApplicationHost& host,
                                 draconic::content::Instance& instance);

        [[nodiscard]] draconic::ui::View* ContentView() override { return m_content.Get(); }
        [[nodiscard]] StringView Title() const override { return m_title.AsView(); }
        [[nodiscard]] Status Save() override;

        void OnClose() override {}

        // Record a coalesced undo step for an in-place edit that already happened (merge by key).
        void CommitEdit(StringView mergeKey);

    private:
        friend class BusLayoutTreeAdapter;

        class EditBusLayoutCommand final : public IEditorCommand
        {
        public:
            EditBusLayoutCommand(AudioBusLayoutEditorPage& page, StringView mergeKey,
                                 Array<byte> before, Array<byte> after)
                : m_page(&page), m_mergeKey(mergeKey), m_before(Move(before)), m_after(Move(after))
            {
            }
            [[nodiscard]] bool Execute() override
            {
                m_page->ApplyAssetBlob(m_after);
                return true;
            }
            void Undo() override { m_page->ApplyAssetBlob(m_before); }
            [[nodiscard]] StringView TypeId() const override { return u8"edit_buslayout"; }
            [[nodiscard]] bool MergeInto(IEditorCommand& previous) override
            {
                auto& prev = static_cast<EditBusLayoutCommand&>(previous);
                if (prev.m_page != m_page || prev.m_mergeKey.AsView() != m_mergeKey.AsView())
                {
                    return false;
                }
                prev.m_after = Move(m_after);
                return true;
            }

        private:
            AudioBusLayoutEditorPage* m_page;
            String m_mergeKey;
            Array<byte> m_before;
            Array<byte> m_after;
        };

        // Node identity: 0..3 = Master/Effects/Music/UI, 4+i = custom slot i (used slots only
        // appear in the tree; the node table maps node -> slot index).
        struct BusNode
        {
            bool fixed = true;
            i32 fixedIndex = 0; // 0 Master, 1 Effects, 2 Music, 3 UI
            i32 slotIndex = -1; // custom slot
            i32 depth = 0;
            Array<i32> children; // node ids
        };

        void RebuildTree();
        void RebuildInspector();
        void QueueStructural(StringView undoKey, Function<void()> mutate);
        [[nodiscard]] audio::AudioBusLayoutAsset::Bus* SelectedBus();

        [[nodiscard]] Array<byte> SnapshotAsset() const;
        void ApplyAssetBlob(const Array<byte>& blob);

        [[nodiscard]] ui::UIContext* Ctx() const;

        EditorContext* m_context = nullptr;
        String m_title;

        RefPtr<audio::AudioBusLayoutAsset> m_asset;

        RefPtr<ui::toolkit::DraggableTreeView> m_tree;
        UniquePtr<BusLayoutTreeAdapter> m_adapter;
        RefPtr<ui::toolkit::PropertyGrid> m_grid;
        RefPtr<ui::Label> m_inspectorTitle;
        RefPtr<draconic::ui::View> m_content;

        Array<BusNode> m_nodes;
        Array<i32> m_roots;
        i32 m_selectedNode = 0; // Master by default
        Array<byte> m_undoBaseline;
    };

    // Read-only tree over the page's bus-node table (structure edits go through the inspector).
    class BusLayoutTreeAdapter final : public ui::toolkit::IReorderableTreeAdapter
    {
    public:
        explicit BusLayoutTreeAdapter(AudioBusLayoutEditorPage& owner) : m_owner(&owner) {}

        [[nodiscard]] i32 RootCount() const override;
        [[nodiscard]] i32 GetChildCount(i32 nodeId) const override;
        [[nodiscard]] i32 GetChildId(i32 parentId, i32 childIndex) const override;
        [[nodiscard]] i32 GetDepth(i32 nodeId) const override;
        [[nodiscard]] bool HasChildren(i32 nodeId) const override;
        [[nodiscard]] RefPtr<ui::View> CreateView(i32 viewType) override;
        void BindView(ui::View* view, i32 nodeId, i32 depth, bool isExpanded) override;

        [[nodiscard]] bool CanMove(i32, i32) override { return false; }
        void MoveItem(i32, i32) override {}

    private:
        AudioBusLayoutEditorPage* m_owner;
    };

    class AudioBusLayoutPageFactory final : public IEditorPageFactory
    {
    public:
        explicit AudioBusLayoutPageFactory(runtime::IApplicationHost& host) : m_host(&host) {}

        [[nodiscard]] const TypeInfo* PrimaryType() const override;
        [[nodiscard]] UniquePtr<EditorPage>
        CreatePage(EditorContext& context, draconic::content::Instance& instance) override;

    private:
        runtime::IApplicationHost* m_host;
    };

    // Registers the AudioBusLayout page factory (the New-Asset creator already lives in the
    // editor executable's creator set).
    void RegisterBusLayoutEditor(EditorContext& context, runtime::IApplicationHost& host);
}
