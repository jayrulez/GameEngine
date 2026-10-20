// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI.Docking - :controller partition: DockController, a layout and the panels it places (D3 of
// Specs/gui-lab-evolution-3.md).
//
// Panels are registered by id with a description: the title and icon their tab shows, a factory
// for their content, what closing them does, whether they can be closed, and where they go when a
// restored layout does not place them. The controller owns each panel's content once made, so a
// panel moved to another stack or window, or hidden, keeps it (foundation.ui's port destroyed a
// panel that had no id on close and crashed reaching it after). Closing a panel follows its policy:
// Hide keeps its content and its place, Rebuild lets its content go and keeps its place (the
// factory makes it again when shown), Destroy lets both go.
//
// The layout changes through the controller, which tells every DockSpace showing it to show it
// again. What the user already did in the widgets (a separator dragged, a tab chosen or dragged to
// another place in its strip) is noted into the layout quietly: the widgets already show it.

module;
#include "Core/Prelude.h"

export module experimental.gui.docking:controller;

import foundation.core;
import experimental.gui; // Node
import :layout;

using namespace foundation::core;
namespace core = foundation::core;

export namespace experimental::gui
{
    enum class DockClosePolicy : u8
    {
        Hide,    ///< content kept, place kept
        Rebuild, ///< content let go, place kept: made again when shown
        Destroy, ///< content and place let go
    };

    struct DockPanelDescription
    {
        String Id;
        String Title; ///< its tab's text (the id when empty)
        String Icon;  ///< its tab's icon, by path
        Function<RefPtr<Node>()> Factory;
        DockClosePolicy Close = DockClosePolicy::Hide;
        bool Closable = true;
        DockDefault Default; ///< where it goes when a restored layout does not place it (its Panel: the id)
    };

    /// What shows a controller's layout: told to show it again when it changed.
    class IDockView
    {
    public:
        virtual ~IDockView() = default;
        virtual void ShowLayout() = 0;
    };

    class DockController
    {
    public:
        explicit DockController(IAllocator& allocator) noexcept : m_panels(allocator), m_contents(allocator), m_views(allocator) {}
        DockController(const DockController&) = delete;
        DockController& operator=(const DockController&) = delete;

        /// A panel the layout may place; false when its id is empty or taken.
        bool Register(DockPanelDescription description)
        {
            if (description.Id.IsEmpty() || Describe(description.Id.AsView()) != nullptr)
                return false;
            description.Default.Panel = description.Id;
            m_panels.PushBack(core::Move(description));
            ShowEverywhere(); // a placeholder for it may show it now
            return true;
        }
        [[nodiscard]] const DockPanelDescription* Describe(core::StringView panel) const noexcept
        {
            for (const DockPanelDescription& description : m_panels)
                if (description.Id.AsView() == panel)
                    return &description;
            return nullptr;
        }

        [[nodiscard]] const DockLayout& GetLayout() const noexcept { return m_layout; }
        /// Changes the layout through `edit`; when it did something, every view shows it.
        DockOutcome Edit(const Function<DockOutcome(DockLayout&)>& edit)
        {
            DockOutcome outcome = edit(m_layout);
            if (outcome.Ok())
                ShowEverywhere();
            return outcome;
        }
        /// Closes a docked panel as its description says.
        DockOutcome Close(core::StringView panel)
        {
            const DockPanelDescription* description = Describe(panel);
            if (description != nullptr && !description->Closable)
                return DockOutcome::Refuse(u8"it cannot be closed");
            DockOutcome outcome = m_layout.Close(panel);
            if (!outcome.Ok())
                return outcome;
            const DockClosePolicy policy = description != nullptr ? description->Close : DockClosePolicy::Hide;
            if (policy != DockClosePolicy::Hide)
                ForgetContent(panel);
            if (policy == DockClosePolicy::Destroy)
                (void)m_layout.Forget(panel);
            ShowEverywhere();
            return outcome;
        }
        /// Shows a registered panel: where it was closed, else at its default place.
        DockOutcome Show(core::StringView panel)
        {
            if (m_layout.StackOf(panel) != kNoDock)
                return DockOutcome::Refuse(u8"it is docked");
            if (m_layout.FindHidden(panel) != nullptr)
                return Edit([panel](DockLayout& layout) { return layout.Show(panel); });
            const DockPanelDescription* description = Describe(panel);
            if (description == nullptr)
                return DockOutcome::Refuse(u8"it is not registered");
            const DockDefault where = description->Default;
            return Edit(
                [panel, where](DockLayout& layout)
                {
                    return where.Central ? layout.InsertTab(panel, layout.CentralStack())
                                         : layout.InsertAtEdge(panel, layout.MainSurface(), where.Side, where.Share);
                });
        }
        /// A restored layout taken (DockLayout::Adopt with the registered panels' default places).
        DockOutcome Adopt(DockLayout read, Span<const Rectangle> screens = {})
        {
            Array<DockDefault> defaults;
            for (const DockPanelDescription& description : m_panels)
                defaults.PushBack(description.Default);
            DockOutcome outcome = m_layout.Adopt(core::Move(read), Span<const DockDefault>(defaults.Data(), defaults.Size()), screens);
            if (outcome.Ok())
                ShowEverywhere();
            return outcome;
        }

        /// A registered panel's content: made by its factory the first time it is asked for, kept
        /// after; null for a panel not registered (or one whose factory makes none).
        RefPtr<Node> Content(core::StringView panel)
        {
            for (const Made& made : m_contents)
                if (made.Panel.AsView() == panel)
                    return made.Content;
            const DockPanelDescription* description = Describe(panel);
            if (description == nullptr || !description->Factory)
                return {};
            RefPtr<Node> content = description->Factory();
            if (content)
                m_contents.PushBack(Made{String(panel), content});
            return content;
        }
        /// Whether a panel's content is made (and kept).
        [[nodiscard]] bool HasContent(core::StringView panel) const noexcept
        {
            for (const Made& made : m_contents)
                if (made.Panel.AsView() == panel)
                    return true;
            return false;
        }

        // === What the widgets already show, noted quietly ===
        void NoteWeights(DockNodeId split, Span<const f32> weights) { (void)m_layout.SetWeights(split, weights); }
        void NoteActive(core::StringView panel) { (void)m_layout.Activate(panel); }
        void NoteTabPlace(core::StringView panel, DockNodeId stack, i32 index) { (void)m_layout.MoveToTab(panel, stack, index); }

        // === The views ===
        void AddView(IDockView& view)
        {
            for (IDockView* known : m_views)
                if (known == &view)
                    return;
            m_views.PushBack(&view);
        }
        void RemoveView(IDockView& view)
        {
            for (usize i = 0; i < m_views.Size(); ++i)
                if (m_views[i] == &view)
                {
                    m_views.RemoveAt(i);
                    return;
                }
        }

    private:
        struct Made
        {
            String Panel;
            RefPtr<Node> Content;
        };
        void ForgetContent(core::StringView panel)
        {
            for (usize i = 0; i < m_contents.Size(); ++i)
                if (m_contents[i].Panel.AsView() == panel)
                {
                    m_contents[i].Content->RemoveFromParent();
                    m_contents.RemoveAt(i);
                    return;
                }
        }
        void ShowEverywhere()
        {
            for (IDockView* view : m_views)
                view->ShowLayout();
        }

        DockLayout m_layout;
        Array<DockPanelDescription> m_panels;
        Array<Made> m_contents;
        Array<IDockView*> m_views;
    };
}
