// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::PropertyAnimation - the `:clip_source` partition (implementation).

module;
#include "Core/Prelude.h"

module editor.propertyanimation;

import foundation.core;
import foundation.propertyanimation;
import foundation.gui;
import foundation.gui.toolkit;

using namespace foundation::core;
namespace propanim = foundation::propertyanimation;
namespace gui = foundation::gui;

namespace editor
{
    namespace
    {
        constexpr f32 kSameTime = 1e-4f; // keys this close count as the same time

        // The part of a track's path that names a channel: x, y, z for a Float3; r, g, b, a for a
        // Color.
        [[nodiscard]] StringView PartName(propanim::TrackValueKind kind, i32 curve)
        {
            static constexpr StringView kXyz[] = {u8"x", u8"y", u8"z"};
            static constexpr StringView kRgba[] = {u8"r", u8"g", u8"b", u8"a"};
            if (kind == propanim::TrackValueKind::Float3 && curve >= 0 && curve < 3)
                return kXyz[curve];
            if (kind == propanim::TrackValueKind::Color && curve >= 0 && curve < 4)
                return kRgba[curve];
            return StringView{};
        }

        // A curve's stroke: x and r red, y and g green, z and b blue, a grey, a lone float amber.
        [[nodiscard]] Color PartColor(propanim::TrackValueKind kind, i32 curve)
        {
            if (kind == propanim::TrackValueKind::Float)
                return Color{0.95f, 0.75f, 0.3f, 1.0f};
            static constexpr Color kColors[] = {{0.92f, 0.38f, 0.38f, 1.0f}, {0.42f, 0.82f, 0.45f, 1.0f}, {0.42f, 0.62f, 1.0f, 1.0f}, {0.8f, 0.8f, 0.8f, 1.0f}};
            return curve >= 0 && curve < 4 ? kColors[curve] : Color{0.9f, 0.9f, 0.9f, 1.0f};
        }

        // A scalar channel's value, taken from the application's value for the whole track.
        [[nodiscard]] f32 PartOf(const Variant& value, propanim::TrackValueKind kind, i32 curve)
        {
            if (kind == propanim::TrackValueKind::Float && value.Is<f32>())
                return value.Get<f32>();
            if (kind == propanim::TrackValueKind::Float3 && value.Is<Float3>())
            {
                const Float3 v = value.Get<Float3>();
                const f32 parts[] = {v.x, v.y, v.z};
                return curve >= 0 && curve < 3 ? parts[curve] : 0.0f;
            }
            if (kind == propanim::TrackValueKind::Color && value.Is<Color>())
            {
                const Color c = value.Get<Color>();
                const f32 parts[] = {c.r, c.g, c.b, c.a};
                return curve >= 0 && curve < 4 ? parts[curve] : 0.0f;
            }
            return 0.0f;
        }

        // Sorts keys by time again after some were moved, and records where each moved key went.
        template <typename Key>
        void MoveKeys(Array<Key>& keys, Span<const usize> movedIndices, f32 delta, Array<usize>& newIndices, f32 Key::* time)
        {
            struct Entry
            {
                Key key;
                usize from = 0;
            };
            Array<Entry> entries;
            for (usize i = 0; i < keys.Size(); ++i)
            {
                Entry entry{keys[i], i};
                for (const usize moved : movedIndices)
                    if (moved == i)
                        entry.key.*time = Max(0.0f, entry.key.*time + delta);
                entries.PushBack(entry);
            }
            for (usize i = 1; i < entries.Size(); ++i) // stable: a key keeps its place among equals
            {
                const Entry entry = entries[i];
                usize j = i;
                while (j > 0 && entries[j - 1].key.*time > entry.key.*time)
                {
                    entries[j] = entries[j - 1];
                    --j;
                }
                entries[j] = entry;
            }
            newIndices.Clear();
            for (const usize moved : movedIndices)
                for (usize i = 0; i < entries.Size(); ++i)
                    if (entries[i].from == moved)
                        newIndices.PushBack(i);
            for (usize i = 0; i < entries.Size(); ++i)
                keys[i] = entries[i].key;
        }
    }

    ClipSource::ClipSource(ClipDocument& document) : m_document(&document)
    {
        Rebuild();
    }

    ClipSource::~ClipSource() = default;

    String ClipSource::TrackPath(const propanim::PropertyTrack& track)
    {
        return Format(u8"{}.{}", track.componentType, track.propertyPath);
    }

    void ClipSource::ShowTracks(Span<const usize> tracks)
    {
        m_shown.Clear();
        for (const usize track : tracks)
            m_shown.PushBack(track);
        Refresh();
    }

    void ClipSource::Refresh()
    {
        Rebuild();
        NotifyTimelineChanged();
        NotifyCurvesChanged();
    }

    void ClipSource::Rebuild()
    {
        const propanim::PropertyAnimationClip& clip = m_document->Clip();
        m_channels.Clear();
        for (usize t = 0; t < clip.tracks.Size(); ++t)
        {
            const propanim::PropertyTrack& track = clip.tracks[t];
            if (track.kind == propanim::TrackValueKind::Quat)
            {
                m_channels.PushBack(Channel{t, -1});
                continue;
            }
            for (u32 c = 0; c < propanim::ChannelCount(track.kind); ++c)
                m_channels.PushBack(Channel{t, static_cast<i32>(c)});
        }
        // Keep the shown tracks that still exist; their scalar channels become the curves.
        for (usize i = m_shown.Size(); i > 0; --i)
            if (m_shown[i - 1] >= clip.tracks.Size())
                m_shown.RemoveAt(i - 1);
        m_curves.Clear();
        for (const usize t : m_shown)
            for (u32 c = 0; c < propanim::ChannelCount(clip.tracks[t].kind); ++c)
                m_curves.PushBack(Channel{t, static_cast<i32>(c)});
    }

    bool ClipSource::Valid(const Channel& channel) const
    {
        const propanim::PropertyAnimationClip& clip = m_document->Clip();
        if (channel.Track >= clip.tracks.Size())
            return false;
        const propanim::PropertyTrack& track = clip.tracks[channel.Track];
        return channel.Curve < 0 ? track.kind == propanim::TrackValueKind::Quat : static_cast<u32>(channel.Curve) < propanim::ChannelCount(track.kind);
    }

    // === ITimelineSource ===

    String ClipSource::GetChannelPath(usize channel) const
    {
        if (channel >= m_channels.Size() || !Valid(m_channels[channel]))
            return String{};
        const propanim::PropertyTrack& track = m_document->Clip().tracks[m_channels[channel].Track];
        const StringView part = PartName(track.kind, m_channels[channel].Curve);
        return part.IsEmpty() ? TrackPath(track) : Format(u8"{}.{}", TrackPath(track), part);
    }

    void ClipSource::GetKeyTimes(usize channel, Array<f32>& out) const
    {
        out.Clear();
        if (channel >= m_channels.Size() || !Valid(m_channels[channel]))
            return;
        const Channel& at = m_channels[channel];
        const propanim::PropertyTrack& track = m_document->Clip().tracks[at.Track];
        if (at.Curve < 0)
            for (const propanim::QuatKey& key : track.quatKeys)
                out.PushBack(key.time);
        else
            for (const CurveKey& key : track.channels[at.Curve].Keys())
                out.PushBack(key.time);
    }

    void ClipSource::BeginKeysMove()
    {
        m_document->BeginGesture();
        m_moveFrom = m_document->Clip();
    }

    void ClipSource::PreviewKeysMove(Span<const gui::TimelineKeyRef> keys, f32 delta, Array<gui::TimelineKeyRef>& moved)
    {
        moved.Clear();
        if (!m_document->InGesture())
            return;
        propanim::PropertyAnimationClip& clip = m_document->GestureClip();
        clip = m_moveFrom; // each preview starts from where the keys were
        // Channel by channel: shift the moved keys, then sort the channel again.
        Array<usize> done;
        for (const gui::TimelineKeyRef& first : keys)
        {
            bool seen = false;
            for (const usize channel : done)
                seen = seen || channel == first.Channel;
            if (seen || first.Channel >= m_channels.Size() || !Valid(m_channels[first.Channel]))
                continue;
            done.PushBack(first.Channel);
            Array<usize> indices;
            for (const gui::TimelineKeyRef& key : keys)
                if (key.Channel == first.Channel)
                    indices.PushBack(key.Index);
            const Channel& at = m_channels[first.Channel];
            propanim::PropertyTrack& track = clip.tracks[at.Track];
            Array<usize> placed;
            if (at.Curve < 0)
                MoveKeys(track.quatKeys, indices.AsSpan(), delta, placed, &propanim::QuatKey::time);
            else
            {
                MoveKeys(track.channels[at.Curve].Keys(), indices.AsSpan(), delta, placed, &CurveKey::time);
                track.channels[at.Curve].ApplyTangentModes();
            }
            for (usize i = 0; i < placed.Size(); ++i)
                moved.PushBack(gui::TimelineKeyRef{first.Channel, placed[i]});
        }
        m_document->PreviewGesture();
    }

    void ClipSource::CommitKeysMove()
    {
        m_document->CommitGesture(u8"propanim-move-keys");
    }

    void ClipSource::CancelKeysMove()
    {
        m_document->CancelGesture();
    }

    bool ClipSource::DeleteKeys(Span<const gui::TimelineKeyRef> keys)
    {
        // Only keys that exist; with none, nothing changes.
        Array<gui::TimelineKeyRef> there;
        Array<f32> times;
        for (const gui::TimelineKeyRef& key : keys)
        {
            if (key.Channel >= m_channels.Size())
                continue;
            GetKeyTimes(key.Channel, times);
            if (key.Index < times.Size())
                there.PushBack(key);
        }
        if (there.IsEmpty())
            return false;
        const Array<Channel> channels = m_channels;
        m_document->Edit(u8"propanim-delete-keys", [&there, &channels](propanim::PropertyAnimationClip& clip)
                         {
                             // Remove the last first, so each index still names its key.
                             for (usize i = 1; i < there.Size(); ++i)
                             {
                                 const gui::TimelineKeyRef key = there[i];
                                 usize j = i;
                                 while (j > 0 && there[j - 1].Index < key.Index)
                                 {
                                     there[j] = there[j - 1];
                                     --j;
                                 }
                                 there[j] = key;
                             }
                             for (const gui::TimelineKeyRef& key : there)
                             {
                                 const Channel& at = channels[key.Channel];
                                 propanim::PropertyTrack& track = clip.tracks[at.Track];
                                 if (at.Curve < 0)
                                     track.quatKeys.RemoveAt(key.Index);
                                 else
                                 {
                                     track.channels[at.Curve].Keys().RemoveAt(key.Index);
                                     track.channels[at.Curve].ApplyTangentModes();
                                 }
                             }
                         });
        return true;
    }

    bool ClipSource::InsertKey(usize channel, f32 time, gui::TimelineKeyRef& made)
    {
        if (channel >= m_channels.Size() || !Valid(m_channels[channel]) || !m_document->HasClip())
            return false;
        time = Max(0.0f, time);
        const Channel at = m_channels[channel];
        const propanim::PropertyTrack& track = m_document->Clip().tracks[at.Track];
        // A key already at that time is the one returned.
        Array<f32> times;
        GetKeyTimes(channel, times);
        for (usize i = 0; i < times.Size(); ++i)
            if (Abs(times[i] - time) < kSameTime)
            {
                made = gui::TimelineKeyRef{channel, i};
                return true;
            }
        const Variant current = CurrentValue ? CurrentValue(track) : Variant{};
        usize index = 0;
        if (at.Curve < 0)
        {
            const Quaternion value = !track.quatKeys.IsEmpty() ? track.SampleQuat(time)
                                                               : (current.Is<Quaternion>() ? current.Get<Quaternion>() : Quaternion::Identity);
            for (const propanim::QuatKey& key : track.quatKeys)
                index += key.time < time ? 1u : 0u;
            m_document->Edit(u8"propanim-insert-key", [&at, time, value, index](propanim::PropertyAnimationClip& clip)
                             { clip.tracks[at.Track].quatKeys.Insert(index, propanim::QuatKey{time, value}); });
        }
        else
        {
            const Curve& curve = track.channels[at.Curve];
            const f32 value = curve.KeyCount() > 0 ? curve.Evaluate(time) : PartOf(current, track.kind, at.Curve);
            for (const CurveKey& key : curve.Keys())
                index += key.time < time ? 1u : 0u;
            m_document->Edit(u8"propanim-insert-key", [&at, time, value](propanim::PropertyAnimationClip& clip)
                             {
                                 Curve& edited = clip.tracks[at.Track].channels[at.Curve];
                                 edited.AddKey(CurveKey{time, value, 0.0f, 0.0f, CurveKeyInterpolation::Cubic, CurveTangentMode::Auto});
                                 edited.ApplyTangentModes();
                             });
        }
        made = gui::TimelineKeyRef{channel, index};
        return true;
    }

    // === ICurveSource ===

    gui::CurveInfo ClipSource::GetCurveInfo(usize curve) const
    {
        gui::CurveInfo info;
        if (curve >= m_curves.Size() || !Valid(m_curves[curve]))
            return info;
        const Channel& at = m_curves[curve];
        const propanim::PropertyTrack& track = m_document->Clip().tracks[at.Track];
        const StringView part = PartName(track.kind, at.Curve);
        info.Name = part.IsEmpty() ? TrackPath(track) : Format(u8"{}.{}", TrackPath(track), part);
        info.StrokeColor = PartColor(track.kind, at.Curve);
        return info;
    }

    Span<const CurveKey> ClipSource::GetKeys(usize curve) const
    {
        if (curve >= m_curves.Size() || !Valid(m_curves[curve]))
            return Span<const CurveKey>{};
        const Channel& at = m_curves[curve];
        return m_document->Clip().tracks[at.Track].channels[at.Curve].Keys().AsSpan();
    }

    void ClipSource::BeginKeysEdit()
    {
        m_document->BeginGesture();
    }

    void ClipSource::PreviewKeys(usize curve, Span<const CurveKey> keys)
    {
        if (!m_document->InGesture() || curve >= m_curves.Size() || !Valid(m_curves[curve]))
            return;
        const Channel& at = m_curves[curve];
        Array<CurveKey>& edited = m_document->GestureClip().tracks[at.Track].channels[at.Curve].Keys();
        edited.Clear();
        for (const CurveKey& key : keys)
            edited.PushBack(key);
        m_document->PreviewGesture();
    }

    void ClipSource::CommitKeysEdit()
    {
        m_document->CommitGesture(u8"propanim-edit-curve");
    }

    void ClipSource::CancelKeysEdit()
    {
        m_document->CancelGesture();
    }
}
