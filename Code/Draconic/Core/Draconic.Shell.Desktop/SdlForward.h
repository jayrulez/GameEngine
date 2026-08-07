// Forward declarations of the opaque SDL3 handle types the draconic.shell.desktop interface
// references (as pointers only). Included from the global module fragment so the interface
// stays free of the heavy <SDL3/SDL.h>; the real definitions live in SDL3ShellImpl.cpp.
#pragma once

struct SDL_Window;
struct SDL_Cursor;
struct SDL_Gamepad;
