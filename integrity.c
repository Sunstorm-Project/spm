/* Streaming digests through runtime-loaded OpenSSL, like http.c. */
#include "integrity.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <dlfcn.h>
#include <sys/stat.h>

typedef struct evp_md_ctx_st EVP_MD_CTX;
typedef struct evp_md_st EVP_MD;

int spm_safe_component(const char *name)
{
    const unsigned char *p = (const unsigned char *)name;
    if (!p || !*p || !isalnum(*p)) return 0;
    for (; *p; p++) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || strchr("._+-", *p))) return 0;
    }
    return 1;
}

int spm_verify_file(const char *path, const char *expected, int require_sha256)
{
    void *library = NULL;
    EVP_MD_CTX *ctx = NULL;
    EVP_MD_CTX *(*ctx_new)(void);
    void (*ctx_free)(EVP_MD_CTX *);
    const EVP_MD *(*method)(void);
    int (*init)(EVP_MD_CTX *, const EVP_MD *, void *);
    int (*update)(EVP_MD_CTX *, const void *, size_t);
    int (*finish)(EVP_MD_CTX *, unsigned char *, unsigned int *);
    unsigned char buffer[16384], digest[64];
    unsigned int digest_length = 0;
    size_t length, n, i;
    FILE *stream = NULL;
    struct stat st;
    char hex[129];
    const char *algorithm;
    int result = -1;
    int tagged_sha256 = 0, tagged_md5 = 0;
    if (!expected || !*expected) goto done;
    if (strncmp(expected, "sha256:", 7) == 0) { expected += 7; tagged_sha256 = 1; }
    else if (strncmp(expected, "md5:", 4) == 0) {
        if (require_sha256) goto done;
        expected += 4; tagged_md5 = 1;
    }
    length = strlen(expected);
    if ((tagged_sha256 && length != 64) || (tagged_md5 && length != 32)) goto done;
    if (length == 64) algorithm = "EVP_sha256";
    else if (length == 32 && !require_sha256) algorithm = "EVP_md5";
    else goto done;
    for (i = 0; i < length; i++)
        if (!isxdigit((unsigned char)expected[i])) goto done;
    if (lstat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) goto done;
#ifdef SPM_TEST_CRYPTO_LIBRARY
    library = dlopen(SPM_TEST_CRYPTO_LIBRARY, RTLD_NOW | RTLD_LOCAL);
#else
    library = dlopen("/opt/sst/lib/libcrypto.so", RTLD_NOW | RTLD_LOCAL);
    if (!library) library = dlopen("/usr/tgcware/lib/libcrypto.so", RTLD_NOW | RTLD_LOCAL);
    if (!library) library = dlopen("libcrypto.so", RTLD_NOW | RTLD_LOCAL);
#endif
    if (!library) goto done;
    ctx_new = (EVP_MD_CTX *(*)(void))dlsym(library, "EVP_MD_CTX_new");
    ctx_free = (void (*)(EVP_MD_CTX *))dlsym(library, "EVP_MD_CTX_free");
    if (!ctx_new) ctx_new = (EVP_MD_CTX *(*)(void))dlsym(library, "EVP_MD_CTX_create");
    if (!ctx_free) ctx_free = (void (*)(EVP_MD_CTX *))dlsym(library, "EVP_MD_CTX_destroy");
    method = (const EVP_MD *(*)(void))dlsym(library, algorithm);
    init = (int (*)(EVP_MD_CTX *, const EVP_MD *, void *))dlsym(library, "EVP_DigestInit_ex");
    update = (int (*)(EVP_MD_CTX *, const void *, size_t))dlsym(library, "EVP_DigestUpdate");
    finish = (int (*)(EVP_MD_CTX *, unsigned char *, unsigned int *))dlsym(library, "EVP_DigestFinal_ex");
    if (!ctx_new || !ctx_free || !method || !init || !update || !finish) goto done;
    ctx = ctx_new();
    if (!ctx || init(ctx, method(), NULL) != 1) goto done;
    stream = fopen(path, "rb");
    if (!stream) goto done;
    while ((n = fread(buffer, 1, sizeof(buffer), stream)) > 0)
        if (update(ctx, buffer, n) != 1) goto done;
    if (ferror(stream) || finish(ctx, digest, &digest_length) != 1 ||
        digest_length * 2 != length) goto done;
    for (i = 0; i < digest_length; i++) sprintf(hex + i * 2, "%02x", digest[i]);
    hex[length] = '\0';
    result = 0;
    for (i = 0; i < length; i++)
        if (hex[i] != tolower((unsigned char)expected[i])) result = -1;
done:
    if (stream) fclose(stream);
    if (ctx) ctx_free(ctx);
    if (library) dlclose(library);
    if (result) fprintf(stderr, "spm: package checksum missing, unsupported, or mismatched: %s\n", path);
    return result;
}
