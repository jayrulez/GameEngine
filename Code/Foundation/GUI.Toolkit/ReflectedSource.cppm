// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI.Toolkit - :reflected_source partition
//
// The property source over core reflection. It covers one reflected instance or several of one
// type, and exposes their properties as the grid's paths.
//   - bool, every integer width, f32, f64 and String are values. An enum is a choice of its
//     enumerators, or flags when it has a `flags` attribute; either is read and written as an
//     integer through the property's address. A ReadOnly property is read only.
//   - A Color is a colour, with its alpha hidden by `noAlpha`. A Float4 with `hdrColor` (sRGB rgb
//     and a linear intensity in w) is an HDR colour, hinted "ColorHdr", with the parts `color` and
//     `intensity` (shown in stops). A Float4 with `color` is a colour just as a Color is (the four
//     floats a runtime keeps its tints in), and `noAlpha` hides its alpha.
//   - Float2, Float3 and Float4 are groups hinted with their type names, with parts x, y, z and w.
//     A Quaternion is shown as a Float3 group of Euler degrees (pitch, yaw, roll), which the source
//     converts, as the editor's Transform row does. A Nested struct, or a reflected value struct,
//     is a group.
//   - Array<T> and BoundedArray are lists (through reflection's emplaceDefault, removeAt and
//     moveElement), with their elements by index. An Array<RefPtr<Base>> adds by type: it offers
//     the types that derive from its base and that its factory can make, by their displayName.
//   - A reference-shaped type (Core's TypeInfo::reference, such as a Ref<T>) is one value of kind
//     Reference: its Guid as text (empty for none), naming the type it points at. A write drops
//     the reference's binding, so the next resolve binds the new id. The toolkit has no slot editor
//     for it; the application registers one for the kind. A bare Guid is a reference too: to the
//     type named by its property's `referenceType` attribute, or else to nothing in particular. A
//     list of bare Guids takes its elements' target type from its own property's attribute.
//   - A struct's group carries its type's name as its hint, and so does any other type, so an
//     application can register an editor for it (an EntityRef, say). A struct that no editor takes
//     shows its own rows; any other type shows the grid's row saying that no editor fits.
//   - A struct the application registers as a reference (naming its Guid property) is one value of
//     kind Reference, like Core's reference-shaped types. It names itself as the type it points at
//     and is hinted with its own name, so the application's editor for it (the editor's entity
//     reference field) is chosen before the editor for asset references.
//   - A type the application registers as a curve (naming its keys property, a list of Core's
//     CurveKey) is one value of kind Curve, holding its keys whole. A type registered as a gradient
//     (naming its colour stops, alpha stops and blend properties) is one value of kind Gradient.
//     The toolkit names no runtime type itself; the application says which of its types these are
//     and where their parts are.
//   - The attributes read are those the editor's inspector reads: `displayName` (else the name
//     made readable), `description`, `range` (a Float4 of min, max and step), `visibleWhen`
//     ("prop", or "prop=1,2" tested against the raw integer of a sibling; the row shows when the
//     condition holds for every object), and `flags`.
// Defaults come from the type, read from an instance its creator makes, with any providers the
// application stacks over it (a prefab's, by path and object) asked first. A revert restores each
// object's own default; a list is restored whole from the type's instance.
// Writes go through the property's address, or through its setter for a computed property. Each
// commit is reported to the application as one change: what changed, where, and each object's value
// there before and after as text. The text is written with the text format the application gives
// (its serializer), and Restore reads it back for an undo. The undo belongs to the application.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.gui.toolkit:reflected_source;

import foundation.core;
import foundation.gui;
import :property_source;

using namespace foundation::core;
namespace core = foundation::core;

export namespace foundation::gui
{
    /// What the application gives a reflected source to write text with: its serializer (the
    /// engine's XmlSerializer, say), so the toolkit depends on no format.
    class IPropertyTextFormat
    {
    public:
        virtual ~IPropertyTextFormat() = default;
        /// Returns the text written by `write`, or empty when it failed.
        [[nodiscard]] virtual core::String Write(core::Function<void(core::ISerializer&)> write) = 0;
        /// Reads `text` with `read`. Returns false when the text is not in this format or the read
        /// failed.
        virtual bool Read(core::StringView text, core::Function<void(core::ISerializer&)> read) = 0;
    };

    /// A change a reflected source made: what changed (in words), where, and each object's value
    /// there before and after, as text in the text format (empty when there is no format).
    struct ReflectedChange
    {
        core::String Words;
        core::String Path;
        core::Array<core::String> Before;
        core::Array<core::String> After;
    };

    /// Names the Guid property that identifies a struct registered as a reference.
    struct ReflectedReferenceParts
    {
        core::String Id;
    };
    /// Names the property where a type registered as a curve keeps its keys: a list (an Array or a
    /// BoundedArray) of Core's CurveKey. The most keys it holds is the list's capacity.
    struct ReflectedCurveParts
    {
        core::String Keys;
    };
    /// Names the properties where a type registered as a gradient keeps its parts: its lists of
    /// Core's GradientColorStop and GradientAlphaStop, and its GradientInterpolation. The most stops
    /// of each kind is the smaller list's capacity.
    struct ReflectedGradientParts
    {
        core::String Colors;
        core::String Alphas;
        core::String Interpolation;
    };

    /// Returns a reflected name as a row shows it, following the editor's convention:
    /// "castsShadows" -> "Casts Shadows", "fovYRadians" -> "Fov Y Radians", "IBL" -> "IBL".
    [[nodiscard]] inline core::String ReadablePropertyName(core::StringView name)
    {
        core::String out;
        bool prevLower = false;
        bool prevUpper = false;
        for (usize i = 0; i < name.Size(); ++i)
        {
            char8_t c = name[i];
            const bool upper = c >= u8'A' && c <= u8'Z';
            const bool lower = c >= u8'a' && c <= u8'z';
            if (i == 0 && lower)
                c = static_cast<char8_t>(c - (u8'a' - u8'A'));
            else if (upper)
            {
                const bool nextLower = i + 1 < name.Size() && name[i + 1] >= u8'a' && name[i + 1] <= u8'z';
                if (prevLower || (prevUpper && nextLower))
                    out.Append(core::StringView(u8" "));
            }
            out.Append(core::StringView(&c, 1));
            prevLower = lower;
            prevUpper = upper;
        }
        return out;
    }

    class ReflectedPropertySource : public IPropertySource
    {
    public:
        explicit ReflectedPropertySource(core::IAllocator& allocator)
            : m_allocator(allocator), m_objects(allocator), m_originals(allocator), m_providers(allocator)
        {
        }

        /// Sets the instances it stands for, all of one type (several components make several
        /// objects).
        void SetObjects(core::Span<const core::Instance> objects)
        {
            m_objects.Clear();
            for (const core::Instance& object : objects)
                if (!object.IsEmpty())
                    m_objects.PushBack(object);
            m_originals.Clear();
            // The type's defaults come from an instance its creator makes (none for a type without
            // a creator).
            m_typeDefault = m_objects.IsEmpty() ? core::RefPtr<core::Object>{} : core::CreateObject(*m_objects[0].Type(), m_allocator);
            NotifyPropertiesChanged();
        }
        [[nodiscard]] core::Span<const core::Instance> GetObjects() const noexcept { return m_objects.AsSpan(); }
        /// Sets the text format that changes and copies are written in (not owned; null for none).
        void SetTextFormat(IPropertyTextFormat* format) noexcept { m_format = format; }
        /// Called with each change made, so it can be kept for undo.
        void SetOnChange(core::Function<void(const ReflectedChange&)> onChange) { m_onChange = core::Move(onChange); }
        /// Restores `change`'s values from before it (an undo, with `before`) or after it (a redo).
        /// Returns false without a text format or when the objects no longer accept the values.
        bool Restore(const ReflectedChange& change, bool before)
        {
            const core::Array<core::String>& states = before ? change.Before : change.After;
            if (m_format == nullptr || states.Size() != m_objects.Size())
                return false;
            bool ok = true;
            for (usize i = 0; i < m_objects.Size(); ++i)
                ok = ReadState(change.Path.AsView(), i, states[i].AsView()) && ok;
            NotifyPropertiesChanged(); // a list's count may have changed back
            return ok;
        }
        /// Returns a default for a path and an object, or empty when it has none there; it takes
        /// precedence over the type's own default.
        using DefaultProvider = core::Function<Variant(core::StringView path, usize object)>;
        /// Stacks `provider` over the defaults; the last one stacked is asked first. A revert is then
        /// labelled `revertText` ("Revert to prefab").
        void PushDefaults(DefaultProvider provider, core::StringView revertText)
        {
            m_providers.PushBack(Provider{core::Move(provider), core::String(revertText)});
            NotifyValuesChanged();
        }
        void ClearDefaults()
        {
            m_providers.Clear();
            NotifyValuesChanged();
        }

        /// Tells the grid that the objects changed from outside it (an undo the application made).
        void Refresh() { NotifyPropertiesChanged(); }

        /// Treats the struct named `typeName` as a reference: one value, with its Guid at `parts.Id`,
        /// pointing at the struct type itself.
        void RegisterReferenceType(core::StringView typeName, ReflectedReferenceParts parts)
        {
            m_referenceTypes.PushBack(ReferenceType{core::String(typeName), core::Move(parts)});
            NotifyPropertiesChanged();
        }
        /// Hints a property that carries `attribute` with the attribute's name, so the application's
        /// editor for that attribute is chosen (a string naming a bone of a skeleton, say).
        void RegisterHintAttribute(core::StringView attribute)
        {
            m_hintAttributes.PushBack(core::String(attribute));
            NotifyPropertiesChanged();
        }
        /// Treats the type named `typeName` as a curve: one value, with its keys at `parts.Keys`.
        void RegisterCurveType(core::StringView typeName, ReflectedCurveParts parts)
        {
            m_curveTypes.PushBack(CurveType{core::String(typeName), core::Move(parts)});
            NotifyPropertiesChanged();
        }
        /// Treats the type named `typeName` as a gradient: one value holding its stops and blend.
        void RegisterGradientType(core::StringView typeName, ReflectedGradientParts parts)
        {
            m_gradientTypes.PushBack(GradientType{core::String(typeName), core::Move(parts)});
            NotifyPropertiesChanged();
        }

        // === IPropertySource ===
        [[nodiscard]] usize ObjectCount() const override { return m_objects.Size(); }

        void GetProperties(core::StringView parent, core::Array<PropertyInfo>& out) const override
        {
            const usize first = out.Size();
            ListProperties(parent, out);
            // Let the application's describe hook adjust each row last (a list element named by
            // one of its fields, a group's summary line, a row shown only when another value holds).
            if (m_onDescribe)
                for (usize i = first; i < out.Size(); ++i)
                    m_onDescribe(JoinPropertyPath(parent, out[i].Name.AsView()).AsView(), out[i]);
        }
        /// Called for each row as it is described, with its info to change. It supplies what
        /// reflection cannot (an element's name from one of its fields, a closed group's summary
        /// line, a row shown or hidden by another value), and reads the objects through GetValue.
        void SetOnDescribe(core::Function<void(core::StringView path, PropertyInfo& info)> onDescribe) { m_onDescribe = core::Move(onDescribe); }

    private:
        void ListProperties(core::StringView parent, core::Array<PropertyInfo>& out) const
        {
            if (m_objects.IsEmpty())
                return;
            Target target;
            if (!Resolve(0, parent, target))
                return;
            if (parent.Size() == 0 || IsGroup(target))
            {
                ListPropertiesOf(parent, target, out);
                return;
            }
            const Shape shape = ShapeOf(*target.Type);
            if (target.Part < 0 && shape == Shape::List)
            {
                ListElements(parent, out);
                return;
            }
            if (target.Part < 0 && target.Hdr)
                ListHdrParts(out);
            else if (target.Part < 0 && !target.AsColor && (shape == Shape::Float2 || shape == Shape::Float3 || shape == Shape::Float4 || shape == Shape::Quaternion))
                ListParts(shape, out);
        }

    public:
        [[nodiscard]] Variant GetValue(core::StringView path, usize object) const override
        {
            Target target;
            return Resolve(object, path, target) ? Read(target) : Variant{};
        }

        void BeginEdit(core::StringView path) override
        {
            Original original{core::String(path), core::Array<Variant>(m_allocator), core::Array<core::String>(m_allocator)};
            for (usize i = 0; i < m_objects.Size(); ++i)
            {
                original.Values.PushBack(GetValue(path, i));
                original.States.PushBack(WriteState(path, i));
            }
            m_originals.PushBack(core::Move(original));
        }
        void PreviewEdit(core::StringView path, const PropertyWrite& write) override
        {
            for (usize i = 0; i < m_objects.Size(); ++i)
                WriteValue(path, i, write);
            NotifyValuesChanged();
        }
        void CommitEdit(core::StringView path, const PropertyWrite& write) override
        {
            ReflectedChange change{core::Format(u8"Set {}", path), core::String(path), core::Array<core::String>(m_allocator),
                                   core::Array<core::String>(m_allocator)};
            const Original* original = FindOriginal(path);
            for (usize i = 0; i < m_objects.Size(); ++i)
            {
                if (original != nullptr && i < original->Values.Size())
                {
                    Target target;
                    if (Resolve(i, path, target))
                        WriteTarget(target, original->Values[i]); // restore the original, then apply the one change
                    change.Before.PushBack(original->States[i]);
                }
                else
                    change.Before.PushBack(WriteState(path, i));
                WriteValue(path, i, write);
                change.After.PushBack(WriteState(path, i));
            }
            DropOriginal(path);
            Changed(path);
            if (m_onChange)
                m_onChange(change);
        }
        void CancelEdit(core::StringView path) override
        {
            if (const Original* original = FindOriginal(path))
                for (usize i = 0; i < m_objects.Size() && i < original->Values.Size(); ++i)
                {
                    Target target;
                    if (Resolve(i, path, target))
                        WriteTarget(target, original->Values[i]);
                }
            DropOriginal(path);
            NotifyValuesChanged();
        }

        bool EditList(core::StringView path, const ListEdit& edit) override
        {
            ReflectedChange change{ListWords(path, edit), core::String(path), core::Array<core::String>(m_allocator), core::Array<core::String>(m_allocator)};
            bool changed = false;
            for (usize o = 0; o < m_objects.Size(); ++o)
            {
                change.Before.PushBack(WriteState(path, o));
                Target target;
                if (Resolve(o, path, target) && target.Type != nullptr && ShapeOf(*target.Type) == Shape::List && !target.ReadOnly && target.Address != nullptr)
                    changed = ChangeList(target, edit) || changed;
                change.After.PushBack(WriteState(path, o));
            }
            if (!changed)
                return false;
            NotifyPropertiesChanged();
            if (m_onChange)
                m_onChange(change);
            return true;
        }
        [[nodiscard]] Variant GetDefault(core::StringView path, usize object) const override
        {
            Target target;
            if (!Resolve(object, path, target) || target.ReadOnly)
                return Variant{}; // a place that cannot be written has no default to restore
            for (usize i = m_providers.Size(); i > 0; --i)
                if (const Variant value = m_providers[i - 1].Default(path, object); !value.IsEmpty())
                    return value;
            Target byDefault;
            return m_typeDefault && ResolveIn(core::Instance(m_typeDefault.Get(), m_objects[0].Type()), path, byDefault) ? Read(byDefault) : Variant{};
        }
        bool Revert(core::StringView path) override
        {
            ReflectedChange change{core::Format(u8"Reverted {}", path), core::String(path), core::Array<core::String>(m_allocator),
                                   core::Array<core::String>(m_allocator)};
            bool changed = false;
            for (usize o = 0; o < m_objects.Size(); ++o)
            {
                change.Before.PushBack(WriteState(path, o));
                changed = RevertUnder(path, o) || changed;
                change.After.PushBack(WriteState(path, o));
            }
            if (!changed)
                return false;
            NotifyPropertiesChanged();
            if (m_onChange)
                m_onChange(change);
            return true;
        }
        [[nodiscard]] core::String GetRevertText(core::StringView) const override
        {
            return m_providers.IsEmpty() ? core::String(u8"Revert to default") : m_providers.Back().RevertText;
        }
        // Writes the first object's value at `path` in the text format: its shape tag, then the
        // value.
        [[nodiscard]] core::String CopyValue(core::StringView path) const override
        {
            Target target;
            if (m_format == nullptr || !Resolve(0, path, target) || target.Type == nullptr)
                return core::String{};
            core::String shape = ShapeTag(target);
            if (shape.Size() == 0)
                return core::String{};
            const ReflectedPropertySource* self = this;
            return m_format->Write([self, target, shape](core::ISerializer& ar) mutable
            {
                core::Serialize(ar, "shape", shape);
                self->SerializeTarget(ar, "value", target);
            });
        }
        // Pastes into each object as one change. Refuses when the shape differs or nothing can be
        // written.
        bool PasteValue(core::StringView path, core::StringView text, core::String& reason) override
        {
            Target target;
            if (m_format == nullptr || !Resolve(0, path, target) || target.Type == nullptr)
            {
                reason = core::String(u8"Nothing to paste into");
                return false;
            }
            if (target.ReadOnly)
            {
                reason = core::Format(u8"{} is read only", path);
                return false;
            }
            core::String copied;
            const bool read = m_format->Read(text, [&copied](core::ISerializer& ar) { core::Serialize(ar, "shape", copied); });
            if (!read || copied.Size() == 0)
            {
                reason = core::String(u8"The clipboard holds no copied property");
                return false;
            }
            const core::String expected = ShapeTag(target);
            if (copied != expected)
            {
                reason = core::Format(u8"A {} does not paste onto a {}", copied, expected);
                return false;
            }
            ReflectedChange change{core::Format(u8"Pasted {}", path), core::String(path), core::Array<core::String>(m_allocator),
                                   core::Array<core::String>(m_allocator)};
            for (usize o = 0; o < m_objects.Size(); ++o)
            {
                change.Before.PushBack(WriteState(path, o));
                Target into;
                if (Resolve(o, path, into))
                {
                    const ReflectedPropertySource* self = this;
                    (void)m_format->Read(text, [self, into](core::ISerializer& ar) mutable
                    {
                        core::String shape;
                        core::Serialize(ar, "shape", shape);
                        self->SerializeTarget(ar, "value", into);
                    });
                }
                change.After.PushBack(WriteState(path, o));
            }
            NotifyPropertiesChanged();
            if (m_onChange)
                m_onChange(change);
            return true;
        }

    protected:
        // How the grid sees a reflected type.
        enum class Shape : u8
        {
            Bool,
            Integer,
            Float,
            String,
            Enum,
            Float2,
            Float3,
            Float4,
            Quaternion,
            Color,
            Curve,    // a type registered as a curve
            Gradient, // a type registered as a gradient
            Reference, // a reference-shaped type (core::TypeInfo::reference), shown as its Guid
            Group, // a nested or value struct, an object
            List,
            Other,
        };

        // The place a path names in one object: a property of an owner, an element of a list, a
        // part of a Float or a Quaternion, or an object (the root, a nested struct, an element).
        struct Target
        {
            const core::TypeInfo* Type = nullptr;
            void* Address = nullptr;
            core::Instance Owner;                       // the owner of a property
            const core::PropertyInfo* Property = nullptr;
            core::Instance List;                        // an element's list
            usize Index = 0;
            bool Element = false;
            i32 Part = -1;                              // a part of the Float or Quaternion above
            bool Hdr = false;                           // a Float4 that is an HDR colour
            bool AsColor = false;                       // a Float4 that is a colour (`color`)
            bool ReadOnly = false;
        };

        // Returns a type's shape: the one the application registered (a curve, a gradient), else
        // the shape its reflection gives.
        [[nodiscard]] Shape ShapeOf(const core::TypeInfo& type) const
        {
            const core::StringView name(reinterpret_cast<const char8_t*>(type.name));
            if (FindCurveType(name) != nullptr)
                return Shape::Curve;
            if (FindGradientType(name) != nullptr)
                return Shape::Gradient;
            if (FindReferenceType(name) != nullptr)
                return Shape::Reference;
            return BaseShapeOf(type);
        }
        [[nodiscard]] const ReflectedCurveParts* FindCurveType(core::StringView name) const
        {
            for (const CurveType& entry : m_curveTypes)
                if (entry.Name.AsView() == name)
                    return &entry.Parts;
            return nullptr;
        }
        [[nodiscard]] const ReflectedReferenceParts* FindReferenceType(core::StringView name) const
        {
            for (const ReferenceType& entry : m_referenceTypes)
                if (entry.Name.AsView() == name)
                    return &entry.Parts;
            return nullptr;
        }
        // Returns a reference's Guid at `address`, through Core's reference ops or the registered
        // struct's Guid property, or null when there is none.
        [[nodiscard]] const Guid* ReferenceIdAt(const core::TypeInfo& type, const void* address) const
        {
            if (type.reference != nullptr)
                return type.reference->Id(address);
            if (&type == &core::TypeOf<Guid>() || type.id == core::TypeOf<Guid>().id)
                return static_cast<const Guid*>(address); // a bare Guid is its own id
            const ReflectedReferenceParts* parts = FindReferenceType(core::StringView(reinterpret_cast<const char8_t*>(type.name)));
            const core::PropertyInfo* id = parts != nullptr ? core::FindProperty(type, reinterpret_cast<const char*>(parts->Id.CStr())) : nullptr;
            if (id == nullptr || id->address == nullptr || id->type != &core::TypeOf<Guid>())
                return nullptr;
            return static_cast<const Guid*>(id->address(core::Instance(const_cast<void*>(address), &type)));
        }
        // Returns what a reference points at: the target of Core's reference, or a registered
        // struct itself.
        [[nodiscard]] const core::TypeInfo* ReferenceTargetOf(const core::TypeInfo& type) const
        {
            if (type.reference != nullptr)
                return type.reference->Target != nullptr ? type.reference->Target() : nullptr;
            if (&type == &core::TypeOf<Guid>() || type.id == core::TypeOf<Guid>().id)
                return nullptr; // a bare Guid does not say what it points at, though its property may (`referenceType`)
            return &type;
        }
        [[nodiscard]] const ReflectedGradientParts* FindGradientType(core::StringView name) const
        {
            for (const GradientType& entry : m_gradientTypes)
                if (entry.Name.AsView() == name)
                    return &entry.Parts;
            return nullptr;
        }
        [[nodiscard]] static Shape BaseShapeOf(const core::TypeInfo& type)
        {
            const auto is = [&type]<typename T>() { return &type == &core::TypeOf<T>() || type.id == core::TypeOf<T>().id; };
            if (is.template operator()<bool>())
                return Shape::Bool;
            if (is.template operator()<i8>() || is.template operator()<u8>() || is.template operator()<i16>() || is.template operator()<u16>() ||
                is.template operator()<i32>() || is.template operator()<u32>() || is.template operator()<i64>() || is.template operator()<u64>())
                return Shape::Integer;
            if (is.template operator()<f32>() || is.template operator()<f64>())
                return Shape::Float;
            if (is.template operator()<core::String>())
                return Shape::String;
            if (is.template operator()<core::Float2>())
                return Shape::Float2;
            if (is.template operator()<core::Float3>())
                return Shape::Float3;
            if (is.template operator()<core::Float4>())
                return Shape::Float4;
            if (is.template operator()<core::Quaternion>())
                return Shape::Quaternion;
            if (is.template operator()<core::Color>())
                return Shape::Color;
            if (core::IsEnum(type))
                return Shape::Enum;
            if (type.reference != nullptr || is.template operator()<Guid>())
                return Shape::Reference; // a bare Guid identifies something, as a Ref<T>'s id does
            if (type.container != nullptr)
                return Shape::List;
            for (const core::TypeInfo* t = &type; t != nullptr; t = t->base)
                if (t->propertyCount != 0)
                    return Shape::Group;
            return Shape::Other;
        }
        [[nodiscard]] bool IsGroup(const Target& target) const
        {
            return target.Part < 0 && target.Type != nullptr && ShapeOf(*target.Type) == Shape::Group;
        }

        // Resolves the place `path` names in object `object`.
        bool Resolve(usize object, core::StringView path, Target& out) const
        {
            return object < m_objects.Size() && ResolveIn(m_objects[object], path, out);
        }
        bool ResolveIn(const core::Instance& root, core::StringView path, Target& out) const
        {
            Target at;
            at.Type = root.Type();
            at.Address = root.Pointer();
            usize start = 0;
            while (start < path.Size())
            {
                usize end = start;
                while (end < path.Size() && path[end] != u8'.')
                    ++end;
                const core::StringView segment = path.SubStr(start, end - start);
                start = end + 1;
                if (!Step(at, segment))
                    return false;
            }
            out = at;
            return true;
        }
        // Moves `at` one segment further: to a property of a group, an element of a list, or a part.
        bool Step(Target& at, core::StringView segment) const
        {
            if (at.Part >= 0 || at.Type == nullptr)
                return false;
            const Shape shape = ShapeOf(*at.Type);
            if (shape == Shape::Group)
            {
                if (at.Address == nullptr)
                    return false;
                const core::Instance owner(at.Address, at.Type);
                const core::PropertyInfo* property = FindPropertyNamed(*at.Type, segment);
                if (property == nullptr)
                    return false;
                Target next;
                next.Owner = owner;
                next.Property = property;
                next.Type = property->type;
                next.Address = property->address != nullptr ? property->address(owner) : nullptr;
                next.ReadOnly = at.ReadOnly || (static_cast<u32>(property->flags) & static_cast<u32>(core::PropertyFlags::ReadOnly)) != 0;
                next.Hdr = core::FindAttribute(*property, core::StringView(u8"hdrColor")) != nullptr && ShapeOf(*property->type) == Shape::Float4;
                next.AsColor = core::FindAttribute(*property, core::StringView(u8"color")) != nullptr && ShapeOf(*property->type) == Shape::Float4;
                at = next;
                return true;
            }
            if (shape == Shape::List)
            {
                const core::Optional<i64> index = core::ParseInt(segment);
                const core::ContainerInfo& container = *at.Type->container;
                if (!index.HasValue() || index.Value() < 0 || at.Address == nullptr)
                    return false;
                const core::Instance list(at.Address, at.Type);
                const usize i = static_cast<usize>(index.Value());
                if (i >= core::ContainerSize(container, list))
                    return false;
                Target next;
                next.List = list;
                next.Index = i;
                next.Element = true;
                next.ReadOnly = at.ReadOnly;
                next.Type = container.elementType;
                const core::Instance element = core::ContainerAddressAt(container, list, i);
                if (!element.IsEmpty())
                {
                    next.Address = element.Pointer();
                    next.Type = element.Type(); // a polymorphic element's own type
                }
                at = next;
                return true;
            }
            if (at.AsColor)
                return false; // a colour is one value, not its parts
            const i32 part = at.Hdr ? HdrPartIndex(segment) : PartIndex(shape, segment);
            if (part >= 0)
            {
                at.Part = part;
                return true;
            }
            return false;
        }
        [[nodiscard]] static const core::PropertyInfo* FindPropertyNamed(const core::TypeInfo& type, core::StringView name)
        {
            for (const core::TypeInfo* t = &type; t != nullptr; t = t->base)
                for (u32 i = 0; i < t->propertyCount; ++i)
                    if (core::StringView(reinterpret_cast<const char8_t*>(t->properties[i].name)) == name)
                        return &t->properties[i];
            return nullptr;
        }
        // An HDR colour's parts: its colour (rgb) and its intensity (w).
        static constexpr i32 kHdrColorPart = 10;
        [[nodiscard]] static i32 HdrPartIndex(core::StringView segment)
        {
            if (segment == core::StringView(u8"color"))
                return kHdrColorPart;
            return segment == core::StringView(u8"intensity") ? 3 : -1;
        }
        [[nodiscard]] static i32 PartIndex(Shape shape, core::StringView segment)
        {
            if (shape == Shape::Quaternion)
            {
                const core::StringView names[] = {core::StringView(u8"pitch"), core::StringView(u8"yaw"), core::StringView(u8"roll")};
                for (i32 i = 0; i < 3; ++i)
                    if (segment == names[i])
                        return i;
                return -1;
            }
            const i32 count = shape == Shape::Float2 ? 2 : (shape == Shape::Float3 ? 3 : (shape == Shape::Float4 ? 4 : 0));
            const core::StringView names[] = {core::StringView(u8"x"), core::StringView(u8"y"), core::StringView(u8"z"), core::StringView(u8"w")};
            for (i32 i = 0; i < count; ++i)
                if (segment == names[i])
                    return i;
            return -1;
        }

        // Lists a group's properties as rows, its type's bases first.
        void ListPropertiesOf(core::StringView path, const Target& group, core::Array<PropertyInfo>& out) const
        {
            if (group.Type == nullptr || group.Address == nullptr)
                return;
            core::Array<const core::TypeInfo*> chain(m_allocator);
            for (const core::TypeInfo* t = group.Type; t != nullptr; t = t->base)
                chain.Insert(0, t);
            for (const core::TypeInfo* t : chain)
                for (u32 i = 0; i < t->propertyCount; ++i)
                {
                    PropertyInfo info;
                    if (Describe(path, group, t->properties[i], info))
                        out.PushBack(core::Move(info));
                }
        }
        // Describes a property as a row: its kind, hint, names and the attributes it carries.
        bool Describe(core::StringView path, const Target& group, const core::PropertyInfo& property, PropertyInfo& info) const
        {
            const core::StringView name(reinterpret_cast<const char8_t*>(property.name));
            info.Name = core::String(name);
            const core::String* shown = StringAttribute(property, u8"displayName");
            info.DisplayName = shown != nullptr ? *shown : ReadablePropertyName(name);
            if (const core::String* description = StringAttribute(property, u8"description"))
                info.Description = *description;
            info.ReadOnly = group.ReadOnly || (static_cast<u32>(property.flags) & static_cast<u32>(core::PropertyFlags::ReadOnly)) != 0;
            DescribeType(*property.type, core::FindAttribute(property, core::StringView(u8"flags")) != nullptr, info);
            if (info.Kind == PropertyKind::Color)
                info.ShowsAlpha = core::FindAttribute(property, core::StringView(u8"noAlpha")) == nullptr;
            if (ShapeOf(*property.type) == Shape::Float4 && core::FindAttribute(property, core::StringView(u8"hdrColor")) != nullptr)
                info.Hint = core::String(u8"ColorHdr");
            else if (ShapeOf(*property.type) == Shape::Float4 && core::FindAttribute(property, core::StringView(u8"color")) != nullptr)
            {
                // Four floats kept as a colour (a particle's tint) are one colour, as a Color is.
                info.Kind = PropertyKind::Color;
                info.Hint = core::String{};
                info.ShowsAlpha = core::FindAttribute(property, core::StringView(u8"noAlpha")) == nullptr;
            }
            if (const core::Attribute* range = core::FindAttribute(property, core::StringView(u8"range")))
                if (const core::Float4* bounds = range->value.TryGet<core::Float4>())
                {
                    info.HasRange = true;
                    info.Min = bounds->x;
                    info.Max = bounds->y;
                    info.Step = bounds->z;
                    // A ranged float slides over its range (an integer's range only bounds its field).
                    if (info.Kind == PropertyKind::Float && info.Hint.IsEmpty())
                        info.Hint = core::String(u8"slider");
                }
            for (const core::String& attribute : m_hintAttributes)
                if (info.Hint.IsEmpty() && core::FindAttribute(property, attribute.AsView()) != nullptr)
                    info.Hint = attribute;
            // A bare Guid's property may name what it points at, by its type's name.
            if (info.Kind == PropertyKind::Reference && info.ReferenceType == nullptr)
                if (const core::String* named = StringAttribute(property, u8"referenceType"))
                    info.ReferenceType = TypeNamed(named->AsView());
            if (const core::String* condition = StringAttribute(property, u8"visibleWhen"))
                info.Visible = Holds(path, condition->AsView());
            return true;
        }
        // Describes a type's kind for a row, for a property or an element; `flags` reads an enum as
        // flags.
        void DescribeType(const core::TypeInfo& type, bool flags, PropertyInfo& info) const
        {
            switch (ShapeOf(type))
            {
            case Shape::Bool:
                info.Kind = PropertyKind::Bool;
                break;
            case Shape::Integer:
                info.Kind = PropertyKind::Integer;
                break;
            case Shape::Float:
                info.Kind = PropertyKind::Float;
                break;
            case Shape::String:
                info.Kind = PropertyKind::String;
                break;
            case Shape::Enum:
                info.Kind = flags ? PropertyKind::Flags : PropertyKind::Enum;
                for (const core::EnumValue& value : core::Enumerators(type))
                    info.Choices.PushBack(PropertyChoice{ReadablePropertyName(core::StringView(reinterpret_cast<const char8_t*>(value.name))), value.value});
                break;
            case Shape::Float2:
                info.Kind = PropertyKind::Group;
                info.Hint = core::String(u8"Float2");
                break;
            case Shape::Float3:
                info.Kind = PropertyKind::Group;
                info.Hint = core::String(u8"Float3");
                break;
            case Shape::Float4:
                info.Kind = PropertyKind::Group;
                info.Hint = core::String(u8"Float4");
                break;
            case Shape::Quaternion:
                info.Kind = PropertyKind::Group;
                info.Hint = core::String(u8"Float3"); // Euler degrees
                break;
            case Shape::Color:
                info.Kind = PropertyKind::Color;
                break;
            case Shape::Curve:
                info.Kind = PropertyKind::Curve;
                info.MaxCount = PartCapacity(type, FindCurveType(core::StringView(reinterpret_cast<const char8_t*>(type.name)))->Keys.AsView());
                break;
            case Shape::Gradient:
            {
                info.Kind = PropertyKind::Gradient;
                const ReflectedGradientParts& parts = *FindGradientType(core::StringView(reinterpret_cast<const char8_t*>(type.name)));
                const u32 colors = PartCapacity(type, parts.Colors.AsView());
                const u32 alphas = PartCapacity(type, parts.Alphas.AsView());
                info.MaxCount = colors == 0 ? alphas : (alphas == 0 ? colors : core::Min(colors, alphas)); // one limit for both kinds
                break;
            }
            case Shape::Reference:
            {
                info.Kind = PropertyKind::Reference;
                info.ReferenceType = ReferenceTargetOf(type);
                if (type.reference == nullptr && info.ReferenceType != nullptr) // a registered struct is hinted with its type's name, for the application's editor
                    info.Hint = core::String(reinterpret_cast<const char8_t*>(type.name));
                break;
            }
            case Shape::Group: // hinted with its type's name for an editor of its own; else it shows its rows
                info.Kind = PropertyKind::Group;
                info.Hint = core::String(reinterpret_cast<const char8_t*>(type.name));
                break;
            case Shape::List:
            {
                info.Kind = PropertyKind::List;
                const core::ContainerInfo& container = *type.container;
                const bool polymorphic = core::IsPolymorphicContainer(container);
                if (!info.ReadOnly)
                    info.ListOps = static_cast<u8>(((polymorphic ? container.createElement != nullptr : container.emplaceDefault != nullptr) ? PropertyListAdd : 0) |
                                                   (container.removeAt != nullptr ? PropertyListRemove : 0) |
                                                   (container.moveElement != nullptr ? PropertyListMove : 0));
                if (polymorphic && container.elementType != nullptr)
                    for (const core::TypeInfo* concrete : CreatableTypes(container))
                        info.ElementTypes.PushBack(core::String(TypeShownName(*concrete)));
                info.MaxCount = static_cast<u32>(core::ContainerCapacity(container)); // a BoundedArray's N
                break;
            }
            case Shape::Other:
                info.Kind = PropertyKind::Group;
                info.Hint = core::String(reinterpret_cast<const char8_t*>(type.name));
                break;
            }
        }
        // Returns the types an add to a polymorphic list can make: those that derive from its base
        // and that its factory accepts.
        [[nodiscard]] core::Array<const core::TypeInfo*> CreatableTypes(const core::ContainerInfo& container) const
        {
            core::Array<const core::TypeInfo*> derived(m_allocator);
            core::Array<const core::TypeInfo*> out(m_allocator);
            if (container.elementType == nullptr)
                return out;
            core::EnumerateDerived(*container.elementType, derived);
            for (const core::TypeInfo* type : derived)
                if (core::ContainerCanCreateElement(container, *type))
                    out.PushBack(type);
            return out;
        }
        [[nodiscard]] static core::StringView TypeShownName(const core::TypeInfo& type)
        {
            return core::TypeAttrString(type, "displayName", core::StringView(reinterpret_cast<const char8_t*>(type.name)));
        }
        // Lists a list's elements, as many as the object with the fewest has. Each is named by its
        // index, and an object element also by its type's name.
        void ListElements(core::StringView path, core::Array<PropertyInfo>& out) const
        {
            usize count = ~usize{0};
            for (usize o = 0; o < m_objects.Size(); ++o)
                count = core::Min(count, static_cast<usize>(GetValue(path, o).AsInt()));
            if (m_objects.IsEmpty())
                return;
            // A list of bare ids gets its elements' target type the way a bare id does, from its
            // own property's `referenceType`.
            Target list;
            const core::String* named = Resolve(0, path, list) && list.Property != nullptr ? StringAttribute(*list.Property, u8"referenceType") : nullptr;
            for (usize i = 0; i < count; ++i)
            {
                const core::String element = core::Format(u8"{}.{}", path, i);
                Target target;
                if (!Resolve(0, element.AsView(), target) || target.Type == nullptr)
                    continue;
                PropertyInfo info;
                info.Name = core::Format(u8"{}", i);
                info.ReadOnly = target.ReadOnly;
                DescribeType(*target.Type, false, info);
                if (info.Kind == PropertyKind::Reference && info.ReferenceType == nullptr && named != nullptr)
                    info.ReferenceType = TypeNamed(named->AsView());
                const bool object = info.Kind == PropertyKind::Group && target.Address != nullptr;
                info.DisplayName = object ? core::Format(u8"{} {}", TypeShownName(*target.Type), i + 1) : core::Format(u8"Element {}", i + 1);
                out.PushBack(core::Move(info));
            }
        }
        [[nodiscard]] static const core::TypeInfo* TypeNamed(core::StringView name)
        {
            for (const core::TypeInfo* type : core::GlobalTypeRegistry().All())
                if (type != nullptr && type->name != nullptr && core::StringView(reinterpret_cast<const char8_t*>(type->name)) == name)
                    return type;
            return nullptr;
        }
        [[nodiscard]] static const core::String* StringAttribute(const core::PropertyInfo& property, const char8_t* key)
        {
            const core::Attribute* attribute = core::FindAttribute(property, core::StringView(key));
            return attribute != nullptr ? attribute->value.TryGet<core::String>() : nullptr;
        }
        // Tests a `visibleWhen` condition ("prop", or "prop=1,2" against its raw integer) on a
        // sibling's value, in every object. A condition that does not parse (no name, an empty or
        // non-integer value) hides nothing.
        bool Holds(core::StringView group, core::StringView condition) const
        {
            usize eq = 0;
            while (eq < condition.Size() && condition[eq] != u8'=')
                ++eq;
            if (eq == 0)
                return true;
            if (eq < condition.Size())
            {
                usize start = eq + 1;
                while (start <= condition.Size())
                {
                    usize end = start;
                    while (end < condition.Size() && condition[end] != u8',')
                        ++end;
                    if (!core::ParseInt(core::Trim(condition.SubStr(start, end - start))).HasValue())
                        return true;
                    start = end + 1;
                }
            }
            const core::String sibling = JoinPropertyPath(group, condition.SubStr(0, eq));
            const auto matches = [&](i64 value)
            {
                if (eq == condition.Size())
                    return value != 0;
                usize start = eq + 1;
                while (start <= condition.Size())
                {
                    usize end = start;
                    while (end < condition.Size() && condition[end] != u8',')
                        ++end;
                    const core::Optional<i64> listed = core::ParseInt(core::Trim(condition.SubStr(start, end - start)));
                    if (listed.HasValue() && listed.Value() == value)
                        return true;
                    start = end + 1;
                }
                return false;
            };
            for (usize o = 0; o < m_objects.Size(); ++o)
            {
                const Variant value = GetValue(sibling.AsView(), o);
                if (!matches(value.GetType() == Variant::Type::Bool ? (value.AsBool() ? 1 : 0) : value.AsInt()))
                    return false;
            }
            return true;
        }
        static void ListHdrParts(core::Array<PropertyInfo>& out)
        {
            PropertyInfo color;
            color.Name = core::String(u8"color");
            color.DisplayName = core::String(u8"Color");
            color.Kind = PropertyKind::Color;
            color.ShowsAlpha = false;
            out.PushBack(core::Move(color));
            PropertyInfo intensity;
            intensity.Name = core::String(u8"intensity");
            intensity.DisplayName = core::String(u8"Intensity");
            intensity.Kind = PropertyKind::Float;
            intensity.Hint = core::String(u8"stops");
            out.PushBack(core::Move(intensity));
        }
        static void ListParts(Shape shape, core::Array<PropertyInfo>& out)
        {
            const char8_t* const floats[] = {u8"x", u8"y", u8"z", u8"w"};
            const char8_t* const shownFloats[] = {u8"X", u8"Y", u8"Z", u8"W"};
            const char8_t* const angles[] = {u8"pitch", u8"yaw", u8"roll"};
            // Name the angles after the axes they turn about (pitch about X, yaw about Y, roll about
            // Z), so a rotation reads like the position and scale beside it.
            const char8_t* const shownAngles[] = {u8"X", u8"Y", u8"Z"};
            const usize count = shape == Shape::Float2 ? 2 : (shape == Shape::Float4 ? 4 : 3);
            for (usize i = 0; i < count; ++i)
            {
                PropertyInfo info;
                const bool euler = shape == Shape::Quaternion;
                info.Name = core::String(euler ? angles[i] : floats[i]);
                info.DisplayName = core::String(euler ? shownAngles[i] : shownFloats[i]);
                info.Kind = PropertyKind::Float;
                info.Step = euler ? 1.0 : 0.01;
                out.PushBack(core::Move(info));
            }
        }

        // === Values ===

        // Reads the value at a place, through its address or else its property's getter.
        [[nodiscard]] Variant Read(const Target& target) const
        {
            if (target.Type == nullptr)
                return Variant{};
            const Shape shape = ShapeOf(*target.Type);
            if (target.Part == kHdrColorPart)
            {
                core::Float4 parts{};
                return ReadCompound(target, shape, parts) ? Variant(core::Color{parts.x, parts.y, parts.z, 1.0f}) : Variant{};
            }
            if (target.AsColor)
            {
                core::Float4 parts{};
                return ReadCompound(target, shape, parts) ? Variant(core::Color{parts.x, parts.y, parts.z, parts.w}) : Variant{};
            }
            if (target.Part >= 0)
                return Variant(PartOf(target, shape));
            if (shape == Shape::List)
                return target.Address != nullptr ? Variant(static_cast<i64>(core::ContainerSize(*target.Type->container, core::Instance(target.Address, target.Type))))
                                                 : Variant{};
            if (shape == Shape::Curve)
                return ReadCurve(target);
            if (shape == Shape::Gradient)
                return ReadGradient(target);
            if (target.Address != nullptr)
                return ReadAt(target.Address, *target.Type, shape);
            if (target.Element && !target.List.IsEmpty())
                return FromCore(core::ContainerGetAt(*target.List.Type()->container, target.List, target.Index), shape, *target.Type);
            if (target.Property != nullptr && target.Property->get != nullptr && !target.Owner.IsEmpty())
                return FromCore(target.Property->get(target.Owner), shape, *target.Type);
            return Variant{};
        }
        // Writes `value` to a place. Returns false when it cannot be written.
        bool WriteTarget(const Target& target, const Variant& value) const
        {
            if (target.Type == nullptr || target.ReadOnly || value.IsEmpty())
                return false;
            const Shape shape = ShapeOf(*target.Type);
            if (target.Part == kHdrColorPart)
            {
                core::Float4 parts{};
                if (!ReadCompound(target, shape, parts))
                    return false;
                const core::Color color = value.AsColor();
                return WriteCompound(target, shape, core::Float4{color.r, color.g, color.b, parts.w});
            }
            if (target.AsColor)
            {
                const core::Color color = value.AsColor();
                return WriteCompound(target, shape, core::Float4{color.r, color.g, color.b, color.a});
            }
            if (target.Part >= 0)
                return SetPart(target, shape, value.AsFloat());
            if (shape == Shape::Curve)
                return value.GetType() == Variant::Type::Curve && WriteCurve(target, value.AsCurve());
            if (shape == Shape::Gradient)
                return value.GetType() == Variant::Type::Gradient && WriteGradient(target, value.AsGradient());
            if (target.Address != nullptr)
                return WriteAt(target.Address, *target.Type, shape, value);
            if (target.Element && !target.List.IsEmpty())
            {
                const core::Variant boxed = ToCore(value, shape, *target.Type);
                return boxed.Type() != nullptr && core::ContainerSetAt(*target.List.Type()->container, target.List, target.Index, boxed).IsOk();
            }
            if (target.Property != nullptr && !target.Owner.IsEmpty())
            {
                const core::Variant boxed = ToCore(value, shape, *target.Type);
                return boxed.Type() != nullptr && core::SetProperty(*target.Property, target.Owner, boxed).IsOk();
            }
            return false;
        }
        void WriteValue(core::StringView path, usize object, const PropertyWrite& write) const
        {
            Target target;
            if (Resolve(object, path, target))
                (void)WriteTarget(target, ApplyPropertyWrite(Read(target), write));
        }

        [[nodiscard]] Variant ReadAt(const void* address, const core::TypeInfo& type, Shape shape) const
        {
            const auto is = [&type]<typename T>() { return &type == &core::TypeOf<T>() || type.id == core::TypeOf<T>().id; };
            switch (shape)
            {
            case Shape::Bool:
                return Variant(*static_cast<const bool*>(address));
            case Shape::Integer:
                if (is.template operator()<i8>())
                    return Variant(static_cast<i64>(*static_cast<const i8*>(address)));
                if (is.template operator()<u8>())
                    return Variant(static_cast<i64>(*static_cast<const u8*>(address)));
                if (is.template operator()<i16>())
                    return Variant(static_cast<i64>(*static_cast<const i16*>(address)));
                if (is.template operator()<u16>())
                    return Variant(static_cast<i64>(*static_cast<const u16*>(address)));
                if (is.template operator()<i32>())
                    return Variant(static_cast<i64>(*static_cast<const i32*>(address)));
                if (is.template operator()<u32>())
                    return Variant(static_cast<i64>(*static_cast<const u32*>(address)));
                if (is.template operator()<u64>())
                    return Variant(static_cast<i64>(*static_cast<const u64*>(address)));
                return Variant(*static_cast<const i64*>(address));
            case Shape::Float:
                return is.template operator()<f32>() ? Variant(static_cast<f64>(*static_cast<const f32*>(address))) : Variant(*static_cast<const f64*>(address));
            case Shape::String:
                return Variant(static_cast<const core::String*>(address)->AsView());
            case Shape::Enum:
                return Variant(core::ReadEnumValue(address, type));
            case Shape::Color:
                return Variant(*static_cast<const core::Color*>(address));
            case Shape::Reference:
            {
                const Guid* id = ReferenceIdAt(type, address);
                if (id == nullptr || id->IsNil())
                    return Variant(core::StringView{});
                utf8char text[37];
                id->ToChars(text);
                return Variant(core::StringView(text, 36));
            }
            default:
                return Variant{};
            }
        }
        [[nodiscard]] bool WriteAt(void* address, const core::TypeInfo& type, Shape shape, const Variant& value) const
        {
            const auto is = [&type]<typename T>() { return &type == &core::TypeOf<T>() || type.id == core::TypeOf<T>().id; };
            switch (shape)
            {
            case Shape::Bool:
                *static_cast<bool*>(address) = value.AsBool();
                return true;
            case Shape::Integer:
            {
                const i64 v = value.AsInt();
                if (is.template operator()<i8>())
                    *static_cast<i8*>(address) = static_cast<i8>(v);
                else if (is.template operator()<u8>())
                    *static_cast<u8*>(address) = static_cast<u8>(v);
                else if (is.template operator()<i16>())
                    *static_cast<i16*>(address) = static_cast<i16>(v);
                else if (is.template operator()<u16>())
                    *static_cast<u16*>(address) = static_cast<u16>(v);
                else if (is.template operator()<i32>())
                    *static_cast<i32*>(address) = static_cast<i32>(v);
                else if (is.template operator()<u32>())
                    *static_cast<u32*>(address) = static_cast<u32>(v);
                else if (is.template operator()<u64>())
                    *static_cast<u64*>(address) = static_cast<u64>(v);
                else
                    *static_cast<i64*>(address) = v;
                return true;
            }
            case Shape::Float:
                if (is.template operator()<f32>())
                    *static_cast<f32*>(address) = static_cast<f32>(value.AsFloat());
                else
                    *static_cast<f64*>(address) = value.AsFloat();
                return true;
            case Shape::String:
                *static_cast<core::String*>(address) = core::String(value.AsString());
                return true;
            case Shape::Enum:
                core::WriteEnumValue(address, type, value.AsInt());
                return true;
            case Shape::Color:
                *static_cast<core::Color*>(address) = value.AsColor();
                return true;
            case Shape::Reference:
            {
                // The value is its Guid's text; empty clears it to a nil Guid.
                const core::StringView text = core::Trim(value.AsString());
                Guid id;
                if (!text.IsEmpty() && !Guid::TryParse(text, id))
                    return false;
                if (type.reference != nullptr)
                {
                    type.reference->SetId(address, id);
                    type.reference->ClearBinding(address); // the next resolve binds the new id
                    return true;
                }
                Guid* own = const_cast<Guid*>(ReferenceIdAt(type, address));
                if (own == nullptr)
                    return false;
                *own = id;
                return true;
            }
            default:
                return false;
            }
        }
        // Converts a computed property's core value to the grid's value, and back.
        [[nodiscard]] Variant FromCore(const core::Variant& value, Shape shape, const core::TypeInfo& type) const
        {
            if (value.Type() == nullptr || value.ValuePointer() == nullptr)
                return Variant{};
            if (shape == Shape::Enum)
                return Variant(value.AsEnumInt());
            return ReadAt(value.ValuePointer(), type, shape);
        }
        [[nodiscard]] static core::Variant ToCore(const Variant& value, Shape shape, const core::TypeInfo& type)
        {
            const auto is = [&type]<typename T>() { return &type == &core::TypeOf<T>() || type.id == core::TypeOf<T>().id; };
            switch (shape)
            {
            case Shape::Bool:
                return core::Variant::From<bool>(value.AsBool());
            case Shape::Integer:
                if (is.template operator()<i32>())
                    return core::Variant::From<i32>(static_cast<i32>(value.AsInt()));
                if (is.template operator()<u32>())
                    return core::Variant::From<u32>(static_cast<u32>(value.AsInt()));
                if (is.template operator()<u64>())
                    return core::Variant::From<u64>(static_cast<u64>(value.AsInt()));
                if (is.template operator()<i64>())
                    return core::Variant::From<i64>(value.AsInt());
                return core::Variant{};
            case Shape::Float:
                return is.template operator()<f32>() ? core::Variant::From<f32>(static_cast<f32>(value.AsFloat())) : core::Variant::From<f64>(value.AsFloat());
            case Shape::String:
                return core::Variant::From<core::String>(core::String(value.AsString()));
            case Shape::Color:
                return core::Variant::From<core::Color>(value.AsColor());
            default:
                return core::Variant{}; // an enum or a compound is written through its address only
            }
        }

        // Returns a Float's part, or a Quaternion's Euler angle in degrees.
        [[nodiscard]] f64 PartOf(const Target& target, Shape shape) const
        {
            core::Float4 parts{};
            if (!ReadCompound(target, shape, parts))
                return 0.0;
            const f32 values[] = {parts.x, parts.y, parts.z, parts.w};
            return static_cast<f64>(values[target.Part]);
        }
        bool SetPart(const Target& target, Shape shape, f64 value) const
        {
            core::Float4 parts{};
            if (!ReadCompound(target, shape, parts))
                return false;
            f32* slots[] = {&parts.x, &parts.y, &parts.z, &parts.w};
            *slots[target.Part] = static_cast<f32>(value);
            return WriteCompound(target, shape, parts);
        }
        // Reads a Float2/3/4 as x, y, z, w, or a Quaternion as its Euler degrees (pitch, yaw, roll).
        bool ReadCompound(const Target& target, Shape shape, core::Float4& out) const
        {
            Target whole = target;
            whole.Part = -1;
            const void* address = whole.Address;
            core::Variant boxed;
            if (address == nullptr && whole.Property != nullptr && !whole.Owner.IsEmpty())
            {
                boxed = whole.Property->get(whole.Owner);
                address = boxed.ValuePointer();
            }
            if (address == nullptr)
                return false;
            switch (shape)
            {
            case Shape::Float2:
            {
                const core::Float2& v = *static_cast<const core::Float2*>(address);
                out = core::Float4{v.x, v.y, 0.0f, 0.0f};
                return true;
            }
            case Shape::Float3:
            {
                const core::Float3& v = *static_cast<const core::Float3*>(address);
                out = core::Float4{v.x, v.y, v.z, 0.0f};
                return true;
            }
            case Shape::Float4:
                out = *static_cast<const core::Float4*>(address);
                return true;
            case Shape::Quaternion:
            {
                f32 yaw = 0.0f, pitch = 0.0f, roll = 0.0f;
                core::ToYawPitchRoll(*static_cast<const core::Quaternion*>(address), yaw, pitch, roll);
                out = core::Float4{core::RadiansToDegrees(pitch), core::RadiansToDegrees(yaw), core::RadiansToDegrees(roll), 0.0f};
                return true;
            }
            default:
                return false;
            }
        }
        bool WriteCompound(const Target& target, Shape shape, const core::Float4& parts) const
        {
            Target whole = target;
            whole.Part = -1;
            if (whole.ReadOnly)
                return false;
            const auto store = [&]<typename T>(const T& value) -> bool
            {
                if (whole.Address != nullptr)
                {
                    *static_cast<T*>(whole.Address) = value;
                    return true;
                }
                return whole.Property != nullptr && !whole.Owner.IsEmpty() &&
                       core::SetProperty(*whole.Property, whole.Owner, core::Variant::From<T>(value)).IsOk();
            };
            switch (shape)
            {
            case Shape::Float2:
                return store(core::Float2{parts.x, parts.y});
            case Shape::Float3:
                return store(core::Float3{parts.x, parts.y, parts.z});
            case Shape::Float4:
                return store(parts);
            case Shape::Quaternion:
                return store(core::FromYawPitchRoll(core::DegreesToRadians(parts.y), core::DegreesToRadians(parts.x), core::DegreesToRadians(parts.z)));
            default:
                return false;
            }
        }

        // === Text: each object's value at a path, written and read with the text format ===

        [[nodiscard]] core::String WriteState(core::StringView path, usize object) const
        {
            if (m_format == nullptr)
                return core::String{};
            Target target;
            if (!Resolve(object, path, target))
                return core::String{};
            const ReflectedPropertySource* self = this;
            return m_format->Write([self, target](core::ISerializer& ar) mutable { self->SerializeTarget(ar, "value", target); });
        }
        bool ReadState(core::StringView path, usize object, core::StringView text)
        {
            Target target;
            if (m_format == nullptr || !Resolve(object, path, target))
                return false;
            const ReflectedPropertySource* self = this;
            return m_format->Read(text, [self, target](core::ISerializer& ar) mutable { self->SerializeTarget(ar, "value", target); });
        }
        // Writes or reads a place under `key`: a value as its type, a Float or a Quaternion as Core
        // serializes it, and a group as an object of its properties by name.
        void SerializeTarget(core::ISerializer& ar, const char* key, const Target& target) const
        {
            if (target.Type == nullptr)
                return;
            const bool reading = ar.Mode() == core::SerializeMode::Read;
            const Shape shape = ShapeOf(*target.Type);
            if (target.Part == kHdrColorPart || target.AsColor)
            {
                core::Color value = Read(target).AsColor();
                core::Serialize(ar, key, value);
                if (reading)
                    (void)WriteTarget(target, Variant(value));
                return;
            }
            if (target.Part >= 0)
            {
                f64 value = PartOf(target, shape);
                core::Serialize(ar, key, value);
                if (reading)
                    (void)SetPart(target, shape, value);
                return;
            }
            switch (shape)
            {
            case Shape::Bool:
            {
                bool value = Read(target).AsBool();
                core::Serialize(ar, key, value);
                if (reading)
                    (void)WriteTarget(target, Variant(value));
                return;
            }
            case Shape::Integer:
            case Shape::Enum:
            {
                i64 value = Read(target).AsInt();
                core::Serialize(ar, key, value);
                if (reading)
                    (void)WriteTarget(target, Variant(value));
                return;
            }
            case Shape::Float:
            {
                f64 value = Read(target).AsFloat();
                core::Serialize(ar, key, value);
                if (reading)
                    (void)WriteTarget(target, Variant(value));
                return;
            }
            case Shape::String:
            case Shape::Reference: // its Guid's text
            {
                core::String value(Read(target).AsString());
                core::Serialize(ar, key, value);
                if (reading)
                    (void)WriteTarget(target, Variant(value.AsView()));
                return;
            }
            case Shape::Color:
            {
                core::Color value = Read(target).AsColor();
                core::Serialize(ar, key, value);
                if (reading)
                    (void)WriteTarget(target, Variant(value));
                return;
            }
            case Shape::Float2:
            case Shape::Float3:
            case Shape::Float4:
            case Shape::Quaternion:
            {
                core::Float4 parts{};
                (void)ReadCompound(target, shape, parts);
                if (shape == Shape::Float2)
                {
                    core::Float2 value{parts.x, parts.y};
                    core::Serialize(ar, key, value);
                    parts = core::Float4{value.x, value.y, 0.0f, 0.0f};
                }
                else if (shape == Shape::Float4)
                    core::Serialize(ar, key, parts);
                else
                {
                    core::Float3 value{parts.x, parts.y, parts.z}; // a Quaternion as its Euler degrees, as the grid shows it
                    core::Serialize(ar, key, value);
                    parts = core::Float4{value.x, value.y, value.z, 0.0f};
                }
                if (reading)
                    (void)WriteCompound(target, shape, parts);
                return;
            }
            case Shape::List:
            {
                if (target.Address == nullptr)
                    return;
                SerializeList(ar, key, target);
                return;
            }
            case Shape::Curve: // written as its struct, since Core reflects its keys and stops
            case Shape::Gradient:
            case Shape::Group:
            {
                if (target.Address == nullptr)
                    return;
                ar.Key(key);
                ar.BeginObject();
                const core::Instance owner(target.Address, target.Type);
                for (const core::TypeInfo* t = target.Type; t != nullptr; t = t->base)
                    for (u32 i = 0; i < t->propertyCount; ++i)
                    {
                        const core::PropertyInfo& property = t->properties[i];
                        Target child;
                        child.Owner = owner;
                        child.Property = &property;
                        child.Type = property.type;
                        child.Address = property.address != nullptr ? property.address(owner) : nullptr;
                        child.ReadOnly = target.ReadOnly || (static_cast<u32>(property.flags) & static_cast<u32>(core::PropertyFlags::ReadOnly)) != 0;
                        if (child.ReadOnly && reading)
                            continue;
                        if (reading && !ar.HasKey(property.name))
                            continue;
                        SerializeTarget(ar, property.name, child);
                    }
                ar.EndObject();
                return;
            }
            default:
                return;
            }
        }

        // Writes or reads a list: its count, each polymorphic element's type, then each element.
        // When reading, the list is resized to the count (elements made or removed at its end)
        // before its elements are read.
        void SerializeList(core::ISerializer& ar, const char* key, const Target& target) const
        {
            const bool reading = ar.Mode() == core::SerializeMode::Read;
            const core::ContainerInfo& container = *target.Type->container;
            const core::Instance list(target.Address, target.Type);
            const bool polymorphic = core::IsPolymorphicContainer(container);
            ar.Key(key);
            ar.BeginObject();
            i64 count = static_cast<i64>(core::ContainerSize(container, list));
            core::Serialize(ar, "count", count);
            if (reading && !target.ReadOnly)
            {
                // A polymorphic list is rebuilt from the types written; a homogeneous one is grown or
                // cut to the count.
                if (polymorphic)
                {
                    while (core::ContainerSize(container, list) != 0)
                        (void)core::ContainerRemoveAt(container, list, core::ContainerSize(container, list) - 1);
                    for (i64 i = 0; i < count; ++i)
                    {
                        core::String type;
                        const core::FixedArray<char, 24> typeKey = ElementKey(i, true);
                        core::Serialize(ar, typeKey.items, type);
                        for (const core::TypeInfo* concrete : CreatableTypes(container))
                            if (core::StringView(reinterpret_cast<const char8_t*>(concrete->name)) == type.AsView())
                            {
                                (void)core::ContainerCreateElement(container, list, static_cast<usize>(i), *concrete);
                                break;
                            }
                    }
                }
                else
                {
                    while (static_cast<i64>(core::ContainerSize(container, list)) > count && count >= 0)
                        (void)core::ContainerRemoveAt(container, list, core::ContainerSize(container, list) - 1);
                    while (static_cast<i64>(core::ContainerSize(container, list)) < count)
                        if (core::ContainerEmplaceDefault(container, list, core::ContainerSize(container, list)).IsEmpty())
                            break; // a bounded array is full
                }
            }
            else if (polymorphic)
                for (i64 i = 0; i < count; ++i)
                {
                    const core::Instance element = core::ContainerAddressAt(container, list, static_cast<usize>(i));
                    core::String type(element.IsEmpty() ? core::StringView{} : core::StringView(reinterpret_cast<const char8_t*>(element.Type()->name)));
                    const core::FixedArray<char, 24> typeKey = ElementKey(i, true);
                    core::Serialize(ar, typeKey.items, type);
                }
            const usize size = core::ContainerSize(container, list);
            for (usize i = 0; i < size; ++i)
            {
                Target element;
                element.List = list;
                element.Index = i;
                element.Element = true;
                element.ReadOnly = target.ReadOnly;
                element.Type = container.elementType;
                const core::Instance at = core::ContainerAddressAt(container, list, i);
                if (!at.IsEmpty())
                {
                    element.Address = at.Pointer();
                    element.Type = at.Type();
                }
                const core::FixedArray<char, 24> elementKey = ElementKey(static_cast<i64>(i), false);
                if (reading && !ar.HasKey(elementKey.items))
                    continue;
                if (element.Address == nullptr && element.Type != nullptr && ShapeOf(*element.Type) == Shape::Group)
                {
                    // A struct element with no address (a BoundedArray's) is copied: the copy is
                    // written, or read and then set back.
                    core::Variant copy = core::ContainerGetAt(container, list, i);
                    if (copy.ValuePointer() == nullptr)
                        continue;
                    Target held = element;
                    held.Address = copy.ValuePointer();
                    held.Element = false;
                    SerializeTarget(ar, elementKey.items, held);
                    if (reading && !target.ReadOnly)
                        (void)core::ContainerSetAt(container, list, i, copy);
                    continue;
                }
                SerializeTarget(ar, elementKey.items, element);
            }
            ar.EndObject();
        }
        // "e3" for element 3, "t3" for its type: keys the serializer reads as C strings.
        [[nodiscard]] static core::FixedArray<char, 24> ElementKey(i64 index, bool type)
        {
            core::FixedArray<char, 24> key;
            key.Add(type ? 't' : 'e');
            const core::String digits = core::Format(u8"{}", index);
            for (usize i = 0; i < digits.Size() && key.Size() + 1 < key.CapacityValue(); ++i)
                key.Add(static_cast<char>(digits[i]));
            key.Add('\0'); // the serializer reads item keys as C strings
            return key;
        }

        // Restores each value at `path` and under it in object `object` to its default; a list is
        // restored whole, as the type's instance has it. Returns whether anything changed.
        bool RevertUnder(core::StringView path, usize object)
        {
            Target target;
            if (!Resolve(object, path, target) || target.Type == nullptr || target.ReadOnly)
                return false;
            const Shape shape = target.Part >= 0 ? Shape::Float : ShapeOf(*target.Type);
            if (shape == Shape::List)
            {
                if (!m_typeDefault || m_format == nullptr)
                    return false;
                const core::String before = WriteState(path, object);
                Target byDefault;
                if (!ResolveIn(core::Instance(m_typeDefault.Get(), m_objects[0].Type()), path, byDefault))
                    return false;
                const ReflectedPropertySource* self = this;
                const core::String text = m_format->Write([self, byDefault](core::ISerializer& ar) mutable { self->SerializeTarget(ar, "value", byDefault); });
                (void)ReadState(path, object, text.AsView());
                return WriteState(path, object) != before;
            }
            core::Array<PropertyInfo> children(m_allocator);
            if (shape == Shape::Group || shape == Shape::Float2 || shape == Shape::Float3 || shape == Shape::Float4 || shape == Shape::Quaternion)
            {
                GetProperties(path, children);
                bool changed = false;
                for (const PropertyInfo& child : children)
                    changed = RevertUnder(JoinPropertyPath(path, child.Name.AsView()).AsView(), object) || changed;
                return changed;
            }
            const Variant byDefault = GetDefault(path, object);
            const Variant now = Read(target);
            if (byDefault.IsEmpty() || now.Compare(byDefault) == 0)
                return false;
            return WriteTarget(target, byDefault);
        }

        // Returns a tag for what a copy holds, so a paste accepts only the same shape: a Float's
        // name (a Quaternion's Euler degrees count as a Float3, as the grid shows them), a struct's
        // type, a list's element type, or a value's kind.
        [[nodiscard]] core::String ShapeTag(const Target& target) const
        {
            if (target.Part == kHdrColorPart || target.AsColor)
                return core::String(u8"Color");
            if (target.Part >= 0)
                return core::String(u8"number");
            if (target.Hdr)
                return core::String(u8"ColorHdr");
            switch (ShapeOf(*target.Type))
            {
            case Shape::Bool:
                return core::String(u8"bool");
            case Shape::Integer:
            case Shape::Enum:
                return core::String(u8"integer");
            case Shape::Float:
                return core::String(u8"number");
            case Shape::String:
                return core::String(u8"text");
            case Shape::Float2:
                return core::String(u8"Float2");
            case Shape::Float3:
            case Shape::Quaternion:
                return core::String(u8"Float3");
            case Shape::Float4:
                return core::String(u8"Float4");
            case Shape::Color:
                return core::String(u8"Color");
            case Shape::Reference: // a reference pastes only into a reference of the same type
            {
                const core::TypeInfo* to = ReferenceTargetOf(*target.Type);
                return core::Format(u8"reference to {}", to != nullptr ? core::StringView(reinterpret_cast<const char8_t*>(to->name)) : core::StringView(u8"?"));
            }
            case Shape::Curve:
            case Shape::Gradient:
            case Shape::Group:
                return core::String(reinterpret_cast<const char8_t*>(target.Type->name));
            case Shape::List:
            {
                const core::TypeInfo* element = target.Type->container->elementType;
                return core::Format(u8"list of {}", element != nullptr ? core::StringView(reinterpret_cast<const char8_t*>(element->name)) : core::StringView(u8"?"));
            }
            default:
                return core::String{};
            }
        }

        // Changes one object's list as `edit` asks.
        bool ChangeList(const Target& target, const ListEdit& edit) const
        {
            const core::ContainerInfo& container = *target.Type->container;
            const core::Instance list(target.Address, target.Type);
            const usize size = core::ContainerSize(container, list);
            switch (edit.Kind)
            {
            case ListEdit::Op::Insert:
            {
                const usize at = core::Min(edit.Index, size);
                if (core::IsPolymorphicContainer(container))
                {
                    for (const core::TypeInfo* concrete : CreatableTypes(container))
                        if (TypeShownName(*concrete) == edit.ElementType.AsView() || (edit.ElementType.Size() == 0))
                            return !core::ContainerCreateElement(container, list, at, *concrete).IsEmpty();
                    return false;
                }
                return !core::ContainerEmplaceDefault(container, list, at).IsEmpty();
            }
            case ListEdit::Op::Remove:
                return edit.Index < size && core::ContainerRemoveAt(container, list, edit.Index).IsOk();
            case ListEdit::Op::Move:
                return edit.Index < size && edit.To < size && edit.Index != edit.To && core::ContainerMoveElement(container, list, edit.Index, edit.To).IsOk();
            }
            return false;
        }
        [[nodiscard]] static core::String ListWords(core::StringView path, const ListEdit& edit)
        {
            switch (edit.Kind)
            {
            case ListEdit::Op::Insert:
                return core::Format(u8"Added to {}", path);
            case ListEdit::Op::Remove:
                return core::Format(u8"Removed from {}", path);
            default:
                return core::Format(u8"Moved in {}", path);
            }
        }

        // === Curves and gradients: the parts the application named, read and written whole ===

        // Returns the capacity of the list property `name` of `type` (0 for no limit, or no such
        // list).
        [[nodiscard]] static u32 PartCapacity(const core::TypeInfo& type, core::StringView name)
        {
            const core::PropertyInfo* property = FindPropertyNamed(type, name);
            return property != nullptr && property->type != nullptr && property->type->container != nullptr
                       ? static_cast<u32>(core::ContainerCapacity(*property->type->container))
                       : 0u;
        }
        // Returns the list property `name` of the struct at `target` as a list instance, or empty
        // when there is none.
        [[nodiscard]] core::Instance PartList(const Target& target, core::StringView name) const
        {
            if (target.Address == nullptr || target.Type == nullptr)
                return core::Instance{};
            const core::PropertyInfo* property = FindPropertyNamed(*target.Type, name);
            if (property == nullptr || property->type == nullptr || property->type->container == nullptr || property->address == nullptr)
                return core::Instance{};
            void* address = property->address(core::Instance(target.Address, target.Type));
            return address != nullptr ? core::Instance(address, property->type) : core::Instance{};
        }
        // Returns the list's elements as `T` (Core's key or stop), by value.
        template <typename T>
        static void ReadElements(const core::Instance& list, core::Array<T>& out)
        {
            if (list.IsEmpty())
                return;
            const core::ContainerInfo& container = *list.Type()->container;
            for (usize i = 0; i < core::ContainerSize(container, list); ++i)
            {
                const core::Variant element = core::ContainerGetAt(container, list, i);
                if (const T* typed = element.TryGet<T>())
                    out.PushBack(*typed);
            }
        }
        // Sets the list to `values`: grows or cuts it to their count (a bounded array up to its
        // capacity), then sets each element. Returns false when it could not hold them all.
        template <typename T>
        static bool WriteElements(const core::Instance& list, core::Span<const T> values)
        {
            if (list.IsEmpty())
                return false;
            const core::ContainerInfo& container = *list.Type()->container;
            while (core::ContainerSize(container, list) > values.Size())
                if (!core::ContainerRemoveAt(container, list, core::ContainerSize(container, list) - 1).IsOk())
                    return false;
            while (core::ContainerSize(container, list) < values.Size())
                if (core::ContainerEmplaceDefault(container, list, core::ContainerSize(container, list)).IsEmpty())
                    break; // the list is full
            bool all = core::ContainerSize(container, list) == values.Size();
            for (usize i = 0; i < core::ContainerSize(container, list); ++i)
                all = core::ContainerSetAt(container, list, i, core::Variant::From<T>(values[i])).IsOk() && all;
            return all;
        }
        [[nodiscard]] Variant ReadCurve(const Target& target) const
        {
            const ReflectedCurveParts* parts = FindCurveType(core::StringView(reinterpret_cast<const char8_t*>(target.Type->name)));
            const core::Instance list = parts != nullptr ? PartList(target, parts->Keys.AsView()) : core::Instance{};
            if (list.IsEmpty())
                return Variant{};
            core::Array<core::CurveKey> keys(m_allocator);
            ReadElements(list, keys);
            return Variant::FromCurve(keys.AsSpan());
        }
        bool WriteCurve(const Target& target, core::Span<const core::CurveKey> keys) const
        {
            const ReflectedCurveParts* parts = FindCurveType(core::StringView(reinterpret_cast<const char8_t*>(target.Type->name)));
            return parts != nullptr && WriteElements(PartList(target, parts->Keys.AsView()), keys);
        }
        [[nodiscard]] Variant ReadGradient(const Target& target) const
        {
            const ReflectedGradientParts* parts = FindGradientType(core::StringView(reinterpret_cast<const char8_t*>(target.Type->name)));
            if (parts == nullptr || target.Address == nullptr)
                return Variant{};
            ColorGradient gradient;
            ReadElements(PartList(target, parts->Colors.AsView()), gradient.Colors);
            ReadElements(PartList(target, parts->Alphas.AsView()), gradient.Alphas);
            if (const core::PropertyInfo* blend = FindPropertyNamed(*target.Type, parts->Interpolation.AsView()); blend != nullptr && blend->address != nullptr)
                if (const void* address = blend->address(core::Instance(target.Address, target.Type)))
                    gradient.Interpolation = static_cast<core::GradientInterpolation>(core::ReadEnumValue(address, *blend->type));
            return Variant(gradient);
        }
        bool WriteGradient(const Target& target, const ColorGradient& gradient) const
        {
            const ReflectedGradientParts* parts = FindGradientType(core::StringView(reinterpret_cast<const char8_t*>(target.Type->name)));
            if (parts == nullptr || target.Address == nullptr)
                return false;
            bool ok = WriteElements(PartList(target, parts->Colors.AsView()), gradient.Colors.AsSpan());
            ok = WriteElements(PartList(target, parts->Alphas.AsView()), gradient.Alphas.AsSpan()) && ok;
            if (const core::PropertyInfo* blend = FindPropertyNamed(*target.Type, parts->Interpolation.AsView()); blend != nullptr && blend->address != nullptr)
                if (void* address = blend->address(core::Instance(target.Address, target.Type)))
                    core::WriteEnumValue(address, *blend->type, static_cast<i64>(gradient.Interpolation));
            return ok;
        }
        // A property's change may show or hide others (visibleWhen), so report the properties as
        // changed.
        void Changed(core::StringView) { NotifyPropertiesChanged(); }

        struct Original
        {
            core::String Path;
            core::Array<Variant> Values;     // each object's value, for a cancel
            core::Array<core::String> States; // each object's text, for the change
        };
        const Original* FindOriginal(core::StringView path) const
        {
            for (const Original& original : m_originals)
                if (original.Path.AsView() == path)
                    return &original;
            return nullptr;
        }
        void DropOriginal(core::StringView path)
        {
            for (usize i = 0; i < m_originals.Size(); ++i)
                if (m_originals[i].Path.AsView() == path)
                {
                    m_originals.RemoveAt(i);
                    return;
                }
        }

        core::IAllocator& m_allocator;
        core::Array<core::Instance> m_objects;
        core::Array<Original> m_originals;
        IPropertyTextFormat* m_format = nullptr; // not owned
        struct Provider
        {
            DefaultProvider Default;
            core::String RevertText;
        };
        core::Array<Provider> m_providers;
        core::RefPtr<core::Object> m_typeDefault; // the type's defaults, made by its creator
        struct CurveType
        {
            core::String Name;
            ReflectedCurveParts Parts;
        };
        struct GradientType
        {
            core::String Name;
            ReflectedGradientParts Parts;
        };
        struct ReferenceType
        {
            core::String Name;
            ReflectedReferenceParts Parts;
        };
        core::Array<CurveType> m_curveTypes;
        core::Array<ReferenceType> m_referenceTypes;
        core::Array<GradientType> m_gradientTypes;
        core::Array<core::String> m_hintAttributes; // RegisterHintAttribute
        core::Function<void(const ReflectedChange&)> m_onChange;
        core::Function<void(core::StringView, PropertyInfo&)> m_onDescribe; // SetOnDescribe
    };
}
