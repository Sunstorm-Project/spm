# Package integrity checks

Run host regression checks with a local OpenSSL libcrypto:

```
SPM_TEST_CRYPTO_LIBRARY=/absolute/path/to/libcrypto.so python3 -m unittest discover -s tests -p 'test_*integrity.py' -v
SPM_TEST_CRYPTO_LIBRARY=/absolute/path/to/libcrypto.so python3 tests/test_install_gate.py -v
```

The first suite compiles the actual C verifier and checks known digests,
streaming, invalid metadata, legacy MD5, symlinks, and package names. The
install suite extracts the actual pkgdb_install function and substitutes
network and privileged operations; it never invokes pkgadd.

GitHub packages now require SHA-256. TGCware retains legacy MD5 integrity
checking; MD5 does not provide modern collision resistance. Missing or
previously truncated digests are rejected. Refresh indexes before testing
an upgrade; a corrupt cached file must be removed before retrying.

The existing pipe-delimited index field is retained as `md5` for format
compatibility, but its storage now accommodates GitHub's sha256: prefix.
All consumers must be rebuilt after the avail_pkg_t layout changes.

Before release, run on Solaris/SPARC with the supported OpenSSL versions:
valid and altered fresh downloads, valid and altered cached packages,
interrupted downloads, missing metadata, .pkg/.pkg.gz/.pkg.Z, and the CLI,
GUI, and agent builds. Verify that rejected packages never reach pkgadd.

These checks validate bytes against index metadata. They do not authenticate
an untrusted index, repair TLS hostname validation, or repair rollback.

Bootstrap qualification: the shared verifier requires libcrypto even for
HTTP/MD5 packages. On a bare machine install OpenSSL through native pkgadd
from trusted media before using spm-bootstrap for downloaded packages.
Verify this sequence on the bare Solaris snapshot before releasing the
bootstrap package; there is no checksum-bypass mode.
