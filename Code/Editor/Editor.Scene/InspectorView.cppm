// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Scene - :inspector partition.
//
// SceneInspectorView: the reflection-driven property inspector INSIDE a scene page (per-page,
// like everything scene-scoped). A toolkit PropertyGrid rebuilt from the primary
// selection: an Entity section (name / active), a Transform section (position / rotation-as-
// euler-degrees / scale), and one category per component with rows auto-generated from the
// component type's reflected properties (f32, ints, bool, String, Float3, Color, enums via the
// PropertyInfo::address raw path). Every edit routes through the SceneEditContext commands, so
// field scrubs merge into single undo entries. [+ Add Component] lists the scene's managers;
// each component category ends with a Remove row.
//
// Rebuild vs refresh: a cheap per-frame SIGNATURE (selected entity + scene revision + which
// managers have a component) decides structural rebuilds; otherwise per-editor refreshers pull
// model values into the widgets (skipped while that editor has an active edit gesture), so
// undo/redo and external changes (gizmos later) stay live.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"
#include <limits>
#include <initializer_list>

export module editor.scene:inspector;

import foundation.core;
import foundation.content;
import foundation.resource;
import foundation.geometry;
import foundation.animation;
import foundation.materials;
import foundation.texture.resource;
import foundation.particles.resource;
import foundation.scene;
import engine.render;
import foundation.physics;
import foundation.physics.resource;
import engine.physics;
import foundation.audio;
import foundation.audio.resource;
import engine.audio;
import foundation.ui.resource;
import foundation.script.resource;
import engine.script;
import foundation.ui;
import foundation.ui.toolkit;
import editor.core;
import editor.app;
import :edit;

using namespace foundation::core;
namespace core = foundation::core;

export namespace editor
{
    namespace ui = foundation::ui;
    namespace scene = foundation::scene;

    // The asset reference row is Editor.App's (every page uses it): typed at construction, and
    // BindAsset wires pick, drop and clear to one assignment.
    using editor::app::ResourceRefEditor;

    // --- Property-attribute conventions (reflection PropAttribute metadata -> inspector) ---
    //
    //   "displayName"  String  - row label override (default: prettified property name)
    //   "description"  String  - row tooltip
    //   "range"        Float4  - {min, max, step, unused}: f32 rows become slider+field
    //   "visibleWhen"  String  - "prop" (visible while prop is truthy) or "prop=1,2"
    //                            (visible while prop's raw int value is in the list)

    /// Parsed "visibleWhen" condition.
    struct PropertyCondition
    {
        String prop;       // the dependent property's reflected name
        Array<i64> values; // empty = truthy test
    };

    [[nodiscard]] inline bool ParsePropertyCondition(StringView spec, PropertyCondition& out)
    {
        const utf8char* d = spec.Data();
        usize eq = spec.Size();
        for (usize i = 0; i < spec.Size(); ++i)
        {
            if (d[i] == u8'=')
            {
                eq = i;
                break;
            }
        }
        if (eq == 0)
        {
            return false;
        }
        out.prop = String(spec.SubStr(0, eq));
        out.values.Clear();
        if (eq == spec.Size())
        {
            return true;
        } // truthy form
        i64 value = 0;
        bool negative = false;
        bool any = false;
        for (usize i = eq + 1; i <= spec.Size(); ++i)
        {
            const utf8char c = (i < spec.Size()) ? d[i] : u8','; // sentinel comma flushes
            if (c == u8',')
            {
                if (!any)
                {
                    return false;
                }
                out.values.PushBack(negative ? -value : value);
                value = 0;
                negative = false;
                any = false;
            }
            else if (c == u8'-' && !any && !negative)
            {
                negative = true;
            }
            else if (c >= u8'0' && c <= u8'9')
            {
                value = value * 10 + (c - u8'0');
                any = true;
            }
            else
            {
                return false;
            }
        }
        return !out.values.IsEmpty();
    }

    [[nodiscard]] inline bool MatchesPropertyCondition(const PropertyCondition& condition, i64 raw)
    {
        if (condition.values.IsEmpty())
        {
            return raw != 0;
        }
        for (i64 v : condition.values)
        {
            if (v == raw)
            {
                return true;
            }
        }
        return false;
    }

    /// "castsShadows" -> "Casts Shadows", "fovYRadians" -> "Fov Y Radians", "IBL" -> "IBL".
    [[nodiscard]] inline String PrettifyPropertyName(StringView name)
    {
        const utf8char* d = name.Data();
        String out;
        bool prevLower = false;
        bool prevUpper = false;
        for (usize i = 0; i < name.Size(); ++i)
        {
            utf8char c = d[i];
            const bool upper = (c >= u8'A' && c <= u8'Z');
            const bool lower = (c >= u8'a' && c <= u8'z');
            if (i == 0 && lower)
            {
                c = static_cast<utf8char>(c - (u8'a' - u8'A'));
            }
            else if (upper)
            {
                const bool nextLower =
                    (i + 1 < name.Size()) && (d[i + 1] >= u8'a' && d[i + 1] <= u8'z');
                if (prevLower || (prevUpper && nextLower))
                {
                    out += u8' ';
                }
            }
            out += c;
            prevLower = lower;
            prevUpper = upper;
        }
        return out;
    }

    // Bespoke editor for the physics collision-group matrix (a shape reflection rows
    // can't express): one row per named group - name field + a toggle per column group.
    // Symmetric by construction (a toggle writes BOTH directions); every edit is one
    // whole-block undoable command, and the inspector's structural rebuild re-reads.
    class CollisionMatrixEditor final : public ui::toolkit::PropertyEditor
    {
        RTTI_OBJECT(CollisionMatrixEditor, ui::toolkit::PropertyEditor)
    public:
        Array<String> names; // display names (index = group)
        Array<u32> matrix;   // parallel collide masks
        // The last COMMITTED names, snapshotted at each grid build: the blur-commit path compares
        // against these so leaving an untouched field pushes no undo entry, and an Enter-committed
        // rename (whose rebuild refreshes this snapshot) is not committed a second time on the old
        // field's focus loss.
        Array<String> committedNames;
        Function<void(usize, String)> OnRename;
        Function<void(usize, usize)> OnToggle; // (row group, column group)
        Function<void()> OnAddGroup;
        Function<void(usize)> OnRemoveGroup; // remove group `index` (only the last is offered)

        CollisionMatrixEditor(StringView name, StringView category)
            : ui::toolkit::PropertyEditor(name, category)
        {
        }

        void RefreshView() override {}

        /// Rebuild the grid after a STRUCTURAL change (add group / rename / toggle). Deferred via the
        /// UI mutation queue: the trigger is a button-click event, and rebuilding tears down the very
        /// views dispatching it, which must not happen mid-event (UIContext mutation-queue rule).
        void RequestRebuild();

    protected:
        RefPtr<ui::View> CreateEditorView() override;

    private:
        void BuildGrid(ui::FlexLayout& column); // the row/cell/add-button build (rerun on rebuild)
        RefPtr<ui::FlexLayout> m_column;         // the editor view, kept so a rebuild can repopulate it
    };

    // A read-only inspector NOTICE row: a wrapped advisory label spanning the editor column (no value
    // editing). Used for conditional hints like "RigidBody shape=Cooked but no collision shape set".
    class NoticeEditor final : public ui::toolkit::PropertyEditor
    {
        RTTI_OBJECT(NoticeEditor, ui::toolkit::PropertyEditor)
    public:
        String message;
        NoticeEditor(StringView name, StringView category)
            : ui::toolkit::PropertyEditor(name, category)
        {
        }
        void RefreshView() override {}

    protected:
        RefPtr<ui::View> CreateEditorView() override;
    };

    // The generic list-of-asset-slots editor (add icon + AssetPickerSlot rows with move/remove) now
    // lives in the shared editor.app layer as editor::app::ContainerListEditor, so bespoke asset pages
    // reuse the identical widget. Brought into this namespace below for the inspector's use sites.
    using editor::app::ContainerListEditor;

    // Raw integral value of a bool/enum/int property via the address escape hatch.
    [[nodiscard]] inline i64 RawPropertyValue(const Instance& obj, const PropertyInfo& p)
    {
        void* address = (p.address != nullptr) ? p.address(obj) : nullptr;
        return address != nullptr ? ReadEnumValue(address, *p.type) : 0;
    }

    // Applies the displayName/description/visibleWhen conventions to every row that `prop`'s row
    // builder just added to `grid` (rows firstRow..end). `instance` is a copyable callable
    // re-reading the owning object each frame so visibleWhen rows follow live edits (a plain
    // lambda, NOT core::Function - that one is move-only and each row's refresher needs its own).
template <typename GetInstance>
void ApplyPropertyPresentation(ui::toolkit::PropertyGrid& grid,
                               Array<Function<void()>>& refreshers, const TypeInfo* type,
                               const PropertyInfo& prop, usize firstRow, GetInstance instance)
    {
        const core::Attribute* displayName = FindAttribute(prop, u8"displayName");
        const core::Attribute* description = FindAttribute(prop, u8"description");
        const core::Attribute* visibleWhen = FindAttribute(prop, u8"visibleWhen");

        // Resolve the dependent property + condition once; refreshers share them.
        const PropertyInfo* dependent = nullptr;
        PropertyCondition condition;
        if (visibleWhen != nullptr)
        {
            const String* spec = visibleWhen->value.TryGet<String>();
            if (spec != nullptr && ParsePropertyCondition(spec->AsView(), condition))
            {
                for (const PropertyInfo& p : Properties(*type))
                {
                    if (StringView(reinterpret_cast<const utf8char*>(p.name)) ==
                        condition.prop.AsView())
                    {
                        dependent = &p;
                        break;
                    }
                }
            }
        }

        for (usize i = firstRow; i < grid.PropertyCount(); ++i)
        {
            ui::toolkit::PropertyEditor* editor = grid.PropertyAt(i);
            const String* label =
                (displayName != nullptr) ? displayName->value.TryGet<String>() : nullptr;
            editor->SetDisplayName(label != nullptr
                                       ? label->AsView()
                                       : PrettifyPropertyName(editor->Name()).AsView());
            if (description != nullptr)
            {
                if (const String* s = description->value.TryGet<String>())
                {
                    editor->SetTooltip(s->AsView());
                }
            }
            if (dependent != nullptr)
            {
                auto refresh = [editor, dependent, condition, get = instance]()
                {
                    const Instance obj = get();
                    editor->SetRowVisible(
                        !obj.IsEmpty() &&
                        MatchesPropertyCondition(condition, RawPropertyValue(obj, *dependent)));
                };
                refresh();
                refreshers.PushBack(Function<void()>{Move(refresh)});
            }
        }
    }

    // A settings block's section name: its type's name minus a trailing "Settings"
    // ("EnvironmentSettings" -> "Environment").
    [[nodiscard]] inline StringView SettingsCategoryName(const TypeInfo& type)
    {
        StringView category(reinterpret_cast<const utf8char*>(type.name));
        const StringView suffix = u8"Settings";
        if (category.Size() > suffix.Size() &&
            category.SubStr(category.Size() - suffix.Size(), suffix.Size()) == suffix)
        {
            category = category.SubStr(0, category.Size() - suffix.Size());
        }
        return category;
    }

    // === Settings rows: a reflected settings block's fields, wherever its values live ===
    // The scene inspector's settings sections and a profile asset's page build the same rows
    // (enum dropdowns, colour pickers, sliders from the reflected ranges, reference pickers):
    // `values` answers where a field's value is now, and the setters are the host's undoable
    // writes (the scene's settings commands, the page's asset edits). Ref-counted so its
    // move-only hooks are shared by every row's closures.
    struct SettingsAccess : public RefCounted
    {
        const TypeInfo* type = nullptr;
        Function<void*(const char* property)> values; // null = nothing to show
        Function<void(const char* property, const Variant& value)> set;
        Function<void(const char* property, i64 raw)> setRaw; // enums, by their underlying value
        Function<void(const char* property, const Guid& id)> setReference; // reference-shaped
    };

    class SettingsRows
    {
    public:
        SettingsRows(EditorContext& editor, ui::toolkit::PropertyGrid& grid,
                     Array<Function<void()>>& refreshers)
            : m_editor(&editor), m_grid(&grid), m_refreshers(&refreshers)
        {
        }

        /// `prop`'s row (a leaf field of access->type), presented by its attributes. A kind with
        /// no row yet adds none.
        void Build(const RefPtr<SettingsAccess>& access, const PropertyInfo& prop,
                   StringView category);

        /// A row of the host's own (a button), refreshed with the rest.
        void AddEditor(ui::toolkit::PropertyEditor* editor, Function<void()> refresher);

    private:
        void BuildRow(const RefPtr<SettingsAccess>& access, const PropertyInfo& prop,
                      StringView category);
        void BuildReferenceRow(const RefPtr<SettingsAccess>& access, const PropertyInfo& prop,
                               StringView category);

        EditorContext* m_editor;
        ui::toolkit::PropertyGrid* m_grid;
        Array<Function<void()>>* m_refreshers;
    };

    class SceneInspectorView : public ui::ViewGroup
    {
        RTTI_OBJECT(SceneInspectorView, ui::ViewGroup)
    public:
        /// A script behavior's section in the grid: "Behavior N - <class>".
        [[nodiscard]] static String ScriptBehaviorSection(usize index, StringView scriptName);

        SceneInspectorView(EditorContext& editor, SceneEditContext& edit)
            : m_editor(&editor), m_edit(&edit)
        {
            // Two tabs: Entity (the selected entity's sections + Add/Paste) and Scene (the scene's
            // settings). The Scene tab makes scene-settings a first-class view reachable anytime,
            // instead of requiring a deselect (empty-viewport click) to surface them.
            m_tabView = MakeRef<ui::TabView>(MemoryAllocator());
            m_tabView->TabsClosable.SetValue(false);
            {
                SceneInspectorView* self = this;
                m_tabView->OnTabChanged.Add(
                    [self](ui::TabView*, i32) { self->m_forceRebuild = true; });
            }

            // --- Entity tab ---
            auto entityColumn = MakeRef<ui::FlexLayout>(MemoryAllocator());
            entityColumn->Direction = ui::Orientation::Vertical;
            entityColumn->Padding = ui::Thickness{8, 6}; // inset off the panel edge (like hierarchy)

            m_emptyLabel = MakeRef<ui::Label>(MemoryAllocator(),
                                              StringView(u8"Select an entity to inspect."));
            m_emptyLabel->FontSize.SetValue(12.0f);
            m_emptyLabel->Visibility = ui::Visibility::Gone; // shown only when nothing is selected
            {
                ui::LayoutStyle lp;
                lp.Width = ui::SizeSpec::Match();
                entityColumn->AddView(m_emptyLabel.Get(), lp);
            }

            m_entityGrid = MakeRef<ui::toolkit::PropertyGrid>(MemoryAllocator());
            {
                ui::LayoutStyle grow;
                grow.FlexGrow = 1.0f;
                entityColumn->AddView(m_entityGrid.Get(), grow);
            }

            m_addButton = MakeRef<ui::Button>(MemoryAllocator(), StringView(u8"Add Component"));
            {
                SceneInspectorView* self = this;
                m_addButton->OnClick.Add([self](ui::ButtonBase*) { self->ShowAddComponentMenu(); });
                ui::LayoutStyle lp;
                lp.Width = ui::SizeSpec::Match();
                entityColumn->AddView(m_addButton.Get(), lp);
            }

            // Paste Component: below Add Component, shown only when the clipboard holds a component
            // (UpdatePasteButton, run each Refresh). Pasting over an existing same-type component
            // overwrites it, so that case asks for confirmation first.
            m_pasteButton = MakeRef<ui::Button>(MemoryAllocator(), StringView(u8"Paste Component"));
            {
                SceneInspectorView* self = this;
                m_pasteButton->OnClick.Add([self](ui::ButtonBase*) { self->PasteSelectedComponent(); });
                m_pasteButton->Visibility = ui::Visibility::Gone;
                ui::LayoutStyle lp;
                lp.Width = ui::SizeSpec::Match();
                lp.Margin = ui::Thickness{0.0f, 6.0f, 0.0f, 0.0f}; // gap below Add Component
                entityColumn->AddView(m_pasteButton.Get(), lp);
            }

            // --- Scene tab ---
            auto sceneColumn = MakeRef<ui::FlexLayout>(MemoryAllocator());
            sceneColumn->Direction = ui::Orientation::Vertical;
            sceneColumn->Padding = ui::Thickness{8, 6};
            m_sceneGrid = MakeRef<ui::toolkit::PropertyGrid>(MemoryAllocator());
            {
                ui::LayoutStyle grow;
                grow.FlexGrow = 1.0f;
                sceneColumn->AddView(m_sceneGrid.Get(), grow);
            }

            m_tabView->AddTab(u8"Entity", entityColumn.Get());
            m_tabView->AddTab(u8"Scene", sceneColumn.Get());
            m_grid = m_entityGrid; // the active grid; Rebuild re-points it to the selected tab

            AddView(m_tabView.Get());
        }

        /// Per-frame: structural rebuild when the shape changed, else pull values into widgets.
        void Refresh();

        [[nodiscard]] ui::toolkit::PropertyGrid* Grid() const noexcept { return m_grid.Get(); }

        // Fill the available space (wrap-to-children would collapse the scrolling grid).
        void OnMeasure(ui::BoxConstraints constraints) override;
        void OnLayout(f32, f32, f32 width, f32 height) override;

    private:
        // TypeOf<T> for a type nobody registered still exists, named "<value>" - such
        // components (e.g. from a game module without reflection) can't be edited or even
        // sensibly LISTED, so the Add menu skips them and sections fall back to the manager's
        // serialization id when available.
        [[nodiscard]] static bool IsRegisteredType(const TypeInfo* type);

        [[nodiscard]] Guid SelectedEntity() const;

        // Selected entity + scene revision + which managers have a component on it.
        [[nodiscard]] u64 Signature();

        void Rebuild();

        void BuildEntitySection(const Guid& id);

        void BuildTransformSection(const Guid& id);

        void BuildSceneSettingsSections();

        // The collision-group matrix (physics settings): a bespoke grid row editing the
        // names + symmetric collide matrix through whole-block undoable commands.
        void BuildCollisionMatrixRow(const TypeInfo* type, StringView category);

        // A block whose values can come from a profile: Make Profile, Copy Into Scene and Open
        // Profile, between the block's own fields (the source, the profile) and its values.
        void BuildSettingsProfileRows(SettingsRows& rows, const TypeInfo* type, StringView category);
        // The scene's settings block of `type` as a settings-rows access: values where an edit of
        // each field lands (SceneEditContext::SettingsValuesFor), written through its commands.
        [[nodiscard]] RefPtr<SettingsAccess> SceneSettingsAccess(const TypeInfo* type);

        void BuildComponentSection(const Guid& id, scene::ComponentManagerBase& mgr);

        // Raw integral value of a bool/enum/int property via the address escape hatch.
        [[nodiscard]] static i64 RawIntValue(const Instance& obj, const PropertyInfo& p);

        // Applies the displayName/description/visibleWhen conventions to every row that
        // `prop`'s Build*Row call just added (rows firstRow..end). `instance` is a copyable
        // callable re-reading the owning object each frame so visibleWhen rows follow live
        // edits (a plain lambda, NOT core::Function - that one is move-only and each row's
        // refresher needs its own copy).
        template <typename GetInstance>
        void ApplyPropertyPresentation(const TypeInfo* type, const PropertyInfo& prop,
                                       usize firstRow, GetInstance instance)
        {
            editor::ApplyPropertyPresentation(*m_grid, m_refreshers, type, prop, firstRow,
                                              Move(instance));
        }

        // `path` addresses the property's owner: the component (empty) or a struct element of one
        // of its reflected container properties (BuildContainerRows' per-slot rows).
        void BuildPropertyRow(const Guid& id, const TypeInfo* type, const PropertyInfo& prop,
                              StringView category, ComponentPropertyPath path = {});

        // Generic reflected CONTAINER property (a reflected list member): one grid row whose editor is
        // a ContainerListEditor - a header add + a slot row per element (an asset-picker slot + move /
        // remove icons). Slot text + the pick/add/remove/move callbacks are wired here to the reflection
        // container ops via MutateComponent (one undo step each); a content-diff refresher forces a
        // rebuild when the list changes (which Signature() does not track). Element pick is currently
        // specialized to Ref<Material>; struct-element leaf editing + the polymorphic add-by-type menu
        // are a follow-up.
        // `path` set: the list is a field of that container element (a slot's own list).
        void BuildContainerRows(const Guid& id, const TypeInfo* type, const PropertyInfo& prop,
                                StringView category, ComponentPropertyPath path = {});

        // Current target Guid of a Ref<T> property (nil when unset/unresolvable).
        template <typename T>
        [[nodiscard]] Guid RefTarget(const Guid& id, const TypeInfo* type, const char* propName,
                                     const ComponentPropertyPath& path = {})
        {
            const TypeInfo* ownerType = nullptr;
            const Instance owner = m_edit->ResolvePropertyOwner(id, type, path, &ownerType);
            const PropertyInfo* p = (owner.IsEmpty() || ownerType == nullptr)
                                        ? nullptr
                                        : FindProperty(*ownerType, propName);
            void* address = (p != nullptr && p->address != nullptr) ? p->address(owner) : nullptr;
            return (address != nullptr) ? static_cast<foundation::resource::Ref<T>*>(address)->id
                                        : Guid{};
        }


        /// The display names the slots editor renders - recomputed by its refresher to
        /// detect shape/content changes (the inspector Signature only sees selection +
        /// component PRESENCE, so a data-only slot mutation or its undo changes nothing it
        /// watches).
        [[nodiscard]] Array<String> MaterialSlotNames(const engine::render::MeshComponent& mc);

        void BuildMaterialSlots(const Guid& id, StringView category);


        // The cooked ScriptClass a behavior references (bound through the editor's
        // resource manager so the harvested metadata is available; null when unset or
        // not yet cooked).
        [[nodiscard]] foundation::script::ScriptClass*
        BehaviorClass(const engine::script::ScriptBehavior& behavior);

        // Signature of the behavior list's SHAPE (count + script ids + enabled flags +
        // override counts) - the refresher forces a rebuild when it changes, since the
        // inspector's own Signature only watches selection + component presence.
        [[nodiscard]] u64 ScriptBehaviorsSignature(const engine::script::ScriptComponent& c);

        void BuildScriptBehaviors(const Guid& id, StringView category);

        // A behavior's section: its rows, with its move and remove icons in the header.
        void BuildScriptBehaviorRows(const Guid& id, usize index, usize count);
        // The live ScriptComponent of `id`, or null (re-resolved: component pools move).
        [[nodiscard]] engine::script::ScriptComponent* LiveScript(const Guid& id);

        // A property row's read/write hooks, so ONE set of row builders serves both the entity
        // behavior tier (override lives on a ScriptComponent behavior) and the scene-script/Level
        // tier (override lives on the scene's SceneScriptSettings). `effective` returns the current
        // value (override or harvested default); `setOverride`/`removeOverride` are undoable writes.
        // Ref-counted so the (move-only Function) hooks can be shared into multiple editor closures.
        struct ScriptPropertyAccess : public RefCounted
        {
            Function<foundation::script::ScriptPropertyValue()> effective;
            Function<void(const foundation::script::ScriptPropertyValue&)> setOverride;
            Function<void()> removeOverride;
        };

        void BuildScriptPropertyRow(StringView category,
                                    const foundation::script::ScriptPropertyDesc& property,
                                    const RefPtr<ScriptPropertyAccess>& access);

        // Entity-typed property: a picker over the CURRENT scene's entities (a menu of
        // names; the override stores the target's guid).
        void BuildScriptEntityPropertyRow(StringView category,
                                          const foundation::script::ScriptPropertyDesc& property,
                                          const RefPtr<ScriptPropertyAccess>& access);

        // Asset-typed property (asset:<TypeName>): an AssetPickerDialog over that
        // asset type; the override stores the picked guid.
        void BuildScriptAssetPropertyRow(StringView category,
                                         const foundation::script::ScriptPropertyDesc& property,
                                         const RefPtr<ScriptPropertyAccess>& access);

        // The Level tier's property rows: harvested metadata of the scene's bound Level class,
        // edited as overrides on the scene's SceneScriptSettings (committed as a settings blob).
        void BuildSceneScriptPropertyRows(const TypeInfo* settingsType, StringView category);

        [[nodiscard]] StringView AssetNameFor(const Guid& target);

        template <typename T>
        void BuildResourceRefRow(const Guid& id, const TypeInfo* type, const PropertyInfo& prop,
                                 StringView category,
                                 std::initializer_list<StringView> assetTypeNames,
                                 ComponentPropertyPath path = {})
        {
            SceneInspectorView* self = this;
            SceneEditContext* edit = m_edit;
            const char* propName = prop.name;
            const StringView name(reinterpret_cast<const utf8char*>(prop.name));
            auto editor = MakeRef<ResourceRefEditor>(
                MemoryAllocator(), name, AssetNameFor(RefTarget<T>(id, type, propName, path)),
                category, Span<const StringView>{assetTypeNames.begin(), assetTypeNames.size()});
            ResourceRefEditor* raw = editor.Get();
            // Pick, drop and clear are the one undoable component write.
            raw->BindAsset(
                *m_editor,
                [self, id, type, propName, path]()
                { return self->RefTarget<T>(id, type, propName, path); },
                [self, edit, id, type, propName, path](const Guid& target)
                {
                    if (self->m_editor->Project() != nullptr)
                    {
                        edit->SetComponentResourceRef<T>(id, type, path, propName, target,
                                                         self->m_editor->Resources());
                    }
                });
            AddEditor(raw, [raw]() { raw->Refresh(); });
        }

        // Entity-reference row: the entity-picker twin of BuildResourceRefRow. Same ResourceRefEditor
        // widget, but the pick menu lists the CURRENT scene's entities (names; "(none)" clears) and
        // the choice writes the component's EntityRef via SetComponentEntityRef. Defined in the impl.
        void BuildEntityRefRow(const Guid& id, const TypeInfo* type, const PropertyInfo& prop,
                               StringView category, ComponentPropertyPath path = {});

        // The "range" attribute's {min, max, step} payload, or null when absent/mistyped.
        [[nodiscard]] static const Float4* RangeOf(const PropertyInfo& prop);

        void AddEditor(ui::toolkit::PropertyEditor* editor, Function<void()> refresher);

        [[nodiscard]] static Float3 EulerDegrees(Quaternion q);

        void ShowAddComponentMenu();
        // Paste the clipboard component onto the selected entity; if it already has that component
        // type the paste OVERWRITES it, so confirm first (it is undoable either way).
        void PasteSelectedComponent();
        // Show/hide the Paste button based on whether the clipboard holds a component (per Refresh).
        void UpdatePasteButton();

        static constexpr i32 kEntityTab = 0;
        static constexpr i32 kSceneTab = 1;

        EditorContext* m_editor;  // borrowed (project + resources)
        SceneEditContext* m_edit; // borrowed (the page owns it)
        RefPtr<ui::TabView> m_tabView;
        RefPtr<ui::toolkit::PropertyGrid> m_grid; // the ACTIVE tab's grid (re-pointed each Rebuild)
        RefPtr<ui::toolkit::PropertyGrid> m_entityGrid;
        RefPtr<ui::toolkit::PropertyGrid> m_sceneGrid;
        RefPtr<ui::Label> m_emptyLabel; // "Select an entity to inspect." (Entity tab, no selection)
        RefPtr<ui::Button> m_addButton;
        RefPtr<ui::Button> m_pasteButton;
        Array<Function<void()>> m_refreshers;
        Guid m_lastSelectedForTab; // selection last seen (auto-switch to Entity tab on a new pick)
        u64 m_signature = ~0ull;
        bool m_forceRebuild = false; // set when a data-only mutation changed a section's SHAPE
    };

    // ResourceRefEditor's RTTI define moved with the class to editor.app (ResourceRefEditorImpl.cpp).
    RTTI_DEFINE_OBJECT(NoticeEditor, "rtti::editor::editor")
    RTTI_DEFINE_OBJECT(CollisionMatrixEditor, "rtti::editor::editor")
    // ContainerListEditor's RTTI define moved with the class to editor.app (ContainerListEditorImpl.cpp).
    RTTI_DEFINE_OBJECT(SceneInspectorView, "rtti::editor::editor")
}
