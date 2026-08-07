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

module draconic.editor.scene;

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

namespace draconic::editor
{
    RefPtr<ui::View> CollisionMatrixEditor::CreateEditorView()
    {
        auto column = MakeRef<ui::FlexLayout>(DefaultAllocator());
        column->Direction = ui::Orientation::Vertical;
        column->Spacing = 2.0f;

        CollisionMatrixEditor* self = this;
        const usize count = names.Size();
        for (usize i = 0; i < count; ++i)
        {
            auto row = MakeRef<ui::FlexLayout>(DefaultAllocator());
            row->Direction = ui::Orientation::Horizontal;
            row->Spacing = 2.0f;

            auto name = MakeRef<ui::EditText>(DefaultAllocator());
            name->SetText(names[i].AsView());
            ui::EditText* nameRaw = name.Get();
            name->OnSubmit.Add(
                [self, i, nameRaw](ui::EditText*)
                {
                    if (self->OnRename)
                    {
                        self->OnRename(i, String(nameRaw->Text()));
                    }
                });
            {
                auto lp = MakeRef<ui::FlexLayoutParams>(DefaultAllocator());
                lp->Grow = 1.0f;
                row->AddView(name.Get(), lp);
            }

            for (usize j = 0; j < count; ++j)
            {
                const bool collides = i < matrix.Size() && (matrix[i] & (1u << j)) != 0;
                auto cell = MakeRef<ui::Button>(DefaultAllocator(),
                                                collides ? StringView(u8"+") : StringView(u8"-"));
                cell->FontSize.SetValue(Optional<f32>{12.0f});
                String tip(u8"vs ");
                tip.Append(names[j].AsView());
                cell->TooltipText = Move(tip);
                cell->OnClick.Add(
                    [self, i, j](ui::ButtonBase*)
                    {
                        if (self->OnToggle)
                        {
                            self->OnToggle(i, j);
                        }
                    });
                auto lp = MakeRef<ui::FlexLayoutParams>(DefaultAllocator());
                lp->Width = ui::SizeSpec::Fixed(ui::Unit::Px(22.0f));
                row->AddView(cell.Get(), lp);
            }

            auto lp = MakeRef<ui::FlexLayoutParams>(DefaultAllocator());
            lp->Width = ui::SizeSpec::Match();
            lp->Height = ui::SizeSpec::Fixed(ui::Unit::Px(22.0f));
            column->AddView(row.Get(), lp);
        }

        if (count < draconic::physics::kCollisionGroupCount)
        {
            auto add = MakeRef<ui::Button>(DefaultAllocator(), StringView(u8"+ Add Group"));
            add->FontSize.SetValue(Optional<f32>{12.0f});
            add->OnClick.Add(
                [self](ui::ButtonBase*)
                {
                    if (self->OnAddGroup)
                    {
                        self->OnAddGroup();
                    }
                });
            auto lp = MakeRef<ui::FlexLayoutParams>(DefaultAllocator());
            lp->Width = ui::SizeSpec::Match();
            lp->Height = ui::SizeSpec::Fixed(ui::Unit::Px(22.0f));
            column->AddView(add.Get(), lp);
        }
        return column;
    }

    RefPtr<ui::View> ContainerListEditor::CreateEditorView()
    {
        auto column = MakeRef<ui::FlexLayout>(DefaultAllocator());
        column->Direction = ui::Orientation::Vertical;
        column->Spacing = 2.0f;

        ContainerListEditor* self = this;
        draconic::editor::app::EditorIcons& icons = draconic::editor::app::EditorIcons::Get();

        // Header: a spacer that grows + the add icon button pinned to the right.
        {
            auto header = MakeRef<ui::FlexLayout>(DefaultAllocator());
            header->Direction = ui::Orientation::Horizontal;
            auto spacer = MakeRef<ui::FlexLayout>(DefaultAllocator());
            {
                auto lp = MakeRef<ui::FlexLayoutParams>(DefaultAllocator());
                lp->Grow = 1.0f;
                header->AddView(spacer.Get(), lp);
            }
            auto add = MakeRef<ui::IconButton>(DefaultAllocator(), icons.add.Get());
            add->OnClick.Add([self](ui::ButtonBase*)
                             {
                                 if (self->OnAdd)
                                 {
                                     self->OnAdd();
                                 }
                             });
            header->AddView(add.Get());
            auto lp = MakeRef<ui::FlexLayoutParams>(DefaultAllocator());
            lp->Width = ui::SizeSpec::Match();
            lp->Height = ui::SizeSpec::Fixed(ui::Unit::Px(22.0f));
            column->AddView(header.Get(), lp);
        }

        // Slot rows: a picker slot that fills + move-up / move-down / remove icon buttons.
        for (usize i = 0; i < slotNames.Size(); ++i)
        {
            auto row = MakeRef<ui::FlexLayout>(DefaultAllocator());
            row->Direction = ui::Orientation::Horizontal;
            row->Spacing = 4.0f;

            auto slot = MakeRef<draconic::editor::app::AssetPickerSlot>(DefaultAllocator(), slotNames[i].AsView());
            slot->FontSize.SetValue(Optional<f32>{12.0f});
            slot->OnClick.Add([self, i](ui::ButtonBase*)
                              {
                                  if (self->OnPickSlot)
                                  {
                                      self->OnPickSlot(i);
                                  }
                              });
            {
                auto lp = MakeRef<ui::FlexLayoutParams>(DefaultAllocator());
                lp->Grow = 1.0f;
                row->AddView(slot.Get(), lp);
            }
            auto up = MakeRef<ui::IconButton>(DefaultAllocator(), icons.moveUp.Get());
            up->IsEnabled = i > 0;
            up->OnClick.Add([self, i](ui::ButtonBase*)
                            {
                                if (self->OnMoveSlot)
                                {
                                    self->OnMoveSlot(i, true);
                                }
                            });
            row->AddView(up.Get());
            auto down = MakeRef<ui::IconButton>(DefaultAllocator(), icons.moveDown.Get());
            down->IsEnabled = i + 1 < slotNames.Size();
            down->OnClick.Add([self, i](ui::ButtonBase*)
                              {
                                  if (self->OnMoveSlot)
                                  {
                                      self->OnMoveSlot(i, false);
                                  }
                              });
            row->AddView(down.Get());
            auto remove = MakeRef<ui::IconButton>(DefaultAllocator(), icons.remove.Get());
            remove->OnClick.Add([self, i](ui::ButtonBase*)
                                {
                                    if (self->OnRemoveSlot)
                                    {
                                        self->OnRemoveSlot(i);
                                    }
                                });
            row->AddView(remove.Get());

            auto lp = MakeRef<ui::FlexLayoutParams>(DefaultAllocator());
            lp->Width = ui::SizeSpec::Match();
            lp->Height = ui::SizeSpec::Fixed(ui::Unit::Px(22.0f));
            column->AddView(row.Get(), lp);
        }
        return column;
    }
    void ResourceRefEditor::SetValueText(StringView text)
    {
        if (m_valueText.AsView() == text)
        {
            return;
        }
        m_valueText = String(text);
        if (m_button.Get() != nullptr)
        {
            m_button->SetText(m_valueText.AsView());
        }
    }

    RefPtr<ui::View> ResourceRefEditor::CreateEditorView()
    {
        m_button = MakeRef<ui::Button>(DefaultAllocator(), m_valueText.AsView());
        ResourceRefEditor* self = this;
        m_button->OnClick.Add(
            [self](ui::ButtonBase*)
            {
                if (self->OnPick)
                {
                    self->OnPick();
                }
            });
        return RefPtr<ui::View>(m_button.Get());
    }
    void SceneInspectorView::Refresh()
    {
        UpdatePasteButton(); // clipboard can change any frame; keep the Paste button in sync
        const u64 signature = Signature();
        if (m_forceRebuild || signature != m_signature)
        {
            m_forceRebuild = false;
            m_signature = signature;
            Rebuild();
        }
        else
        {
            for (const Function<void()>& refresher : m_refreshers)
            {
                refresher();
            }
        }
    }

    void SceneInspectorView::OnMeasure(ui::BoxConstraints constraints)
    {
        for (usize i = 0; i < ChildCount(); ++i)
        {
            GetChildAt(i)->Measure(constraints);
        }
        MeasuredSize = Float2{constraints.MaxWidth, constraints.MaxHeight};
    }

    void SceneInspectorView::OnLayout(f32, f32, f32 width, f32 height)
    {
        for (usize i = 0; i < ChildCount(); ++i)
        {
            GetChildAt(i)->Layout(0, 0, width, height);
        }
    }

    bool SceneInspectorView::IsRegisteredType(const TypeInfo* type)
    {
        if (type == nullptr || type->name == nullptr)
        {
            return false;
        }
        const char* n = type->name;
        return !(n[0] == '<');
    }

    Guid SceneInspectorView::SelectedEntity() const
    {
        const Guid* primary = m_edit->EntitySelection().Primary();
        return (primary != nullptr) ? *primary : Guid{};
    }

    u64 SceneInspectorView::Signature()
    {
        const Guid id = SelectedEntity();
        u64 signature = id.high ^ (id.low * 0x9E3779B97F4A7C15ull) ^ m_edit->Scene().Revision();
        const scene::EntityHandle e = m_edit->Resolve(id);
        if (e.IsAssigned())
        {
            u64 bit = 1;
            m_edit->Scene().ForEachManager(
                [&](scene::ComponentManagerBase& mgr)
                {
                    if (mgr.HasComponent(e))
                    {
                        signature ^= bit * 0xBF58476D1CE4E5B9ull;
                    }
                    bit <<= 1;
                });
        }
        return signature;
    }

    void SceneInspectorView::Rebuild()
    {
        m_grid->Clear();
        m_refreshers.Clear();

        const Guid id = SelectedEntity();
        const scene::EntityHandle e = m_edit->Resolve(id);
        m_addButton->Visibility =
            e.IsAssigned() ? ui::VisibilityValue::Visible : ui::VisibilityValue::Gone;
        // No entity selected: the SCENE's settings (Sedulous scene-modules pattern) -
        // every scene system exposing a reflected settings block gets a category.
        if (!e.IsAssigned())
        {
            BuildSceneSettingsSections();
            Invalidate();
            return;
        }

        BuildEntitySection(id);
        BuildTransformSection(id);

        m_edit->Scene().ForEachManager(
            [&](scene::ComponentManagerBase& mgr)
            {
                const scene::EntityHandle live = m_edit->Resolve(id);
                if (live.IsAssigned() && mgr.HasComponent(live))
                {
                    BuildComponentSection(id, mgr);
                }
            });
        Invalidate();
    }

    void SceneInspectorView::BuildEntitySection(const Guid& id)
    {
        SceneEditContext* edit = m_edit;

        auto name = MakeRef<ui::toolkit::StringEditor>(
            DefaultAllocator(), StringView(u8"Name"),
            m_edit->Scene().GetEntityName(m_edit->Resolve(id)),
            Function<void(StringView)>{[edit, id](StringView v) { edit->RenameEntity(id, v); }},
            StringView(u8"Entity"));
        AddEditor(name.Get(), [edit, id, raw = name.Get()]()
                  { raw->SetValue(edit->Scene().GetEntityName(edit->Resolve(id))); });

        auto active = MakeRef<ui::toolkit::BoolEditor>(
            DefaultAllocator(), StringView(u8"Active"),
            m_edit->Scene().IsActive(m_edit->Resolve(id)),
            Function<void(bool)>{[edit, id](bool v) { edit->SetEntityActive(id, v); }},
            StringView(u8"Entity"));
        AddEditor(active.Get(), [edit, id, raw = active.Get()]()
                  { raw->SetValue(edit->Scene().IsActive(edit->Resolve(id))); });
    }

    void SceneInspectorView::BuildTransformSection(const Guid& id)
    {
        SceneEditContext* edit = m_edit;
        const StringView category = u8"Transform";
        const foundation::Transform t = m_edit->Scene().GetLocalTransform(m_edit->Resolve(id));

        auto position = MakeRef<ui::toolkit::Float3Editor>(
            DefaultAllocator(), StringView(u8"Position"), t.position, -100000.0f, 100000.0f, 0.1f,
            Function<void(Float3)>{[edit, id](Float3 v)
                                   {
                                       foundation::Transform current =
                                           edit->Scene().GetLocalTransform(edit->Resolve(id));
                                       current.position = v;
                                       edit->SetLocalTransform(id, current);
                                   }},
            category);
        AddEditor(position.Get(), [edit, id, raw = position.Get()]()
                  { raw->SetValue(edit->Scene().GetLocalTransform(edit->Resolve(id)).position); });

        // Rotation displayed as euler DEGREES (x = pitch, y = yaw, z = roll).
        auto rotation = MakeRef<ui::toolkit::Float3Editor>(
            DefaultAllocator(), StringView(u8"Rotation"), EulerDegrees(t.rotation), -360.0f, 360.0f,
            1.0f,
            Function<void(Float3)>{[edit, id](Float3 v)
                                   {
                                       foundation::Transform current =
                                           edit->Scene().GetLocalTransform(edit->Resolve(id));
                                       current.rotation = FromYawPitchRoll(DegreesToRadians(v.y),
                                                                           DegreesToRadians(v.x),
                                                                           DegreesToRadians(v.z));
                                       edit->SetLocalTransform(id, current);
                                   }},
            category);
        AddEditor(rotation.Get(),
                  [edit, id, raw = rotation.Get()]()
                  {
                      raw->SetValue(EulerDegrees(
                          edit->Scene().GetLocalTransform(edit->Resolve(id)).rotation));
                  });

        auto scale = MakeRef<ui::toolkit::Float3Editor>(
            DefaultAllocator(), StringView(u8"Scale"), t.scale, -100000.0f, 100000.0f, 0.1f,
            Function<void(Float3)>{[edit, id](Float3 v)
                                   {
                                       foundation::Transform current =
                                           edit->Scene().GetLocalTransform(edit->Resolve(id));
                                       current.scale = v;
                                       edit->SetLocalTransform(id, current);
                                   }},
            category);
        AddEditor(scale.Get(), [edit, id, raw = scale.Get()]()
                  { raw->SetValue(edit->Scene().GetLocalTransform(edit->Resolve(id)).scale); });
    }

    void SceneInspectorView::BuildSceneSettingsSections()
    {
        m_edit->Scene().ForEachSystem(
            [&](scene::SceneSystem& system)
            {
                const TypeInfo* type = system.SettingsType();
                if (type == nullptr || !IsRegisteredType(type))
                {
                    return;
                }
                // Category = the settings type minus a trailing "Settings"
                // ("EnvironmentSettings" -> "Environment").
                StringView category(reinterpret_cast<const utf8char*>(type->name));
                const StringView suffix = u8"Settings";
                if (category.Size() > suffix.Size() &&
                    category.SubStr(category.Size() - suffix.Size(), suffix.Size()) == suffix)
                {
                    category = category.SubStr(0, category.Size() - suffix.Size());
                }
                for (const PropertyInfo& prop : Properties(*type))
                {
                    if (IsNested(prop))
                    {
                        continue; // nested structures are recursed elsewhere, not a leaf row
                    }
                    const usize firstRow = m_grid->PropertyCount();
                    BuildSettingRow(type, prop, category);
                    ApplyPropertyPresentation(
                        type, prop, firstRow,
                        [edit = m_edit, type]() -> Instance
                        {
                            scene::SceneSystem* system = edit->FindSystemBySettingsType(type);
                            return (system != nullptr) ? Instance{system->SettingsInstance(), type}
                                                       : Instance{};
                        });
                }
                if (type == &TypeOf<draconic::physics::PhysicsSceneSettings>())
                {
                    BuildCollisionMatrixRow(type, category);
                }
            });
    }

    void SceneInspectorView::BuildCollisionMatrixRow(const TypeInfo* type, StringView category)
    {
        using draconic::physics::PhysicsSceneSettings;
        SceneEditContext* edit = m_edit;
        scene::SceneSystem* system = edit->FindSystemBySettingsType(type);
        if (system == nullptr)
        {
            return;
        }
        auto* live = static_cast<PhysicsSceneSettings*>(system->SettingsInstance());

        auto matrix = MakeRef<CollisionMatrixEditor>(DefaultAllocator(),
                                                     StringView(u8"Collision Groups"), category);
        // Display copy: at least one row ("Default"); rows without a stored mask
        // read as collide-with-everything.
        matrix->names = live->groupNames;
        if (matrix->names.IsEmpty())
        {
            matrix->names.PushBack(String(u8"Default"));
        }
        matrix->matrix = live->groupCollides;
        while (matrix->matrix.Size() < matrix->names.Size())
        {
            matrix->matrix.PushBack(0xFFFFFFFFu);
        }

        auto commit = [edit, type](PhysicsSceneSettings copy)
        {
            MemoryStream buffer;
            BinarySerializer writer(buffer, SerializeMode::Write);
            draconic::physics::SerializePhysicsSceneSettings(writer, copy);
            Array<byte> blob;
            const Span<const byte> bytes = buffer.Bytes();
            blob.Reserve(bytes.Size());
            for (byte b : bytes)
            {
                blob.PushBack(b);
            }
            (void)edit->ApplySceneSettingsBlock(type, Move(blob));
        };
        auto editedCopy = [live, raw = matrix.Get()]()
        {
            PhysicsSceneSettings copy = *live;
            copy.groupNames = raw->names;
            copy.groupCollides = raw->matrix;
            return copy;
        };

        matrix->OnRename = [commit, editedCopy, raw = matrix.Get()](usize i, String name)
        {
            if (i >= raw->names.Size())
            {
                return;
            }
            raw->names[i] = Move(name);
            commit(editedCopy());
        };
        matrix->OnToggle = [commit, editedCopy, raw = matrix.Get()](usize i, usize j)
        {
            if (i >= raw->matrix.Size() || j >= raw->matrix.Size())
            {
                return;
            }
            const bool collides = (raw->matrix[i] & (1u << j)) != 0;
            if (collides)
            {
                raw->matrix[i] &= ~(1u << j);
                raw->matrix[j] &= ~(1u << i); // symmetric
            }
            else
            {
                raw->matrix[i] |= (1u << j);
                raw->matrix[j] |= (1u << i);
            }
            commit(editedCopy());
        };
        matrix->OnAddGroup = [commit, editedCopy, raw = matrix.Get()]()
        {
            String name(u8"Group ");
            const usize index = raw->names.Size();
            if (index >= 10)
            {
                name.PushBack(static_cast<utf8char>('0' + index / 10 % 10));
            }
            name.PushBack(static_cast<utf8char>('0' + index % 10));
            raw->names.PushBack(Move(name));
            raw->matrix.PushBack(0xFFFFFFFFu);
            commit(editedCopy());
        };
        AddEditor(matrix.Get(), []() {});
    }

    void SceneInspectorView::BuildSettingRow(const TypeInfo* type, const PropertyInfo& prop,
                                             StringView category)
    {
        SceneEditContext* edit = m_edit;
        const StringView name(reinterpret_cast<const utf8char*>(prop.name));
        const bool readOnly =
            (static_cast<u32>(prop.flags) & static_cast<u32>(PropertyFlags::ReadOnly)) != 0;
        const char* propName = prop.name;

        auto getInstance = [edit, type]() -> Instance
        {
            scene::SceneSystem* system = edit->FindSystemBySettingsType(type);
            return (system != nullptr) ? Instance{system->SettingsInstance(), type} : Instance{};
        };

        // Resource references (the environment's sky texture): the browser-mirroring picker,
        // writing through the settings-flavored ref command.
        if (prop.type == &TypeOf<draconic::resource::Ref<draconic::texture::Texture>>())
        {
            BuildSettingResourceRefRow<draconic::texture::Texture>(type, prop, category,
                                                                   {u8"TextureAsset"});
            return;
        }
        // The scene's Level-script reference (SceneScriptSettings::script): the same
        // browser-mirroring picker, filtered to script class assets.
        if (prop.type == &TypeOf<draconic::resource::Ref<draconic::script::ScriptClass>>())
        {
            BuildSettingResourceRefRow<draconic::script::ScriptClass>(type, prop, category,
                                                                      {u8"ScriptClassAsset"});
            return;
        }
        auto getVariant = [getInstance, type, propName]() -> Variant
        {
            const Instance settings = getInstance();
            const PropertyInfo* p = settings.IsEmpty() ? nullptr : FindProperty(*type, propName);
            return (p != nullptr) ? GetProperty(*p, settings) : Variant{};
        };

        if (IsEnum(*prop.type))
        {
            const Span<const EnumValue> values = Enumerators(*prop.type);
            Array<StringView> items;
            for (const EnumValue& v : values)
            {
                items.PushBack(StringView(reinterpret_cast<const utf8char*>(v.name)));
            }

            auto rawRead = [getInstance, type, propName]() -> i64
            {
                const Instance settings = getInstance();
                const PropertyInfo* p =
                    settings.IsEmpty() ? nullptr : FindProperty(*type, propName);
                void* address =
                    (p != nullptr && p->address != nullptr) ? p->address(settings) : nullptr;
                if (address == nullptr)
                {
                    return 0;
                }
                switch (p->type->size)
                {
                case 1:
                    return *static_cast<const i8*>(address);
                case 2:
                    return *static_cast<const i16*>(address);
                case 8:
                    return *static_cast<const i64*>(address);
                default:
                    return *static_cast<const i32*>(address);
                }
            };
            auto indexOf = [values](i64 value) -> i32
            {
                for (usize i = 0; i < values.Size(); ++i)
                {
                    if (values[i].value == value)
                    {
                        return static_cast<i32>(i);
                    }
                }
                return 0;
            };
            auto editor = MakeRef<ui::toolkit::EnumEditor>(
                DefaultAllocator(), name, indexOf(rawRead()),
                Span<const StringView>{items.Data(), items.Size()},
                readOnly ? Function<void(i32)>{}
                         : Function<void(i32)>{[edit, type, propName, values](i32 index)
                                               {
                                                   if (index >= 0 &&
                                                       index < static_cast<i32>(values.Size()))
                                                   {
                                                       edit->SetSceneSettingPropertyRaw(
                                                           type, propName,
                                                           values[static_cast<usize>(index)].value);
                                                   }
                                               }},
                category);
            AddEditor(editor.Get(), [rawRead, indexOf, raw = editor.Get()]()
                      { raw->SetValue(indexOf(rawRead())); });
            return;
        }

        if (prop.type == &TypeOf<f32>())
        {
            auto value = [getVariant]() -> f64
            {
                const Variant v = getVariant();
                const f32* f = v.TryGet<f32>();
                return (f != nullptr) ? static_cast<f64>(*f) : 0.0;
            };
            // "range" attribute -> bounded slider+field instead of a bare numeric field.
            if (const Float4* range = RangeOf(prop))
            {
                auto editor = MakeRef<ui::toolkit::RangeEditor>(
                    DefaultAllocator(), name, static_cast<f32>(value()), range->x, range->y,
                    range->z,
                    readOnly ? Function<void(f32)>{}
                             : Function<void(f32)>{[edit, type, propName](f32 v)
                                                   {
                                                       edit->SetSceneSettingProperty(
                                                           type, propName, Variant::From<f32>(v));
                                                   }},
                    category);
                AddEditor(editor.Get(), [value, raw = editor.Get()]()
                          { raw->SetValue(static_cast<f32>(value())); });
                return;
            }
            auto editor = MakeRef<ui::toolkit::FloatEditor>(
                DefaultAllocator(), name, value(), -1e9, 1e9, 0.1, 2,
                readOnly ? Function<void(f64)>{}
                         : Function<void(f64)>{[edit, type, propName](f64 v)
                                               {
                                                   edit->SetSceneSettingProperty(
                                                       type, propName,
                                                       Variant::From<f32>(static_cast<f32>(v)));
                                               }},
                category);
            AddEditor(editor.Get(), [value, raw = editor.Get()]() { raw->SetValue(value()); });
            return;
        }

        if (prop.type == &TypeOf<Color>())
        {
            auto value = [getVariant]() -> Color
            {
                const Variant v = getVariant();
                const Color* c = v.TryGet<Color>();
                return (c != nullptr) ? *c : Color{1, 1, 1, 1};
            };
            auto editor = MakeRef<ui::toolkit::ColorEditor>(
                DefaultAllocator(), name, value(),
                readOnly ? Function<void(Color)>{}
                         : Function<void(Color)>{[edit, type, propName](Color v)
                                                 {
                                                     edit->SetSceneSettingProperty(
                                                         type, propName, Variant::From<Color>(v));
                                                 }},
                category);
            AddEditor(editor.Get(), [value, raw = editor.Get()]() { raw->SetValue(value()); });
            return;
        }

        if (prop.type == &TypeOf<bool>())
        {
            auto value = [getVariant]() -> bool
            {
                const Variant v = getVariant();
                const bool* b = v.TryGet<bool>();
                return (b != nullptr) && *b;
            };
            auto editor = MakeRef<ui::toolkit::BoolEditor>(
                DefaultAllocator(), name, value(),
                readOnly ? Function<void(bool)>{}
                         : Function<void(bool)>{[edit, type, propName](bool v)
                                                {
                                                    edit->SetSceneSettingProperty(
                                                        type, propName, Variant::From<bool>(v));
                                                }},
                category);
            AddEditor(editor.Get(), [value, raw = editor.Get()]() { raw->SetValue(value()); });
            return;
        }

        if (prop.type == &TypeOf<Float3>())
        {
            auto value = [getVariant]() -> Float3
            {
                const Variant v = getVariant();
                const Float3* f = v.TryGet<Float3>();
                return (f != nullptr) ? *f : Float3{};
            };
            auto editor = MakeRef<ui::toolkit::Float3Editor>(
                DefaultAllocator(), name, value(), -100000.0f, 100000.0f, 0.1f,
                readOnly ? Function<void(Float3)>{}
                         : Function<void(Float3)>{[edit, type, propName](Float3 v)
                                                  {
                                                      edit->SetSceneSettingProperty(
                                                          type, propName, Variant::From<Float3>(v));
                                                  }},
                category);
            AddEditor(editor.Get(), [value, raw = editor.Get()]() { raw->SetValue(value()); });
            return;
        }
        // Other kinds: extend when a settings block needs them.
    }

    namespace
    {
        // User-facing component name: the authored "displayName" type attribute wins;
        // unannotated types fall back to spaced PascalCase minus a trailing "Component"
        // ("ReflectionProbeComponent" -> "Reflection Probe"), so nothing renders raw.
        [[nodiscard]] String ComponentDisplayName(const TypeInfo* type)
        {
            if (const Variant* v = FindAttribute(*type, "displayName"))
            {
                if (const String* s = v->TryGet<String>())
                {
                    return String(s->AsView());
                }
            }
            StringView n(reinterpret_cast<const utf8char*>(type->name));
            const StringView suffix = u8"Component";
            if (n.Size() > suffix.Size() &&
                n.SubStr(n.Size() - suffix.Size(), suffix.Size()) == suffix)
            {
                n = n.SubStr(0, n.Size() - suffix.Size());
            }
            return PrettifyPropertyName(n);
        }

        // Add-menu grouping: the authored "category" type attribute; unannotated types
        // gather under "Other" (the cue that a category is missing, not a design).
        [[nodiscard]] StringView ComponentCategory(const TypeInfo* type)
        {
            if (const Variant* v = FindAttribute(*type, "category"))
            {
                if (const String* s = v->TryGet<String>())
                {
                    return s->AsView();
                }
            }
            return u8"Other";
        }

        // A stable label for a container element: its dynamic type's displayName attribute, else the
        // prettified type name.
        [[nodiscard]] String ContainerElementLabel(const TypeInfo* elementType)
        {
            if (elementType == nullptr)
            {
                return String(u8"(element)");
            }
            const StringView disp =
                TypeAttrString(*elementType, "displayName", StringView{});
            if (!disp.IsEmpty())
            {
                return String(disp);
            }
            return PrettifyPropertyName(StringView(reinterpret_cast<const utf8char*>(elementType->name)));
        }

    }

    // Material-slots UI switch: true = the generic reflection-driven list editor (materials reflected
    // as a container); false = the bespoke mesh-aware editor (BuildMaterialSlots). Both render through
    // the same ContainerListEditor UI. Materials stays reflected either way.
    inline constexpr bool kUseReflectedMaterialSlots = true;

    void SceneInspectorView::BuildComponentSection(const Guid& id, scene::ComponentManagerBase& mgr)
    {
        const TypeInfo* type = mgr.ComponentType();
        if (type == nullptr)
        {
            return;
        }
        // Category = the type name minus a trailing "Component", prettified
        // ("ReflectionProbeComponent" -> "Reflection Probe").
        const StringView fallback = mgr.SerializationTypeId();
        String categoryStorage;
        if (IsRegisteredType(type))
        {
            categoryStorage = ComponentDisplayName(type);
        }
        else
        {
            categoryStorage =
                fallback.IsEmpty() ? StringView(u8"(unreflected component)") : fallback;
        }
        const StringView category = categoryStorage.AsView();

        for (const PropertyInfo& prop : Properties(*type))
        {
            if (prop.type != nullptr && IsContainer(*prop.type))
            {
                // Mesh materials has a bespoke (mesh-aware) editor; when that mode is selected, skip
                // the generic list here and let BuildMaterialSlots render it below.
                const bool bespokeMeshMaterials =
                    !kUseReflectedMaterialSlots &&
                    mgr.SerializationTypeId() == StringView(u8"mesh") &&
                    StringView(reinterpret_cast<const utf8char*>(prop.name)) ==
                        StringView(u8"materials");
                if (!bespokeMeshMaterials)
                {
                    BuildContainerRows(id, type, prop, category); // generic reflected list editor
                }
                continue;
            }
            if (IsNested(prop))
            {
                continue; // nested structures are recursed elsewhere, not a leaf row
            }
            const usize firstRow = m_grid->PropertyCount();
            BuildPropertyRow(id, type, prop, category);
            ApplyPropertyPresentation(type, prop, firstRow,
                                      [edit = m_edit, id, type]() -> Instance
                                      {
                                          scene::ComponentManagerBase* mgr =
                                              edit->FindManager(type);
                                          const scene::EntityHandle e = edit->Resolve(id);
                                          return (mgr != nullptr && e.IsAssigned())
                                                     ? mgr->GetComponentInstance(e)
                                                     : Instance{};
                                      });
        }

        SceneEditContext* edit = m_edit;
        EditorContext* editor = m_editor;

        // MeshComponent: the material SLOT list (unified array; slot 0 = whole-mesh). Two UIs, chosen
        // by kUseReflectedMaterialSlots: the bespoke mesh-aware editor (below, renders through the same
        // ContainerListEditor) OR the generic reflection-driven list (already emitted above). Materials
        // is reflected either way (scriptable / tooling-traversable); the flag only picks the inspector
        // UI.
        if (!kUseReflectedMaterialSlots && mgr.SerializationTypeId() == StringView(u8"mesh"))
        {
            BuildMaterialSlots(id, category);
        }

        // ScriptComponent: the ordered behavior list, each a script picker + the
        // rows the cooked ScriptClass metadata drives (scripting.md P1 §5).
        if (mgr.SerializationTypeId() == StringView(u8"script"))
        {
            BuildScriptBehaviors(id, category);
        }

        // Prefab members: a per-component revert row whose label carries a LIVE override
        // dot (recomputed by the refresher, so it tracks edits and undo without grid
        // rebuilds). Revert rides the undoable paste-component path.
        {
            scene::PrefabMemberInfo member;
            if (mgr.IsSerializable() && scene::FindPrefabMember(edit->Scene(), id, member))
            {
                auto revert = MakeRef<ui::toolkit::ButtonEditor>(
                    DefaultAllocator(), StringView(u8"Revert to Prefab"),
                    Function<void()>{[edit, id, type]()
                                     { (void)edit->RevertComponentToBaseline(id, type); }},
                    category);
                revert->SetTooltip(u8"Reverts this component to the prefab's values (undoable).");
                revert->SetButtonEnabled(false); // refresher enables it on an override
                scene::ComponentManagerBase* manager = &mgr;
                AddEditor(revert.Get(),
                          [edit, id, manager, raw = revert.Get()]()
                          {
                              scene::PrefabMemberInfo m;
                              const bool overridden =
                                  scene::FindPrefabMember(edit->Scene(), id, m) &&
                                  scene::IsPrefabComponentOverridden(edit->Scene(), m, *manager);
                              raw->SetButtonEnabled(overridden);
                              // " *" matches the dirty-tab convention AND stays inside the editor
                              // font's rasterized range (ExtendedLatin = codepoints <= 255; a
                              // U+25CF dot has no glyph and silently renders as nothing).
                              raw->SetDisplayName(overridden ? StringView(u8"Revert to Prefab *")
                                                             : StringView(u8"Revert to Prefab"));
                          });
            }
        }

        // Copy / Remove as right-aligned ICON actions in this component's category header (clicking an
        // icon runs the action; clicking elsewhere on the header toggles the section). Only regular
        // components get these - Transform / scene-settings sections do not add header actions.
        {
            draconic::editor::app::EditorIcons& icons = draconic::editor::app::EditorIcons::Get();
            auto actions = MakeRef<ui::FlexLayout>(DefaultAllocator());
            actions->Direction = ui::Orientation::Horizontal;
            actions->Spacing = 2.0f;

            auto copyBtn = MakeRef<ui::IconButton>(DefaultAllocator(), icons.copy.Get(), 18.0f);
            copyBtn->TooltipText = String(u8"Copy component");
            copyBtn->OnClick.Add(
                [edit, editor, id, type](ui::ButtonBase*)
                {
                    Array<byte> blob = edit->CopyComponent(id, type);
                    if (!blob.IsEmpty())
                    {
                        editor->SetClipboard(u8"component", Move(blob));
                    }
                });
            actions->AddView(copyBtn.Get());

            auto removeBtn = MakeRef<ui::IconButton>(DefaultAllocator(), icons.remove.Get(), 18.0f);
            removeBtn->TooltipText = String(u8"Remove component");
            removeBtn->OnClick.Add([edit, id, type](ui::ButtonBase*)
                                   { edit->RemoveComponent(id, type); });
            actions->AddView(removeBtn.Get());

            m_grid->SetCategoryHeaderActions(category, RefPtr<ui::View>(actions.Get()));
        }
    }

    i64 SceneInspectorView::RawIntValue(const Instance& obj, const PropertyInfo& p)
    {
        void* address = (p.address != nullptr) ? p.address(obj) : nullptr;
        if (address == nullptr)
        {
            return 0;
        }
        switch (p.type->size)
        {
        case 1:
            return *static_cast<const i8*>(address);
        case 2:
            return *static_cast<const i16*>(address);
        case 8:
            return *static_cast<const i64*>(address);
        default:
            return *static_cast<const i32*>(address);
        }
    }

    void SceneInspectorView::BuildPropertyRow(const Guid& id, const TypeInfo* type,
                                              const PropertyInfo& prop, StringView category)
    {
        SceneEditContext* edit = m_edit;
        const StringView name(reinterpret_cast<const utf8char*>(prop.name));
        const bool readOnly =
            (static_cast<u32>(prop.flags) & static_cast<u32>(PropertyFlags::ReadOnly)) != 0;
        const char* propName = prop.name;

        // Resource references: a picker over the source DB's matching assets. Matched by
        // EXACT Ref<T> type identity (the TypeInfo pointer), so the unregistered template
        // type name ("<value>") never matters.
        if (prop.type == &TypeOf<draconic::resource::Ref<draconic::geometry::StaticMesh>>())
        {
            // SkinnedMeshAsset too: SkinnedMesh IS-A StaticMesh (bind pose when drawn
            // through the static path), so both asset types are valid targets.
            BuildResourceRefRow<draconic::geometry::StaticMesh>(
                id, type, prop, category, {u8"StaticMeshAsset", u8"SkinnedMeshAsset"});
            return;
        }
        if (prop.type == &TypeOf<draconic::resource::Ref<draconic::materials::Material>>())
        {
            BuildResourceRefRow<draconic::materials::Material>(id, type, prop, category,
                                                               {u8"MaterialAsset"});
            return;
        }
        if (prop.type == &TypeOf<draconic::resource::Ref<draconic::animation::Skeleton>>())
        {
            BuildResourceRefRow<draconic::animation::Skeleton>(id, type, prop, category,
                                                               {u8"SkeletonAsset"});
            return;
        }
        if (prop.type == &TypeOf<draconic::resource::Ref<draconic::animation::AnimationClip>>())
        {
            BuildResourceRefRow<draconic::animation::AnimationClip>(id, type, prop, category,
                                                                    {u8"AnimationClipAsset"});
            return;
        }
        if (prop.type == &TypeOf<draconic::resource::Ref<draconic::animation::AnimationGraph>>())
        {
            BuildResourceRefRow<draconic::animation::AnimationGraph>(id, type, prop, category,
                                                                     {u8"AnimationGraphAsset"});
            return;
        }
        if (prop.type == &TypeOf<draconic::resource::Ref<draconic::texture::Texture>>())
        {
            BuildResourceRefRow<draconic::texture::Texture>(id, type, prop, category,
                                                            {u8"TextureAsset"});
            return;
        }
        if (prop.type ==
            &TypeOf<draconic::resource::Ref<draconic::particles::ParticleEffectResource>>())
        {
            BuildResourceRefRow<draconic::particles::ParticleEffectResource>(
                id, type, prop, category, {u8"ParticleEffectAsset"});
            return;
        }
        if (prop.type == &TypeOf<draconic::resource::Ref<draconic::physics::CollisionShape>>())
        {
            BuildResourceRefRow<draconic::physics::CollisionShape>(id, type, prop, category,
                                                                   {u8"CollisionShapeAsset"});
            return;
        }
        if (prop.type == &TypeOf<draconic::resource::Ref<draconic::physics::PhysicalMaterial>>())
        {
            BuildResourceRefRow<draconic::physics::PhysicalMaterial>(id, type, prop, category,
                                                                     {u8"PhysicalMaterialAsset"});
            return;
        }
        if (prop.type == &TypeOf<draconic::resource::Ref<draconic::audio::AudioClip>>())
        {
            BuildResourceRefRow<draconic::audio::AudioClip>(id, type, prop, category,
                                                            {u8"AudioClipAsset"});
            return;
        }
        if (prop.type == &TypeOf<draconic::resource::Ref<draconic::audio::SoundCue>>())
        {
            BuildResourceRefRow<draconic::audio::SoundCue>(id, type, prop, category,
                                                           {u8"SoundCueAsset"});
            return;
        }
        if (prop.type == &TypeOf<draconic::resource::Ref<draconic::ui::UIDocument>>())
        {
            BuildResourceRefRow<draconic::ui::UIDocument>(id, type, prop, category,
                                                          {u8"UIDocumentAsset"});
            return;
        }
        if (prop.type == &TypeOf<draconic::resource::Ref<draconic::ui::UITheme>>())
        {
            BuildResourceRefRow<draconic::ui::UITheme>(id, type, prop, category,
                                                       {u8"UIThemeAsset"});
            return;
        }

        // Pulls the current Variant (empty component -> default Variant guards below).
        auto getVariant = [edit, id, type, propName]() -> Variant
        {
            scene::ComponentManagerBase* mgr = edit->FindManager(type);
            const scene::EntityHandle e = edit->Resolve(id);
            if (mgr == nullptr || !e.IsAssigned())
            {
                return {};
            }
            const Instance component = mgr->GetComponentInstance(e);
            const PropertyInfo* p = component.IsEmpty() ? nullptr : FindProperty(*type, propName);
            return (p != nullptr) ? GetProperty(*p, component) : Variant{};
        };

        if (IsEnum(*prop.type))
        {
            const Span<const EnumValue> values = Enumerators(*prop.type);
            Array<StringView> items;
            for (const EnumValue& v : values)
            {
                items.PushBack(StringView(reinterpret_cast<const utf8char*>(v.name)));
            }

            auto rawRead = [edit, id, type, propName]() -> i64
            {
                scene::ComponentManagerBase* mgr = edit->FindManager(type);
                const scene::EntityHandle e = edit->Resolve(id);
                if (mgr == nullptr || !e.IsAssigned())
                {
                    return 0;
                }
                const Instance component = mgr->GetComponentInstance(e);
                const PropertyInfo* p =
                    component.IsEmpty() ? nullptr : FindProperty(*type, propName);
                void* address =
                    (p != nullptr && p->address != nullptr) ? p->address(component) : nullptr;
                if (address == nullptr)
                {
                    return 0;
                }
                switch (p->type->size)
                {
                case 1:
                    return *static_cast<const i8*>(address);
                case 2:
                    return *static_cast<const i16*>(address);
                case 8:
                    return *static_cast<const i64*>(address);
                default:
                    return *static_cast<const i32*>(address);
                }
            };
            auto indexOf = [values](i64 value) -> i32
            {
                for (usize i = 0; i < values.Size(); ++i)
                {
                    if (values[i].value == value)
                    {
                        return static_cast<i32>(i);
                    }
                }
                return 0;
            };

            auto editor = MakeRef<ui::toolkit::EnumEditor>(
                DefaultAllocator(), name, indexOf(rawRead()),
                Span<const StringView>{items.Data(), items.Size()},
                readOnly ? Function<void(i32)>{}
                         : Function<void(i32)>{[edit, id, type, propName, values](i32 index)
                                               {
                                                   if (index >= 0 &&
                                                       index < static_cast<i32>(values.Size()))
                                                   {
                                                       edit->SetComponentPropertyRaw(
                                                           id, type, propName,
                                                           values[static_cast<usize>(index)].value);
                                                   }
                                               }},
                category);
            AddEditor(editor.Get(), [rawRead, indexOf, raw = editor.Get()]()
                      { raw->SetValue(indexOf(rawRead())); });
            return;
        }

        if (prop.type == &TypeOf<f32>())
        {
            auto value = [getVariant]() -> f64
            {
                const Variant v = getVariant();
                const f32* f = v.TryGet<f32>();
                return (f != nullptr) ? static_cast<f64>(*f) : 0.0;
            };
            // "range" attribute -> bounded slider+field instead of a bare numeric field.
            if (const Float4* range = RangeOf(prop))
            {
                auto editor = MakeRef<ui::toolkit::RangeEditor>(
                    DefaultAllocator(), name, static_cast<f32>(value()), range->x, range->y,
                    range->z,
                    readOnly
                        ? Function<void(f32)>{}
                        : Function<void(f32)>{[edit, id, type, propName](f32 v)
                                              {
                                                  edit->SetComponentProperty(id, type, propName,
                                                                             Variant::From<f32>(v));
                                              }},
                    category);
                AddEditor(editor.Get(), [value, raw = editor.Get()]()
                          { raw->SetValue(static_cast<f32>(value())); });
                return;
            }
            auto editor = MakeRef<ui::toolkit::FloatEditor>(
                DefaultAllocator(), name, value(), -1e9, 1e9, 0.1, 2,
                readOnly ? Function<void(f64)>{}
                         : Function<void(f64)>{[edit, id, type, propName](f64 v)
                                               {
                                                   edit->SetComponentProperty(
                                                       id, type, propName,
                                                       Variant::From<f32>(static_cast<f32>(v)));
                                               }},
                category);
            AddEditor(editor.Get(), [value, raw = editor.Get()]() { raw->SetValue(value()); });
            return;
        }

        if (prop.type == &TypeOf<bool>())
        {
            auto value = [getVariant]() -> bool
            {
                const Variant v = getVariant();
                const bool* b = v.TryGet<bool>();
                return b != nullptr && *b;
            };
            auto editor = MakeRef<ui::toolkit::BoolEditor>(
                DefaultAllocator(), name, value(),
                readOnly ? Function<void(bool)>{}
                         : Function<void(bool)>{[edit, id, type, propName](bool v)
                                                {
                                                    edit->SetComponentProperty(
                                                        id, type, propName, Variant::From<bool>(v));
                                                }},
                category);
            AddEditor(editor.Get(), [value, raw = editor.Get()]() { raw->SetValue(value()); });
            return;
        }

        if (prop.type == &TypeOf<i32>() || prop.type == &TypeOf<u32>() ||
            prop.type == &TypeOf<i64>() || prop.type == &TypeOf<u64>())
        {
            const TypeInfo* intType = prop.type;
            auto value = [getVariant, intType]() -> i64
            {
                const Variant v = getVariant();
                if (intType == &TypeOf<i32>())
                {
                    const i32* p = v.TryGet<i32>();
                    return p ? *p : 0;
                }
                if (intType == &TypeOf<u32>())
                {
                    const u32* p = v.TryGet<u32>();
                    return p ? static_cast<i64>(*p) : 0;
                }
                if (intType == &TypeOf<u64>())
                {
                    const u64* p = v.TryGet<u64>();
                    return p ? static_cast<i64>(*p) : 0;
                }
                const i64* p = v.TryGet<i64>();
                return p ? *p : 0;
            };
            auto setter = [edit, id, type, propName, intType](i64 v)
            {
                if (intType == &TypeOf<i32>())
                {
                    edit->SetComponentProperty(id, type, propName,
                                               Variant::From<i32>(static_cast<i32>(v)));
                }
                else if (intType == &TypeOf<u32>())
                {
                    edit->SetComponentProperty(id, type, propName,
                                               Variant::From<u32>(static_cast<u32>(v)));
                }
                else if (intType == &TypeOf<u64>())
                {
                    edit->SetComponentProperty(id, type, propName,
                                               Variant::From<u64>(static_cast<u64>(v)));
                }
                else
                {
                    edit->SetComponentProperty(id, type, propName, Variant::From<i64>(v));
                }
            };
            auto editor = MakeRef<ui::toolkit::IntEditor>(
                DefaultAllocator(), name, value(), std::numeric_limits<i64>::min(),
                std::numeric_limits<i64>::max(),
                readOnly ? Function<void(i64)>{} : Function<void(i64)>{Move(setter)}, category);
            AddEditor(editor.Get(), [value, raw = editor.Get()]() { raw->SetValue(value()); });
            return;
        }

        if (prop.type == &TypeOf<String>())
        {
            auto value = [getVariant]() -> String
            {
                const Variant v = getVariant();
                const String* s = v.TryGet<String>();
                return (s != nullptr) ? String(*s) : String{};
            };
            auto editor = MakeRef<ui::toolkit::StringEditor>(
                DefaultAllocator(), name, value().AsView(),
                readOnly ? Function<void(StringView)>{}
                         : Function<void(StringView)>{[edit, id, type, propName](StringView v)
                                                      {
                                                          edit->SetComponentProperty(
                                                              id, type, propName,
                                                              Variant::From<String>(String(v)));
                                                      }},
                category);
            AddEditor(editor.Get(),
                      [value, raw = editor.Get()]() { raw->SetValue(value().AsView()); });
            return;
        }

        if (prop.type == &TypeOf<Float3>())
        {
            auto value = [getVariant]() -> Float3
            {
                const Variant v = getVariant();
                const Float3* f = v.TryGet<Float3>();
                return (f != nullptr) ? *f : Float3{};
            };
            auto editor = MakeRef<ui::toolkit::Float3Editor>(
                DefaultAllocator(), name, value(), -100000.0f, 100000.0f, 0.1f,
                readOnly
                    ? Function<void(Float3)>{}
                    : Function<void(Float3)>{[edit, id, type, propName](Float3 v)
                                             {
                                                 edit->SetComponentProperty(
                                                     id, type, propName, Variant::From<Float3>(v));
                                             }},
                category);
            AddEditor(editor.Get(), [value, raw = editor.Get()]() { raw->SetValue(value()); });
            return;
        }

        if (prop.type == &TypeOf<Color>())
        {
            auto value = [getVariant]() -> Color
            {
                const Variant v = getVariant();
                const Color* c = v.TryGet<Color>();
                return (c != nullptr) ? *c : Color{1, 1, 1, 1};
            };
            auto editor = MakeRef<ui::toolkit::ColorEditor>(
                DefaultAllocator(), name, value(),
                readOnly
                    ? Function<void(Color)>{}
                    : Function<void(Color)>{[edit, id, type, propName](Color v)
                                            {
                                                edit->SetComponentProperty(id, type, propName,
                                                                           Variant::From<Color>(v));
                                            }},
                category);
            AddEditor(editor.Get(), [value, raw = editor.Get()]() { raw->SetValue(value()); });
            return;
        }

        if (prop.type == &TypeOf<Float2>())
        {
            auto value = [getVariant]() -> Float2
            {
                const Variant v = getVariant();
                const Float2* f = v.TryGet<Float2>();
                return (f != nullptr) ? *f : Float2{};
            };
            auto editor = MakeRef<ui::toolkit::Float2Editor>(
                DefaultAllocator(), name, value(), -100000.0f, 100000.0f, 0.1f,
                readOnly
                    ? Function<void(Float2)>{}
                    : Function<void(Float2)>{[edit, id, type, propName](Float2 v)
                                             {
                                                 edit->SetComponentProperty(
                                                     id, type, propName, Variant::From<Float2>(v));
                                             }},
                category);
            AddEditor(editor.Get(), [value, raw = editor.Get()]() { raw->SetValue(value()); });
            return;
        }

        if (prop.type == &TypeOf<Float4>())
        {
            auto value = [getVariant]() -> Float4
            {
                const Variant v = getVariant();
                const Float4* f = v.TryGet<Float4>();
                return (f != nullptr) ? *f : Float4{};
            };
            auto editor = MakeRef<ui::toolkit::Float4Editor>(
                DefaultAllocator(), name, value(), -100000.0f, 100000.0f, 0.1f,
                readOnly
                    ? Function<void(Float4)>{}
                    : Function<void(Float4)>{[edit, id, type, propName](Float4 v)
                                             {
                                                 edit->SetComponentProperty(
                                                     id, type, propName, Variant::From<Float4>(v));
                                             }},
                category);
            AddEditor(editor.Get(), [value, raw = editor.Get()]() { raw->SetValue(value()); });
            return;
        }

        // Unsupported reflected type: skipped.
    }

    void SceneInspectorView::MutateMeshMaterials(
        const Guid& id, const Function<void(draconic::render::MeshComponent&)>& mutate)
    {
        const scene::EntityHandle e = m_edit->Resolve(id);
        auto* manager = m_edit->Scene().GetSystem<draconic::render::MeshComponentManager>();
        draconic::render::MeshComponent* live =
            (manager != nullptr && e.IsAssigned()) ? manager->Get(e) : nullptr;
        if (live == nullptr)
        {
            return;
        }
        const draconic::render::MeshComponent before = *live;
        mutate(*live);
        Array<byte> blob = m_edit->CopyComponent(id, &TypeOf<draconic::render::MeshComponent>());
        *live = before;
        if (!blob.IsEmpty())
        {
            (void)m_edit->PasteComponent(id, Span<const byte>{blob.Data(), blob.Size()});
        }
    }

    Array<String> SceneInspectorView::MaterialSlotNames(const draconic::render::MeshComponent& mc)
    {
        Array<String> names;
        for (usize i = 0; i < mc.materials.Size(); ++i)
        {
            const Guid target = mc.materials[i].id;
            if (!target.IsNil())
            {
                names.PushBack(String(AssetNameFor(target)));
            }
            else if (mc.materials[i].Get() != nullptr)
            {
                names.PushBack(String(u8"(runtime)"));
            }
            else
            {
                names.PushBack(String(u8"(none)"));
            }
        }
        return names;
    }

    void SceneInspectorView::BuildMaterialSlots(const Guid& id, StringView category)
    {
        const scene::EntityHandle e = m_edit->Resolve(id);
        auto* manager = m_edit->Scene().GetSystem<draconic::render::MeshComponentManager>();
        draconic::render::MeshComponent* mc =
            (manager != nullptr && e.IsAssigned()) ? manager->Get(e) : nullptr;
        if (mc == nullptr)
        {
            return;
        }

        auto slots =
            MakeRef<ContainerListEditor>(DefaultAllocator(), StringView(u8"Materials"), category);
        slots->SetTooltip(u8"Material slots, indexed by the mesh's submesh material index. "
                          u8"Slot 0 also covers single-material meshes and any submesh "
                          u8"whose index has no slot.");
        slots->slotNames = MaterialSlotNames(*mc);

        SceneInspectorView* self = this;
        slots->OnAdd = [self, id]()
        {
            self->MutateMeshMaterials(
                id,
                [](draconic::render::MeshComponent& c)
                {
                    c.materials.PushBack(draconic::resource::Ref<draconic::materials::Material>{});
                });
        };
        slots->OnRemoveSlot = [self, id](usize slot)
        {
            self->MutateMeshMaterials(id,
                                      [slot](draconic::render::MeshComponent& c)
                                      {
                                          if (slot < c.materials.Size())
                                          {
                                              c.materials.RemoveAt(slot);
                                          }
                                      });
        };
        slots->OnMoveSlot = [self, id](usize slot, bool up)
        {
            self->MutateMeshMaterials(
                id,
                [slot, up](draconic::render::MeshComponent& c)
                {
                    const usize other = up ? slot - 1 : slot + 1;
                    if (slot < c.materials.Size() && other < c.materials.Size())
                    {
                        draconic::resource::Ref<draconic::materials::Material> tmp =
                            c.materials[slot];
                        c.materials[slot] = c.materials[other];
                        c.materials[other] = tmp;
                    }
                });
        };
        slots->OnPickSlot = [self, id](usize slot)
        {
            if (self->Context == nullptr || self->m_editor->Project() == nullptr)
            {
                return;
            }
            Array<String> typeNames;
            typeNames.PushBack(String(u8"MaterialAsset"));
            auto picker = MakeRef<draconic::editor::app::AssetPickerDialog>(
                DefaultAllocator(), *self->m_editor, Move(typeNames));
            picker->OnPicked = [self, id, slot](const Guid& picked)
            {
                self->MutateMeshMaterials(
                    id,
                    [slot, picked](draconic::render::MeshComponent& c)
                    {
                        if (slot >= c.materials.Size())
                        {
                            return;
                        }
                        c.materials[slot] =
                            draconic::resource::Ref<draconic::materials::Material>{};
                        c.materials[slot].SetId(picked);
                    });
            };
            picker->Show(self->Context);
        };
        RefPtr<ContainerListEditor> slotsRef = slots;
        AddEditor(slots.Get(),
                  [self, id, slotsRef]()
                  {
                      const scene::EntityHandle live = self->m_edit->Resolve(id);
                      auto* mgr =
                          self->m_edit->Scene().GetSystem<draconic::render::MeshComponentManager>();
                      draconic::render::MeshComponent* c =
                          (mgr != nullptr && live.IsAssigned()) ? mgr->Get(live) : nullptr;
                      if (c == nullptr)
                      {
                          return;
                      } // presence loss flips the Signature anyway
                      const Array<String> names = self->MaterialSlotNames(*c);
                      bool same = names.Size() == slotsRef->slotNames.Size();
                      for (usize i = 0; same && i < names.Size(); ++i)
                      {
                          same = names[i] == slotsRef->slotNames[i];
                      }
                      if (!same)
                      {
                          self->m_forceRebuild = true;
                      }
                  });
    }

    void SceneInspectorView::MutateScriptComponent(
        const Guid& id, const Function<void(draconic::script::ScriptComponent&)>& mutate)
    {
        const scene::EntityHandle e = m_edit->Resolve(id);
        auto* manager = m_edit->Scene().GetSystem<draconic::script::ScriptComponentManager>();
        draconic::script::ScriptComponent* live =
            (manager != nullptr && e.IsAssigned()) ? manager->Get(e) : nullptr;
        if (live == nullptr)
        {
            return;
        }
        const draconic::script::ScriptComponent before = *live;
        mutate(*live);
        Array<byte> blob = m_edit->CopyComponent(id, &TypeOf<draconic::script::ScriptComponent>());
        *live = before;
        if (!blob.IsEmpty())
        {
            (void)m_edit->PasteComponent(id, Span<const byte>{blob.Data(), blob.Size()});
        }
    }

    draconic::script::ScriptClass*
    SceneInspectorView::BehaviorClass(const draconic::script::ScriptBehavior& behavior)
    {
        if (behavior.script.Get() != nullptr)
        {
            return behavior.script.Get();
        }
        if (behavior.script.id.IsNil() || m_editor->Resources() == nullptr)
        {
            return nullptr;
        }
        auto proxy = m_editor->Resources()->Bind<draconic::script::ScriptClass>(behavior.script.id);
        return proxy.Get();
    }

    u64 SceneInspectorView::ScriptBehaviorsSignature(const draconic::script::ScriptComponent& c)
    {
        u64 hash = HashInteger(c.behaviors.Size());
        for (const draconic::script::ScriptBehavior& b : c.behaviors)
        {
            hash = HashBytes(&b.script.id, sizeof(Guid), hash);
            const u64 flags = (b.enabled ? 1u : 0u) | (b.overrides.Size() << 1);
            hash = HashBytes(&flags, sizeof(flags), hash);
        }
        return hash;
    }

    void SceneInspectorView::BuildScriptBehaviors(const Guid& id, StringView category)
    {
        const scene::EntityHandle e = m_edit->Resolve(id);
        auto* manager = m_edit->Scene().GetSystem<draconic::script::ScriptComponentManager>();
        draconic::script::ScriptComponent* component =
            (manager != nullptr && e.IsAssigned()) ? manager->Get(e) : nullptr;
        if (component == nullptr)
        {
            return;
        }

        SceneInspectorView* self = this;
        for (usize i = 0; i < component->behaviors.Size(); ++i)
        {
            BuildScriptBehaviorRows(id, category, i);
        }

        auto add = MakeRef<ui::toolkit::ButtonEditor>(
            DefaultAllocator(), StringView(u8"+ Add Behavior"),
            Function<void()>{[self, id]()
                             {
                                 self->MutateScriptComponent(
                                     id, [](draconic::script::ScriptComponent& c)
                                     { c.behaviors.PushBack(draconic::script::ScriptBehavior{}); });
                             }},
            category);
        m_grid->AddProperty(RefPtr<ui::toolkit::PropertyEditor>(add.Get()));

        // Shape-change watcher (add/remove/reorder/pick/override toggle rebuilds).
        const u64 signature = ScriptBehaviorsSignature(*component);
        auto watcher = MakeRef<ui::toolkit::ButtonEditor>(DefaultAllocator(), StringView(u8""),
                                                          Function<void()>{[]() {}}, category);
        watcher->SetRowVisible(false);
        AddEditor(
            watcher.Get(),
            [self, id, signature]()
            {
                const scene::EntityHandle live = self->m_edit->Resolve(id);
                auto* mgr =
                    self->m_edit->Scene().GetSystem<draconic::script::ScriptComponentManager>();
                draconic::script::ScriptComponent* c =
                    (mgr != nullptr && live.IsAssigned()) ? mgr->Get(live) : nullptr;
                if (c != nullptr && self->ScriptBehaviorsSignature(*c) != signature)
                {
                    self->m_forceRebuild = true;
                }
            });
    }

    void SceneInspectorView::BuildScriptBehaviorRows(const Guid& id, StringView category,
                                                     usize index)
    {
        const scene::EntityHandle e = m_edit->Resolve(id);
        auto* manager = m_edit->Scene().GetSystem<draconic::script::ScriptComponentManager>();
        draconic::script::ScriptComponent* component =
            (manager != nullptr && e.IsAssigned()) ? manager->Get(e) : nullptr;
        if (component == nullptr || index >= component->behaviors.Size())
        {
            return;
        }
        draconic::script::ScriptBehavior& behavior = component->behaviors[index];
        SceneInspectorView* self = this;

        // Script picker (AssetPickerDialog filtered to ScriptClass).
        const StringView assetName =
            behavior.script.id.IsNil() ? StringView(u8"(none)") : AssetNameFor(behavior.script.id);
        auto picker = MakeRef<ResourceRefEditor>(DefaultAllocator(), StringView(u8"Script"),
                                                 assetName, category);
        ResourceRefEditor* pickerRaw = picker.Get();
        pickerRaw->OnPick = [self, id, index]()
        {
            if (self->Context == nullptr || self->m_editor->Project() == nullptr)
            {
                return;
            }
            Array<String> typeNames;
            typeNames.PushBack(String(u8"ScriptClassAsset"));
            auto dialog = MakeRef<draconic::editor::app::AssetPickerDialog>(
                DefaultAllocator(), *self->m_editor, Move(typeNames));
            dialog->OnPicked = [self, id, index](const Guid& picked)
            {
                self->MutateScriptComponent(
                    id,
                    [index, picked](draconic::script::ScriptComponent& c)
                    {
                        if (index >= c.behaviors.Size())
                        {
                            return;
                        }
                        c.behaviors[index].script =
                            draconic::resource::Ref<draconic::script::ScriptClass>{};
                        c.behaviors[index].script.SetId(picked);
                        c.behaviors[index].overrides.Clear(); // metadata changed
                    });
            };
            dialog->Show(self->Context);
        };
        AddEditor(
            pickerRaw,
            [self, id, index, pickerRaw]()
            {
                const scene::EntityHandle live = self->m_edit->Resolve(id);
                auto* mgr =
                    self->m_edit->Scene().GetSystem<draconic::script::ScriptComponentManager>();
                draconic::script::ScriptComponent* c =
                    (mgr != nullptr && live.IsAssigned()) ? mgr->Get(live) : nullptr;
                if (c == nullptr || index >= c->behaviors.Size())
                {
                    return;
                }
                const Guid target = c->behaviors[index].script.id;
                pickerRaw->SetValueText(target.IsNil() ? StringView(u8"(none)")
                                                       : self->AssetNameFor(target));
            });

        // Enabled toggle.
        auto enabled = MakeRef<ui::toolkit::BoolEditor>(
            DefaultAllocator(), StringView(u8"Enabled"), behavior.enabled,
            Function<void(bool)>{[self, id, index](bool value)
                                 {
                                     self->MutateScriptComponent(
                                         id,
                                         [index, value](draconic::script::ScriptComponent& c)
                                         {
                                             if (index < c.behaviors.Size())
                                             {
                                                 c.behaviors[index].enabled = value;
                                             }
                                         });
                                 }},
            category);
        m_grid->AddProperty(RefPtr<ui::toolkit::PropertyEditor>(enabled.Get()));

        // Update interval (P3 throttling): seconds between onUpdate; 0 = every tick.
        auto interval = MakeRef<ui::toolkit::FloatEditor>(
            DefaultAllocator(), StringView(u8"Update Interval"),
            static_cast<f64>(behavior.updateInterval), 0.0, 3600.0, 0.05, 3,
            Function<void(f64)>{[self, id, index](f64 value)
                                {
                                    self->MutateScriptComponent(
                                        id,
                                        [index, value](draconic::script::ScriptComponent& c)
                                        {
                                            if (index < c.behaviors.Size())
                                            {
                                                c.behaviors[index].updateInterval =
                                                    static_cast<f32>(value < 0.0 ? 0.0 : value);
                                            }
                                        });
                                }},
            category);
        interval->SetTooltip(StringView(u8"Seconds between onUpdate calls (0 = every frame)"));
        m_grid->AddProperty(RefPtr<ui::toolkit::PropertyEditor>(interval.Get()));

        // Reorder / remove.
        auto up = MakeRef<ui::toolkit::ButtonEditor>(
            DefaultAllocator(), StringView(u8"Move Up"),
            Function<void()>{[self, id, index]()
                             {
                                 self->MutateScriptComponent(
                                     id,
                                     [index](draconic::script::ScriptComponent& c)
                                     {
                                         if (index > 0 && index < c.behaviors.Size())
                                         {
                                             draconic::script::ScriptBehavior tmp =
                                                 Move(c.behaviors[index]);
                                             c.behaviors[index] = Move(c.behaviors[index - 1]);
                                             c.behaviors[index - 1] = Move(tmp);
                                         }
                                     });
                             }},
            category);
        up->SetButtonEnabled(index > 0);
        m_grid->AddProperty(RefPtr<ui::toolkit::PropertyEditor>(up.Get()));
        auto remove = MakeRef<ui::toolkit::ButtonEditor>(
            DefaultAllocator(), StringView(u8"Remove Behavior"),
            Function<void()>{[self, id, index]()
                             {
                                 self->MutateScriptComponent(
                                     id,
                                     [index](draconic::script::ScriptComponent& c)
                                     {
                                         if (index < c.behaviors.Size())
                                         {
                                             c.behaviors.RemoveAt(index);
                                         }
                                     });
                             }},
            category);
        m_grid->AddProperty(RefPtr<ui::toolkit::PropertyEditor>(remove.Get()));

        // Property rows from the cooked ScriptClass metadata (data-driven; no VM).
        draconic::script::ScriptClass* scriptClass = BehaviorClass(behavior);
        if (scriptClass == nullptr)
        {
            return;
        }
        for (const draconic::script::ScriptPropertyDesc& property : scriptClass->properties)
        {
            BuildScriptPropertyRow(id, category, index, property);
        }
    }

    void
    SceneInspectorView::BuildScriptPropertyRow(const Guid& id, StringView category, usize index,
                                               const draconic::script::ScriptPropertyDesc& property)
    {
        SceneInspectorView* self = this;
        const u64 hash = property.hash;
        using draconic::script::ScriptComponent;
        using draconic::script::ScriptPropertyType;
        using draconic::script::ScriptPropertyValue;

        // The effective value = override if present, else the harvested default.
        auto effective = [self, id, index, hash, property]() -> ScriptPropertyValue
        {
            const scene::EntityHandle live = self->m_edit->Resolve(id);
            auto* mgr = self->m_edit->Scene().GetSystem<draconic::script::ScriptComponentManager>();
            ScriptComponent* c = (mgr != nullptr && live.IsAssigned()) ? mgr->Get(live) : nullptr;
            if (c != nullptr && index < c->behaviors.Size())
            {
                if (const auto* over = c->behaviors[index].FindOverride(hash))
                {
                    return over->value;
                }
            }
            return property.defaultValue;
        };
        auto setOverride = [self, id, index, hash](const ScriptPropertyValue& value)
        {
            self->MutateScriptComponent(id,
                                        [index, hash, value](ScriptComponent& c)
                                        {
                                            if (index < c.behaviors.Size())
                                            {
                                                c.behaviors[index].SetOverride(hash, value);
                                            }
                                        });
        };
        const StringView name = property.name.AsView();

        switch (property.type)
        {
        case ScriptPropertyType::Float:
        {
            auto editor = MakeRef<ui::toolkit::FloatEditor>(
                DefaultAllocator(), name, effective().number, -1e9, 1e9, 0.1, 3,
                Function<void(f64)>{[setOverride](f64 v)
                                    {
                                        ScriptPropertyValue value;
                                        value.kind = ScriptPropertyType::Float;
                                        value.number = v;
                                        setOverride(value);
                                    }},
                category);
            if (!property.description.IsEmpty())
            {
                editor->SetTooltip(property.description.AsView());
            }
            AddEditor(editor.Get(),
                      [effective, raw = editor.Get()]() { raw->SetValue(effective().number); });
            break;
        }
        case ScriptPropertyType::Int:
        {
            auto editor = MakeRef<ui::toolkit::IntEditor>(
                DefaultAllocator(), name, static_cast<i64>(effective().number),
                std::numeric_limits<i64>::min(), std::numeric_limits<i64>::max(),
                Function<void(i64)>{[setOverride](i64 v)
                                    {
                                        ScriptPropertyValue value;
                                        value.kind = ScriptPropertyType::Int;
                                        value.number = static_cast<f64>(v);
                                        setOverride(value);
                                    }},
                category);
            if (!property.description.IsEmpty())
            {
                editor->SetTooltip(property.description.AsView());
            }
            AddEditor(editor.Get(), [effective, raw = editor.Get()]()
                      { raw->SetValue(static_cast<i64>(effective().number)); });
            break;
        }
        case ScriptPropertyType::Bool:
        {
            auto editor = MakeRef<ui::toolkit::BoolEditor>(
                DefaultAllocator(), name, effective().boolean,
                Function<void(bool)>{[setOverride](bool v)
                                     {
                                         ScriptPropertyValue value;
                                         value.kind = ScriptPropertyType::Bool;
                                         value.boolean = v;
                                         setOverride(value);
                                     }},
                category);
            if (!property.description.IsEmpty())
            {
                editor->SetTooltip(property.description.AsView());
            }
            AddEditor(editor.Get(),
                      [effective, raw = editor.Get()]() { raw->SetValue(effective().boolean); });
            break;
        }
        case ScriptPropertyType::String:
        {
            auto editor = MakeRef<ui::toolkit::StringEditor>(
                DefaultAllocator(), name, effective().text.AsView(),
                Function<void(StringView)>{[setOverride](StringView v)
                                           {
                                               ScriptPropertyValue value;
                                               value.kind = ScriptPropertyType::String;
                                               value.text = String(v);
                                               setOverride(value);
                                           }},
                category);
            if (!property.description.IsEmpty())
            {
                editor->SetTooltip(property.description.AsView());
            }
            AddEditor(editor.Get(), [effective, raw = editor.Get()]()
                      { raw->SetValue(effective().text.AsView()); });
            break;
        }
        case ScriptPropertyType::Color:
        {
            auto editor = MakeRef<ui::toolkit::ColorEditor>(
                DefaultAllocator(), name, effective().color,
                Function<void(Color)>{[setOverride](Color v)
                                      {
                                          ScriptPropertyValue value;
                                          value.kind = ScriptPropertyType::Color;
                                          value.color = v;
                                          setOverride(value);
                                      }},
                category);
            if (!property.description.IsEmpty())
            {
                editor->SetTooltip(property.description.AsView());
            }
            AddEditor(editor.Get(),
                      [effective, raw = editor.Get()]() { raw->SetValue(effective().color); });
            break;
        }
        case ScriptPropertyType::Vec3:
        {
            auto editor = MakeRef<ui::toolkit::Float3Editor>(
                DefaultAllocator(), name, effective().vector, -1e9f, 1e9f, 0.1f,
                Function<void(Float3)>{[setOverride](Float3 v)
                                       {
                                           ScriptPropertyValue value;
                                           value.kind = ScriptPropertyType::Vec3;
                                           value.vector = v;
                                           setOverride(value);
                                       }},
                category);
            if (!property.description.IsEmpty())
            {
                editor->SetTooltip(property.description.AsView());
            }
            AddEditor(editor.Get(),
                      [effective, raw = editor.Get()]() { raw->SetValue(effective().vector); });
            break;
        }
        case ScriptPropertyType::Entity:
        {
            BuildScriptEntityPropertyRow(id, category, index, property);
            break;
        }
        case ScriptPropertyType::Asset:
        {
            BuildScriptAssetPropertyRow(id, category, index, property);
            break;
        }
        case ScriptPropertyType::None:
        default:
            break;
        }
    }

    void SceneInspectorView::BuildScriptEntityPropertyRow(
        const Guid& id, StringView category, usize index,
        const draconic::script::ScriptPropertyDesc& property)
    {
        using draconic::script::ScriptComponent;
        using draconic::script::ScriptPropertyType;
        using draconic::script::ScriptPropertyValue;
        SceneInspectorView* self = this;
        const u64 hash = property.hash;

        auto currentTarget = [self, id, index, hash]() -> Guid
        {
            const scene::EntityHandle live = self->m_edit->Resolve(id);
            auto* mgr = self->m_edit->Scene().GetSystem<draconic::script::ScriptComponentManager>();
            ScriptComponent* c = (mgr != nullptr && live.IsAssigned()) ? mgr->Get(live) : nullptr;
            if (c != nullptr && index < c->behaviors.Size())
            {
                if (const auto* over = c->behaviors[index].FindOverride(hash))
                {
                    return over->value.guid;
                }
            }
            return Guid{};
        };
        auto nameOf = [self](const Guid& target) -> StringView
        {
            if (target.IsNil())
            {
                return u8"(none)";
            }
            const scene::EntityHandle h = self->m_edit->Scene().FindEntity(target);
            return h.IsAssigned() ? self->m_edit->Scene().GetEntityName(h)
                                  : StringView(u8"(missing)");
        };

        auto editor = MakeRef<ResourceRefEditor>(DefaultAllocator(), property.name.AsView(),
                                                 nameOf(currentTarget()), category);
        ResourceRefEditor* raw = editor.Get();
        if (!property.description.IsEmpty())
        {
            raw->SetTooltip(property.description.AsView());
        }
        raw->OnPick = [self, id, index, hash]()
        {
            if (self->Context == nullptr)
            {
                return;
            }
            auto menu = MakeRef<ui::ContextMenu>(DefaultAllocator());
            menu->AddItem(StringView(u8"(none)"),
                          [self, id, index, hash]()
                          {
                              self->MutateScriptComponent(
                                  id,
                                  [index, hash](draconic::script::ScriptComponent& c)
                                  {
                                      if (index < c.behaviors.Size())
                                      {
                                          c.behaviors[index].RemoveOverride(hash);
                                      }
                                  });
                          });
            menu->AddSeparator();
            self->m_edit->Scene().ForEachEntity(
                [self, id, index, hash, &menu](scene::EntityHandle handle)
                {
                    const Guid target = self->m_edit->Scene().GetEntityId(handle);
                    String label(self->m_edit->Scene().GetEntityName(handle));
                    menu->AddItem(
                        label.AsView(),
                        [self, id, index, hash, target]()
                        {
                            self->MutateScriptComponent(
                                id,
                                [index, hash, target](draconic::script::ScriptComponent& c)
                                {
                                    if (index >= c.behaviors.Size())
                                    {
                                        return;
                                    }
                                    ScriptPropertyValue value;
                                    value.kind = ScriptPropertyType::Entity;
                                    value.guid = target;
                                    c.behaviors[index].SetOverride(hash, value);
                                });
                        });
                });
            const Float2 pos = self->m_addButton->LocalToScreen(Float2{0.0f, 0.0f});
            menu->Show(self->Context, pos.x, pos.y);
        };
        AddEditor(raw,
                  [self, currentTarget, nameOf, raw]()
                  {
                      (void)self;
                      raw->SetValueText(nameOf(currentTarget()));
                  });
    }

    void SceneInspectorView::BuildScriptAssetPropertyRow(
        const Guid& id, StringView category, usize index,
        const draconic::script::ScriptPropertyDesc& property)
    {
        using draconic::script::ScriptComponent;
        using draconic::script::ScriptPropertyType;
        using draconic::script::ScriptPropertyValue;
        SceneInspectorView* self = this;
        const u64 hash = property.hash;
        const String assetType = property.assetType.IsEmpty() ? String(u8"") : property.assetType;

        auto currentTarget = [self, id, index, hash]() -> Guid
        {
            const scene::EntityHandle live = self->m_edit->Resolve(id);
            auto* mgr = self->m_edit->Scene().GetSystem<draconic::script::ScriptComponentManager>();
            ScriptComponent* c = (mgr != nullptr && live.IsAssigned()) ? mgr->Get(live) : nullptr;
            if (c != nullptr && index < c->behaviors.Size())
            {
                if (const auto* over = c->behaviors[index].FindOverride(hash))
                {
                    return over->value.guid;
                }
            }
            return Guid{};
        };

        auto editor = MakeRef<ResourceRefEditor>(DefaultAllocator(), property.name.AsView(),
                                                 AssetNameFor(currentTarget()), category);
        ResourceRefEditor* raw = editor.Get();
        if (!property.description.IsEmpty())
        {
            raw->SetTooltip(property.description.AsView());
        }
        raw->OnPick = [self, id, index, hash, assetType]()
        {
            if (self->Context == nullptr || self->m_editor->Project() == nullptr)
            {
                return;
            }
            Array<String> typeNames;
            // The harvested "AudioClip" maps to the "AudioClipAsset" source type.
            String assetTypeName(assetType.AsView());
            assetTypeName.Append(u8"Asset");
            typeNames.PushBack(Move(assetTypeName));
            auto dialog = MakeRef<draconic::editor::app::AssetPickerDialog>(
                DefaultAllocator(), *self->m_editor, Move(typeNames));
            dialog->OnPicked = [self, id, index, hash](const Guid& picked)
            {
                self->MutateScriptComponent(
                    id,
                    [index, hash, picked](draconic::script::ScriptComponent& c)
                    {
                        if (index >= c.behaviors.Size())
                        {
                            return;
                        }
                        ScriptPropertyValue value;
                        value.kind = ScriptPropertyType::Asset;
                        value.guid = picked;
                        c.behaviors[index].SetOverride(hash, value);
                    });
            };
            dialog->Show(self->Context);
        };
        AddEditor(raw, [self, currentTarget, raw]()
                  { raw->SetValueText(self->AssetNameFor(currentTarget())); });
    }

    StringView SceneInspectorView::AssetNameFor(const Guid& target)
    {
        if (target.IsNil())
        {
            return u8"(none)";
        }
        if (m_editor->Project() != nullptr)
        {
            if (draconic::content::Instance* inst =
                    m_editor->Project()->SourceDb().GetInstance(target))
            {
                return inst->Name();
            }
        }
        return u8"(missing)";
    }

    const Float4* SceneInspectorView::RangeOf(const PropertyInfo& prop)
    {
        const foundation::Attribute* attr = FindAttribute(prop, u8"range");
        return (attr != nullptr) ? attr->value.TryGet<Float4>() : nullptr;
    }

    void SceneInspectorView::AddEditor(ui::toolkit::PropertyEditor* editor,
                                       Function<void()> refresher)
    {
        m_grid->AddProperty(RefPtr<ui::toolkit::PropertyEditor>(editor));
        ui::toolkit::PropertyEditor* raw = editor;
        m_refreshers.PushBack(Function<void()>{[raw, pull = Move(refresher)]()
                                               {
                                                   if (!raw->IsEditing())
                                                   {
                                                       pull();
                                                   }
                                               }});
    }

    void SceneInspectorView::MutateComponent(const Guid& id, const TypeInfo* type,
                                             const Function<void(const Instance&)>& mutate)
    {
        const scene::EntityHandle e = m_edit->Resolve(id);
        scene::ComponentManagerBase* mgr = m_edit->FindManager(type);
        if (mgr == nullptr || !e.IsAssigned() || !mgr->HasComponent(e))
        {
            return;
        }
        Array<byte> before = m_edit->CopyComponent(id, type); // snapshot A (current)
        if (before.IsEmpty())
        {
            return;
        }
        mutate(mgr->GetComponentInstance(e));                // live -> B
        Array<byte> after = m_edit->CopyComponent(id, type); // snapshot B
        // Restore live to A (non-undoable ReadComponent), then PASTE B - the paste command captures
        // the pre-state (A), so the whole mutation is one undo step, exactly like the typed helpers.
        {
            MemoryStream buffer;
            (void)buffer.Write(before.Data(), before.Size());
            (void)buffer.Seek(0, SeekOrigin::Begin);
            BinarySerializer ar(buffer, SerializeMode::Read);
            String typeId;
            draconic::foundation::Serialize(ar, "type", typeId);
            mgr->ReadComponent(ar, e);
        }
        if (!after.IsEmpty())
        {
            (void)m_edit->PasteComponent(id, Span<const byte>{after.Data(), after.Size()});
        }
    }

    void SceneInspectorView::BuildContainerRows(const Guid& id, const TypeInfo* type,
                                                const PropertyInfo& prop, StringView category)
    {
        if (prop.type == nullptr || prop.type->container == nullptr)
        {
            return;
        }
        SceneInspectorView* self = this;
        const PropertyInfo* propPtr = &prop;
        using MatRef = draconic::resource::Ref<draconic::materials::Material>;

        // Per-slot display text from the live container: a material Ref shows its asset name / "None";
        // a struct element shows its type label. Recomputed by the refresher to detect changes.
        auto computeNames = [self, id, type, propPtr]() -> Array<String>
        {
            Array<String> names;
            scene::ComponentManagerBase* mgr = self->m_edit->FindManager(type);
            const scene::EntityHandle e = self->m_edit->Resolve(id);
            if (mgr == nullptr || !e.IsAssigned() || !mgr->HasComponent(e))
            {
                return names;
            }
            const Instance comp = mgr->GetComponentInstance(e);
            if (comp.IsEmpty())
            {
                return names;
            }
            const Instance container(propPtr->address(comp), propPtr->type);
            const ContainerInfo& ci = *propPtr->type->container;
            const usize n = ContainerSize(ci, container);
            for (usize i = 0; i < n; ++i)
            {
                const Instance el = ContainerAddressAt(ci, container, i);
                if (el.Pointer() != nullptr && el.Type() == &TypeOf<MatRef>())
                {
                    const Guid target = static_cast<const MatRef*>(el.Pointer())->id;
                    names.PushBack(target.IsNil() ? String(u8"None")
                                                  : String(self->AssetNameFor(target)));
                }
                else if (el.Type() != nullptr)
                {
                    names.PushBack(ContainerElementLabel(el.Type()));
                }
                else
                {
                    names.PushBack(String(u8"(none)"));
                }
            }
            return names;
        };

        const String label =
            PrettifyPropertyName(StringView(reinterpret_cast<const utf8char*>(prop.name)));
        auto listEditor = MakeRef<ContainerListEditor>(DefaultAllocator(), label.AsView(), category);
        ContainerListEditor* rawList = listEditor.Get();
        // "description" property attribute -> the list's hover tooltip (the reflection-consistent way
        // to carry help text, e.g. the mesh material slot-0 / submesh semantics).
        if (const foundation::Attribute* description = FindAttribute(prop, u8"description"))
        {
            if (const String* text = description->value.TryGet<String>())
            {
                rawList->SetTooltip(text->AsView());
            }
        }
        rawList->slotNames = computeNames();

        // Add a default element (homogeneous). The polymorphic add-by-type menu is the next pass.
        rawList->OnAdd = [self, id, type, propPtr]()
        {
            self->MutateComponent(id, type,
                                  [propPtr](const Instance& comp)
                                  {
                                      const Instance container(propPtr->address(comp), propPtr->type);
                                      const ContainerInfo& ci = *propPtr->type->container;
                                      (void)ContainerEmplaceDefault(ci, container,
                                                                    ContainerSize(ci, container));
                                  });
            self->m_forceRebuild = true;
        };
        rawList->OnRemoveSlot = [self, id, type, propPtr](usize i)
        {
            self->MutateComponent(id, type,
                                  [propPtr, i](const Instance& comp)
                                  {
                                      const Instance container(propPtr->address(comp), propPtr->type);
                                      (void)ContainerRemoveAt(*propPtr->type->container, container, i);
                                  });
            self->m_forceRebuild = true;
        };
        rawList->OnMoveSlot = [self, id, type, propPtr](usize i, bool up)
        {
            self->MutateComponent(id, type,
                                  [propPtr, i, up](const Instance& comp)
                                  {
                                      const Instance container(propPtr->address(comp), propPtr->type);
                                      const ContainerInfo& ci = *propPtr->type->container;
                                      const usize n = ContainerSize(ci, container);
                                      if (up && i > 0)
                                      {
                                          (void)ContainerMoveElement(ci, container, i, i - 1);
                                      }
                                      else if (!up && i + 1 < n)
                                      {
                                          (void)ContainerMoveElement(ci, container, i, i + 1);
                                      }
                                  });
            self->m_forceRebuild = true;
        };
        // Pick opens the type-filtered asset picker for this slot (Material for now).
        rawList->OnPickSlot = [self, id, type, propPtr](usize i)
        {
            if (self->Context == nullptr || self->m_editor->Project() == nullptr)
            {
                return;
            }
            Array<String> typeNames;
            typeNames.PushBack(String(u8"MaterialAsset"));
            auto dialog = MakeRef<draconic::editor::app::AssetPickerDialog>(
                DefaultAllocator(), *self->m_editor, Move(typeNames));
            dialog->OnPicked = [self, id, type, propPtr, i](const Guid& target)
            {
                self->MutateComponent(
                    id, type,
                    [propPtr, i, target](const Instance& comp)
                    {
                        const Instance container(propPtr->address(comp), propPtr->type);
                        const ContainerInfo& ci = *propPtr->type->container;
                        if (i >= ContainerSize(ci, container))
                        {
                            return;
                        }
                        const Instance el = ContainerAddressAt(ci, container, i);
                        if (el.Pointer() != nullptr && el.Type() == &TypeOf<MatRef>())
                        {
                            MatRef* r = static_cast<MatRef*>(el.Pointer());
                            *r = MatRef{};
                            r->SetId(target);
                        }
                    });
                self->m_forceRebuild = true;
            };
            dialog->Show(self->Context);
        };

        // One grid row for the whole property; the refresher recomputes the slot text and forces a
        // rebuild when the list changes (count/content) - e.g. from undo/redo, which Signature() misses.
        AddEditor(rawList,
                  [self, rawList, computeNames]()
                  {
                      const Array<String> names = computeNames();
                      if (names.Size() != rawList->slotNames.Size())
                      {
                          self->m_forceRebuild = true;
                          return;
                      }
                      for (usize k = 0; k < names.Size(); ++k)
                      {
                          if (names[k] != rawList->slotNames[k])
                          {
                              self->m_forceRebuild = true;
                              return;
                          }
                      }
                  });
    }

    Float3 SceneInspectorView::EulerDegrees(Quaternion q)
    {
        f32 yaw = 0, pitch = 0, roll = 0;
        ToYawPitchRoll(q, yaw, pitch, roll);
        return Float3{RadiansToDegrees(pitch), RadiansToDegrees(yaw), RadiansToDegrees(roll)};
    }

    void SceneInspectorView::ShowAddComponentMenu()
    {
        const Guid id = SelectedEntity();
        const scene::EntityHandle e = m_edit->Resolve(id);
        if (!e.IsAssigned() || Context == nullptr)
        {
            return;
        }

        SceneEditContext* edit = m_edit;
        auto menu = MakeRef<ui::ContextMenu>(DefaultAllocator());

        // Category submenus with authored display names (editor-polish.md P1) - not the
        // flat raw-type-name dump this used to be. Categories and items sort
        // alphabetically so placement is stable as subsystems register.
        struct Entry
        {
            String label;
            StringView category;
            const TypeInfo* type = nullptr;
        };
        Array<Entry> entries;
        m_edit->Scene().ForEachManager(
            [&](scene::ComponentManagerBase& mgr)
            {
                const TypeInfo* type = mgr.ComponentType();
                if (!IsRegisteredType(type) || mgr.HasComponent(e))
                {
                    return;
                } // skip unreflected
                entries.PushBack(Entry{ComponentDisplayName(type), ComponentCategory(type), type});
            });
        const auto viewLess = [](StringView a, StringView b)
        {
            const usize n = Min(a.Size(), b.Size());
            for (usize k = 0; k < n; ++k)
            {
                if (a[k] != b[k])
                {
                    return static_cast<u8>(a[k]) < static_cast<u8>(b[k]);
                }
            }
            return a.Size() < b.Size();
        };
        for (usize i = 1; i < entries.Size(); ++i) // insertion sort: category, then label
        {
            for (usize j = i; j > 0; --j)
            {
                const bool before =
                    viewLess(entries[j].category, entries[j - 1].category) ||
                    (entries[j].category == entries[j - 1].category &&
                     viewLess(entries[j].label.AsView(), entries[j - 1].label.AsView()));
                if (!before)
                {
                    break;
                }
                Swap(entries[j], entries[j - 1]);
            }
        }
        ui::ContextMenu* section = nullptr;
        StringView sectionName;
        for (const Entry& entry : entries)
        {
            if (section == nullptr || entry.category != sectionName)
            {
                ui::MenuItem* item = menu->AddSubmenu(entry.category);
                section = Cast<ui::ContextMenu>(item->Submenu.Get());
                sectionName = entry.category;
            }
            if (section != nullptr)
            {
                const TypeInfo* type = entry.type;
                section->AddItem(entry.label.AsView(),
                                 [edit, id, type]() { edit->AddComponent(id, type); });
            }
        }
        // (Paste lives on the dedicated Paste Component button now - it confirms before overwriting.)
        const Float2 screenPos = m_addButton->LocalToScreen(Float2{0.0f, 0.0f});
        menu->Show(Context, screenPos.x, screenPos.y);
    }

    void SceneInspectorView::UpdatePasteButton()
    {
        if (!m_pasteButton || m_editor == nullptr)
        {
            return;
        }
        const bool hasComponent = !m_editor->ClipboardData(u8"component").IsEmpty();
        const ui::Visibility want = hasComponent ? ui::Visibility::Visible : ui::Visibility::Gone;
        if (m_pasteButton->Visibility != want)
        {
            m_pasteButton->Visibility = want;
            Invalidate();
        }
    }

    void SceneInspectorView::PasteSelectedComponent()
    {
        const Guid id = SelectedEntity();
        const scene::EntityHandle e = m_edit->Resolve(id);
        if (!e.IsAssigned() || Context == nullptr || m_editor == nullptr)
        {
            return;
        }
        const Span<const byte> clip = m_editor->ClipboardData(u8"component");
        if (clip.IsEmpty())
        {
            return;
        }

        // If the entity already has this component type, pasting OVERWRITES it - confirm first (still
        // undoable). Otherwise paste straight away.
        const StringView typeId = SceneEditContext::PeekComponentTypeId(clip);
        scene::ComponentManagerBase* mgr = m_edit->Scene().FindManagerBySerializationId(typeId);
        if (mgr != nullptr && mgr->HasComponent(e))
        {
            String message(u8"This entity already has a ");
            message += ComponentDisplayName(mgr->ComponentType());
            message += StringView(u8" component. Pasting overwrites it (you can undo). Continue?");
            RefPtr<ui::Dialog> dialog =
                ui::Dialog::Confirm(StringView(u8"Overwrite Component?"), message.AsView());
            SceneInspectorView* self = this;
            dialog->OnClosed.Add(
                [self, id](ui::Dialog*, ui::DialogResult result)
                {
                    if (result == ui::DialogResult::OK && self->m_editor != nullptr)
                    {
                        (void)self->m_edit->PasteComponent(
                            id, self->m_editor->ClipboardData(u8"component"));
                    }
                });
            dialog->Show(Context);
            return;
        }
        (void)m_edit->PasteComponent(id, clip);
    }
}
