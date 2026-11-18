"""Actual fork/exec runner: literal arguments, statuses and redirection."""
import ctypes
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

class ProcessTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(); cls.addClassCleanup(cls.tmp.cleanup)
        output = Path(cls.tmp.name)/'process.so'
        subprocess.run(['cc', '-shared', '-fPIC', '-std=c89', '-Wall', '-Wextra', '-Werror',
                        str(ROOT/'process.c'), '-o', str(output)], check=True)
        cls.lib = ctypes.CDLL(str(output))
        cls.lib.spm_run.argtypes = [ctypes.POINTER(ctypes.c_char_p), ctypes.c_int]

    def run_command(self, arguments, fd=-1):
        array = (ctypes.c_char_p*(len(arguments)+1))(*[os.fsencode(a) for a in arguments], None)
        return self.lib.spm_run(array, fd)

    def test_literal_metacharacters_and_stdout(self):
        with tempfile.TemporaryDirectory() as tmp:
            marker = str(Path(tmp)/'must-not-exist')
            argument = '$(touch '+marker+'); literal'
            with (Path(tmp)/'output').open('wb') as output:
                self.assertEqual(self.run_command(['/bin/echo', argument], output.fileno()), 0)
            self.assertFalse(Path(marker).exists())
            self.assertEqual((Path(tmp)/'output').read_text().strip(), argument)

    def test_status_and_missing_executable(self):
        self.assertEqual(self.run_command(['/usr/bin/true']), 0)
        self.assertNotEqual(self.run_command(['/usr/bin/false']), 0)
        self.assertEqual(self.run_command(['/no/such/spm-test-command']), 127)
        self.assertEqual(self.run_command(['relative-command']), -1)

    def test_signal_termination(self):
        with tempfile.TemporaryDirectory() as tmp:
            source, binary = Path(tmp)/'signal.c', Path(tmp)/'signal'
            source.write_text('#include <signal.h>\nint main(void) { raise(SIGTERM); return 0; }\n')
            subprocess.run(['cc', str(source), '-o', str(binary)], check=True)
            self.assertEqual(self.run_command([str(binary)]), -1)

if __name__ == '__main__': unittest.main()
