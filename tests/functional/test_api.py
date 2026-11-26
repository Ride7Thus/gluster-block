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
"""Functional tests of gluster-block-apid (API v1) and of the JSON answers of
gluster-blockd it is built on, on the test cluster of setup.sh.

Every API response is checked against docs/api/openapi.yaml in strict mode
(a field that is not in the contract fails), every daemon answer against
tests/compat/json/daemon.schema.json. The responses are recorded in
$GB_TEST_OUT/fixtures for the tests of the Cockpit plugin.
"""

import os
import sys
import time
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from gbtest import (HOSTS, api, check_api, check_daemon, cli, record, sh,  # noqa: E402
                    wait_operation)

VOL = "rv"


class ApiReadTest(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        hosts = ",".join(HOSTS)
        cli("--wait", "create", "%s/b1" % VOL, "ha", str(len(HOSTS)), hosts, "16MiB", check=True)
        cli("--wait", "create", "%s/b2" % VOL, "ha", "2", "auth", "enable",
            ",".join(HOSTS[:2]), "32MiB", check=True)

    def get(self, path, status=200, schema=None, fixture=None):
        code, doc, headers = api("GET", path)
        self.assertEqual(code, status, "%s: %s" % (path, doc))
        if status >= 400:
            check_api(doc, "Error")
        elif schema:
            check_api(doc, schema)
        if doc is not None:
            self.assertEqual(headers.get("Content-Type"), "application/json")
        if fixture:
            record(fixture, doc)
        return doc

    # -- index, health, version ----------------------------------------------

    def test_01_index_health(self):
        doc = self.get("/api/v1", schema="Index", fixture="index")
        self.assertEqual(doc["api_version"], "1.0")
        self.get("/healthz", schema="Health", fixture="healthz")
        self.get("/api/v1/healthz", schema="Health")

    def test_02_version(self):
        rc, raw = cli("version")
        self.assertEqual(rc, 0)
        check_daemon(raw, "version")
        record("daemon-version", raw)

        doc = self.get("/api/v1/version", schema="Version", fixture="version")
        self.assertGreaterEqual(doc["daemon"]["cli_rpc_rev"], 16)
        self.assertIn("block_create", doc["features"])
        self.assertNotIn("volume", doc)

    def test_03_version_volume(self):
        rc, raw = cli("version", VOL)
        self.assertEqual(rc, 0)
        check_daemon(raw, "version_volume")
        record("daemon-version-volume", raw)

        doc = self.get("/api/v1/version?volume=%s" % VOL, schema="Version",
                       fixture="version-volume")
        self.assertEqual(sorted(h["host"] for h in doc["volume"]["hosts"]), sorted(HOSTS))
        for h in doc["volume"]["hosts"]:
            self.assertEqual(h["daemon"], "up")
            self.assertIn("host_maintenance", h["capabilities"])
        self.assertIn("host_maintenance", doc["features"])

    # -- volumes --------------------------------------------------------------

    def test_10_volumes(self):
        rc, raw = cli("volumes")
        self.assertEqual(rc, 0)
        check_daemon(raw, "volumes")
        record("daemon-volumes", raw)

        doc = self.get("/api/v1/volumes?refresh=1", schema="VolumeList", fixture="volumes")
        vols = {v["name"]: v for v in doc["volumes"]}
        self.assertTrue(vols[VOL]["block_hosting"])
        self.assertEqual(vols[VOL]["blocks"], 2)
        self.assertEqual(vols[VOL]["type"], "replicate")
        self.assertEqual(len(vols[VOL]["bricks"]), 3)
        self.assertEqual(vols["dv"]["type"], "disperse")
        self.assertFalse(vols["plain"]["block_hosting"])
        self.assertIsNone(vols["plain"]["blocks"])

        one = self.get("/api/v1/volumes/%s" % VOL, schema="Volume", fixture="volume")
        self.assertEqual(one["name"], VOL)
        self.get("/api/v1/volumes/nosuchvol", status=404, fixture="error-volume-not-found")

    def test_11_not_block_hosting(self):
        doc = self.get("/api/v1/volumes/plain/blocks", status=404,
                       fixture="error-not-block-hosting")
        self.assertEqual(doc["error"]["code"], "not_block_hosting")
        # asking must not turn the volume into a block hosting one
        doc = self.get("/api/v1/volumes/plain?refresh=1", schema="Volume")
        self.assertFalse(doc["block_hosting"])

    def test_12_read_creates_nothing(self):
        # only create makes the metadata of a volume, nothing else may
        rc, raw = cli("list", "plain")
        self.assertEqual(rc, 0, raw)
        self.assertEqual(raw["blocks"], [])
        rc, raw = cli("info", "plain/nosuch")
        self.assertEqual(rc, 2, raw)
        rc, raw = cli("hosts", "plain")
        self.assertEqual(rc, 0, raw)
        self.assertEqual(raw["HOSTS"], [])
        rc, raw = cli("version", "plain")
        self.assertEqual(rc, 0, raw)
        self.assertEqual(raw["HOSTS"], [])
        rc, raw = cli("status", "plain")
        self.assertEqual(rc, 0, raw)
        rc, raw = cli("delete", "plain/nosuch")
        self.assertEqual(rc, 2, raw)
        rc, raw = cli("modify", "plain/nosuch", "size", "32MiB")
        self.assertNotEqual(rc, 0, raw)
        rc, raw = cli("genconfig", "plain", "enable-tpg", HOSTS[0])
        self.assertEqual(rc, 0, raw)

        doc = self.get("/api/v1/volumes/plain?refresh=1", schema="Volume")
        self.assertFalse(doc["block_hosting"], "a read only command made block-meta")
        rc, raw = cli("volumes")
        plain = [v for v in raw["VOLUMES"] if v["NAME"] == "plain"][0]
        self.assertFalse(plain["BLOCK-HOSTING"])

    # -- blocks ---------------------------------------------------------------

    def test_20_blocks(self):
        rc, raw = cli("list", VOL)
        self.assertEqual(rc, 0)
        check_daemon(raw, "list")

        doc = self.get("/api/v1/volumes/%s/blocks" % VOL, schema="BlockList", fixture="blocks")
        self.assertEqual(sorted(b["name"] for b in doc["blocks"]), ["b1", "b2"])
        self.assertEqual(sorted(doc["blocks"][0].keys()), ["name"])

        doc = self.get("/api/v1/volumes/%s/blocks?detail=1" % VOL, schema="BlockList",
                       fixture="blocks-detail")
        for b in doc["blocks"]:
            check_api(b, "Block")

    def test_21_block(self):
        rc, raw = cli("info", "%s/b1" % VOL)
        self.assertEqual(rc, 0)
        check_daemon(raw, "info")
        record("daemon-info", raw)

        b = self.get("/api/v1/volumes/%s/blocks/b1" % VOL, schema="Block", fixture="block")
        self.assertEqual(b["size_bytes"], 16 * 1024 * 1024)
        self.assertEqual(b["ha"], len(HOSTS))
        self.assertIn(b["prio_path"], HOSTS)
        self.assertEqual(sorted(b["exported_on"]), sorted(HOSTS))
        self.assertEqual(sorted(b["portals"]), sorted(h + ":3260" for h in HOSTS))
        self.assertTrue(b["iqn"].endswith(b["gbid"]))
        self.assertFalse(b["auth"]["enabled"])
        self.assertNotIn("password", b["auth"])
        self.assertEqual(b["create_state"], "done")

    def test_22_block_auth(self):
        b = self.get("/api/v1/volumes/%s/blocks/b2" % VOL, schema="Block")
        self.assertTrue(b["auth"]["enabled"])
        self.assertEqual(b["auth"]["username"], b["gbid"])
        self.assertNotIn("password", b["auth"])
        b = self.get("/api/v1/volumes/%s/blocks/b2?show_password=true" % VOL, schema="Block")
        self.assertTrue(b["auth"]["password"])

    def test_23_block_errors(self):
        doc = self.get("/api/v1/volumes/%s/blocks/nosuch" % VOL, status=404,
                       fixture="error-block-not-found")
        self.assertEqual(doc["error"]["code"], "block_not_found")
        doc = self.get("/api/v1/volumes/%s/blocks/bad.name" % VOL, status=400)
        self.assertEqual(doc["error"]["code"], "invalid_request")

    # -- hosts ----------------------------------------------------------------

    def test_30_hosts(self):
        rc, raw = cli("hosts", VOL)
        self.assertEqual(rc, 0)
        check_daemon(raw, "hosts")
        record("daemon-hosts", raw)

        doc = self.get("/api/v1/volumes/%s/hosts" % VOL, schema="HostList", fixture="hosts")
        hosts = {h["host"]: h for h in doc["hosts"]}
        self.assertEqual(sorted(hosts), sorted(HOSTS))
        for h in hosts.values():
            self.assertEqual(h["daemon"], "up")
            self.assertEqual(h["maintenance"], "active")
            self.assertEqual(h["tcmu_runner"], "running")
        self.assertEqual(sum(h["prio_paths"] for h in hosts.values()), 2)

    def test_31_host(self):
        rc, raw = cli("hosts", VOL, HOSTS[1])
        self.assertEqual(rc, 0)
        check_daemon(raw, "hosts")

        h = self.get("/api/v1/volumes/%s/hosts/%s" % (VOL, HOSTS[1]), schema="HostDetail",
                     fixture="host")
        self.assertEqual(h["volume"], VOL)
        names = {b["name"]: b for b in h["block_states"]}
        self.assertEqual(sorted(names), ["b1", "b2"])
        for b in names.values():
            self.assertTrue(b["backstore_loaded"])
            self.assertTrue(b["tpg_enabled"])
            self.assertIn(b["alua"], ("ao", "ano"))
            self.assertEqual(b["alua"] == "ao", b["prio"])

        self.get("/api/v1/volumes/%s/hosts/10.250.250.250" % VOL, status=404,
                 fixture="error-host-not-found")

    # -- operations -----------------------------------------------------------

    def test_40_operations(self):
        rc, raw = cli("status", VOL)
        self.assertEqual(rc, 0)
        check_daemon(raw, "status")

        doc = self.get("/api/v1/volumes/%s/operations" % VOL, schema="OperationList",
                       fixture="operations")
        self.assertFalse(doc["busy"])
        self.assertEqual(doc["last"]["state"], "succeeded")

    def test_41_operation_progress(self):
        # zero filling 64 MiB takes long enough to see it running
        rc, raw = cli("create", "%s/b3" % VOL, "ha", str(len(HOSTS)), "zerofill", "yes",
                      ",".join(HOSTS), "64MiB")
        self.assertEqual(rc, 0, raw)
        check_daemon(raw, "accepted")
        opid = raw["OPERATION-ID"]

        doc = self.get("/api/v1/volumes/%s/operations/%s" % (VOL, opid),
                       schema="OperationDetail", fixture="operation-running")
        self.assertIn(doc["state"], ("running", "succeeded"))
        busy = self.get("/api/v1/volumes/%s/operations" % VOL, schema="OperationList")
        if doc["state"] == "running" and busy["busy"]:
            record("operations-busy", busy)
            self.assertEqual(busy["current"]["id"], opid)

        status, doc = wait_operation(VOL, opid)
        self.assertEqual(status, 200)
        check_api(doc, "OperationDetail")
        record("operation-done", doc)
        self.assertEqual(doc["state"], "succeeded", doc)
        self.assertEqual(doc["operation"], "create")
        self.assertEqual(doc["block"], "b3")
        self.assertTrue(doc["result"]["iqn"])
        self.assertEqual(len(doc["result"]["portals"]), len(HOSTS))

    def test_42_operation_errors(self):
        doc = self.get("/api/v1/volumes/%s/operations/00000000-0000-0000-0000-000000000000"
                       % VOL, status=404, fixture="error-operation-not-found")
        self.assertEqual(doc["error"]["code"], "operation_not_found")

    # -- routing --------------------------------------------------------------

    def test_50_routing(self):
        self.get("/api/v1/nothing", status=404)
        self.get("/nothing", status=404)
        code, doc, headers = api("POST", "/api/v1/volumes")
        self.assertEqual(code, 405)
        self.assertEqual(headers.get("Allow"), "GET")
        check_api(doc, "Error")

    # -- hosts and daemons down (last: they change the cluster) ---------------

    def test_80_host_down(self):
        if len(HOSTS) < 3:
            self.skipTest("needs 3 hosts")
        sh("host_down 3")
        try:
            doc = self.get("/api/v1/volumes/%s/hosts" % VOL, schema="HostList",
                           fixture="hosts-down")
            h = {x["host"]: x for x in doc["hosts"]}[HOSTS[2]]
            self.assertEqual(h["daemon"], "unreachable")
            self.assertTrue(h["problems"])
            self.assertFalse(doc["healthy"])

            doc = self.get("/api/v1/version?volume=%s" % VOL, schema="Version",
                           fixture="version-host-down")
            v = {x["host"]: x for x in doc["volume"]["hosts"]}[HOSTS[2]]
            self.assertEqual(v["daemon"], "unreachable")
            # a host that does not answer does not narrow the features
            self.assertIn("host_maintenance", doc["features"])
        finally:
            sh("host_up 3")

    def test_90_daemon_down(self):
        # host 1 only: its gluster-blockd is the one in the network namespace of pid 1
        sh("for p in $(pgrep -x gluster-blockd); do "
           "[ \"$(readlink /proc/$p/ns/net)\" = \"$(readlink /proc/1/ns/net)\" ] && kill $p; done")
        time.sleep(2)
        try:
            doc = self.get("/healthz", status=503, fixture="error-daemon-unavailable")
            self.assertEqual(doc["error"]["code"], "daemon_unavailable")
            doc = self.get("/api/v1/volumes/%s/blocks" % VOL, status=503)
            self.assertEqual(doc["error"]["code"], "daemon_unavailable")
        finally:
            sh("gluster-blockd --log-level DEBUG > /dev/null 2>&1 & "
               "for i in $(seq 1 30); do [ -S /var/run/gluster-blockd.socket ] && "
               "ss -ltn | grep -q ':24010 ' && break; sleep 1; done")
        time.sleep(1)
        self.get("/healthz", schema="Health")


if __name__ == "__main__":
    unittest.main()
