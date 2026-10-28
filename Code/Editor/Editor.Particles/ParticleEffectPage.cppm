// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Particles - the `:page` partition.
//
// ParticleEffectEditorPage: a particle effect on the GUI from its markup
// (Data/Editor/ParticleEffectPage.gml; Documentation/Specs/editor-gui.md, 5.4, layer 8). The effect's
// systems a tree (each its emitter, initializers, behaviors and sub-emitters), the live preview beside it,
// and the grid showing the chosen system from reflection (the particle types are reflected): its
// options, each shown where its render mode uses it, its emitter, and its modules as the grid's lists,
// an add choosing the module's type among those deriving from the list's base, so the page names no
// module type. A module chosen in the tree opens in place there. Curves and gradients are the
// toolkit's editors over the particle curve types, registered with the rows. A system's
// sub-emitters (the old page had none) are a list of its rows too: each, on its particles' births
// or deaths, spawns into another system chosen by name, named in a line by what sets it off and
// where it spawns ("On death into Sparks"). The tree's bar adds a
// system and removes the chosen system (while another remains) or module. Every edit is one undo
// step: the grid's through the reflected rows, a system added or removed the whole asset.
//
// The preview simulates the authored effect itself, so an edit shows at once; when the modules or the
// systems change it starts again over them, and when what they draw with changes (a texture, a mesh,
// a material) it binds them again. Its transport plays, pauses and clears it and never writes the
// effect: the old page's Stop and Play set every system emitting (a sub-emitter's child too, which
// emits only when spawned) and so changed what was saved. Its speed is the page's alone.

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

    /// A row of the page's tree: what it stands for.
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
        static constexpr StringView kPath = u8"Editor/ParticleEffectPage.gml";
        /// The grid's section the chosen system's rows are in: its paths are "system/" and the system's own.
        static constexpr StringView kSystemSection = u8"system";
        /// The system's own path of a grid row's ("system/emitter.mode" is "emitter.mode").
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

        /// The system the grid shows (-1: none, an empty effect).
        [[nodiscard]] i32 ChosenSystem() const noexcept { return m_system; }
        /// What the tree has chosen: its system shown, and the place it names opened in the grid.
        void Choose(const ParticleTreeRow& row);
        /// A system added after the others (the new asset's default one), chosen.
        void AddSystem();
        /// The tree's chosen row taken out: a module, or a system while another remains.
        bool RemoveChosen();
        /// A system's name, one undo step (its tree row renamed in place, or its Name row).
        bool RenameSystem(i32 system, StringView name);
        /// Its tree row's name to type (F2; Rename on its menu).
        bool BeginRename(i32 system);
        /// What the Initializers' or Behaviors' folder adds, by type name.
        [[nodiscard]] Array<String> ModuleTypes(ParticleRowKind folder);
        /// A module of `type` added at the end of `folder`'s list, chosen; one step.
        bool AddModule(const ParticleTreeRow& folder, StringView type);
        /// A module, or a sub-emitter link, moved `by` places in its list, still chosen; one step.
        bool MoveModule(const ParticleTreeRow& row, i32 by);
        /// The tree row's menu at `at` over `owner`: a system's Add System, Rename and Delete; a
        /// folder's modules to add by type; a module's Move Up, Move Down and Delete.
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
        /// The preview follows the effect: started again when its modules or systems changed, what
        /// they draw with bound again when that changed.
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
        Array<ParticleTreeRow> m_rows; // the tree's, by node id
        i32 m_system = -1;
        ParticleTreeRow m_chosen;
        bool m_showing = false; // choosing from code: not the user's
        RefPtr<foundation::gui::Menu> m_rowMenu; // a tree row's

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

    /// The particle effect page (asking for its services when it is made).
    void RegisterParticleEditor(EditorContext& context, const app::AssetPageServicesSource& services);
}
