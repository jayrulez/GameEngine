// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Engine::UI.Script - `engine.ui.script` module aggregator.
//
// The UI SCRIPT SURFACE: reflected typed view handles (UILabel/UIButton/UIProgressBar/UITextBox/UIViewGroup/
// UIScreen) + the `ui` facade (screen tier: finders + push/pop/replace over the gamekit ScreenStack).
// Out-of-tree facade module (the facade-pattern rule) consuming CORE foundation.ui + foundation.ui.
// gamekit; joins RegisterAllScriptFacades. Consumers write `import engine.ui.script;`.

export module engine.ui.script;

export import :types;
export import :facade;
