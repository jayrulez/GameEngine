// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI Toolkit - :range_editor partition
//
// Range/slider property editor - a Slider + NumericField side by side (both synced). Ported from
// Sedulous.UI.Toolkit/src/PropertyGrid/RangeEditor.bf. Beef `float` -> f32; `delegate void(float) Setter`
// -> Function<void(f32)>; Slider Min/Max/Step/Value `.Value =` -> `.SetValue(...)`; `new
// FlexLayout.LayoutParams()` -> LayoutStyle; `.Fixed(.Px(w))` -> SizeSpec::Fixed(Unit::Dp(w));
// `Math.Log10` -> std::log10. The private inner `RangeNumericField : NumericField` becomes a PUBLIC nested
// class (own RTTI_OBJECT identity) whose focus overrides are defined out-of-line after RangeEditor is
// complete.

module;
#include <cmath>
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.ui.toolkit:range_editor;

import foundation.core;
import foundation.vg;
import foundation.ui;
import :property_editor;

using namespace foundation::core;
namespace core = foundation::core;

export namespace foundation::ui::toolkit
{
    /// Range/slider property editor - Slider + NumericField side by side. Both synced.
    class RangeEditor : public PropertyEditor
    {
        RTTI_OBJECT(RangeEditor, PropertyEditor)
    public:
        Function<void(f32)> Setter;

        RangeEditor(StringView name, f32 initialValue, f32 min = 0, f32 max = 1, f32 step = 0,
                    Function<void(f32)> setter = {}, StringView category = {})
            : PropertyEditor(name, category), Setter(Move(setter)), m_value(initialValue),
              m_min(min), m_max(max), m_step(step)
        {
        }

        [[nodiscard]] f32 Value() const noexcept { return m_value; }
        void SetValue(f32 value)
        {
            m_value = value;
            if (!m_syncing)
            {
                RefreshView();
            }
        }

        void RefreshView() override
        {
            if (!m_syncing)
            {
                m_syncing = true;
                if (m_slider != nullptr)
                {
                    m_slider->Value.SetValue(m_value);
                }
                if (m_numericField != nullptr)
                {
                    m_numericField->SetValue(m_value);
                }
                m_syncing = false;
            }
        }

        /// NumericField subclass that tracks edit transactions via focus.
        class RangeNumericField : public NumericField
        {
            RTTI_OBJECT(RangeNumericField, NumericField)
        public:
            explicit RangeNumericField(RangeEditor* editor) : m_editor(editor) {}

            void OnFocusGained() override;
            void OnFocusLost() override;

        private:
            RangeEditor* m_editor;
        };

    protected:
        RefPtr<View> CreateEditorView() override
        {
            RefPtr<FlexLayout> row = MakeRef<FlexLayout>(MemoryAllocator());
            row->Direction = Orientation::Horizontal;
            row->Spacing = 4.0f;

            // Slider (fills available space).
            RefPtr<Slider> slider = MakeRef<Slider>(MemoryAllocator());
            m_slider = slider.Get();
            m_slider->Min.SetValue(m_min);
            m_slider->Max.SetValue(m_max);
            m_slider->Step.SetValue(m_step);
            m_slider->Value.SetValue(m_value);
            RangeEditor* self = this;
            m_slider->OnDragStarted.Add([self](Slider*) { self->BeginEdit(); });
            m_slider->OnValueChanged.Add(
                [self](Slider*, f32 val)
                {
                    if (!self->m_syncing)
                    {
                        self->m_syncing = true;
                        self->m_value = val;
                        if (self->m_numericField != nullptr)
                        {
                            self->m_numericField->SetValue(val);
                        }
                        if (self->Setter)
                        {
                            self->Setter(val);
                        }
                        self->NotifyValueChanged();
                        self->m_syncing = false;
                    }
                });
            m_slider->OnDragEnded.Add([self](Slider*) { self->EndEdit(); });
            {
                LayoutStyle lp;
                lp.Width = SizeSpec::Wrap();
                lp.Height = SizeSpec::Match();
                lp.FlexGrow = 1.0f;
                row->AddView(slider.Get(), lp);
            }

            // NumericField (fixed width for precise input).
            RefPtr<RangeNumericField> field = MakeRef<RangeNumericField>(MemoryAllocator(), this);
            field->AddClass(u8"property-field");
            m_numericField = field.Get();
            m_numericField->SetMin(m_min);
            m_numericField->SetMax(m_max);
            m_numericField->SetStep((m_step > 0) ? m_step : 0.1);
            m_numericField->SetDecimalPlaces(2);
            m_numericField->SetValue(m_value);
            m_numericField->OnValueChanged.Add(
                [self](NumericField*, f64 val)
                {
                    if (!self->m_syncing)
                    {
                        self->m_syncing = true;
                        self->m_value = static_cast<f32>(val);
                        if (self->m_slider != nullptr)
                        {
                            self->m_slider->Value.SetValue(static_cast<f32>(val));
                        }
                        if (self->Setter)
                        {
                            self->Setter(static_cast<f32>(val));
                        }
                        self->NotifyValueChanged();
                        self->m_syncing = false;
                    }
                });
            {
                LayoutStyle lp;
                lp.Width = SizeSpec::Fixed(Unit::Dp(ComputeNumericFieldWidth()));
                lp.Height = SizeSpec::Match();
                row->AddView(field.Get(), lp);
            }

            return row;
        }

    private:
        /// Worst-case rendered width for the numeric readout. Picks the longer of |Min| and Max, computes
        /// integer digits + dot + decimals, adds a sign slot if the range can go negative, plus padding.
        [[nodiscard]] f32 ComputeNumericFieldWidth() const
        {
            const f32 absMax = core::Max(Abs(m_min), Abs(m_max));
            i32 intDigits = (absMax >= 1) ? static_cast<i32>(std::log10(absMax)) + 1 : 1;
            i32 chars = intDigits + 1 + 2; // int + dot + 2 decimals
            if (m_min < 0)
            {
                chars++;
            } // negative sign
            chars = core::Min(chars, 6);
            return core::Max(60.0f, static_cast<f32>(chars) * 12.0f + 16.0f);
        }

        f32 m_value;
        f32 m_min;
        f32 m_max;
        f32 m_step;
        Slider* m_slider = nullptr;             // borrowed; the row tree owns it
        NumericField* m_numericField = nullptr; // borrowed; the row tree owns it
        bool m_syncing = false;
    };

    // === Inner-view out-of-line bodies (need the complete RangeEditor type) ===

    inline void RangeEditor::RangeNumericField::OnFocusGained()
    {
        NumericField::OnFocusGained();
        if (!m_editor->IsEditing())
        {
            m_editor->BeginEdit();
        }
    }

    inline void RangeEditor::RangeNumericField::OnFocusLost()
    {
        NumericField::OnFocusLost();
        if (m_editor->IsEditing())
        {
            m_editor->EndEdit();
        }
    }

    RTTI_DEFINE_OBJECT(RangeEditor, "rtti::ui::toolkit")
    RTTI_DEFINE_OBJECT(RangeEditor::RangeNumericField, "rtti::ui::toolkit")
}
