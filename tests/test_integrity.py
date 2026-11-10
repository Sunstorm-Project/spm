"""Compile and exercise the actual streaming verifier with a local libcrypto.

Usage: SPM_TEST_CRYPTO_LIBRARY=/absolute/path/to/libcrypto python3 tests/test_integrity.py
"""
import ctypes
import hashlib
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

class IntegrityTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        library = os.environ['SPM_TEST_CRYPTO_LIBRARY']
        if not Path(library).is_file():
            raise RuntimeError('SPM_TEST_CRYPTO_LIBRARY must name an existing libcrypto')
        cls.build = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.build.cleanup)
        output = Path(cls.build.name)/'integrity.so'
        command = ['cc', '-shared', '-fPIC', '-std=c89', '-Wall', '-Wextra', '-Werror',
                   '-DSPM_TEST_CRYPTO_LIBRARY="'+library+'"', str(ROOT/'integrity.c'), '-o', str(output)]
        if os.uname().sysname != 'Darwin': command.append('-ldl')
        subprocess.run(command, check=True)
        cls.lib = ctypes.CDLL(str(output))
        cls.lib.spm_verify_file.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int]
        cls.lib.spm_safe_component.argtypes = [ctypes.c_char_p]

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.path = Path(self.tmp.name)/'payload.pkg'
        self.data = b'abc'
        self.path.write_bytes(self.data)

    def verify(self, digest, required=1, success=True):
        result = self.lib.spm_verify_file(os.fsencode(self.path), digest.encode(), required)
        self.assertEqual(result == 0, success)

    def test_sha256_known_vector_and_case(self):
        value = 'ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad'
        self.verify(value)
        self.verify('sha256:'+value)
        self.verify(value.upper())

    def test_legacy_md5(self):
        value = hashlib.md5(self.data).hexdigest()
        self.verify(value, 0)
        self.verify('md5:'+value, 0)
        self.verify(value, 1, False)
        self.verify('md5:'+value, 1, False)

    def test_bad_metadata(self):
        for value in ['', 'sha256:', 'f'*63, 'g'*64, 'sha256:'+'f'*32,
                      'md5:'+'f'*64, 'sha512:'+'f'*128]:
            self.verify(value, 0, False)

    def test_tampering(self):
        digest = hashlib.sha256(self.data).hexdigest()
        self.path.write_bytes(b'abd')
        self.verify(digest, success=False)

    def test_empty_missing_and_symlink(self):
        digest = hashlib.sha256(b'').hexdigest()
        self.path.write_bytes(b'')
        self.verify(digest, success=False)
        self.path.unlink()
        self.verify(digest, success=False)
        target = self.path.with_name('target')
        target.write_bytes(b'abc')
        self.path.symlink_to(target)
        self.verify(hashlib.sha256(b'abc').hexdigest(), success=False)

    def test_streaming_large_file(self):
        data = bytes(range(256))*2048
        self.path.write_bytes(data)
        self.verify(hashlib.sha256(data).hexdigest())

    def test_names(self):
        for value in ['SSTgcc-15.2.0-1.sst-sunos5.7-sparc.pkg.Z', 'libstdc++', '10.2p1']:
            self.assertEqual(self.lib.spm_safe_component(value.encode()), 1)
        for value in ['', '../x', '/tmp/x', '.hidden', '-flag', 'a;b', 'a b', 'a$(id)', 'a\nb']:
            self.assertEqual(self.lib.spm_safe_component(value.encode()), 0)

if __name__ == '__main__':
    unittest.main()
