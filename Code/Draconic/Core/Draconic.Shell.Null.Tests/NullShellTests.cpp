#include <doctest/doctest.h>

#include "Draconic.Foundation/Prelude.h"

import draconic.foundation;
import draconic.shell;
import draconic.shell.null;

using namespace draconic::foundation;
using namespace draconic::shell;

TEST_CASE("shell.null: a headless shell reports a window and run state")
{
    WindowSettings settings;
    settings.width = 800;
    settings.height = 600;

    NullShell shell(settings);
    REQUIRE(shell.MainWindow() != nullptr);
    CHECK(shell.MainWindow()->Width() == 800u);
    CHECK(shell.MainWindow()->Height() == 600u);
    CHECK(shell.MainWindow()->Native().system == WindowSystem::Unknown); // headless: no handles
    CHECK(shell.MainWindow()->Native().window == nullptr);
    CHECK(shell.IsRunning());

    shell.ProcessEvents(); // no-op, must not change run state
    CHECK(shell.IsRunning());

    shell.RequestExit();
    CHECK_FALSE(shell.IsRunning());
}

TEST_CASE("shell.null: closing the window stops the shell")
{
    NullShell shell;
    CHECK(shell.IsRunning());
    shell.MainWindow()->Close();
    CHECK_FALSE(shell.MainWindow()->IsOpen());
    CHECK_FALSE(shell.IsRunning());
}

TEST_CASE("shell.null: window manager creates, lists, and looks up windows")
{
    NullShell shell;
    IWindowManager* wm = shell.WindowManager();
    REQUIRE(wm != nullptr);

    // The shell seeds one main window; it is Windows()[0] and MainWindow().
    REQUIRE(wm->Windows().Size() == 1u);
    IWindow* main = wm->MainWindow();
    REQUIRE(main != nullptr);
    CHECK(wm->Windows()[0] == main);
    CHECK(main->Id() != 0u); // 0 is never a valid id
    CHECK(wm->GetWindow(main->Id()) == main);
    CHECK(wm->GetWindow(99999u) == nullptr);

    // Open a second window; ids are distinct, main is unchanged.
    WindowSettings s;
    s.width = 320;
    s.height = 240;
    Result<IWindow*> second = wm->CreateWindow(s);
    REQUIRE(second.HasValue());
    CHECK(second.Value()->Id() != main->Id());
    CHECK(wm->Windows().Size() == 2u);
    CHECK(wm->MainWindow() == main); // still the first
    CHECK(second.Value()->Width() == 320u);
}

TEST_CASE("shell.null: DestroyWindow defers until FlushDestroyed")
{
    NullShell shell;
    IWindowManager* wm = shell.WindowManager();
    Result<IWindow*> second = wm->CreateWindow(WindowSettings{});
    REQUIRE(second.HasValue());
    const u32 secondId = second.Value()->Id();
    REQUIRE(wm->Windows().Size() == 2u);

    // Destroy is deferred: the window stays listed (but closed) until flush.
    wm->DestroyWindow(second.Value());
    CHECK(wm->Windows().Size() == 2u);
    CHECK_FALSE(second.Value()->IsOpen());

    wm->FlushDestroyed();
    CHECK(wm->Windows().Size() == 1u);
    CHECK(wm->GetWindow(secondId) == nullptr);
    CHECK(wm->MainWindow() != nullptr); // main survived
}

TEST_CASE("shell.null: window events queue is empty (no OS source)")
{
    NullShell shell;
    shell.ProcessEvents();
    CHECK(shell.WindowManager()->Events().Size() == 0u);
}

TEST_CASE("shell.null: NullWindow resize hook updates reported size")
{
    NullShell shell;
    auto* main = static_cast<NullWindow*>(shell.MainWindow());
    main->Resize(1024, 768);
    CHECK(main->Width() == 1024u);
    CHECK(main->Height() == 768u);
    main->SetMinimized(true);
    CHECK(main->IsMinimized());
}

TEST_CASE("shell.null: input is present and reports no activity")
{
    NullShell shell;
    IInputManager* input = shell.Input();
    REQUIRE(input != nullptr);

    // Devices are reachable so callers need no null checks.
    REQUIRE(input->Keyboard() != nullptr);
    REQUIRE(input->Mouse() != nullptr);
    REQUIRE(input->Touch() != nullptr);

    CHECK_FALSE(input->Keyboard()->IsKeyDown(KeyCode::Space));
    CHECK(input->Keyboard()->Modifiers() == KeyModifiers::None);
    CHECK(input->Mouse()->X() == 0.0f);
    CHECK_FALSE(input->Mouse()->IsButtonDown(MouseButton::Left));
    CHECK(input->Mouse()->CursorVisible());
    CHECK_FALSE(input->Touch()->HasTouch());
    CHECK(input->GamepadCount() == 0);
    CHECK(input->GetGamepad(0) == nullptr);

    input->Update(); // must be a harmless no-op
}

TEST_CASE("shell.null: destroying the main window does not promote another window")
{
    NullShell shell;
    IWindowManager* wm = shell.WindowManager();
    IWindow* main = wm->MainWindow();
    REQUIRE(main != nullptr);
    const u32 mainId = main->Id();

    // A second window is open alongside the main window.
    Result<IWindow*> second = wm->CreateWindow(WindowSettings{});
    REQUIRE(second.HasValue());
    IWindow* secondary = second.Value();
    REQUIRE(secondary != main);
    REQUIRE(wm->MainWindow() == main); // still the first window, not the newest

    // Close and destroy the main window while the secondary stays open.
    main->Close();
    CHECK_FALSE(shell.IsRunning()); // main window closed -> shell stops
    wm->DestroyWindow(main);
    wm->FlushDestroyed();

    // The secondary is still live and open, but must NOT be promoted to main,
    // and IsRunning() must not flip back to true.
    CHECK(wm->GetWindow(secondary->Id()) == secondary);
    CHECK(secondary->IsOpen());
    CHECK(wm->MainWindow() == nullptr);
    CHECK(wm->GetWindow(mainId) == nullptr);
    CHECK_FALSE(shell.IsRunning());
}

TEST_CASE("shell.null: DestroyWindow ignores windows it does not own")
{
    NullShell a;
    NullShell b;
    IWindowManager* wmA = a.WindowManager();
    IWindowManager* wmB = b.WindowManager();

    IWindow* aMain = wmA->MainWindow();
    IWindow* bMain = wmB->MainWindow();
    REQUIRE(aMain != nullptr);
    REQUIRE(bMain != nullptr);
    // Each manager numbers ids independently, so the two main windows collide on
    // id: DestroyWindow must reject by pointer identity, not by id.
    REQUIRE(aMain->Id() == bMain->Id());

    // Ask A to destroy B's window (and a null). Both must be no-ops: B's window
    // stays open, and A's bookkeeping (its own same-id window) is untouched.
    wmA->DestroyWindow(bMain);
    wmA->DestroyWindow(nullptr);
    wmA->FlushDestroyed();

    CHECK(bMain->IsOpen());            // foreign window not closed
    CHECK(wmB->MainWindow() == bMain); // B unaffected
    CHECK(wmA->MainWindow() == aMain); // A's same-id window survived
    CHECK(wmA->Windows().Size() == 1u);
    CHECK(a.IsRunning());
    CHECK(b.IsRunning());
}

TEST_CASE("shell.null: window geometry + content scale round-trip")
{
    WindowSettings settings;
    settings.width = 400;
    settings.height = 300;
    settings.positioned = true;
    settings.x = 120;
    settings.y = 80;

    NullShell shell(settings);
    IWindow* w = shell.MainWindow();
    REQUIRE(w != nullptr);

    // Initial position comes from the settings (headless records it verbatim).
    CHECK(w->X() == 120);
    CHECK(w->Y() == 80);
    CHECK(w->ContentScale() == doctest::Approx(1.0f)); // headless default

    // Atomic move / resize round-trip (per-axis setters intentionally do not exist).
    w->SetPosition(-5, 42);
    CHECK(w->X() == -5);
    CHECK(w->Y() == 42);
    w->SetSize(1024u, 768u);
    CHECK(w->Width() == 1024u);
    CHECK(w->Height() == 768u);
}

TEST_CASE("shell.null: an unpositioned window defaults to the origin")
{
    NullShell shell(WindowSettings{}); // positioned = false
    IWindow* w = shell.MainWindow();
    REQUIRE(w != nullptr);
    CHECK(w->X() == 0);
    CHECK(w->Y() == 0);
}

TEST_CASE("shell.null: global mouse position is zero (no OS device)")
{
    NullShell shell(WindowSettings{});
    IMouse* mouse = shell.Input()->Mouse();
    REQUIRE(mouse != nullptr);
    CHECK(mouse->GlobalX() == 0.0f);
    CHECK(mouse->GlobalY() == 0.0f);
}

TEST_CASE("shell.null: dialog service cancels immediately (empty result, one callback)")
{
    NullShell shell(WindowSettings{});
    IDialogService* dialogs = shell.Dialogs();
    REQUIRE(dialogs != nullptr);

    // Each Show* must invoke the callback exactly once with an empty result (== cancel), so callers
    // can drive the dialog path uniformly on a headless backend.
    int calls = 0;
    usize lastCount = 999;
    auto cb = [&](Span<const String> paths)
    {
        ++calls;
        lastCount = paths.Size();
    };

    const FileFilter filters[] = {{u8"Images", u8"png;jpg"}, {u8"All", u8"*"}};

    dialogs->ShowOpenFile(cb, Span<const FileFilter>(filters, 2), u8"/tmp", true, 0);
    CHECK(calls == 1);
    CHECK(lastCount == 0u);

    dialogs->ShowSaveFile(cb, {}, u8"/tmp/out.scene", 0);
    CHECK(calls == 2);
    CHECK(lastCount == 0u);

    dialogs->ShowOpenFolder(cb, u8"/tmp", false, 0);
    CHECK(calls == 3);
    CHECK(lastCount == 0u);

    dialogs->OpenPath(u8"/tmp"); // headless no-op: callable, no callback, must not crash
    CHECK(calls == 3);
}
