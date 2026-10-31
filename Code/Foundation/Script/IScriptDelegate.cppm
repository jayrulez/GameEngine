// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Script - :script_delegate partition
//
// IScriptDelegate: a script function held as a native callback. A native API takes one
// as RefPtr<IScriptDelegate>; holding it keeps the underlying script function alive
// (GC-safe), and Invoke() calls back into script with reflected Variant args, returning
// the script result.
//
// The interface is backend-neutral: each backend subclasses IScriptDelegate to wrap its
// own callable primitive (an AngelScript function handle, a Luau function ref), and
// marshals a script function argument into one when a reflected method parameter is typed
// RefPtr<IScriptDelegate>. Because IScriptDelegate derives core::Object, the reflection
// object-argument marshalling (ParamTypeOf, AcceptArg, ConvertArg) already recognizes
// RefPtr<IScriptDelegate> as an object parameter; the only backend-specific work is
// wrapping the callable into a delegate.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.script:script_delegate;

import foundation.core;

namespace core = foundation::core;

export namespace foundation::script
{
    /// A script function exposed to native code as a typed callback. A native API takes
    /// one as `RefPtr<IScriptDelegate>`; holding the RefPtr keeps the script function
    /// alive across garbage collection. Invoke marshals `args` into the script call and
    /// returns its result (empty Variant for a void callback), or an error if the call
    /// faults or the owning context is gone.
    class IScriptDelegate : public core::Object
    {
        RTTI_OBJECT(IScriptDelegate, core::Object)
    public:
        [[nodiscard]] virtual core::Result<core::Variant>
        Invoke(core::Span<core::Variant> args) = 0;
    };

    /// Name a function type a facade's callbacks take, in AngelScript's funcdef form (for example
    /// `void NetNumberHandler(int sender, double value)`), so scripts can pass a handler of that
    /// type without declaring it. A backend that needs named types (AngelScript) declares every
    /// registered one when it starts; one whose functions are plain values (Luau) ignores them.
    /// Call before a script manager is created, beside the facade's own registration. Idempotent.
    void RegisterDelegateShape(core::StringView declaration);
    /// The function types registered so far, in registration order.
    [[nodiscard]] core::Span<const core::String> DelegateShapes();
}

namespace foundation::script
{
    // StaticType() is defined here rather than through the reflected registry: scripts never
    // construct an IScriptDelegate, and the type identity only needs to exist so reflected
    // method parameters can be spelled RefPtr<IScriptDelegate> and the backends recognize it.
    RTTI_DEFINE_OBJECT(IScriptDelegate, "rtti::script")
}
