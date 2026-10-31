// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::PropertyAnimation - the `:clip_document` partition.
//
// ClipDocument is the property animation clip the panel edits. A clip is an asset of its own, not
// part of the scene it animates in, so it keeps its own history and its own unsaved mark rather
// than putting its edits on the scene page's undo stack. Every edit is one undo step that records
// the whole clip before and after (a clip is small). A gesture (dragging a key) edits live and
// becomes one step when it ends, or nothing when it is cancelled. The clip is unsaved whenever its
// history stands anywhere other than where it was saved, so undoing back to the save clears the
// mark. Save writes the asset and asks for a cook. There is no clip until one is loaded, so every
// edit belongs to a loaded asset.

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

        /// Reads the clip asset `clipId` and drops the current history; false when there is no such
        /// clip.
        bool Load(const Guid& clipId);
        /// Writes the clip to its asset and asks for a cook; the clip is then marked saved.
        Status Save();
        /// Closes the clip and drops its history.
        void Close();

        [[nodiscard]] bool HasClip() const noexcept { return !m_id.IsNil(); }
        [[nodiscard]] const Guid& ClipId() const noexcept { return m_id; }
        [[nodiscard]] StringView Name() const noexcept { return m_name.AsView(); }
        [[nodiscard]] bool IsDirty() const noexcept { return HasClip() && m_commands.UndoIndex() != m_savedAt; }
        [[nodiscard]] const foundation::propertyanimation::PropertyAnimationClip& Clip() const noexcept { return m_clip; }
        /// The clip's length: the authored length or its last key's time, whichever is later (at
        /// least a second).
        [[nodiscard]] f32 Length() const noexcept;
        [[nodiscard]] EditorCommandStack& Commands() noexcept { return m_commands; }

        /// Makes an immediate edit: `mutate` changes the clip as one undo step (the length grows to
        /// cover the keys).
        void Edit(StringView typeId, const Function<void(foundation::propertyanimation::PropertyAnimationClip&)>& mutate);
        /// Starts a gesture: the clip is edited live (GestureClip, then PreviewGesture to show it)
        /// and becomes one undo step when it commits; a cancelled gesture restores the clip.
        void BeginGesture();
        [[nodiscard]] foundation::propertyanimation::PropertyAnimationClip& GestureClip() noexcept { return m_clip; }
        void PreviewGesture() { Changed(); }
        void CommitGesture(StringView typeId);
        void CancelGesture();
        [[nodiscard]] bool InGesture() const noexcept { return m_inGesture; }

        /// Called after any change to the clip: an edit, a gesture's preview, an undo, a load.
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
        static constexpr i64 kNeverSaved = -2; // the history no longer contains the saved state
        i64 m_savedAt = -1;                    // the history's position when last saved (or loaded)
        foundation::propertyanimation::PropertyAnimationClip m_gestureBefore;
        bool m_inGesture = false;
    };
}
