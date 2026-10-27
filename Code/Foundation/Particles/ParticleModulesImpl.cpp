// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Particles - reflection implementation unit (batch 1): the range leaf value types +
// the flat/range particle module classes, so the module types stop being tooling-invisible.
// The particle editor page stays bespoke - this reflection is for
// scriptability/tooling visibility, not a generated inspector.
//
// REFLECT_MEMBERS bodies live out of the ParticleModules.cppm interface (GCC module hygiene:
// property member-pointers make GCC emit a gcm cluster). A module's authored config fields are
// reflected here; the runtime-set "hidden" fields (emitterPosition/emitterVelocity) are not.
// Range struct fields are Nested (particle value types are not script-marshalled leaves - they
// are traversed via reflection); Core math types (Float3) are plain Properties. Modules that use
// EmissionShape (Position/Velocity) or curves (the OverLifetime behaviors) are a later batch.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

module foundation.particles;

import foundation.core;

using namespace foundation::core;

namespace foundation::particles
{
    // ---- range leaf value types --------------------------------------------------------------
    REFLECT_VALUE(RangeFloat, "rtti::particles")
    {
        builder.Property<&RangeFloat::min>("min").Property<&RangeFloat::max>("max");
    }
    REFLECT_VALUE(RangeFloat2, "rtti::particles")
    {
        builder.Property<&RangeFloat2::min>("min").Property<&RangeFloat2::max>("max");
    }
    REFLECT_VALUE(RangeColor, "rtti::particles")
    {
        builder.Property<&RangeColor::min>("min").PropAttribute("color", true).Property<&RangeColor::max>("max").PropAttribute("color", true);
    }

    // ---- emission shape (flat struct + its type discriminator) -------------------------------
    REFLECT_ENUM(EmissionShapeType, "rtti::particles")
    {
        builder.Value("Point", EmissionShapeType::Point);
        builder.Value("Sphere", EmissionShapeType::Sphere);
        builder.Value("Hemisphere", EmissionShapeType::Hemisphere);
        builder.Value("Box", EmissionShapeType::Box);
        builder.Value("Cone", EmissionShapeType::Cone);
        builder.Value("Ring", EmissionShapeType::Ring);
        builder.Value("Circle", EmissionShapeType::Circle);
        builder.Value("Edge", EmissionShapeType::Edge);
    }
    REFLECT_VALUE(EmissionShape, "rtti::particles")
    {
        // Each measure where its shape uses it (`visibleWhen` of the type's raw value: 0 a point,
        // 1 a sphere, 2 a hemisphere, 3 a box, 4 a cone, 5 a ring, 6 a circle, 7 an edge).
        builder.Property<&EmissionShape::type>("type")
            .PropAttribute("displayName", String(u8"Shape"))
            .Property<&EmissionShape::radius>("radius")
            .PropAttribute("visibleWhen", String(u8"type=1,2,4,5,6,7"))
            .PropAttribute("description", String(u8"An edge's half its length"))
            .Property<&EmissionShape::extents>("extents")
            .PropAttribute("displayName", String(u8"Half Extents"))
            .PropAttribute("visibleWhen", String(u8"type=3"))
            .Property<&EmissionShape::angle>("angle")
            .PropAttribute("description", String(u8"The cone's half angle, in radians"))
            .PropAttribute("visibleWhen", String(u8"type=4"))
            .Property<&EmissionShape::arc>("arc")
            .PropAttribute("description", String(u8"How much of the way round it emits"))
            .PropAttribute("range", Float4{0.0f, 1.0f, 0.01f, 0.0f})
            .PropAttribute("visibleWhen", String(u8"type=1,2,4,5,6"))
            .Property<&EmissionShape::emitFromShell>("emitFromShell")
            .PropAttribute("displayName", String(u8"From Its Surface"))
            .PropAttribute("visibleWhen", String(u8"type=1,2,6"));
    }

    // ---- initializers using the emission shape -----------------------------------------------
    REFLECT_MEMBERS(PositionInitializer, "rtti::particles")
    {
        builder.Attribute("displayName", String(u8"Position"))
            .Nested<&PositionInitializer::shape>("shape")
            .Property<&PositionInitializer::localSpace>("localSpace");
    }
    REFLECT_MEMBERS(VelocityInitializer, "rtti::particles")
    {
        builder.Attribute("displayName", String(u8"Velocity"))
            .Property<&VelocityInitializer::baseVelocity>("baseVelocity")
            .Property<&VelocityInitializer::randomness>("randomness")
            .Property<&VelocityInitializer::shapeDirectionSpeed>("shapeDirectionSpeed")
            .Property<&VelocityInitializer::velocityInheritance>("velocityInheritance")
            .PropAttribute("range", Float4{0.0f, 1.0f, 0.01f, 0.0f})
            .Nested<&VelocityInitializer::shape>("shape");
    }

    // ---- initializers (flat / range) ---------------------------------------------------------
    REFLECT_MEMBERS(LifetimeInitializer, "rtti::particles")
    {
        builder.Attribute("displayName", String(u8"Lifetime"))
            .Nested<&LifetimeInitializer::lifetime>("lifetime");
    }
    REFLECT_MEMBERS(ColorInitializer, "rtti::particles")
    {
        builder.Attribute("displayName", String(u8"Color"))
            .Nested<&ColorInitializer::color>("color");
    }
    REFLECT_MEMBERS(SizeInitializer, "rtti::particles")
    {
        builder.Attribute("displayName", String(u8"Size")).Nested<&SizeInitializer::size>("size");
    }
    REFLECT_MEMBERS(RotationInitializer, "rtti::particles")
    {
        builder.Attribute("displayName", String(u8"Rotation"))
            .Nested<&RotationInitializer::rotation>("rotation")
            .Nested<&RotationInitializer::rotationSpeed>("rotationSpeed");
    }
    REFLECT_MEMBERS(MeshOrientationInitializer, "rtti::particles")
    {
        builder.Attribute("displayName", String(u8"Mesh Orientation"))
            .Property<&MeshOrientationInitializer::randomAxis>("randomAxis")
            .Property<&MeshOrientationInitializer::fixedAxis>("fixedAxis");
    }

    // ---- behaviors (flat / vector force fields) ----------------------------------------------
    REFLECT_MEMBERS(GravityBehavior, "rtti::particles")
    {
        builder.Attribute("displayName", String(u8"Gravity"))
            .Property<&GravityBehavior::multiplier>("multiplier")
            .Property<&GravityBehavior::direction>("direction");
    }
    REFLECT_MEMBERS(DragBehavior, "rtti::particles")
    {
        builder.Attribute("displayName", String(u8"Drag")).Property<&DragBehavior::drag>("drag");
    }
    REFLECT_MEMBERS(WindBehavior, "rtti::particles")
    {
        builder.Attribute("displayName", String(u8"Wind"))
            .Property<&WindBehavior::force>("force")
            .Property<&WindBehavior::turbulence>("turbulence");
    }
    REFLECT_MEMBERS(TurbulenceBehavior, "rtti::particles")
    {
        builder.Attribute("displayName", String(u8"Turbulence"))
            .Property<&TurbulenceBehavior::strength>("strength")
            .Property<&TurbulenceBehavior::frequency>("frequency")
            .Property<&TurbulenceBehavior::speed>("speed");
    }
    REFLECT_MEMBERS(VortexBehavior, "rtti::particles")
    {
        builder.Attribute("displayName", String(u8"Vortex"))
            .Property<&VortexBehavior::strength>("strength")
            .Property<&VortexBehavior::center>("center")
            .Property<&VortexBehavior::axis>("axis");
    }

    // ---- attractor / radial force / collision (flat) -----------------------------------------
    REFLECT_MEMBERS(AttractorBehavior, "rtti::particles")
    {
        builder.Attribute("displayName", String(u8"Attractor"))
            .Property<&AttractorBehavior::strength>("strength")
            .Property<&AttractorBehavior::position>("position")
            .Property<&AttractorBehavior::radius>("radius");
    }
    REFLECT_MEMBERS(RadialForceBehavior, "rtti::particles")
    {
        builder.Attribute("displayName", String(u8"Radial Force"))
            .Property<&RadialForceBehavior::strength>("strength");
    }

    REFLECT_VALUE(CollisionPlane, "rtti::particles")
    {
        builder.Property<&CollisionPlane::normal>("normal")
            .Property<&CollisionPlane::distance>("distance");
    }
    REFLECT_VALUE(CollisionSphere, "rtti::particles")
    {
        builder.Property<&CollisionSphere::center>("center")
            .Property<&CollisionSphere::radius>("radius");
    }
    REFLECT_VALUE(CollisionBox, "rtti::particles")
    {
        builder.Property<&CollisionBox::center>("center")
            .Property<&CollisionBox::halfExtents>("halfExtents");
    }
    REFLECT_MEMBERS(CollisionBehavior, "rtti::particles")
    {
        // The fixed-capacity shape lists are count-bound inline vectors, reflected as BoundedArray
        // containers (size = the live count clamped to capacity); the counts stay as scalars too.
        // (The counts are the lists' sizes, not rows of their own.)
        builder.Attribute("displayName", String(u8"Collision"))
            .BoundedArray<&CollisionBehavior::planes, &CollisionBehavior::planeCount>("planes")
            .BoundedArray<&CollisionBehavior::spheres, &CollisionBehavior::sphereCount>("spheres")
            .BoundedArray<&CollisionBehavior::boxes, &CollisionBehavior::boxCount>("boxes")
            .Property<&CollisionBehavior::radius>("radius")
            .PropAttribute("description", String(u8"A particle's, as it meets them"))
            .Property<&CollisionBehavior::bounce>("bounce")
            .PropAttribute("range", Float4{0.0f, 1.0f, 0.01f, 0.0f})
            .Property<&CollisionBehavior::friction>("friction")
            .PropAttribute("range", Float4{0.0f, 1.0f, 0.01f, 0.0f})
            .Property<&CollisionBehavior::lifetimeLoss>("lifetimeLoss")
            .PropAttribute("description", String(u8"The fraction of its life a hit takes"))
            .PropAttribute("range", Float4{0.0f, 1.0f, 0.01f, 0.0f});
    }

    // ---- curves (count-bound key arrays) + the OverLifetime behaviors ------------------------
    // Their keys and stops are Core's, reflected by RegisterCoreTypes.
    REFLECT_VALUE(ParticleCurveFloat, "rtti::particles")
    {
        builder.BoundedArray<&ParticleCurveFloat::keys, &ParticleCurveFloat::keyCount>("keys")
            .Property<&ParticleCurveFloat::keyCount>("keyCount");
    }
    REFLECT_VALUE(ParticleCurveColor, "rtti::particles")
    {
        builder.BoundedArray<&ParticleCurveColor::colors, &ParticleCurveColor::colorCount>("colors")
            .Property<&ParticleCurveColor::colorCount>("colorCount")
            .BoundedArray<&ParticleCurveColor::alphas, &ParticleCurveColor::alphaCount>("alphas")
            .Property<&ParticleCurveColor::alphaCount>("alphaCount")
            .Property<&ParticleCurveColor::interpolation>("interpolation");
    }

    REFLECT_MEMBERS(ColorOverLifetimeBehavior, "rtti::particles")
    {
        builder.Attribute("displayName", String(u8"Color over Lifetime"))
            .Nested<&ColorOverLifetimeBehavior::curve>("curve");
    }
    REFLECT_MEMBERS(AlphaOverLifetimeBehavior, "rtti::particles")
    {
        builder.Attribute("displayName", String(u8"Alpha over Lifetime"))
            .Nested<&AlphaOverLifetimeBehavior::curve>("curve");
    }
    REFLECT_MEMBERS(SizeOverLifetimeBehavior, "rtti::particles")
    {
        builder.Attribute("displayName", String(u8"Size over Lifetime"))
            .Nested<&SizeOverLifetimeBehavior::size>("size")
            .Property<&SizeOverLifetimeBehavior::separateAxes>("separateAxes")
            .Nested<&SizeOverLifetimeBehavior::sizeY>("sizeY")
            .PropAttribute("visibleWhen", String(u8"separateAxes"));
    }
    REFLECT_MEMBERS(RotationOverLifetimeBehavior, "rtti::particles")
    {
        builder.Attribute("displayName", String(u8"Rotation over Lifetime"))
            .Nested<&RotationOverLifetimeBehavior::curve>("curve");
    }
    REFLECT_MEMBERS(SpeedOverLifetimeBehavior, "rtti::particles")
    {
        builder.Attribute("displayName", String(u8"Speed over Lifetime"))
            .Nested<&SpeedOverLifetimeBehavior::curve>("curve");
    }

    // Registers the range + shape + collision + curve leaf value types (the modules' StaticType()
    // self-builds via REFLECT_MEMBERS). Idempotent; call from RegisterParticleModules().
    void RegisterParticleModuleReflection()
    {
        static const bool once = []()
        {
            RttiRegisterValue_RangeFloat();
            RttiRegisterValue_RangeFloat2();
            RttiRegisterValue_RangeColor();
            RttiRegisterEnum_EmissionShapeType();
            RttiRegisterValue_EmissionShape();
            RttiRegisterValue_CollisionPlane();
            RttiRegisterValue_CollisionSphere();
            RttiRegisterValue_CollisionBox();
            RegisterCoreTypes(); // the curves' keys and stops (Core's), before the curves
            RttiRegisterValue_ParticleCurveFloat();
            RttiRegisterValue_ParticleCurveColor();
            // The module arrays are polymorphic: register Array<RefPtr<Base>> as a polymorphic
            // container so tooling recurses into each module's concrete reflected type. Pass the
            // standard serialization create-by-type adapter (the factory flows in as function
            // pointers - reflection never imports serialization).
            RegisterPolymorphicArrayType<ParticleInitializer>(
                &CreateSerializableElement<ParticleInitializer>,
                &CanCreateSerializableElement<ParticleInitializer>);
            RegisterPolymorphicArrayType<ParticleBehavior>(
                &CreateSerializableElement<ParticleBehavior>,
                &CanCreateSerializableElement<ParticleBehavior>);
            return true;
        }();
        (void)once;
    }
}
