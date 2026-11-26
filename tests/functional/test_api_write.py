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
"""Functional tests of the changes through gluster-block-apid: create,
resize, auth, prio path, rebalance, maintenance and delete, the long
operations (202 + polling), confirmations, idempotency, validation, the
busy volume, and delete of the cli in background.

Runs on the volumes "plain" (no blocks before: the first create makes it a
block hosting volume) and "dv" (dispersed: thin blocks need a confirmation).
"""

import os
import sys
import time
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from gbtest import (HOSTS, api, check_api, cli, record, wait_operation)  # noqa: E402

VOL = "plain"
MIB = 1024 * 1024


def blocks_url(vol=VOL):
    return "/api/v1/volumes/%s/blocks" % vol


class ApiWriteTest(unittest.TestCase):

    def call(self, method, path, body=None, status=None, schema=None, fixture=None):
        code, doc, headers = api(method, path, body)
        if status is not None:
            self.assertEqual(code, status, "%s %s %s: %s" % (method, path, body, doc))
        if code >= 400:
            check_api(doc, "Error")
        elif schema:
            check_api(doc, schema)
        if fixture:
            record(fixture, doc)
        return code, doc, headers

    def accepted(self, method, path, body=None, fixture=None):
        """a long operation: 202, Location, then poll it to the end"""
        code, doc, headers = self.call(method, path, body, status=202,
                                       schema="OperationAccepted", fixture=fixture)
        self.assertEqual(doc["state"], "running")
        self.assertTrue(doc["id"])
        self.assertEqual(headers.get("Location"),
                         "/api/v1/volumes/%s/operations/%s" % (doc["volume"], doc["id"]))
        status, op = wait_operation(doc["volume"], doc["id"])
        self.assertEqual(status, 200, op)
        check_api(op, "OperationDetail")
        self.assertEqual(op["state"], "succeeded", op)
        return op

    def create_body(self, name, size=16 * MIB, **kw):
        body = {"name": name, "size_bytes": size, "hosts": list(HOSTS)}
        body.update(kw)
        return body

    # -- validation -----------------------------------------------------------

    def test_01_validation(self):
        bad = [
            (None, "body"),
            ({"name": "x", "size_bytes": MIB, "hosts": HOSTS, "colour": "red"}, "unknown"),
            ({"name": "bad.name", "size_bytes": MIB, "hosts": HOSTS}, "name"),
            ({"name": "x", "size_bytes": "1GiB", "hosts": HOSTS}, "size type"),
            ({"name": "x", "size_bytes": 0, "hosts": HOSTS}, "size zero"),
            ({"name": "x", "size_bytes": MIB, "hosts": ["node1.example.com"]}, "host name"),
            ({"name": "x", "size_bytes": MIB, "hosts": [HOSTS[0], HOSTS[0]]}, "twice"),
            ({"name": "x", "size_bytes": MIB, "hosts": HOSTS, "ha": 1}, "ha"),
            ({"name": "x", "size_bytes": MIB, "hosts": HOSTS, "prealloc": "no",
              "zerofill": True}, "zerofill"),
            ({"name": "x", "size_bytes": MIB, "hosts": HOSTS, "prealloc": "half"}, "prealloc"),
        ]
        for body, what in bad:
            code, doc, _ = self.call("POST", blocks_url(), body, status=400)
            self.assertEqual(doc["error"]["code"], "invalid_request", what)
        record("error-invalid-request", doc)

        code, doc, headers = self.call("PUT", blocks_url(), {}, status=405)
        self.assertEqual(headers.get("Allow"), "GET, POST")
        self.call("GET", blocks_url("rv"), {"x": 1}, status=400)
        # not a JSON object
        code, doc, _ = api("POST", blocks_url(), [1, 2])
        self.assertEqual(code, 400)
        self.call("POST", blocks_url("nosuchvol"), self.create_body("x"), status=404)

    # -- create ---------------------------------------------------------------

    def test_10_create_first_block(self):
        code, vol, _ = self.call("GET", "/api/v1/volumes/%s?refresh=1" % VOL, status=200)
        self.assertFalse(vol["block_hosting"])

        op = self.accepted("POST", blocks_url(), self.create_body("w1"),
                           fixture="create-accepted")
        record("create-done", op)
        self.assertEqual(op["operation"], "create")
        self.assertEqual(op["block"], "w1")
        self.assertEqual(len(op["result"]["portals"]), len(HOSTS))

        code, b, _ = self.call("GET", blocks_url() + "/w1", status=200, schema="Block")
        self.assertEqual(b["size_bytes"], 16 * MIB)
        self.assertEqual(sorted(b["exported_on"]), sorted(HOSTS))
        code, vol, _ = self.call("GET", "/api/v1/volumes/%s" % VOL, status=200)
        self.assertTrue(vol["block_hosting"])

    def test_11_create_idempotent(self):
        code, b, _ = self.call("POST", blocks_url(), self.create_body("w1"), status=200,
                               schema="Block")
        self.assertEqual(b["name"], "w1")
        code, doc, _ = self.call("POST", blocks_url(), self.create_body("w1", size=32 * MIB),
                                 status=409, fixture="error-already-exists")
        self.assertEqual(doc["error"]["code"], "already_exists")

    def test_12_confirm(self):
        body = self.create_body("thin", prealloc="no")
        code, doc, _ = self.call("POST", blocks_url("dv"), body, status=409,
                                 fixture="error-confirm-required")
        self.assertEqual(doc["error"]["code"], "confirm_required")
        self.assertIn("dispersed", doc["error"]["message"])
        body["confirm"] = True
        self.accepted("POST", blocks_url("dv"), body)

    def test_13_busy(self):
        # zero filling keeps the volume busy for a while
        code, first, _ = self.call("POST", blocks_url(),
                                   self.create_body("w2", size=64 * MIB, zerofill=True),
                                   status=202)
        code, doc, _ = api("POST", blocks_url(), self.create_body("w3"))
        if code == 423:
            check_api(doc, "Error")
            record("error-volume-busy", doc)
            self.assertEqual(doc["error"]["code"], "volume_busy")
        else:
            # the zero fill was too fast to see it busy: both are accepted
            self.assertIn(code, (202, 200), doc)
            if code == 202:
                wait_operation(VOL, doc["id"])
        status, op = wait_operation(VOL, first["id"])
        self.assertEqual(op["state"], "succeeded", op)

    # -- changes --------------------------------------------------------------

    def test_20_resize(self):
        op = self.accepted("PATCH", blocks_url() + "/w1", {"size_bytes": 32 * MIB})
        self.assertEqual(op["operation"], "modify-size")
        code, b, _ = self.call("GET", blocks_url() + "/w1", status=200, schema="Block")
        self.assertEqual(b["size_bytes"], 32 * MIB)

        # shrinking needs force
        code, doc, _ = self.call("PATCH", blocks_url() + "/w1", {"size_bytes": 16 * MIB},
                                 status=409, fixture="error-force-required")
        self.assertEqual(doc["error"]["code"], "force_required")

        for body in ({}, {"size_bytes": 64 * MIB, "prio_path": HOSTS[0]}, {"force": True},
                     {"auth": True}, {"auth": {"enabled": True}, "force": True},
                     {"prio_path": "nodename"}):
            code, doc, _ = self.call("PATCH", blocks_url() + "/w1", body, status=400)
        self.call("PATCH", blocks_url() + "/nosuch", {"size_bytes": 64 * MIB}, status=404)

    def test_21_auth(self):
        code, b, _ = self.call("PATCH", blocks_url() + "/w1", {"auth": {"enabled": True}},
                               status=200, schema="Block", fixture="block-auth")
        self.assertTrue(b["auth"]["enabled"])
        code, b, _ = self.call("PATCH", blocks_url() + "/w1", {"auth": {"enabled": False}},
                               status=200, schema="Block")
        self.assertFalse(b["auth"]["enabled"])

    def test_22_prio_path(self):
        for host in (HOSTS[1], HOSTS[0]):
            code, b, _ = self.call("PATCH", blocks_url() + "/w1", {"prio_path": host},
                                   status=200, schema="Block")
            self.assertEqual(b["prio_path"], host)
        code, h, _ = self.call("GET", "/api/v1/volumes/%s/hosts/%s" % (VOL, HOSTS[0]),
                               status=200, schema="HostDetail")
        w1 = [x for x in h["block_states"] if x["name"] == "w1"][0]
        self.assertEqual(w1["alua"], "ao")

    def test_30_rebalance(self):
        code, plan, _ = self.call("POST", "/api/v1/volumes/%s/rebalance" % VOL,
                                  {"dry_run": True}, status=200, schema="RebalancePlan",
                                  fixture="rebalance-plan")
        self.assertTrue(plan["dry_run"])
        self.assertEqual(sorted(plan["prio_paths"]), sorted(HOSTS))
        code, doc, _ = api("POST", "/api/v1/volumes/%s/rebalance" % VOL, {})
        if plan["moves"]:
            self.assertEqual(code, 202, doc)
            status, op = wait_operation(VOL, doc["id"])
            self.assertEqual(op["state"], "succeeded", op)
        else:
            # nothing to move: done at once, the plan without a dry run
            self.assertEqual(code, 200, doc)
            check_api(doc, "RebalancePlan")
            self.assertFalse(doc["dry_run"])
        self.call("POST", "/api/v1/volumes/%s/rebalance" % VOL, {"dryrun": True}, status=400)

    def test_40_maintenance(self):
        url = "/api/v1/volumes/%s/hosts/%s/maintenance" % (VOL, HOSTS[-1])
        for body in (None, {"action": "pause"}, {"action": "start", "rebalance": True},
                     {"action": "start", "force": "yes"}):
            self.call("POST", url, body, status=400)

        op = self.accepted("POST", url, {"action": "start"}, fixture="maintenance-accepted")
        record("maintenance-done", op)
        code, h, _ = self.call("GET", "/api/v1/volumes/%s/hosts/%s" % (VOL, HOSTS[-1]),
                               status=200, schema="HostDetail")
        self.assertEqual(h["maintenance"], "maintenance")
        self.assertEqual(h["prio_paths"], 0)

        self.accepted("POST", url, {"action": "stop", "rebalance": True})
        code, h, _ = self.call("GET", "/api/v1/volumes/%s/hosts/%s" % (VOL, HOSTS[-1]),
                               status=200, schema="HostDetail")
        self.assertEqual(h["maintenance"], "active")

    # -- delete ---------------------------------------------------------------

    def test_50_delete(self):
        op = self.accepted("DELETE", blocks_url() + "/w1", fixture="delete-accepted")
        record("delete-done", op)
        self.assertEqual(op["operation"], "delete")
        self.call("GET", blocks_url() + "/w1", status=404)
        # again: it is gone, as asked
        code, doc, _ = self.call("DELETE", blocks_url() + "/w1", status=204)
        self.assertIsNone(doc)
        self.call("DELETE", blocks_url() + "/w1", {"force": True}, status=400)

    def test_51_cli_delete_background(self):
        cli("--wait", "create", "%s/c1" % VOL, "ha", str(len(HOSTS)), ",".join(HOSTS),
            "16MiB", check=True)
        cli("--wait", "create", "%s/c2" % VOL, "ha", str(len(HOSTS)), ",".join(HOSTS),
            "16MiB", check=True)

        rc, doc = cli("delete", "%s/c1" % VOL)
        self.assertEqual(rc, 0, doc)
        self.assertEqual(doc["RESULT"], "IN-PROGRESS", doc)
        status, op = wait_operation(VOL, doc["OPERATION-ID"])
        self.assertEqual(op["state"], "succeeded", op)
        self.assertEqual(op["operation"], "delete")

        rc, doc = cli("--wait", "delete", "%s/c2" % VOL)
        self.assertEqual(rc, 0, doc)
        self.assertEqual(doc["RESULT"], "SUCCESS", doc)
        rc, doc = cli("delete", "%s/c2" % VOL)
        self.assertEqual(rc, 2, doc)

    # -- audit ----------------------------------------------------------------

    def test_90_audit(self):
        time.sleep(1)
        with open("/var/log/gluster-block/gluster-block-apid.log") as f:
            log = f.read()
        self.assertIn("AUDIT unix:uid=0", log)
        self.assertRegex(log, r"AUDIT \S+ POST /api/v1/volumes/plain/blocks .*\"name\":\"w1\"")
        self.assertRegex(log, r"AUDIT \S+ DELETE /api/v1/volumes/plain/blocks/w1 - -> 202")
        with open("/var/log/gluster-block/cmd_history.log") as f:
            self.assertIn("api unix:uid=0", f.read())


if __name__ == "__main__":
    unittest.main()
