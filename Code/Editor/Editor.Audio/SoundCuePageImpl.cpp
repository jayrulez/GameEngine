// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Audio - the sound cue page's implementation (see SoundCuePage.cppm).

module;
#include "Core/Prelude.h"

module editor.audio;

import :sound_cue_page;
import :audition;

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

using namespace foundation::core;
namespace audio = foundation::audio;
namespace gui = foundation::gui;

namespace editor
{
    namespace
    {
        // A reference row's value is its asset's id as text; nil is the empty text.
        [[nodiscard]] gui::Variant GuidText(const Guid& id)
        {
            if (id.IsNil())
                return gui::Variant(StringView{});
            utf8char text[37];
            id.ToChars(text);
            return gui::Variant(String(StringView(text, 36)));
        }
        [[nodiscard]] Guid GuidOf(const gui::Variant& value)
        {
            Guid id;
            const StringView text = Trim(value.AsString());
            if (!text.IsEmpty())
                (void)Guid::TryParse(text, id);
            return id;
        }
        // The variant row of slot `slot` ("slot3"), and that slot back from a row's path.
        [[nodiscard]] String SlotPath(usize slot) { return Format(u8"slot{}", slot); }
        [[nodiscard]] i32 SlotOf(StringView path)
        {
            const StringView top = gui::ParentPropertyPath(path).IsEmpty() ? path : gui::ParentPropertyPath(path);
            if (!top.StartsWith(u8"slot"))
                return -1;
            const Optional<i64> slot = ParseInt(top.SubStr(4, top.Size() - 4));
            return slot.HasValue() && slot.Value() >= 0 && slot.Value() < static_cast<i64>(pipeline::kSoundCueSlotCount) ? static_cast<i32>(slot.Value()) : -1;
        }

        struct RangeRow
        {
            const char8_t* path;
            const char8_t* shown;
            f32 pipeline::SoundCueAsset::*field;
        };
        constexpr RangeRow kRanges[] = {
            {u8"pitchMin", u8"Pitch Min", &pipeline::SoundCueAsset::pitchMin},
            {u8"pitchMax", u8"Pitch Max", &pipeline::SoundCueAsset::pitchMax},
            {u8"volumeMin", u8"Volume Min", &pipeline::SoundCueAsset::volumeMin},
            {u8"volumeMax", u8"Volume Max", &pipeline::SoundCueAsset::volumeMax},
        };
    }

    SoundCueEditorPage::SoundCueEditorPage(EditorContext& context, app::AssetPageServices services, foundation::content::Instance& instance)
        : app::AssetObjectPage(context, Move(services), instance, pipeline::SoundCueAsset::StaticType(), kPath)
    {
        Provide<IPlaybackPage>(*this);
        HideAssetRows(); // the cue reflects nothing of its own: its rows are the page's
        m_voice = MakeUnique<AuditionVoice>(Allocator(), Services().host != nullptr ? Services().host->Ctx().GetSubsystem<engine::audio::AudioSubsystem>() : nullptr);
        m_state = Find<gui::Label>(u8"cue-state");
        BuildRows();
        BindAction(Find<gui::ButtonBase>(u8"cue-restart"), u8"playback.restart");
        BindAction(Find<gui::ButtonBase>(u8"cue-play"), u8"playback.play");
        BindAction(Find<gui::ButtonBase>(u8"cue-stop"), u8"playback.stop");
        ShowRows();
        ShowState(u8"stopped", u8"Stopped");
    }

    SoundCueEditorPage::~SoundCueEditorPage()
    {
        StopAudition();
    }

    StringView SoundCueEditorPage::State() const
    {
        return m_state != nullptr ? m_state->GetText() : StringView{};
    }

    usize SoundCueEditorPage::VariantCount() const
    {
        usize count = 0;
        if (const pipeline::SoundCueAsset* cue = Cue())
            for (const Guid& clip : cue->clipIds)
                count += clip.IsNil() ? 0u : 1u;
        return count;
    }

    void SoundCueEditorPage::ShowRows()
    {
        const pipeline::SoundCueAsset* cue = Cue();
        if (cue == nullptr)
            return;
        // The rows are made again when the variants changed in number (a bag holds what it was made
        // with), on the next Refresh: an edit lands from inside a bag's commit, which must outlive it.
        const usize count = VariantCount();
        if (count != m_rowsFor)
        {
            m_rowsDirty = true;
            return;
        }
        m_cueRows->SetValue(u8"mode", 0, gui::Variant(static_cast<i64>(cue->mode)));
        for (const RangeRow& range : kRanges)
            m_cueRows->SetValue(range.path, 0, gui::Variant(static_cast<f64>(cue->*range.field)));
        for (usize slot = 0; slot < pipeline::kSoundCueSlotCount; ++slot)
        {
            const String path = SlotPath(slot);
            m_variantRows->SetValue(gui::JoinPropertyPath(path.AsView(), u8"clip").AsView(), 0, GuidText(cue->clipIds[slot]));
            m_variantRows->SetValue(gui::JoinPropertyPath(path.AsView(), u8"weight").AsView(), 0, gui::Variant(static_cast<f64>(cue->weights[slot])));
        }
        SetNotice(u8"empty", count == 0 ? StringView(u8"This cue has no clip: pick one in its variants, or nothing plays.") : StringView{});
    }

    void SoundCueEditorPage::Refresh()
    {
        app::AssetObjectPage::Refresh();
        if (!m_rowsDirty)
            return;
        m_rowsDirty = false;
        BuildRows();
        ShowRows();
    }

    void SoundCueEditorPage::BuildRows()
    {
        const pipeline::SoundCueAsset* cue = Cue();
        if (cue == nullptr)
            return;
        m_rowsFor = VariantCount();
        ClearSections();
        m_cueRows = MakeUnique<gui::PropertyBag>(Allocator(), Allocator());
        m_variantRows = MakeUnique<gui::PropertyBag>(Allocator(), Allocator());
        {
            gui::PropertyInfo& mode = m_cueRows->Add(u8"mode", gui::PropertyKind::Enum);
            mode.DisplayName = String(u8"Picks");
            mode.Description = String(u8"How a play picks its variant: at random but never the one before, at random, or each in turn");
            mode.Choices.PushBack(gui::PropertyChoice{String(u8"At random, never twice running"), static_cast<i64>(audio::SoundCueMode::RandomNoRepeat)});
            mode.Choices.PushBack(gui::PropertyChoice{String(u8"At random"), static_cast<i64>(audio::SoundCueMode::Random)});
            mode.Choices.PushBack(gui::PropertyChoice{String(u8"In order"), static_cast<i64>(audio::SoundCueMode::Sequential)});
            for (const RangeRow& range : kRanges)
            {
                gui::PropertyInfo& info = m_cueRows->Add(range.path, gui::PropertyKind::Float);
                info.DisplayName = String(range.shown);
                info.HasRange = true;
                info.Min = 0.0;
                info.Max = 4.0;
                info.Step = 0.01;
                info.Hint = String(u8"slider");
            }
        }
        // A variant per clip, then one to fill while there is room.
        {
            usize shown = 0;
            bool open = false; // one empty slot offered to fill
            for (usize slot = 0; slot < pipeline::kSoundCueSlotCount; ++slot)
            {
                const bool filled = !cue->clipIds[slot].IsNil();
                if (!filled && open)
                    continue;
                open = open || !filled;
                const String path = SlotPath(slot);
                gui::PropertyInfo& group = m_variantRows->Add(path.AsView(), gui::PropertyKind::Group);
                group.DisplayName = filled ? Format(u8"Variant {}", ++shown) : String(u8"Add a Variant");
                gui::PropertyInfo& clip = m_variantRows->Add(gui::JoinPropertyPath(path.AsView(), u8"clip").AsView(), gui::PropertyKind::Reference);
                clip.DisplayName = String(u8"Clip");
                clip.ReferenceType = &audio::AudioClip::StaticType();
                if (filled)
                {
                    gui::PropertyInfo& weight = m_variantRows->Add(gui::JoinPropertyPath(path.AsView(), u8"weight").AsView(), gui::PropertyKind::Float);
                    weight.DisplayName = String(u8"Weight");
                    weight.Description = String(u8"How often it is picked against the others' weights");
                    weight.HasRange = true;
                    weight.Min = 0.0;
                    weight.Max = 100.0;
                    weight.Step = 0.1;
                }
            }
        }
        SoundCueEditorPage* self = this;
        gui::PropertyBag* const bags[] = {m_cueRows.Get(), m_variantRows.Get()};
        for (gui::PropertyBag* raw : bags)
        {
            gui::PropertyBag* rows = raw;
            rows->SetOnCommit([self, raw](const gui::PropertyBag::Commit& commit)
                              {
                                  if (!commit.After.IsEmpty())
                                      self->Committed(*raw, commit.Path.AsView(), commit.After[0]);
                              });
        }
        AddSection(u8"cue", u8"Cue", *m_cueRows);
        AddSection(u8"variants", u8"Variants", *m_variantRows);
    }

    void SoundCueEditorPage::OnObjectChanged()
    {
        ShowRows();
    }

    void SoundCueEditorPage::EditRow(gui::PropertyBag& rows, StringView path, const gui::Variant& value)
    {
        rows.SetValue(path, 0, value); // as the grid's commit leaves it
        Committed(rows, path, value);
    }

    void SoundCueEditorPage::Committed(gui::PropertyBag& rows, StringView path, const gui::Variant& value)
    {
        if (Cue() == nullptr)
            return;
        if (&rows == m_cueRows.Get())
        {
            if (path == u8"mode")
            {
                const u8 mode = static_cast<u8>(Clamp<i64>(value.AsInt(), 0, 2));
                EditWhole(u8"edit_soundcue", [mode](ISerializable& object) { static_cast<pipeline::SoundCueAsset&>(object).mode = mode; });
                return;
            }
            for (const RangeRow& range : kRanges)
                if (path == range.path)
                {
                    const f32 v = static_cast<f32>(value.AsFloat());
                    auto field = range.field;
                    EditWhole(u8"edit_soundcue", [field, v](ISerializable& object) { static_cast<pipeline::SoundCueAsset&>(object).*field = v; });
                }
            return;
        }
        const i32 slot = SlotOf(path);
        if (slot < 0)
            return;
        const usize s = static_cast<usize>(slot);
        if (path.EndsWith(u8".clip"))
        {
            // Picked, dropped or cleared: one write; cleared, the variant leaves the list.
            const Guid clip = GuidOf(value);
            EditWhole(u8"edit_soundcue", [s, clip](ISerializable& object)
                      {
                          auto& cue = static_cast<pipeline::SoundCueAsset&>(object);
                          cue.clipIds[s] = clip;
                          if (clip.IsNil())
                              cue.weights[s] = 1.0f; // a slot filled again starts even
                      });
        }
        else if (path.EndsWith(u8".weight"))
        {
            const f32 weight = static_cast<f32>(Max(0.0, value.AsFloat()));
            EditWhole(u8"edit_soundcue", [s, weight](ISerializable& object) { static_cast<pipeline::SoundCueAsset&>(object).weights[s] = weight; });
        }
    }

    bool SoundCueEditorPage::CanPlay() const
    {
        return VariantCount() > 0 && m_voice && m_voice->HasEngine();
    }

    void SoundCueEditorPage::ShowState(StringView state, StringView text)
    {
        ShowTransportState(m_state, state, text);
    }

    RefPtr<audio::AudioClip> SoundCueEditorPage::SlotClip(usize slot)
    {
        const pipeline::SoundCueAsset* cue = Cue();
        const Guid id = cue != nullptr ? cue->clipIds[slot] : Guid{};
        if (id.IsNil() || Context().Project() == nullptr)
            return {};
        if (RefPtr<audio::AudioClip>* kept = m_clips.Find(id))
            return *kept;
        foundation::content::Instance* instance = Context().Project()->SourceDb().GetInstance(id);
        RefPtr<ISerializable> object = instance != nullptr ? instance->ReadObject() : RefPtr<ISerializable>{};
        const auto* asset = Cast<pipeline::AudioClipAsset>(object.Get());
        RefPtr<audio::AudioClip> clip = asset != nullptr ? ReadAuditionClip(Allocator(), *Context().Project(), *asset) : RefPtr<audio::AudioClip>{};
        if (clip.Get() != nullptr)
            m_clips.InsertOrAssign(id, clip);
        return clip;
    }

    void SoundCueEditorPage::Audition()
    {
        StopAudition();
        const pipeline::SoundCueAsset* cue = Cue();
        if (cue == nullptr || !CanPlay())
            return;
        // As the game resolves it: the runtime's own resolver over the same weights and ranges.
        audio::SoundCue resolved;
        resolved.mode = static_cast<audio::SoundCueMode>(cue->mode);
        resolved.pitchMin = cue->pitchMin;
        resolved.pitchMax = cue->pitchMax;
        resolved.volumeMin = cue->volumeMin;
        resolved.volumeMax = cue->volumeMax;
        for (usize slot = 0; slot < pipeline::kSoundCueSlotCount; ++slot)
        {
            audio::SoundCueVariant variant;
            variant.clip = SlotClip(slot);
            variant.weight = cue->weights[slot];
            resolved.variants.PushBack(Move(variant));
        }
        const audio::SoundCuePick pick = audio::ResolveSoundCue(resolved, m_random, m_lastVariant, m_sequence);
        if (pick.variantIndex < 0)
        {
            ShowState(u8"faulted", u8"No variant could play: its clips are missing or did not decode");
            return;
        }
        m_lastVariant = pick.variantIndex;
        audio::AudioPlayParams params;
        params.pitch = pick.pitch;
        params.volume = pick.volume;
        if (!m_voice->Start(resolved.variants[static_cast<usize>(pick.variantIndex)].clip, params))
        {
            ShowState(u8"faulted", u8"No voice: the audio engine did not start one");
            return;
        }
        // Which variant, counted as the grid shows them.
        usize shown = 0;
        for (usize slot = 0; slot <= static_cast<usize>(pick.variantIndex); ++slot)
            shown += cue->clipIds[slot].IsNil() ? 0u : 1u;
        m_picked = Format(u8"Variant {}, pitch {}, volume {}", shown, FormatFixed(pick.pitch, 2), FormatFixed(pick.volume, 2));
        ShowState(u8"playing", m_picked.AsView());
    }

    void SoundCueEditorPage::Play()
    {
        if (!m_voice->IsActive())
        {
            Audition();
            return;
        }
        m_voice->SetPaused(false);
        ShowState(u8"playing", m_picked.AsView());
    }

    void SoundCueEditorPage::Pause()
    {
        if (!m_voice->IsPlaying())
            return;
        m_voice->SetPaused(true);
        ShowState(u8"paused", Format(u8"Paused: {}", m_picked).AsView());
    }

    void SoundCueEditorPage::StopAudition()
    {
        if (m_voice)
            m_voice->Stop();
        ShowState(u8"stopped", u8"Stopped");
    }

    void SoundCueEditorPage::OnUpdate(foundation::runtime::IApplicationHost& host, f32 dt)
    {
        app::AssetObjectPage::OnUpdate(host, dt); // the bar, the transport with it, each frame
        f32 cursor = 0.0f;
        if (m_voice->IsActive() && !m_voice->Poll(cursor))
            StopAudition(); // played out, or its voice taken
    }

    const TypeInfo* SoundCuePageFactory::PrimaryType() const
    {
        return &pipeline::SoundCueAsset::StaticType();
    }

    UniquePtr<EditorPage> SoundCuePageFactory::CreatePage(EditorContext& context, foundation::content::Instance& instance)
    {
        auto* page = editor::EditorRootAllocator().New<SoundCueEditorPage>(context, m_services->Make(), instance);
        return UniquePtr<EditorPage>(page, editor::EditorRootAllocator());
    }
}
