// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor.App :property_names: a reflected property's name as a row's label.

#include <doctest/doctest.h>

#include "Core/Prelude.h"

import foundation.core;
import editor.app;

using namespace foundation::core;
using editor::app::PrettifyPropertyName;

TEST_CASE("property names: a property's name becomes a row's label")
{
    CHECK(PrettifyPropertyName(u8"castsShadows").AsView() == StringView(u8"Casts Shadows"));
    CHECK(PrettifyPropertyName(u8"fovYRadians").AsView() == StringView(u8"Fov Y Radians"));
    CHECK(PrettifyPropertyName(u8"type").AsView() == StringView(u8"Type"));
    CHECK(PrettifyPropertyName(u8"skyIntensity").AsView() == StringView(u8"Sky Intensity"));
    CHECK(PrettifyPropertyName(u8"IBL").AsView() == StringView(u8"IBL"));
    CHECK(PrettifyPropertyName(u8"ReflectionProbe").AsView() == StringView(u8"Reflection Probe"));
}
