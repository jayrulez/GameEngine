// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::PropertyAnimation - the `:clip_document` partition.
//
// ClipDocument: the property animation clip the panel edits (Documentation/Specs/editor-gui.md, 5.4,
// layer 9, part 1). A clip is an asset of its own, not the scene it animates in, so it keeps its own
// history and its own unsaved mark (user 2026-10-27: the old panel put its edits on the scene page's
// stack, where they read as the scene's). Every edit is one step, the whole clip before and after
// (a clip is small); a gesture (a key dragged) edits live and is one step when it ends, or nothing
// when it is given up. The clip is unsaved while its history stands anywhere but where it was
// saved, so undoing back to the save clears the mark. Save writes the asset and asks for a cook.
// There is no clip until one is loaded: every edit has a home.

module;
#include "Core/Prelude.h"

export module editor.propertyanimation:clip_document;

import foundation.core;
import foundation.content;
import foundation.propertyanimation;
import editor.core;

using namespace foundation::core;

export namespace editor
{
    class ClipDocument
    {
    public:
        explicit ClipDocument(EditorContext& context);
        ClipDocument(const ClipDocument&) = delete;
        ClipDocument& operator=(const ClipDocument&) = delete;

        /// Reads the clip asset `clipId` (its history let go); false when there is no such clip.
        bool Load(const Guid& clipId);
        /// Writes the clip to its asset and asks for a cook; the clip is then saved.
        Status Save();
        /// No clip, its history let go.
        void Close();

        [[nodiscard]] bool HasClip() const noexcept { return !m_id.IsNil(); }
        [[nodiscard]] const Guid& ClipId() const noexcept { return m_id; }
        [[nodiscard]] StringView Name() const noexcept { return m_name.AsView(); }
        [[nodiscard]] bool IsDirty() const noexcept { return HasClip() && m_commands.UndoIndex() != m_savedAt; }
        [[nodiscard]] const foundation::propertyanimation::PropertyAnimationClip& Clip() const noexcept { return m_clip; }
        /// Its length: the one authored, or its last key's time, whichever is later (at least a second).
        [[nodiscard]] f32 Length() const noexcept;
        [[nodiscard]] EditorCommandStack& Commands() noexcept { return m_commands; }

        /// An edit made at once: `mutate` changes the clip, one step (its length grown to its keys).
        void Edit(StringView typeId, const Function<void(foundation::propertyanimation::PropertyAnimationClip&)>& mutate);
        /// A gesture: the clip edited live (GestureClip, then PreviewGesture to show it), one step
        /// when it commits; cancelled, the clip as it was.
        void BeginGesture();
        [[nodiscard]] foundation::propertyanimation::PropertyAnimationClip& GestureClip() noexcept { return m_clip; }
        void PreviewGesture() { Changed(); }
        void CommitGesture(StringView typeId);
        void CancelGesture();
        [[nodiscard]] bool InGesture() const noexcept { return m_inGesture; }

        /// Told after any change to the clip: an edit, a gesture's preview, an undo, a load.
        Function<void()> OnChanged;

    private:
        class Step;
        void Push(StringView typeId, foundation::propertyanimation::PropertyAnimationClip before);
        void Apply(const foundation::propertyanimation::PropertyAnimationClip& state);
        void Changed();

        EditorContext* m_context;
        foundation::propertyanimation::PropertyAnimationClip m_clip;
        Guid m_id;
        String m_name;
        EditorCommandStack m_commands;
        static constexpr i64 kNeverSaved = -2; // a history the save is no longer on
        i64 m_savedAt = -1;                    // the history's place when last saved (or loaded)
        foundation::propertyanimation::PropertyAnimationClip m_gestureBefore;
        bool m_inGesture = false;
    };
}
