// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Audio - the `:sound_cue_page` partition.
//
// SoundCueEditorPage: a sound cue on the GUI, built from its markup (Data/Editor/SoundCuePage.xml).
// The grid shows how the cue picks (at random without playing the same variant twice running, at
// random, or in order), its pitch and volume ranges, then its variants, each a clip (an asset slot)
// and a weight. It lists the filled variants plus one more to fill while there is room (eight at
// most); clearing a clip removes its variant, and the rest keep their order. Each edit is one undo
// step. The kit's transport auditions the cue as the game plays it: the runtime's resolver picks
// the variant (by its weights, no-repeat rule and order) and the pitch and volume within the
// ranges, and it plays through the runtime's audio engine; what was picked is shown beside the
// transport. A cue with no clip says so.

module;
#include "Core/Prelude.h"

export module editor.audio:sound_cue_page;

import foundation.core;
import foundation.content;
import foundation.runtime.client;
import foundation.audio;
import audio.pipeline;
import engine.audio;
import foundation.gui;
import foundation.gui.toolkit;
import editor.core;
import editor.app;
import :audition;

using namespace foundation::core;

export namespace editor
{
    namespace audio = foundation::audio;

    class SoundCueEditorPage final : public app::AssetObjectPage, public IPlaybackPage
    {
    public:
        static constexpr StringView kPath = u8"Editor/SoundCuePage.xml";

        SoundCueEditorPage(EditorContext& context, app::AssetPageServices services, foundation::content::Instance& instance);
        ~SoundCueEditorPage() override;

        void OnUpdate(foundation::runtime::IApplicationHost& host, f32 dt) override;
        void OnClose() override { StopAudition(); }

        // === IPlaybackPage ===
        [[nodiscard]] bool CanPlay() const override;
        [[nodiscard]] bool IsPlaying() const override { return m_voice && m_voice->IsPlaying(); }
        void Play() override;
        void Pause() override;
        void Stop() override { StopAudition(); }
        void Restart() override { Audition(); }

        // === For tests ===
        [[nodiscard]] foundation::gui::PropertyBag& CueRows() noexcept { return *m_cueRows; }
        [[nodiscard]] foundation::gui::PropertyBag& VariantRows() noexcept { return *m_variantRows; }
        /// Edits a row as the grid commits one (`rows` is one of the page's bags).
        void EditRow(foundation::gui::PropertyBag& rows, StringView path, const foundation::gui::Variant& value);
        [[nodiscard]] StringView State() const;
        /// How many variants hold a clip.
        [[nodiscard]] usize VariantCount() const;

    protected:
        void OnObjectChanged() override;

    private:
        [[nodiscard]] pipeline::SoundCueAsset* Cue() const { return ObjectAs<pipeline::SoundCueAsset>(); }
        /// Builds the rows from the cue: rebuilt when the number of variants changed, otherwise only
        /// their values are updated.
        void ShowRows();
        void BuildRows();
        void Committed(foundation::gui::PropertyBag& rows, StringView path, const foundation::gui::Variant& value);
        void ListEdited(const foundation::gui::ListEdit& edit);
        void Audition();
        void StopAudition();
        void ShowState(StringView state, StringView text);
        /// Variant `slot`'s clip, read for auditioning (cached by its asset's id).
        [[nodiscard]] RefPtr<audio::AudioClip> SlotClip(usize slot);

        UniquePtr<AuditionVoice> m_voice;
        UniquePtr<foundation::gui::PropertyBag> m_cueRows;
        UniquePtr<foundation::gui::PropertyBag> m_variantRows;
        Array<f32> m_pending; // weights of variants added but not yet given a clip (the cue holds no variant without a clip)
        HashMap<Guid, RefPtr<audio::AudioClip>> m_clips;
        Random m_random;
        i32 m_lastVariant = -1;
        u32 m_sequence = 0;
        String m_picked; // what the last play picked, shown beside the transport
        foundation::gui::Label* m_state = nullptr;
    };

    class SoundCuePageFactory final : public IEditorPageFactory
    {
    public:
        explicit SoundCuePageFactory(const app::AssetPageServicesSource& services) : m_services(&services) {}
        [[nodiscard]] const TypeInfo* PrimaryType() const override;
        [[nodiscard]] UniquePtr<EditorPage> CreatePage(EditorContext& context, foundation::content::Instance& instance) override;

    private:
        const app::AssetPageServicesSource* m_services = nullptr;
    };
}
