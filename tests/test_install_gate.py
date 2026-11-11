"""Exercise the actual install function with download/privileged calls replaced."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

class InstallGateTests(unittest.TestCase):
    def test_install_gates(self):
        source = (ROOT/'pkgdb.c').read_text()
        start = source.index('int pkgdb_install(')
        end = source.index('/* ================================================================', start)
        function = source[start:end]
        helpers = source[source.index('static int install_cached('):source.index('void pkgdb_record_install(')]
        rollback_start = source.index('int pkgdb_rollback(')
        rollback_end = source.index('/* ================================================================', rollback_start)
        function = helpers + function + source[rollback_start:rollback_end]
        prelude = r'''
#include "pkgdb.h"
#include "http.h"
#include "integrity.h"
#include "process.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#undef SPM_CACHE
#undef SPM_ROLLBACK
#define SPM_CACHE cache_directory
#define SPM_ROLLBACK cache_directory
static char *cache_directory;
static int downloads, commands, records, corrupt_download;
static void download_progress(long a, long b) { (void)a; (void)b; }
static const char *pkgadd_admin_file(void) { return "/test/admin"; }
int http_download(const char *url, const char *path, http_progress_fn callback) {
    FILE *f = fopen(path, "wb"); (void)url; (void)callback;
    downloads++; if (!f) return -1;
    fputs(corrupt_download ? "abd" : "abc", f); fclose(f); return 0;
}
int pkgdb_sys_installed(const char *code) { (void)code; return 0; }
void pkgdb_record_install(pkgdb_t *db, const avail_pkg_t *pkg, const char *path) {
    (void)db; (void)pkg; (void)path; records++;
}
int spm_run(char *const arguments[], int fd) { (void)arguments; (void)fd; commands++; return 0; }
'''
        main = r'''
int main(int argc, char **argv) {
    avail_pkg_t pkg; pkgdb_t db; char path[512]; FILE *f; int rc;
    if (argc != 2) return 100;
    cache_directory = argv[1];
    memset(&pkg, 0, sizeof(pkg)); memset(&db, 0, sizeof(db));
    strcpy(pkg.name, "demo"); strcpy(pkg.version, "1.0");
    strcpy(pkg.filename, "SSTdemo-1.0.pkg"); strcpy(pkg.pkg_code, "SSTdemo");
    strcpy(pkg.md5, "sha256:ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    pkg.source_type = SRC_GITHUB; db.available = &pkg; db.avail_count = 1;
    snprintf(path, sizeof(path), "%s/%s", cache_directory, pkg.filename);
    rc = pkgdb_install(&db, 0);
    if (rc || downloads != 1 || commands != 1 || records != 1) return 1;
    rc = pkgdb_install(&db, 0);
    if (rc || downloads != 1 || commands != 2 || records != 2) return 2;
    f = fopen(path, "wb"); fputs("abd", f); fclose(f);
    rc = pkgdb_install(&db, 0);
    if (!rc || commands != 2 || records != 2) return 3;
    unlink(path); corrupt_download = 1;
    rc = pkgdb_install(&db, 0);
    if (!rc || access(path, F_OK) == 0 || commands != 2) return 4;
    corrupt_download = 0; pkg.md5[0] = 0;
    rc = pkgdb_install(&db, 0);
    if (!rc || access(path, F_OK) == 0 || commands != 2) return 5;
    strcpy(pkg.filename, "../escape.pkg");
    rc = pkgdb_install(&db, 0);
    if (!rc || commands != 2) return 6;
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            harness = root/'harness.c'
            harness.write_text(prelude+function+main)
            binary = root/'harness'
            library = os.environ['SPM_TEST_CRYPTO_LIBRARY']
            command = ['cc', '-Wall', '-Wextra', '-Werror', '-I'+str(ROOT),
                       '-DSPM_TEST_CRYPTO_LIBRARY="'+library+'"', str(harness),
                       str(ROOT/'integrity.c'), '-o', str(binary)]
            if os.uname().sysname != 'Darwin': command.append('-ldl')
            subprocess.run(command, check=True)
            cache = root/'cache'; cache.mkdir()
            result = subprocess.run([str(binary), str(cache)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
            self.assertEqual(list(cache.iterdir()), [])

if __name__ == '__main__':
    unittest.main()
