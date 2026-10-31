// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::PropertyAnimation - the `:clip_source` partition.
//
// ClipSource presents a clip document to the toolkit's dopesheet and curve view. Its channels come
// from the clip: each curve of a scalar track is a channel ("Light.intensity",
// "Transform.position.x", "Light.color.r"), and a rotation track's keys form one channel
// ("Transform.rotation"), so the dopesheet groups them by component, property and channel. Its
// curves are the scalar channels of the tracks the panel shows (a rotation's keys are slerped, so
// they are not a curve). Every edit goes through the document: a drag is a gesture that becomes one
// undo step when it ends, and each deletion or insertion is one step. An inserted key takes the
// clip's value at that time or, where the channel has no keys yet, the value the application gives
// (the bound entity's), so a first key holds what the scene shows.

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
        /// A channel: its track and, for a scalar track, which of its curves (-1 for a rotation's keys).
        struct Channel
        {
            usize Track = 0;
            i32 Curve = -1;
        };

        explicit ClipSource(ClipDocument& document);
        ~ClipSource() override;
        ClipSource(const ClipSource&) = delete;
        ClipSource& operator=(const ClipSource&) = delete;

        /// Sets the tracks whose curves the curve view shows (the chosen ones); empty shows none.
        void ShowTracks(Span<const usize> tracks);
        [[nodiscard]] Span<const usize> ShownTracks() const noexcept { return m_shown.AsSpan(); }
        /// The application's current value of a track's target (the bound entity's), used for a
        /// first key; when unset or empty, the type's zero is used.
        Function<Variant(const foundation::propertyanimation::PropertyTrack&)> CurrentValue;

        /// Called by the owner from the document's OnChanged: reads the channels and curves again
        /// and notifies the views.
        void Refresh();

        [[nodiscard]] Span<const Channel> Channels() const noexcept { return m_channels.AsSpan(); }
        /// The path shared by a track's channels ("Light.color"); a channel's own path adds its part
        /// ("Light.color.r").
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
        Array<Channel> m_channels; // the dopesheet's channels
        Array<Channel> m_curves;   // the curve view's channels: the shown tracks' scalar channels
        Array<usize> m_shown;
        foundation::propertyanimation::PropertyAnimationClip m_moveFrom; // the clip as it was when a move began
    };
}
