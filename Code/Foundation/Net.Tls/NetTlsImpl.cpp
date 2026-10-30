// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// foundation.net.tls - implementation: the system's trusted roots.

module;
#include "Core/Prelude.h"
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wincrypt.h>
#endif

module foundation.net.tls;

import foundation.core;

using namespace foundation::core;

namespace foundation::net
{
    TlsRoots LoadSystemRoots(String* where)
    {
        TlsRoots roots;
#if defined(_WIN32)
        // The machine's trusted roots, as Windows' own TLS checks them.
        HCERTSTORE store = ::CertOpenSystemStoreW(0, L"ROOT");
        if (store == nullptr)
        {
            if (where != nullptr)
                *where = String(u8"the Windows ROOT store did not open");
            return roots;
        }
        for (PCCERT_CONTEXT cert = ::CertEnumCertificatesInStore(store, nullptr); cert != nullptr;
             cert = ::CertEnumCertificatesInStore(store, cert))
        {
            if (cert->dwCertEncodingType != X509_ASN_ENCODING)
                continue;
            Array<byte> der;
            der.Resize(cert->cbCertEncoded);
            for (DWORD i = 0; i < cert->cbCertEncoded; ++i)
                der[i] = static_cast<byte>(cert->pbCertEncoded[i]);
            roots.der.PushBack(Move(der));
        }
        ::CertCloseStore(store, 0);
        if (where != nullptr)
            *where = Format(u8"the Windows ROOT store ({} certificates)", roots.der.Size());
#elif PLATFORM_WEB
        // A browser checks certificates itself; a web build has no TLS backend.
        if (where != nullptr)
            *where = String(u8"none: the browser does TLS");
#else
        // The bundle a distribution keeps (Debian/Ubuntu/SteamOS/Arch, Fedora/RHEL, openSUSE,
        // Alpine), the first that exists.
        static constexpr StringView kBundles[] = {
            u8"/etc/ssl/certs/ca-certificates.crt",
            u8"/etc/pki/tls/certs/ca-bundle.crt",
            u8"/etc/ssl/ca-bundle.pem",
            u8"/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem",
            u8"/etc/ssl/cert.pem",
        };
        for (StringView path : kBundles)
        {
            Result<Array<byte>> bytes = ReadFile(path);
            if (!bytes.HasValue() || bytes.Value().IsEmpty())
                continue;
            roots.pem = String(StringView(reinterpret_cast<const utf8char*>(bytes.Value().Data()), bytes.Value().Size()));
            if (where != nullptr)
                *where = String(path);
            return roots;
        }
        if (where != nullptr)
            *where = String(u8"no bundle at /etc/ssl/certs/ca-certificates.crt or the other usual paths");
#endif
        return roots;
    }
}
