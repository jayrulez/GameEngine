// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor App - :asset_object_page partition
//
// AssetObjectPage: what a page over one reflected asset does whatever its asset is
// (Documentation/Specs/editor-gui.md, 5.4), so a page adds only what is its own (a picture, a
// preview). It reads the asset when it opens; its markup (the page's) has the kit's bar, whose
// `page-save`, `page-undo` and `page-redo` it binds to the actions over the page, a grid
// (`page-grid`) and the state a page shows when its asset did not read (`page-state`, in place of
// the grid), and names its asset's kind in `page-kind`. The grid holds the asset's reflected properties under its type's name and, under
// "Source", what the page says of it as read-only rows (a file, a size), never as fields that do
// nothing when typed into (the catalog's inconsistency 8). Every edit is one undo step on the
// page's stack (ReflectedEditCommand), after which, and after its undo and redo, the page is told
// (OnObjectChanged) so its preview follows; an edit made by code (a preset) is one step too
// (EditWhole), and so is an edit a page follows with more (FollowEdit: a texture's usage sets its
// colour space). What a page warns of (a texture's data stored as sRGB) is a notice row over the
// rest, shown while it has something to say (SetNotice). Save writes the asset and asks for a cook, so what
// uses it takes the new one.

module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"
#include "Core/Reflection/Reflect.h"

export module editor.app:asset_object_page;

import foundation.core;
import foundation.content;
import foundation.runtime.client;
import foundation.gui;
import foundation.gui.toolkit;
import pipeline.core; // Asset: its source file
import editor.core;
import :ui_page;
import :page_action_bar;
import :property_sections;
import :property_extras;
import :resource_ref_editor;
import :asset_page;

using namespace foundation::core;

export namespace editor::app
{
    /// An asset's source file as its row shows it: read only, the path as the asset names it
    /// (reflection has no text for a SourcePath, so the page that holds the asset says it).
    class SourcePathEditor final : public gui::SingleControlEditor
    {
        RTTI_OBJECT(SourcePathEditor, gui::SingleControlEditor)
    public:
        static constexpr StringView kHint = u8"SourcePath";

        explicit SourcePathEditor(Function<String()> path) : m_path(foundation::core::Move(path)) // a widget's own Move would move it
        {
            AddClass(u8"source-path");
            m_text = MakeRef<gui::Label>(MemoryAllocator());
            m_text->AddClass(u8"source-path-text");
            Hold(*m_text);
        }
        [[nodiscard]] StringView Text() const { return m_text->GetText(); }

    protected:
        void OnBind(const gui::PropertyInfo&, const gui::PropertyValue&) override { m_text->SetText(m_path ? m_path().AsView() : StringView{}); }

    private:
        Function<String()> m_path;
        RefPtr<gui::Label> m_text;
    };

    RTTI_DEFINE_OBJECT(SourcePathEditor, "rtti::editor::editor::app")

    class AssetObjectPage : public UIEditorPage
    {
    public:
        static constexpr StringView kFactsTitle = u8"Source";
        static constexpr StringView kNoticesTitle = u8"Check";

        /// The page over `instance`, an asset of `type` (or one derived from it), from the markup
        /// at `markupPath`.
        AssetObjectPage(EditorContext& context, AssetPageServices services, foundation::content::Instance& instance, const TypeInfo& type,
                        StringView markupPath)
            : UIEditorPage(context.Allocator()), m_context(&context), m_services(Move(services)), m_title(instance.Name()), m_format(context.Allocator())
        {
            SetInstanceId(instance.Id());
            RefPtr<ISerializable> object = instance.ReadObject();
            if (object.Get() != nullptr && object->GetType() != nullptr && IsDerivedFrom(object->GetType(), &type))
                m_object = Move(object);
            else
                LOG_ERROR(u8"Editor", u8"asset '{}' failed to read - its page shows why", m_title);

            AssetObjectPage* self = this;
            m_properties = MakeUnique<gui::ReflectedPropertySource>(Allocator(), Allocator());
            m_properties->SetTextFormat(&m_format);
            m_properties->SetOnChange([self](const gui::ReflectedChange& change) { self->Changed(change); });
            m_facts = MakeUnique<gui::PropertyBag>(Allocator(), Allocator());
            m_notices = MakeUnique<gui::PropertyBag>(Allocator(), Allocator());
            m_sections = MakeUnique<PropertySections>(Allocator(), Allocator());
            if (m_object.Get() != nullptr)
            {
                const Instance target(m_object.Get(), m_object->GetType());
                m_properties->SetObjects(Span<const Instance>(&target, 1u));
                const StringView named = TypeAttrString(*m_object->GetType(), "displayName", StringView{});
                m_assetTitle = String(named.IsEmpty() ? StringView(u8"Properties") : named);
                StackSections();
                m_baseline = SnapshotObject(*m_object);
            }

            m_content = m_services.loadMarkup ? m_services.loadMarkup(markupPath) : RefPtr<gui::Node>{};
            if (!m_content)
                return;
            m_bar = MakeUnique<PageActionBar>(Allocator(), Allocator(), context.Actions(), *this);
            m_bar->Bind(Find<gui::ButtonBase>(u8"page-save"), u8"file.save");
            m_bar->Bind(Find<gui::ButtonBase>(u8"page-undo"), u8"edit.undo");
            m_bar->Bind(Find<gui::ButtonBase>(u8"page-redo"), u8"edit.redo");
            if (auto* kind = Find<gui::Label>(u8"page-kind"); kind != nullptr && m_object.Get() != nullptr)
                kind->SetText(TypeAttrString(*m_object->GetType(), "displayName", StringView{}));
            if (gui::Node* state = Find<gui::Node>(u8"page-state"))
                state->SetDisplayed(m_object.Get() == nullptr);
            m_grid = Find<gui::PropertyGrid>(u8"page-grid");
            if (m_grid != nullptr)
            {
                m_grid->SetDisplayed(m_object.Get() != nullptr);
                if (m_services.refs != nullptr)
                    ResourceRefEditor::Register(m_grid->GetEditors(), *m_services.refs);
                PropertyExtras::Register(m_grid->GetEditors()); // the notices
                m_grid->GetEditors().RegisterHint(SourcePathEditor::kHint, [self](IAllocator& allocator) -> RefPtr<gui::PropertyEditor>
                                                  { return MakeRef<SourcePathEditor>(allocator, [self]() { return self->SourceFile(); }); });
                m_grid->SetSource(m_sections.Get());
            }
        }
        ~AssetObjectPage() override
        {
            if (m_grid != nullptr)
                m_grid->SetSource(nullptr);
        }
        AssetObjectPage(const AssetObjectPage&) = delete;
        AssetObjectPage& operator=(const AssetObjectPage&) = delete;

        [[nodiscard]] StringView Title() const override { return m_title.AsView(); }
        [[nodiscard]] RefPtr<gui::Node> Content() override { return m_content; }
        void OnUpdate(foundation::runtime::IApplicationHost&, f32) override { Refresh(); }

        [[nodiscard]] Status Save() override
        {
            if (m_object.Get() == nullptr || m_context->Project() == nullptr)
                return Status{ErrorCode::NotFound};
            foundation::content::Instance* instance = m_context->Project()->SourceDb().GetInstance(InstanceId());
            if (instance == nullptr)
                return Status{ErrorCode::NotFound};
            const Status saved = instance->WriteObject(*m_object);
            if (saved.IsOk())
            {
                ClearDirty();
                m_context->RequestCook(false);
                LOG_INFO(u8"Editor", u8"saved asset '{}'", m_title);
            }
            return saved;
        }

        /// Once a frame: the bar as the actions are over the page.
        virtual void Refresh()
        {
            if (m_bar)
                m_bar->Sync();
        }

        // === For tests ===
        [[nodiscard]] bool LoadFailed() const noexcept { return m_object.Get() == nullptr; }
        [[nodiscard]] ISerializable* Object() const noexcept { return m_object.Get(); }
        [[nodiscard]] gui::PropertyGrid* Grid() const noexcept { return m_grid; }
        [[nodiscard]] gui::ReflectedPropertySource& Properties() noexcept { return *m_properties; }
        [[nodiscard]] gui::PropertyBag& Facts() noexcept { return *m_facts; }
        [[nodiscard]] PropertySections& Sections() noexcept { return *m_sections; }
        /// The asset's source file as its row says it ("Not set": embedded, or not a source asset).
        [[nodiscard]] String SourceFile() const
        {
            const auto* asset = Cast<pipeline::Asset>(m_object.Get());
            return asset != nullptr && !asset->fileName.IsEmpty() ? String(asset->fileName.View()) : String(u8"Not set");
        }
        /// What the notice `key` says ("" when it is not shown).
        [[nodiscard]] String Notice(StringView key) const
        {
            const gui::PropertyInfo* info = const_cast<gui::PropertyBag&>(*m_notices).FindInfo(FactPath(key).AsView());
            return info != nullptr && info->Visible ? String(m_notices->GetValue(FactPath(key).AsView(), 0).AsString()) : String{};
        }
        /// The value of the read-only row `name` ("" when there is none).
        [[nodiscard]] String Fact(StringView name) const
        {
            const gui::Variant value = m_facts->GetValue(FactPath(name).AsView(), 0);
            return String(value.AsString());
        }
        /// An edit of the asset's property `path` to `write`, as the grid commits one.
        void Edit(StringView path, const gui::PropertyWrite& write)
        {
            m_properties->BeginEdit(path);
            m_properties->CommitEdit(path, write);
        }

    protected:
        template <typename T>
        [[nodiscard]] T* ObjectAs() const
        {
            return Cast<T>(m_object.Get());
        }
        [[nodiscard]] EditorContext& Context() const noexcept { return *m_context; }
        [[nodiscard]] const AssetPageServices& Services() const noexcept { return m_services; }
        template <typename T>
        [[nodiscard]] T* Find(StringView id) const
        {
            return m_content ? FindById<T>(*m_content, id) : nullptr;
        }

        /// Shows `value` in the read-only row `name` under "Source", in the order first set.
        void SetFact(StringView name, StringView value)
        {
            const String path = FactPath(name);
            if (m_facts->FindInfo(path.AsView()) == nullptr)
            {
                gui::PropertyInfo& info = m_facts->Add(path.AsView(), gui::PropertyKind::String, gui::Variant(value));
                info.DisplayName = String(name);
                info.ReadOnly = true;
                m_facts->PropertiesChanged(); // Add told the grid before its name was set
                return;
            }
            if (m_facts->GetValue(path.AsView(), 0).AsString() != value)
                m_facts->SetValue(path.AsView(), 0, gui::Variant(value));
        }

        /// A section of the page's own (rows reflection does not show: a font's size ramp) after
        /// the asset's and before the facts; `source` is the page's and outlives it.
        void AddSection(StringView key, StringView title, gui::IPropertySource& source)
        {
            m_own.PushBack(OwnSection{String(key), String(title), &source});
            if (m_object.Get() != nullptr)
                StackSections();
        }

        /// Shows `text` as the notice `key` (a warning over the asset's rows); empty hides it.
        void SetNotice(StringView key, StringView text)
        {
            const String path = FactPath(key);
            gui::PropertyInfo* info = m_notices->FindInfo(path.AsView());
            if (info == nullptr)
            {
                gui::PropertyInfo& added = m_notices->Add(path.AsView(), gui::PropertyKind::String, gui::Variant(text));
                added.Hint = String(NoticePropertyEditor::kHint);
                added.ReadOnly = true;
                added.Visible = !text.IsEmpty();
                m_notices->PropertiesChanged(); // Add told the grid before the rest was set
                return;
            }
            if (info->Visible != !text.IsEmpty())
            {
                info->Visible = !text.IsEmpty();
                m_notices->PropertiesChanged();
            }
            if (m_notices->GetValue(path.AsView(), 0).AsString() != text)
                m_notices->SetValue(path.AsView(), 0, gui::Variant(text));
        }

        /// An edit of `path` (the grid's) has just landed in `object`: a page that follows it with
        /// more (a texture's usage setting its colour space) does so here and says true, and the
        /// whole of it is one undo step.
        virtual bool FollowEdit(StringView path, ISerializable& object)
        {
            (void)path;
            (void)object;
            return false;
        }

        /// An edit made by code that moves many values at once (a preset): `mutate` changes the
        /// asset, the whole of it before and after kept as one undo step.
        void EditWhole(StringView typeId, const Function<void(ISerializable&)>& mutate)
        {
            if (m_object.Get() == nullptr)
                return;
            Array<byte> before = m_baseline;
            mutate(*m_object);
            Array<byte> after = SnapshotObject(*m_object);
            AssetObjectPage* self = this;
            (void)Commands().Execute(UniquePtr<editor::IEditorCommand>(
                EditorRootAllocator().New<ObjectSnapshotCommand>(
                    [self]() { return self->m_object.Get(); }, Move(before), Move(after), typeId, [self]() { self->Applied(); }),
                EditorRootAllocator()));
        }

        /// The asset changed: an edit, its undo or redo. The page's preview follows it.
        virtual void OnObjectChanged() {}

    private:
        // A row's path: its name without what a path may not hold.
        [[nodiscard]] static String FactPath(StringView name)
        {
            String path;
            for (usize i = 0; i < name.Size(); ++i)
                if (name[i] != u8'.' && name[i] != u8'/' && name[i] != u8' ')
                    path.Append(name.SubStr(i, 1));
            return path;
        }

        void Changed(const gui::ReflectedChange& change)
        {
            AssetObjectPage* self = this;
            if (FollowEdit(change.Path.AsView(), *m_object))
            {
                // Followed by more than the source wrote: the whole asset before and after.
                (void)Commands().Execute(UniquePtr<editor::IEditorCommand>(
                    EditorRootAllocator().New<ObjectSnapshotCommand>(
                        [self]() { return self->m_object.Get(); }, m_baseline, SnapshotObject(*m_object), u8"asset_properties", [self]() { self->Applied(); }),
                    EditorRootAllocator()));
                return;
            }
            (void)Commands().Execute(UniquePtr<editor::IEditorCommand>(
                EditorRootAllocator().New<ReflectedEditCommand>(
                    *m_object->GetType(), [self]() -> void* { return self->m_object.Get(); }, change, m_format, u8"asset_properties",
                    [self]() { self->Applied(); }),
                EditorRootAllocator()));
        }

        // The sections in their order: the notices, the asset's, the page's own, the facts.
        void StackSections()
        {
            m_sections->Clear();
            m_sections->Add(u8"notices", kNoticesTitle, *m_notices);
            m_sections->Add(u8"asset", m_assetTitle.AsView(), *m_properties);
            for (const OwnSection& own : m_own)
                m_sections->Add(own.key.AsView(), own.title.AsView(), *own.source);
            m_sections->Add(u8"facts", kFactsTitle, *m_facts);
        }

        void Applied()
        {
            m_baseline = SnapshotObject(*m_object); // what the next edit made by code starts from
            m_properties->Refresh(); // an undo or a preset wrote it behind the source
            OnObjectChanged();
        }

        EditorContext* m_context;
        AssetPageServices m_services;
        String m_title;
        XmlPropertyTextFormat m_format;
        RefPtr<ISerializable> m_object;
        UniquePtr<gui::ReflectedPropertySource> m_properties;
        UniquePtr<gui::PropertyBag> m_facts;
        UniquePtr<gui::PropertyBag> m_notices;
        Array<byte> m_baseline; // the asset as the last step left it: a followed edit's before
        UniquePtr<PropertySections> m_sections;
        struct OwnSection
        {
            String key;
            String title;
            gui::IPropertySource* source = nullptr;
        };
        Array<OwnSection> m_own; // the page's sections (AddSection)
        String m_assetTitle;
        RefPtr<gui::Node> m_content;
        gui::PropertyGrid* m_grid = nullptr; // the markup's
        UniquePtr<PageActionBar> m_bar;
    };
}
