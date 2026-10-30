// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// ModelViewer - `modelviewer:tabs`: the models open in the viewer, one tab each
// (Documentation/Specs/model-viewer.md, P1). A file opened again goes to its tab rather than a
// second one; closing a tab hands the front to its neighbour, as the editor's tabs do. Each tab
// keeps its model bound and what the window puts in the scene for it (its instance, its bounds,
// the camera it was left with), so switching back finds it as it was. The side panel's rows come
// from a tab's numbers (StatRows).

module;
#include "Core/Prelude.h"

export module modelviewer:tabs;

import foundation.core;
import foundation.rhi;
import engine.model;
import :library;
import :playback;

using namespace foundation::core;

export namespace modelviewer
{
    /// Where the camera was left on a tab (the editor camera's pose, kept without the editor).
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
        engine::model::ModelInstance instance; // what the window put in the scene for it
        UniquePtr<ClipPlayback> playback;      // its animation bar's state, over that instance
        Float3 center{};                       // its bounds as placed, measured by the window
        f32 radius = 1.0f;
        Float3 size{};                         // their extent (the panel's Size: upright, scaled)
        TabCamera camera;
    };

    class ModelTabs
    {
    public:
        explicit ModelTabs(IAllocator& allocator) : m_allocator(&allocator), m_tabs(allocator) {}

        struct Opened
        {
            i32 index = -1;     // the tab it is in, -1 when it did not open
            bool added = false; // a new tab (false: it was open already)
            OpenModel::Outcome outcome = OpenModel::Outcome::NotFound;
        };

        /// Opens `path` into a new tab at the end and brings it to the front; a file open already
        /// is brought to the front instead. A file that does not open adds no tab.
        /// `looseTextures` as OpenModel::SetLooseTextures.
        Opened Open(StringView path, StringView cacheRoot, foundation::rhi::Device* device, bool looseTextures = true)
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
            result.outcome = open->Open(path, cacheRoot, device);
            if (!open->IsOpen())
                return result;
            ModelTab tab;
            tab.path = String(path);
            tab.title = FileTitle(path);
            tab.open = foundation::core::Move(open);
            m_tabs.PushBack(foundation::core::Move(tab));
            m_active = static_cast<i32>(m_tabs.Size()) - 1;
            result.index = m_active;
            result.added = true;
            return result;
        }

        /// Closes a tab; the one after it comes to the front (else the one before), or none.
        /// Returns the tab in front after it.
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

        /// A tab dragged from one place to another; the one in front stays in front.
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

        /// The tab a file is open in, -1 when none.
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

    /// The side panel's numbers for a model, in the order it shows them.
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
