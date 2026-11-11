"""Exercise actual configuration defaults without touching a machine's config."""
from pathlib import Path
import configparser
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]

class DefaultsTests(unittest.TestCase):
    def test_packaged_trust_and_no_enabled_legacy_repository(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);source=root/'defaults.c';binary=root/'defaults'
            source.write_text(r'''
#include "config.h"
#include <stdio.h>
int main(void) {
    spm_config_t cfg; int i;
    config_defaults(&cfg);
    printf("%s\n%d\n",cfg.ca_bundle,cfg.repo_count);
    for(i=0;i<cfg.repo_count;i++)
        printf("%s %d %d %s\n",cfg.repos[i].name,cfg.repos[i].enabled,cfg.repos[i].type,cfg.repos[i].owner);
    return 0;
}
''')
            subprocess.run(['cc','-Wall','-Wextra','-I'+str(ROOT),str(ROOT/'config.c'),str(source),'-o',str(binary)],check=True)
            lines=subprocess.check_output([str(binary)],text=True).splitlines()
            self.assertEqual(lines[:2],['/opt/sst/etc/ssl/cert.pem','2'])
            repos={line.split()[0]:line.split()[1:] for line in lines[2:]}
            self.assertEqual(set(repos),{'firefly128','sunstorm'})
            self.assertTrue(all(fields[:2]==['1','2'] for fields in repos.values()))
            self.assertEqual(repos['sunstorm'][2],'Sunstorm-Project')
            sample=configparser.ConfigParser();sample.read(ROOT/'repos.conf.example')
            self.assertEqual(sample['ssl']['ca_bundle'],lines[0])
            self.assertEqual(sample['repo:sunstorm']['owner'],repos['sunstorm'][2])
            self.assertFalse(any(section=='repo:tgcware' for section in sample.sections()))

if __name__=='__main__':unittest.main()
