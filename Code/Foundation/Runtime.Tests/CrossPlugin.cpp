// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The cross-boundary identity plugin: built only in ENGINE_SHARED_LIBS lanes, where
// it links the engine's .so set (the supported plugin model; a plugin against a
// static engine would re-embed Core and duplicate every global, which is unsupported).
// Loaded by the RuntimeTests cross-boundary case, it proves shared-library identity
// end to end across a real dlopen boundary:
//   - resolves a host-registered subsystem via TypeId-keyed SubsystemManager lookup
//   - creates a ref-counted object through the (single) MakeRef handshake slot
//   - registers a type the host then finds in the (single) GlobalTypeRegistry

#include "Core/Prelude.h"

import foundation.core;
import foundation.runtime;
import foundation.scene;

#include "CrossBoundaryProbe.h"

using namespace foundation::core;
using namespace foundation::runtime;

#if defined(_WIN32)
#define PLUGIN_EXPORT __declspec(dllexport)
#else
#define PLUGIN_EXPORT __attribute__((visibility("default")))
#endif

namespace
{
    int g_resolvedHostValue = -1; // HostProbeSubsystem::hostValue seen at OnLoad, or -1

    class CrossPlugin final : public IRuntimePlugin
    {
    public:
        [[nodiscard]] StringView Name() const noexcept override { return u8"CrossPlugin"; }

        void OnLoad(SubsystemManager& context) override
        {
            // TypeId-keyed lookup of a subsystem the host registered: this binary's
            // TypeOf<HostProbeSubsystem>() static is its own copy at its own address,
            // so only the id makes this resolve (a pointer-keyed map would return null).
            if (crossprobe::HostProbeSubsystem* probe =
                    context.GetSubsystem<crossprobe::HostProbeSubsystem>())
            {
                g_resolvedHostValue = probe->hostValue;
            }
            // Register this binary's ProbeObject metadata; the host looks it up by name
            // through the shared registry instance.
            GlobalTypeRegistry().Register(crossprobe::ProbeObject::StaticType());
            // Contribute the Fancy component manager to every scene, live ones included.
            // The host's recorder reverses it on unload.
            foundation::scene::SceneModuleContributions::Contribution fancy;
            fancy.id = u8"crossprobe.fancy";
            fancy.install = &crossprobe::InstallFancy;
            fancy.systemType = TypeOf<crossprobe::FancyManager>().id;
            foundation::scene::SceneModuleContributions::Global().Add(fancy);
        }
        void OnUnload(SubsystemManager&) override {}
    };
} // namespace

extern "C" PLUGIN_EXPORT IRuntimePlugin* CreatePlugin()
{
    static CrossPlugin plugin;
    return &plugin;
}

// The host-value the plugin observed while resolving the host's subsystem (-1 = miss).
extern "C" PLUGIN_EXPORT int CrossPluginResolvedHostValue() { return g_resolvedHostValue; }

// This binary's TypeOf<Float3>(): the slot must be the host's (one TypeInfo per process),
// carrying the properties Core's registrar patched in (metadata patched inside one image
// must not be invisible to another).
extern "C" PLUGIN_EXPORT const foundation::core::TypeInfo* CrossPluginTypeOfFloat3()
{
    return &TypeOf<Float3>();
}
extern "C" PLUGIN_EXPORT unsigned CrossPluginFloat3PropertyCount()
{
    return static_cast<unsigned>(Properties(TypeOf<Float3>()).Size());
}

// Create a ProbeObject in this binary via MakeRef and hand the caller an owning
// reference (detached: +1 strong the host adopts and releases). Exercises the
// RefControl handshake and the release path across the boundary.
extern "C" PLUGIN_EXPORT foundation::core::Object* CrossPluginCreateObject(int payload)
{
    RefPtr<crossprobe::ProbeObject> object =
        MakeRef<crossprobe::ProbeObject>(DefaultAllocator());
    object->payload = payload;
    object->AddRef(); // the caller adopts this reference
    return object.Get();
}
