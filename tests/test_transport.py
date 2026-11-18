"""Local TLS integration tests for the actual spm HTTP implementation.

Set SPM_TEST_CRYPTO_LIBRARY, SPM_TEST_SSL_LIBRARY and SPM_TEST_OPENSSL.
All servers listen only on loopback; certificates are temporary fixtures.
"""
import contextlib
import os
from pathlib import Path
import socketserver
import ssl
import subprocess
import tempfile
import threading
import unittest

ROOT = Path(__file__).resolve().parents[1]

class TransportTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        cls.root = Path(cls.tmp.name)
        cls.binary = cls.root/'http-test'
        harness = cls.root/'main.c'
        harness.write_text(r'''
#include "http.h"
#include <stdio.h>
#include <string.h>
int main(int argc, char **argv) {
    http_response_t response; int rc;
    if (argc != 5) return 9;
    http_set_ca_bundle(argv[1]);
    if (http_init() != 0) { http_shutdown(); return 2; }
    if (strcmp(argv[3], "get") == 0) {
        rc = http_get(argv[2], &response);
        if (!rc) { if (response.body) fwrite(response.body, 1, response.body_len, stdout); http_response_free(&response); }
    } else rc = http_download(argv[2], argv[4], NULL);
    http_shutdown(); return rc == 0 ? 0 : 3;
}
''')
        command = ['cc', '-Wall', '-Wextra', '-I'+str(ROOT),
                   '-DSPM_TEST_CRYPTO_LIBRARY="'+os.environ['SPM_TEST_CRYPTO_LIBRARY']+'"',
                   '-DSPM_TEST_SSL_LIBRARY="'+os.environ['SPM_TEST_SSL_LIBRARY']+'"',
                   str(ROOT/'http.c'), str(harness), '-o', str(cls.binary)]
        if os.uname().sysname != 'Darwin': command.append('-ldl')
        subprocess.run(command, check=True)
        cls.certs = {}
        for name, san in [('localhost', 'DNS:localhost'), ('wrong', 'DNS:wrong.invalid'), ('ip', 'IP:127.0.0.1')]:
            cert, key = cls.root/(name+'.pem'), cls.root/(name+'.key')
            subprocess.run([os.environ['SPM_TEST_OPENSSL'], 'req', '-x509', '-newkey', 'rsa:2048',
                            '-nodes', '-days', '1', '-subj', '/CN='+name, '-addext', 'subjectAltName='+san,
                            '-keyout', str(key), '-out', str(cert)], check=True, capture_output=True)
            cls.certs[name] = (cert, key)

        cert, key = cls.certs['localhost']
        expired, csr = cls.root/'expired.pem', cls.root/'expired.csr'
        config = cls.root/'ca.cnf'
        (cls.root/'index').write_text('')
        (cls.root/'serial').write_text('01\n')
        config.write_text(f"""[ca]
default_ca=fixture
[fixture]
database={cls.root}/index
new_certs_dir={cls.root}
serial={cls.root}/serial
private_key={key}
certificate={cert}
default_md=sha256
policy=policy
x509_extensions=extensions
[policy]
commonName=supplied
[extensions]
subjectAltName=DNS:localhost
basicConstraints=CA:FALSE
""")
        subprocess.run([os.environ['SPM_TEST_OPENSSL'], 'req', '-new', '-key', str(key),
                        '-subj', '/CN=localhost', '-out', str(csr)], check=True, capture_output=True)
        subprocess.run([os.environ['SPM_TEST_OPENSSL'], 'ca', '-selfsign', '-batch',
                        '-config', str(config), '-in', str(csr), '-out', str(expired),
                        '-startdate', '20000101000000Z', '-enddate', '20010101000000Z'],
                       check=True, capture_output=True)
        cls.certs['expired'] = (expired, key)

    @contextlib.contextmanager
    def server(self, name='localhost', response=None, tls=True):
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(*map(str, self.certs[name]))
        seen = []
        class Handler(socketserver.BaseRequestHandler):
            def handle(handler):
                stream = handler.request
                stream.settimeout(3)
                try:
                    if tls: stream = context.wrap_socket(stream, server_side=True)
                    data = b''
                    while b'\r\n\r\n' not in data:
                        part = stream.recv(4096)
                        if not part: return
                        data += part
                    seen.append(data)
                    stream.sendall((response(data) if callable(response) else response) or b'HTTP/1.0 200 OK\r\nContent-Length: 3\r\n\r\nabc')
                    if tls: stream.unwrap()
                except (ssl.SSLError, OSError): pass
                finally: stream.close()
        class Server(socketserver.ThreadingTCPServer):
            allow_reuse_address = True
            daemon_threads = True
        server = Server(('127.0.0.1', 0), Handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try: yield server.server_address[1], seen
        finally:
            server.shutdown(); server.server_close(); thread.join()

    def request(self, url, cert='localhost', mode='get', success=True):
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp)/'download'
            ca = self.certs[cert][0] if cert in self.certs else self.root/'missing.pem'
            result = subprocess.run([str(self.binary), str(ca), url, mode, str(output)], capture_output=True, timeout=8)
            self.assertEqual(result.returncode == 0, success, result.stderr.decode())
            if success:
                self.assertEqual(output.read_bytes() if mode == 'download' else result.stdout, b'abc')
            elif mode == 'download': self.assertFalse(output.exists())
            return result

    def test_valid_dns_get_and_download(self):
        with self.server() as (port, _):
            for mode in ['get', 'download']: self.request(f'https://localhost:{port}/package', mode=mode)

    def test_wrong_dns_rejected(self):
        with self.server('wrong') as (port, seen):
            for mode in ['get', 'download']: self.request(f'https://localhost:{port}/', cert='wrong', mode=mode, success=False)
            self.assertEqual(seen, [])

    def test_untrusted_certificate(self):
        with self.server('wrong') as (port, seen):
            self.request(f'https://localhost:{port}/', success=False)
            self.assertEqual(seen, [])

    def test_missing_ca_fails_initialization(self):
        self.assertEqual(self.request('https://localhost:1/', cert='missing', success=False).returncode, 2)

    def test_valid_ip_certificate(self):
        with self.server('ip') as (port, _): self.request(f'https://127.0.0.1:{port}/', cert='ip')

    def test_dns_certificate_cannot_authenticate_ip(self):
        with self.server() as (port, seen):
            self.request(f'https://127.0.0.1:{port}/', success=False)
            self.assertEqual(seen, [])

    def test_downgrade_blocked_before_plain_http_request(self):
        with self.server(tls=False) as (plain_port, seen):
            response = f'HTTP/1.0 302 Found\r\nLocation: http://127.0.0.1:{plain_port}/\r\nContent-Length: 0\r\n\r\n'.encode()
            with self.server(response=response) as (port, _):
                for mode in ['get', 'download']: self.request(f'https://localhost:{port}/', mode=mode, success=False)
            self.assertEqual(seen, [])

    def test_https_redirect_and_308(self):
        with self.server() as (target, _):
            response = f'HTTP/1.0 308 Permanent Redirect\r\nLocation: https://localhost:{target}/next\r\nContent-Length: 0\r\n\r\n'.encode()
            with self.server(response=response) as (port, _):
                for mode in ['get', 'download']: self.request(f'https://localhost:{port}/', mode=mode)

    def test_redirect_to_wrong_identity(self):
        with self.server('localhost') as (target, _):
            response = f'HTTP/1.0 302 Found\r\nLocation: https://127.0.0.1:{target}/\r\nContent-Length: 0\r\n\r\n'.encode()
            with self.server(response=response) as (port, _): self.request(f'https://localhost:{port}/', success=False)

    def test_truncated_body(self):
        response = b'HTTP/1.0 200 OK\r\nContent-Length: 12\r\n\r\nabc'
        with self.server(response=response) as (port, _):
            for mode in ['get', 'download']: self.request(f'https://localhost:{port}/', mode=mode, success=False)

    def test_relative_redirect_preserves_https(self):
        def response(data):
            if data.startswith(b'GET /start '):
                return b'HTTP/1.0 302 Found\r\nLocation: /end\r\nContent-Length: 0\r\n\r\n'
            return b'HTTP/1.0 200 OK\r\nContent-Length: 3\r\n\r\nabc'
        with self.server(response=response) as (port, _):
            for mode in ['get', 'download']: self.request(f'https://localhost:{port}/start', mode=mode)

    def test_redirect_loop_bounded(self):
        response = b'HTTP/1.0 302 Found\r\nLocation: /again\r\nContent-Length: 0\r\n\r\n'
        with self.server(response=response) as (port, seen):
            for mode in ['get', 'download']: self.request(f'https://localhost:{port}/', mode=mode, success=False)
            self.assertLessEqual(len(seen), 12)

    def test_scheme_relative_redirect(self):
        with self.server() as (target, _):
            response = f'HTTP/1.0 302 Found\r\nLocation: //localhost:{target}/end\r\nContent-Length: 0\r\n\r\n'.encode()
            with self.server(response=response) as (port, _): self.request(f'https://localhost:{port}/')

    def test_oversized_download_header(self):
        response = b'HTTP/1.0 200 OK\r\nX-Large: ' + b'x'*70000 + b'\r\nContent-Length: 3\r\n\r\nabc'
        with self.server(response=response) as (port, _): self.request(f'https://localhost:{port}/', mode='download', success=False)

    def test_expired_certificate_rejected(self):
        with self.server('expired') as (port, seen):
            self.request(f'https://localhost:{port}/', cert='expired', success=False)
            self.assertEqual(seen, [])

    def test_oversized_in_memory_response_rejected(self):
        body = b'x' * (4 * 1024 * 1024)
        response = b'HTTP/1.0 200 OK\r\nContent-Length: 4194304\r\n\r\n' + body
        with self.server(response=response) as (port, _): self.request(f'https://localhost:{port}/', success=False)

    def test_invalid_urls(self):
        for url in ['ftp://localhost/x', 'https://localhost:0/', 'https://localhost:65536/',
                    'https://localhost:abc/', 'https://user@localhost/', 'https://localhost/\r\nInjected:yes',
                    'https://'+('x'*256)+'/']:
            self.request(url, success=False)

if __name__ == '__main__': unittest.main()
