"""Compare real native VERSION fixtures with actual GitHub asset imports."""
from pathlib import Path
import json
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

class RevisionIdentityTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        cls.root = Path(cls.tmp.name)
        source = (ROOT/'pkgdb.c').read_text()
        management = source[source.index('pkgdb_t *pkgdb_new('):source.index('/* ================================================================\n * TGCWARE INDEX IMPORT')]
        importer = source[source.index('void pkgdb_add_github('):source.index('/* ================================================================\n * INDEX PERSISTENCE')]
        normalizer = source[source.index('static void normalize_sst_pkginfo_version('):source.index('int pkgdb_save_installed(')]
        loader = source[source.index('int pkgdb_load_installed('):source.index('/* ================================================================\n * SEARCH AND LOOKUP')]
        # Redirect only the actual loader's native database root into our fixture.
        loader = loader.replace('opendir("/var/sadm/pkg")', 'opendir(database_root)')
        loader = loader.replace('"/var/sadm/pkg/%s/pkginfo", de->d_name', '"%s/%s/pkginfo", database_root, de->d_name')
        status = source[source.index('int pkgdb_avail_is_installed('):source.index('/* ================================================================\n * VERSION ROLLUP HELPERS')]
        integrity = (ROOT/'integrity.c').read_text()
        safe = integrity[integrity.index('int spm_safe_component('):integrity.index('int spm_verify_file(')]
        prelude = r'''
#include "pkgdb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <sys/stat.h>
#include <dirent.h>
static const char *database_root;
'''
        main = r'''
int main(int argc, char **argv) {
    pkgdb_t *db;
    int installed_revision;
    char expected[128];
    if (argc != 6) return 90;
    database_root = argv[1]; installed_revision = atoi(argv[4]);
    db = pkgdb_new();
    if (pkgdb_load_installed(db) || db->inst_count != 1) return 1;
    if (strcmp(db->installed[0].version, argv[3])) {
        fprintf(stderr, "loaded version: %s\n", db->installed[0].version); return 2;
    }
    pkgdb_add_github(db, argv[2], "sunstorm", "packages", "Sunstorm-Project");
    if (db->avail_count != 2) return 3;
    snprintf(expected, sizeof(expected), "%s-2", argv[5]);
    if (strcmp(db->available[0].version, expected)) return 4;
    snprintf(expected, sizeof(expected), "%s-1", argv[5]);
    if (strcmp(db->available[1].version, expected)) return 4;
    if (pkgdb_avail_is_installed(db, 0) != (installed_revision == 2) ||
        pkgdb_avail_is_installed(db, 1) != (installed_revision == 1)) return 5;
    pkgdb_free(db); return 0;
}
'''
        path=cls.root/'main.c';path.write_text(prelude+safe+management+importer+normalizer+loader+status+main)
        cls.binary=cls.root/'identity'
        subprocess.run(['cc','-Wall','-Wextra','-I'+str(ROOT),str(path),str(ROOT/'json.c'),'-o',str(cls.binary)],check=True)

    def run_fixture(self, version, expected, revision, upstream="1.0"):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);package=root/'SSTdemo';package.mkdir()
            (package/'pkginfo').write_text('PKG=SSTdemo\nNAME=demo\nVERSION='+version+'\n')
            assets=[{'name':f'SSTdemo-{upstream}-{rev}.sst-sunos5.7-sparc.pkg.Z','browser_download_url':'https://example.invalid/package'} for rev in (2,1)]
            body=json.dumps([{'tag_name':'latest','assets':assets}])
            subprocess.run([str(self.binary),str(root),body,expected,str(revision),upstream],check=True)

    def test_quoted_native_revision_matches_only_same_release(self):
        self.run_fixture('"1.0,REV=2"','1.0-2',2)

    def test_revision_one_is_distinct_from_two(self):
        self.run_fixture('1.0,REV=1','1.0-1',1)

    def test_missing_revision_uses_writer_default(self):
        self.run_fixture('1.0','1.0-1',1)

    def test_prerelease_letters_and_dates_retain_complete_identity(self):
        for upstream in ['1.2.0-rc1', '9f', '2026.11.05']:
            with self.subTest(upstream=upstream):
                self.run_fixture(upstream+',REV=2',upstream+'-2',2,upstream)

    def test_duplicate_revision_is_not_silently_accepted(self):
        self.run_fixture('1.0,REV=1,REV=2','1.0,REV=1,REV=2',0)

if __name__=='__main__':unittest.main()
