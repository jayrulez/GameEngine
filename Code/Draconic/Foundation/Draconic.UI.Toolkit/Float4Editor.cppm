// Draconic UI Toolkit - :float4_editor partition
//
// Property editor for Float4 values - four NumericFields (X, Y, Z, W) with colored axis labels.
// A mechanical sibling of Float3Editor (the faithful Sedulous port); grown here for the editor
// inspector's Float4 component properties (e.g. sprite uvRect).

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.ui.toolkit:float4_editor;

import draconic.foundation;
import draconic.vg;
import draconic.ui;
import :property_editor;
import :vector_fields; // reuse AxisLabel + AxisColors

using namespace draconic::foundation;

export namespace draconic::ui::toolkit
{
    /// Property editor for Float4 values. Four NumericFields (X, Y, Z, W) side by side with axis labels.
    class Float4Editor : public PropertyEditor
    {
        DRACONIC_OBJECT(Float4Editor, PropertyEditor)
    public:
        Function<void(Float4)> Setter;

        Float4Editor(StringView name, Float4 value, f32 min = -100000, f32 max = 100000,
                     f32 step = 0.1f, Function<void(Float4)> setter = {}, StringView category = {})
            : PropertyEditor(name, category), Setter(Move(setter)), m_value(value), m_min(min),
              m_max(max), m_step(step)
        {
        }

        [[nodiscard]] Float4 Value() const noexcept { return m_value; }
        void SetValue(Float4 value)
        {
            m_value = value;
            if (!m_syncing)
            {
                RefreshView();
            }
        }

        void RefreshView() override
        {
            if (m_xField != nullptr && !m_syncing)
            {
                m_syncing = true;
                m_xField->SetValue(m_value.x);
                m_yField->SetValue(m_value.y);
                m_zField->SetValue(m_value.z);
                m_wField->SetValue(m_value.w);
                m_syncing = false;
            }
        }

        /// NumericField subclass that tracks edit transactions via focus.
        class VectorNumericField : public NumericField
        {
            DRACONIC_OBJECT(VectorNumericField, NumericField)
        public:
            VectorNumericField(Float4Editor* editor, i32 axis) : m_editor(editor), m_axis(axis) {}

            void OnFocusGained() override;
            void OnFocusLost() override;

        private:
            Float4Editor* m_editor;
            [[maybe_unused]] i32 m_axis; // axis index (kept for parity with the Beef inner field)
        };

    protected:
        RefPtr<View> CreateEditorView() override
        {
            RefPtr<FlexLayout> row = MakeRef<FlexLayout>(DefaultAllocator());
            row->Direction = Orientation::Horizontal;
            row->Spacing = 4.0f;

            Float4Editor* self = this;

            RefPtr<VectorNumericField> x =
                MakeField(0, StringView(u8"X"), AxisColors::X, m_value.x);
            m_xField = x.Get();
            m_xField->OnValueChanged.Add(
                [self](NumericField*, f64 val)
                {
                    if (!self->m_syncing)
                    {
                        self->m_syncing = true;
                        self->m_value.x = static_cast<f32>(val);
                        if (self->Setter)
                        {
                            self->Setter(self->m_value);
                        }
                        self->NotifyValueChanged();
                        self->m_syncing = false;
                    }
                });
            row->AddView(x.Get(), GrowParams());

            RefPtr<VectorNumericField> y =
                MakeField(1, StringView(u8"Y"), AxisColors::Y, m_value.y);
            m_yField = y.Get();
            m_yField->OnValueChanged.Add(
                [self](NumericField*, f64 val)
                {
                    if (!self->m_syncing)
                    {
                        self->m_syncing = true;
                        self->m_value.y = static_cast<f32>(val);
                        if (self->Setter)
                        {
                            self->Setter(self->m_value);
                        }
                        self->NotifyValueChanged();
                        self->m_syncing = false;
                    }
                });
            row->AddView(y.Get(), GrowParams());

            RefPtr<VectorNumericField> z =
                MakeField(2, StringView(u8"Z"), AxisColors::Z, m_value.z);
            m_zField = z.Get();
            m_zField->OnValueChanged.Add(
                [self](NumericField*, f64 val)
                {
                    if (!self->m_syncing)
                    {
                        self->m_syncing = true;
                        self->m_value.z = static_cast<f32>(val);
                        if (self->Setter)
                        {
                            self->Setter(self->m_value);
                        }
                        self->NotifyValueChanged();
                        self->m_syncing = false;
                    }
                });
            row->AddView(z.Get(), GrowParams());

            RefPtr<VectorNumericField> w =
                MakeField(3, StringView(u8"W"), AxisColors::W, m_value.w);
            m_wField = w.Get();
            m_wField->OnValueChanged.Add(
                [self](NumericField*, f64 val)
                {
                    if (!self->m_syncing)
                    {
                        self->m_syncing = true;
                        self->m_value.w = static_cast<f32>(val);
                        if (self->Setter)
                        {
                            self->Setter(self->m_value);
                        }
                        self->NotifyValueChanged();
                        self->m_syncing = false;
                    }
                });
            row->AddView(w.Get(), GrowParams());

            return row;
        }

    private:
        RefPtr<VectorNumericField> MakeField(i32 axis, StringView axisText, Color axisColor,
                                             f32 initial)
        {
            RefPtr<VectorNumericField> f =
                MakeRef<VectorNumericField>(DefaultAllocator(), this, axis);
            f->AddClass(u8"property-field");
            f->ShowSpinButtons.SetValue(false);
            f->SetMin(m_min);
            f->SetMax(m_max);
            f->SetStep(m_step);
            f->SetDecimalPlaces(3);
            f->SetValue(initial);
            RefPtr<AxisLabel> label = MakeRef<AxisLabel>(DefaultAllocator(), axisText, axisColor);
            f->SetPrefix(label.Get());
            return f;
        }

        static RefPtr<FlexLayoutParams> GrowParams()
        {
            RefPtr<FlexLayoutParams> lp = MakeRef<FlexLayoutParams>(DefaultAllocator());
            lp->Grow = 1.0f;
            return lp;
        }

        Float4 m_value;
        f32 m_min;
        f32 m_max;
        f32 m_step;
        NumericField* m_xField = nullptr; // borrowed; the row tree owns them
        NumericField* m_yField = nullptr;
        NumericField* m_zField = nullptr;
        NumericField* m_wField = nullptr;
        bool m_syncing = false;
    };

    // === Inner-view out-of-line bodies (need the complete Float4Editor type) ===

    inline void Float4Editor::VectorNumericField::OnFocusGained()
    {
        NumericField::OnFocusGained();
        if (!m_editor->IsEditing())
        {
            m_editor->BeginEdit();
        }
    }

    inline void Float4Editor::VectorNumericField::OnFocusLost()
    {
        NumericField::OnFocusLost();
        if (m_editor->IsEditing())
        {
            m_editor->EndEdit();
        }
    }

    DRACONIC_DEFINE_OBJECT(Float4Editor, "draconic::ui::toolkit")
    DRACONIC_DEFINE_OBJECT(Float4Editor::VectorNumericField, "draconic::ui::toolkit")
}
