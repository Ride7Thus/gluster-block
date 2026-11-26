#
# Copyright (c) 2026 gluster-block contributors
# This file is part of gluster-block.
#
# This file is licensed to you under your choice of the GNU Lesser
# General Public License, version 3 or any later version (LGPLv3 or
# later), or the GNU General Public License, version 2 (GPLv2), in all
# cases as published by the Free Software Foundation.
#
"""Helpers of the functional tests: the API over its unix socket, the cli,
the contracts (docs/api/openapi.yaml, tests/compat/json/daemon.schema.json)
and the recorded fixtures.
"""

import copy
import http.client
import json
import os
import socket
import subprocess
import time

import jsonschema
import yaml

SRC = os.environ.get("GB_TEST_SRC", "/src")
OUT = os.environ.get("GB_TEST_OUT", "/tmp/gbtest")
API_SOCKET = os.environ.get("GB_API_SOCKET", "/run/gluster-block/api.sock")
FIXTURES = os.path.join(OUT, "fixtures")

HOSTS = os.environ.get("HOSTS", "").split(",")


class UnixHTTPConnection(http.client.HTTPConnection):
    def __init__(self, path, timeout=300):
        super().__init__("localhost", timeout=timeout)
        self.path = path

    def connect(self):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(self.timeout)
        self.sock.connect(self.path)


def api(method, path, body=None):
    """(status, parsed JSON or None, headers)"""
    conn = UnixHTTPConnection(API_SOCKET)
    data = json.dumps(body).encode() if body is not None else None
    headers = {"Content-Type": "application/json"} if data else {}
    conn.request(method, path, body=data, headers=headers)
    r = conn.getresponse()
    raw = r.read()
    conn.close()
    doc = json.loads(raw) if raw else None
    return r.status, doc, dict(r.getheaders())


def cli(*args, check=False):
    """(exit code, parsed JSON or text) of gluster-block --json ..."""
    p = subprocess.run(["gluster-block"] + list(args) + ["--json"],
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                       universal_newlines=True)
    text = p.stdout or p.stderr
    try:
        doc = json.loads(text)
    except ValueError:
        doc = text
    if check and p.returncode:
        raise AssertionError("gluster-block %s failed (%d): %s"
                             % (" ".join(args), p.returncode, text))
    return p.returncode, doc


def sh(cmd):
    """a shell command with the functions of setup.sh"""
    return subprocess.run(["bash", "-c", ". %s/tests/functional/setup.sh; %s" % (SRC, cmd)],
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          universal_newlines=True)


def record(name, doc):
    """keep a response as fixture for the tests of the Cockpit plugin"""
    os.makedirs(FIXTURES, exist_ok=True)
    with open(os.path.join(FIXTURES, name + ".json"), "w") as f:
        json.dump(doc, f, indent=2, sort_keys=True)
        f.write("\n")


def wait_operation(volume, opid, timeout=120):
    end = time.time() + timeout
    while time.time() < end:
        status, doc, _ = api("GET", "/api/v1/volumes/%s/operations/%s" % (volume, opid))
        if status != 200 or doc.get("state") != "running":
            return status, doc
        time.sleep(1)
    raise AssertionError("operation %s still runs after %ds" % (opid, timeout))


# ---------------------------------------------------------------------------
# contracts
# ---------------------------------------------------------------------------

def _load_openapi():
    with open(os.path.join(SRC, "docs", "api", "openapi.yaml")) as f:
        return yaml.safe_load(f)


def _resolve(node, doc):
    if isinstance(node, dict):
        if "$ref" in node:
            target = doc
            for part in node["$ref"].lstrip("#/").split("/"):
                target = target[part]
            return _resolve(copy.deepcopy(target), doc)
        return {k: _resolve(v, doc) for k, v in node.items()}
    if isinstance(node, list):
        return [_resolve(v, doc) for v in node]
    return node


def _merge_allof(s):
    """allOf of objects into one object schema, so strict mode works"""
    parts = s.pop("allOf")
    out = {"type": "object", "properties": {}, "required": [],
           "additionalProperties": False}
    for p in parts:
        p = _to_jsonschema(p)
        if p.get("type") not in ("object", ["object", "null"]):
            return dict(s, allOf=parts)
        out["properties"].update(p.get("properties", {}))
        out["required"] += [r for r in p.get("required", []) if r not in out["required"]]
        if p.get("additionalProperties") is True:
            out["additionalProperties"] = True
    for k, v in s.items():
        if k != "nullable":
            out[k] = v
    if s.get("nullable"):
        out["type"] = ["object", "null"]
    return out


def _to_jsonschema(s):
    """OpenAPI 3.0 schema to JSON schema: nullable, allOf; objects strict"""
    if isinstance(s, list):
        return [_to_jsonschema(v) for v in s]
    if not isinstance(s, dict):
        return s
    s = dict(s)
    if "allOf" in s:
        return _merge_allof(s)
    for k in ("example", "description", "format"):
        s.pop(k, None)
    if "properties" in s:
        s["properties"] = {k: _to_jsonschema(v) for k, v in s["properties"].items()}
        s.setdefault("additionalProperties", False)
    if "items" in s:
        s["items"] = _to_jsonschema(s["items"])
    if s.pop("nullable", False):
        t = s.get("type")
        if t:
            s["type"] = [t, "null"] if isinstance(t, str) else t + ["null"]
        if "enum" in s and None not in s["enum"]:
            s["enum"] = s["enum"] + [None]
    return s


OPENAPI = _load_openapi()
_SCHEMAS = _resolve(OPENAPI["components"]["schemas"], OPENAPI)

with open(os.path.join(SRC, "tests", "compat", "json", "daemon.schema.json")) as f:
    DAEMON = json.load(f)


def api_schema(name):
    return _to_jsonschema(_SCHEMAS[name])


def check_api(doc, name):
    """the response follows schema @name of the OpenAPI, no unknown fields"""
    jsonschema.validate(doc, api_schema(name))


def check_daemon(doc, name):
    """the daemon answer follows definition @name of daemon.schema.json"""
    schema = {"$ref": "#/definitions/%s" % name, "definitions": DAEMON["definitions"]}
    jsonschema.validate(doc, schema)


def openapi_paths():
    return OPENAPI["paths"]
