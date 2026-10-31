// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI.Toolkit - :curve_source partition
//
// What a curve view reads. Curves are made of Core's `CurveKey`s, the engine's one curve model, so
// what is edited is what plays; the toolkit has no curve type of its own. A source says which
// curves there are and gives each one's keys. The view edits through it in gestures, as a property
// grid does through a property source: a gesture begins, previews live (sending each changed
// curve's keys whole), and then either commits (one change, one undo entry) or cancels (the keys
// go back as they were). The undo belongs to the source, that is, to the application.
//
// CurveSet is a source that holds its own curves in memory. Tests use it, as does a page with no
// curves of its own. It reports each commit to whoever keeps the undo, and that owner hands the
// change back to undo or redo it. It is also a timeline source, with each curve a channel named by
// its path (its name), so a dopesheet and a curve view can stack over one clip.

module;
#include "Core/Prelude.h"

export module foundation.gui.toolkit:curve_source;

import foundation.core;
import foundation.gui;
import :timeline_source;

using namespace foundation::core;
namespace core = foundation::core;

export namespace foundation::gui
{
    /// What a source reports about one curve.
    struct CurveInfo
    {
        core::String Name;
        Color StrokeColor{0.9f, 0.9f, 0.9f, 1.0f};
        bool HasRange = false; ///< values are kept between Min and Max, and framing shows at least that range
        f32 Min = 0.0f;
        f32 Max = 0.0f;
        u32 MaxKeys = 0;       ///< the most keys it holds (a particle curve's eight); 0 for no limit
        bool Locked = false;   ///< drawn but not editable
    };

    /// Identifies a key by its curve and its index among that curve's keys (sorted by time).
    struct CurveKeyRef
    {
        usize Curve = 0;
        usize Index = 0;
        [[nodiscard]] bool operator==(const CurveKeyRef&) const noexcept = default;
    };

    /// The notifications a source sends its views.
    class ICurveSourceClient
    {
    public:
        virtual ~ICurveSourceClient() = default;
        /// Called when the curves, their info or their keys changed; the view reads them again.
        virtual void OnCurvesChanged() = 0;
    };

    class ICurveSource
    {
    public:
        virtual ~ICurveSource() = default;

        [[nodiscard]] virtual usize CurveCount() const = 0;
        [[nodiscard]] virtual CurveInfo GetCurveInfo(usize curve) const = 0;
        /// Returns the keys of `curve`, sorted by time.
        [[nodiscard]] virtual core::Span<const CurveKey> GetKeys(usize curve) const = 0;

        /// Begins an edit. The keys as they are now are what a cancel restores.
        virtual void BeginKeysEdit() = 0;
        /// Sets a curve's keys live while the edit goes on (sorted, with tangents set by their
        /// modes).
        virtual void PreviewKeys(usize curve, core::Span<const CurveKey> keys) = 0;
        /// Ends the edit, making everything it previewed one change.
        virtual void CommitKeysEdit() = 0;
        /// Cancels the edit and restores the keys as they were.
        virtual void CancelKeysEdit() = 0;

        void AddClient(ICurveSourceClient* client) { m_clients.PushBack(client); }
        void RemoveClient(ICurveSourceClient* client)
        {
            for (usize i = 0; i < m_clients.Size(); ++i)
                if (m_clients[i] == client)
                {
                    m_clients.RemoveAt(i);
                    return;
                }
        }

    protected:
        void NotifyCurvesChanged()
        {
            for (usize i = 0; i < m_clients.Size(); ++i)
                m_clients[i]->OnCurvesChanged();
        }

    private:
        core::Array<ICurveSourceClient*> m_clients; // non-owning: each view removes itself before it goes away
    };

    /// One curve's keys before and after a change.
    struct CurveEdit
    {
        usize Curve = 0;
        core::Array<CurveKey> Before;
        core::Array<CurveKey> After;
    };
    /// A committed change: the edits to each curve it touched.
    struct CurveChange
    {
        core::Array<CurveEdit> Curves;
    };

    /// A set of curves held in memory. As a timeline source, each curve is a channel named by its
    /// name.
    class CurveSet : public ICurveSource, public ITimelineSource
    {
    public:
        /// Adds a curve and returns its index.
        usize AddCurve(const CurveInfo& info, core::Span<const CurveKey> keys = {})
        {
            Curve curve;
            curve.Info = info;
            for (const CurveKey& key : keys)
                curve.Keys.PushBack(key);
            m_curves.PushBack(core::Move(curve));
            Changed();
            return m_curves.Size() - 1;
        }
        /// Sets a curve's keys from code. The views show them, but no commit is reported.
        void SetKeys(usize curve, core::Span<const CurveKey> keys)
        {
            if (curve >= m_curves.Size())
                return;
            Assign(m_curves[curve].Keys, keys);
            Changed();
        }
        [[nodiscard]] CurveInfo& Info(usize curve) { return m_curves[curve].Info; }
        /// Tells the views that some curve's info changed.
        void InfoChanged() { Changed(); }

        /// Called with each commit, for whoever keeps the undo.
        void SetOnCommit(core::Function<void(const CurveChange&)> callback) { m_onCommit = core::Move(callback); }
        /// Restores a change's keys: its befores with `undo`, else its afters. The views are told,
        /// but no commit is reported.
        void Apply(const CurveChange& change, bool undo)
        {
            for (const CurveEdit& edit : change.Curves)
                if (edit.Curve < m_curves.Size())
                    Assign(m_curves[edit.Curve].Keys, (undo ? edit.Before : edit.After).AsSpan());
            Changed();
        }
        [[nodiscard]] bool IsEditing() const noexcept { return m_editing; }

        // === ICurveSource ===
        [[nodiscard]] usize CurveCount() const override { return m_curves.Size(); }
        [[nodiscard]] CurveInfo GetCurveInfo(usize curve) const override { return curve < m_curves.Size() ? m_curves[curve].Info : CurveInfo{}; }
        [[nodiscard]] core::Span<const CurveKey> GetKeys(usize curve) const override
        {
            return curve < m_curves.Size() ? m_curves[curve].Keys.AsSpan() : core::Span<const CurveKey>{};
        }
        void BeginKeysEdit() override
        {
            if (m_editing)
                CancelKeysEdit();
            m_editing = true;
            m_before.Clear();
            for (const Curve& curve : m_curves)
            {
                core::Array<CurveKey> copy;
                Assign(copy, curve.Keys.AsSpan());
                m_before.PushBack(core::Move(copy));
            }
        }
        void PreviewKeys(usize curve, core::Span<const CurveKey> keys) override
        {
            if (!m_editing || curve >= m_curves.Size())
                return;
            Assign(m_curves[curve].Keys, keys);
            Changed();
        }
        void CommitKeysEdit() override
        {
            if (!m_editing)
                return;
            m_editing = false;
            CurveChange change;
            for (usize c = 0; c < m_curves.Size() && c < m_before.Size(); ++c)
            {
                if (Same(m_before[c].AsSpan(), m_curves[c].Keys.AsSpan()))
                    continue;
                CurveEdit edit;
                edit.Curve = c;
                edit.Before = core::Move(m_before[c]);
                Assign(edit.After, m_curves[c].Keys.AsSpan());
                change.Curves.PushBack(core::Move(edit));
            }
            m_before.Clear();
            if (!change.Curves.IsEmpty() && m_onCommit)
                m_onCommit(change);
            Changed();
        }
        void CancelKeysEdit() override
        {
            if (!m_editing)
                return;
            m_editing = false;
            for (usize c = 0; c < m_curves.Size() && c < m_before.Size(); ++c)
                m_curves[c].Keys = core::Move(m_before[c]);
            m_before.Clear();
            Changed();
        }

        // === ITimelineSource ===
        [[nodiscard]] usize ChannelCount() const override { return m_curves.Size(); }
        [[nodiscard]] core::String GetChannelPath(usize channel) const override { return channel < m_curves.Size() ? m_curves[channel].Info.Name : core::String{}; }
        void GetKeyTimes(usize channel, core::Array<f32>& out) const override
        {
            out.Clear();
            if (channel < m_curves.Size())
                for (const CurveKey& key : m_curves[channel].Keys)
                    out.PushBack(key.time);
        }
        void BeginKeysMove() override { BeginKeysEdit(); }
        void PreviewKeysMove(core::Span<const TimelineKeyRef> keys, f32 delta, core::Array<TimelineKeyRef>& moved) override
        {
            moved.Clear();
            if (!m_editing)
                return;
            for (usize c = 0; c < m_curves.Size() && c < m_before.Size(); ++c)
            {
                bool any = false;
                for (const TimelineKeyRef& ref : keys)
                    any = any || ref.Channel == c;
                if (!any || m_curves[c].Info.Locked)
                    continue;
                core::Array<CurveKey> next;
                core::Array<bool> chosen;
                for (usize i = 0; i < m_before[c].Size(); ++i)
                {
                    CurveKey key = m_before[c][i];
                    bool selected = false;
                    for (const TimelineKeyRef& ref : keys)
                        selected = selected || (ref.Channel == c && ref.Index == i);
                    if (selected)
                        key.time = core::Max(0.0f, key.time + delta);
                    next.PushBack(key);
                    chosen.PushBack(selected);
                }
                // Sort the keys again, stably, and track where the moved keys end up.
                for (usize i = 1; i < next.Size(); ++i)
                    for (usize j = i; j > 0 && next[j].time < next[j - 1].time; --j)
                    {
                        core::Swap(next[j], next[j - 1]);
                        const bool m = chosen[j];
                        chosen[j] = chosen[j - 1];
                        chosen[j - 1] = m;
                    }
                ApplyTangentModes(next.AsSpan());
                m_curves[c].Keys = core::Move(next);
                for (usize n = 0; n < chosen.Size(); ++n)
                    if (chosen[n])
                        moved.PushBack(TimelineKeyRef{c, n});
            }
            Changed();
        }
        void CommitKeysMove() override { CommitKeysEdit(); }
        void CancelKeysMove() override { CancelKeysEdit(); }
        bool DeleteKeys(core::Span<const TimelineKeyRef> keys) override
        {
            if (keys.IsEmpty())
                return false;
            BeginKeysEdit();
            bool any = false;
            for (usize c = 0; c < m_curves.Size(); ++c)
            {
                if (m_curves[c].Info.Locked)
                    continue;
                core::Array<CurveKey> kept;
                for (usize i = 0; i < m_curves[c].Keys.Size(); ++i)
                {
                    bool gone = false;
                    for (const TimelineKeyRef& ref : keys)
                        gone = gone || (ref.Channel == c && ref.Index == i);
                    if (gone)
                        any = true;
                    else
                        kept.PushBack(m_curves[c].Keys[i]);
                }
                ApplyTangentModes(kept.AsSpan());
                m_curves[c].Keys = core::Move(kept);
            }
            CommitKeysEdit();
            return any;
        }
        bool InsertKey(usize channel, f32 time, TimelineKeyRef& made) override
        {
            if (channel >= m_curves.Size())
                return false;
            Curve& curve = m_curves[channel];
            if (curve.Info.Locked || (curve.Info.MaxKeys != 0 && curve.Keys.Size() >= curve.Info.MaxKeys))
                return false;
            BeginKeysEdit();
            CurveKey key;
            key.time = time;
            key.value = EvaluateCurve(curve.Keys.AsSpan(), time);
            key.interpolation = CurveKeyInterpolation::Cubic;
            key.tangentMode = CurveTangentMode::Auto;
            usize at = 0;
            while (at < curve.Keys.Size() && curve.Keys[at].time <= time)
                ++at;
            if (at > 0)
                key.interpolation = curve.Keys[at - 1].interpolation; // takes the interpolation of the segment it splits
            curve.Keys.Insert(at, key);
            ApplyTangentModes(curve.Keys.AsSpan());
            CommitKeysEdit();
            made = TimelineKeyRef{channel, at};
            return true;
        }

        /// Returns whether two key lists are the same, key by key.
        [[nodiscard]] static bool Same(core::Span<const CurveKey> a, core::Span<const CurveKey> b) noexcept
        {
            if (a.Size() != b.Size())
                return false;
            for (usize i = 0; i < a.Size(); ++i)
                if (a[i].time != b[i].time || a[i].value != b[i].value || a[i].tangentIn != b[i].tangentIn ||
                    a[i].tangentOut != b[i].tangentOut || a[i].interpolation != b[i].interpolation || a[i].tangentMode != b[i].tangentMode)
                    return false;
            return true;
        }

    private:
        // Tells the views: the curve views and the dopesheets.
        void Changed()
        {
            NotifyCurvesChanged();
            NotifyTimelineChanged();
        }
        struct Curve
        {
            CurveInfo Info;
            core::Array<CurveKey> Keys;
        };
        static void Assign(core::Array<CurveKey>& to, core::Span<const CurveKey> from)
        {
            to.Clear();
            for (const CurveKey& key : from)
                to.PushBack(key);
        }

        core::Array<Curve> m_curves;
        core::Array<core::Array<CurveKey>> m_before;
        bool m_editing = false;
        core::Function<void(const CurveChange&)> m_onCommit;
    };
}
