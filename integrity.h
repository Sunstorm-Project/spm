#ifndef SPM_INTEGRITY_H
#define SPM_INTEGRITY_H
/* Strict basename validation for privileged package operations. */
int spm_safe_component(const char *name);
/* Accept sha256:<64 hex>, bare SHA-256, and legacy MD5 digests.
 * require_sha256 rejects MD5 for GitHub packages. Missing metadata fails.
 * Returns zero only when the file matches; no shell commands are used. */
int spm_verify_file(const char *path, const char *expected, int require_sha256);
#endif
