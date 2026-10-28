// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Core - :context partition.
//
// EditorContext: the central service object handed to every page/panel/plugin (Sedulous's
// EditorContext, Traktor's IEditor). Holds the open project, the registries, the open pages +
// active page, the global asset selection, and the status sink. Per-subsystem editor modules
// register their factories here from RegisterEditor(EditorContext&);
// the statically-assembled editor executable calls those entry points.

module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"

module editor.core;

import foundation.core;
import foundation.gui;
import foundation.resource;
import pipeline.importer;
import foundation.content;
import :command;
import :selection;
import :page;
import editor.project;

using namespace foundation::core;


namespace editor
{
    void EditorContext::RequestCook(bool rebuild)
    {
        if (OnCookRequested)
        {
            OnCookRequested(rebuild);
        }
    }

    void EditorContext::RegisterAssetEdit(const Guid& assetId,
                                          Function<Status(foundation::content::ContentDatabase&)> persist)
    {
        if (assetId.IsNil() || !persist)
        {
            return; // in-memory / unresolved assets have no source to persist to (edit-live-only)
        }
        for (PendingAssetEdit& e : m_pendingAssetEdits)
        {
            if (e.id == assetId)
            {
                e.persist = Move(persist); // last edit for this asset wins
                return;
            }
        }
        m_pendingAssetEdits.PushBack(PendingAssetEdit{assetId, Move(persist)});
    }

    Status EditorContext::DrainAssetEdits(foundation::content::ContentDatabase& db)
    {
        if (m_pendingAssetEdits.IsEmpty())
        {
            return Status{};
        }
        Array<PendingAssetEdit> pending = Move(m_pendingAssetEdits);
        m_pendingAssetEdits.Clear();
        Status result{};
        bool anyOk = false;
        for (PendingAssetEdit& e : pending)
        {
            const Status s = e.persist ? e.persist(db) : Status{};
            if (s.IsOk())
            {
                anyOk = true;
            }
            else if (result.IsOk())
            {
                result = s; // report the first failure (all others still attempted)
            }
        }
        if (anyOk)
        {
            RequestCook(false); // hot-swap the re-persisted products
        }
        return result;
    }

    StringView EditorContext::AssetNameFor(const Guid& id) const
    {
        if (id.IsNil())
        {
            return u8"(none)";
        }
        if (m_project != nullptr)
        {
            if (foundation::content::Instance* instance = m_project->SourceDb().GetInstance(id))
            {
                return instance->Name();
            }
        }
        return u8"(missing)";
    }

    void EditorContext::Notify(NoticeKind kind, StringView message)
    {
        // Errors and warnings ALWAYS reach the console log too: a toast is transient,
        // and a failure a user reports from memory ("an error flashed") must be
        // reconstructable from the log (user ruling 2026-09-03).
        if (kind == NoticeKind::Error)
        {
            LOG_ERROR(u8"Editor", u8"{}", message);
        }
        else if (kind == NoticeKind::Warning)
        {
            LOG_WARNING(u8"Editor", u8"{}", message);
        }
        if (OnNotice)
        {
            OnNotice(kind, message);
        }
        else
        {
            SetStatus(message);
        }
    }

    void EditorContext::SetProject(EditorProject* project)
    {
        m_project = project;
        m_assetSelection.Clear();
    }

    void EditorContext::SetResources(foundation::resource::ResourceManager* resources) noexcept
    {
        m_resources = resources;
    }

    foundation::resource::ResourceManager* EditorContext::Resources() const noexcept
    {
        return m_resources;
    }

    bool EditorContext::IsFavorite(const Guid& id) const
    {
        for (const Guid& f : m_favorites)
        {
            if (f == id)
            {
                return true;
            }
        }
        return false;
    }

    void EditorContext::ToggleFavorite(const Guid& id)
    {
        for (usize i = 0; i < m_favorites.Size(); ++i)
        {
            if (m_favorites[i] == id)
            {
                m_favorites.RemoveAt(i);
                if (OnFavoritesChanged)
                {
                    OnFavoritesChanged();
                }
                return;
            }
        }
        m_favorites.PushBack(id);
        if (OnFavoritesChanged)
        {
            OnFavoritesChanged();
        }
    }

    Span<const Guid> EditorContext::Favorites() const noexcept
    {
        return Span<const Guid>{m_favorites.Data(), m_favorites.Size()};
    }

    void EditorContext::SetClipboard(StringView kind, Array<byte> data)
    {
        m_clipboardKind = String(kind);
        m_clipboard = Move(data);
    }

    void EditorContext::CopyToEditorClipboard(StringView kind, Array<byte> data, StringView what)
    {
        SetClipboard(kind, Move(data));
        Notify(NoticeKind::Success, Format(u8"Copied {}", what).AsView());
    }

    bool EditorContext::CopyText(foundation::gui::IClipboard* clipboard, StringView text,
                                 StringView what)
    {
        if (clipboard == nullptr)
        {
            Notify(NoticeKind::Warning, Format(u8"Could not copy {} to the clipboard", what).AsView());
            return false;
        }
        clipboard->SetText(text);
        Notify(NoticeKind::Success, Format(u8"Copied {}", what).AsView());
        return true;
    }

    Span<const byte> EditorContext::ClipboardData(StringView kind) const noexcept
    {
        return (m_clipboardKind == kind) ? Span<const byte>{m_clipboard.Data(), m_clipboard.Size()}
                                         : Span<const byte>{};
    }

    EditorPage* EditorContext::OpenPage(foundation::content::Instance& instance)
    {
        for (const UniquePtr<EditorPage>& page : m_pages)
        {
            if (page->InstanceId() == instance.Id())
            {
                SetActivePage(page.Get());
                return page.Get();
            }
        }

        const TypeInfo* type = GlobalTypeRegistry().FindByName(
            reinterpret_cast<const char*>(String(instance.TypeNamespace()).CStr()),
            reinterpret_cast<const char*>(String(instance.TypeName()).CStr()));
        if (type == nullptr)
        {
            return nullptr;
        }

        IEditorPageFactory* factory = m_pageRegistry.FindFactory(*type);
        if (factory == nullptr)
        {
            return nullptr;
        }

        const Stopwatch openClock = Stopwatch::StartNew();
        UniquePtr<EditorPage> page = factory->CreatePage(*this, instance);
        if (!page)
        {
            return nullptr;
        }
        // The number that must stay small: a page's constructor runs on the UI thread (a
        // Bistro-sized prefab: the document parse; its resources bind async and pop in).
        LOG_INFO(u8"Editor", u8"page '{}' ({}) opened in {} ms", instance.Name(),
                 instance.TypeName(), static_cast<i64>(openClock.Elapsed().AsMilliseconds()));
        page->SetInstanceId(instance.Id());

        EditorPage* raw = page.Get();
        m_pages.PushBack(Move(page));
        m_activePage = raw;
        NotifyPagesChanged();
        return raw;
    }

    EditorPage* EditorContext::AdoptPage(UniquePtr<EditorPage> page)
    {
        if (!page)
        {
            return nullptr;
        }
        EditorPage* raw = page.Get();
        m_pages.PushBack(Move(page));
        m_activePage = raw;
        NotifyPagesChanged();
        return raw;
    }

    void EditorContext::ClosePage(EditorPage* page)
    {
        for (usize i = 0; i < m_pages.Size(); ++i)
        {
            if (m_pages[i].Get() == page)
            {
                if (m_activePage == page)
                {
                    m_activePage = m_pages.Size() > 1
                                       ? m_pages[i + 1 < m_pages.Size() ? i + 1 : i - 1].Get()
                                       : nullptr;
                }
                m_pages.RemoveAt(i);
                NotifyPagesChanged();
                return;
            }
        }
    }

    Span<const UniquePtr<EditorPage>> EditorContext::OpenPages() const noexcept
    {
        return Span<const UniquePtr<EditorPage>>{m_pages.Data(), m_pages.Size()};
    }

    usize EditorContext::NotifyAssetExternallyModified(const Guid& assetId)
    {
        usize told = 0;
        for (const UniquePtr<EditorPage>& page : m_pages)
        {
            if (page->InstanceId() == assetId)
            {
                page->OnAssetExternallyModified();
                ++told;
            }
        }
        return told;
    }

    void EditorContext::SetActivePage(EditorPage* page)
    {
        if (m_activePage == page)
        {
            return;
        }
        m_activePage = page;
        NotifyPagesChanged();
    }

    void EditorContext::RevealPage(EditorPage* page)
    {
        SetActivePage(page);
        if (page != nullptr && OnRevealPage)
        {
            OnRevealPage(page);
        }
    }

    Selection<const foundation::content::Instance*>& EditorContext::AssetSelection() noexcept
    {
        return m_assetSelection;
    }

    void EditorContext::AddImportListener(
        Function<void(foundation::content::Instance&, const pipeline::ImportOptions*)> listener)
    {
        m_importListeners.PushBack(Move(listener));
    }

    void EditorContext::NotifyImported(foundation::content::Instance& instance,
                                       const pipeline::ImportOptions* options)
    {
        for (const auto& listener : m_importListeners)
        {
            listener(instance, options);
        }
    }

    void EditorContext::ToggleBreakpoint(StringView file, i32 line)
    {
        for (usize i = 0; i < m_breakpoints.Size(); ++i)
        {
            if (m_breakpoints[i].line == line && m_breakpoints[i].file.AsView() == file)
            {
                m_breakpoints.RemoveAt(i);
                if (OnBreakpointsChanged)
                {
                    OnBreakpointsChanged();
                }
                return;
            }
        }
        m_breakpoints.PushBack(ScriptBreakpoint{String(file), line});
        if (OnBreakpointsChanged)
        {
            OnBreakpointsChanged();
        }
    }

    bool EditorContext::HasBreakpoint(StringView file, i32 line) const
    {
        for (const ScriptBreakpoint& breakpoint : m_breakpoints)
        {
            if (breakpoint.line == line && breakpoint.file.AsView() == file)
            {
                return true;
            }
        }
        return false;
    }

    Span<const EditorContext::ScriptBreakpoint> EditorContext::Breakpoints() const noexcept
    {
        return Span<const ScriptBreakpoint>{m_breakpoints.Data(), m_breakpoints.Size()};
    }

    void EditorContext::SetStatus(StringView text)
    {
        if (OnStatus)
        {
            OnStatus(text);
        }
    }

    void EditorContext::NotifyPagesChanged()
    {
        if (OnPagesChanged)
        {
            OnPagesChanged();
        }
    }
}
