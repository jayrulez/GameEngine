// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Scene - :scene_tools partition, its body. See SceneTools.cppm.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

module editor.scene;

import foundation.core;
import foundation.gui;
import foundation.gui.toolkit;
import editor.core;
import editor.app;
import editor.viewporttools;
import :scene_page_interface;
import :scene_tools;

using namespace foundation::core;
namespace gui = foundation::gui;

namespace editor
{
    namespace
    {
        // The page's tool `id`, when the subject is a scene page that has it.
        IViewportTool* ToolOf(EditorPage* page, StringView id, ViewportToolManager** manager = nullptr)
        {
            ISceneEditorPage* scene = ServiceOf<ISceneEditorPage>(page);
            ViewportToolManager* tools = scene != nullptr ? scene->ViewportTools() : nullptr;
            if (manager != nullptr)
                *manager = tools;
            return tools != nullptr ? tools->FindById(id) : nullptr;
        }
    }

    // === ToolPanelHead ===

    void ToolPanelHead::OnMouseDown(const gui::MouseEvent& event)
    {
        if (event.Button != gui::MouseButton::Left)
            return;
        event.Handled = true;
        if (event.ClickCount == 2)
        {
            if (OnReset)
                OnReset();
            return;
        }
        m_dragging = true;
        m_from = event.Position;
        if (gui::EventDispatcher* dispatcher = GetEventDispatcher())
            dispatcher->CapturePointer(*this);
        if (OnDragStart)
            OnDragStart();
    }

    void ToolPanelHead::OnMouseMove(const gui::MouseEvent& event)
    {
        if (!m_dragging)
            return;
        event.Handled = true;
        if (OnDrag)
            OnDrag(event.Position - m_from);
    }

    void ToolPanelHead::OnMouseUp(const gui::MouseEvent& event)
    {
        if (!m_dragging || event.Button != gui::MouseButton::Left)
            return;
        event.Handled = true;
        m_dragging = false;
        if (gui::EventDispatcher* dispatcher = GetEventDispatcher())
            dispatcher->ReleasePointer(*this);
        if (OnDragEnd)
            OnDragEnd();
    }

    // === SceneTools ===

    SceneTools::SceneTools(IAllocator& allocator, Services services)
        : m_allocator(allocator), m_services(Move(services)), m_buttons(allocator), m_categories(allocator)
    {
        RegisterActions();
        BuildBar();
        BuildChrome();
        if (m_services.tools != nullptr)
        {
            SceneTools* self = this;
            m_host = MakeUnique<ViewportToolPanelHost>(m_allocator, *m_services.tools, ViewportToolPanelRegistry::Get(), m_services.hostContext,
                                                       [self](gui::Node* panel, IViewportTool& tool) { self->Mount(panel, tool); },
                                                       [self]() { self->Clear(); });
        }
    }

    SceneTools::~SceneTools()
    {
        if (m_chrome && m_chrome->GetParent() != nullptr)
            m_chrome->RemoveFromParent();
    }

    void SceneTools::RegisterActions()
    {
        if (m_services.context == nullptr || m_services.tools == nullptr)
            return;
        EditorActionRegistry& actions = m_services.context->Actions();
        EditorContext* context = m_services.context;
        // Every tool but the default (Select is the bar's own choice); the first page to have a
        // tool declares its action. The bindings name the tool by id, so they serve every page.
        for (usize i = 1; i < m_services.tools->Count(); ++i)
        {
            IViewportTool* tool = m_services.tools->ToolAt(i);
            const String action = ActionOf(tool->Id());
            if (actions.Find(action.AsView()) != nullptr)
                continue;
            EditorActionDeclaration d;
            d.id = action;
            d.label = String(tool->DisplayName());
            d.description = Format(u8"Use the {} tool in the scene view (again: back to Select)", tool->DisplayName());
            d.menuPath = tool->Category().IsEmpty() ? Format(u8"Scene/Tools/{}", tool->DisplayName())
                                                    : Format(u8"Scene/Tools/{}/{}", tool->Category(), tool->DisplayName());
            d.menuOrder = 400 + static_cast<i32>(i);
            d.icon = String(tool->Icon());
            d.kind = EditorActionKind::Toggle;
            const String id(tool->Id());
            d.enabled = [id](EditorPage* page)
            {
                ISceneEditorPage* scene = ServiceOf<ISceneEditorPage>(page);
                IViewportTool* found = ToolOf(page, id.AsView());
                return found != nullptr && !scene->IsSimulating() && found->IsAvailable();
            };
            d.disabledReason = [id](EditorPage* page) -> String
            {
                ISceneEditorPage* scene = ServiceOf<ISceneEditorPage>(page);
                IViewportTool* found = ToolOf(page, id.AsView());
                if (found == nullptr)
                    return String{};
                if (scene->IsSimulating())
                    return String(u8"Stop the simulation to edit the scene.");
                return found->IsAvailable() ? String{} : String(found->UnavailableReason());
            };
            d.checked = [id](EditorPage* page)
            {
                ViewportToolManager* manager = nullptr;
                IViewportTool* found = ToolOf(page, id.AsView(), &manager);
                return found != nullptr && manager->ActiveTool() == found;
            };
            d.execute = [id, context](EditorPage* page)
            {
                ViewportToolManager* manager = nullptr;
                IViewportTool* found = ToolOf(page, id.AsView(), &manager);
                if (found == nullptr)
                    return;
                if (manager->ActiveTool() == found)
                    manager->ActivateDefault();
                else if (!manager->ActivateById(id.AsView()))
                    context->Notify(NoticeKind::Warning, found->UnavailableReason()); // reported, rather than silently snapping back
            };
            (void)actions.Register(Move(d));
        }
    }

    void SceneTools::BuildBar()
    {
        gui::Node* end = m_services.barEnd;
        gui::Node* bar = end != nullptr ? end->GetParent() : nullptr;
        if (bar == nullptr || m_services.tools == nullptr || m_services.context == nullptr || m_services.page == nullptr)
            return;
        m_bar = MakeUnique<app::PageActionBar>(m_allocator, m_allocator, m_services.context->Actions(), *m_services.page);
        Array<ViewportToolGroup> groups(m_allocator);
        GroupViewportTools(*m_services.tools, groups);
        const auto insert = [&](gui::ButtonBase& button)
        {
            usize at = 0;
            while (at < bar->ChildCount() && bar->GetChildAt(at) != end)
                ++at;
            (void)bar->AddChildAt(&button, at);
        };
        for (ViewportToolGroup& group : groups)
        {
            if (group.category.IsEmpty() || group.toolIds.Size() < 2)
            {
                for (const String& id : group.toolIds)
                {
                    IViewportTool* tool = m_services.tools->FindById(id.AsView());
                    auto button = MakeRef<gui::ToolButton>(m_allocator);
                    button->SetToggle(true);
                    if (!tool->Icon().IsEmpty())
                        button->SetIconSource(String(tool->Icon()));
                    else
                        button->SetShowsText(true);
                    insert(*button);
                    m_bar->Bind(button.Get(), ActionOf(id.AsView()).AsView());
                    m_buttons.PushBack(button);
                }
                continue;
            }
            // A category: one menu with its first tool's icon, each tool an item.
            auto menuButton = MakeRef<gui::ToolMenuButton>(m_allocator);
            menuButton->SetToggle(true); // lit while one of its tools is on
            IViewportTool* first = m_services.tools->FindById(group.toolIds[0].AsView());
            if (first != nullptr && !first->Icon().IsEmpty())
                menuButton->SetIconSource(String(first->Icon()));
            else
            {
                menuButton->SetText(group.category.AsView());
                menuButton->SetShowsText(true);
            }
            menuButton->SetTooltip(Format(u8"{} tools", group.category).AsView());
            Category category;
            category.name = group.category;
            category.button = menuButton.Get();
            EditorActionRegistry* actions = &m_services.context->Actions();
            EditorPage* page = m_services.page;
            for (const String& id : group.toolIds)
            {
                IViewportTool* tool = m_services.tools->FindById(id.AsView());
                const String action = ActionOf(id.AsView());
                gui::MenuItem* item = menuButton->GetMenu().AddCheckItem(tool->DisplayName(), false, [actions, page, action](bool)
                                                                         { (void)actions->Execute(action.AsView(), page); });
                category.ids.PushBack(id);
                category.items.PushBack(item);
            }
            insert(*menuButton);
            m_buttons.PushBack(menuButton);
            m_categories.PushBack(Move(category));
        }
    }

    void SceneTools::BuildChrome()
    {
        m_chrome = MakeRef<gui::Panel>(m_allocator);
        m_chrome->AddClass(u8"hud-panel");
        m_chrome->AddClass(u8"tool-panel");
        m_head = MakeRef<ToolPanelHead>(m_allocator);
        auto icon = MakeRef<gui::Image>(m_allocator);
        icon->AddClass(u8"row-icon");
        m_icon = icon.Get();
        m_head->AddChild(icon.Get());
        auto title = MakeRef<gui::Label>(m_allocator);
        title->AddClass(u8"hud-panel-title");
        m_title = title.Get();
        m_head->AddChild(title.Get());
        auto spacer = MakeRef<gui::Panel>(m_allocator);
        spacer->EditLayoutStyle([](gui::LayoutStyle& s) { s.Grow = 1.0f; });
        m_head->AddChild(spacer.Get());
        SceneTools* self = this;
        auto fold = MakeRef<gui::Button>(m_allocator);
        fold->AddClass(u8"slot-button");
        fold->SetIconSource(String(u8"Editor/Icons/chevron-down.svg"));
        fold->SetTooltip(u8"Fold the panel to its title");
        fold->SetOnClick([self]() { self->SetFolded(!self->m_folded); });
        m_fold = fold.Get();
        m_head->AddChild(fold.Get());
        auto close = MakeRef<gui::Button>(m_allocator);
        close->AddClass(u8"slot-button");
        close->SetIconSource(String(u8"Editor/Icons/close.svg"));
        close->SetTooltip(u8"Close the tool (back to Select)");
        close->SetOnClick([self]()
                          {
                              if (self->m_services.tools != nullptr)
                                  self->m_services.tools->ActivateDefault();
                          });
        m_head->AddChild(close.Get());
        m_head->SetTooltip(u8"Drag to move; double-click to put it back under the view's chips");
        m_chrome->AddChild(m_head.Get());

        m_head->OnDragStart = [self]()
        {
            // From where it stands (under the chips until it was moved).
            self->m_moved = true;
            self->m_dragFrom = self->m_place;
        };
        m_head->OnDrag = [self](Float2 delta)
        {
            self->m_place = self->m_dragFrom + delta;
            self->Clamp();
        };
        m_head->OnDragEnd = [self]()
        {
            if (self->m_services.placeChanged)
                self->m_services.placeChanged();
        };
        m_head->OnReset = [self]()
        {
            self->SetPlace(nullptr);
            if (self->m_services.placeChanged)
                self->m_services.placeChanged();
        };
    }

    void SceneTools::Mount(gui::Node* panel, IViewportTool& tool)
    {
        m_body = RefPtr<gui::Node>(panel);
        m_body->SetDisplayed(!m_folded);
        m_chrome->AddChild(m_body.Get());
        m_title->SetText(tool.DisplayName());
        m_icon->SetDisplayed(!tool.Icon().IsEmpty());
        if (!tool.Icon().IsEmpty())
            m_icon->SetSource(String(tool.Icon()));
        m_panelShown = true;
        Reparent();
    }

    void SceneTools::Clear()
    {
        if (m_body)
        {
            m_body->RemoveFromParent();
            m_body = {};
        }
        if (m_chrome->GetParent() != nullptr)
            m_chrome->RemoveFromParent();
        m_panelShown = false;
    }

    // In the viewport's room (a stack), at its top start, placed by its margins. It always stays
    // there, so a drag never takes it out of the tree under the pointer.
    void SceneTools::Reparent()
    {
        if (!m_panelShown || m_services.pane == nullptr)
            return;
        if (m_chrome->GetParent() != m_services.pane)
        {
            if (m_chrome->GetParent() != nullptr)
                m_chrome->RemoveFromParent();
            m_services.pane->AddChild(m_chrome.Get());
            m_chrome->EditLayoutStyle([](gui::LayoutStyle& s)
                                      {
                                          s.AlignSelf = gui::Align::Start;
                                          s.JustifySelf = gui::Align::Start;
                                      });
        }
        Clamp();
    }

    // Under the chips while it has not moved (at the zone's content edge, below the zone);
    // otherwise at its place, kept inside the viewport (which may have shrunk since).
    void SceneTools::Clamp()
    {
        if (m_services.pane == nullptr)
            return;
        const gui::Rect pane = m_services.pane->GetScreenBounds();
        if (!m_moved && m_services.zone != nullptr)
        {
            const gui::Rect zone = m_services.zone->GetScreenBounds();
            m_place = Float2{zone.x - pane.x + m_services.zone->GetContentBounds().x, zone.y + zone.height - pane.y};
        }
        const Float2 size = m_chrome->GetSize();
        m_place.x = Max(0.0f, Min(m_place.x, pane.width - size.x));
        m_place.y = Max(0.0f, Min(m_place.y, pane.height - size.y));
        const gui::Thickness margin{m_place.x, m_place.y, 0.0f, 0.0f};
        const gui::Thickness now = m_chrome->GetLayoutStyle().Margin;
        if (now.Left != margin.Left || now.Top != margin.Top)
            m_chrome->SetMargin(margin);
    }

    void SceneTools::SetPlace(const Float2* place)
    {
        m_moved = place != nullptr;
        if (place != nullptr)
            m_place = *place;
        Reparent();
    }

    void SceneTools::SetFolded(bool folded)
    {
        m_folded = folded;
        if (m_body)
            m_body->SetDisplayed(!folded);
        if (m_fold != nullptr)
        {
            m_fold->SetIconSource(String(folded ? StringView(u8"Editor/Icons/chevron-right.svg") : StringView(u8"Editor/Icons/chevron-down.svg")));
            m_fold->SetTooltip(folded ? StringView(u8"Unfold the panel") : StringView(u8"Fold the panel to its title"));
        }
    }

    void SceneTools::Sync()
    {
        if (m_bar)
            m_bar->Sync();
        if (m_services.context != nullptr && m_services.page != nullptr)
        {
            EditorActionRegistry& actions = m_services.context->Actions();
            for (Category& category : m_categories)
            {
                bool any = false;
                for (usize i = 0; i < category.ids.Size(); ++i)
                {
                    const String action = ActionOf(category.ids[i].AsView());
                    const bool on = actions.IsChecked(action.AsView(), m_services.page);
                    const bool enabled = actions.IsEnabled(action.AsView(), m_services.page);
                    any = any || on;
                    gui::MenuItem* item = category.items[i];
                    if (item->IsChecked() != on)
                        item->SetCheckedSilently(on);
                    if (item->IsEnabled() != enabled)
                        item->SetEnabled(enabled);
                    item->SetTooltip(actions.DisabledReason(action.AsView(), m_services.page).AsView());
                }
                // Lit while one of its tools is on; it opens even when all are disabled (each
                // item says why).
                gui::BoundCommandState state;
                state.Available = true;
                state.Checked = any;
                category.button->OnBoundCommandState(state);
            }
        }
        if (m_host)
            m_host->Sync();
        // A panel built from the kit shows the tool's current state (its keys change it too).
        if (auto* body = m_body ? Cast<app::ToolPanelBody>(m_body.Get()) : nullptr)
            body->Sync();
        if (m_panelShown)
            Clamp();
    }

    StringView SceneTools::PanelTitle() const { return m_panelShown && m_title != nullptr ? m_title->GetText() : StringView{}; }

    gui::ToolMenuButton* SceneTools::CategoryButton(StringView category) const
    {
        for (const Category& known : m_categories)
            if (known.name.AsView() == category)
                return known.button;
        return nullptr;
    }
}
