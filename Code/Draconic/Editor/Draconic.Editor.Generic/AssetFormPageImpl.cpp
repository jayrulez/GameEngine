// Draconic::EditorGeneric - the `draconic.editor.generic` module (implementation).

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Log/Log.h"

module draconic.editor.generic;

import draconic.foundation;
import draconic.content;
import draconic.ui;
import draconic.ui.toolkit;
import draconic.editor.core;
import draconic.editor.app;

using namespace draconic::foundation;

namespace draconic::editor
{
    namespace
    {
        // Push the type's CURRENT data-version chain (concrete + versioned bases) WITHOUT
        // serializing header noise - scans/replays run against the live object, which is at
        // the current version by definition. Mirrors BeginVersionedPayload's chain shape.
        u32 PushCurrentVersions(ISerializer& ar, const TypeInfo& type)
        {
            SerializedDataVersion chain[16];
            u32 count = 0;
            chain[count++] = SerializedDataVersion{type.id, type.dataVersion};
            for (const TypeInfo* base = type.base; base != nullptr && count < 16; base = base->base)
            {
                if (base->dataVersion > 0)
                {
                    chain[count++] = SerializedDataVersion{base->id, base->dataVersion};
                }
            }
            ar.PushVersionScope(chain, count);
            return count;
        }

        // Widen a raw scalar into the field's storage / narrow it back out.
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
                m_fields->PushBack(Move(field));
                BumpArrayIndex();
            }
            void Text(String& value) override
            {
                AssetFormField field;
                field.label = CurrentLabel();
                field.kind = AssetFormFieldKind::Text;
                field.textValue = String(value.AsView());
                m_fields->PushBack(Move(field));
                BumpArrayIndex();
            }
            void GuidValue(Guid& value) override
            {
                AssetFormField field;
                field.label = CurrentLabel();
                field.kind = AssetFormFieldKind::Guid;
                field.guidValue = value;
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

        // Feeds recorded fields back through Serialize, with ONE field patched. A shape
        // mismatch flips the replay inert - the object keeps its live values from there.
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
        PushCurrentVersions(scanner, *object.GetType());
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
        PushCurrentVersions(replayer, *object.GetType());
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

    GenericAssetEditorPage::GenericAssetEditorPage(EditorContext& context,
                                                   draconic::content::Instance& instance)
        : m_context(&context), m_title(instance.Name())
    {
        SetInstanceId(instance.Id());

        m_object = instance.ReadObject();
        if (m_object.Get() == nullptr)
        {
            DRACONIC_LOG_ERROR(u8"Editor", u8"asset '{}' failed to read - page opens empty",
                               m_title);
        }
        else
        {
            (void)ScanAssetForm(*m_object, m_fields);
        }
        m_undoBaseline = Snapshot();

        m_info = MakeRef<ui::Label>(DefaultAllocator(), StringView(u8""));
        m_info->FontSize.SetValue(12.0f);
        if (m_object.Get() != nullptr)
        {
            String text(StringView(reinterpret_cast<const utf8char*>(m_object->GetType()->name)));
            text.Append(u8"  (generic editor)");
            m_info->SetText(text.AsView());
        }

        m_grid = MakeRef<ui::toolkit::PropertyGrid>(DefaultAllocator());
        BuildGrid();

        auto column = MakeRef<ui::FlexLayout>(DefaultAllocator());
        column->Direction = ui::Orientation::Vertical;
        column->Spacing = 6.0f;
        column->Padding = ui::Thickness{8, 6};
        {
            auto lp = MakeRef<ui::FlexLayoutParams>(DefaultAllocator());
            lp->Width = ui::SizeSpec::Match();
            column->AddView(m_info.Get(), lp);
        }
        {
            auto lp = MakeRef<ui::FlexLayoutParams>(DefaultAllocator());
            lp->Grow = 1.0f;
            lp->Width = ui::SizeSpec::Match();
            column->AddView(m_grid.Get(), lp);
        }
        m_content = column;
    }

    void GenericAssetEditorPage::BuildGrid()
    {
        m_grid->Clear();
        GenericAssetEditorPage* self = this;
        for (usize i = 0; i < m_fields.Size(); ++i)
        {
            const AssetFormField& field = m_fields[i];
            const usize index = i;
            const StringView cat = u8"Properties";
            if (field.kind == AssetFormFieldKind::ArrayCount)
            {
                continue; // structural, not authorable
            }
            if (field.kind == AssetFormFieldKind::Blob)
            {
                String label(field.label.AsView());
                m_grid->AddProperty(RefPtr<ui::toolkit::PropertyEditor>(
                    MakeRef<ui::toolkit::StringEditor>(
                        DefaultAllocator(), label.AsView(),
                        Format(u8"(blob, {} bytes)", field.blobValue.Size()).AsView(),
                        Function<void(StringView)>{}, cat)
                        .Get()));
                continue;
            }
            if (field.kind == AssetFormFieldKind::Text)
            {
                m_grid->AddProperty(RefPtr<ui::toolkit::PropertyEditor>(
                    MakeRef<ui::toolkit::StringEditor>(
                        DefaultAllocator(), field.label.AsView(), field.textValue.AsView(),
                        Function<void(StringView)>{[self, index](StringView v)
                                                   {
                                                       AssetFormField patch = self->m_fields[index];
                                                       patch.textValue = String(v);
                                                       self->ApplyFieldEdit(index, patch);
                                                   }},
                        cat)
                        .Get()));
                continue;
            }
            if (field.kind == AssetFormFieldKind::Guid)
            {
                // Canonical string row (TryParse-validated) + an untyped Pick button.
                utf8char buffer[37];
                field.guidValue.ToChars(buffer);
                m_grid->AddProperty(RefPtr<ui::toolkit::PropertyEditor>(
                    MakeRef<ui::toolkit::StringEditor>(
                        DefaultAllocator(), field.label.AsView(), StringView{buffer, 36},
                        Function<void(StringView)>{[self, index](StringView v)
                                                   {
                                                       Guid parsed{};
                                                       if (!Guid::TryParse(v, parsed))
                                                       {
                                                           return; // invalid: keep the old value
                                                       }
                                                       AssetFormField patch = self->m_fields[index];
                                                       patch.guidValue = parsed;
                                                       self->ApplyFieldEdit(index, patch);
                                                   }},
                        cat)
                        .Get()));
                String pickLabel(u8"Pick ");
                pickLabel.Append(field.label.AsView());
                m_grid->AddProperty(RefPtr<ui::toolkit::PropertyEditor>(
                    MakeRef<ui::toolkit::ButtonEditor>(
                        DefaultAllocator(), pickLabel.AsView(),
                        Function<void()>{[self, index]()
                                         {
                                             ui::UIContext* ctx = self->Ctx();
                                             if (ctx == nullptr ||
                                                 self->m_context->Project() == nullptr)
                                             {
                                                 return;
                                             }
                                             auto dialog = MakeRef<app::AssetPickerDialog>(
                                                 DefaultAllocator(), *self->m_context,
                                                 Array<String>{}); // empty filter = every type
                                             dialog->OnPicked = [self, index](const Guid& picked)
                                             {
                                                 AssetFormField patch = self->m_fields[index];
                                                 patch.guidValue = picked;
                                                 self->ApplyFieldEdit(index, patch);
                                             };
                                             dialog->Show(ctx);
                                         }},
                        cat)
                        .Get()));
                continue;
            }
            // Scalars.
            switch (field.scalarKind)
            {
            case ScalarKind::Bool:
                m_grid->AddProperty(RefPtr<ui::toolkit::PropertyEditor>(
                    MakeRef<ui::toolkit::BoolEditor>(
                        DefaultAllocator(), field.label.AsView(), field.intValue != 0,
                        Function<void(bool)>{[self, index](bool v)
                                             {
                                                 AssetFormField patch = self->m_fields[index];
                                                 patch.intValue = v ? 1 : 0;
                                                 self->ApplyFieldEdit(index, patch);
                                             }},
                        cat)
                        .Get()));
                break;
            case ScalarKind::Float32:
            case ScalarKind::Float64:
                m_grid->AddProperty(RefPtr<ui::toolkit::PropertyEditor>(
                    MakeRef<ui::toolkit::FloatEditor>(
                        DefaultAllocator(), field.label.AsView(), field.floatValue, -1e12, 1e12,
                        0.01, 4,
                        Function<void(f64)>{[self, index](f64 v)
                                            {
                                                AssetFormField patch = self->m_fields[index];
                                                patch.floatValue = v;
                                                self->ApplyFieldEdit(index, patch);
                                            }},
                        cat)
                        .Get()));
                break;
            default:
                m_grid->AddProperty(RefPtr<ui::toolkit::PropertyEditor>(
                    MakeRef<ui::toolkit::IntEditor>(
                        DefaultAllocator(), field.label.AsView(), field.intValue,
                        -9007199254740992ll, 9007199254740992ll,
                        Function<void(i64)>{[self, index](i64 v)
                                            {
                                                AssetFormField patch = self->m_fields[index];
                                                patch.intValue = v;
                                                self->ApplyFieldEdit(index, patch);
                                            }},
                        cat)
                        .Get()));
                break;
            }
        }
    }

    void GenericAssetEditorPage::ApplyFieldEdit(usize index, const AssetFormField& newValue)
    {
        if (m_object.Get() == nullptr)
        {
            return;
        }
        (void)ApplyAssetFormField(*m_object, m_fields, index, newValue);

        // Undo step (coalesced per field label + ordinal).
        String key(newValue.label.AsView());
        key.Append(Format(u8"#{}", index).AsView());
        Array<byte> after = Snapshot();
        (void)Commands().Execute(UniquePtr<IEditorCommand>(
            DefaultAllocator().New<EditGenericCommand>(*this, key.AsView(), m_undoBaseline, after),
            DefaultAllocator()));
        m_undoBaseline = Move(after);
        MarkDirty();

        // Re-scan: a conditional-branch shape change rebuilds the grid (deferred - never
        // mid-scrub); a same-shape patch just refreshes the stored fields.
        Array<AssetFormField> rescanned;
        (void)ScanAssetForm(*m_object, rescanned);
        const bool sameShape = AssetFormShapeEquals(m_fields, rescanned);
        m_fields = Move(rescanned);
        if (!sameShape)
        {
            GenericAssetEditorPage* self = this;
            if (ui::UIContext* ctx = Ctx())
            {
                ctx->MutationQueueRef().QueueAction(
                    Function<void()>{[self]() { self->BuildGrid(); }});
            }
        }
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
        PushCurrentVersions(ar, *m_object->GetType()); // include version-gated fields
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
        PushCurrentVersions(ar, *m_object->GetType());
        m_object->Serialize(ar);
        ar.PopVersionScope();
        m_undoBaseline = blob;
        (void)ScanAssetForm(*m_object, m_fields);
        GenericAssetEditorPage* self = this;
        if (ui::UIContext* ctx = Ctx())
        {
            ctx->MutationQueueRef().QueueAction(Function<void()>{[self]() { self->BuildGrid(); }});
        }
        else
        {
            BuildGrid();
        }
        MarkDirty();
    }

    ui::UIContext* GenericAssetEditorPage::Ctx() const
    {
        return (m_grid.Get() != nullptr) ? m_grid->Context : nullptr;
    }

    Status GenericAssetEditorPage::Save()
    {
        if (m_object.Get() == nullptr || m_context->Project() == nullptr)
        {
            return Status{ErrorCode::NotFound};
        }
        draconic::content::Instance* instance =
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
            DRACONIC_LOG_INFO(u8"Editor", u8"saved asset '{}'", m_title);
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
                                                              draconic::content::Instance& instance)
    {
        auto* page = DefaultAllocator().New<GenericAssetEditorPage>(context, instance);
        return UniquePtr<EditorPage>(page, DefaultAllocator());
    }
}
