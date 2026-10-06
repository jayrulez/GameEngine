// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Engine::GameInstance - a run's save file and the `Save` facade.

module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"
#include "Core/Reflection/Reflect.h" // the Save facade reflection body

module engine.gameinstance;

import foundation.core;
import foundation.settings;
import foundation.script;
import foundation.script.facades;  // RegisterExtraFacadeName
import foundation.xml.serialization; // a save file is XML, like the user settings beside it

using namespace foundation::core;
using namespace foundation;

namespace engine::runtime
{
    void RunSave::Open(StringView path)
    {
        m_path = String(path, *m_allocator);
        m_store = MakeUnique<settings::Settings>(*m_allocator, *m_allocator);
        m_changed = false;
        if (m_path.IsEmpty() || !FileExists(m_path.AsView()))
        {
            return;
        }
        settings::RegisterSaveValuesType(); // the load instantiates the section by its name
        FileStream file(m_path.AsView(), FileMode::Read);
        if (!file.IsValid() || !m_store->Load(file, xml::XmlSerializerFactory()).IsOk())
        {
            LOG_WARNING(u8"Save", u8"could not read the save '{}'; starting from an empty save",
                        m_path.AsView());
            m_store = MakeUnique<settings::Settings>(*m_allocator, *m_allocator);
        }
    }

    bool RunSave::Flush()
    {
        if (!IsOpen())
        {
            return false;
        }
        if (!m_changed)
        {
            return true;
        }
        MemoryStream buffer(*m_allocator);
        if (!m_store->Save(buffer, xml::XmlSerializerFactory()).IsOk())
        {
            LOG_WARNING(u8"Save", u8"could not encode the save '{}'", m_path.AsView());
            return false;
        }
        (void)CreateDirectories(PathParent(m_path.AsView()));
        if (!WriteFileAtomic(m_path.AsView(), buffer.Bytes(), *m_allocator).IsOk())
        {
            LOG_WARNING(u8"Save", u8"could not write the save '{}'", m_path.AsView());
            return false;
        }
        m_changed = false;
        PersistUserData(); // a browser keeps it only once pushed to the page's storage
        return true;
    }

    bool Save::has(core::String key)
    {
        RunSave* save = Resolve();
        return save != nullptr && save->Values().Has(key.AsView());
    }

    void Save::remove(core::String key)
    {
        if (RunSave* save = Resolve(); save != nullptr && save->Values().Remove(key.AsView()))
        {
            save->MarkChanged();
        }
    }

    void Save::clear()
    {
        if (RunSave* save = Resolve(); save != nullptr && save->Values().Clear())
        {
            save->MarkChanged();
        }
    }

    i32 Save::getInt(core::String key, i32 fallback)
    {
        RunSave* save = Resolve();
        return save != nullptr ? save->Values().GetInt(key.AsView(), fallback) : fallback;
    }

    void Save::setInt(core::String key, i32 value)
    {
        if (RunSave* save = Resolve(); save != nullptr && save->Values().SetInt(key.AsView(), value))
        {
            save->MarkChanged();
        }
    }

    f32 Save::getFloat(core::String key, f32 fallback)
    {
        RunSave* save = Resolve();
        return save != nullptr ? save->Values().GetFloat(key.AsView(), fallback) : fallback;
    }

    void Save::setFloat(core::String key, f32 value)
    {
        if (RunSave* save = Resolve(); save != nullptr && save->Values().SetFloat(key.AsView(), value))
        {
            save->MarkChanged();
        }
    }

    bool Save::getBool(core::String key, bool fallback)
    {
        RunSave* save = Resolve();
        return save != nullptr ? save->Values().GetBool(key.AsView(), fallback) : fallback;
    }

    void Save::setBool(core::String key, bool value)
    {
        if (RunSave* save = Resolve(); save != nullptr && save->Values().SetBool(key.AsView(), value))
        {
            save->MarkChanged();
        }
    }

    core::String Save::getString(core::String key, core::String fallback)
    {
        RunSave* save = Resolve();
        return save != nullptr ? save->Values().GetText(key.AsView(), fallback.AsView()) : fallback;
    }

    void Save::setString(core::String key, core::String value)
    {
        if (RunSave* save = Resolve();
            save != nullptr && save->Values().SetText(key.AsView(), value.AsView()))
        {
            save->MarkChanged();
        }
    }

    core::Array<f32> Save::getFloats(core::String key)
    {
        RunSave* save = Resolve();
        return save != nullptr ? save->Values().GetFloats(key.AsView()) : core::Array<f32>{};
    }

    void Save::setFloats(core::String key, core::Array<f32> values)
    {
        if (RunSave* save = Resolve(); save != nullptr &&
                                       save->Values().SetFloats(key.AsView(), Span<const f32>(values.Data(), values.Size())))
        {
            save->MarkChanged();
        }
    }

    bool Save::flush()
    {
        RunSave* save = Resolve();
        return save != nullptr && save->Flush();
    }

    REFLECT_MEMBERS(Save, "rtti::engine::runtime")
    {
        builder.Method<&Save::has>("has", {"key"});
        builder.Method<&Save::remove>("remove", {"key"});
        builder.Method<&Save::clear>("clear");
        builder.Method<&Save::getInt>("getInt", {"key", "fallback"});
        builder.Method<&Save::setInt>("setInt", {"key", "value"});
        builder.Method<&Save::getFloat>("getFloat", {"key", "fallback"});
        builder.Method<&Save::setFloat>("setFloat", {"key", "value"});
        builder.Method<&Save::getBool>("getBool", {"key", "fallback"});
        builder.Method<&Save::setBool>("setBool", {"key", "value"});
        builder.Method<&Save::getString>("getString", {"key", "fallback"});
        builder.Method<&Save::setString>("setString", {"key", "value"});
        builder.Method<&Save::getFloats>("getFloats", {"key"});
        builder.Method<&Save::setFloats>("setFloats", {"key", "values"});
        builder.Method<&Save::flush>("flush");
        builder.Constructor();
    }

    void RegisterSaveScriptFacade()
    {
        static const bool once = []()
        {
            settings::RegisterSaveValuesType(); // reading a save file instantiates the section by name
            RegisterArrayType<f32>();           // getFloats/setFloats: a list crosses as array<float>
            GlobalTypeRegistry().Register(Save::StaticType());
            foundation::script::RegisterExtraFacadeName(u8"Save");
            return true;
        }();
        (void)once;
    }
}
