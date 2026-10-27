// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::PropertyAnimation - the `:clip_document` partition (implementation).

module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"

module editor.propertyanimation;

import foundation.core;
import foundation.content;
import foundation.propertyanimation;
import foundation.propertyanimation.resource;
import propertyanimation.pipeline;
import editor.core;

using namespace foundation::core;
namespace propanim = foundation::propertyanimation;

namespace editor
{
    // One step: the whole clip before and after.
    class ClipDocument::Step final : public IEditorCommand
    {
    public:
        Step(ClipDocument& document, StringView typeId, propanim::PropertyAnimationClip before, propanim::PropertyAnimationClip after)
            : m_document(&document), m_typeId(typeId), m_before(Move(before)), m_after(Move(after))
        {
        }
        [[nodiscard]] bool Execute() override
        {
            m_document->Apply(m_after);
            return true;
        }
        void Undo() override { m_document->Apply(m_before); }
        [[nodiscard]] StringView TypeId() const override { return m_typeId.AsView(); }

    private:
        ClipDocument* m_document;
        String m_typeId;
        propanim::PropertyAnimationClip m_before;
        propanim::PropertyAnimationClip m_after;
    };

    ClipDocument::ClipDocument(EditorContext& context) : m_context(&context)
    {
        // Told once the history has moved (a step, an undo, a redo), so the unsaved mark it shows
        // is the history's new place, not the one it is leaving.
        ClipDocument* self = this;
        m_commands.OnChanged = [self]() { self->Changed(); };
    }

    bool ClipDocument::Load(const Guid& clipId)
    {
        foundation::content::Instance* instance = m_context->Project() != nullptr ? m_context->Project()->SourceDb().GetInstance(clipId) : nullptr;
        RefPtr<ISerializable> object = instance != nullptr ? instance->ReadObject() : RefPtr<ISerializable>{};
        auto* asset = Cast<pipeline::PropertyAnimationClipAsset>(object.Get());
        if (asset == nullptr)
            return false;
        CancelGesture();
        m_clip = propanim::PropertyAnimationClip{};
        asset->source.FillClip(m_clip);
        m_id = clipId;
        m_name = String(instance->Name());
        m_commands.Clear();
        m_savedAt = m_commands.UndoIndex();
        Changed();
        return true;
    }

    Status ClipDocument::Save()
    {
        foundation::content::Instance* instance = HasClip() && m_context->Project() != nullptr ? m_context->Project()->SourceDb().GetInstance(m_id) : nullptr;
        if (instance == nullptr)
            return Status{ErrorCode::NotFound};
        pipeline::PropertyAnimationClipAsset asset;
        propanim::PropertyAnimationClipSource::FromClip(m_clip, asset.source);
        const Status written = instance->WriteObject(asset);
        if (!written.IsOk())
        {
            LOG_WARNING(u8"Editor", u8"writing the clip '{}' failed", m_name);
            return written;
        }
        m_savedAt = m_commands.UndoIndex();
        m_context->RequestCook(false);
        Changed(); // the unsaved mark
        return written;
    }

    void ClipDocument::Close()
    {
        CancelGesture();
        m_clip = propanim::PropertyAnimationClip{};
        m_id = Guid{};
        m_name = String{};
        m_commands.Clear();
        m_savedAt = m_commands.UndoIndex();
        Changed();
    }

    f32 ClipDocument::Length() const noexcept
    {
        return Max(Max(m_clip.duration, m_clip.ComputeDuration()), 1.0f);
    }

    void ClipDocument::Edit(StringView typeId, const Function<void(propanim::PropertyAnimationClip&)>& mutate)
    {
        if (!HasClip() || m_inGesture)
            return;
        propanim::PropertyAnimationClip before = m_clip;
        mutate(m_clip);
        Push(typeId, Move(before));
    }

    void ClipDocument::BeginGesture()
    {
        if (!HasClip())
            return;
        if (m_inGesture)
            CancelGesture();
        m_gestureBefore = m_clip;
        m_inGesture = true;
    }

    void ClipDocument::CommitGesture(StringView typeId)
    {
        if (!m_inGesture)
            return;
        m_inGesture = false;
        Push(typeId, Move(m_gestureBefore));
    }

    void ClipDocument::CancelGesture()
    {
        if (!m_inGesture)
            return;
        m_inGesture = false;
        m_clip = Move(m_gestureBefore);
        Changed();
    }

    void ClipDocument::Push(StringView typeId, propanim::PropertyAnimationClip before)
    {
        // Its length never cuts a key off.
        m_clip.duration = Max(m_clip.duration, m_clip.ComputeDuration());
        propanim::PropertyAnimationClip after = m_clip;
        // A step taken below the save leaves the save off the history: no place is saved now.
        if (m_savedAt > m_commands.UndoIndex())
            m_savedAt = kNeverSaved;
        (void)m_commands.Execute(UniquePtr<IEditorCommand>(EditorRootAllocator().New<Step>(*this, typeId, Move(before), Move(after)), EditorRootAllocator()));
    }

    void ClipDocument::Apply(const propanim::PropertyAnimationClip& state)
    {
        m_clip = state; // the history tells, once it has moved
    }

    void ClipDocument::Changed()
    {
        if (OnChanged)
            OnChanged();
    }
}
