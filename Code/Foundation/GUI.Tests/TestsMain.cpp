// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Tests - doctest entry point (shared main: redirects scratch data to .test-scratch/).
//
// The GUI's text is shaped by HarfBuzz here, as its hosts (the sandbox, the engine) shape it. The
// shaper is installed before any test runs, so every font a test loads uses it. A test that needs
// the basic shaper uninstalls it around itself.
#include "TestMain.h"

import foundation.fonts.harfbuzz;

namespace
{
    struct InstallHarfBuzz
    {
        InstallHarfBuzz() { foundation::fonts::HarfBuzzShaping::Install(); }
    };
    const InstallHarfBuzz kInstallHarfBuzz; // installed before main, since the registry it fills is a function-local static
}
