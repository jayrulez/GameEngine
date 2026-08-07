// Draconic::EditorScene - :inspector partition.
//
// SceneInspectorView: the reflection-driven property inspector INSIDE a scene page (per-page,
// like everything scene-scoped; §3.5). A toolkit PropertyGrid rebuilt from the primary
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
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"
#include <limits>
#include <initializer_list>

export module draconic.editor.scene:inspector;

import draconic.foundation;
import draconic.content;
import draconic.resource;
import draconic.geometry;
import draconic.animation;
import draconic.materials;
import draconic.texture.resource;
import draconic.particles.resource;
import draconic.scene;
import draconic.engine.render;
import draconic.physics;
import draconic.physics.resource;
import draconic.engine.physics;
import draconic.audio;
import draconic.audio.resource;
import draconic.engine.audio;
import draconic.ui.resource;
import draconic.script.resource;
import draconic.engine.script;
import draconic.ui;
import draconic.ui.toolkit;
import draconic.editor.core;
import draconic.editor.app;
import :edit;

using namespace draconic::foundation;
namespace foundation = draconic::foundation;

export namespace draconic::editor
{
    namespace ui = draconic::ui;
    namespace scene = draconic::scene;

    // A property row for resource::Ref fields: [name | button showing the current asset,
    // click = picker menu]. The value text refreshes from the ref's Guid each frame.
    class ResourceRefEditor final : public ui::toolkit::PropertyEditor
    {
        DRACONIC_OBJECT(ResourceRefEditor, ui::toolkit::PropertyEditor)
    public:
        Function<void()> OnPick; // opens the picker (wired by the inspector)

        ResourceRefEditor(StringView name, StringView valueText, StringView category)
            : ui::toolkit::PropertyEditor(name, category), m_valueText(valueText)
        {
        }

        void SetValueText(StringView text);

        void RefreshView() override {}

    protected:
        RefPtr<ui::View> CreateEditorView() override;

    private:
        String m_valueText;
        RefPtr<ui::Button> m_button;
    };

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
        DRACONIC_OBJECT(CollisionMatrixEditor, ui::toolkit::PropertyEditor)
    public:
        Array<String> names; // display names (index = group)
        Array<u32> matrix;   // parallel collide masks
        Function<void(usize, String)> OnRename;
        Function<void(usize, usize)> OnToggle; // (row group, column group)
        Function<void()> OnAddGroup;

        CollisionMatrixEditor(StringView name, StringView category)
            : ui::toolkit::PropertyEditor(name, category)
        {
        }

        void RefreshView() override {}

    protected:
        RefPtr<ui::View> CreateEditorView() override;
    };

    // ui::IconButton + ui::AssetPickerSlot are the shared UI controls (Draconic.UI/Controls); the
    // list editor below composes them.

    // The GENERIC reflected list editor: ONE grid row whose editor view is a header (add icon, top
    // right) over a column of slot rows (an AssetPickerSlot that fills + move-up / move-down / remove
    // icon buttons). Same callback shape as MaterialSlotsEditor but generic + icon-driven; the
    // inspector wires the callbacks to the reflection MutateComponent + the type-filtered asset picker.
    class ContainerListEditor final : public ui::toolkit::PropertyEditor
    {
        DRACONIC_OBJECT(ContainerListEditor, ui::toolkit::PropertyEditor)
    public:
        Function<void(usize)> OnPickSlot;
        Function<void(usize)> OnRemoveSlot;
        Function<void(usize, bool)> OnMoveSlot; // true = up
        Function<void()> OnAdd;
        Array<String> slotNames; // per-slot display text, set before the row builds

        ContainerListEditor(StringView name, StringView category)
            : ui::toolkit::PropertyEditor(name, category)
        {
        }

        void RefreshView() override {}

    protected:
        RefPtr<ui::View> CreateEditorView() override;
    };

    class SceneInspectorView : public ui::ViewGroup
    {
        DRACONIC_OBJECT(SceneInspectorView, ui::ViewGroup)
    public:
        SceneInspectorView(EditorContext& editor, SceneEditContext& edit)
            : m_editor(&editor), m_edit(&edit)
        {
            auto column = MakeRef<ui::FlexLayout>(DefaultAllocator());
            column->Direction = ui::Orientation::Vertical;
            column->Padding =
                ui::Thickness{8, 6}; // inset the content off the panel edge (like the hierarchy)

            m_grid = MakeRef<ui::toolkit::PropertyGrid>(DefaultAllocator());
            {
                auto grow = MakeRef<ui::FlexLayoutParams>(DefaultAllocator());
                grow->Grow = 1.0f;
                column->AddView(m_grid.Get(), grow);
            }

            m_addButton = MakeRef<ui::Button>(DefaultAllocator(), StringView(u8"Add Component"));
            {
                SceneInspectorView* self = this;
                m_addButton->OnClick.Add([self](ui::ButtonBase*) { self->ShowAddComponentMenu(); });
                auto lp = MakeRef<ui::FlexLayoutParams>(DefaultAllocator());
                lp->Width = ui::SizeSpec::Match();
                column->AddView(m_addButton.Get(), lp);
            }

            // Paste Component: below Add Component, shown only when the clipboard holds a component
            // (UpdatePasteButton, run each Refresh). Pasting over an existing same-type component
            // overwrites it, so that case asks for confirmation first.
            m_pasteButton = MakeRef<ui::Button>(DefaultAllocator(), StringView(u8"Paste Component"));
            {
                SceneInspectorView* self = this;
                m_pasteButton->OnClick.Add([self](ui::ButtonBase*) { self->PasteSelectedComponent(); });
                m_pasteButton->Visibility = ui::Visibility::Gone;
                auto lp = MakeRef<ui::FlexLayoutParams>(DefaultAllocator());
                lp->Width = ui::SizeSpec::Match();
                lp->Margin = ui::Thickness{0.0f, 6.0f, 0.0f, 0.0f}; // gap below Add Component
                column->AddView(m_pasteButton.Get(), lp);
            }

            AddView(column.Get());
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

        // A scene-setting property row: same editor kinds as components, but reading the
        // system's settings instance and writing through SetSceneSettingProperty commands
        // (merged scrubs, one undo entry). Covers the kinds settings blocks use today.
        void BuildSettingRow(const TypeInfo* type, const PropertyInfo& prop, StringView category);

        void BuildComponentSection(const Guid& id, scene::ComponentManagerBase& mgr);

        // Raw integral value of a bool/enum/int property via the address escape hatch.
        [[nodiscard]] static i64 RawIntValue(const Instance& obj, const PropertyInfo& p);

        // Applies the displayName/description/visibleWhen conventions to every row that
        // `prop`'s Build*Row call just added (rows firstRow..end). `instance` is a copyable
        // callable re-reading the owning object each frame so visibleWhen rows follow live
        // edits (a plain lambda, NOT foundation::Function - that one is move-only and each row's
        // refresher needs its own copy).
        template <typename GetInstance>
        void ApplyPropertyPresentation(const TypeInfo* type, const PropertyInfo& prop,
                                       usize firstRow, GetInstance instance)
        {
            const foundation::Attribute* displayName = FindAttribute(prop, u8"displayName");
            const foundation::Attribute* description = FindAttribute(prop, u8"description");
            const foundation::Attribute* visibleWhen = FindAttribute(prop, u8"visibleWhen");

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

            for (usize i = firstRow; i < m_grid->PropertyCount(); ++i)
            {
                ui::toolkit::PropertyEditor* editor = m_grid->PropertyAt(i);
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
                            MatchesPropertyCondition(condition, RawIntValue(obj, *dependent)));
                    };
                    refresh();
                    m_refreshers.PushBack(Function<void()>{Move(refresh)});
                }
            }
        }

        void BuildPropertyRow(const Guid& id, const TypeInfo* type, const PropertyInfo& prop,
                              StringView category);

        // Generic reflected CONTAINER property (a reflected list member): one grid row whose editor is
        // a ContainerListEditor - a header add + a slot row per element (an asset-picker slot + move /
        // remove icons). Slot text + the pick/add/remove/move callbacks are wired here to the reflection
        // container ops via MutateComponent (one undo step each); a content-diff refresher forces a
        // rebuild when the list changes (which Signature() does not track). Element pick is currently
        // specialized to Ref<Material>; struct-element leaf editing + the polymorphic add-by-type menu
        // are a follow-up.
        void BuildContainerRows(const Guid& id, const TypeInfo* type, const PropertyInfo& prop,
                                StringView category);
        // One undoable mutation of a component via a generic snapshot/restore/paste (any component type
        // - unlike the typed Mutate*Component helpers): copy the live value, mutate live, snapshot,
        // restore (non-undoable ReadComponent), PASTE (the paste command captures the pre-state).
        void MutateComponent(const Guid& id, const TypeInfo* type,
                             const Function<void(const Instance&)>& mutate);

        // Current target Guid of a Ref<T> property (nil when unset/unresolvable).
        template <typename T>
        [[nodiscard]] Guid RefTarget(const Guid& id, const TypeInfo* type, const char* propName)
        {
            scene::ComponentManagerBase* mgr = m_edit->FindManager(type);
            const scene::EntityHandle e = m_edit->Resolve(id);
            if (mgr == nullptr || !e.IsAssigned())
            {
                return Guid{};
            }
            const Instance component = mgr->GetComponentInstance(e);
            const PropertyInfo* p = component.IsEmpty() ? nullptr : FindProperty(*type, propName);
            void* address =
                (p != nullptr && p->address != nullptr) ? p->address(component) : nullptr;
            return (address != nullptr) ? static_cast<draconic::resource::Ref<T>*>(address)->id
                                        : Guid{};
        }

        // One undoable mutation of the selected entity's MeshComponent materials: copy the
        // live value, mutate live, snapshot to a clipboard blob, restore, PASTE (the paste
        // command captures the pre-state, so every slot action is one undo step).
        void MutateMeshMaterials(const Guid& id,
                                 const Function<void(draconic::render::MeshComponent&)>& mutate);

        /// The display names the slots editor renders - recomputed by its refresher to
        /// detect shape/content changes (the inspector Signature only sees selection +
        /// component PRESENCE, so a data-only slot mutation or its undo changes nothing it
        /// watches).
        [[nodiscard]] Array<String> MaterialSlotNames(const draconic::render::MeshComponent& mc);

        void BuildMaterialSlots(const Guid& id, StringView category);

        // One undoable mutation of the selected entity's ScriptComponent (the mesh-
        // materials pattern: mutate live, snapshot to a clipboard blob, restore, PASTE
        // - the paste command captures the pre-state, so every action is one undo step).
        void
        MutateScriptComponent(const Guid& id,
                              const Function<void(draconic::script::ScriptComponent&)>& mutate);

        // The cooked ScriptClass a behavior references (bound through the editor's
        // resource manager so the harvested metadata is available; null when unset or
        // not yet cooked).
        [[nodiscard]] draconic::script::ScriptClass*
        BehaviorClass(const draconic::script::ScriptBehavior& behavior);

        // Signature of the behavior list's SHAPE (count + script ids + enabled flags +
        // override counts) - the refresher forces a rebuild when it changes, since the
        // inspector's own Signature only watches selection + component presence.
        [[nodiscard]] u64 ScriptBehaviorsSignature(const draconic::script::ScriptComponent& c);

        void BuildScriptBehaviors(const Guid& id, StringView category);

        void BuildScriptBehaviorRows(const Guid& id, StringView category, usize index);

        void BuildScriptPropertyRow(const Guid& id, StringView category, usize index,
                                    const draconic::script::ScriptPropertyDesc& property);

        // Entity-typed property: a picker over the CURRENT scene's entities (a menu of
        // names; the override stores the target's guid).
        void BuildScriptEntityPropertyRow(const Guid& id, StringView category, usize index,
                                          const draconic::script::ScriptPropertyDesc& property);

        // Asset-typed property (asset:<TypeName>): an AssetPickerDialog over that
        // asset type; the override stores the picked guid.
        void BuildScriptAssetPropertyRow(const Guid& id, StringView category, usize index,
                                         const draconic::script::ScriptPropertyDesc& property);

        [[nodiscard]] StringView AssetNameFor(const Guid& target);

        // The settings twin of RefTarget (the Ref lives on a scene system's settings block).
        template <typename T>
        [[nodiscard]] Guid SettingRefTarget(const TypeInfo* type, const char* propName)
        {
            scene::SceneSystem* system = m_edit->FindSystemBySettingsType(type);
            if (system == nullptr)
            {
                return Guid{};
            }
            const Instance settings{system->SettingsInstance(), type};
            const PropertyInfo* p = FindProperty(*type, propName);
            void* address =
                (p != nullptr && p->address != nullptr) ? p->address(settings) : nullptr;
            return (address != nullptr) ? static_cast<draconic::resource::Ref<T>*>(address)->id
                                        : Guid{};
        }

        // The settings twin of BuildResourceRefRow.
        template <typename T>
        void BuildSettingResourceRefRow(const TypeInfo* type, const PropertyInfo& prop,
                                        StringView category,
                                        std::initializer_list<StringView> assetTypeNames)
        {
            SceneInspectorView* self = this;
            SceneEditContext* edit = m_edit;
            const char* propName = prop.name;
            const StringView name(reinterpret_cast<const utf8char*>(prop.name));

            auto editor = MakeRef<ResourceRefEditor>(
                DefaultAllocator(), name, AssetNameFor(SettingRefTarget<T>(type, propName)),
                category);
            ResourceRefEditor* raw = editor.Get();
            Array<String> assetTypes;
            for (StringView typeName : assetTypeNames)
            {
                assetTypes.PushBack(String(typeName));
            }
            raw->OnPick = [self, edit, type, propName, assetTypes]()
            {
                if (self->Context == nullptr || self->m_editor->Project() == nullptr)
                {
                    return;
                }
                draconic::resource::ResourceManager* resources = self->m_editor->Resources();
                Array<String> typeNames = assetTypes;
                auto dialog = MakeRef<draconic::editor::app::AssetPickerDialog>(
                    DefaultAllocator(), *self->m_editor, Move(typeNames));
                dialog->OnPicked = [edit, type, propName, resources](const Guid& target)
                { edit->SetSceneSettingResourceRef<T>(type, propName, target, resources); };
                dialog->Show(self->Context);
            };
            AddEditor(raw,
                      [self, type, propName, raw]()
                      {
                          raw->SetValueText(
                              self->AssetNameFor(self->SettingRefTarget<T>(type, propName)));
                      });
        }

        template <typename T>
        void BuildResourceRefRow(const Guid& id, const TypeInfo* type, const PropertyInfo& prop,
                                 StringView category,
                                 std::initializer_list<StringView> assetTypeNames)
        {
            SceneInspectorView* self = this;
            SceneEditContext* edit = m_edit;
            const char* propName = prop.name;
            const StringView name(reinterpret_cast<const utf8char*>(prop.name));

            auto editor = MakeRef<ResourceRefEditor>(
                DefaultAllocator(), name, AssetNameFor(RefTarget<T>(id, type, propName)), category);
            ResourceRefEditor* raw = editor.Get();
            Array<String> assetTypes;
            for (StringView typeName : assetTypeNames)
            {
                assetTypes.PushBack(String(typeName));
            }
            raw->OnPick = [self, edit, id, type, propName, assetTypes]()
            {
                if (self->Context == nullptr || self->m_editor->Project() == nullptr)
                {
                    return;
                }
                draconic::resource::ResourceManager* resources = self->m_editor->Resources();

                // The browser-mirroring picker (readonly; favorites pinned first; [Clear] = none).
                Array<String> typeNames = assetTypes;
                auto dialog = MakeRef<draconic::editor::app::AssetPickerDialog>(
                    DefaultAllocator(), *self->m_editor, Move(typeNames));
                dialog->OnPicked = [edit, id, type, propName, resources](const Guid& target)
                { edit->SetComponentResourceRef<T>(id, type, propName, target, resources); };
                dialog->Show(self->Context);
            };
            AddEditor(
                raw, [self, id, type, propName, raw]()
                { raw->SetValueText(self->AssetNameFor(self->RefTarget<T>(id, type, propName))); });
        }

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

        EditorContext* m_editor;  // borrowed (project + resources)
        SceneEditContext* m_edit; // borrowed (the page owns it)
        RefPtr<ui::toolkit::PropertyGrid> m_grid;
        RefPtr<ui::Button> m_addButton;
        RefPtr<ui::Button> m_pasteButton;
        Array<Function<void()>> m_refreshers;
        u64 m_signature = ~0ull;
        bool m_forceRebuild = false; // set when a data-only mutation changed a section's SHAPE
    };

    DRACONIC_DEFINE_OBJECT(ResourceRefEditor, "draconic::editor")
    DRACONIC_DEFINE_OBJECT(CollisionMatrixEditor, "draconic::editor")
    DRACONIC_DEFINE_OBJECT(ContainerListEditor, "draconic::editor")
    DRACONIC_DEFINE_OBJECT(SceneInspectorView, "draconic::editor")
}
