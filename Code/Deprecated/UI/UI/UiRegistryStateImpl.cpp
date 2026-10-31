// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Registry storage for the UI's markup, theme, type and drawable registries. It is
// defined here, one instance per process, because registration and lookup happen in
// different libraries, and inline function-local statics in the interfaces would be
// duplicated per shared library.

module;
#include "Core/Prelude.h"

module foundation.ui;

using namespace foundation::core;

namespace foundation::ui
{
    HashMap<String, MarkupRegistry::ViewFactory>& MarkupRegistry::ViewFactories()
    {
        static HashMap<String, ViewFactory> v;
        return v;
    }
    HashMap<String, MarkupRegistry::PropertySetter>& MarkupRegistry::ViewProps()
    {
        static HashMap<String, PropertySetter> v;
        return v;
    }
    // The RegisterBuiltins lock and run-once flag are out of line for the same reason as the
    // maps: an inline function-local guard is duplicated per shared library, so one library's
    // registration would satisfy only its own copy. The lock exists because the UI document
    // cook registers on job workers.
    Mutex& MarkupRegistry::RegistrationLock()
    {
        static Mutex lock;
        return lock;
    }
    bool& MarkupRegistry::BuiltinsRegisteredFlag()
    {
        static bool registered = false;
        return registered;
    }
} // namespace foundation::ui

namespace foundation::ui
{
    ThemeIconSet& ThemeIconSet::Get()
    {
        static ThemeIconSet instance;
        return instance;
    }
} // namespace foundation::ui

namespace foundation::ui::detail
{
    Array<IThemeExtension*>& ThemeExtensionList()
    {
        static Array<IThemeExtension*> extensions;
        return extensions;
    }

    HashMap<String, const TypeInfo*>& UITypeMap()
    {
        static HashMap<String, const TypeInfo*> types;
        return types;
    }

    HashMap<String, DrawableFactoryRegistry::FactoryFn>& FactoryMap()
    {
        static HashMap<String, DrawableFactoryRegistry::FactoryFn> factories;
        return factories;
    }

    bool& DrawableBuiltinsRegisteredFlag()
    {
        static bool registered = false;
        return registered;
    }
    Mutex& DrawableRegistrationLock()
    {
        static Mutex lock;
        return lock;
    }
} // namespace foundation::ui::detail
