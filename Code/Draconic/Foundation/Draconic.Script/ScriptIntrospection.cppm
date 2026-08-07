// Draconic Script - :script_introspection partition
//
// The accurate data source for a future ScriptClassesView / autocomplete: what is
// callable in THIS backend's language and how it is spelled. The reflection registry
// says what types EXIST; DescribeBoundApi (on IScriptManager) says what a backend
// actually BOUND and under what script-visible names (they differ per language:
// AngelScript spells a static as `Float3::Dot`, Wren as a static foreign method, and a
// backend may silently fail to bind a type at all). A conformance diff against the
// reflection registry catches exactly that silent gap.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.script:script_introspection;

import draconic.foundation;

namespace foundation = draconic::foundation;

export namespace draconic::script
{
    /// What kind of member a bound API entry is, in the language's own terms.
    enum class ScriptApiMemberKind : foundation::u8
    {
        Method,   // callable member / free function
        Property, // field-like accessor (get / get+set)
        Constant, // named constant value
    };

    /// One script-visible member of a bound type, spelled the way the backend presents
    /// it (the `signature` is language-formatted, e.g. Wren `Dot(_,_)` or AngelScript
    /// `Float3@ Dot(const Float3&in, const Float3&in)`).
    struct ScriptApiMember
    {
        foundation::String name;
        foundation::String signature;
        bool isStatic = false;
        ScriptApiMemberKind kind = ScriptApiMemberKind::Method;
    };

    /// One script-visible type (or namespace) the backend bound, with its members. The
    /// `scriptName` is the exact name a script uses; `isNamespace` distinguishes a real
    /// class from a namespace a backend synthesizes for statics.
    struct ScriptApiType
    {
        foundation::String scriptName;
        // The reflected identity behind the binding (0 = none). Tooling maps it back to
        // registry metadata - e.g. TypeRegistry::DomainOf marks editor-only bindings.
        foundation::TypeId typeId = 0;
        bool isNamespace = false;
        foundation::Array<ScriptApiMember> members;
    };
}
