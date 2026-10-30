// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// foundation.net.tls.mbedtls - implementation: Mbed TLS sessions over in-memory buffers, and the
// self-signed certificate. The only unit that includes Mbed TLS.

module;
#include "Core/Prelude.h"
#include <cstring>
#include <ctime>
#include <psa/crypto.h>
#include <mbedtls/error.h>
#include <mbedtls/md.h>
#include <mbedtls/pk.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

module foundation.net.tls.mbedtls;

import foundation.core;
import foundation.net.tls;

using namespace foundation::core;

namespace foundation::net
{
    namespace
    {
        // Mbed TLS's words for an error code ("SSL - ..."), for a failure's message.
        String ErrorText(int code)
        {
            char buffer[200] = {};
            mbedtls_strerror(code, buffer, sizeof(buffer));
            return String(StringView(reinterpret_cast<const utf8char*>(buffer), std::strlen(buffer)));
        }

        // PSA must be up before any key or session; it is idempotent.
        bool EnsureCrypto() { return psa_crypto_init() == PSA_SUCCESS; }

        mbedtls_ssl_protocol_version ToMbed(TlsVersion v)
        {
            return v == TlsVersion::Tls13 ? MBEDTLS_SSL_VERSION_TLS1_3 : MBEDTLS_SSL_VERSION_TLS1_2;
        }

        // A NUL-terminated copy: Mbed TLS's PEM parsers take the terminator in their length.
        Array<unsigned char> Terminated(StringView text)
        {
            Array<unsigned char> bytes;
            bytes.Resize(text.Size() + 1);
            for (usize i = 0; i < text.Size(); ++i)
                bytes[i] = static_cast<unsigned char>(text[i]);
            bytes[text.Size()] = 0;
            return bytes;
        }

        class MbedTlsSession final : public ITlsSession
        {
        public:
            MbedTlsSession()
            {
                mbedtls_ssl_config_init(&m_config);
                mbedtls_ssl_init(&m_ssl);
                mbedtls_x509_crt_init(&m_roots);
                mbedtls_x509_crt_init(&m_chain);
                mbedtls_pk_init(&m_key);
            }
            ~MbedTlsSession() override
            {
                mbedtls_ssl_free(&m_ssl);
                mbedtls_ssl_config_free(&m_config);
                mbedtls_x509_crt_free(&m_roots);
                mbedtls_x509_crt_free(&m_chain);
                mbedtls_pk_free(&m_key);
            }
            MbedTlsSession(const MbedTlsSession&) = delete;
            MbedTlsSession& operator=(const MbedTlsSession&) = delete;

            void SetUpClient(const TlsClientConfig& config)
            {
                if (!Common(MBEDTLS_SSL_IS_CLIENT, config.minimum, config.maximum, config.alpn))
                    return;
                if (config.trust.kind == TlsTrustKind::InsecureForTests)
                    mbedtls_ssl_conf_authmode(&m_config, MBEDTLS_SSL_VERIFY_NONE);
                else
                {
                    String where;
                    const TlsRoots roots = config.trust.kind == TlsTrustKind::System ? LoadSystemRoots(&where) : config.trust.roots;
                    usize trusted = 0;
                    if (!roots.pem.IsEmpty())
                    {
                        // A bundle may hold a certificate this build cannot read: the rest still count.
                        const Array<unsigned char> pem = Terminated(roots.pem.AsView());
                        (void)mbedtls_x509_crt_parse(&m_roots, pem.Data(), pem.Size());
                    }
                    for (const Array<byte>& der : roots.der)
                        (void)mbedtls_x509_crt_parse_der(&m_roots, reinterpret_cast<const unsigned char*>(der.Data()), der.Size());
                    for (const mbedtls_x509_crt* c = &m_roots; c != nullptr && c->raw.len > 0; c = c->next)
                        ++trusted;
                    if (trusted == 0)
                    {
                        Fail(config.trust.kind == TlsTrustKind::System
                                 ? Format(u8"no trusted roots: the system's were not found ({})", where.AsView())
                                 : String(u8"no trusted roots: none of the given certificates could be read"));
                        return;
                    }
                    mbedtls_ssl_conf_authmode(&m_config, MBEDTLS_SSL_VERIFY_REQUIRED);
                    mbedtls_ssl_conf_ca_chain(&m_config, &m_roots, nullptr);
                }
                if (!Setup())
                    return;
                if (config.serverName.IsEmpty() && config.trust.kind != TlsTrustKind::InsecureForTests)
                {
                    Fail(String(u8"a client needs the server's name to check its certificate against"));
                    return;
                }
                if (!config.serverName.IsEmpty())
                {
                    const String name = config.serverName;
                    if (const int r = mbedtls_ssl_set_hostname(&m_ssl, reinterpret_cast<const char*>(name.CStr())); r != 0)
                    {
                        Fail(Format(u8"the server name was refused: {}", ErrorText(r).AsView()));
                        return;
                    }
                }
                Advance(); // the ClientHello
            }

            void SetUpServer(const TlsServerConfig& config)
            {
                if (!Common(MBEDTLS_SSL_IS_SERVER, config.minimum, config.maximum, config.alpn))
                    return;
                const Array<unsigned char> chain = Terminated(config.certificateChainPem.AsView());
                if (const int r = mbedtls_x509_crt_parse(&m_chain, chain.Data(), chain.Size()); r != 0)
                {
                    Fail(Format(u8"the certificate chain could not be read: {}", ErrorText(r).AsView()));
                    return;
                }
                const Array<unsigned char> key = Terminated(config.privateKeyPem.AsView());
                if (const int r = mbedtls_pk_parse_key(&m_key, key.Data(), key.Size(), nullptr, 0); r != 0)
                {
                    Fail(Format(u8"the private key could not be read: {}", ErrorText(r).AsView()));
                    return;
                }
                if (const int r = mbedtls_ssl_conf_own_cert(&m_config, &m_chain, &m_key); r != 0)
                {
                    Fail(Format(u8"the certificate and key were refused: {}", ErrorText(r).AsView()));
                    return;
                }
                (void)Setup(); // the server waits for the ClientHello
            }

            Status Receive(Span<const byte> fromPeer) override
            {
                if (m_state == TlsState::Failed)
                    return Status{ErrorCode::Internal};
                for (const byte b : fromPeer)
                    m_in.PushBack(b);
                Advance();
                return m_state == TlsState::Failed ? Status{ErrorCode::Internal} : Status{};
            }

            usize TakeOutgoing(Array<byte>& toPeer) override
            {
                const usize n = m_out.Size();
                for (const byte b : m_out)
                    toPeer.PushBack(b);
                m_out.Clear();
                return n;
            }

            usize Read(Span<byte> plaintext) override
            {
                if (m_state != TlsState::Open || plaintext.IsEmpty())
                    return 0;
                for (;;)
                {
                    const int r = mbedtls_ssl_read(&m_ssl, reinterpret_cast<unsigned char*>(plaintext.Data()), plaintext.Size());
                    if (r > 0)
                        return static_cast<usize>(r);
                    if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE)
                        return 0;
                    if (r == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET)
                        continue; // TLS 1.3's after-handshake ticket: read on
                    if (r == 0 || r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY)
                    {
                        m_state = TlsState::Closed;
                        return 0;
                    }
                    Fail(Format(u8"reading failed: {}", ErrorText(r).AsView()));
                    return 0;
                }
            }

            Status Write(Span<const byte> plaintext) override
            {
                if (m_state == TlsState::Failed || m_state == TlsState::Closed)
                    return Status{ErrorCode::Internal};
                if (m_state == TlsState::Handshaking)
                {
                    for (const byte b : plaintext)
                        m_pending.PushBack(b);
                    return Status{};
                }
                return WriteNow(plaintext);
            }

            void Close() override
            {
                if (m_state == TlsState::Open)
                    (void)mbedtls_ssl_close_notify(&m_ssl);
                if (m_state != TlsState::Failed)
                    m_state = TlsState::Closed;
            }

            TlsState State() const override { return m_state; }
            StringView Failure() const override { return m_failure.AsView(); }
            StringView AlpnChosen() const override
            {
                const char* alpn = m_state == TlsState::Open ? mbedtls_ssl_get_alpn_protocol(&m_ssl) : nullptr;
                return alpn != nullptr ? StringView(reinterpret_cast<const utf8char*>(alpn), std::strlen(alpn)) : StringView{};
            }
            TlsVersion Version() const override
            {
                return mbedtls_ssl_get_version_number(&m_ssl) == MBEDTLS_SSL_VERSION_TLS1_3 ? TlsVersion::Tls13 : TlsVersion::Tls12;
            }

        private:
            bool Common(int endpoint, TlsVersion minimum, TlsVersion maximum, const Array<String>& alpn)
            {
                if (!EnsureCrypto())
                {
                    Fail(String(u8"the crypto library did not start"));
                    return false;
                }
                if (const int r = mbedtls_ssl_config_defaults(&m_config, endpoint, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT); r != 0)
                {
                    Fail(Format(u8"the TLS configuration could not be made: {}", ErrorText(r).AsView()));
                    return false;
                }
                mbedtls_ssl_conf_min_tls_version(&m_config, ToMbed(minimum));
                mbedtls_ssl_conf_max_tls_version(&m_config, ToMbed(maximum));
                if (!alpn.IsEmpty())
                {
                    // Mbed TLS keeps the pointers: the names live as long as the session.
                    for (const String& p : alpn)
                        m_alpnNames.PushBack(p);
                    for (const String& p : m_alpnNames)
                        m_alpn.PushBack(reinterpret_cast<const char*>(p.CStr()));
                    m_alpn.PushBack(nullptr);
                    if (const int r = mbedtls_ssl_conf_alpn_protocols(&m_config, m_alpn.Data()); r != 0)
                    {
                        Fail(Format(u8"the ALPN protocols were refused: {}", ErrorText(r).AsView()));
                        return false;
                    }
                }
                return true;
            }

            bool Setup()
            {
                if (const int r = mbedtls_ssl_setup(&m_ssl, &m_config); r != 0)
                {
                    Fail(Format(u8"the TLS session could not be made: {}", ErrorText(r).AsView()));
                    return false;
                }
                mbedtls_ssl_set_bio(&m_ssl, this, &MbedTlsSession::SendToPeer, &MbedTlsSession::ReceiveFromPeer, nullptr);
                return true;
            }

            // The handshake as far as the bytes so far take it; held plaintext goes once Open.
            void Advance()
            {
                if (m_state != TlsState::Handshaking)
                    return;
                const int r = mbedtls_ssl_handshake(&m_ssl);
                if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE)
                    return;
                if (r != 0)
                {
                    if (r == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED)
                    {
                        char info[512] = {};
                        (void)mbedtls_x509_crt_verify_info(info, sizeof(info), "", mbedtls_ssl_get_verify_result(&m_ssl));
                        String reasons(StringView(reinterpret_cast<const utf8char*>(info), std::strlen(info)));
                        while (!reasons.IsEmpty() && reasons[reasons.Size() - 1] == u8'\n')
                            reasons = String(reasons.AsView().SubStr(0, reasons.Size() - 1));
                        for (usize i = 0; i < reasons.Size(); ++i)
                            if (reasons[i] == u8'\n')
                                reasons[i] = u8';';
                        Fail(Format(u8"the server's certificate was refused: {}", reasons.AsView()));
                    }
                    else
                        Fail(Format(u8"the handshake failed: {}", ErrorText(r).AsView()));
                    return;
                }
                m_state = TlsState::Open;
                if (!m_pending.IsEmpty())
                {
                    Array<byte> held = Move(m_pending);
                    m_pending.Clear();
                    (void)WriteNow(held.AsSpan());
                }
            }

            Status WriteNow(Span<const byte> plaintext)
            {
                usize sent = 0;
                while (sent < plaintext.Size())
                {
                    const int r = mbedtls_ssl_write(&m_ssl, reinterpret_cast<const unsigned char*>(plaintext.Data() + sent), plaintext.Size() - sent);
                    if (r < 0)
                    {
                        Fail(Format(u8"writing failed: {}", ErrorText(r).AsView()));
                        return Status{ErrorCode::Internal};
                    }
                    sent += static_cast<usize>(r);
                }
                return Status{};
            }

            void Fail(String why)
            {
                m_state = TlsState::Failed;
                m_failure = Move(why);
            }

            // The memory BIO: what Mbed TLS sends goes to m_out; what it reads comes from m_in.
            static int SendToPeer(void* context, const unsigned char* bytes, size_t size)
            {
                auto* self = static_cast<MbedTlsSession*>(context);
                for (size_t i = 0; i < size; ++i)
                    self->m_out.PushBack(static_cast<byte>(bytes[i]));
                return static_cast<int>(size);
            }
            static int ReceiveFromPeer(void* context, unsigned char* bytes, size_t size)
            {
                auto* self = static_cast<MbedTlsSession*>(context);
                const usize available = self->m_in.Size() - self->m_inHead;
                if (available == 0)
                    return MBEDTLS_ERR_SSL_WANT_READ;
                const usize n = Min(available, static_cast<usize>(size));
                for (usize i = 0; i < n; ++i)
                    bytes[i] = static_cast<unsigned char>(self->m_in[self->m_inHead + i]);
                self->m_inHead += n;
                if (self->m_inHead == self->m_in.Size())
                {
                    self->m_in.Clear();
                    self->m_inHead = 0;
                }
                return static_cast<int>(n);
            }

            mbedtls_ssl_config m_config;
            mbedtls_ssl_context m_ssl;
            mbedtls_x509_crt m_roots;
            mbedtls_x509_crt m_chain;
            mbedtls_pk_context m_key;
            Array<String> m_alpnNames;
            Array<const char*> m_alpn;
            Array<byte> m_in;
            usize m_inHead = 0;
            Array<byte> m_out;
            Array<byte> m_pending; // written before Open
            TlsState m_state = TlsState::Handshaking;
            String m_failure;
        };

        class MbedTlsBackend final : public ITlsBackend
        {
        public:
            explicit MbedTlsBackend(IAllocator& allocator) : m_allocator(&allocator) { (void)EnsureCrypto(); }

            UniquePtr<ITlsSession> CreateClient(const TlsClientConfig& config) override
            {
                UniquePtr<MbedTlsSession> session = MakeUnique<MbedTlsSession>(*m_allocator);
                session->SetUpClient(config);
                return session;
            }
            UniquePtr<ITlsSession> CreateServer(const TlsServerConfig& config) override
            {
                UniquePtr<MbedTlsSession> session = MakeUnique<MbedTlsSession>(*m_allocator);
                session->SetUpServer(config);
                return session;
            }
            StringView Name() const override { return u8"Mbed TLS"; }

        private:
            IAllocator* m_allocator;
        };
    }

    UniquePtr<ITlsBackend> CreateMbedTlsBackend(IAllocator& allocator) { return MakeUnique<MbedTlsBackend>(allocator, allocator); }

    String CertificateTime(i32 days)
    {
        const std::time_t when = std::time(nullptr) + static_cast<std::time_t>(days) * 86400;
        std::tm utc{};
#if defined(_WIN32)
        gmtime_s(&utc, &when);
#else
        gmtime_r(&when, &utc);
#endif
        // Each field zero-padded to its width, as X.509's GeneralizedTime writes it.
        auto two = [](int v) { return v < 10 ? Format(u8"0{}", v) : Format(u8"{}", v); };
        return Format(u8"{}{}{}{}{}{}", utc.tm_year + 1900, two(utc.tm_mon + 1).AsView(), two(utc.tm_mday).AsView(), two(utc.tm_hour).AsView(),
                      two(utc.tm_min).AsView(), two(utc.tm_sec).AsView());
    }

    bool CreateSelfSignedCertificate(StringView name, StringView notBefore, StringView notAfter, TlsCertificate& out, String& error)
    {
        if (!EnsureCrypto())
        {
            error = String(u8"the crypto library did not start");
            return false;
        }
        // A new P-256 key, made in PSA and copied out for the writer.
        psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
        psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_SIGN_HASH | PSA_KEY_USAGE_SIGN_MESSAGE | PSA_KEY_USAGE_EXPORT);
        psa_set_key_algorithm(&attributes, PSA_ALG_ECDSA(PSA_ALG_SHA_256));
        psa_set_key_type(&attributes, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
        psa_set_key_bits(&attributes, 256);
        mbedtls_svc_key_id_t id = MBEDTLS_SVC_KEY_ID_INIT;
        if (psa_generate_key(&attributes, &id) != PSA_SUCCESS)
        {
            error = String(u8"the key could not be made");
            return false;
        }
        mbedtls_pk_context key;
        mbedtls_pk_init(&key);
        const int copied = mbedtls_pk_copy_from_psa(id, &key);
        (void)psa_destroy_key(id);
        if (copied != 0)
        {
            mbedtls_pk_free(&key);
            error = Format(u8"the key could not be copied: {}", ErrorText(copied).AsView());
            return false;
        }

        mbedtls_x509write_cert writer;
        mbedtls_x509write_crt_init(&writer);
        mbedtls_x509write_crt_set_version(&writer, MBEDTLS_X509_CRT_VERSION_3);
        mbedtls_x509write_crt_set_md_alg(&writer, MBEDTLS_MD_SHA256);
        mbedtls_x509write_crt_set_subject_key(&writer, &key);
        mbedtls_x509write_crt_set_issuer_key(&writer, &key);
        const String subject = Format(u8"CN={}", name);
        unsigned char serial[16] = {};
        (void)psa_generate_random(serial, sizeof(serial));
        serial[0] = static_cast<unsigned char>((serial[0] & 0x7F) | 0x01); // positive, never zero
        const String before(notBefore);
        const String after(notAfter);
        mbedtls_x509_san_list san{};
        san.node.type = MBEDTLS_X509_SAN_DNS_NAME;
        san.node.san.unstructured_name.p = const_cast<unsigned char*>(reinterpret_cast<const unsigned char*>(name.Data()));
        san.node.san.unstructured_name.len = name.Size();
        san.next = nullptr;
        int r = mbedtls_x509write_crt_set_subject_name(&writer, reinterpret_cast<const char*>(subject.CStr()));
        if (r == 0)
            r = mbedtls_x509write_crt_set_issuer_name(&writer, reinterpret_cast<const char*>(subject.CStr()));
        if (r == 0)
            r = mbedtls_x509write_crt_set_serial_raw(&writer, serial, sizeof(serial));
        if (r == 0)
            r = mbedtls_x509write_crt_set_validity(&writer, reinterpret_cast<const char*>(before.CStr()), reinterpret_cast<const char*>(after.CStr()));
        if (r == 0)
            r = mbedtls_x509write_crt_set_subject_alternative_name(&writer, &san);
        if (r == 0)
            r = mbedtls_x509write_crt_set_key_usage(&writer, MBEDTLS_X509_KU_DIGITAL_SIGNATURE);
        Array<unsigned char> pem;
        pem.Resize(4096);
        if (r == 0)
            r = mbedtls_x509write_crt_pem(&writer, pem.Data(), pem.Size());
        mbedtls_x509write_crt_free(&writer);
        if (r != 0)
        {
            mbedtls_pk_free(&key);
            error = Format(u8"the certificate could not be written: {}", ErrorText(r).AsView());
            return false;
        }
        out.certificatePem = String(StringView(reinterpret_cast<const utf8char*>(pem.Data()), std::strlen(reinterpret_cast<const char*>(pem.Data()))));
        Array<unsigned char> keyPem;
        keyPem.Resize(2048);
        r = mbedtls_pk_write_key_pem(&key, keyPem.Data(), keyPem.Size());
        mbedtls_pk_free(&key);
        if (r != 0)
        {
            error = Format(u8"the key could not be written: {}", ErrorText(r).AsView());
            return false;
        }
        out.privateKeyPem = String(StringView(reinterpret_cast<const utf8char*>(keyPem.Data()), std::strlen(reinterpret_cast<const char*>(keyPem.Data()))));
        return true;
    }
}
