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
        return root != nullptr && BindData(property, root->GetContext().GetDataRef(), key);
    }

    // === The facade and registration ===

    namespace
    {
        DataModel* ScriptDataModel()
        {
            foundation::script::IScriptContext* context = foundation::script::CurrentScriptContext();
            const auto* binding =
                context != nullptr ? static_cast<const GuiScriptBinding*>(context->GetService(kGuiScriptService)) : nullptr;
            return binding != nullptr && binding->root != nullptr ? &binding->root->GetContext().GetData() : nullptr;
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

    namespace
    {
        SceneRoot* ScriptRoot()
        {
            foundation::script::IScriptContext* context = foundation::script::CurrentScriptContext();
            const auto* binding =
                context != nullptr ? static_cast<const GuiScriptBinding*>(context->GetService(kGuiScriptService)) : nullptr;
            return binding != nullptr ? binding->root : nullptr;
        }
    }

    void Gui::DefineCommand(StringView id, StringView label)
    {
        if (SceneRoot* root = ScriptRoot())
            root->GetContext().DefineCommand(id).SetLabel(label.Size() != 0 ? label : id);
    }
    void Gui::SetCommandEnabled(StringView id, bool enabled)
    {
        if (SceneRoot* root = ScriptRoot())
            root->GetContext().DefineCommand(id).SetEnabled(enabled);
    }
    void Gui::SetCommandChecked(StringView id, bool checked)
    {
        if (SceneRoot* root = ScriptRoot())
            root->GetContext().DefineCommand(id).SetChecked(checked);
    }
    bool Gui::IsCommandChecked(StringView id)
    {
        SceneRoot* root = ScriptRoot();
        const Command* command = root != nullptr ? root->GetContext().GetCommands().Find(id) : nullptr;
        return command != nullptr && command->IsChecked();
    }
    bool Gui::RunCommand(StringView id)
    {
        SceneRoot* root = ScriptRoot();
        return root != nullptr && root->RunCommand(id);
    }

    bool Node::FocusElement()
    {
        EventDispatcher* dispatcher = GetEventDispatcher();
        Node* target = dispatcher != nullptr ? FocusTargetOf(*this) : nullptr;
        if (target == nullptr)
            return false;
        dispatcher->SetFocusNode(target);
        return true;
    }

    bool Gui::BindKey(StringView keys, StringView command, StringView context)
    {
        SceneRoot* root = ScriptRoot();
        return root != nullptr && root->GetContext().GetKeymap().Bind(keys, command, context);
    }

    void Node::OnCommandScript(StringView id, RefPtr<foundation::script::IScriptDelegate> handler)
    {
        if (!handler)
            return;
        (void)OnCommand(id,
                        [handler](CommandEvent& event)
                        {
                            Result<core::Variant> result = handler->Invoke(Span<core::Variant>{});
                            const bool* goOn = result.HasValue() ? result.Value().TryGet<bool>() : nullptr;
                            event.Propagate = goOn != nullptr && *goOn;
                        });
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
                &ComboBox::StaticType(),
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
        // Navigation (G7): reflected with their CSS names, so a sheet, markup and a script set them.
        builder.AccessorProperty<&Node::GetNavUp, &Node::SetNavUp>("navUp").PropAttribute("css", core::String(u8"nav-up"));
        builder.AccessorProperty<&Node::GetNavDown, &Node::SetNavDown>("navDown").PropAttribute("css", core::String(u8"nav-down"));
        builder.AccessorProperty<&Node::GetNavLeft, &Node::SetNavLeft>("navLeft").PropAttribute("css", core::String(u8"nav-left"));
        builder.AccessorProperty<&Node::GetNavRight, &Node::SetNavRight>("navRight").PropAttribute("css", core::String(u8"nav-right"));
        builder.AccessorProperty<&Node::GetNavNext, &Node::SetNavNext>("navNext").PropAttribute("css", core::String(u8"nav-next"));
        builder.AccessorProperty<&Node::GetNavPrevious, &Node::SetNavPrevious>("navPrev").PropAttribute("css", core::String(u8"nav-prev"));
        builder.AccessorProperty<&Node::GetNavScopeName, &Node::SetNavScopeName>("navScope").PropAttribute("css", core::String(u8"nav-scope"));
        builder.AccessorProperty<&Node::GetDefaultFocusId, &Node::SetDefaultFocusId>("navDefault").PropAttribute("css", core::String(u8"nav-default"));
        builder.AccessorProperty<&Node::GetCommand, &Node::SetCommand>("command").PropAttribute("css", core::String(u8"command"));
        builder.AccessorProperty<&Node::GetKeyContext, &Node::SetKeyContext>("keyContext").PropAttribute("css", core::String(u8"key-context"));
        builder.Method<&Node::AddElement, Node>("add");
        builder.Method<&Node::FindElement, Node>("find");
        builder.Method<&Node::ParentElement, Node>("parent");
        builder.Method<&Node::ChildElement, Node>("childAt");
        builder.Method<&Node::ElementChildCount>("childCount");
        builder.Method<&Node::On>("on");
        builder.Method<&Node::BindScript>("bind");
        builder.Method<&Node::BindSceneData>("bindData");
        builder.Method<&Node::OnCommandScript>("onCommand");
        builder.Method<&Node::FocusElement>("focus");
        builder.Method<&Node::Destroy>("destroy");
        builder.Method<&ElementOf<Node>, Node, Node>("of");
    }

    REFLECT_MEMBERS(SceneRoot, "rtti::gui")
    {
        builder.Method<&ElementOf<SceneRoot>, SceneRoot, Node>("of");
    }

    REFLECT_MEMBERS(UINode, "rtti::gui")
    {
        builder.Method<&ElementOf<UINode>, UINode, Node>("of");
    }

    REFLECT_MEMBERS(UIWidget, "rtti::gui")
    {
        builder.AccessorProperty<&UIWidget::GetId, &UIWidget::SetId>("id");
        builder.AccessorProperty<&UIWidget::GetTooltipText, &UIWidget::SetTooltip>("tooltip").PropAttribute("css", core::String(u8"tooltip"));
        builder.Method<&UIWidget::AddClass>("addClass");
        builder.Method<&UIWidget::RemoveClass>("removeClass");
        builder.Method<&UIWidget::HasClass>("hasClass");
        builder.Method<&UIWidget::ToggleClass>("toggleClass");
        builder.Method<&ElementOf<UIWidget>, UIWidget, Node>("of");
    }

    REFLECT_MEMBERS(Label, "rtti::gui")
    {
        builder.AccessorProperty<&Label::GetText, &Label::SetText>("text");
        builder.AccessorProperty<&Label::GetTextColor, &Label::SetTextColor>("textColor");
        builder.Method<&ElementOf<Label>, Label, Node>("of");
        builder.Creator();
    }

    REFLECT_MEMBERS(Button, "rtti::gui")
    {
        builder.Method<&ElementOf<Button>, Button, Node>("of");
        builder.Creator();
    }

    REFLECT_MEMBERS(CheckBox, "rtti::gui")
    {
        builder.AccessorProperty<&CheckBox::IsChecked, &CheckBox::SetChecked>("checked")
            .PropAttribute("twoWay", true); // the user changes it: a binding may write it back
        builder.Method<&ElementOf<CheckBox>, CheckBox, Node>("of");
        builder.Creator();
    }

    REFLECT_MEMBERS(RadioButton, "rtti::gui")
    {
        builder.Method<&ElementOf<RadioButton>, RadioButton, Node>("of");
        builder.Creator();
    }

    REFLECT_MEMBERS(Slider, "rtti::gui")
    {
        builder.AccessorProperty<&Slider::GetValue, &Slider::SetValue>("value")
            .PropAttribute("twoWay", true); // the user changes it: a binding may write it back
        builder.Method<&ElementOf<Slider>, Slider, Node>("of");
        builder.Creator();
    }

    REFLECT_MEMBERS(ProgressBar, "rtti::gui")
    {
        builder.AccessorProperty<&ProgressBar::GetProgress, &ProgressBar::SetProgress>("progress");
        builder.Method<&ElementOf<ProgressBar>, ProgressBar, Node>("of");
        builder.Creator();
    }

    REFLECT_MEMBERS(TextField, "rtti::gui")
    {
        builder.AccessorProperty<&TextField::GetPlaceholder, &TextField::SetPlaceholder>("placeholder").PropAttribute("css", core::String(u8"placeholder"));
        builder.AccessorProperty<&TextField::GetMaxLength, &TextField::SetMaxLength>("maxLength").PropAttribute("css", core::String(u8"max-length"));
        builder.AccessorProperty<&TextField::GetText, &TextField::SetText>("text")
            .PropAttribute("twoWay", true); // the user changes it: a binding may write it back
        builder.Method<&ElementOf<TextField>, TextField, Node>("of");
        builder.Creator();
    }

    REFLECT_MEMBERS(LinearLayout, "rtti::gui")
    {
        builder.Method<&ElementOf<LinearLayout>, LinearLayout, Node>("of");
        builder.Creator();
    }

    REFLECT_MEMBERS(GridLayout, "rtti::gui")
    {
        builder.AccessorProperty<&GridLayout::GetColumns, &GridLayout::SetColumns>("columns").PropAttribute("css", core::String(u8"columns"));
        builder.AccessorProperty<&GridLayout::GetHorizontalSpacing, &GridLayout::SetUniformSpacing>("spacing").PropAttribute("css", core::String(u8"spacing"));
        builder.Method<&ElementOf<GridLayout>, GridLayout, Node>("of");
        builder.Creator();
    }

    REFLECT_MEMBERS(RelativeLayout, "rtti::gui")
    {
        builder.Method<&ElementOf<RelativeLayout>, RelativeLayout, Node>("of");
        builder.Creator();
    }

    REFLECT_MEMBERS(FlexLayout, "rtti::gui")
    {
        builder.Method<&ElementOf<FlexLayout>, FlexLayout, Node>("of");
        builder.Creator();
    }

    REFLECT_MEMBERS(ScrollView, "rtti::gui")
    {
        builder.AccessorProperty<&ScrollView::IsAutoMeasureContent, &ScrollView::SetAutoMeasureContent>("autoMeasure").PropAttribute("css", core::String(u8"auto-measure"));
        builder.Method<&ElementOf<ScrollView>, ScrollView, Node>("of");
        builder.Creator();
    }

    REFLECT_MEMBERS(Image, "rtti::gui")
    {
        builder.Method<&ElementOf<Image>, Image, Node>("of");
        builder.Creator();
    }

    REFLECT_MEMBERS(ListBox, "rtti::gui")
    {
        builder.AccessorProperty<&ListBox::GetItemsText, &ListBox::SetItemsText>("items").PropAttribute("css", core::String(u8"items"));
        builder.Method<&ElementOf<ListBox>, ListBox, Node>("of");
        builder.Creator();
    }

    REFLECT_MEMBERS(ComboBox, "rtti::gui")
    {
        builder.AccessorProperty<&ComboBox::GetItemsText, &ComboBox::SetItemsText>("items").PropAttribute("css", core::String(u8"items"));
        builder.Method<&ElementOf<ComboBox>, ComboBox, Node>("of");
        builder.Creator();
    }

    REFLECT_MEMBERS(Window, "rtti::gui")
    {
        builder.AccessorProperty<&Window::GetTitle, &Window::SetTitle>("title").PropAttribute("css", core::String(u8"title"));
        builder.Method<&ElementOf<Window>, Window, Node>("of");
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
        builder.Method<&Gui::DefineCommand>("defineCommand");
        builder.Method<&Gui::SetCommandEnabled>("setCommandEnabled");
        builder.Method<&Gui::SetCommandChecked>("setCommandChecked");
        builder.Method<&Gui::IsCommandChecked>("isCommandChecked");
        builder.Method<&Gui::RunCommand>("runCommand");
        builder.Method<&Gui::BindKey>("bindKey");
    }
}
