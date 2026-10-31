// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The model-loader registry storage: one instance per process, because loader libraries
// register here and Model.IO dispatches from it, which must be the same list across shared
// libraries.

module;
#include "Core/Prelude.h"

module foundation.model.io;

using namespace foundation::core;

namespace foundation::model::io::detail
{
    Array<ModelLoader*>& loaders()
    {
        static Array<ModelLoader*> s;
        return s;
    }
} // namespace foundation::model::io::detail
