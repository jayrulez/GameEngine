// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Engine::GameInstance - :save partition.
//
// A run's save: the values a game keeps between runs (a best time, an unlocked level, its own
// options), the file they live in, and whether they changed since the last write. The host names
// the file (the player in the user data directory, play in editor in the project's Editor/
// folder); the run's script contexts reach it through the `Save` facade. The file is an ordinary
// settings file holding one SaveValues section, written whole and atomically. See
// Documentation/Specs/save-data.md.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h" // the Save facade (RTTI_OBJECT)

export module engine.gameinstance:save;

import foundation.core;
import foundation.settings;
import foundation.script;

using namespace foundation::core;
using namespace foundation;
namespace core = foundation::core;

export namespace engine::runtime
{
    namespace script = foundation::script;

    inline constexpr StringView kSaveScriptService = u8"run.save";

    class RunSave
    {
    public:
        explicit RunSave(IAllocator& allocator)
            : m_allocator(&allocator), m_path(allocator),
              m_store(MakeUnique<settings::Settings>(allocator, allocator))
        {
        }
        RunSave(const RunSave&) = delete;
        RunSave& operator=(const RunSave&) = delete;

        // Names the save's file and reads it. Absent is an empty save; unreadable is an empty save
        // and a warning, and the file is left as it is until the game writes. An empty path closes
        // the save: values stay readable but are written nowhere.
        void Open(StringView path);

        [[nodiscard]] bool IsOpen() const noexcept { return !m_path.IsEmpty(); }
        [[nodiscard]] StringView Path() const noexcept { return m_path.AsView(); }

        [[nodiscard]] settings::SaveValues& Values() { return m_store->Section<settings::SaveValues>(); }

        // A change the next Flush writes. The facade calls it when a setter reports a change.
        void MarkChanged() noexcept { m_changed = true; }
        [[nodiscard]] bool HasChanges() const noexcept { return m_changed; }

        // Writes the values if they changed since the last write. True when the file holds them
        // afterwards (nothing to write counts); false when the write failed or there is no file.
        bool Flush();

    private:
        IAllocator* m_allocator;
        String m_path;
        // The file's whole settings store: the values' section, and any section a newer build
        // wrote, kept so this build's write does not drop it.
        UniquePtr<settings::Settings> m_store;
        bool m_changed = false;
    };

    // The `Save` script facade: a static facade over the calling context's run save, the same
    // shape on both backends (`Save::setInt("best.level2", 4210)`, `Save.getInt(...)` in Luau). A
    // context with no run save (an editor tool) reads every fallback and writes nowhere.
    class Save final : public Object
    {
        RTTI_OBJECT(Save, Object)
    public:
        [[nodiscard]] static RunSave* Resolve()
        {
            script::IScriptContext* context = script::CurrentScriptContext();
            return context != nullptr ? static_cast<RunSave*>(context->GetService(kSaveScriptService))
                                      : nullptr;
        }

        [[nodiscard]] static bool has(core::String key);
        static void remove(core::String key);
        static void clear();

        [[nodiscard]] static i32 getInt(core::String key, i32 fallback);
        static void setInt(core::String key, i32 value);
        [[nodiscard]] static f32 getFloat(core::String key, f32 fallback);
        static void setFloat(core::String key, f32 value);
        [[nodiscard]] static bool getBool(core::String key, bool fallback);
        static void setBool(core::String key, bool value);
        [[nodiscard]] static core::String getString(core::String key, core::String fallback);
        static void setString(core::String key, core::String value);
        // A list of numbers (a recorded run): set whole, read whole; empty when absent or another
        // kind. AngelScript `array<float>`, Luau a table.
        [[nodiscard]] static core::Array<f32> getFloats(core::String key);
        static void setFloats(core::String key, core::Array<f32> values);

        // Writes now if anything changed; false if the write failed or the run has no save file.
        static bool flush();
    };

    inline void InstallSaveScriptService(script::IScriptContext& context, RunSave& save)
    {
        context.SetService(kSaveScriptService, &save);
    }

    // Registers the `Save` facade (and the save section's type, which reading a save file needs).
    // Idempotent; the run domain calls it with the run facade.
    void RegisterSaveScriptFacade();
}
