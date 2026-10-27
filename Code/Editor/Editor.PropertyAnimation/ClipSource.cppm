// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::PropertyAnimation - the `:clip_source` partition.
//
// ClipSource: a clip document as the toolkit's dopesheet and curve view read it (Documentation/
// Specs/editor-gui.md, 5.4, layer 9, part 1). Its channels are the clip's: a scalar track's each of
// its curves ("Light.intensity", "Transform.position.x", "Light.color.r"), a rotation track one
// channel of its keys ("Transform.rotation"), so the dopesheet folds them by component, property
// and channel. Its curves are the scalar channels of the tracks the panel shows (a rotation's keys
// are slerped, not a curve). Every edit goes through the document: a drag is a gesture, one step
// when it ends; a deletion or an insertion one step each. A key inserted takes the clip's value
// there, or, where the channel has no keys yet, the value the application gives (the bound
// entity's), so a first key holds what the scene shows.

module;
#include "Core/Prelude.h"

export module editor.propertyanimation:clip_source;

import foundation.core;
import foundation.propertyanimation;
import foundation.gui;
import foundation.gui.toolkit;
import :clip_document;

using namespace foundation::core;

export namespace editor
{
    class ClipSource final : public foundation::gui::ITimelineSource, public foundation::gui::ICurveSource
    {
    public:
        /// A channel: its track and, for a scalar track, which of its curves (-1: a rotation's keys).
        struct Channel
        {
            usize Track = 0;
            i32 Curve = -1;
        };

        explicit ClipSource(ClipDocument& document);
        ~ClipSource() override;
        ClipSource(const ClipSource&) = delete;
        ClipSource& operator=(const ClipSource&) = delete;

        /// The tracks whose curves the curve view shows (the chosen ones); empty: none.
        void ShowTracks(Span<const usize> tracks);
        [[nodiscard]] Span<const usize> ShownTracks() const noexcept { return m_shown.AsSpan(); }
        /// The application's value of a track's target now (the bound entity's), for a first key;
        /// unset or empty: the type's zero.
        Function<Variant(const foundation::propertyanimation::PropertyTrack&)> CurrentValue;

        /// The document changed (its owner calls this from the document's OnChanged): the channels
        /// and curves read again, the views told.
        void Refresh();

        [[nodiscard]] Span<const Channel> Channels() const noexcept { return m_channels.AsSpan(); }
        /// A track's channels' path ("Light.color"), and a channel's ("Light.color.r").
        [[nodiscard]] static String TrackPath(const foundation::propertyanimation::PropertyTrack& track);

        // === ITimelineSource ===
        [[nodiscard]] usize ChannelCount() const override { return m_channels.Size(); }
        [[nodiscard]] String GetChannelPath(usize channel) const override;
        void GetKeyTimes(usize channel, Array<f32>& out) const override;
        void BeginKeysMove() override;
        void PreviewKeysMove(Span<const foundation::gui::TimelineKeyRef> keys, f32 delta, Array<foundation::gui::TimelineKeyRef>& moved) override;
        void CommitKeysMove() override;
        void CancelKeysMove() override;
        bool DeleteKeys(Span<const foundation::gui::TimelineKeyRef> keys) override;
        bool InsertKey(usize channel, f32 time, foundation::gui::TimelineKeyRef& made) override;

        // === ICurveSource ===
        [[nodiscard]] usize CurveCount() const override { return m_curves.Size(); }
        [[nodiscard]] foundation::gui::CurveInfo GetCurveInfo(usize curve) const override;
        [[nodiscard]] Span<const CurveKey> GetKeys(usize curve) const override;
        void BeginKeysEdit() override;
        void PreviewKeys(usize curve, Span<const CurveKey> keys) override;
        void CommitKeysEdit() override;
        void CancelKeysEdit() override;

    private:
        void Rebuild();
        [[nodiscard]] bool Valid(const Channel& channel) const;

        ClipDocument* m_document;
        Array<Channel> m_channels; // the dopesheet's
        Array<Channel> m_curves;   // the curve view's: the shown tracks' scalar channels
        Array<usize> m_shown;
        foundation::propertyanimation::PropertyAnimationClip m_moveFrom; // a move's clip as it began
    };
}
