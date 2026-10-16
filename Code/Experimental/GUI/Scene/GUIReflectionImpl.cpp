// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - the lab's reflection, element registry and script surface (G1 of
// Specs/vg-and-gui-lab.md).
//
// The widgets and their style-facing properties are reflected here as their native API is spelled
// (StringView text included), so the engine's reflection-driven binding reaches the node tree from
// AngelScript and Luau with no UI-script layer of its own, markup creates the same types by tag,
// and a library adds widgets of its own the same way (ElementTypes::Register). The tree owns its
// nodes; native code (Element<T>) and scripts (a resolve-mode handle, anchored on the table) hold
// them weakly through their GUI's element table. Destroying from a script is deferred to the end
// of the GUI's update (Node::Close).

module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"
#include "Core/Reflection/Reflect.h"

module experimental.gui;

using namespace foundation::core;
namespace core = foundation::core;

namespace experimental::gui
{
    namespace
    {
        // A script handle's context: plain bytes in the Variant; the table is its retained anchor.
        struct ElementHandleCtx
        {
            ElementTable* table;
            u32 slot;
            u32 generation;
        };

        void* ResolveElement(const core::Variant& handle)
        {
            const auto* ctx = static_cast<const ElementHandleCtx*>(handle.ResolveContext());
            return ctx->table->Resolve(ctx->slot, ctx->generation);
        }

        [[nodiscard]] bool DerivesFrom(const TypeInfo* type, const TypeInfo& base) noexcept
        {
            for (const TypeInfo* t = type; t != nullptr; t = t->base)
                if (t == &base)
                    return true;
            return false;
        }

        // `X.of(node)`: the same node's handle typed as X, or null when it is not an X (or gone).
        template <typename T>
        [[nodiscard]] core::Variant ElementOf(core::Variant handle)
        {
            Node* node = Node::FromHandle(handle);
            T* typed = node != nullptr ? Cast<T>(node) : nullptr;
            return typed != nullptr ? typed->HandleAs(T::StaticType()) : core::Variant{};
        }

        [[nodiscard]] bool EventTypeNamed(StringView name, EventType& out) noexcept
        {
            struct Named
            {
                const char8_t* name;
                EventType type;
            };
            static constexpr Named kNames[] = {
                {u8"click", EventType::MouseClick},      {u8"mousedown", EventType::MouseDown},
                {u8"mouseup", EventType::MouseUp},       {u8"mouseenter", EventType::MouseEnter},
                {u8"mouseleave", EventType::MouseLeave}, {u8"mousemove", EventType::MouseMove},
                {u8"wheel", EventType::MouseWheel},      {u8"keydown", EventType::KeyDown},
                {u8"keyup", EventType::KeyUp},           {u8"textinput", EventType::TextInput},
                {u8"focus", EventType::FocusGained},     {u8"blur", EventType::FocusLost},
                {u8"change", EventType::Changed},
            };
            for (const Named& entry : kNames)
            {
                if (StringView(entry.name) == name)
                {
                    out = entry.type;
                    return true;
                }
            }
            return false;
        }

        [[nodiscard]] Node* FindById(Node& node, StringView id)
        {
            if (const UIWidget* widget = Cast<UIWidget>(&node); widget != nullptr && widget->GetId() == id)
                return &node;
            for (usize i = 0; i < node.ChildCount(); ++i)
                if (Node* found = FindById(*node.GetChildAt(i), id))
                    return found;
            return nullptr;
        }

        // The element registry: type knowledge, one per process (an implementation unit's static,
        // the shared-library rendezvous rule).
        struct RegisteredElement
        {
            String tag;
            const TypeInfo* type = nullptr;
        };
        Array<RegisteredElement>& RegisteredElements()
        {
            static Array<RegisteredElement> elements;
            return elements;
        }
    }

    // === The element registry ===

    StringView ElementTypes::TagOf(const TypeInfo& type)
    {
        return TypeAttrString(type, "element", StringView(reinterpret_cast<const char8_t*>(type.name)));
    }

    bool ElementTypes::Register(const TypeInfo& type)
    {
        const StringView tag = TagOf(type);
        if (!DerivesFrom(&type, Node::StaticType()) || type.creator == nullptr)
        {
            LOG_ERROR(u8"GUI", u8"'{}' is not an element type: a reflected Node type with a creator", tag);
            return false;
        }
        for (const RegisteredElement& element : RegisteredElements())
        {
            if (element.tag.AsView() != tag)
                continue;
            if (element.type == &type)
                return true; // registered already
            LOG_ERROR(u8"GUI", u8"the element tag '{}' is taken by another type ({}); not registered", tag,
                      StringView(reinterpret_cast<const char8_t*>(element.type->namespaceName)));
            return false;
        }
        RegisteredElements().PushBack(RegisteredElement{String(tag), &type});
        GlobalTypeRegistry().Register(type); // scripts reach what markup creates
        return true;
    }

    void ElementTypes::Unregister(const TypeInfo& type)
    {
        Array<RegisteredElement>& elements = RegisteredElements();
        for (usize i = 0; i < elements.Size(); ++i)
        {
            if (elements[i].type == &type)
            {
                elements.RemoveAt(i);
                return;
            }
        }
    }

    const TypeInfo* ElementTypes::Find(StringView tag)
    {
        for (const RegisteredElement& element : RegisteredElements())
            if (element.tag.AsView() == tag)
                return element.type;
        return nullptr;
    }

    void ElementTypes::ForEach(const Function<void(StringView, const TypeInfo&)>& visit)
    {
        for (const RegisteredElement& element : RegisteredElements())
            visit(element.tag.AsView(), *element.type);
    }

    // === Weak references ===

    Node::ElementRef Node::TakeElementRef()
    {
        if (!m_elementTable)
        {
            ElementTable* table = GetElementTable();
            if (table == nullptr)
                return ElementRef{};
            m_elementTable = RefPtr<ElementTable>(table);
            m_elementSlot = table->Take(*this);
        }
        return ElementRef{m_elementTable.Get(), m_elementSlot, m_elementTable->Generation(m_elementSlot)};
    }

    core::Variant Node::HandleAs(const TypeInfo& type)
    {
        const ElementRef ref = TakeElementRef();
        if (ref.table == nullptr)
            return core::Variant{};
        return core::Variant::Resolving(&type, &ResolveElement, ElementHandleCtx{ref.table, ref.slot, ref.generation},
                                  ref.table);
    }

    Node* Node::FromHandle(const core::Variant& handle)
    {
        if (!handle.IsResolving() || !DerivesFrom(handle.Type(), Node::StaticType()))
            return nullptr;
        return static_cast<Node*>(handle.Resolve());
    }

    // === The tree by name ===

    core::Variant Node::AddElement(StringView tag)
    {
        const ElementSet* elements = GetElementSet();
        RefPtr<Node> element = elements != nullptr ? elements->Create(tag, MemoryAllocator()) : RefPtr<Node>{};
        if (!element)
            return core::Variant{};
        AddChild(element.Get());
        return element->HandleAs(Node::StaticType());
    }

    core::Variant Node::FindElement(StringView id)
    {
        Node* found = FindById(*this, id);
        return found != nullptr ? found->HandleAs(Node::StaticType()) : core::Variant{};
    }

    core::Variant Node::ParentElement()
    {
        return m_parent != nullptr ? m_parent->HandleAs(Node::StaticType()) : core::Variant{};
    }

    core::Variant Node::ChildElement(i32 index)
    {
        Node* child = index >= 0 ? GetChildAt(static_cast<usize>(index)) : nullptr;
        return child != nullptr ? child->HandleAs(Node::StaticType()) : core::Variant{};
    }

    void Node::On(StringView eventName, RefPtr<foundation::script::IScriptDelegate> handler)
    {
        EventType type = EventType::MouseClick;
        if (!handler || !EventTypeNamed(eventName, type))
            return;
        // The node holds the handler (and with it the script function); the script holds the node
        // only weakly, so neither keeps the other alive.
        (void)AddEventListener(type, [handler](const Event&) { (void)handler->Invoke(Span<core::Variant>{}); });
    }

    // === Bindings ===

    namespace
    {
        // The reflected property named `name` on `type` or a base, or null.
        const PropertyInfo* FindReflectedProperty(const TypeInfo* type, StringView name)
        {
            for (; type != nullptr; type = type->base)
                for (usize i = 0; i < PropertyCount(*type); ++i)
                {
                    const PropertyInfo& property = PropertyAt(*type, i);
                    if (StringView(reinterpret_cast<const utf8char*>(property.name)) == name)
                        return &property;
                }
            return nullptr;
        }
    }

    bool Node::BindProperty(StringView name, core::Function<core::Variant()> expression)
    {
        const PropertyInfo* property = FindReflectedProperty(GetType(), name);
        if (property == nullptr || property->type == nullptr ||
            (static_cast<u32>(property->flags) & static_cast<u32>(PropertyFlags::ReadOnly)) != 0)
            return false;
        Bind([this, property, expression = core::Move(expression)]() mutable
             {
                 const core::Variant value = ConvertVariant(expression(), *property->type);
                 // The write is not a read: setting a property must not make this binding follow it.
                 if (!value.IsEmpty())
                     Untracked([&] { (void)SetProperty(*property, Instance::From(this), value); });
             });
        return true;
    }

    bool Node::BindPropertyBack(StringView name, core::Function<void(const core::Variant&)> write)
    {
        const PropertyInfo* property = FindReflectedProperty(GetType(), name);
        if (property == nullptr || FindAttribute(*property, u8"twoWay") == nullptr)
            return false;
        Bind([this, property, write = core::Move(write)]() mutable
             {
                 const core::Variant value = GetProperty(*property, Instance::From(this)); // followed
                 Untracked([&] { write(value); });
             });
        return true;
    }

    bool Node::BindScript(StringView property, RefPtr<foundation::script::IScriptDelegate> expression)
    {
        if (!expression)
            return false;
        // The node holds the function (as it holds an event handler); a failed call binds nothing.
        return BindProperty(property,
                            [expression]
                            {
                                Result<core::Variant> result = expression->Invoke(Span<core::Variant>{});
                                return result.HasValue() ? result.Value() : core::Variant{};
                            });
    }

    bool Node::BindSceneData(StringView property, StringView key)
    {
        SceneRoot* root = GetSceneRoot();
        return root != nullptr && BindData(property, root->GetDataRef(), key);
    }

    // === The facade and registration ===

    namespace
    {
        DataModel* ScriptDataModel()
        {
            foundation::script::IScriptContext* context = foundation::script::CurrentScriptContext();
            const auto* binding =
                context != nullptr ? static_cast<const GuiScriptBinding*>(context->GetService(kGuiScriptService)) : nullptr;
            return binding != nullptr && binding->root != nullptr ? &binding->root->GetData() : nullptr;
        }
    }

    f64 Gui::GetNumber(StringView name)
    {
        DataModel* data = ScriptDataModel();
        const core::Variant value = data != nullptr ? ConvertVariant(data->Get(name), TypeOf<f64>()) : core::Variant{};
        return value.IsEmpty() ? 0.0 : value.Get<f64>();
    }

    core::String Gui::GetText(StringView name)
    {
        DataModel* data = ScriptDataModel();
        return data != nullptr ? FormatVariant(data->Get(name)) : core::String{};
    }

    bool Gui::GetBool(StringView name)
    {
        DataModel* data = ScriptDataModel();
        const core::Variant value = data != nullptr ? ConvertVariant(data->Get(name), TypeOf<bool>()) : core::Variant{};
        return !value.IsEmpty() && value.Get<bool>();
    }

    void Gui::Set(StringView name, const core::Variant& value)
    {
        if (DataModel* data = ScriptDataModel())
            data->Set(name, value);
    }

    core::Variant Gui::Root()
    {
        foundation::script::IScriptContext* context = foundation::script::CurrentScriptContext();
        const auto* binding =
            context != nullptr ? static_cast<const GuiScriptBinding*>(context->GetService(kGuiScriptService)) : nullptr;
        return binding != nullptr && binding->root != nullptr ? binding->root->HandleAs(Node::StaticType())
                                                              : core::Variant{};
    }

    void RegisterGuiElements()
    {
        static const bool once = []()
        {
            const TypeInfo* const kLabElements[] = {
                &Label::StaticType(),        &Button::StaticType(),       &CheckBox::StaticType(),
                &RadioButton::StaticType(),  &Slider::StaticType(),       &ProgressBar::StaticType(),
                &TextField::StaticType(),    &LinearLayout::StaticType(), &GridLayout::StaticType(),
                &RelativeLayout::StaticType(), &FlexLayout::StaticType(), &ScrollView::StaticType(),
                &Image::StaticType(),        &ListBox::StaticType(),      &Window::StaticType(),
            };
            for (const TypeInfo* type : kLabElements)
                (void)ElementTypes::Register(*type);
            return true;
        }();
        (void)once;
    }

    void RegisterGuiScriptSurface()
    {
        static const bool once = []()
        {
            RegisterGuiElements();
            TypeRegistry& registry = GlobalTypeRegistry();
            registry.Register(Node::StaticType()); // the bases a script names (Node.of, UIWidget.of)
            registry.Register(SceneRoot::StaticType());
            registry.Register(UINode::StaticType());
            registry.Register(UIWidget::StaticType());
            registry.Register(Gui::StaticType()); // binds as `gui` (its scriptName)
            return true;
        }();
        (void)once;
    }

    // === Reflection ===

    REFLECT_MEMBERS(Node, "rtti::gui")
    {
        builder.AccessorProperty<&Node::GetX, &Node::SetX>("x");
        builder.AccessorProperty<&Node::GetY, &Node::SetY>("y");
        builder.AccessorProperty<&Node::GetWidth, &Node::SetWidth>("width");
        builder.AccessorProperty<&Node::GetHeight, &Node::SetHeight>("height");
        builder.AccessorProperty<&Node::IsVisible, &Node::SetVisible>("visible");
        builder.AccessorProperty<&Node::IsEnabled, &Node::SetEnabled>("enabled");
        builder.AccessorProperty<&Node::GetAlpha, &Node::SetAlpha>("opacity");
        builder.Method<&Node::AddElement, Node>("add");
        builder.Method<&Node::FindElement, Node>("find");
        builder.Method<&Node::ParentElement, Node>("parent");
        builder.Method<&Node::ChildElement, Node>("childAt");
        builder.Method<&Node::ElementChildCount>("childCount");
        builder.Method<&Node::On>("on");
        builder.Method<&Node::BindScript>("bind");
        builder.Method<&Node::BindSceneData>("bindData");
        builder.Method<&Node::Destroy>("destroy");
        builder.Method<&ElementOf<Node>, Node>("of");
    }

    REFLECT_MEMBERS(SceneRoot, "rtti::gui")
    {
        builder.Method<&ElementOf<SceneRoot>, SceneRoot>("of");
    }

    REFLECT_MEMBERS(UINode, "rtti::gui")
    {
        builder.Method<&ElementOf<UINode>, UINode>("of");
    }

    REFLECT_MEMBERS(UIWidget, "rtti::gui")
    {
        builder.AccessorProperty<&UIWidget::GetId, &UIWidget::SetId>("id");
        builder.Method<&UIWidget::AddClass>("addClass");
        builder.Method<&UIWidget::RemoveClass>("removeClass");
        builder.Method<&UIWidget::HasClass>("hasClass");
        builder.Method<&UIWidget::ToggleClass>("toggleClass");
        builder.Method<&ElementOf<UIWidget>, UIWidget>("of");
    }

    REFLECT_MEMBERS(Label, "rtti::gui")
    {
        builder.AccessorProperty<&Label::GetText, &Label::SetText>("text");
        builder.AccessorProperty<&Label::GetTextColor, &Label::SetTextColor>("textColor");
        builder.Method<&ElementOf<Label>, Label>("of");
        builder.Creator();
    }

    REFLECT_MEMBERS(Button, "rtti::gui")
    {
        builder.Method<&ElementOf<Button>, Button>("of");
        builder.Creator();
    }

    REFLECT_MEMBERS(CheckBox, "rtti::gui")
    {
        builder.AccessorProperty<&CheckBox::IsChecked, &CheckBox::SetChecked>("checked")
            .PropAttribute("twoWay", true); // the user changes it: a binding may write it back
        builder.Method<&ElementOf<CheckBox>, CheckBox>("of");
        builder.Creator();
    }

    REFLECT_MEMBERS(RadioButton, "rtti::gui")
    {
        builder.Method<&ElementOf<RadioButton>, RadioButton>("of");
        builder.Creator();
    }

    REFLECT_MEMBERS(Slider, "rtti::gui")
    {
        builder.AccessorProperty<&Slider::GetValue, &Slider::SetValue>("value")
            .PropAttribute("twoWay", true); // the user changes it: a binding may write it back
        builder.Method<&ElementOf<Slider>, Slider>("of");
        builder.Creator();
    }

    REFLECT_MEMBERS(ProgressBar, "rtti::gui")
    {
        builder.AccessorProperty<&ProgressBar::GetProgress, &ProgressBar::SetProgress>("progress");
        builder.Method<&ElementOf<ProgressBar>, ProgressBar>("of");
        builder.Creator();
    }

    REFLECT_MEMBERS(TextField, "rtti::gui")
    {
        builder.AccessorProperty<&TextField::GetText, &TextField::SetText>("text")
            .PropAttribute("twoWay", true); // the user changes it: a binding may write it back
        builder.Method<&ElementOf<TextField>, TextField>("of");
        builder.Creator();
    }

    REFLECT_MEMBERS(LinearLayout, "rtti::gui")
    {
        builder.Method<&ElementOf<LinearLayout>, LinearLayout>("of");
        builder.Creator();
    }

    REFLECT_MEMBERS(GridLayout, "rtti::gui")
    {
        builder.Method<&ElementOf<GridLayout>, GridLayout>("of");
        builder.Creator();
    }

    REFLECT_MEMBERS(RelativeLayout, "rtti::gui")
    {
        builder.Method<&ElementOf<RelativeLayout>, RelativeLayout>("of");
        builder.Creator();
    }

    REFLECT_MEMBERS(FlexLayout, "rtti::gui")
    {
        builder.Method<&ElementOf<FlexLayout>, FlexLayout>("of");
        builder.Creator();
    }

    REFLECT_MEMBERS(ScrollView, "rtti::gui")
    {
        builder.Method<&ElementOf<ScrollView>, ScrollView>("of");
        builder.Creator();
    }

    REFLECT_MEMBERS(Image, "rtti::gui")
    {
        builder.Method<&ElementOf<Image>, Image>("of");
        builder.Creator();
    }

    REFLECT_MEMBERS(ListBox, "rtti::gui")
    {
        builder.Method<&ElementOf<ListBox>, ListBox>("of");
        builder.Creator();
    }

    REFLECT_MEMBERS(Window, "rtti::gui")
    {
        builder.Method<&ElementOf<Window>, Window>("of");
        builder.Creator();
    }

    REFLECT_MEMBERS(Gui, "rtti::gui")
    {
        builder.Attribute("scriptName", "gui");
        builder.Method<&Gui::Root, Node>("root");
        builder.Method<&Gui::GetNumber>("getNumber");
        builder.Method<&Gui::GetText>("getText");
        builder.Method<&Gui::GetBool>("getBool");
        builder.Method<&Gui::Set>("set");
    }
}
