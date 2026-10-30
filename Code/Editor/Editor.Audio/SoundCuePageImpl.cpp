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
        m_voice = MakeUnique<AuditionVoice>(Allocator(), Services().host != nullptr ? Services().host->Subsystems().GetSubsystem<engine::audio::AudioSubsystem>() : nullptr);
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

    namespace
    {
        // The cue's variants packed in order (the list the grid shows): each a clip and its weight.
        struct CueVariant
        {
            Guid clip;
            f32 weight = 1.0f;
        };
        [[nodiscard]] Array<CueVariant> VariantsOf(const pipeline::SoundCueAsset& cue)
        {
            Array<CueVariant> variants;
            for (usize slot = 0; slot < pipeline::kSoundCueSlotCount; ++slot)
                if (!cue.clipIds[slot].IsNil())
                    variants.PushBack(CueVariant{cue.clipIds[slot], cue.weights[slot]});
            return variants;
        }
        // Writes `variants` back from the first slot on, the rest emptied (a gap between variants
        // closes: the resolver skips empty slots, so the cue plays the same).
        void SetVariants(pipeline::SoundCueAsset& cue, const Array<CueVariant>& variants)
        {
            for (usize slot = 0; slot < pipeline::kSoundCueSlotCount; ++slot)
            {
                const bool used = slot < variants.Size();
                cue.clipIds[slot] = used ? variants[slot].clip : Guid{};
                cue.weights[slot] = used ? variants[slot].weight : 1.0f;
            }
        }
        // The element a path names ("variants.2.weight": 2), or -1.
        [[nodiscard]] i32 ElementOf(StringView path)
        {
            const StringView element = gui::ParentPropertyPath(path);
            if (gui::ParentPropertyPath(element) != StringView(u8"variants"))
                return -1;
            const Optional<i64> index = ParseInt(element.SubStr(9, element.Size() - 9));
            return index.HasValue() && index.Value() >= 0 ? static_cast<i32>(index.Value()) : -1;
        }
    }

    void SoundCueEditorPage::ShowRows()
    {
        const pipeline::SoundCueAsset* cue = Cue();
        if (cue == nullptr)
            return;
        m_cueRows->SetValue(u8"mode", 0, gui::Variant(static_cast<i64>(cue->mode)));
        for (const RangeRow& range : kRanges)
            m_cueRows->SetValue(range.path, 0, gui::Variant(static_cast<f64>(cue->*range.field)));
        // The variants with a clip, then those added and given no clip yet (the page's own).
        const Array<CueVariant> variants = VariantsOf(*cue);
        const usize room = pipeline::kSoundCueSlotCount - variants.Size();
        while (m_pending.Size() > room)
            m_pending.PopBack();
        m_variantRows->ResizeList(u8"variants", 0, variants.Size() + m_pending.Size());
        for (usize i = 0; i < variants.Size() + m_pending.Size(); ++i)
        {
            const bool kept = i < variants.Size();
            const String element = Format(u8"variants.{}", i);
            m_variantRows->SetValue(gui::JoinPropertyPath(element.AsView(), u8"clip").AsView(), 0, GuidText(kept ? variants[i].clip : Guid{}));
            m_variantRows->SetValue(gui::JoinPropertyPath(element.AsView(), u8"weight").AsView(), 0,
                                    gui::Variant(static_cast<f64>(kept ? variants[i].weight : m_pending[i - variants.Size()])));
        }
        SetNotice(u8"empty", variants.IsEmpty() ? StringView(u8"This cue has no clip: add a variant and pick its clip, or nothing plays.") : StringView{});
    }

    void SoundCueEditorPage::BuildRows()
    {
        m_cueRows = MakeUnique<gui::PropertyBag>(Allocator(), Allocator());
        m_variantRows = MakeUnique<gui::PropertyBag>(Allocator(), Allocator());
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
        // A list as every page's: the count with Add (eight at most), a variant a clip and its weight.
        gui::PropertyInfo& list = m_variantRows->AddList(u8"variants", gui::PropertyKind::Group, gui::Variant{});
        list.DisplayName = String(u8"Variants");
        list.Description = String(u8"What a play picks from: each a clip, picked against the others by its weight");
        list.MaxCount = static_cast<u32>(pipeline::kSoundCueSlotCount);
        if (gui::PropertyInfo* clip = m_variantRows->AddElementField(u8"variants", u8"clip", gui::PropertyKind::Reference, gui::Variant(StringView{})))
        {
            clip->DisplayName = String(u8"Clip");
            clip->ReferenceType = &audio::AudioClip::StaticType();
        }
        if (gui::PropertyInfo* weight = m_variantRows->AddElementField(u8"variants", u8"weight", gui::PropertyKind::Float, gui::Variant(1.0)))
        {
            weight->DisplayName = String(u8"Weight");
            weight->Description = String(u8"How often it is picked against the others' weights");
            weight->HasRange = true;
            weight->Min = 0.0;
            weight->Max = 100.0;
            weight->Step = 0.1;
        }
        SoundCueEditorPage* self = this;
        gui::PropertyBag* const bags[] = {m_cueRows.Get(), m_variantRows.Get()};
        for (gui::PropertyBag* raw : bags)
        {
            gui::PropertyBag* rows = raw;
            rows->SetOnCommit([self, rows](const gui::PropertyBag::Commit& commit)
                              {
                                  if (!commit.After.IsEmpty())
                                      self->Committed(*rows, commit.Path.AsView(), commit.After[0]);
                              });
        }
        m_variantRows->SetOnListEdited([self](StringView, const gui::ListEdit& edit) { self->ListEdited(edit); });
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
        const i32 element = ElementOf(path);
        if (element < 0)
            return;
        const usize index = static_cast<usize>(element);
        const usize kept = VariantsOf(*Cue()).Size();
        if (path.EndsWith(u8".clip"))
        {
            const Guid clip = GuidOf(value);
            if (index < kept)
            {
                // Its clip picked again, or cleared: a cleared one leaves the cue, its row kept to fill.
                if (clip.IsNil())
                    m_pending.Insert(0, 1.0f);
                EditWhole(u8"edit_soundcue", [index, clip](ISerializable& object)
                          {
                              auto& cue = static_cast<pipeline::SoundCueAsset&>(object);
                              Array<CueVariant> variants = VariantsOf(cue);
                              if (clip.IsNil())
                                  variants.RemoveAt(index);
                              else
                                  variants[index].clip = clip;
                              SetVariants(cue, variants);
                          });
            }
            else if (!clip.IsNil() && index - kept < m_pending.Size())
            {
                // A variant added and now given its clip joins the cue, with the weight it was given.
                const f32 weight = m_pending[index - kept];
                m_pending.RemoveAt(index - kept);
                EditWhole(u8"edit_soundcue", [clip, weight](ISerializable& object)
                          {
                              auto& cue = static_cast<pipeline::SoundCueAsset&>(object);
                              Array<CueVariant> variants = VariantsOf(cue);
                              variants.PushBack(CueVariant{clip, weight});
                              SetVariants(cue, variants);
                          });
            }
            return;
        }
        if (path.EndsWith(u8".weight"))
        {
            const f32 weight = static_cast<f32>(Max(0.0, value.AsFloat()));
            if (index < kept)
                EditWhole(u8"edit_soundcue", [index, weight](ISerializable& object)
                          {
                              auto& cue = static_cast<pipeline::SoundCueAsset&>(object);
                              Array<CueVariant> variants = VariantsOf(cue);
                              variants[index].weight = weight;
                              SetVariants(cue, variants);
                          });
            else if (index - kept < m_pending.Size())
                m_pending[index - kept] = weight; // kept until it has a clip
        }
    }

    void SoundCueEditorPage::ListEdited(const gui::ListEdit& edit)
    {
        const pipeline::SoundCueAsset* cue = Cue();
        if (cue == nullptr)
            return;
        const usize kept = VariantsOf(*cue).Size();
        switch (edit.Kind)
        {
        case gui::ListEdit::Op::Insert:
            // Added at the end with no clip yet: the page's until a clip is picked (no step: the cue
            // has not changed).
            m_pending.PushBack(1.0f);
            break;
        case gui::ListEdit::Op::Remove:
            if (edit.Index < kept)
            {
                const usize index = edit.Index;
                EditWhole(u8"edit_soundcue", [index](ISerializable& object)
                          {
                              auto& asset = static_cast<pipeline::SoundCueAsset&>(object);
                              Array<CueVariant> variants = VariantsOf(asset);
                              variants.RemoveAt(index);
                              SetVariants(asset, variants);
                          });
            }
            else if (edit.Index - kept < m_pending.Size())
                m_pending.RemoveAt(edit.Index - kept);
            break;
        case gui::ListEdit::Op::Move:
            if (edit.Index < kept && edit.To < kept)
            {
                const usize from = edit.Index;
                const usize to = edit.To;
                EditWhole(u8"edit_soundcue", [from, to](ISerializable& object)
                          {
                              auto& asset = static_cast<pipeline::SoundCueAsset&>(object);
                              Array<CueVariant> variants = VariantsOf(asset);
                              const CueVariant moved = variants[from];
                              variants.RemoveAt(from);
                              variants.Insert(to, moved);
                              SetVariants(asset, variants);
                          });
            }
            break;
        }
        ShowRows(); // the list as the cue and the page hold it (a move past those with a clip goes back)
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
