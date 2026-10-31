// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// ModelViewer - `modelviewer:tabs`: the models open in the viewer, one tab each. Opening a file
// again switches to its existing tab. Closing the front tab brings a neighbour to the front, as the
// editor's tabs do. Each tab keeps its model bound along with what the window placed in the scene
// for it (its instance, its bounds, its last camera), so switching back finds it unchanged. The
// side panel's rows come from a tab's stats (StatRows). A tab can be added before its model is
// ready (Begin), with the file read on a worker and the open completed when the read arrives
// (Finish), so the window shows the tab at once.

module;
#include "Core/Prelude.h"

export module modelviewer:tabs;

import foundation.core;
import foundation.rhi;
import engine.model;
import :library;
import :playback;
import :loads;

using namespace foundation::core;

export namespace modelviewer
{
    /// The camera pose last used on a tab: the editor camera's state, stored without the editor.
    struct TabCamera
    {
        bool saved = false;
        Float3 position{};
        f32 yaw = 0.0f;
        f32 pitch = 0.0f;
        f32 focusDistance = 0.0f;
    };

    /// One open model.
    struct ModelTab
    {
        String path;
        String title; // the file's name
        UniquePtr<OpenModel> open;
        bool ready = false;                    // its model is bound (false while its file is read)
        engine::model::ModelInstance instance; // what the window placed in the scene for it
        UniquePtr<ClipPlayback> playback;      // the animation bar's state for that instance
        Float3 center{};                       // the bounds as placed, measured by the window
        f32 radius = 1.0f;
        Float3 size{};                         // the bounds' extent, upright and scaled (the panel's Size row)
        TabCamera camera;
    };

    class ModelTabs
    {
    public:
        explicit ModelTabs(IAllocator& allocator) : m_allocator(&allocator), m_tabs(allocator) {}

        struct Opened
        {
            i32 index = -1;     // the model's tab, -1 when it did not open
            bool added = false; // a new tab was added (false: it was open already)
            bool needsFile = false; // the tab was added before its model is ready: read the file, then call Finish
            OpenModel::Outcome outcome = OpenModel::Outcome::NotFound;
        };

        /// Like Open, but without reading the file. A model cooked before is bound and its tab is
        /// ready; a model whose file must be read gets its tab now with `needsFile` set, and the
        /// caller calls Finish once the file is read.
        Opened Begin(StringView path, StringView cacheRoot, foundation::rhi::Device* device, bool looseTextures = true)
        {
            Opened result;
            if (const i32 existing = Find(path); existing >= 0)
            {
                m_active = existing;
                result.index = existing;
                result.outcome = OpenModel::Outcome::FromCache;
                return result;
            }
            UniquePtr<OpenModel> open = MakeUnique<OpenModel>(*m_allocator, *m_allocator);
            open->SetLooseTextures(looseTextures);
            const OpenModel::Begun begun = open->Begin(path, cacheRoot, device, result.outcome);
            if (begun == OpenModel::Begun::Failed)
                return result;
            ModelTab tab;
            tab.path = String(path);
            tab.title = FileTitle(path);
            tab.open = foundation::core::Move(open);
            tab.ready = begun == OpenModel::Begun::Bound;
            m_tabs.PushBack(foundation::core::Move(tab));
            m_active = static_cast<i32>(m_tabs.Size()) - 1;
            result.index = m_active;
            result.added = true;
            result.needsFile = !m_tabs[static_cast<usize>(m_active)].ready;
            return result;
        }

        /// Completes the open of a tab that Begin left waiting, using `read` (from FileLoads or
        /// OpenModel::LoadFile). On Opened the tab is ready; on any other outcome the tab is left
        /// for the caller to close.
        OpenModel::Outcome Finish(i32 index, FileRead& read)
        {
            if (!Valid(index))
                return OpenModel::Outcome::NotFound;
            ModelTab& tab = m_tabs[static_cast<usize>(index)];
            if (read.outcome != OpenModel::Outcome::Opened)
                return read.outcome;
            const OpenModel::Outcome outcome = tab.open->Finish(read.model, read.stats);
            tab.ready = outcome == OpenModel::Outcome::Opened;
            return outcome;
        }

        /// Opens `path` into a new tab at the end and brings it to the front; a file that is already
        /// open has its tab brought to the front instead. A file that fails to open adds no tab.
        /// `looseTextures` is passed to OpenModel::SetLooseTextures.
        Opened Open(StringView path, StringView cacheRoot, foundation::rhi::Device* device, bool looseTextures = true)
        {
            const i32 before = m_active;
            Opened result = Begin(path, cacheRoot, device, looseTextures);
            if (!result.needsFile)
                return result;
            FileRead read;
            read.outcome = OpenModel::LoadFile(path, looseTextures, read.model, read.stats);
            result.outcome = Finish(result.index, read);
            result.needsFile = false;
            if (result.outcome != OpenModel::Outcome::Opened)
            {
                (void)Close(result.index);
                m_active = Valid(before) ? before : m_active; // a file that fails to open adds no tab
                result.index = -1;
                result.added = false;
            }
            return result;
        }

        /// Closes a tab. When it was in front, the tab after it comes to the front (else the one
        /// before); otherwise the front tab stays. Returns the front tab's index, -1 when none is left.
        i32 Close(i32 index)
        {
            if (!Valid(index))
                return m_active;
            m_tabs.RemoveAt(static_cast<usize>(index));
            if (m_tabs.IsEmpty())
                m_active = -1;
            else if (index < m_active || m_active >= static_cast<i32>(m_tabs.Size()))
                m_active = Max(m_active - 1, 0);
            return m_active;
        }

        /// Moves a tab from one position to another; the front tab stays in front.
        bool Move(i32 from, i32 to)
        {
            if (!Valid(from) || !Valid(to) || from == to)
                return false;
            ModelTab moved = foundation::core::Move(m_tabs[static_cast<usize>(from)]);
            m_tabs.RemoveAt(static_cast<usize>(from));
            m_tabs.Insert(static_cast<usize>(to), foundation::core::Move(moved));
            if (m_active == from)
                m_active = to;
            else if (from < m_active && to >= m_active)
                --m_active;
            else if (from > m_active && to <= m_active)
                ++m_active;
            return true;
        }

        void SetActive(i32 index)
        {
            if (Valid(index))
                m_active = index;
        }
        [[nodiscard]] i32 Active() const noexcept { return m_active; }
        [[nodiscard]] ModelTab* ActiveTab() { return Valid(m_active) ? &m_tabs[static_cast<usize>(m_active)] : nullptr; }
        [[nodiscard]] i32 Count() const noexcept { return static_cast<i32>(m_tabs.Size()); }
        [[nodiscard]] ModelTab& At(i32 index) { return m_tabs[static_cast<usize>(index)]; }
        [[nodiscard]] bool Valid(i32 index) const noexcept { return index >= 0 && index < static_cast<i32>(m_tabs.Size()); }

        /// The tab a file is open in, or -1.
        [[nodiscard]] i32 Find(StringView path) const
        {
            for (usize i = 0; i < m_tabs.Size(); ++i)
                if (m_tabs[i].path.AsView() == path)
                    return static_cast<i32>(i);
            return -1;
        }

        void Clear()
        {
            m_tabs.Clear();
            m_active = -1;
        }

    private:
        IAllocator* m_allocator;
        Array<ModelTab> m_tabs;
        i32 m_active = -1;
    };

    /// One row of the side panel.
    struct StatRow
    {
        String label;
        String value;
    };

    /// A count with its thousands grouped (12,345).
    [[nodiscard]] inline String GroupedCount(u64 value)
    {
        String digits = Format(u8"{}", value);
        String grouped;
        const usize n = digits.Size();
        for (usize i = 0; i < n; ++i)
        {
            if (i > 0 && (n - i) % 3 == 0)
                grouped += u8",";
            grouped += digits.AsView().SubStr(i, 1);
        }
        return grouped;
    }

    /// The side panel's rows for a model's stats, in display order.
    [[nodiscard]] inline Array<StatRow> StatRows(const ModelStats& stats)
    {
        Array<StatRow> rows;
        auto row = [&](StringView label, String value) { rows.PushBack(StatRow{String(label), foundation::core::Move(value)}); };
        row(u8"Meshes", GroupedCount(stats.meshes));
        row(u8"Vertices", GroupedCount(stats.vertices));
        row(u8"Triangles", GroupedCount(stats.triangles));
        row(u8"Materials", GroupedCount(stats.materials));
        row(u8"Textures", GroupedCount(stats.textures));
        row(u8"Bones", GroupedCount(stats.bones));
        row(u8"Clips", GroupedCount(stats.clips.Size()));
        row(u8"Size", Format(u8"{} x {} x {}", FormatFixed(stats.size.x, 2), FormatFixed(stats.size.y, 2), FormatFixed(stats.size.z, 2)));
        return rows;
    }
}
