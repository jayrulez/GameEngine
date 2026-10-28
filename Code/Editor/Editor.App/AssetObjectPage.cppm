// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor App - :asset_object_page partition
//
// AssetObjectPage: what a page over one reflected asset does whatever its asset is
// (Documentation/Specs/editor-gui.md, 5.4), so a page adds only what is its own (a picture, a
// preview). It reads the asset when it opens; its markup (the page's) has the kit's bar, whose
// `page-save`, `page-undo`, `page-redo`, `page-discard`, `page-cook` and `page-rebuild` it binds to the actions
// over the page, a grid (`page-grid`) and the state a page shows when its asset did not read
// (`page-state`, in place of the grid), and names its asset's kind in `page-kind`. The grid holds
// the asset's reflected properties under its type's name and, under "Source", what the page says
// of it as read-only rows (a file, a size), never as fields that do nothing when typed into (the
// catalog's inconsistency 8). Every edit is one undo step on the page's stack
// (ReflectedEditCommand), after which, and after its undo and redo, the page is told
// (OnObjectChanged) so its preview follows; an edit made by code (a preset) is one step too
// (EditWhole), and so is an edit a page follows with more (FollowEdit: a texture's usage sets its
// colour space). What a page warns of (a texture's data stored as sRGB) is a notice row over the
// rest, shown while it has something to say (SetNotice). Save writes the asset and asks for a
// cook, so what uses it takes the new one.

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
import foundation.vfs; // SourcePath: a relinked source
import editor.core;
import :ui_page;
import :page_action_bar;
import :property_sections;
import :property_extras;
import :resource_ref_editor;
import :asset_page;
import :path_picker_dialog;
import :picker;

using namespace foundation::core;

export namespace editor::app
{
    /// An asset's source file as its row shows it: the path as the asset names it (reflection has
    /// no text for a SourcePath, so the page that holds the asset says it), never typed (a typo
    /// would cook a dangling reference); with `browse`, a Browse button picking another file of
    /// the project's (a font's source, relinked).
    class SourcePathEditor final : public gui::PropertyEditor
    {
        RTTI_OBJECT(SourcePathEditor, gui::PropertyEditor)
    public:
        static constexpr StringView kHint = u8"SourcePath";

        explicit SourcePathEditor(Function<String()> path, Function<void()> browse = {})
            : m_path(foundation::core::Move(path)), m_browse(foundation::core::Move(browse)) // a widget's own Move would move it
        {
            AddClass(u8"source-path");
            EditLayoutStyle([](gui::LayoutStyle& s)
            {
                s.Direction = gui::FlexDirection::Row;
                s.AlignItems = gui::Align::Center;
                s.ColumnGap = 6.0f;
            });
            m_text = MakeRef<gui::Label>(MemoryAllocator());
            m_text->AddClass(u8"source-path-text");
            m_text->EditLayoutStyle([](gui::LayoutStyle& s)
            {
                s.Grow = 1.0f;
                s.Shrink = 1.0f;
            });
            AddChild(m_text.Get());
            m_button = MakeRef<gui::Button>(MemoryAllocator());
            m_button->AddClass(u8"source-path-browse");
            m_button->SetText(u8"Browse...");
            m_button->SetTooltip(u8"Pick another file of the project's as its source");
            SourcePathEditor* self = this;
            m_button->SetOnClick([self]()
                                 {
                                     if (self->m_browse)
                                         self->m_browse();
                                 });
            m_button->SetDisplayed(static_cast<bool>(m_browse));
            AddChild(m_button.Get());
        }
        [[nodiscard]] StringView Text() const { return m_text->GetText(); }
        [[nodiscard]] gui::Button* BrowseButton() const noexcept { return m_browse ? m_button.Get() : nullptr; }

    protected:
        void OnBind(const gui::PropertyInfo&, const gui::PropertyValue&) override
        {
            m_text->SetText(m_path ? m_path().AsView() : StringView{});
            m_button->SetEnabled(!IsReadOnly());
        }
        void OnTextStyleChange() override
        {
            ShareTextStyle(*m_text);
            ShareTextStyle(*m_button);
        }

    private:
        Function<String()> m_path;
        Function<void()> m_browse;
        RefPtr<gui::Label> m_text;
        RefPtr<gui::Button> m_button;
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
            m_bar->Bind(Find<gui::ButtonBase>(u8"page-discard"), u8"page.discardChanges");
            m_bar->Bind(Find<gui::ButtonBase>(u8"page-cook"), u8"build.cookPage");
            m_bar->Bind(Find<gui::ButtonBase>(u8"page-rebuild"), u8"build.rebuildPage");
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
                                                  {
                                                      Function<void()> browse;
                                                      if (!self->SourceExtensions().IsEmpty() && self->m_services.pick)
                                                          browse = [self]() { self->BrowseSource(); };
                                                      return MakeRef<SourcePathEditor>(allocator, [self]() { return self->SourceFile(); }, Move(browse));
                                                  });
                m_grid->SetSource(m_sections.Get());
            }
        }
        ~AssetObjectPage() override
        {
            if (m_grid != nullptr)
                m_grid->SetSource(nullptr);
            // The page's own sections' sources died with the derived page, before this runs: forget
            // every section unasked (each source dies with the page, so none keeps a client in use).
            if (m_sections)
                m_sections->Abandon();
        }
        AssetObjectPage(const AssetObjectPage&) = delete;
        AssetObjectPage& operator=(const AssetObjectPage&) = delete;

        [[nodiscard]] StringView Title() const override { return m_title.AsView(); }
        [[nodiscard]] RefPtr<gui::Node> Content() override { return m_content; }
        void OnUpdate(foundation::runtime::IApplicationHost&, f32) override { Refresh(); }
        /// The asset written outside the page (an agent's asset_data_write, page_reload): a clean
        /// page shows what is stored now, its undo let go; one with unsaved edits keeps them and
        /// says so, as the scene page does.
        void OnAssetExternallyModified() override
        {
            if (IsDirty())
            {
                m_context->Notify(NoticeKind::Warning, Format(u8"'{}' changed on disk but has unsaved edits here - not refreshed.", m_title).AsView());
                return;
            }
            (void)ReadAgain();
        }

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
        /// The kinds of file its source may be relinked to (".ttf"); none: its source row only says
        /// it (the default).
        [[nodiscard]] virtual Span<const String> SourceExtensions() const { return {}; }
        /// The source row's Browse: the files under the project's sources of those kinds, the
        /// current one checked; a pick relinks it.
        void BrowseSource()
        {
            const auto* asset = Cast<pipeline::Asset>(m_object.Get());
            if (asset == nullptr || !m_services.pick || Context().Project() == nullptr)
                return;
            PickerRequest request = PathPicker::Request(Allocator(), Context().Project()->SourcesRoot().AsView(), SourceExtensions(), asset->fileName.View());
            AssetObjectPage* self = this;
            request.OnPicked = [self](StringView picked) { self->RelinkSource(picked); };
            m_services.pick(Move(request));
        }
        /// Names `path` (under the project's sources) as the asset's source, one undo step; the
        /// page's rows read it again.
        void RelinkSource(StringView path)
        {
            if (Cast<pipeline::Asset>(m_object.Get()) == nullptr || path.IsEmpty())
                return;
            const String file(path);
            EditWhole(u8"asset_page.source", [file](ISerializable& object)
                      {
                          if (auto* asset = Cast<pipeline::Asset>(&object))
                              asset->fileName = foundation::vfs::SourcePath(file.AsView());
                      });
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
        /// A button of the page's own bound to the action `id` over the page, as the kit's bar's
        /// are (a clip's transport to the playback actions): enabled, checked and its tooltip
        /// following the action each frame.
        void BindAction(gui::ButtonBase* button, StringView id)
        {
            if (m_bar)
                m_bar->Bind(button, id);
        }
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
        /// the asset's and before the facts; `source` is the page's, dying with it.
        void AddSection(StringView key, StringView title, gui::IPropertySource& source)
        {
            m_own.PushBack(OwnSection{String(key), String(title), &source});
            if (m_object.Get() != nullptr)
                StackSections();
        }

        /// The asset read again from its source (something else wrote it: a scene's save writing a
        /// profile's values); its undo let go, as the steps named the asset it had. False when it
        /// did not read, the page keeping what it had.
        bool ReadAgain()
        {
            foundation::content::Instance* instance = m_context->Project() != nullptr ? m_context->Project()->SourceDb().GetInstance(InstanceId()) : nullptr;
            RefPtr<ISerializable> object = instance != nullptr ? instance->ReadObject() : RefPtr<ISerializable>{};
            if (m_object.Get() == nullptr || object.Get() == nullptr || object->GetType() != m_object->GetType())
                return false;
            m_object = Move(object);
            const Instance target(m_object.Get(), m_object->GetType());
            m_properties->SetObjects(Span<const Instance>(&target, 1u));
            m_baseline = SnapshotObject(*m_object);
            Commands().Clear();
            ClearDirty();
            OnObjectChanged();
            return true;
        }

        /// The page's own sections let go (they are rebuilt: a material's rows follow its shader).
        void ClearSections()
        {
            m_own.Clear();
            if (m_object.Get() != nullptr)
                StackSections();
        }
        /// The asset's reflected rows left out: a page whose asset reflects what no row should show
        /// (a material's source, a property table in parallel arrays) gives the grid its own.
        void HideAssetRows()
        {
            m_showsAssetRows = false;
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
            // The asset as it is (not the last step of the base's own: a page's own rows edit it too).
            Array<byte> before = SnapshotObject(*m_object);
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
            if (m_showsAssetRows)
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
        bool m_showsAssetRows = true; // HideAssetRows
        RefPtr<gui::Node> m_content;
        gui::PropertyGrid* m_grid = nullptr; // the markup's
        UniquePtr<PageActionBar> m_bar;
    };
}
