// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Generic - the `editor.generic` module (implementation).

module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"

module editor.generic;

import foundation.core;
import foundation.content;
import foundation.runtime.client;
import foundation.gui;
import foundation.gui.toolkit;
import editor.core;
import editor.app;

using namespace foundation::core;
namespace gui = foundation::gui;

namespace editor
{
    namespace
    {
        // Widen a raw scalar into the field's storage, or narrow it back out.
        void ReadScalarInto(AssetFormField& field, const void* value, ScalarKind kind)
        {
            field.scalarKind = kind;
            switch (kind)
            {
            case ScalarKind::Bool:
                field.intValue = *static_cast<const bool*>(value) ? 1 : 0;
                break;
            case ScalarKind::Int8:
                field.intValue = *static_cast<const i8*>(value);
                break;
            case ScalarKind::UInt8:
                field.intValue = *static_cast<const u8*>(value);
                break;
            case ScalarKind::Int16:
                field.intValue = *static_cast<const i16*>(value);
                break;
            case ScalarKind::UInt16:
                field.intValue = *static_cast<const u16*>(value);
                break;
            case ScalarKind::Int32:
                field.intValue = *static_cast<const i32*>(value);
                break;
            case ScalarKind::UInt32:
                field.intValue = *static_cast<const u32*>(value);
                break;
            case ScalarKind::Int64:
                field.intValue = *static_cast<const i64*>(value);
                break;
            case ScalarKind::UInt64:
                field.intValue = static_cast<i64>(*static_cast<const u64*>(value));
                break;
            case ScalarKind::Float32:
                field.floatValue = *static_cast<const f32*>(value);
                break;
            case ScalarKind::Float64:
                field.floatValue = *static_cast<const f64*>(value);
                break;
            }
        }

        void WriteScalarFrom(const AssetFormField& field, void* value, ScalarKind kind)
        {
            switch (kind)
            {
            case ScalarKind::Bool:
                *static_cast<bool*>(value) = field.intValue != 0;
                break;
            case ScalarKind::Int8:
                *static_cast<i8*>(value) = static_cast<i8>(field.intValue);
                break;
            case ScalarKind::UInt8:
                *static_cast<u8*>(value) = static_cast<u8>(field.intValue);
                break;
            case ScalarKind::Int16:
                *static_cast<i16*>(value) = static_cast<i16>(field.intValue);
                break;
            case ScalarKind::UInt16:
                *static_cast<u16*>(value) = static_cast<u16>(field.intValue);
                break;
            case ScalarKind::Int32:
                *static_cast<i32*>(value) = static_cast<i32>(field.intValue);
                break;
            case ScalarKind::UInt32:
                *static_cast<u32*>(value) = static_cast<u32>(field.intValue);
                break;
            case ScalarKind::Int64:
                *static_cast<i64*>(value) = field.intValue;
                break;
            case ScalarKind::UInt64:
                *static_cast<u64*>(value) = static_cast<u64>(field.intValue);
                break;
            case ScalarKind::Float32:
                *static_cast<f32*>(value) = static_cast<f32>(field.floatValue);
                break;
            case ScalarKind::Float64:
                *static_cast<f64*>(value) = field.floatValue;
                break;
            }
        }

        // Records every value Serialize produces as an ordinal field list.
        class FormScanSerializer final : public Serializer
        {
        public:
            explicit FormScanSerializer(Array<AssetFormField>& fields)
                : Serializer(SerializeMode::Write), m_fields(&fields)
            {
            }

            void Key(const char* name) noexcept override
            {
                m_lastKey = name != nullptr ? name : "";
            }
            void BeginArray(u32& count) override
            {
                AssetFormField field;
                field.label = CurrentLabel();
                field.kind = AssetFormFieldKind::ArrayCount;
                field.scalarKind = ScalarKind::UInt32;
                field.intValue = count;
                StampCategory(field);
                m_fields->PushBack(Move(field));
                m_arrayKeys.PushBack(String(CurrentLabel().AsView()));
                m_arrayIndices.PushBack(0);
            }
            void EndArray() override
            {
                if (!m_arrayKeys.IsEmpty())
                {
                    m_arrayKeys.PopBack();
                    m_arrayIndices.PopBack();
                }
            }
            void Scalar(void* value, ScalarKind kind) override
            {
                AssetFormField field;
                field.label = CurrentLabel();
                field.kind = AssetFormFieldKind::Scalar;
                ReadScalarInto(field, value, kind);
                StampCategory(field);
                m_fields->PushBack(Move(field));
                BumpArrayIndex();
            }
            void Text(String& value) override
            {
                AssetFormField field;
                field.label = CurrentLabel();
                field.kind = AssetFormFieldKind::Text;
                field.textValue = String(value.AsView());
                StampCategory(field);
                m_fields->PushBack(Move(field));
                BumpArrayIndex();
            }
            void GuidValue(Guid& value) override
            {
                AssetFormField field;
                field.label = CurrentLabel();
                field.kind = AssetFormFieldKind::Guid;
                field.guidValue = value;
                StampCategory(field);
                m_fields->PushBack(Move(field));
                BumpArrayIndex();
            }
            void Blob(void* data, usize size) override
            {
                AssetFormField field;
                field.label = CurrentLabel();
                field.kind = AssetFormFieldKind::Blob;
                field.blobValue.Resize(size);
                if (size > 0)
                {
                    MemCopy(field.blobValue.Data(), data, size);
                }
                StampCategory(field);
                m_fields->PushBack(Move(field));
                BumpArrayIndex();
            }

        private:
            // Inside an unkeyed array (elements serialize without Key calls), label elements
            // as "arrayKey[i]". A keyed field inside an array keeps its own key.
            [[nodiscard]] String CurrentLabel() const
            {
                if (!m_arrayKeys.IsEmpty() && m_lastKey.IsEmpty())
                {
                    return Format(u8"{}[{}]", m_arrayKeys.Back(),
                                  m_arrayIndices[m_arrayIndices.Size() - 1]);
                }
                if (!m_arrayKeys.IsEmpty() &&
                    m_arrayKeys.Back().AsView() ==
                        StringView(reinterpret_cast<const utf8char*>(m_lastKey.CStr())))
                {
                    return Format(u8"{}[{}]", m_arrayKeys.Back(),
                                  m_arrayIndices[m_arrayIndices.Size() - 1]);
                }
                return String(StringView(reinterpret_cast<const utf8char*>(m_lastKey.CStr())));
            }
            // The outermost enclosing array's key becomes the field's category (see
            // AssetFormField::category; array groups start collapsed).
            void StampCategory(AssetFormField& field) const
            {
                if (!m_arrayKeys.IsEmpty())
                {
                    field.category = String(m_arrayKeys[0].AsView());
                }
            }
            void BumpArrayIndex()
            {
                if (!m_arrayIndices.IsEmpty())
                {
                    m_arrayIndices[m_arrayIndices.Size() - 1]++;
                }
            }

            Array<AssetFormField>* m_fields;
            // m_lastKey uses a plain byte string (Key hands a const char*).
            struct KeyString
            {
                char data[96] = {};
                usize size = 0;
                void operator=(const char* s)
                {
                    size = 0;
                    while (s != nullptr && s[size] != '\0' && size < sizeof(data) - 1)
                    {
                        data[size] = s[size];
                        ++size;
                    }
                    data[size] = '\0';
                }
                [[nodiscard]] bool IsEmpty() const { return size == 0; }
                [[nodiscard]] const char* CStr() const { return data; }
            } m_lastKey;
            Array<String> m_arrayKeys;
            Array<u32> m_arrayIndices;
        };

        // Feeds recorded fields back through Serialize, with one field patched. A shape
        // mismatch makes the replay inert, and the object keeps its live values from there.
        class FormReplaySerializer final : public Serializer
        {
        public:
            FormReplaySerializer(const Array<AssetFormField>& fields, usize patchIndex,
                                 const AssetFormField& patch)
                : Serializer(SerializeMode::Read), m_fields(&fields), m_patchIndex(patchIndex),
                  m_patch(&patch)
            {
            }

            void BeginArray(u32& count) override
            {
                const AssetFormField* field = Next(AssetFormFieldKind::ArrayCount);
                if (field != nullptr)
                {
                    count = static_cast<u32>(field->intValue);
                }
            }
            void Scalar(void* value, ScalarKind kind) override
            {
                const AssetFormField* field = Next(AssetFormFieldKind::Scalar);
                if (field != nullptr && field->scalarKind == kind)
                {
                    WriteScalarFrom(*field, value, kind);
                }
                else if (field != nullptr)
                {
                    m_desynced = true;
                }
            }
            void Text(String& value) override
            {
                const AssetFormField* field = Next(AssetFormFieldKind::Text);
                if (field != nullptr)
                {
                    value = String(field->textValue.AsView());
                }
            }
            void GuidValue(Guid& value) override
            {
                const AssetFormField* field = Next(AssetFormFieldKind::Guid);
                if (field != nullptr)
                {
                    value = field->guidValue;
                }
            }
            void Blob(void* data, usize size) override
            {
                const AssetFormField* field = Next(AssetFormFieldKind::Blob);
                if (field != nullptr && field->blobValue.Size() == size && size > 0)
                {
                    MemCopy(data, field->blobValue.Data(), size);
                }
                else if (field != nullptr && field->blobValue.Size() != size)
                {
                    m_desynced = true;
                }
            }

        private:
            [[nodiscard]] const AssetFormField* Next(AssetFormFieldKind expected)
            {
                if (m_desynced || m_cursor >= m_fields->Size())
                {
                    m_desynced = true;
                    return nullptr;
                }
                const usize index = m_cursor++;
                const AssetFormField& stored = (*m_fields)[index];
                if (stored.kind != expected)
                {
                    m_desynced = true;
                    return nullptr;
                }
                return (index == m_patchIndex) ? m_patch : &stored;
            }

            const Array<AssetFormField>* m_fields;
            usize m_patchIndex;
            const AssetFormField* m_patch;
            usize m_cursor = 0;
            bool m_desynced = false;
        };
    } // namespace

    Status ScanAssetForm(ISerializable& object, Array<AssetFormField>& outFields)
    {
        outFields.Clear();
        FormScanSerializer scanner(outFields);
        app::PushCurrentDataVersions(scanner, *object.GetType());
        object.Serialize(scanner);
        scanner.PopVersionScope();
        return scanner.GetStatus();
    }

    Status ApplyAssetFormField(ISerializable& object, const Array<AssetFormField>& fields,
                               usize index, const AssetFormField& newValue)
    {
        if (index >= fields.Size())
        {
            return Status{ErrorCode::InvalidArgument};
        }
        FormReplaySerializer replayer(fields, index, newValue);
        app::PushCurrentDataVersions(replayer, *object.GetType());
        object.Serialize(replayer);
        replayer.PopVersionScope();
        return replayer.GetStatus();
    }

    bool AssetFormShapeEquals(const Array<AssetFormField>& a, const Array<AssetFormField>& b)
    {
        if (a.Size() != b.Size())
        {
            return false;
        }
        for (usize i = 0; i < a.Size(); ++i)
        {
            if (a[i].kind != b[i].kind || a[i].scalarKind != b[i].scalarKind ||
                a[i].label.AsView() != b[i].label.AsView())
            {
                return false;
            }
        }
        return true;
    }

    // ============================ Page ======================================================

    namespace
    {
        // An integer field's range, by its width (a 64-bit one as far as a double counts exactly).
        void IntegerRange(ScalarKind kind, gui::PropertyInfo& info)
        {
            info.HasRange = true;
            switch (kind)
            {
            case ScalarKind::Int8: info.Min = -128.0; info.Max = 127.0; break;
            case ScalarKind::UInt8: info.Min = 0.0; info.Max = 255.0; break;
            case ScalarKind::Int16: info.Min = -32768.0; info.Max = 32767.0; break;
            case ScalarKind::UInt16: info.Min = 0.0; info.Max = 65535.0; break;
            case ScalarKind::Int32: info.Min = -2147483648.0; info.Max = 2147483647.0; break;
            case ScalarKind::UInt32: info.Min = 0.0; info.Max = 4294967295.0; break;
            case ScalarKind::UInt64: info.Min = 0.0; info.Max = 9007199254740992.0; break;
            default: info.Min = -9007199254740992.0; info.Max = 9007199254740992.0; break;
            }
        }

        String GuidText(const Guid& id)
        {
            if (id.IsNil())
                return String{};
            utf8char buffer[37];
            id.ToChars(buffer);
            return String(StringView{buffer, 36});
        }

        // What a field's row shows.
        gui::Variant ValueOf(const AssetFormField& field)
        {
            switch (field.kind)
            {
            case AssetFormFieldKind::Text: return gui::Variant(field.textValue.AsView());
            case AssetFormFieldKind::Guid: return gui::Variant(GuidText(field.guidValue));
            case AssetFormFieldKind::Blob: return gui::Variant(Format(u8"{} bytes", field.blobValue.Size()));
            default: break;
            }
            if (field.scalarKind == ScalarKind::Bool)
                return gui::Variant(field.intValue != 0);
            if (field.scalarKind == ScalarKind::Float32 || field.scalarKind == ScalarKind::Float64)
                return gui::Variant(field.floatValue);
            return gui::Variant(field.intValue);
        }

        gui::PropertyKind KindOf(const AssetFormField& field)
        {
            switch (field.kind)
            {
            case AssetFormFieldKind::Text:
            case AssetFormFieldKind::Blob: return gui::PropertyKind::String;
            case AssetFormFieldKind::Guid: return gui::PropertyKind::Reference;
            default: break;
            }
            if (field.scalarKind == ScalarKind::Bool)
                return gui::PropertyKind::Bool;
            if (field.scalarKind == ScalarKind::Float32 || field.scalarKind == ScalarKind::Float64)
                return gui::PropertyKind::Float;
            return gui::PropertyKind::Integer;
        }
    }

    GenericAssetEditorPage::GenericAssetEditorPage(EditorContext& context, app::AssetPageServices services,
                                                   foundation::content::Instance& instance)
        : app::UIEditorPage(context.Allocator()),
          m_context(&context), m_services(Move(services)), m_title(instance.Name()), m_fields(Allocator()), m_paths(Allocator())
    {
        SetInstanceId(instance.Id());

        m_object = instance.ReadObject();
        if (m_object.Get() == nullptr)
        {
            LOG_ERROR(u8"Editor", u8"asset '{}' failed to read - page opens empty",
                               m_title);
        }
        else
        {
            (void)ScanAssetForm(*m_object, m_fields);
        }
        m_undoBaseline = Snapshot();

        m_content = m_services.loadMarkup ? m_services.loadMarkup(kPath) : RefPtr<gui::Node>{};
        if (m_content)
        {
            m_bar = MakeUnique<app::PageActionBar>(Allocator(), Allocator(), context.Actions(), *this);
            m_bar->Bind(Find<gui::ButtonBase>(u8"form-save"), u8"file.save");
            m_bar->Bind(Find<gui::ButtonBase>(u8"form-undo"), u8"edit.undo");
            m_bar->Bind(Find<gui::ButtonBase>(u8"form-redo"), u8"edit.redo");
            m_bar->Bind(Find<gui::ButtonBase>(u8"form-discard"), u8"page.discardChanges");
            m_bar->Bind(Find<gui::ButtonBase>(u8"form-cook"), u8"build.cookPage");
            m_bar->Bind(Find<gui::ButtonBase>(u8"form-rebuild"), u8"build.rebuildPage");
            if (auto* kind = Find<gui::Label>(u8"form-kind"); kind != nullptr && m_object.Get() != nullptr)
                kind->SetText(TypeAttrString(*m_object->GetType(), "displayName", StringView(reinterpret_cast<const utf8char*>(m_object->GetType()->name)))); // its display name, as every page shows it
            if (gui::Node* state = Find<gui::Node>(u8"form-state"))
                state->SetDisplayed(m_object.Get() == nullptr);
            m_grid = Find<gui::PropertyGrid>(u8"form-grid");
            if (m_grid != nullptr)
            {
                m_grid->SetDisplayed(m_object.Get() != nullptr);
                if (m_services.refs != nullptr)
                    app::ResourceRefEditor::Register(m_grid->GetEditors(), *m_services.refs);
            }
            if (auto* search = Find<gui::TextField>(u8"form-search"))
            {
                GenericAssetEditorPage* self = this;
                search->SetOnTextChanging([self](StringView text)
                                         {
                                             if (self->m_grid != nullptr)
                                                 self->m_grid->SetFilter(text);
                                         });
            }
        }
        BuildBag();
    }

    GenericAssetEditorPage::~GenericAssetEditorPage()
    {
        if (m_grid != nullptr)
            m_grid->SetSource(nullptr);
    }

    void GenericAssetEditorPage::OnUpdate(foundation::runtime::IApplicationHost&, f32)
    {
        Refresh();
    }

    void GenericAssetEditorPage::Refresh()
    {
        if (m_bar)
            m_bar->Sync();
        if (m_rebuild)
            BuildBag();
        else if (m_reshow)
            ShowValues();
        m_rebuild = false;
        m_reshow = false;
    }

    StringView GenericAssetEditorPage::PathOf(usize index) const
    {
        return index < m_paths.Size() ? m_paths[index].AsView() : StringView{};
    }

    void GenericAssetEditorPage::BuildBag()
    {
        UniquePtr<gui::PropertyBag> bag = MakeUnique<gui::PropertyBag>(Allocator(), Allocator());
        m_paths.Clear();
        // An array's fields under one group, by the outermost array's key, in the order the
        // arrays come; a group's path is its ordinal, so what is open survives a rebuild of the
        // same shape.
        Array<String> groups(Allocator());
        for (usize i = 0; i < m_fields.Size(); ++i)
        {
            const AssetFormField& field = m_fields[i];
            if (field.kind == AssetFormFieldKind::ArrayCount)
            {
                m_paths.PushBack(String{}); // structural, not authorable
                continue;
            }
            String parent;
            if (!field.category.IsEmpty())
            {
                usize group = groups.Size();
                for (usize g = 0; g < groups.Size(); ++g)
                    if (groups[g] == field.category)
                        group = g;
                parent = Format(u8"g{}", group);
                if (group == groups.Size())
                {
                    groups.PushBack(field.category);
                    gui::PropertyInfo& info = bag->Add(parent.AsView(), gui::PropertyKind::Group);
                    info.DisplayName = gui::ReadablePropertyName(field.category.AsView());
                    info.Description = field.category;
                }
            }
            const String path = gui::JoinPropertyPath(parent.AsView(), Format(u8"f{}", i).AsView());
            gui::PropertyInfo& info = bag->Add(path.AsView(), KindOf(field), ValueOf(field));
            info.DisplayName = gui::ReadablePropertyName(field.label.AsView());
            info.Description = field.label; // the key as it is saved
            info.ReadOnly = !field.Editable();
            if (info.Kind == gui::PropertyKind::Integer)
                IntegerRange(field.scalarKind, info);
            m_paths.PushBack(path);
        }
        GenericAssetEditorPage* self = this;
        bag->SetOnCommit([self](const gui::PropertyBag::Commit& commit) { self->Committed(commit); });
        if (m_grid != nullptr)
            m_grid->SetSource(bag.Get());
        m_bag = Move(bag); // after the grid let go of the old one
    }

    void GenericAssetEditorPage::ShowValues()
    {
        if (!m_bag)
            return;
        for (usize i = 0; i < m_fields.Size() && i < m_paths.Size(); ++i)
        {
            if (m_paths[i].IsEmpty())
                continue;
            const gui::Variant value = ValueOf(m_fields[i]);
            const gui::Variant shown = m_bag->GetValue(m_paths[i].AsView(), 0);
            if (shown.GetType() != value.GetType() || shown.Compare(value) != 0)
                m_bag->SetValue(m_paths[i].AsView(), 0, value);
        }
    }

    void GenericAssetEditorPage::Edit(StringView path, const gui::Variant& value)
    {
        gui::PropertyBag::Commit commit;
        commit.Path = String(path);
        commit.Before.PushBack(m_bag ? m_bag->GetValue(path, 0) : gui::Variant{});
        commit.After.PushBack(value);
        if (m_bag)
            m_bag->SetValue(path, 0, value); // as the grid's commit leaves it
        Committed(commit);
    }

    void GenericAssetEditorPage::Committed(const gui::PropertyBag::Commit& commit)
    {
        usize index = m_paths.Size();
        for (usize i = 0; i < m_paths.Size(); ++i)
            if (m_paths[i] == commit.Path)
                index = i;
        if (index >= m_fields.Size() || commit.After.IsEmpty())
            return;
        const gui::Variant& value = commit.After[0];
        AssetFormField patch = m_fields[index];
        switch (patch.kind)
        {
        case AssetFormFieldKind::Text:
            patch.textValue = String(value.AsString());
            break;
        case AssetFormFieldKind::Guid:
        {
            Guid parsed{};
            if (!value.AsString().IsEmpty() && !Guid::TryParse(value.AsString(), parsed))
            {
                m_reshow = true; // not a guid: the row shows the old one again
                return;
            }
            patch.guidValue = parsed;
            break;
        }
        case AssetFormFieldKind::Scalar:
            if (patch.scalarKind == ScalarKind::Bool)
                patch.intValue = value.AsBool() ? 1 : 0;
            else if (patch.scalarKind == ScalarKind::Float32 || patch.scalarKind == ScalarKind::Float64)
                patch.floatValue = value.AsFloat();
            else
                patch.intValue = value.AsInt();
            break;
        default:
            return; // blobs and counts are not edited
        }
        ApplyFieldEdit(index, patch);
    }

    void GenericAssetEditorPage::ApplyFieldEdit(usize index, const AssetFormField& newValue)
    {
        if (m_object.Get() == nullptr)
        {
            return;
        }
        (void)ApplyAssetFormField(*m_object, m_fields, index, newValue);

        // Undo step (coalesced per field label and ordinal).
        String key(newValue.label.AsView());
        key.Append(Format(u8"#{}", index).AsView());
        Array<byte> after = Snapshot();
        (void)Commands().Execute(UniquePtr<IEditorCommand>(
            Allocator().New<EditGenericCommand>(*this, key.AsView(), m_undoBaseline, after),
            Allocator()));
        m_undoBaseline = Move(after);
        MarkDirty();

        // Re-scan: a conditional-branch shape change rebuilds the rows; a same-shape patch shows
        // the values again (the replay may have narrowed one, a u8 past 255).
        Array<AssetFormField> rescanned;
        (void)ScanAssetForm(*m_object, rescanned);
        const bool sameShape = AssetFormShapeEquals(m_fields, rescanned);
        m_fields = Move(rescanned);
        if (sameShape)
            m_reshow = true;
        else
            m_rebuild = true;
    }

    Array<byte> GenericAssetEditorPage::Snapshot() const
    {
        Array<byte> blob;
        if (m_object.Get() == nullptr)
        {
            return blob;
        }
        MemoryStream stream;
        BinarySerializer ar(stream, SerializeMode::Write);
        app::PushCurrentDataVersions(ar, *m_object->GetType()); // include version-gated fields
        m_object->Serialize(ar);
        ar.PopVersionScope();
        const Span<const byte> bytes = stream.Bytes();
        blob.Reserve(bytes.Size());
        for (byte b : bytes)
        {
            blob.PushBack(b);
        }
        return blob;
    }

    void GenericAssetEditorPage::ApplyBlob(const Array<byte>& blob)
    {
        if (m_object.Get() == nullptr)
        {
            return;
        }
        Array<byte> current = Snapshot();
        if (current.Size() == blob.Size())
        {
            bool same = true;
            for (usize i = 0; i < blob.Size(); ++i)
            {
                if (current[i] != blob[i])
                {
                    same = false;
                    break;
                }
            }
            if (same)
            {
                return;
            }
        }
        MemoryStream stream;
        (void)stream.Write(blob.Data(), blob.Size());
        (void)stream.Seek(0, SeekOrigin::Begin);
        BinarySerializer ar(stream, SerializeMode::Read);
        app::PushCurrentDataVersions(ar, *m_object->GetType());
        m_object->Serialize(ar);
        ar.PopVersionScope();
        m_undoBaseline = blob;
        Array<AssetFormField> rescanned;
        (void)ScanAssetForm(*m_object, rescanned);
        if (AssetFormShapeEquals(m_fields, rescanned))
            m_reshow = true;
        else
            m_rebuild = true;
        m_fields = Move(rescanned);
        MarkDirty();
    }

    Status GenericAssetEditorPage::Save()
    {
        if (m_object.Get() == nullptr || m_context->Project() == nullptr)
        {
            return Status{ErrorCode::NotFound};
        }
        foundation::content::Instance* instance =
            m_context->Project()->SourceDb().GetInstance(InstanceId());
        if (instance == nullptr)
        {
            return Status{ErrorCode::NotFound};
        }
        const Status saved = instance->WriteObject(*m_object);
        if (saved.IsOk())
        {
            ClearDirty();
            m_context->RequestCook(false);
            LOG_INFO(u8"Editor", u8"saved asset '{}'", m_title);
        }
        return saved;
    }

    const TypeInfo* GenericAssetPageFactory::PrimaryType() const
    {
        // ISerializable: the root every asset (and document) derives from. Nearest-base
        // dispatch means every bespoke page (a concrete type, distance 0) wins over this.
        return &ISerializable::StaticType();
    }

    UniquePtr<EditorPage> GenericAssetPageFactory::CreatePage(EditorContext& context,
                                                              foundation::content::Instance& instance)
    {
        auto* page = editor::EditorRootAllocator().New<GenericAssetEditorPage>(
            context, m_services->Make(), instance);
        return UniquePtr<EditorPage>(page, editor::EditorRootAllocator());
    }
}
