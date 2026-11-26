#!/usr/bin/python3
#
# Copyright (c) 2026 gluster-block contributors
# This file is part of gluster-block.
#
# This file is licensed to you under your choice of the GNU Lesser
# General Public License, version 3 or any later version (LGPLv3 or
# later), or the GNU General Public License, version 2 (GPLv2), in all
# cases as published by the Free Software Foundation.
#
"""Functional tests of the TLS listener of gluster-block-apid: client
certificates (mutual TLS), bearer tokens, roles, reload of the tokens and
the audit log. A second gluster-block-apid with its own CA, certificates
and tokens runs next to the one of the cluster.
"""

import http.client
import json
import os
import re
import signal
import socket
import ssl
import subprocess
import sys
import time
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from gbtest import check_api, record  # noqa: E402

DIR = "/tmp/gbtls"
PORT = 18443


def openssl(*args):
    subprocess.run(["openssl"] + list(args), check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def make_cert(name, cn, ca=None, server=False):
    key, crt = "%s/%s.key" % (DIR, name), "%s/%s.crt" % (DIR, name)
    if ca is None:
        openssl("req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "2",
                "-subj", "/CN=%s" % cn, "-keyout", key, "-out", crt,
                "-addext", "basicConstraints=critical,CA:TRUE",
                "-addext", "keyUsage=critical,keyCertSign,cRLSign")
        return key, crt
    csr = "%s/%s.csr" % (DIR, name)
    ext = "%s/%s.ext" % (DIR, name)
    with open(ext, "w") as f:
        if server:
            f.write("subjectAltName=IP:127.0.0.1,DNS:localhost\n"
                    "extendedKeyUsage=serverAuth\n")
        else:
            f.write("extendedKeyUsage=clientAuth\n")
    openssl("req", "-newkey", "rsa:2048", "-nodes", "-subj", "/CN=%s" % cn,
            "-keyout", key, "-out", csr)
    openssl("x509", "-req", "-in", csr, "-CA", ca[1], "-CAkey", ca[0],
            "-CAcreateserial", "-days", "2", "-out", crt, "-extfile", ext)
    return key, crt


def new_token(name, role):
    out = subprocess.run(["gluster-block-apid", "--new-token", name, role],
                         stdout=subprocess.PIPE, universal_newlines=True,
                         check=True).stdout
    token = re.search(r"^token: (\S+)$", out, re.M).group(1)
    line = re.search(r"^(token %s %s sha256:[0-9a-f]{64})$" % (name, role), out, re.M).group(1)
    return token, line


class TLSClient(object):
    def __init__(self, cert=None, token=None):
        self.ctx = ssl.create_default_context(cafile="%s/ca.crt" % DIR)
        if cert:
            self.ctx.load_cert_chain("%s/%s.crt" % (DIR, cert), "%s/%s.key" % (DIR, cert))
        self.token = token

    def call(self, method, path, body=None):
        conn = http.client.HTTPSConnection("127.0.0.1", PORT, context=self.ctx, timeout=60)
        headers = {}
        if self.token:
            headers["Authorization"] = "Bearer " + self.token
        data = None
        if body is not None:
            data = json.dumps(body).encode()
            headers["Content-Type"] = "application/json"
        conn.request(method, path, body=data, headers=headers)
        r = conn.getresponse()
        raw = r.read()
        conn.close()
        return r.status, (json.loads(raw) if raw else None), dict(r.getheaders())


class ApiTlsTest(unittest.TestCase):
    apid = None

    @classmethod
    def setUpClass(cls):
        subprocess.run(["rm", "-rf", DIR])
        os.makedirs(DIR + "/log")
        ca = make_cert("ca", "gluster-block test CA")
        make_cert("server", "localhost", ca=ca, server=True)
        make_cert("operator", "operator.example.com", ca=ca)
        make_cert("nobody", "nobody.example.com", ca=ca)
        make_cert("rogue", "operator.example.com")       # self signed, same CN

        cls.read_token, read_line = new_token("reader", "read")
        cls.admin_token, admin_line = new_token("admin1", "admin")
        cls.late_token, cls.late_line = new_token("late", "read")
        cls.tokens = "%s/api-tokens" % DIR
        with open(cls.tokens, "w") as f:
            f.write("# test tokens\n%s\n%s\ncert operator.example.com admin\n"
                    "bogus line\n" % (read_line, admin_line))
        os.chmod(cls.tokens, 0o600)

        cls.env = dict(os.environ,
                       GB_API_SOCKET="%s/api.sock" % DIR,
                       GB_API_LOG_DIR="%s/log" % DIR,
                       GB_API_LISTEN="127.0.0.1:%d" % PORT,
                       GB_API_TLS_CERT="%s/server.crt" % DIR,
                       GB_API_TLS_KEY="%s/server.key" % DIR,
                       GB_API_TLS_CA="%s/ca.crt" % DIR,
                       GB_API_TOKENS_FILE=cls.tokens)
        cls.apid = subprocess.Popen(["gluster-block-apid", "--log-level", "DEBUG"],
                                    env=cls.env, stdout=subprocess.DEVNULL,
                                    stderr=subprocess.DEVNULL)
        for _ in range(50):
            try:
                socket.create_connection(("127.0.0.1", PORT), timeout=1).close()
                break
            except OSError:
                time.sleep(0.2)
        else:
            raise AssertionError("the TLS listener does not come up")

    @classmethod
    def tearDownClass(cls):
        if cls.apid:
            cls.apid.terminate()
            cls.apid.wait(10)

    def log(self):
        with open("%s/log/gluster-block-apid.log" % DIR) as f:
            return f.read()

    def test_01_anonymous(self):
        code, doc, headers = TLSClient().call("GET", "/api/v1/version")
        self.assertEqual(code, 401, doc)
        check_api(doc, "Error")
        record("error-unauthorized", doc)
        self.assertEqual(doc["error"]["code"], "unauthorized")
        self.assertTrue(headers.get("WWW-Authenticate", "").startswith("Bearer"))
        # a load balancer asks without a client
        code, doc, _ = TLSClient().call("GET", "/healthz")
        self.assertEqual(code, 200, doc)

    def test_02_read_token(self):
        c = TLSClient(token=self.read_token)
        code, doc, _ = c.call("GET", "/api/v1/version")
        self.assertEqual(code, 200, doc)
        check_api(doc, "Version")
        code, doc, _ = c.call("POST", "/api/v1/volumes/rv/rebalance", {"dry_run": True})
        self.assertEqual(code, 403, doc)
        check_api(doc, "Error")
        record("error-forbidden", doc)
        self.assertEqual(doc["error"]["code"], "forbidden")

    def test_03_admin_token(self):
        c = TLSClient(token=self.admin_token)
        code, doc, _ = c.call("POST", "/api/v1/volumes/rv/rebalance", {"dry_run": True})
        self.assertEqual(code, 200, doc)
        check_api(doc, "RebalancePlan")

    def test_04_bad_token(self):
        code, doc, _ = TLSClient(token="gbt_" + "0" * 64).call("GET", "/api/v1/version")
        self.assertEqual(code, 401, doc)

    def test_05_client_certificate(self):
        c = TLSClient(cert="operator")
        code, doc, _ = c.call("GET", "/api/v1/volumes")
        self.assertEqual(code, 200, doc)
        code, doc, _ = c.call("POST", "/api/v1/volumes/rv/rebalance", {"dry_run": True})
        self.assertEqual(code, 200, doc)
        self.assertIn("AUDIT cert:operator.example.com@127.0.0.1 POST", self.log())

    def test_06_certificate_without_role(self):
        code, doc, _ = TLSClient(cert="nobody").call("GET", "/api/v1/volumes")
        self.assertEqual(code, 401, doc)
        # a token still works with it
        code, doc, _ = TLSClient(cert="nobody", token=self.read_token).call(
            "GET", "/api/v1/volumes")
        self.assertEqual(code, 200, doc)

    def test_07_certificate_of_another_ca(self):
        try:
            code, doc, _ = TLSClient(cert="rogue").call("GET", "/api/v1/volumes")
        except (ssl.SSLError, ConnectionError):
            return     # refused in the handshake, fine as well
        self.assertEqual(code, 401, doc)

    def test_08_no_plain_http(self):
        conn = http.client.HTTPConnection("127.0.0.1", PORT, timeout=10)
        try:
            conn.request("GET", "/healthz")
            r = conn.getresponse()
            self.assertNotEqual(r.status, 200)
        except (http.client.HTTPException, ConnectionError, OSError):
            pass
        finally:
            conn.close()

    def test_09_reload_tokens(self):
        code, doc, _ = TLSClient(token=self.late_token).call("GET", "/api/v1/version")
        self.assertEqual(code, 401)
        with open(self.tokens, "a") as f:
            f.write(self.late_line + "\n")
        self.apid.send_signal(signal.SIGHUP)
        time.sleep(1)
        code, doc, _ = TLSClient(token=self.late_token).call("GET", "/api/v1/version")
        self.assertEqual(code, 200, doc)
        self.assertIn("line ignored", self.log())

    def test_10_listen_needs_tls(self):
        env = dict(self.env, GB_API_SOCKET="%s/other.sock" % DIR,
                   GB_API_LISTEN="127.0.0.1:%d" % (PORT + 1), GB_API_TLS_CERT="")
        env.pop("GB_API_TLS_CERT")
        p = subprocess.run(["gluster-block-apid", "--log-stderr"], env=env, timeout=30,
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                           universal_newlines=True)
        self.assertNotEqual(p.returncode, 0)
        self.assertIn("TLS only", p.stdout)


if __name__ == "__main__":
    unittest.main()
