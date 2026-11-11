# Package operations and verified rollback

Package installation, removal, decompression, and rollback invoke absolute
program paths with literal argument arrays through fork/execv. No operation
in pkgdb.c constructs a shell command. Compressed input is fully unpacked
into a private temporary file before pkgadd runs. Gzip lookup prefers
/opt/sst/bin/gzip, then /usr/bin/gzip. Native /usr/bin/uncompress handles .Z.

An upgrade records one previous version only when its matching package is
still available in the index and its cached bytes pass checksum verification.
The history is keyed by package code and records the version being replaced,
previous version, filename, checksum, source type, and repository. History
files are written through private temporary files and atomically renamed.
Caches referenced by history must be retained for rollback to remain available.
SST native metadata is normalized to the same upstream-version plus revision
identity used by release filenames: `"1.0,REV=2"` becomes `1.0-2`. Missing REV
uses the writer's default 1. Quoting does not change identity, and duplicate REV
fields are not accepted as a matching release. Native installed and local spool
imports use this normalization. Revision-only upgrades can retain verified
rollback history, and installed revision 1 stays distinct from revision 2.

Rollback checks that history matches the current version, verifies the old
artifact again, and invokes pkgadd with overwrite policy. It never removes
the current package first. A failed decompression or installation does not
consume history or update spm's installed record. Legacy .bak files and a
current installed record's pkg_file alone are not evidence of a previous
version, so they are not used. A successful rollback consumes its history;
this is one-step recovery, not an unlimited version archive.

Installed-database writes now use temporary files and atomic rename. A save
error preserves the previous database and reports the need for reconciliation.
An installation can succeed while history/database persistence fails; such
errors are reported separately. Native pkgadd is not transactional: a failed
pkgadd may still require target package repair even though spm avoids pkgrm.

Host checks (set SPM_TEST_CRYPTO_LIBRARY to a local OpenSSL libcrypto path):

- python3 tests/test_operations.py -v
- python3 tests/test_process.py -v
- python3 tests/test_installed_database.py -v
- python3 tests/test_install_gate.py -v

These compile actual C functions. Privileged pkgadd/pkgrm are mocked; the
process-runner tests execute only benign local commands. Validate the full
upgrade/rollback/remove sequence on a disposable Solaris/SPARC snapshot
before releasing. Commit the spm changes and advance the pipeline submodule
pin together when preparing a release; working-tree edits alone do not change
what CI checks out.
