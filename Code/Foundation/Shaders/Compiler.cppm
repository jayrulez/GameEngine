// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// DXC shader compiler: loads dxcompiler.dll or libdxcompiler.so at runtime.

module;

#include "DxcIncludes.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifndef _WIN32
#include <dlfcn.h>
#endif

export module foundation.shaders:compiler;

import foundation.core;
import :types;
import :properties;

using namespace foundation::core;

export namespace foundation::shaders
{

    /// Configuration for compiler creation.
    struct CompilerDesc
    {
        /// Optional override path to the DXC shared library.
        StringView dxcompilerPath{};
    };

    /// HLSL shader compiler backed by DXC (IDxcCompiler3).
    struct Compiler
    {
        void* state = nullptr;
        IAllocator* m_allocator = nullptr;

        [[nodiscard]] Status compile(const u8* source, usize sourceSize, ShaderStage stage,
                                     StringView entryPoint, ShaderTarget target,
                                     const CompileOptions& options, CompileResult& out);

        void freeResult(CompileResult& result);
        void Destroy();
    };

    [[nodiscard]] Status createCompiler(const CompilerDesc& desc, Compiler*& out,
                                        IAllocator& allocator = DefaultAllocator());

} // namespace foundation::shaders (exported)

// ---- Implementation ----

namespace foundation::shaders
{

#ifdef _WIN32
    using DynLibHandle = HMODULE;
#else
    using DynLibHandle = void*;
#endif

    struct CompilerState
    {
        DynLibHandle dxcompiler = nullptr;
        DxcCreateInstanceProc createInst = nullptr;
        IDxcCompiler3* dxc = nullptr;
        IDxcUtils* utils = nullptr;
        IDxcIncludeHandler* includeHdlr = nullptr;
    };

    static CompilerState* stateOf(Compiler* c) { return static_cast<CompilerState*>(c->state); }

    // wchar_t (DXC) to a UTF-8 std::string. The inverse of widen() below: on Windows (UTF-16)
    // surrogate pairs recombine; on Linux (UTF-32) each unit is a code point.
    static std::string narrow(const wchar_t* text)
    {
        std::string out;
        if (text == nullptr)
        {
            return out;
        }
        for (usize i = 0; text[i] != 0; ++i)
        {
            uint32_t cp = static_cast<uint32_t>(text[i]);
            if (cp >= 0xD800 && cp <= 0xDBFF && text[i + 1] != 0)
            {
                const uint32_t low = static_cast<uint32_t>(text[i + 1]);
                if (low >= 0xDC00 && low <= 0xDFFF)
                {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                    ++i;
                }
            }
            if (cp < 0x80)
            {
                out.push_back(static_cast<char>(cp));
            }
            else if (cp < 0x800)
            {
                out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            }
            else if (cp < 0x10000)
            {
                out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            }
            else
            {
                out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
                out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            }
        }
        return out;
    }

    // An include path as the preprocessor formed it, in the form a resolver and a `#line` want:
    // backslashes become '/', and any leading "./" is stripped (DXC prefixes the includer's
    // "current dir" that way).
    static std::string normalizeIncludePath(LPCWSTR pFilename)
    {
        std::string path = narrow(pFilename);
        for (char& c : path)
        {
            if (c == '\\')
            {
                c = '/';
            }
        }
        while (path.size() >= 2 && path[0] == '.' && path[1] == '/')
        {
            path.erase(0, 2);
        }
        return path;
    }

    // The one include handler every compile gives DXC. The preprocessor forms each candidate path
    // itself (for a quoted include: the includer's directory first, then each -I directory in
    // order) and asks for it. This handler serves the candidate from the caller's resolver when
    // one is set (no disk access; a miss is "not found"), else from disk through DXC's default
    // handler. Either way an `effect` block in the served text is expanded before DXC sees it,
    // so an effect's include works on the cook's disk route and the editor's resolver route
    // alike. A text without an effect block is served as it was read. The handler lives for one
    // Compile call (stack-owned; DXC does not retain it), so the refcount is nominal.
    class EngineIncludeHandler final : public IDxcIncludeHandler
    {
    public:
        EngineIncludeHandler(IDxcUtils& utils, IShaderIncludeResolver* resolver,
                             IDxcIncludeHandler& disk) noexcept
            : m_utils(&utils), m_resolver(resolver), m_disk(&disk)
        {
        }

        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override
        {
            if (ppvObject == nullptr)
            {
                return E_POINTER;
            }
            if (IsEqualIID(riid, __uuidof(IUnknown)) ||
                IsEqualIID(riid, __uuidof(IDxcIncludeHandler)))
            {
                *ppvObject = static_cast<IDxcIncludeHandler*>(this);
                AddRef();
                return S_OK;
            }
            *ppvObject = nullptr;
            return E_NOINTERFACE;
        }
        ULONG STDMETHODCALLTYPE AddRef() override { return ++m_refs; }
        ULONG STDMETHODCALLTYPE Release() override { return m_refs > 0 ? --m_refs : 0; }

        HRESULT STDMETHODCALLTYPE LoadSource(LPCWSTR pFilename,
                                             IDxcBlob** ppIncludeSource) override
        {
            if (ppIncludeSource == nullptr)
            {
                return E_POINTER;
            }
            *ppIncludeSource = nullptr;
            const std::string path = normalizeIncludePath(pFilename);
            const StringView pathView(reinterpret_cast<const utf8char*>(path.data()), path.size());
            if (m_resolver != nullptr)
            {
                String source;
                if (!m_resolver->LoadInclude(pathView, source))
                {
                    return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
                }
                return Serve(source.AsView(), pathView, ppIncludeSource);
            }

            IDxcBlob* diskBlob = nullptr;
            const HRESULT hr = m_disk->LoadSource(pFilename, &diskBlob);
            if (FAILED(hr) || diskBlob == nullptr)
            {
                return FAILED(hr) ? hr : HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
            }
            StringView text(static_cast<const utf8char*>(diskBlob->GetBufferPointer()),
                            diskBlob->GetBufferSize());
            if (text.Size() >= 3 && static_cast<u8>(text.Data()[0]) == 0xEFu &&
                static_cast<u8>(text.Data()[1]) == 0xBBu &&
                static_cast<u8>(text.Data()[2]) == 0xBFu)
            {
                text = text.SubStr(3, text.Size() - 3); // a UTF-8 byte order mark
            }
            if (!HasShaderEffect(text))
            {
                *ppIncludeSource = diskBlob; // exactly what DXC's own handler read
                return S_OK;
            }
            const HRESULT served = Serve(text, pathView, ppIncludeSource);
            diskBlob->Release();
            return served;
        }

    private:
        // The text as DXC compiles it: effect blocks expanded (a block with errors becomes
        // `#error` lines at their lines in this file, which fail the compile naming them).
        HRESULT Serve(StringView text, StringView fileName, IDxcBlob** ppIncludeSource)
        {
            String expanded;
            if (HasShaderEffect(text))
            {
                (void)ExpandShaderEffects(text, fileName, expanded);
                text = expanded.AsView();
            }
            IDxcBlobEncoding* blob = nullptr;
            const HRESULT hr = m_utils->CreateBlob(text.Data(), static_cast<UINT32>(text.Size()),
                                                   DXC_CP_UTF8, &blob);
            if (FAILED(hr) || blob == nullptr)
            {
                return FAILED(hr) ? hr : E_FAIL;
            }
            *ppIncludeSource = blob;
            return S_OK;
        }

        IDxcUtils* m_utils;
        IShaderIncludeResolver* m_resolver; // null = disk
        IDxcIncludeHandler* m_disk;         // DXC's default handler, owned by the compiler
        ULONG m_refs = 1;
    };

    // StringView (UTF-8) to std::wstring for DXC. Decodes UTF-8 codepoints,
    // then on Windows (wchar_t = UTF-16) emits surrogate pairs for astral code
    // points; on Linux (wchar_t = UTF-32) emits the codepoint directly.
    static std::wstring widen(StringView s)
    {
        std::wstring out;
        out.reserve(s.Size());
        usize i = 0;
        while (i < s.Size())
        {
            const u8 lead = static_cast<u8>(s.Data()[i]);
            ++i;
            char32_t cp;
            int extra;
            if (lead < 0x80u)
            {
                cp = lead;
                extra = 0;
            }
            else if ((lead & 0xE0u) == 0xC0u)
            {
                cp = lead & 0x1Fu;
                extra = 1;
            }
            else if ((lead & 0xF0u) == 0xE0u)
            {
                cp = lead & 0x0Fu;
                extra = 2;
            }
            else if ((lead & 0xF8u) == 0xF0u)
            {
                cp = lead & 0x07u;
                extra = 3;
            }
            else
            {
                cp = 0xFFFDu;
                extra = 0;
            }
            for (int k = 0; k < extra && i < s.Size(); ++k)
            {
                cp = (cp << 6) | (static_cast<u8>(s.Data()[i]) & 0x3Fu);
                ++i;
            }
#ifdef _WIN32
            if (cp <= 0xFFFFu)
            {
                out.push_back(static_cast<wchar_t>(cp));
            }
            else
            {
                cp -= 0x10000u;
                out.push_back(static_cast<wchar_t>(0xD800u + (cp >> 10)));
                out.push_back(static_cast<wchar_t>(0xDC00u + (cp & 0x3FFu)));
            }
#else
            out.push_back(static_cast<wchar_t>(cp));
#endif
        }
        return out;
    }

    static const wchar_t* stagePrefix(ShaderStage stage)
    {
        switch (stage)
        {
        case ShaderStage::Vertex:
            return L"vs";
        case ShaderStage::Fragment:
            return L"ps";
        case ShaderStage::Compute:
            return L"cs";
        case ShaderStage::Mesh:
            return L"ms";
        case ShaderStage::Task:
            return L"as";
        case ShaderStage::RayGen:
        case ShaderStage::ClosestHit:
        case ShaderStage::AnyHit:
        case ShaderStage::Miss:
        case ShaderStage::Intersection:
        case ShaderStage::Callable:
            return L"lib";
        }
        return L"vs";
    }

    Status Compiler::compile(const u8* source, usize sourceSize, ShaderStage stage,
                             StringView entryPoint, ShaderTarget target,
                             const CompileOptions& options, CompileResult& out)
    {
        auto* s = stateOf(this);
        out = {};

        std::vector<std::wstring> argStorage;
        argStorage.reserve(64);
        auto push = [&](const wchar_t* a) { argStorage.emplace_back(a); };
        auto pushS = [&](std::wstring a) { argStorage.emplace_back(std::move(a)); };

        push(L"-E");
        pushS(entryPoint.IsEmpty() ? L"main" : widen(entryPoint));

        std::wstring profile;
        profile.append(stagePrefix(stage));
        profile.append(L"_");
        profile.append(widen(options.shaderModel));
        push(L"-T");
        pushS(std::move(profile));

        if (target == ShaderTarget::SPIRV)
        {
            push(L"-spirv");
            if (options.preserveInterface)
            {
                push(L"-fspv-preserve-interface");
            }
            std::wstring targetEnv = L"-fspv-target-env=";
            targetEnv.append(widen(options.spirvTargetEnvironment));
            pushS(std::move(targetEnv));
            for (u32 set = 0; set < options.bindingShiftSets; ++set)
            {
                wchar_t setBuf[16];
                swprintf(setBuf, 16, L"%u", set);
                std::wstring setStr = setBuf;
                auto pushShift = [&](const wchar_t* flag, u32 shift)
                {
                    if (shift == 0)
                        return;
                    wchar_t shBuf[16];
                    swprintf(shBuf, 16, L"%u", shift);
                    push(flag);
                    pushS(shBuf);
                    pushS(setStr);
                };
                pushShift(L"-fvk-b-shift", options.bindingShifts.constantBufferShift);
                pushShift(L"-fvk-t-shift", options.bindingShifts.textureShift);
                pushShift(L"-fvk-s-shift", options.bindingShifts.samplerShift);
                pushShift(L"-fvk-u-shift", options.bindingShifts.uavShift);
            }
        }

        push(options.rowMajorMatrices ? L"-Zpr" : L"-Zpc");

        switch (options.optimizationLevel)
        {
        case 0:
            push(L"-O0");
            break;
        case 1:
            push(L"-O1");
            break;
        case 2:
            push(L"-O2");
            break;
        default:
            push(L"-O3");
            break;
        }
        if (options.enableDebugInfo)
            push(L"-Zi");

        for (usize i = 0; i < options.defines.Size(); ++i)
        {
            std::wstring arg = L"-D";
            arg.append(widen(options.defines[i].name));
            if (!options.defines[i].value.IsEmpty())
            {
                arg.append(L"=");
                arg.append(widen(options.defines[i].value));
            }
            pushS(std::move(arg));
        }
        // The search path the preprocessor forms disk candidates from (EngineIncludeHandler
        // serves them). A resolver owns every include, so it gets none.
        if (options.includeResolver == nullptr)
        {
            for (usize i = 0; i < options.includePaths.Size(); ++i)
            {
                push(L"-I");
                pushS(widen(options.includePaths[i]));
            }
        }
        push(L"-Wno-ignored-attributes");
        if (!options.sourceName.IsEmpty())
        {
            pushS(widen(options.sourceName)); // the positional argument: the input's file name
        }

        std::vector<LPCWSTR> args;
        args.reserve(argStorage.size());
        for (const auto& w : argStorage)
            args.push_back(w.c_str());

        // An effect block in the source itself expands as one in an include does (after any
        // properties expansion, which the caller has made).
        const StringView sourceText(reinterpret_cast<const utf8char*>(source), sourceSize);
        String expandedSource;
        if (HasShaderEffect(sourceText))
        {
            (void)ExpandShaderEffects(sourceText, options.sourceName, expandedSource);
            source = reinterpret_cast<const u8*>(expandedSource.Data());
            sourceSize = expandedSource.Size();
        }

        DxcBuffer src{};
        src.Ptr = source;
        src.Size = sourceSize;
        src.Encoding = DXC_CP_UTF8;
        IDxcResult* result = nullptr;
        EngineIncludeHandler includeHandler(*s->utils, options.includeResolver, *s->includeHdlr);
        const HRESULT hr = s->dxc->Compile(&src, args.data(), static_cast<UINT32>(args.size()),
                                           &includeHandler, IID_PPV_ARGS(&result));

        if (FAILED(hr) || !result)
        {
            std::string msg = "IDxcCompiler3::Compile returned HRESULT 0x" +
                              std::to_string(static_cast<unsigned>(hr));
            out.messagesSize = msg.size();
            out.messages = new char[msg.size() + 1];
            std::memcpy(out.messages, msg.data(), msg.size());
            out.messages[msg.size()] = '\0';
            return ErrorCode::Unknown;
        }

        IDxcBlobUtf8* errBlob = nullptr;
        IDxcBlobWide* errName = nullptr;
        if (SUCCEEDED(result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errBlob), &errName)) &&
            errBlob && errBlob->GetStringLength() > 0)
        {
            auto n = errBlob->GetStringLength();
            out.messagesSize = n;
            out.messages = new char[n + 1];
            std::memcpy(out.messages, errBlob->GetStringPointer(), n);
            out.messages[n] = '\0';
        }
        else
        {
            out.messagesSize = 0;
            out.messages = new char[1];
            out.messages[0] = '\0';
        }
        if (errBlob)
        {
            errBlob->Release();
        }
        if (errName)
        {
            errName->Release();
        }

        HRESULT status = S_OK;
        result->GetStatus(&status);
        if (SUCCEEDED(status))
        {
            IDxcBlob* objBlob = nullptr;
            IDxcBlobWide* objName = nullptr;
            if (SUCCEEDED(result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&objBlob), &objName)) &&
                objBlob && objBlob->GetBufferSize() > 0)
            {
                auto sz = objBlob->GetBufferSize();
                out.bytecodeSize = sz;
                out.bytecode = new u8[sz];
                std::memcpy(out.bytecode, objBlob->GetBufferPointer(), sz);
                out.success = true;
            }
            if (objBlob)
            {
                objBlob->Release();
            }
            if (objName)
            {
                objName->Release();
            }
        }
        result->Release();
        return out.success ? ErrorCode::Ok : ErrorCode::Unknown;
    }

    void Compiler::freeResult(CompileResult& r)
    {
        delete[] r.bytecode;
        delete[] r.messages;
        r = {};
    }

    void Compiler::Destroy()
    {
        auto* s = stateOf(this);
        if (!s)
            return;
        if (s->includeHdlr)
        {
            s->includeHdlr->Release();
            s->includeHdlr = nullptr;
        }
        if (s->utils)
        {
            s->utils->Release();
            s->utils = nullptr;
        }
        if (s->dxc)
        {
            s->dxc->Release();
            s->dxc = nullptr;
        }
#ifdef _WIN32
        if (s->dxcompiler)
        {
            FreeLibrary(s->dxcompiler);
            s->dxcompiler = nullptr;
        }
#else
        if (s->dxcompiler)
        {
            dlclose(s->dxcompiler);
            s->dxcompiler = nullptr;
        }
#endif
        IAllocator& alloc = *m_allocator;
        alloc.Delete(s);
        this->state = nullptr;
        alloc.Delete(this);
    }

    Status createCompiler(const CompilerDesc& desc, Compiler*& out, IAllocator& allocator)
    {
        out = nullptr;
        auto* c = allocator.New<Compiler>();
        auto* s = allocator.New<CompilerState>();
        c->state = s;
        c->m_allocator = &allocator;

#ifdef _WIN32
        {
            std::wstring wpath;
            if (!desc.dxcompilerPath.IsEmpty())
            {
                wpath = widen(desc.dxcompilerPath);
#ifdef DRACO_DXC_PATH
            }
            else
            {
                wpath = widen(DRACO_DXC_PATH);
                s->dxcompiler = LoadLibraryW(wpath.c_str());
                if (!s->dxcompiler)
                    wpath = L"dxcompiler.dll";
#else
            }
            else
            {
                wpath = L"dxcompiler.dll";
#endif
            }
            if (!s->dxcompiler)
                s->dxcompiler = LoadLibraryW(wpath.c_str());
        }
        if (!s->dxcompiler)
        {
            std::fprintf(stderr,
                         "foundation.shaders: LoadLibraryW(dxcompiler.dll) failed (error %lu)\n",
                         GetLastError());
            c->Destroy();
            return ErrorCode::Unknown;
        }
        s->createInst = reinterpret_cast<DxcCreateInstanceProc>(
            GetProcAddress(s->dxcompiler, "DxcCreateInstance"));
#else
        {
            std::string path;
            if (desc.dxcompilerPath.IsEmpty())
            {
#ifdef BUILDSYSTEM_DXC_PATH
                path = BUILDSYSTEM_DXC_PATH;
#else
                path = "libdxcompiler.so";
#endif
            }
            else
            {
                // dxcompilerPath is already UTF-8, so dlopen takes it directly.
                path.assign(reinterpret_cast<const char*>(desc.dxcompilerPath.Data()),
                            desc.dxcompilerPath.Size());
            }
            s->dxcompiler = dlopen(path.c_str(), RTLD_LAZY | RTLD_LOCAL);
            // Fallback for a relocated dist: BUILDSYSTEM_DXC_PATH is the absolute path of the
            // vendored library in the source tree, which does not exist on another machine. Retry
            // the bare soname so the dynamic loader searches the binary's RUNPATH ($ORIGIN, where
            // libdxcompiler.so is staged beside the executable), LD_LIBRARY_PATH and the system
            // dirs, as the Win32 dxcompiler.dll fallback above does.
            if (s->dxcompiler == nullptr && path != "libdxcompiler.so")
            {
                s->dxcompiler = dlopen("libdxcompiler.so", RTLD_LAZY | RTLD_LOCAL);
            }
        }
        if (!s->dxcompiler)
        {
            std::fprintf(stderr, "foundation.shaders: dlopen(libdxcompiler.so) failed: %s\n",
                         dlerror());
            c->Destroy();
            return ErrorCode::Unknown;
        }
        s->createInst =
            reinterpret_cast<DxcCreateInstanceProc>(dlsym(s->dxcompiler, "DxcCreateInstance"));
#endif

        if (!s->createInst)
        {
            std::fprintf(stderr, "foundation.shaders: DXC library missing DxcCreateInstance\n");
            c->Destroy();
            return ErrorCode::Unknown;
        }
        if (FAILED(s->createInst(CLSID_DxcCompiler, IID_PPV_ARGS(&s->dxc))))
        {
            std::fprintf(stderr, "foundation.shaders: DxcCreateInstance(IDxcCompiler3) failed\n");
            c->Destroy();
            return ErrorCode::Unknown;
        }
        if (FAILED(s->createInst(CLSID_DxcUtils, IID_PPV_ARGS(&s->utils))))
        {
            std::fprintf(stderr, "foundation.shaders: DxcCreateInstance(IDxcUtils) failed\n");
            c->Destroy();
            return ErrorCode::Unknown;
        }
        if (FAILED(s->utils->CreateDefaultIncludeHandler(&s->includeHdlr)))
        {
            std::fprintf(stderr, "foundation.shaders: CreateDefaultIncludeHandler failed\n");
            c->Destroy();
            return ErrorCode::Unknown;
        }

        out = c;
        return ErrorCode::Ok;
    }

} // namespace foundation::shaders
