// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Foundation::Net.Tls - the `foundation.net.tls` module.
///
/// The TLS seam (Documentation/Specs/tls-http-wss.md). A TLS session is a byte engine that never
/// touches a socket (a "memory BIO"): the caller hands it what arrived from the peer, takes what it
/// must send, and reads and writes plaintext. So any transport carries it (a TcpSocket, a test's
/// in-memory pipe), two sessions wired together run a whole handshake headlessly, and a backend is
/// only the crypto. The backends live in modules of their own (foundation.net.tls.mbedtls, the
/// first); the application chooses one at composition, and nothing else includes a TLS library.
///
/// A client checks the server's certificate against a TlsTrust: the system's roots (the default;
/// LoadSystemRoots), roots the game ships as PEM (a private backend's own CA), or, for a test only,
/// nothing (InsecureForTests: never the default, never reachable from a script).
///
/// Threading: a session is single-threaded; a backend makes sessions from any one thread at a time.

module;
#include "Core/Prelude.h"

export module foundation.net.tls;

import foundation.core;

using namespace foundation::core;

export namespace foundation::net
{
    /// Certificates a client trusts: a PEM bundle, and certificates in DER (the Windows store
    /// hands them so). Either may be empty.
    struct TlsRoots
    {
        String pem;
        Array<Array<byte>> der;
        [[nodiscard]] bool IsEmpty() const noexcept { return pem.IsEmpty() && der.IsEmpty(); }
    };

    enum class TlsTrustKind : u8
    {
        System,           ///< the system's roots (LoadSystemRoots), the default
        Roots,            ///< `roots` alone (a game's own CA)
        InsecureForTests, ///< no check at all: tests only
    };

    struct TlsTrust
    {
        TlsTrustKind kind = TlsTrustKind::System;
        TlsRoots roots; ///< Roots: what is trusted

        [[nodiscard]] static TlsTrust System() { return {}; }
        [[nodiscard]] static TlsTrust FromPem(StringView pem)
        {
            TlsTrust t;
            t.kind = TlsTrustKind::Roots;
            t.roots.pem = String(pem);
            return t;
        }
        [[nodiscard]] static TlsTrust InsecureForTests()
        {
            TlsTrust t;
            t.kind = TlsTrustKind::InsecureForTests;
            return t;
        }
    };

    enum class TlsVersion : u8
    {
        Tls12,
        Tls13,
    };

    struct TlsClientConfig
    {
        /// The host the client asks for (SNI) and checks the certificate is for; required unless
        /// the trust is InsecureForTests.
        String serverName;
        TlsTrust trust;
        Array<String> alpn; ///< protocols offered, in preference order ("http/1.1")
        TlsVersion minimum = TlsVersion::Tls12;
        TlsVersion maximum = TlsVersion::Tls13;
    };

    struct TlsServerConfig
    {
        String certificateChainPem; ///< the server's certificate first, then its issuers
        String privateKeyPem;
        Array<String> alpn; ///< protocols accepted, in preference order
        TlsVersion minimum = TlsVersion::Tls12;
        TlsVersion maximum = TlsVersion::Tls13;
    };

    enum class TlsState : u8
    {
        Handshaking, ///< keys not yet agreed: plaintext written now waits for Open
        Open,        ///< data flows both ways
        Closed,      ///< the peer closed (close_notify), or Close was called
        Failed,      ///< it failed; Failure() says why
    };

    /// One connection's TLS, either end, over whatever carries its bytes.
    class ITlsSession
    {
    public:
        virtual ~ITlsSession() = default;

        /// Bytes that arrived from the peer; the handshake moves on with them. An error when the
        /// session has failed (Failure says why).
        virtual Status Receive(Span<const byte> fromPeer) = 0;
        /// Appends what must go to the peer to `toPeer`; returns how many bytes.
        virtual usize TakeOutgoing(Array<byte>& toPeer) = 0;
        /// Up to `plaintext.Size()` bytes the peer sent, decrypted; 0 when none are ready.
        virtual usize Read(Span<byte> plaintext) = 0;
        /// Plaintext to send: encrypted into the outgoing bytes once Open (held until then).
        virtual Status Write(Span<const byte> plaintext) = 0;
        /// Tells the peer the connection ends (close_notify); the session is Closed after.
        virtual void Close() = 0;

        [[nodiscard]] virtual TlsState State() const = 0;
        /// Why it failed, said for a person ("the certificate is not for example.com"); empty
        /// otherwise.
        [[nodiscard]] virtual StringView Failure() const = 0;
        /// The protocol ALPN agreed on; empty when none was.
        [[nodiscard]] virtual StringView AlpnChosen() const = 0;
        /// The TLS version agreed on (valid once Open).
        [[nodiscard]] virtual TlsVersion Version() const = 0;
    };

    /// Makes sessions. A session made from a configuration that cannot work (a key that does not
    /// parse, no roots to trust) comes back Failed, saying why, rather than null.
    class ITlsBackend
    {
    public:
        virtual ~ITlsBackend() = default;
        [[nodiscard]] virtual UniquePtr<ITlsSession> CreateClient(const TlsClientConfig& config) = 0;
        [[nodiscard]] virtual UniquePtr<ITlsSession> CreateServer(const TlsServerConfig& config) = 0;
        [[nodiscard]] virtual StringView Name() const = 0;
    };

    /// The system's trusted roots: the distribution's bundle on Linux and SteamOS (the first of
    /// the usual paths that exists), the ROOT store on Windows; empty where none is found (a web
    /// build, a box without a bundle). `where` says what was read, or where it looked.
    [[nodiscard]] TlsRoots LoadSystemRoots(String* where = nullptr);
}
