// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Foundation::Net.Tls.MbedTLS - the `foundation.net.tls.mbedtls` module.
///
/// The Mbed TLS backend of the TLS seam (foundation.net.tls): TLS 1.2 and 1.3, the client checking
/// the server's name and chain against its TlsTrust, the server presenting its chain. The only
/// module that links ThirdParty::MbedTLS; its headers stay in the implementation unit, so nothing
/// that imports this sees them. Desktop and the Deck compose it; a web build has none (the browser
/// does TLS).
///
/// It also makes a self-signed certificate (a P-256 key, ECDSA with SHA-256): what a test serves,
/// and what a host on a LAN can serve to clients that trust it explicitly. A certificate a browser
/// accepts without asking comes from a CA (Let's Encrypt), not from here.

module;
#include "Core/Prelude.h"

export module foundation.net.tls.mbedtls;

import foundation.core;
import foundation.net.tls;

using namespace foundation::core;

export namespace foundation::net
{
    /// The Mbed TLS backend; sessions it makes come from `allocator`.
    [[nodiscard]] UniquePtr<ITlsBackend> CreateMbedTlsBackend(IAllocator& allocator);

    struct TlsCertificate
    {
        String certificatePem;
        String privateKeyPem;
    };

    /// A self-signed certificate for `name` (its subject's CN and its one DNS name), valid from
    /// `notBefore` to `notAfter` (UTC, "YYYYMMDDhhmmss"), with a new P-256 key. False, `error` saying
    /// why, when it could not be made.
    [[nodiscard]] bool CreateSelfSignedCertificate(StringView name, StringView notBefore, StringView notAfter, TlsCertificate& out,
                                                   String& error);

    /// "YYYYMMDDhhmmss" for now plus `days` (negative: before now), UTC, for a certificate's validity.
    [[nodiscard]] String CertificateTime(i32 days);
}
