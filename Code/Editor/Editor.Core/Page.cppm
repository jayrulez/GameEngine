// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Core - :page partition.
//
// The document model: each open asset is an EditorPage - a dock
// tab with its OWN command stack (Sedulous/Traktor per-page undo), dirty tracking, and Save.
// This is the HEADLESS half: concrete pages live in UI-side modules and add their widget tree on top.
//
// Pages are created by IEditorPageFactory, dispatched by the instance's primary-object type
// with nearest-type matching along the base chain (Traktor's type_difference contest), via
// EditorPageRegistry.

module;
#include "Core/Prelude.h"
#include <type_traits>

export module editor.core:page;

import foundation.core;
import foundation.content;
import :command;

using namespace foundation::core;

export namespace editor
{
    class EditorContext; // defined in :context (pages receive it on creation)

    /// An interface a page implements and PUBLISHES for other parts of the editor to act
    /// through - a scene page's ISceneEditorPage (its edit context, its simulation control).
    /// The base is what keeps a page's service table typed: a service IS-A IPageService, and
    /// the TypeId it is published under says which one, so no erased pointer and no RTTI on
    /// pages is needed. Pages have no runtime type of their own; this is how another module
    /// learns what a page can do.
    class IPageService
    {
    protected:
        ~IPageService() = default;
    };

    /// A page that plays something back: a clip, a cue, an effect, a graph preview. The playback
    /// actions (playback.play, playback.stop, playback.restart) drive any page that publishes
    /// this, so every page's transport is the same three toolbar buttons with the same meaning:
    /// - Play is a toggle, checked while playing: it starts, pauses and resumes;
    /// - Stop stops and rewinds to the start;
    /// - Restart plays from the start.
    class IPlaybackPage : public IPageService
    {
    public:
        /// Whether there is anything to play: a clip loaded, an effect built.
        [[nodiscard]] virtual bool CanPlay() const = 0;
        [[nodiscard]] virtual bool IsPlaying() const = 0;
        /// Starts from the start when stopped, or resumes where it paused.
        virtual void Play() = 0;
        virtual void Pause() = 0;
        /// Stops and rewinds to the start.
        virtual void Stop() = 0;
        /// Plays from the start.
        virtual void Restart() = 0;

    protected:
        ~IPlaybackPage() = default;
    };

    /// A page that runs a game (a Game tab). The editor stops every one before it unloads code a
    /// run executes (a native module reload); a run's own exit request stops only its own tab.
    class IGameRunPage : public IPageService
    {
    public:
        /// Stops the run, if one is going; the tab stays open.
        virtual void StopRun() = 0;

    protected:
        ~IGameRunPage() = default;
    };

    // One open document. Owns its command stack; the context routes Edit>Undo/Redo to the
    // active page's stack. `Commands().OnChanged` is wired by the base to mark the page dirty.
    class EditorPage
    {
    public:
        // The allocator (required - the page factory forwards the EditorContext's
        // tagged "Editor" root) backs everything this page creates.
        explicit EditorPage(IAllocator& allocator) : m_allocator(&allocator)
        {
            m_commands.OnChanged = [this]() { m_dirty = true; };
        }

        [[nodiscard]] IAllocator& Allocator() const noexcept { return *m_allocator; }
        virtual ~EditorPage() = default;
        EditorPage(const EditorPage&) = delete;
        EditorPage& operator=(const EditorPage&) = delete;

        /// Tab title (typically the instance name).
        [[nodiscard]] virtual StringView Title() const = 0;

        /// Persist the edited object(s) back to the source database. Clears dirty on success.
        [[nodiscard]] virtual Status Save() = 0;

        [[nodiscard]] bool IsDirty() const noexcept { return m_dirty; }
        void MarkDirty() noexcept { m_dirty = true; }
        void ClearDirty() noexcept { m_dirty = false; }

        /// Revert all unsaved edits to the last-saved state (the toolbar's "Discard Changes").
        /// Default: undo the whole command stack, then clear it and the dirty flag - correct for
        /// pages that route every edit through commands. Pages that mutate state directly (or cache
        /// loaded content) override to reload from the last-saved snapshot / the source DB.
        virtual void DiscardChanges()
        {
            while (m_commands.CanUndo())
            {
                m_commands.Undo();
            }
            m_commands.Clear();
            ClearDirty();
        }

        [[nodiscard]] EditorCommandStack& Commands() noexcept { return m_commands; }

        /// Unsaved work the page holds that is not its asset (a clip its animation panel edits,
        /// with a history and a Save of its own; any panel of its like): each named for the prompts
        /// ("The clip 'Bounce'"), none when all is saved. The page's unsaved mark never covers it;
        /// closing the page and leaving the editor name each beside the page's own changes.
        virtual void ListUnsavedBesides(Array<String>& out) const { (void)out; }
        /// Saves all of that work; what the saves said (the first that failed).
        virtual Status SaveBesides() { return Status{}; }
        /// The page's changes or that work unsaved: what the close and the exit ask about.
        [[nodiscard]] bool HasUnsavedWork() const
        {
            if (IsDirty())
                return true;
            Array<String> besides;
            ListUnsavedBesides(besides);
            return !besides.IsEmpty();
        }

        /// The asset this page edits changed OUTSIDE the page (apply-to-prefab, re-import).
        /// Default no-op; pages that cache loaded content override to refresh themselves.
        virtual void OnAssetExternallyModified() {}

        /// Rebind this page to a DIFFERENT source instance (Save As): the caller created
        /// `instance` and invokes Save() next, so the page's current content lands there.
        /// Pages that cache the asset's name override (calling the base) to refresh it.
        virtual void OnSavedAs(foundation::content::Instance& instance)
        {
            m_instanceId = instance.Id();
        }

        /// The source-DB instance this page edits (zero Guid for instance-less pages).
        [[nodiscard]] const Guid& InstanceId() const noexcept { return m_instanceId; }
        void SetInstanceId(const Guid& id) noexcept { m_instanceId = id; }

        // === Services: the interfaces this page publishes (the SubsystemManager / Scene
        // pattern - a TypeId-keyed table - scoped to one page) ===

        /// Publish an interface this page implements, from its constructor. The table dies
        /// with the page, so nothing is ever unpublished.
        template <typename T>
            requires std::is_base_of_v<IPageService, T>
        void Provide(T& service)
        {
            m_services.InsertOrAssign(TypeOf<T>().id, static_cast<IPageService*>(&service));
        }

        /// The interface this page publishes as T, or null: this is not that kind of page.
        template <typename T>
            requires std::is_base_of_v<IPageService, T>
        [[nodiscard]] T* Service() const noexcept
        {
            IPageService* const* found = m_services.Find(TypeOf<T>().id);
            return found != nullptr ? static_cast<T*>(*found) : nullptr;
        }

    protected:
        IAllocator* m_allocator;
        EditorCommandStack m_commands;
        Guid m_instanceId{};
        bool m_dirty = false;

    private:
        HashMap<TypeId, IPageService*> m_services; // keyed like the SubsystemManager's subsystems
    };

    // Creates pages for one primary-object type (and, via nearest-type dispatch, its
    // subclasses unless a more specific factory is registered).
    class IEditorPageFactory
    {
    public:
        virtual ~IEditorPageFactory() = default;

        /// The primary-object type this factory's pages edit.
        [[nodiscard]] virtual const TypeInfo* PrimaryType() const = 0;

        /// Create a page editing `instance`. Null on failure (unreadable object etc.).
        [[nodiscard]] virtual UniquePtr<EditorPage>
        CreatePage(EditorContext& context, foundation::content::Instance& instance) = 0;
    };

    // Factory registry with Traktor-style nearest-type dispatch: the factory whose
    // PrimaryType() is closest along the asset type's base chain wins.
    class EditorPageRegistry
    {
    public:
        EditorPageRegistry() = default;
        EditorPageRegistry(const EditorPageRegistry&) = delete;
        EditorPageRegistry& operator=(const EditorPageRegistry&) = delete;

        void Register(UniquePtr<IEditorPageFactory> factory)
        {
            if (factory && factory->PrimaryType() != nullptr)
            {
                m_factories.PushBack(Move(factory));
            }
        }

        [[nodiscard]] IEditorPageFactory* FindFactory(const TypeInfo& type) const
        {
            IEditorPageFactory* best = nullptr;
            u32 bestDistance = 0;
            for (const UniquePtr<IEditorPageFactory>& factory : m_factories)
            {
                u32 distance = 0;
                for (const TypeInfo* t = &type; t != nullptr; t = t->base, ++distance)
                {
                    if (t == factory->PrimaryType())
                    {
                        if (best == nullptr || distance < bestDistance)
                        {
                            best = factory.Get();
                            bestDistance = distance;
                        }
                        break;
                    }
                }
            }
            return best;
        }

        [[nodiscard]] usize Size() const noexcept { return m_factories.Size(); }

    private:
        Array<UniquePtr<IEditorPageFactory>> m_factories;
    };
}
