// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - module implementation unit: the accessibility tree (E9d of Specs/gui-lab-evolution.md),
// built from the widgets, which its partition only names. A widget's role is its type's
// `accessRole` attribute (a base's when its own has none) or markup's `role`; its name, value,
// range, description and expanded state are the reflected properties its type marks `access`; its
// other states are the ones its selectors read. A node with no role is no node of its own (what it
// holds belongs to the nearest ancestor that has one), and a leaf role's parts are none either.

module;
#include "Core/Prelude.h"

module experimental.gui;

import foundation.core;

using namespace foundation::core;
namespace core = foundation::core;

namespace experimental::gui
{
    namespace
    {
        [[nodiscard]] u64 AccessIdOf(Node& node)
        {
            const Node::ElementRef ref = node.TakeElementRef();
            return ref.table != nullptr ? (static_cast<u64>(ref.generation) << 32) | ref.slot : 0;
        }

        // The property of the node's type (or a base's) that its type marks `access` as `what`.
        [[nodiscard]] const PropertyInfo* AccessProperty(const Node& node, core::StringView what)
        {
            for (const TypeInfo* type = node.GetType(); type != nullptr; type = type->base)
                for (usize i = 0; i < PropertyCount(*type); ++i)
                {
                    const PropertyInfo& property = PropertyAt(*type, i);
                    const Attribute* access = FindAttribute(property, u8"access");
                    const core::String* name = access != nullptr ? access->value.TryGet<core::String>() : nullptr;
                    if (name != nullptr && name->AsView() == what)
                        return &property;
                }
            return nullptr;
        }
        // A type attribute of the node's type, or of the nearest base that has it.
        [[nodiscard]] const core::Variant* TypeAttribute(const Node& node, const char* key)
        {
            for (const TypeInfo* type = node.GetType(); type != nullptr; type = type->base)
                if (const core::Variant* value = FindAttribute(*type, key))
                    return value;
            return nullptr;
        }
        [[nodiscard]] core::Variant Read(const Node& node, const PropertyInfo& property)
        {
            return GetProperty(property, Instance::From(const_cast<Node*>(&node)));
        }
        [[nodiscard]] core::String AsText(const core::Variant& value)
        {
            const core::Variant text = ConvertVariant(value, TypeOf<core::String>());
            const core::String* s = text.TryGet<core::String>();
            return s != nullptr ? *s : core::String{};
        }
        [[nodiscard]] Optional<f64> AsNumber(const core::Variant& value)
        {
            const core::Variant number = ConvertVariant(value, TypeOf<f64>());
            const f64* n = number.TryGet<f64>();
            return n != nullptr ? Optional<f64>(*n) : Optional<f64>{};
        }
        [[nodiscard]] bool Writable(const PropertyInfo& property) noexcept
        {
            return (static_cast<u32>(property.flags) & static_cast<u32>(PropertyFlags::ReadOnly)) == 0 && property.set != nullptr;
        }
        // A property's number, when it holds one (a slider's value, not a text field's).
        [[nodiscard]] bool IsNumeric(const PropertyInfo& property) noexcept
        {
            return property.type == &TypeOf<f64>() || property.type == &TypeOf<f32>() || property.type == &TypeOf<i32>() ||
                   property.type == &TypeOf<u32>() || property.type == &TypeOf<i64>();
        }

        [[nodiscard]] AccessRole RoleOf(const Node& node)
        {
            if (core::Cast<SceneRoot>(&node) != nullptr)
                return AccessRole::Window;
            const UIWidget* widget = core::Cast<UIWidget>(&node);
            if (widget == nullptr)
                return AccessRole::None;
            AccessRole role = AccessRole::None;
            const core::String named = widget->GetAccessRoleName();
            if (named.Size() != 0 && AccessRoleNamed(named.AsView(), role))
                return role;
            if (const core::Variant* attribute = TypeAttribute(node, "accessRole"))
                if (const core::String* name = attribute->TryGet<core::String>())
                    (void)AccessRoleNamed(name->AsView(), role);
            return role;
        }
        // Not read at all, with what it holds: not displayed, hidden, or aria-hidden.
        [[nodiscard]] bool Excluded(const Node& node)
        {
            if (!node.IsDisplayed() || !node.IsVisible())
                return true;
            const UIWidget* widget = core::Cast<UIWidget>(&node);
            return widget != nullptr && widget->IsAccessHidden();
        }

        // The ids in a space-separated list.
        void EachId(core::StringView list, const core::Function<void(core::StringView)>& each)
        {
            usize i = 0;
            while (i < list.Size())
            {
                while (i < list.Size() && list[i] == u8' ')
                    ++i;
                const usize start = i;
                while (i < list.Size() && list[i] != u8' ')
                    ++i;
                if (i > start)
                    each(list.SubStr(start, i - start));
            }
        }
        [[nodiscard]] UIWidget* FindById(Node& under, core::StringView id)
        {
            if (UIWidget* widget = core::Cast<UIWidget>(&under); widget != nullptr && widget->GetId() == id)
                return widget;
            for (usize i = 0; i < under.ChildCount(); ++i)
                if (Node* child = under.GetChildAt(i))
                    if (UIWidget* found = FindById(*child, id))
                        return found;
            return nullptr;
        }

        struct Builder
        {
            Node& top; // the root the ids in aria lists resolve under

            // The included nodes under `node`: each child with a role, or what a child without
            // one holds, in order.
            void Children(Node& node, Array<u64>& out)
            {
                for (usize i = 0; i < node.ChildCount(); ++i)
                {
                    Node* child = node.GetChildAt(i);
                    if (child == nullptr || Excluded(*child))
                        continue;
                    if (RoleOf(*child) != AccessRole::None)
                        out.PushBack(AccessIdOf(*child));
                    else
                        Children(*child, out);
                }
            }
            // Its name: the widgets that label it, else its aria-label, else what its type names it
            // by, else its tooltip.
            core::String NameOf(UIWidget& widget, u32 depth = 0)
            {
                if (depth < 2 && widget.GetAccessLabelledBy().Size() != 0)
                {
                    core::String joined;
                    EachId(widget.GetAccessLabelledBy().AsView(), [&](core::StringView id)
                    {
                        if (UIWidget* by = FindById(top, id))
                        {
                            if (joined.Size() != 0)
                                joined.Append(core::StringView(u8" "));
                            joined.Append(NameOf(*by, depth + 1).AsView());
                        }
                    });
                    if (joined.Size() != 0)
                        return joined;
                }
                if (widget.GetAccessLabel().Size() != 0)
                    return widget.GetAccessLabel();
                if (const PropertyInfo* name = AccessProperty(widget, u8"name"))
                    if (core::String text = AsText(Read(widget, *name)); text.Size() != 0)
                        return text;
                return core::String(widget.GetTooltipText());
            }
            void Ids(core::StringView list, Array<u64>& out)
            {
                EachId(list, [&](core::StringView id)
                {
                    if (UIWidget* found = FindById(top, id))
                        out.PushBack(AccessIdOf(*found));
                });
            }

            void Describe(Node& node, AccessRole role, AccessNode& out)
            {
                out.Id = AccessIdOf(node);
                out.Role = role;
                out.Bounds = node.GetScreenBounds();
                if (node.IsTabFocusable())
                {
                    out.States |= AccessFocusable;
                    out.Actions |= AccessActionBit(AccessAction::Focus);
                }
                if (node.IsFocused())
                    out.States |= AccessFocused;
                const bool enabled = node.IsTreeEnabled();
                if (!enabled)
                    out.States |= AccessDisabled;
                out.Actions |= AccessActionBit(AccessAction::ScrollIntoView);
                if (UIWidget* widget = core::Cast<UIWidget>(&node))
                {
                    out.Name = NameOf(*widget);
                    const u32 states = widget->GetWidgetStates();
                    if ((states & WidgetStateIndeterminate) != 0)
                        out.States |= AccessMixed;
                    else if ((states & WidgetStateChecked) != 0)
                        out.States |= AccessChecked;
                    if ((states & WidgetStateSelected) != 0)
                        out.States |= AccessSelected;
                    const bool editable = (states & WidgetStateEditable) != 0;
                    if (editable)
                        out.States |= AccessEditable;
                    Ids(widget->GetAccessLabelledBy().AsView(), out.LabelledBy);
                    Ids(widget->GetAccessDescribedBy().AsView(), out.DescribedBy);
                    Ids(widget->GetAccessControls().AsView(), out.Controls);
                    // Its description: the names of the widgets that describe it, else its type's.
                    EachId(widget->GetAccessDescribedBy().AsView(), [&](core::StringView id)
                    {
                        if (UIWidget* by = FindById(top, id))
                        {
                            if (out.Description.Size() != 0)
                                out.Description.Append(core::StringView(u8" "));
                            out.Description.Append(NameOf(*by).AsView());
                        }
                    });
                    if (out.Description.Size() == 0)
                        if (const PropertyInfo* description = AccessProperty(node, u8"description"))
                            out.Description = AsText(Read(node, *description));
                }
                if (const core::Variant* activate = TypeAttribute(node, "accessActivate"); activate != nullptr && enabled)
                    if (const bool* yes = activate->TryGet<bool>(); yes != nullptr && *yes)
                        out.Actions |= AccessActionBit(AccessAction::Activate);
                if (const PropertyInfo* value = AccessProperty(node, u8"value"))
                {
                    const core::Variant v = Read(node, *value);
                    const PropertyInfo* secret = AccessProperty(node, u8"secret");
                    const bool hidden = secret != nullptr && AsNumber(Read(node, *secret)).ValueOr(0.0) != 0.0;
                    out.Value = hidden ? core::String{} : AsText(v); // a password's is not read out
                    if (IsNumeric(*value))
                    {
                        out.HasNumber = true;
                        out.Number = AsNumber(v).ValueOr(0.0);
                        if (const PropertyInfo* p = AccessProperty(node, u8"min"))
                            out.Min = AsNumber(Read(node, *p)).ValueOr(0.0);
                        if (const PropertyInfo* p = AccessProperty(node, u8"max"))
                            out.Max = AsNumber(Read(node, *p)).ValueOr(0.0);
                        if (const PropertyInfo* p = AccessProperty(node, u8"step"))
                            out.Step = AsNumber(Read(node, *p)).ValueOr(0.0);
                    }
                    const bool takesValue = enabled && Writable(*value) && (out.HasNumber || (out.States & AccessEditable) != 0);
                    if (takesValue)
                    {
                        out.Actions |= AccessActionBit(AccessAction::SetValue);
                        if (out.HasNumber)
                            out.Actions |= AccessActionBit(AccessAction::Increment) | AccessActionBit(AccessAction::Decrement);
                    }
                }
                if (const PropertyInfo* expanded = AccessProperty(node, u8"expanded"))
                {
                    const bool open = AsNumber(Read(node, *expanded)).ValueOr(0.0) != 0.0;
                    out.States |= open ? AccessExpanded : AccessCollapsed;
                    if (enabled && Writable(*expanded))
                        out.Actions |= AccessActionBit(open ? AccessAction::Collapse : AccessAction::Expand);
                }
                if (!IsLeafRole(role))
                    Children(node, out.Children);
            }

            // Every included node from `node` down (`node` itself when it has a role).
            void All(Node& node, Array<AccessNode>& out)
            {
                if (Excluded(node) && core::Cast<SceneRoot>(&node) == nullptr)
                    return;
                const AccessRole role = RoleOf(node);
                if (role != AccessRole::None)
                {
                    AccessNode described;
                    Describe(node, role, described);
                    out.PushBack(Move(described));
                    if (IsLeafRole(role))
                        return;
                }
                for (usize i = 0; i < node.ChildCount(); ++i)
                    if (Node* child = node.GetChildAt(i))
                        All(*child, out);
            }
        };

        void ClearMarks(Node& node)
        {
            if (!node.IsAccessDirty() && !node.HasAccessDirtyDescendant())
                return;
            node.ClearAccessDirty();
            for (usize i = 0; i < node.ChildCount(); ++i)
                if (Node* child = node.GetChildAt(i))
                    ClearMarks(*child);
        }

        // The nodes marked since the last update: each one marked, and the included node above it
        // (its children may have changed), each once.
        struct Changes
        {
            Builder& builder;
            Array<AccessNode>& out;
            Array<u64> sent;

            void Send(Node& node)
            {
                const u64 id = AccessIdOf(node);
                for (const u64 s : sent)
                    if (s == id)
                        return;
                sent.PushBack(id);
                AccessNode described;
                builder.Describe(node, RoleOf(node), described);
                out.PushBack(Move(described));
            }
            void Walk(Node& node, Node* owner)
            {
                if (!node.IsAccessDirty() && !node.HasAccessDirtyDescendant())
                    return;
                const bool marked = node.IsAccessDirty();
                const bool below = node.HasAccessDirtyDescendant();
                const bool root = core::Cast<SceneRoot>(&node) != nullptr;
                if (!root && Excluded(node))
                {
                    ClearMarks(node);
                    if (marked && owner != nullptr)
                        Send(*owner); // it left its owner's children
                    return;
                }
                node.ClearAccessDirty();
                const AccessRole role = RoleOf(node);
                if (marked && owner != nullptr && owner != &node)
                    Send(*owner);
                Node* next = role != AccessRole::None ? &node : owner;
                if (IsLeafRole(role))
                {
                    if (marked || below)
                        Send(node); // a part's change is the leaf's (its name from its label)
                    for (usize i = 0; i < node.ChildCount(); ++i)
                        if (Node* child = node.GetChildAt(i))
                            ClearMarks(*child);
                    return;
                }
                if (marked && role != AccessRole::None)
                    Send(node);
                for (usize i = 0; i < node.ChildCount(); ++i)
                    if (Node* child = node.GetChildAt(i))
                        Walk(*child, next);
            }
        };

        [[nodiscard]] u64 FocusOf(SceneRoot& root)
        {
            Node* focus = root.GetEventDispatcher()->GetFocusNode();
            // The focus as a screen reader sees it: the nearest included node holding it.
            for (Node* n = focus; n != nullptr; n = n->GetParent())
                if (RoleOf(*n) != AccessRole::None && !Excluded(*n))
                {
                    // A part of a leaf (a spin button's field) reads as the leaf.
                    Node* leaf = n;
                    for (Node* up = n->GetParent(); up != nullptr; up = up->GetParent())
                        if (IsLeafRole(RoleOf(*up)))
                            leaf = up;
                    return AccessIdOf(*leaf);
                }
            return AccessIdOf(root);
        }
    }

    AccessTreeUpdate GUIContext::BuildAccessTree(SceneRoot& root)
    {
        AccessTreeUpdate update;
        update.Root = AccessIdOf(root);
        update.Full = true;
        Builder builder{root};
        builder.All(root, update.Nodes);
        update.Focus = FocusOf(root);
        return update;
    }

    bool GUIContext::DescribeAccess(Node& node, AccessNode& out)
    {
        SceneRoot* root = node.GetSceneRoot();
        if (root == nullptr)
            return false;
        for (Node* n = &node; n != nullptr; n = n->GetParent())
        {
            if (Excluded(*n) && core::Cast<SceneRoot>(n) == nullptr)
                return false;
            if (n != &node && IsLeafRole(RoleOf(*n)))
                return false; // a leaf's part
        }
        const AccessRole role = RoleOf(node);
        if (role == AccessRole::None)
            return false;
        Builder builder{*root};
        out = AccessNode{};
        builder.Describe(node, role, out);
        return true;
    }

    void GUIContext::FlushAccessibility()
    {
        if (m_accessBridge == nullptr || !m_accessBridge->IsListening())
        {
            m_accessListening = false;
            return;
        }
        const bool starting = !m_accessListening;
        m_accessListening = true;
        for (SceneRoot* root : m_roots)
        {
            if (starting)
                root->m_accessSent = false;
            AccessTreeUpdate update;
            const u64 focusWas = root->m_accessFocus;
            if (!root->m_accessSent)
            {
                update = BuildAccessTree(*root);
                ClearMarks(*root);
                root->m_accessSent = true;
            }
            else
            {
                update.Root = AccessIdOf(*root);
                Builder builder{*root};
                Changes changes{builder, update.Nodes, {}};
                changes.Walk(*root, nullptr);
                update.Focus = FocusOf(*root);
            }
            root->m_accessFocus = update.Focus;
            if (update.Full || !update.Nodes.IsEmpty() || update.Focus != focusWas)
                m_accessBridge->Update(update);
        }
    }

    void GUIContext::RunAccessRequests()
    {
        m_accessLock.Lock();
        Array<AccessActionRequest> requests = Move(m_accessRequests);
        m_accessRequests = Array<AccessActionRequest>{};
        m_accessLock.Unlock();
        for (const AccessActionRequest& request : requests)
            (void)RunAccessAction(request);
    }

    bool GUIContext::RunAccessAction(const AccessActionRequest& request)
    {
        Node* node = ResolveAccessId(request.Target);
        if (node == nullptr)
            return false;
        SceneRoot* root = node->GetSceneRoot();
        if (root == nullptr || Excluded(*node))
            return false;
        // Only what it advertises: the same description a screen reader was sent.
        Builder builder{*root};
        AccessNode described;
        builder.Describe(*node, RoleOf(*node), described);
        if (!described.Takes(request.Action))
            return false;
        const Instance instance = Instance::From(node);
        switch (request.Action)
        {
        case AccessAction::Focus:
            node->RequestFocus();
            return node->IsFocused();
        case AccessAction::Activate:
            return node->HandleActivate();
        case AccessAction::SetValue:
        {
            const PropertyInfo* value = AccessProperty(*node, u8"value");
            const core::Variant given = request.HasNumber ? core::Variant::From<f64>(request.Number) : core::Variant::From<core::String>(request.Text);
            return SetProperty(*value, instance, ConvertVariant(given, *value->type)).IsOk();
        }
        case AccessAction::Increment:
        case AccessAction::Decrement:
        {
            const PropertyInfo* value = AccessProperty(*node, u8"value");
            const f64 step = described.Step > 0.0 ? described.Step : (described.Max - described.Min) / 100.0;
            const f64 to = described.Number + (request.Action == AccessAction::Increment ? step : -step);
            return SetProperty(*value, instance, ConvertVariant(core::Variant::From<f64>(to), *value->type)).IsOk();
        }
        case AccessAction::ScrollIntoView:
            for (Node* up = node->GetParent(); up != nullptr; up = up->GetParent())
                if (up->IsScrollContainer())
                    up->ScrollIntoView(*node);
            return true;
        case AccessAction::Expand:
        case AccessAction::Collapse:
        {
            const PropertyInfo* expanded = AccessProperty(*node, u8"expanded");
            return SetProperty(*expanded, instance, core::Variant::From<bool>(request.Action == AccessAction::Expand)).IsOk();
        }
        }
        return false;
    }
}
