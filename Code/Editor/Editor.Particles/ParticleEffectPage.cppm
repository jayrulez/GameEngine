// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Particles - the `:page` partition.
//
// ParticleEffectEditorPage: a particle effect on the GUI from its markup
// (Data/Editor/ParticleEffectPage.gml; Documentation/Specs/editor-gui.md, 5.4, layer 8). The effect's
// systems a tree (each its emitter, its initializers and its behaviors), the live preview beside it,
// and the grid showing the chosen system from reflection (the particle types are reflected): its
// options, each shown where its render mode uses it, its emitter, and its modules as the grid's lists,
// an add choosing the module's type among those deriving from the list's base, so the page names no
// module type. A module chosen in the tree opens in place there. Curves and gradients are the
// toolkit's editors over the particle curve types, registered with the rows. After the systems the
// effect's sub-emitter links (the old page had none), the tree's Sub-emitters row showing them in
// the grid in the system's place, as they are the effect's and no system's: each spawns into the
// system it names (a choice of the systems by name) on every other system's births or deaths,
// named in a line by what sets it off and where it spawns. A system they spawn into says so. The tree's bar adds a
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
        SubEmitters // the effect's links, after the systems
    };

    struct ParticleTreeRow
    {
        ParticleRowKind kind = ParticleRowKind::System;
        i32 system = -1;
        i32 module = -1; // an initializer's or a behavior's index (-1: not a module)

        [[nodiscard]] bool operator==(const ParticleTreeRow&) const = default;
    };

    class ParticleEffectEditorPage final : public app::AssetObjectPage, public IPlaybackPage
    {
    public:
        static constexpr StringView kPath = u8"Editor/ParticleEffectPage.gml";
        /// The grid's section the chosen system's rows are in: its paths are "system/" and the system's own.
        static constexpr StringView kSystemSection = u8"system";
        /// The grid's section the effect's sub-emitter links are in (paths "effect/subEmitterLinks...").
        static constexpr StringView kEffectSection = u8"effect";
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

        // === For tests ===
        /// The tree's rows, indented ("Sparks\n  Emitter\n  Initializers\n    Lifetime").
        [[nodiscard]] String Tree() const;
        [[nodiscard]] foundation::gui::IPropertySource& SystemRows() noexcept;
        /// The effect's rows: its sub-emitter links.
        [[nodiscard]] foundation::gui::IPropertySource& EffectRows() noexcept;
        [[nodiscard]] particles::ParticleEffect* Effect() const;
        /// How many times the preview started again over a changed set of modules or systems.
        [[nodiscard]] u32 PreviewRestarts() const noexcept { return m_restarts; }

    protected:
        void OnObjectChanged() override;

    private:
        void BindSystem();
        /// The grid's section for what is chosen: the system's rows, or the effect's links.
        void ShowSections();
        void ShowSpawnedBy();
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

        UniquePtr<foundation::gui::ReflectedPropertySource> m_values;  // the chosen system's rows
        UniquePtr<foundation::gui::ReflectedPropertySource> m_effectRows; // the effect's links
        app::XmlPropertyTextFormat m_format;
        UniquePtr<foundation::gui::TreeModel> m_model;
        foundation::gui::TreeView* m_tree = nullptr;
        Array<ParticleTreeRow> m_rows; // the tree's, by node id
        i32 m_system = -1;
        ParticleTreeRow m_chosen;
        bool m_showing = false; // choosing from code: not the user's
        i32 m_sectionsShown = -1; // 0 the system's, 1 the links (-1: none yet)

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
