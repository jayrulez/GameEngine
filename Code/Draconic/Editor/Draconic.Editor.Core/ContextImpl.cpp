// Draconic::EditorCore - :context partition.
//
// EditorContext: the central service object handed to every page/panel/plugin (Sedulous's
// EditorContext, Traktor's IEditor). Holds the open project, the registries, the open pages +
// active page, the global asset selection, and the status sink. Per-subsystem editor modules
// (draconic.<sys>.editor) register their factories here from RegisterEditor(EditorContext&);
// the statically-assembled editor executable calls those entry points (design doc §3.1).

module;
#include "Draconic.Foundation/Prelude.h"

module draconic.editor.core;

import draconic.foundation;
import draconic.resource;
import :importer;
import draconic.content;
import :command;
import :selection;
import :page;
import :project;

using namespace draconic::foundation;

namespace draconic::editor
{
    void EditorContext::RequestCook(bool rebuild)
    {
        if (OnCookRequested)
        {
            OnCookRequested(rebuild);
        }
    }

    void EditorContext::Notify(NoticeKind kind, StringView message)
    {
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

    void EditorContext::SetResources(draconic::resource::ResourceManager* resources) noexcept
    {
        m_resources = resources;
    }

    draconic::resource::ResourceManager* EditorContext::Resources() const noexcept
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

    Span<const byte> EditorContext::ClipboardData(StringView kind) const noexcept
    {
        return (m_clipboardKind == kind) ? Span<const byte>{m_clipboard.Data(), m_clipboard.Size()}
                                         : Span<const byte>{};
    }

    void EditorContext::RegisterCreator(AssetCreator creator)
    {
        if (creator.create)
        {
            m_creators.PushBack(Move(creator));
        }
    }

    Span<const EditorContext::AssetCreator> EditorContext::Creators() const noexcept
    {
        return Span<const AssetCreator>{m_creators.Data(), m_creators.Size()};
    }

    EditorPage* EditorContext::OpenPage(draconic::content::Instance& instance)
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

        UniquePtr<EditorPage> page = factory->CreatePage(*this, instance);
        if (!page)
        {
            return nullptr;
        }
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

    void EditorContext::SetActivePage(EditorPage* page)
    {
        if (m_activePage == page)
        {
            return;
        }
        m_activePage = page;
        NotifyPagesChanged();
    }

    bool EditorContext::CanUndo() const
    {
        return m_activePage != nullptr && m_activePage->Commands().CanUndo();
    }

    bool EditorContext::CanRedo() const
    {
        return m_activePage != nullptr && m_activePage->Commands().CanRedo();
    }

    void EditorContext::Undo()
    {
        if (m_activePage != nullptr)
        {
            m_activePage->Commands().Undo();
        }
    }

    void EditorContext::Redo()
    {
        if (m_activePage != nullptr)
        {
            m_activePage->Commands().Redo();
        }
    }

    Selection<const draconic::content::Instance*>& EditorContext::AssetSelection() noexcept
    {
        return m_assetSelection;
    }

    void EditorContext::AddImportListener(
        Function<void(draconic::content::Instance&, const ImportOptions*)> listener)
    {
        m_importListeners.PushBack(Move(listener));
    }

    void EditorContext::NotifyImported(draconic::content::Instance& instance,
                                       const ImportOptions* options)
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
