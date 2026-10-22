// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI.Toolkit - :curve_source partition
//
// What a curve view reads (T5 of Specs/gui-lab-evolution-3.md): curves of Core's `CurveKey`s, ruling
// 2's one model, so what is edited is what plays; the toolkit has no curve type of its own. A source
// says which curves there are and each one's keys, and the view edits through it as gestures, as a
// property grid does through a property source: begun, previewed live (each changed curve's keys
// whole), then committed (one change, one undo entry) or cancelled (the keys as they were). The undo
// is the source's, an application's (ruling 1).
//
// CurveSet is a source of its own curves in memory: what tests and a page without curves of its own
// edit, its commits told to whoever keeps the undo, which hands them back to undo and redo.

module;
#include "Core/Prelude.h"

export module experimental.gui.toolkit:curve_source;

import foundation.core;
import experimental.gui;

using namespace foundation::core;
namespace core = foundation::core;

export namespace experimental::gui
{
    /// What a source says of one curve.
    struct CurveInfo
    {
        core::String Name;
        Color StrokeColor{0.9f, 0.9f, 0.9f, 1.0f};
        bool HasRange = false; ///< values kept between Min and Max, and a framing shows at least them
        f32 Min = 0.0f;
        f32 Max = 0.0f;
        u32 MaxKeys = 0;       ///< the most keys it holds (a particle curve's eight); 0: no limit
        bool Locked = false;   ///< drawn, not edited
    };

    /// A key: its curve and its place among that curve's keys (sorted by time).
    struct CurveKeyRef
    {
        usize Curve = 0;
        usize Index = 0;
        [[nodiscard]] bool operator==(const CurveKeyRef&) const noexcept = default;
    };

    /// What a source tells its views.
    class ICurveSourceClient
    {
    public:
        virtual ~ICurveSourceClient() = default;
        /// The curves, what they say of themselves, or their keys changed: read them again.
        virtual void OnCurvesChanged() = 0;
    };

    class ICurveSource
    {
    public:
        virtual ~ICurveSource() = default;

        [[nodiscard]] virtual usize CurveCount() const = 0;
        [[nodiscard]] virtual CurveInfo GetCurveInfo(usize curve) const = 0;
        /// Its keys, sorted by time.
        [[nodiscard]] virtual core::Span<const CurveKey> GetKeys(usize curve) const = 0;

        /// An edit begins: the keys now are what a cancel puts back.
        virtual void BeginKeysEdit() = 0;
        /// A curve's keys as the edit goes (sorted, their tangents set by their modes), live.
        virtual void PreviewKeys(usize curve, core::Span<const CurveKey> keys) = 0;
        /// The edit ends: what it previewed is one change.
        virtual void CommitKeysEdit() = 0;
        /// The edit is given up: the keys as they were.
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
        core::Array<ICurveSourceClient*> m_clients; // non-owning: each view leaves as it goes
    };

    /// One curve's keys before and after a change.
    struct CurveEdit
    {
        usize Curve = 0;
        core::Array<CurveKey> Before;
        core::Array<CurveKey> After;
    };
    /// A committed change: the curves it touched.
    struct CurveChange
    {
        core::Array<CurveEdit> Curves;
    };

    /// Curves of its own, in memory.
    class CurveSet : public ICurveSource
    {
    public:
        /// Adds a curve; its index.
        usize AddCurve(const CurveInfo& info, core::Span<const CurveKey> keys = {})
        {
            Curve curve;
            curve.Info = info;
            for (const CurveKey& key : keys)
                curve.Keys.PushBack(key);
            m_curves.PushBack(core::Move(curve));
            NotifyCurvesChanged();
            return m_curves.Size() - 1;
        }
        /// Sets a curve's keys from code: shown, no change told.
        void SetKeys(usize curve, core::Span<const CurveKey> keys)
        {
            if (curve >= m_curves.Size())
                return;
            Assign(m_curves[curve].Keys, keys);
            NotifyCurvesChanged();
        }
        [[nodiscard]] CurveInfo& Info(usize curve) { return m_curves[curve].Info; }
        /// Something changed an info: tell the views.
        void InfoChanged() { NotifyCurvesChanged(); }

        /// Each commit, for whoever keeps the undo.
        void SetOnCommit(core::Function<void(const CurveChange&)> callback) { m_onCommit = core::Move(callback); }
        /// Puts a change's keys back (`undo`: its befores; else its afters), no change told.
        void Apply(const CurveChange& change, bool undo)
        {
            for (const CurveEdit& edit : change.Curves)
                if (edit.Curve < m_curves.Size())
                    Assign(m_curves[edit.Curve].Keys, (undo ? edit.Before : edit.After).AsSpan());
            NotifyCurvesChanged();
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
            NotifyCurvesChanged();
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
            NotifyCurvesChanged();
        }
        void CancelKeysEdit() override
        {
            if (!m_editing)
                return;
            m_editing = false;
            for (usize c = 0; c < m_curves.Size() && c < m_before.Size(); ++c)
                m_curves[c].Keys = core::Move(m_before[c]);
            m_before.Clear();
            NotifyCurvesChanged();
        }

        /// Whether two key lists are the same, key by key.
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
