"""A failed installed-database promotion must preserve its previous contents."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

class InstalledDatabaseTests(unittest.TestCase):
    def test_atomic_save(self):
        source = (ROOT/'pkgdb.c').read_text()
        function = source[source.index('int pkgdb_save_installed('):source.index('int pkgdb_load_installed(')]
        prelude = r'''
#include "pkgdb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#undef SPM_INSTALLED
#define SPM_INSTALLED database_path
static char *database_path;
static int fail_rename;
static int checked_rename(const char *a, const char *b) {
    if (fail_rename) { errno = EACCES; return -1; }
    return rename(a, b);
}
#define rename checked_rename
'''
        main = r'''
int main(int argc, char **argv) {
    pkgdb_t db; installed_pkg_t pkg; FILE *f; int rc;
    if (argc != 3) return 99;
    database_path = argv[1]; fail_rename = atoi(argv[2]);
    f = fopen(database_path, "w"); if (!f) return 98; fputs("previous", f); fclose(f);
    memset(&db, 0, sizeof(db)); memset(&pkg, 0, sizeof(pkg));
    strcpy(pkg.name, "demo"); strcpy(pkg.version, "1.0"); db.installed = &pkg; db.inst_count = 1;
    rc = pkgdb_save_installed(&db);
    return fail_rename ? (rc == -1 ? 0 : 2) : (rc == 0 ? 0 : 3);
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); harness = root/'main.c'; harness.write_text(prelude+function+main)
            binary = root/'save'
            subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', '-I'+str(ROOT), str(harness), '-o', str(binary)], check=True)
            path = root/'installed.db'
            subprocess.run([str(binary), str(path), '1'], check=True)
            self.assertEqual(path.read_text(), 'previous')
            self.assertEqual(list(root.glob('installed.db.tmp-*')), [])
            subprocess.run([str(binary), str(path), '0'], check=True)
            self.assertIn('demo|1.0|', path.read_text())
            self.assertEqual(list(root.glob('installed.db.tmp-*')), [])

if __name__ == '__main__': unittest.main()
