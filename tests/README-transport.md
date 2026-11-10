# Transport regression checks

Run `tests/test_transport.py` on a host with a C compiler, Python 3, and
OpenSSL, setting absolute paths in these variables:

- SPM_TEST_CRYPTO_LIBRARY: libcrypto shared library
- SPM_TEST_SSL_LIBRARY: libssl shared library
- SPM_TEST_OPENSSL: openssl executable

The suite compiles the actual http.c, creates temporary certificates, and
serves only on loopback. It tests GET and download paths, DNS and IPv4
certificate identity, trust failures, missing CA configuration, HTTPS
redirects/downgrades, redirect bounds, malformed URLs, truncated payloads,
and download header bounds. It requires permission to open local sockets.

HTTPS now requires OpenSSL's certificate hostname/IP verification APIs
(available in OpenSSL 1.0.2 and later). Libraries missing the APIs are
rejected; OpenSSL 3.x is the intended deployment. An explicitly configured
CA bundle must load successfully; it does not silently fall back to a
different store. Default-store searches remain supported and fail closed.

Transport continues to support initially configured HTTP repositories for
legacy compatibility. HTTPS cannot redirect to HTTP. The existing IPv4
networking limitation remains; unsupported schemes, invalid authorities,
control characters, oversized URLs, and unsupported redirect forms fail.
Supported redirect forms are HTTP(S) absolute URLs, scheme-relative URLs,
and root-relative paths. Each HTTPS destination verifies its own identity.

Run these tests on Solaris/SPARC with the deployed OpenSSL and CA bundle
before release. Host integration tests do not establish target ABI or
network-stack compatibility.
