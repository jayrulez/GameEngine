// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Shared probe types for the plugin cross-boundary identity test. Included by BOTH
// the host test executable and the CrossPlugin shared library: each binary compiles
// its OWN definition (own StaticType static, own address), which is exactly the
// shared-libraries scenario - identity must hold by TypeId, never by pointer
// (Documentation/Specs/shared-libraries.md). Header-only on purpose.

#pragma once

#include "Core/Reflection/Reflect.h"

namespace crossprobe
{
    // Registered by the HOST before the plugin loads; the plugin must resolve it
    // through the (single, shared-engine) SubsystemManager by TypeId.
    class HostProbeSubsystem final : public foundation::runtime::Subsystem
    {
    public:
        int hostValue = 41;
    };

    // Created by the PLUGIN via MakeRef (exercising the cross-boundary RefControl
    // handshake); the HOST adopts the reference, casts it, and releases it.
    class ProbeObject : public foundation::core::Object
    {
        RTTI_OBJECT(ProbeObject, foundation::core::Object)
    public:
        int payload = 0;
    };

    // The "MyFancyComponent" case (game-native-code.md N6/S1): a component the PLUGIN's
    // manager owns, authored into a scene that outlives the plugin across a reload.
    struct FancyComponent
    {
        foundation::core::i32 payload = 0;
    };
    inline void Serialize(foundation::core::ISerializer& ar, FancyComponent& c)
    {
        foundation::core::Serialize(ar, "payload", c.payload);
    }
    class FancyManager final : public foundation::scene::SerializableComponentManager<FancyComponent>
    {
    public:
        FancyManager() : SerializableComponentManager<FancyComponent>(u8"crossprobe.Fancy") {}
    };
    inline void InstallFancy(foundation::scene::Scene& scene) { scene.AddSystem<FancyManager>(); }

    inline const foundation::core::TypeInfo& ProbeObjectStaticTypeDef() noexcept
    {
        static const foundation::core::TypeInfo info =
            foundation::core::MakeTypeInfo<ProbeObject>("ProbeObject", "crossprobe",
                                                        &foundation::core::Object::StaticType());
        return info;
    }
} // namespace crossprobe

// StaticType out-of-line per binary (RTTI_DEFINE_OBJECT is for .cpp files; a header
// shared by two binaries defines it inline so each binary carries one copy).
inline const foundation::core::TypeInfo& crossprobe::ProbeObject::StaticType() noexcept
{
    return crossprobe::ProbeObjectStaticTypeDef();
}
