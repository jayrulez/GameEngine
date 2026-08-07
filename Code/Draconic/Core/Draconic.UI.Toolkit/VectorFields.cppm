// Draconic UI Toolkit - :vector_fields partition
//
// Standalone multi-component numeric fields for editing Float2/Float3/Float4/Quaternion outside the
// PropertyGrid framework. Each is a horizontal FlexLayout of one NumericField per component with the same
// colored axis labels the property-grid editors use (X=red, Y=green, Z=blue, W=gold). Ported from
// Sedulous.UI.Toolkit/src/VectorFields.bf.
//
// Beef `AxisLabel` (View) + `AggregatingVectorField` (abstract FlexLayout) + the concrete field classes all
// live in one partition. Beef Vector2/3/4 -> Float2/Float3/Float4 (.x/.y/.z/.w); Quaternion -> core
// Quaternion; `SetPrefix(new AxisLabel(...))` -> SetPrefix(View*) (adopts a RefPtr). Byte `Color(r,g,b,a)`
// axis colors -> constexpr Color via /255. The deferred edit-end goes through Context->MutationQueueRef().

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.ui.toolkit:vector_fields;

import draconic.foundation;
import draconic.vg;
import draconic.fonts;
import draconic.ui;

using namespace draconic::foundation;
namespace fonts = draconic::fonts;

export namespace draconic::ui::toolkit
{
    /// Colored single-character axis label used as a NumericField prefix.
    class AxisLabel : public View
    {
        DRACONIC_OBJECT(AxisLabel, View)
    public:
        AxisLabel(StringView text, Color color) : m_text(text), m_color(color) {}

        void OnDraw(UIDrawContext& ctx) override
        {
            if (ctx.FontService() == nullptr)
            {
                return;
            }
            if (fonts::CachedFont* font = ctx.FontService()->GetFont(11.0f))
            {
                ctx.VG().DrawText(m_text, font, Rectangle{0, 0, Width(), Height()},
                                  fonts::TextAlignment::Center, fonts::VerticalAlignment::Middle,
                                  m_color);
            }
        }

    protected:
        void OnMeasure(BoxConstraints constraints) override
        {
            f32 w = 12.0f, h = 14.0f;
            if (Context != nullptr && Context->FontService() != nullptr)
            {
                if (fonts::CachedFont* font = Context->FontService()->GetFont(11.0f))
                {
                    w = font->font->MeasureString(m_text);
                    h = font->font->Metrics().lineHeight;
                }
            }
            MeasuredSize = Float2{constraints.ConstrainWidth(w), constraints.ConstrainHeight(h)};
        }

    private:
        String m_text;
        Color m_color;
    };

    /// Standard axis colors used by all FloatN editor fields (match the property-grid scheme).
    struct AxisColors
    {
        static constexpr Color X{220 / 255.0f, 80 / 255.0f, 80 / 255.0f, 1.0f};
        static constexpr Color Y{80 / 255.0f, 200 / 255.0f, 80 / 255.0f, 1.0f};
        static constexpr Color Z{80 / 255.0f, 120 / 255.0f, 220 / 255.0f, 1.0f};
        static constexpr Color W{200 / 255.0f, 180 / 255.0f, 120 / 255.0f, 1.0f};
    };

    /// Shared aggregation: counts child-field edit transactions and fires one Begin / End on the parent.
    /// The End is deferred through the UI MutationQueue so a focus jump between sibling fields produces a
    /// single edit transaction.
    class AggregatingVectorField : public FlexLayout
    {
        DRACONIC_OBJECT(AggregatingVectorField, FlexLayout)
    public:
        Event<void(AggregatingVectorField*)> OnEditBegan;
        Event<void(AggregatingVectorField*)> OnEditEnded;

    protected:
        static RefPtr<FlexLayoutParams> GrowMatchParams()
        {
            RefPtr<FlexLayoutParams> lp = MakeRef<FlexLayoutParams>(DefaultAllocator());
            lp->Grow = 1.0f;
            lp->Height = SizeSpec::Match();
            return lp;
        }

        static RefPtr<NumericField> MakeField(StringView axisText, Color axisColor)
        {
            RefPtr<NumericField> f = MakeRef<NumericField>(DefaultAllocator());
            f->AddClass(u8"property-field");
            f->ShowSpinButtons.SetValue(false);
            f->SetMin(-1e6);
            f->SetMax(1e6);
            f->SetStep(0.1);
            f->SetDecimalPlaces(3);
            RefPtr<AxisLabel> label = MakeRef<AxisLabel>(DefaultAllocator(), axisText, axisColor);
            f->SetPrefix(label.Get());
            return f;
        }

        void WireChildEditEvents(NumericField* nf)
        {
            AggregatingVectorField* self = this;
            nf->OnEditBegan.Add(
                [self](NumericField*)
                {
                    self->m_pendingEnd = false;
                    if (self->m_editCount == 0)
                    {
                        self->OnEditBegan.Invoke(self);
                    }
                    self->m_editCount++;
                });
            nf->OnEditEnded.Add(
                [self](NumericField*)
                {
                    self->m_editCount--;
                    if (self->m_editCount == 0)
                    {
                        self->m_pendingEnd = true;
                        auto* ctx = self->Context;
                        if (ctx == nullptr)
                        {
                            self->m_pendingEnd = false;
                            self->OnEditEnded.Invoke(self);
                            return;
                        }
                        ctx->MutationQueueRef().QueueAction(
                            [self]()
                            {
                                if (self->m_pendingEnd)
                                {
                                    self->m_pendingEnd = false;
                                    self->OnEditEnded.Invoke(self);
                                }
                            });
                    }
                });
        }

    private:
        i32 m_editCount = 0;
        bool m_pendingEnd = false;
    };

    /// Standalone Float2 input. Two NumericFields with colored X/Y labels.
    class Vector2Field : public AggregatingVectorField
    {
        DRACONIC_OBJECT(Vector2Field, AggregatingVectorField)
    public:
        Event<void(Float2)> OnValueChanged;

        [[nodiscard]] Float2 Value() const { return m_value; }
        void SetValue(Float2 value)
        {
            m_value = value;
            SyncToFields();
        }

        Vector2Field()
        {
            Direction = Orientation::Horizontal;
            Spacing = 4.0f;

            RefPtr<NumericField> x = MakeField(StringView(u8"X"), AxisColors::X);
            RefPtr<NumericField> y = MakeField(StringView(u8"Y"), AxisColors::Y);
            m_x = x.Get();
            m_y = y.Get();

            Vector2Field* self = this;
            m_x->OnValueChanged.Add(
                [self](NumericField*, f64 v)
                {
                    if (!self->m_syncing)
                    {
                        self->m_value.x = static_cast<f32>(v);
                        self->OnValueChanged.Invoke(self->m_value);
                    }
                });
            m_y->OnValueChanged.Add(
                [self](NumericField*, f64 v)
                {
                    if (!self->m_syncing)
                    {
                        self->m_value.y = static_cast<f32>(v);
                        self->OnValueChanged.Invoke(self->m_value);
                    }
                });

            WireChildEditEvents(m_x);
            WireChildEditEvents(m_y);

            AddView(m_x, GrowMatchParams());
            AddView(m_y, GrowMatchParams());
        }

        void SetRange(f64 min, f64 max)
        {
            m_x->SetMin(min);
            m_x->SetMax(max);
            m_y->SetMin(min);
            m_y->SetMax(max);
        }
        [[nodiscard]] f64 Step() const { return m_x->Step(); }
        void SetStep(f64 value)
        {
            m_x->SetStep(value);
            m_y->SetStep(value);
        }
        [[nodiscard]] i32 DecimalPlaces() const { return m_x->DecimalPlaces(); }
        void SetDecimalPlaces(i32 value)
        {
            m_x->SetDecimalPlaces(value);
            m_y->SetDecimalPlaces(value);
        }
        [[nodiscard]] bool ShowSpinButtons() const { return m_x->ShowSpinButtons.Value(); }
        void SetShowSpinButtons(bool value)
        {
            m_x->ShowSpinButtons.SetValue(value);
            m_y->ShowSpinButtons.SetValue(value);
        }

    private:
        void SyncToFields()
        {
            if (m_syncing)
            {
                return;
            }
            m_syncing = true;
            m_x->SetValue(m_value.x);
            m_y->SetValue(m_value.y);
            m_syncing = false;
        }

        NumericField* m_x = nullptr;
        NumericField* m_y = nullptr;
        Float2 m_value{0, 0};
        bool m_syncing = false;
    };

    /// Standalone Float3 input. Three NumericFields with colored X/Y/Z labels.
    class Vector3Field : public AggregatingVectorField
    {
        DRACONIC_OBJECT(Vector3Field, AggregatingVectorField)
    public:
        Event<void(Float3)> OnValueChanged;

        [[nodiscard]] Float3 Value() const { return m_value; }
        void SetValue(Float3 value)
        {
            m_value = value;
            SyncToFields();
        }

        Vector3Field()
        {
            Direction = Orientation::Horizontal;
            Spacing = 4.0f;

            RefPtr<NumericField> x = MakeField(StringView(u8"X"), AxisColors::X);
            RefPtr<NumericField> y = MakeField(StringView(u8"Y"), AxisColors::Y);
            RefPtr<NumericField> z = MakeField(StringView(u8"Z"), AxisColors::Z);
            m_x = x.Get();
            m_y = y.Get();
            m_z = z.Get();

            Vector3Field* self = this;
            m_x->OnValueChanged.Add(
                [self](NumericField*, f64 v)
                {
                    if (!self->m_syncing)
                    {
                        self->m_value.x = static_cast<f32>(v);
                        self->OnValueChanged.Invoke(self->m_value);
                    }
                });
            m_y->OnValueChanged.Add(
                [self](NumericField*, f64 v)
                {
                    if (!self->m_syncing)
                    {
                        self->m_value.y = static_cast<f32>(v);
                        self->OnValueChanged.Invoke(self->m_value);
                    }
                });
            m_z->OnValueChanged.Add(
                [self](NumericField*, f64 v)
                {
                    if (!self->m_syncing)
                    {
                        self->m_value.z = static_cast<f32>(v);
                        self->OnValueChanged.Invoke(self->m_value);
                    }
                });

            WireChildEditEvents(m_x);
            WireChildEditEvents(m_y);
            WireChildEditEvents(m_z);

            AddView(m_x, GrowMatchParams());
            AddView(m_y, GrowMatchParams());
            AddView(m_z, GrowMatchParams());
        }

        void SetRange(f64 min, f64 max)
        {
            m_x->SetMin(min);
            m_x->SetMax(max);
            m_y->SetMin(min);
            m_y->SetMax(max);
            m_z->SetMin(min);
            m_z->SetMax(max);
        }
        [[nodiscard]] f64 Step() const { return m_x->Step(); }
        void SetStep(f64 value)
        {
            m_x->SetStep(value);
            m_y->SetStep(value);
            m_z->SetStep(value);
        }
        [[nodiscard]] i32 DecimalPlaces() const { return m_x->DecimalPlaces(); }
        void SetDecimalPlaces(i32 value)
        {
            m_x->SetDecimalPlaces(value);
            m_y->SetDecimalPlaces(value);
            m_z->SetDecimalPlaces(value);
        }
        [[nodiscard]] bool ShowSpinButtons() const { return m_x->ShowSpinButtons.Value(); }
        void SetShowSpinButtons(bool value)
        {
            m_x->ShowSpinButtons.SetValue(value);
            m_y->ShowSpinButtons.SetValue(value);
            m_z->ShowSpinButtons.SetValue(value);
        }

    private:
        void SyncToFields()
        {
            if (m_syncing)
            {
                return;
            }
            m_syncing = true;
            m_x->SetValue(m_value.x);
            m_y->SetValue(m_value.y);
            m_z->SetValue(m_value.z);
            m_syncing = false;
        }

        NumericField* m_x = nullptr;
        NumericField* m_y = nullptr;
        NumericField* m_z = nullptr;
        Float3 m_value{0, 0, 0};
        bool m_syncing = false;
    };

    /// Standalone Float4 input. Four NumericFields with colored X/Y/Z/W labels.
    class Vector4Field : public AggregatingVectorField
    {
        DRACONIC_OBJECT(Vector4Field, AggregatingVectorField)
    public:
        Event<void(Float4)> OnValueChanged;

        [[nodiscard]] Float4 Value() const { return m_value; }
        void SetValue(Float4 value)
        {
            m_value = value;
            SyncToFields();
        }

        Vector4Field()
        {
            Direction = Orientation::Horizontal;
            Spacing = 4.0f;

            RefPtr<NumericField> x = MakeField(StringView(u8"X"), AxisColors::X);
            RefPtr<NumericField> y = MakeField(StringView(u8"Y"), AxisColors::Y);
            RefPtr<NumericField> z = MakeField(StringView(u8"Z"), AxisColors::Z);
            RefPtr<NumericField> w = MakeField(StringView(u8"W"), AxisColors::W);
            m_x = x.Get();
            m_y = y.Get();
            m_z = z.Get();
            m_w = w.Get();

            Vector4Field* self = this;
            m_x->OnValueChanged.Add(
                [self](NumericField*, f64 v)
                {
                    if (!self->m_syncing)
                    {
                        self->m_value.x = static_cast<f32>(v);
                        self->OnValueChanged.Invoke(self->m_value);
                    }
                });
            m_y->OnValueChanged.Add(
                [self](NumericField*, f64 v)
                {
                    if (!self->m_syncing)
                    {
                        self->m_value.y = static_cast<f32>(v);
                        self->OnValueChanged.Invoke(self->m_value);
                    }
                });
            m_z->OnValueChanged.Add(
                [self](NumericField*, f64 v)
                {
                    if (!self->m_syncing)
                    {
                        self->m_value.z = static_cast<f32>(v);
                        self->OnValueChanged.Invoke(self->m_value);
                    }
                });
            m_w->OnValueChanged.Add(
                [self](NumericField*, f64 v)
                {
                    if (!self->m_syncing)
                    {
                        self->m_value.w = static_cast<f32>(v);
                        self->OnValueChanged.Invoke(self->m_value);
                    }
                });

            WireChildEditEvents(m_x);
            WireChildEditEvents(m_y);
            WireChildEditEvents(m_z);
            WireChildEditEvents(m_w);

            AddView(m_x, GrowMatchParams());
            AddView(m_y, GrowMatchParams());
            AddView(m_z, GrowMatchParams());
            AddView(m_w, GrowMatchParams());
        }

        void SetRange(f64 min, f64 max)
        {
            m_x->SetMin(min);
            m_x->SetMax(max);
            m_y->SetMin(min);
            m_y->SetMax(max);
            m_z->SetMin(min);
            m_z->SetMax(max);
            m_w->SetMin(min);
            m_w->SetMax(max);
        }
        [[nodiscard]] f64 Step() const { return m_x->Step(); }
        void SetStep(f64 value)
        {
            m_x->SetStep(value);
            m_y->SetStep(value);
            m_z->SetStep(value);
            m_w->SetStep(value);
        }
        [[nodiscard]] i32 DecimalPlaces() const { return m_x->DecimalPlaces(); }
        void SetDecimalPlaces(i32 value)
        {
            m_x->SetDecimalPlaces(value);
            m_y->SetDecimalPlaces(value);
            m_z->SetDecimalPlaces(value);
            m_w->SetDecimalPlaces(value);
        }
        [[nodiscard]] bool ShowSpinButtons() const { return m_x->ShowSpinButtons.Value(); }
        void SetShowSpinButtons(bool value)
        {
            m_x->ShowSpinButtons.SetValue(value);
            m_y->ShowSpinButtons.SetValue(value);
            m_z->ShowSpinButtons.SetValue(value);
            m_w->ShowSpinButtons.SetValue(value);
        }

    private:
        void SyncToFields()
        {
            if (m_syncing)
            {
                return;
            }
            m_syncing = true;
            m_x->SetValue(m_value.x);
            m_y->SetValue(m_value.y);
            m_z->SetValue(m_value.z);
            m_w->SetValue(m_value.w);
            m_syncing = false;
        }

        NumericField* m_x = nullptr;
        NumericField* m_y = nullptr;
        NumericField* m_z = nullptr;
        NumericField* m_w = nullptr;
        Float4 m_value{0, 0, 0, 0};
        bool m_syncing = false;
    };

    /// Standalone Quaternion input that exposes Euler angles (degrees) for editing. The quaternion is the
    /// source of truth (Value get/set), but the user manipulates three Euler fields (X=pitch, Y=yaw,
    /// Z=roll). The last typed Eulers are cached so displayed angles stay stable during editing.
    class QuaternionField : public AggregatingVectorField
    {
        DRACONIC_OBJECT(QuaternionField, AggregatingVectorField)
    public:
        Event<void(Quaternion)> OnValueChanged;

        [[nodiscard]] Quaternion Value() const { return m_value; }
        void SetValue(Quaternion value)
        {
            m_value = value;
            m_eulerDegrees = QuaternionToEulerDegrees(value);
            SyncToFields();
        }

        QuaternionField()
        {
            Direction = Orientation::Horizontal;
            Spacing = 4.0f;

            RefPtr<NumericField> x = MakeEulerField(StringView(u8"X"), AxisColors::X);
            RefPtr<NumericField> y = MakeEulerField(StringView(u8"Y"), AxisColors::Y);
            RefPtr<NumericField> z = MakeEulerField(StringView(u8"Z"), AxisColors::Z);
            m_x = x.Get();
            m_y = y.Get();
            m_z = z.Get();

            QuaternionField* self = this;
            m_x->OnValueChanged.Add(
                [self](NumericField*, f64 v)
                {
                    if (!self->m_syncing)
                    {
                        self->m_eulerDegrees.x = static_cast<f32>(v);
                        self->RebuildQuaternionFromEulers();
                    }
                });
            m_y->OnValueChanged.Add(
                [self](NumericField*, f64 v)
                {
                    if (!self->m_syncing)
                    {
                        self->m_eulerDegrees.y = static_cast<f32>(v);
                        self->RebuildQuaternionFromEulers();
                    }
                });
            m_z->OnValueChanged.Add(
                [self](NumericField*, f64 v)
                {
                    if (!self->m_syncing)
                    {
                        self->m_eulerDegrees.z = static_cast<f32>(v);
                        self->RebuildQuaternionFromEulers();
                    }
                });

            WireChildEditEvents(m_x);
            WireChildEditEvents(m_y);
            WireChildEditEvents(m_z);

            SyncToFields();

            AddView(m_x, GrowMatchParams());
            AddView(m_y, GrowMatchParams());
            AddView(m_z, GrowMatchParams());
        }

        void SetRange(f64 min, f64 max)
        {
            m_x->SetMin(min);
            m_x->SetMax(max);
            m_y->SetMin(min);
            m_y->SetMax(max);
            m_z->SetMin(min);
            m_z->SetMax(max);
        }
        [[nodiscard]] f64 Step() const { return m_x->Step(); }
        void SetStep(f64 value)
        {
            m_x->SetStep(value);
            m_y->SetStep(value);
            m_z->SetStep(value);
        }
        [[nodiscard]] i32 DecimalPlaces() const { return m_x->DecimalPlaces(); }
        void SetDecimalPlaces(i32 value)
        {
            m_x->SetDecimalPlaces(value);
            m_y->SetDecimalPlaces(value);
            m_z->SetDecimalPlaces(value);
        }
        [[nodiscard]] bool ShowSpinButtons() const { return m_x->ShowSpinButtons.Value(); }
        void SetShowSpinButtons(bool value)
        {
            m_x->ShowSpinButtons.SetValue(value);
            m_y->ShowSpinButtons.SetValue(value);
            m_z->ShowSpinButtons.SetValue(value);
        }

    private:
        void RebuildQuaternionFromEulers()
        {
            m_value = EulerDegreesToQuaternion(m_eulerDegrees);
            OnValueChanged.Invoke(m_value);
        }

        void SyncToFields()
        {
            if (m_syncing)
            {
                return;
            }
            m_syncing = true;
            m_x->SetValue(m_eulerDegrees.x);
            m_y->SetValue(m_eulerDegrees.y);
            m_z->SetValue(m_eulerDegrees.z);
            m_syncing = false;
        }

        static RefPtr<NumericField> MakeEulerField(StringView axisText, Color axisColor)
        {
            RefPtr<NumericField> f = MakeRef<NumericField>(DefaultAllocator());
            f->AddClass(u8"property-field");
            f->ShowSpinButtons.SetValue(false);
            f->SetMin(-360);
            f->SetMax(360);
            f->SetStep(1);
            f->SetDecimalPlaces(2);
            RefPtr<AxisLabel> label = MakeRef<AxisLabel>(DefaultAllocator(), axisText, axisColor);
            f->SetPrefix(label.Get());
            return f;
        }

        // Conversion helpers - X=pitch (around X), Y=yaw (around Y), Z=roll (around Z).
        static Float3 QuaternionToEulerDegrees(Quaternion q)
        {
            const f32 sinP = 2.0f * (q.w * q.x - q.z * q.y);
            f32 pitch;
            if (Abs(sinP) >= 1.0f)
            {
                pitch = (sinP >= 0) ? (kPi / 2.0f) : -(kPi / 2.0f);
            }
            else
            {
                pitch = Asin(sinP);
            }

            const f32 sinYCosP = 2.0f * (q.w * q.y + q.x * q.z);
            const f32 cosYCosP = 1.0f - 2.0f * (q.x * q.x + q.y * q.y);
            const f32 yaw = Atan2(sinYCosP, cosYCosP);

            const f32 sinRCosP = 2.0f * (q.w * q.z + q.x * q.y);
            const f32 cosRCosP = 1.0f - 2.0f * (q.x * q.x + q.z * q.z);
            const f32 roll = Atan2(sinRCosP, cosRCosP);

            return Float3{pitch * kRadToDeg, yaw * kRadToDeg, roll * kRadToDeg};
        }

        static Quaternion EulerDegreesToQuaternion(Float3 euler)
        {
            const f32 pitch = euler.x * kDegToRad;
            const f32 yaw = euler.y * kDegToRad;
            const f32 roll = euler.z * kDegToRad;

            const f32 cp = Cos(pitch * 0.5f);
            const f32 sp = Sin(pitch * 0.5f);
            const f32 cy = Cos(yaw * 0.5f);
            const f32 sy = Sin(yaw * 0.5f);
            const f32 cr = Cos(roll * 0.5f);
            const f32 sr = Sin(roll * 0.5f);

            return Quaternion{sp * cy * cr - cp * sy * sr, cp * sy * cr + sp * cy * sr,
                              cp * cy * sr - sp * sy * cr, cp * cy * cr + sp * sy * sr};
        }

        NumericField* m_x = nullptr;
        NumericField* m_y = nullptr;
        NumericField* m_z = nullptr;
        Quaternion m_value = Quaternion::Identity;
        Float3 m_eulerDegrees{0, 0, 0};
        bool m_syncing = false;
    };

    DRACONIC_DEFINE_OBJECT(AxisLabel, "draconic::ui::toolkit")
    DRACONIC_DEFINE_OBJECT(AggregatingVectorField, "draconic::ui::toolkit")
    DRACONIC_DEFINE_OBJECT(Vector2Field, "draconic::ui::toolkit")
    DRACONIC_DEFINE_OBJECT(Vector3Field, "draconic::ui::toolkit")
    DRACONIC_DEFINE_OBJECT(Vector4Field, "draconic::ui::toolkit")
    DRACONIC_DEFINE_OBJECT(QuaternionField, "draconic::ui::toolkit")
}
