// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// A plugin part's entry point, written once in the part's source:
//
//     #include "Runtime/PluginEntry.h"
//     import foundation.runtime;
//     ...
//     PLUGIN_PART(&LoadMyPart, &UnloadMyPart)
//
// util_add_plugin_part (cmake/Plugins.cmake) gives each part's sources the manifest's names, so
// the entry is `PluginPart_<id>_<Kind>` and its record carries the id and kind without the
// source writing either. The record is constant: linking a part does no work until a host
// loads it. `unload` may be nullptr.
//
// Use it once per part, in a translation unit that imports foundation.runtime.

#ifndef FOUNDATION_RUNTIME_PLUGIN_ENTRY_H
#define FOUNDATION_RUNTIME_PLUGIN_ENTRY_H

#if !defined(PLUGIN_PART_ENTRY_NAME) || !defined(PLUGIN_PART_ID) || !defined(PLUGIN_PART_KIND)
#error "PluginEntry.h is for a plugin part's sources: declare the part with util_add_plugin_part()"
#endif

#if defined(_WIN32)
#define PLUGIN_PART_EXPORT __declspec(dllexport)
#else
#define PLUGIN_PART_EXPORT __attribute__((visibility("default")))
#endif

#define PLUGIN_PART(loadFn, unloadFn)                                                                    \
    extern "C" PLUGIN_PART_EXPORT const ::foundation::runtime::PluginPart* PLUGIN_PART_ENTRY_NAME()      \
    {                                                                                                    \
        static constexpr ::foundation::runtime::PluginPart part{                                         \
            PLUGIN_PART_ID, ::foundation::runtime::PluginPartKind::PLUGIN_PART_KIND, loadFn, unloadFn};  \
        return &part;                                                                                    \
    }

#endif
