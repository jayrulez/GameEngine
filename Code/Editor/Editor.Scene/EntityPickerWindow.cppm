// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Scene - :entity_picker_window partition.
//
// An entity slot's pick (Documentation/Specs/editor-gui.md, ruling 25; the mock's
// entity-picker.xml): the scene as its hierarchy shows it, the hierarchy's own tree model
// (SceneTreeModel) in a picker's frame. The scene is its top row (open, and no pick); each entity
// by its type's icon in its category's colour, opened and closed by its arrow; the way to the
// current entity open and its row lit. The search keeps each match with what holds it and opens
// every way to them. Up and Down move the light from the search, Enter or a double click picks,
// Esc or its close keeps the value (a slot's own clear sets nothing). It opens in the window's
// middle, as every picker does.

module;
#include "Core/Prelude.h"

export module editor.scene:entity_picker_window;

import foundation.core;
import foundation.scene;
import foundation.gui;
import editor.core;
import :edit;
import :scene_tree;

using namespace foundation::core;

export namespace editor
{
    /// What an entity slot asks the picker for.
    struct EntityPickRequest
    {
        Guid current;                       // its value now (nil: none)
        Function<void(const Guid&)> chosen; // a pick (nil: None)
    };

    class EntityPickerWindow
    {
    public:
        /// Its markup, under the data root.
        static constexpr const char8_t* kPath = u8"Editor/EntityPicker.xml";

        /// What it shows the scene by, as the page's hierarchy does.
        struct Services
        {
            EditorIcons* icons = nullptr;     // the entities' type icons (null: none)
            StringView sceneIcon;             // the top row's
            Function<StringView()> sceneName; // the top row's text (unset: the scene's name)
        };

        /// The picker over `edit`'s scene in `content` (the markup's `entity-picker-search`,
        /// `entity-picker-tree`, `entity-picker-state`, `entity-picker-hint` and
        /// `entity-picker-close`, by id).
        EntityPickerWindow(IAllocator& allocator, SceneEditContext& edit, Services services, RefPtr<foundation::gui::Node> content)
            : m_edit(&edit), m_services(Move(services)), m_content(Move(content)), m_model(allocator)
        {
            namespace gui = foundation::gui;
            if (!m_content)
                return;
            m_search = Find<gui::TextField>(u8"entity-picker-search");
            m_tree = Find<gui::TreeView>(u8"entity-picker-tree");
            m_state = Find<gui::Label>(u8"entity-picker-state");
            m_hint = Find<gui::Label>(u8"entity-picker-hint");
            EntityPickerWindow* self = this;
            if (m_search != nullptr)
                m_search->SetOnTextChanging([self](StringView text) { self->Search(text); });
            if (m_tree != nullptr)
            {
                m_tree->SetModel(&m_model);
                m_tree->SetOnActivated([self](gui::ModelIndex index) { self->PickEntity(self->m_model.EntityOfNode(index.InternalId)); });
            }
            if (auto* close = Find<gui::Button>(u8"entity-picker-close"))
                close->SetOnClick([self]() { self->Close(); });
            // Up, Down and Enter before the search takes them.
            (void)m_content->AddEventListener(gui::EventType::KeyDown, [self](const gui::Event& e)
                                              {
                                                  const auto& key = static_cast<const gui::KeyEvent&>(e);
                                                  const auto code = static_cast<gui::KeyCode>(key.KeyCode);
                                                  if (key.Modifiers != 0)
                                                      return;
                                                  if (code == gui::KeyCode::Down || code == gui::KeyCode::Up)
                                                  {
                                                      self->Step(code == gui::KeyCode::Down ? 1 : -1);
                                                      e.Handled = true;
                                                  }
                                                  else if (code == gui::KeyCode::Return || code == gui::KeyCode::KeypadEnter)
                                                  {
                                                      self->PickLit();
                                                      e.Handled = true;
                                                  }
                                              },
                                              true);
        }
        EntityPickerWindow(const EntityPickerWindow&) = delete;
        EntityPickerWindow& operator=(const EntityPickerWindow&) = delete;
        ~EntityPickerWindow()
        {
            Close();
            if (m_tree != nullptr)
                m_tree->SetModel(nullptr);
        }

        /// Opens it over `root` for `request`, in the window's middle: the scene's tree, the way to
        /// the current entity open and its row lit, the search empty and focused. False with no
        /// frame, a root in no GUI, or one open already.
        bool Open(foundation::gui::SceneRoot& root, EntityPickRequest request)
        {
            namespace gui = foundation::gui;
            namespace scene = foundation::scene;
            gui::EventDispatcher* dispatcher = root.GetEventDispatcher();
            if (!m_content || dispatcher == nullptr || IsOpen())
                return false;
            m_root = &root;
            m_request = Move(request);
            m_query.Clear();
            if (m_search != nullptr)
                m_search->SetText(StringView{});
            Build();
            if (m_hint != nullptr)
            {
                scene::Scene& sc = m_edit->Scene();
                const scene::EntityHandle current = m_request.current.IsNil() ? scene::EntityHandle{} : sc.FindEntity(m_request.current);
                const StringView name = current.IsAssigned() ? sc.GetEntityName(current) : StringView{};
                m_hint->SetText(current.IsAssigned() ? Format(u8"Enter picks, Esc keeps {}", name.IsEmpty() ? StringView(u8"(unnamed)") : name).AsView()
                                                     : StringView(u8"Enter picks, Esc closes"));
            }
            gui::PopupOptions options;
            options.Kind = gui::PopupKind::Auto;
            options.Modal = true;
            options.TakesFocus = true;
            options.FitContent = true;
            options.Centered = true;
            dispatcher->OpenPopup(root, *m_content, Move(options));
            if (m_search != nullptr)
                m_search->RequestFocus();
            return true;
        }
        void Close()
        {
            if (IsOpen())
                (void)m_root->GetEventDispatcher()->ClosePopup(*m_content);
        }
        [[nodiscard]] bool IsOpen() const
        {
            return m_root != nullptr && m_content && m_root->GetEventDispatcher() != nullptr && m_root->GetEventDispatcher()->IsPopupOpen(*m_content);
        }

        /// Searches the scene for `text` (as typing it): a match kept with what holds it.
        void Search(StringView text)
        {
            if (m_query.AsView() == text)
                return;
            m_query = String(text);
            if (m_search != nullptr && m_search->GetText() != text)
                m_search->SetText(text);
            Build();
        }
        /// Moves the light `by` rows (the scene's own row among them).
        void Step(i32 by)
        {
            if (m_tree == nullptr || m_tree->GetItemCount() == 0)
                return;
            const i32 lit = m_tree->GetSelectedItem();
            const i32 last = static_cast<i32>(m_tree->GetItemCount()) - 1;
            m_tree->SetSelectedItem(lit < 0 ? 0 : Clamp(lit + by, 0, last));
        }
        /// Picks the lit entity; the scene's row, or none lit, picks nothing.
        void PickLit()
        {
            if (m_tree != nullptr && m_tree->GetSelectedItem() >= 0)
                PickEntity(m_model.EntityOfNode(m_tree->GetItemIndex(m_tree->GetSelectedItem()).InternalId));
        }

        // === What it shows (for tests) ===
        [[nodiscard]] RefPtr<foundation::gui::Node> Content() const { return m_content; }
        [[nodiscard]] foundation::gui::TreeView* Tree() const noexcept { return m_tree; }
        [[nodiscard]] foundation::gui::Button* CloseButton() const { return Find<foundation::gui::Button>(u8"entity-picker-close"); }
        /// The shown rows' texts in order, the scene's first, nested by two spaces a level.
        [[nodiscard]] String Rows() const
        {
            String out;
            if (m_tree == nullptr)
                return out;
            for (i32 item = 0; item < static_cast<i32>(m_tree->GetItemCount()); ++item)
            {
                if (!out.IsEmpty())
                    out += u8" | ";
                for (i32 depth = m_tree->ItemDepth(item); depth > 0; --depth)
                    out += u8"  ";
                out += m_tree->GetItemText(item);
            }
            return out;
        }
        [[nodiscard]] String LitName() const
        {
            return m_tree != nullptr && m_tree->GetSelectedItem() >= 0 ? m_tree->GetItemText(m_tree->GetSelectedItem()) : String{};
        }
        [[nodiscard]] StringView Hint() const { return m_hint != nullptr ? m_hint->GetText() : StringView{}; }
        [[nodiscard]] StringView State() const { return m_state != nullptr && m_state->IsDisplayed() ? m_state->GetText() : StringView{}; }

    private:
        template <typename T>
        [[nodiscard]] T* Find(StringView id) const
        {
            Array<foundation::gui::Node*> pending;
            pending.PushBack(m_content.Get());
            while (!pending.IsEmpty())
            {
                foundation::gui::Node* node = pending.Back();
                pending.PopBack();
                if (auto* widget = Cast<foundation::gui::UIWidget>(node); widget != nullptr && widget->GetId() == id)
                    return Cast<T>(node);
                for (usize i = 0; i < node->ChildCount(); ++i)
                    pending.PushBack(node->GetChildAt(i));
            }
            return nullptr;
        }

        // The tree from the scene as it is now: the scene's row open; with a search every way to a
        // match open, else the way to the current entity; the current entity lit when shown.
        void Build()
        {
            foundation::scene::Scene& sc = m_edit->Scene();
            const String name = m_services.sceneName ? String(m_services.sceneName()) : String(sc.Name());
            m_model.Build(sc, name.AsView(), m_services.sceneIcon, m_services.icons, m_query.AsView());
            const bool empty = m_model.Rows().Size() <= 1; // the scene's row alone
            if (m_tree != nullptr)
            {
                m_tree->SetIdExpanded(SceneTreeModel::kSceneRowId, true);
                if (!m_query.IsEmpty())
                {
                    for (const SceneTreeModel::Row& row : m_model.Rows())
                        if (!row.entity.IsNil() && !row.children.IsEmpty())
                            m_tree->SetIdExpanded(SceneTreeModel::IdOfEntity(row.entity), true);
                }
                else if (!m_request.current.IsNil())
                {
                    const foundation::scene::EntityHandle current = sc.FindEntity(m_request.current);
                    for (foundation::scene::EntityHandle e = current.IsAssigned() ? sc.GetParent(current) : foundation::scene::EntityHandle{}; sc.IsValid(e);
                         e = sc.GetParent(e))
                        m_tree->SetIdExpanded(SceneTreeModel::IdOfEntity(sc.GetEntityId(e)), true);
                }
                m_model.Reset();
                m_tree->SetDisplayed(!empty);
                Light();
            }
            if (m_state != nullptr)
            {
                m_state->SetDisplayed(empty);
                m_state->SetText(!m_query.IsEmpty() ? Format(u8"No entity matches \"{}\".", m_query).AsView()
                                                     : StringView(u8"The scene has no entity to point at."));
            }
        }
        // The current entity lit; not shown (or none), the first match with a search, else nothing.
        void Light()
        {
            const foundation::gui::ItemId current = m_request.current.IsNil() ? foundation::gui::kNoItem : SceneTreeModel::IdOfEntity(m_request.current);
            const Guid first = m_query.IsEmpty() ? Guid{} : FirstMatch();
            i32 lit = -1;
            for (i32 item = 0; item < static_cast<i32>(m_tree->GetItemCount()); ++item)
            {
                const Guid entity = m_model.EntityOfNode(m_tree->GetItemIndex(item).InternalId);
                if (current != foundation::gui::kNoItem && !entity.IsNil() && SceneTreeModel::IdOfEntity(entity) == current)
                    lit = item;
                else if (lit < 0 && !first.IsNil() && entity == first)
                    lit = item;
            }
            if (lit >= 0)
                m_tree->SetSelectedItem(lit);
            else
                m_tree->SetSelectedIds(Span<const foundation::gui::ItemId>{});
        }
        // The first entity, in the tree's order, whose own name or guid the search finds.
        [[nodiscard]] Guid FirstMatch() const
        {
            foundation::scene::Scene& sc = m_edit->Scene();
            for (const SceneTreeModel::Row& row : m_model.Rows())
            {
                if (row.entity.IsNil())
                    continue;
                const foundation::scene::EntityHandle e = sc.FindEntity(row.entity);
                if (e.IsAssigned() && NameOrGuidMatches(sc.GetEntityName(e), row.entity, m_query.AsView()))
                    return row.entity;
            }
            return Guid{};
        }

        // The scene's own row is no pick.
        void PickEntity(const Guid& entity)
        {
            if (!entity.IsNil())
                Pick(entity);
        }
        void Pick(const Guid& id)
        {
            Function<void(const Guid&)> chosen = Move(m_request.chosen);
            Close();
            if (chosen)
                chosen(id);
        }

        SceneEditContext* m_edit;
        Services m_services;
        RefPtr<foundation::gui::Node> m_content;
        foundation::gui::SceneRoot* m_root = nullptr;
        foundation::gui::TextField* m_search = nullptr;
        foundation::gui::TreeView* m_tree = nullptr;
        foundation::gui::Label* m_state = nullptr;
        foundation::gui::Label* m_hint = nullptr;
        SceneTreeModel m_model;
        EntityPickRequest m_request;
        String m_query;
    };
}
