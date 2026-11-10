"""Keep packages from different namespaces distinct despite equal names."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

class RecordIdentityTests(unittest.TestCase):
    def test_replacement_uses_package_code(self):
        source = (ROOT/'pkgdb.c').read_text()
        add = source[source.index('static void add_installed('):source.index('/* ================================================================\n * TGCWARE INDEX IMPORT')]
        record = source[source.index('void pkgdb_record_install('):source.index('int pkgdb_install(')]
        harness = r'''
#include "pkgdb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
int pkgdb_save_installed(const pkgdb_t *db) { (void)db; return 0; }
'''+add+record+r'''
int main(void) {
    pkgdb_t db; avail_pkg_t next;
    memset(&db, 0, sizeof(db)); memset(&next, 0, sizeof(next));
    db.installed = calloc(3, sizeof(installed_pkg_t)); db.inst_alloc = 3; db.inst_count = 2;
    strcpy(db.installed[0].name, "demo"); strcpy(db.installed[0].pkg_code, "TGCdemo");
    strcpy(db.installed[1].name, "display name"); strcpy(db.installed[1].pkg_code, "SSTdemo");
    strcpy(next.name, "demo"); strcpy(next.pkg_code, "SSTdemo"); strcpy(next.version, "2.0");
    pkgdb_record_install(&db, &next, "/cache/next.pkg");
    if (db.inst_count != 2 || strcmp(db.installed[0].pkg_code, "TGCdemo") != 0 ||
        strcmp(db.installed[1].pkg_code, "SSTdemo") != 0 || strcmp(db.installed[1].version, "2.0") != 0) return 1;
    free(db.installed); return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            source_path, binary = Path(tmp)/'main.c', Path(tmp)/'identity'
            source_path.write_text(harness)
            subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', '-I'+str(ROOT), str(source_path), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

if __name__ == '__main__': unittest.main()
