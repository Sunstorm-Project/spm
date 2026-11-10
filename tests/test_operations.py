"""Host regression harness for package operations; never runs pkgadd/pkgrm."""
import hashlib
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

class OperationsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        root = Path(cls.tmp.name)
        cls.binary = root/'operations'
        source = (ROOT/'pkgdb.c').read_text()
        helpers = source[source.index('static char admin_path'):source.index('void pkgdb_record_install(')]
        start = source.index('int pkgdb_install(')
        operations = source[start:source.index('/* ================================================================\n * DEPENDENCY RESOLUTION', start)]
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
#undef SPM_VAR
#define SPM_CACHE cache_directory
#define SPM_ROLLBACK history_directory
#define SPM_VAR root_directory
static char *root_directory;
static char cache_directory[512], history_directory[512];
static int downloads, adds, removes, records, fail_add, fail_remove, fail_unpack, gzip_calls, fail_primary;
static void download_progress(long a, long b) { (void)a; (void)b; }
int http_download(const char *url, const char *path, http_progress_fn cb) {
    FILE *f = fopen(path, "wb"); (void)url; (void)cb;
    downloads++; if (!f) return -1; fputs("xyz", f); fclose(f); return 0;
}
int spm_run(char *const argv[], int fd) {
    if (strcmp(argv[0], "/usr/sbin/pkgadd") == 0) { adds++; return fail_add; }
    if (strcmp(argv[0], "/usr/sbin/pkgrm") == 0) { removes++; return fail_remove; }
    if (strstr(argv[0], "gzip") || strstr(argv[0], "uncompress")) {
        gzip_calls++;
        if (fail_unpack) return 1;
        if (fail_primary && strcmp(argv[0], "/opt/sst/bin/gzip") == 0) { write(fd, "partial", 7); return 1; }
        if (fail_primary && lseek(fd, 0, SEEK_CUR) != 0) return 1;
        return write(fd, "unpacked", 8) == 8 ? 0 : 1;
    }
    return 99;
}
int pkgdb_save_installed(const pkgdb_t *db) { (void)db; return 0; }
void pkgdb_record_install(pkgdb_t *db, const avail_pkg_t *pkg, const char *path) {
    installed_pkg_t *i = db->installed;
    records++; memset(i, 0, sizeof(*i));
    strcpy(i->name, pkg->name); strcpy(i->pkg_code, pkg->pkg_code);
    strcpy(i->version, pkg->version); strcpy(i->pkg_file, path); db->inst_count = 1;
}
'''
        main = r'''
static void put(const char *path, const char *data) {
    FILE *f = fopen(path, "wb"); if (!f) exit(90); fputs(data, f); fclose(f);
}
int main(int argc, char **argv) {
    avail_pkg_t available[2]; installed_pkg_t installed[2]; pkgdb_t db;
    char oldpath[512], journal[512]; int rc, before;
    if (argc != 3) return 91;
    root_directory = argv[1];
    snprintf(cache_directory, sizeof(cache_directory), "%s/cache", root_directory);
    snprintf(history_directory, sizeof(history_directory), "%s/history", root_directory);
    mkdir(cache_directory, 0700); mkdir(history_directory, 0700);
    memset(available, 0, sizeof(available)); memset(installed, 0, sizeof(installed)); memset(&db, 0, sizeof(db));
    strcpy(available[0].name, "demo"); strcpy(available[0].version, "1.0");
    strcpy(available[0].pkg_code, "SSTdemo"); strcpy(available[0].filename, "SSTdemo-1.0.pkg");
    strcpy(available[0].repo_name, "sunstorm"); available[0].source_type = SRC_GITHUB;
    strcpy(available[0].md5, "OLD_HASH");
    available[1] = available[0]; strcpy(available[1].version, "2.0");
    strcpy(available[1].filename, "SSTdemo-2.0.pkg"); strcpy(available[1].md5, "NEW_HASH");
    strcpy(installed[0].name, "display name with spaces"); strcpy(installed[0].version, "1.0,REV=1");
    strcpy(installed[0].pkg_code, "SSTdemo");
    db.available = available; db.avail_count = 2; db.installed = installed; db.inst_count = 1;
    snprintf(oldpath, sizeof(oldpath), "%s/%s", cache_directory, available[0].filename); put(oldpath, "abc");
    snprintf(journal, sizeof(journal), "%s/SSTdemo.previous", history_directory);
    if (strcmp(argv[2], "remove-failure") == 0) {
        fail_remove = 1; rc = pkgdb_remove(&db, 0);
        return rc != 0 && db.inst_count == 1 ? 0 : 1;
    }
    if (strcmp(argv[2], "unsafe-remove") == 0) {
        strcpy(installed[0].pkg_code, "-bad;code"); rc = pkgdb_remove(&db, 0);
        return rc != 0 && removes == 0 && db.inst_count == 1 ? 0 : 2;
    }
    if (strcmp(argv[2], "legacy-rollback") == 0) {
        strcpy(installed[0].pkg_file, oldpath); rc = pkgdb_rollback(&db, 0);
        return rc != 0 && adds == 0 && removes == 0 ? 0 : 3;
    }
    if (strcmp(argv[2], "unpack-failure") == 0 || strcmp(argv[2], "gzip-fallback") == 0 || strcmp(argv[2], "compress-success") == 0) {
        const char *name = strcmp(argv[2], "compress-success") == 0 ? "fixture.pkg.Z" : "fixture.pkg.gz";
        fail_unpack = strcmp(argv[2], "unpack-failure") == 0;
        fail_primary = strcmp(argv[2], "gzip-fallback") == 0;
        rc = install_cached(oldpath, name);
        if (fail_unpack) return rc != 0 && adds == 0 && removes == 0 ? 0 : 4;
        return rc == 0 && adds == 1 && gzip_calls == (fail_primary ? 2 : 1) ? 0 : 5;
    }
    if (strcmp(argv[2], "install-failure") == 0) fail_add = 1;
    rc = pkgdb_install(&db, 1);
    if (fail_add) return rc != 0 && records == 0 && access(journal, F_OK) != 0 ? 0 : 6;
    if (rc || access(journal, F_OK) != 0 || records != 1) return 7;
    before = adds;
    if (strcmp(argv[2], "corrupt-rollback") == 0) put(oldpath, "abd");
    if (strcmp(argv[2], "stale-history") == 0) strcpy(installed[0].version, "3.0");
    if (strcmp(argv[2], "rollback-failure") == 0) fail_add = 1;
    if (strcmp(argv[2], "malformed-history") == 0) put(journal, "bad|record\n");
    rc = pkgdb_rollback(&db, 0);
    if (strcmp(argv[2], "corrupt-rollback") == 0 || strcmp(argv[2], "stale-history") == 0 || strcmp(argv[2], "malformed-history") == 0)
        return rc != 0 && adds == before && removes == 0 && records == 1 ? 0 : 8;
    if (fail_add) return rc != 0 && removes == 0 && records == 1 && access(journal, F_OK) == 0 ? 0 : 9;
    return rc == 0 && records == 2 && removes == 0 && strcmp(installed[0].version, "1.0") == 0 && access(journal, F_OK) != 0 ? 0 : 10;
}
'''
        main = main.replace('OLD_HASH', hashlib.sha256(b'abc').hexdigest()).replace('NEW_HASH', hashlib.sha256(b'xyz').hexdigest())
        harness = root/'main.c'; harness.write_text(prelude+helpers+operations+main)
        command = ['cc', '-Wall', '-Wextra', '-Werror', '-I'+str(ROOT),
                   '-DSPM_TEST_CRYPTO_LIBRARY="'+os.environ['SPM_TEST_CRYPTO_LIBRARY']+'"',
                   str(harness), str(ROOT/'integrity.c'), '-o', str(cls.binary)]
        if os.uname().sysname != 'Darwin': command.append('-ldl')
        subprocess.run(command, check=True)

    def test_operations(self):
        for scenario in ['upgrade-rollback', 'remove-failure', 'unsafe-remove', 'legacy-rollback',
                         'unpack-failure', 'gzip-fallback', 'compress-success', 'install-failure',
                         'corrupt-rollback', 'stale-history', 'rollback-failure', 'malformed-history']:
            with self.subTest(scenario=scenario), tempfile.TemporaryDirectory() as tmp:
                result = subprocess.run([str(self.binary), tmp, scenario], capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
                self.assertEqual(list((Path(tmp)/'cache').glob('.unpack-*')), [])
                self.assertEqual(list(Path(tmp).glob('admin-*')), [])

if __name__ == '__main__': unittest.main()
