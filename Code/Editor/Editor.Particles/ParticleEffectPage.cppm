// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Particles - the `:page` partition.
//
// ParticleEffectEditorPage edits a particle effect on the GUI, built from its markup
// (Data/Editor/ParticleEffectPage.xml). The effect's systems are shown as a tree, each with its
// emitter, initializers, behaviors and sub-emitters, and the live preview sits beside it. The grid
// shows the chosen system through reflection: its options (each shown only where its render mode
// uses it), its emitter, and its modules as lists. Adding to a list offers the types that derive
// from the list's base, so the page names no module type. A module chosen in the tree opens in place
// in the grid. Curves and gradients use the toolkit's editors over the particle curve types,
// registered with the rows. A system's sub-emitters are a list too: each one spawns into another
// system, chosen by name, when its particles are born or die, and is labelled by its trigger and
// target ("On death into Sparks"). The tree's bar adds a system and removes the chosen module, or
// the chosen system while another remains. Every edit is one undo step: grid edits go through the
// reflected rows, and adding or removing a system records the whole asset.
//
// The preview simulates the authored effect itself, so an edit shows at once. When the modules or
// systems change it restarts over them, and when what they draw with changes (a texture, a mesh, a
// material) it binds them again. Its transport plays, pauses and clears the preview and never writes
// the effect, so playing cannot change what is saved (setting every system emitting would also start
// a sub-emitter's child, which should emit only when spawned). The playback speed belongs to the
// page and is not saved.

module;
#include "Core/Prelude.h"

export module editor.particles:page;

import foundation.core;
import foundation.content;
import foundation.graphics;
import foundation.runtime;
import foundation.runtime.client;
import foundation.scene;
import foundation.particles;
import particles.pipeline;
import foundation.gui;
import foundation.gui.toolkit;
import editor.core;
import editor.app;

using namespace foundation::core;

export namespace editor
{
    namespace particles = foundation::particles;

    /// What a row of the page's tree stands for.
    enum class ParticleRowKind : u8
    {
        System,
        Emitter,
        Initializers,
        Behaviors,
        Initializer,
        Behavior,
        SubEmitters, // the system's sub-emitters
        SubEmitter
    };

    struct ParticleTreeRow
    {
        ParticleRowKind kind = ParticleRowKind::System;
        i32 system = -1;
        i32 module = -1; // an initializer's, a behavior's or a sub-emitter's index (-1: none)

        [[nodiscard]] bool operator==(const ParticleTreeRow&) const = default;
    };

    class ParticleEffectEditorPage final : public app::AssetObjectPage, public IPlaybackPage
    {
    public:
        static constexpr StringView kPath = u8"Editor/ParticleEffectPage.xml";
        /// The grid section that holds the chosen system's rows. Their paths are "system/" followed by
        /// the path within the system.
        static constexpr StringView kSystemSection = u8"system";
        /// A grid row's path within the system ("system/emitter.mode" gives "emitter.mode").
        [[nodiscard]] static StringView SystemPath(StringView gridPath);

        ParticleEffectEditorPage(EditorContext& context, app::AssetPageServices services, foundation::content::Instance& instance);
        ~ParticleEffectEditorPage() override;

        void OnUpdate(foundation::runtime::IApplicationHost& host, f32 dt) override;
        void OnRenderWindow(foundation::runtime::IApplicationHost&, foundation::graphics::FrameContext& frame) override;
        void OnClose() override;

        // === IPlaybackPage: the preview's transport ===
        [[nodiscard]] bool CanPlay() const override;
        [[nodiscard]] bool IsPlaying() const override { return !m_paused && !m_stopped; }
        void Play() override;
        void Pause() override;
        void Stop() override;
        void Restart() override;

        /// The system the grid shows, or -1 when the effect has none.
        [[nodiscard]] i32 ChosenSystem() const noexcept { return m_system; }
        /// Chooses a tree row: shows its system and opens the place the row names in the grid.
        void Choose(const ParticleTreeRow& row);
        /// Adds a system after the others (the default one a new asset starts with) and chooses it.
        void AddSystem();
        /// Removes the tree's chosen row: a module, or a system while another remains.
        bool RemoveChosen();
        /// Renames a system as one undo step, from its tree row renamed in place or from its Name row.
        bool RenameSystem(i32 system, StringView name);
        /// Starts editing the name in the system's tree row (F2, or Rename on its menu).
        bool BeginRename(i32 system);
        /// The module type names the Initializers or Behaviors folder can add.
        [[nodiscard]] Array<String> ModuleTypes(ParticleRowKind folder);
        /// Adds a module of `type` at the end of `folder`'s list and chooses it, as one undo step.
        bool AddModule(const ParticleTreeRow& folder, StringView type);
        /// Moves a module or a sub-emitter link `by` places in its list and keeps it chosen, as one
        /// undo step.
        bool MoveModule(const ParticleTreeRow& row, i32 by);
        /// Opens the tree row's menu at `at` over `owner`. A system offers Add System, Rename and
        /// Delete; a folder offers the module types it can add; a module offers Move Up, Move Down
        /// and Delete.
        void ShowRowMenu(const ParticleTreeRow& row, foundation::gui::Node& owner, Float2 at);
        [[nodiscard]] foundation::gui::Menu* RowMenu() const noexcept { return m_rowMenu && m_rowMenu->IsOpen() ? m_rowMenu.Get() : nullptr; }

        // === For tests ===
        /// The tree's rows, indented ("Sparks\n  Emitter\n  Initializers\n    Lifetime").
        [[nodiscard]] String Tree() const;
        [[nodiscard]] foundation::gui::IPropertySource& SystemRows() noexcept;
        [[nodiscard]] particles::ParticleEffect* Effect() const;
        /// How many times the preview started again over a changed set of modules or systems.
        [[nodiscard]] u32 PreviewRestarts() const noexcept { return m_restarts; }

    protected:
        void OnObjectChanged() override;

    private:
        void BindSystem();
        void ShowTree();
        void ShowFacts();
        /// Keeps the preview in step with the effect: restarts it when the modules or systems change,
        /// and binds what they draw with again when that changes.
        void FollowEffect();
        void Changed();

        void BuildPreviewScene();
        void RebindPreviewResources();
        void FramePreview();
        void DrawEmissionShape();
        void ShowTransport();
        [[nodiscard]] particles::ParticleEffectInstance* PreviewInstance() const;

        UniquePtr<foundation::gui::ReflectedPropertySource> m_values; // the chosen system's rows
        app::XmlPropertyTextFormat m_format;
        UniquePtr<foundation::gui::TreeModel> m_model;
        foundation::gui::TreeView* m_tree = nullptr;
        Array<ParticleTreeRow> m_rows; // the tree's rows, by node id
        i32 m_system = -1;
        ParticleTreeRow m_chosen;
        bool m_showing = false; // set while the page rebuilds the tree, so its selection is not the user's
        RefPtr<foundation::gui::Menu> m_rowMenu; // a tree row's context menu

        UniquePtr<app::PreviewPane> m_pane;
        foundation::scene::EntityHandle m_emitter;
        RefPtr<foundation::core::Object> m_previewResources; // the effect's references bound, for the preview's drawing
        Array<const void*> m_shape;        // the systems and modules the preview runs over
        Array<Guid> m_bindings;            // what they draw with, as last bound
        u32 m_restarts = 0;
        bool m_paused = false;
        bool m_stopped = false;
        foundation::gui::Label* m_count = nullptr;
    };

    class ParticleEffectPageFactory final : public IEditorPageFactory
    {
    public:
        explicit ParticleEffectPageFactory(const app::AssetPageServicesSource& services) : m_services(&services) {}
        [[nodiscard]] const TypeInfo* PrimaryType() const override;
        [[nodiscard]] UniquePtr<EditorPage> CreatePage(EditorContext& context, foundation::content::Instance& instance) override;

    private:
        const app::AssetPageServicesSource* m_services = nullptr;
    };

    /// Registers the particle effect page, which asks for its services when it is made.
    void RegisterParticleEditor(EditorContext& context, const app::AssetPageServicesSource& services);
}
