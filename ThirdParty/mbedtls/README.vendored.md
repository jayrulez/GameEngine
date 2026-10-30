# Mbed TLS - vendored

Source: https://github.com/Mbed-TLS/mbedtls
Pinned release: **4.2.0** (release `mbedtls-4.2.0`, the archive `mbedtls-4.2.0.tar.bz2`, SHA-256
`2bed9d713b4668f76553b097e72b8aa30bc8f112a940d7ae228d524bbde6ffea` as the release page gives it).
License: Apache-2.0 OR GPL-2.0-or-later, taken under **Apache-2.0** (see `LICENSE` and
`tf-psa-crypto/LICENSE`). Its drivers: everest (Curve25519, Apache-2.0), p256-m (Apache-2.0 OR
GPL-2.0-or-later), pqcp (Apache-2.0 OR ISC OR MIT).

For the engine's HTTP client and secure WebSockets (wss): TLS 1.2 and 1.3, X.509 certificates, and
the PSA crypto beneath them. Vendored BY COPY (house rule, like Jolt and recastnavigation; no
submodule). Only the library is taken; the release's `tests/`, `programs/`, `docs/`, `doxygen/`,
`scripts/`, `framework/` (the test and code-generation tooling), `ChangeLog*` and every build file
(`CMakeLists.txt`, `Makefile`, `*.cmake`) are NOT vendored, and neither are the `.gitignore`s.

## What is here

| Part | Compiled by `ThirdParty::MbedTLS`? |
|---|---|
| `include/`, `library/` | yes: TLS, X.509, the generated sources (`error.c`, `version_features.c`, `ssl_debug_helpers_generated.c`) as the release ships them |
| `tf-psa-crypto/include/`, `core/`, `dispatch/`, `platform/`, `utilities/`, `extras/` | yes: the PSA crypto API and its core |
| `tf-psa-crypto/drivers/builtin/`, `everest/`, `p256-m/` | yes: the built-in algorithms, Curve25519, the small P-256 |
| `tf-psa-crypto/drivers/pqcp/` (`include/`, `src/`) | yes, its two shims, which compile to nothing while ML-DSA is off (the default) |
| `tf-psa-crypto/drivers/pqcp/mldsa-native/` | NOT vendored (6.6 MB, post-quantum ML-DSA, off by default): re-vendor it from the same release before turning `TF_PSA_CRYPTO_PQCP_MLDSA_ENABLED` on |
| `configs/`, `tf-psa-crypto/configs/` | no: upstream's example configurations, for reference |

## Configuration

Upstream's defaults, unchanged: `include/mbedtls/mbedtls_config.h` and
`tf-psa-crypto/include/psa/crypto_config.h`. No defines are passed; the source list is the one
upstream's own CMake configures with those defaults (115 files). To change the configuration, set
`MBEDTLS_CONFIG_FILE` / `TF_PSA_CRYPTO_CONFIG_FILE` (or the `_USER_` variants) on the target rather
than editing the vendored headers, so an update is a plain copy.

## Build wiring

The `ThirdParty::MbedTLS` house target lives in `ThirdParty/CMakeLists.txt`: one static C99
library, its public include dirs PUBLIC + SYSTEM (`#include <mbedtls/ssl.h>`, `<psa/crypto.h>`),
built without the engine's `-Werror`. Windows links `ws2_32` and `bcrypt`. Built for Emscripten too
(a game in a browser speaks TLS through the browser's fetch and WebSocket instead).

## Updating

Download the release archive (it carries `tf-psa-crypto` and the generated sources; a git checkout
does not), check its SHA-256 against the release page, and copy the parts above over these,
dropping the same folders and build files. Check the source list against upstream's CMake
(configure the archive with `-DENABLE_TESTING=OFF -DENABLE_PROGRAMS=OFF
-DCMAKE_EXPORT_COMPILE_COMMANDS=ON` and read `compile_commands.json`), and both `LICENSE` files.
