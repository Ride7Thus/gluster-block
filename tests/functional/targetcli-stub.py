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
"""targetcli stand-in for the functional tests, where there is no LIO.

It answers the commands gluster-blockd pipes into targetcli with the lines
gluster-blockd checks, and keeps the LIO configuration as a directory tree
like configfs, under $GB_FAKE_CONFIGFS (default /sys/kernel/config/target,
a tmpfs per test host), where gluster-blockd reads it:

  core/user_0/<block>/{alua/<group>/, .cfgstring, .size}
  iscsi/<iqn>/tpgt_<n>/{enable, np/<ip>:3260/, dynamic_sessions,
                        lun/lun_0/{alua_tg_pt_gp, statistics/scsi_tgt_port/*}}
"""

import fcntl
import os
import re
import shutil
import sys

ROOT = os.environ.get("GB_FAKE_CONFIGFS", "/sys/kernel/config/target")
CORE = os.path.join(ROOT, "core", "user_0")
ISCSI = os.path.join(ROOT, "iscsi")
GLFS = "/backstores/user:glfs"


def write(path, value):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        f.write("%s\n" % value)


def read(path, default=""):
    try:
        with open(path) as f:
            return f.read().strip()
    except OSError:
        return default


def tpgdir(iqn, n):
    return os.path.join(ISCSI, iqn, "tpgt_%s" % n)


def mktpg(iqn, n):
    d = tpgdir(iqn, n)
    os.makedirs(os.path.join(d, "np"), exist_ok=True)
    if not os.path.exists(os.path.join(d, "enable")):
        write(os.path.join(d, "enable"), 0)
        write(os.path.join(d, "dynamic_sessions"), "")


def ls_backstores():
    if not os.path.isdir(CORE):
        return
    for name in sorted(os.listdir(CORE)):
        d = os.path.join(CORE, name)
        print("  | o- %s ......... [%s (%s) activated]"
              % (name, read(os.path.join(d, ".cfgstring")), read(os.path.join(d, ".size"))))


def ls_iscsi(iqn=None):
    if not os.path.isdir(ISCSI):
        return
    if iqn:
        n = len([t for t in os.listdir(os.path.join(ISCSI, iqn)) if t.startswith("tpgt_")])
        print("Status for /iscsi/%s: TPGs: %d" % (iqn, n))
        return
    for name in sorted(os.listdir(ISCSI)):
        n = len([t for t in os.listdir(os.path.join(ISCSI, name)) if t.startswith("tpgt_")])
        print("  o- %s ......... [TPGs: %d]" % (name, n))


def run(line, out):
    line = line.strip()
    if not line or line == "exit":
        return

    m = re.match(GLFS + r" create name=(\S+) size=(\d+) cfgstring=(\S+)", line)
    if m:
        d = os.path.join(CORE, m.group(1))
        os.makedirs(os.path.join(d, "alua"), exist_ok=True)
        write(os.path.join(d, ".cfgstring"), m.group(3))
        write(os.path.join(d, ".size"), m.group(2))
        out.append("Created user-backed storage object %s size %s." % (m.group(1), m.group(2)))
        return
    m = re.match(GLFS + r" delete (?:name=)?([^\s=]+)", line)
    if m:
        shutil.rmtree(os.path.join(CORE, m.group(1)), ignore_errors=True)
        out.append("Deleted storage object %s." % m.group(1))
        return
    m = re.match(GLFS + r"/(\S+)/alua create name=(\S+)", line)
    if m:
        os.makedirs(os.path.join(CORE, m.group(1), "alua", m.group(2)), exist_ok=True)
        out.append("Created ALUA TG PT Group %s." % m.group(2))
        return
    m = re.match(GLFS + r"/(\S+) set attribute dev_size=(\d+)", line)
    if m:
        write(os.path.join(CORE, m.group(1), ".size"), m.group(2))
        out.append("Parameter dev_size is now '%s'." % m.group(2))
        return
    m = re.match(GLFS + r"/(\S+) saveconfig", line)
    if m:
        out.append("Configuration saved to /etc/target/saveconfig.json")
        return
    m = re.match(r"/iscsi create (\S+)", line)
    if m:
        mktpg(m.group(1), 1)
        out.append("Created target %s." % m.group(1))
        out.append("Created TPG 1.")
        return
    m = re.match(r"/iscsi delete (\S+)", line)
    if m:
        shutil.rmtree(os.path.join(ISCSI, m.group(1)), ignore_errors=True)
        out.append("Deleted Target %s." % m.group(1))
        return

    # several commands may be on one line: "... create tpg2\n /iscsi/... create tpg3"
    for m in re.finditer(r"/iscsi/(\S+) create tpg(\d+)", line):
        mktpg(m.group(1), m.group(2))
        out.append("Created TPG %s." % m.group(2))
    m = re.match(r"/iscsi/(\S+)/tpg(\d+)/luns create (\S+)", line)
    if m:
        d = os.path.join(tpgdir(m.group(1), m.group(2)), "lun", "lun_0")
        write(os.path.join(d, "alua_tg_pt_gp"), "default_tg_pt_gp")
        for f in ("in_cmds", "read_mbytes", "write_mbytes"):
            write(os.path.join(d, "statistics", "scsi_tgt_port", f), 0)
        out.append("Created LUN 0.")
        return
    m = re.match(r"/iscsi/(\S+)/tpg(\d+)/luns/lun0 set alua alua_tg_pt_gp_name=(\S+)", line)
    if m:
        write(os.path.join(tpgdir(m.group(1), m.group(2)), "lun", "lun_0", "alua_tg_pt_gp"),
              m.group(3))
        out.append("Parameter alua_tg_pt_gp_name is now '%s'." % m.group(3))
        return
    m = re.match(r"/iscsi/(\S+)/tpg(\d+)/portals create (\S+)", line)
    if m:
        os.makedirs(os.path.join(tpgdir(m.group(1), m.group(2)), "np", "%s:3260" % m.group(3)),
                    exist_ok=True)
        out.append("Created network portal %s:3260." % m.group(3))
        return
    m = re.match(r"/iscsi/(\S+)/tpg(\d+)/portals delete (\S+)", line)
    if m:
        shutil.rmtree(os.path.join(tpgdir(m.group(1), m.group(2)), "np", "%s:3260" % m.group(3)),
                      ignore_errors=True)
        out.append("Deleted network portal %s:3260" % m.group(3))
        return
    m = re.match(r"/iscsi/(\S+)/tpg(\d+) (enable|disable)$", line)
    if m:
        write(os.path.join(tpgdir(m.group(1), m.group(2)), "enable"),
              1 if m.group(3) == "enable" else 0)
        out.append("The TPGT has been %sd." % m.group(3))
        return
    if " set attribute " in line or " set auth " in line or " set alua " in line:
        for k, v in re.findall(r"(\w+)=(\S+)", line):
            out.append("Parameter %s is now '%s'." % (k, v))
        return
    if line.endswith("saveconfig"):
        out.append("Configuration saved to /etc/target/saveconfig.json")


def main():
    os.makedirs(ROOT, exist_ok=True)
    lock = open(os.path.join(ROOT, ".tgstub.lock"), "w")
    fcntl.flock(lock, fcntl.LOCK_EX)

    args = " ".join(sys.argv[1:])
    if args in ("--version", "version"):
        print("targetcli version 2.1.53")
        return 0
    m = re.match(r"/iscsi/(\S+) status$", args)
    if m:
        ls_iscsi(m.group(1))
        return 0
    if args.endswith(" ls") or args == "ls":
        path = args[:-3].strip() if args != "ls" else ""
        if path.startswith(GLFS) or path == "/backstores":
            ls_backstores()
        elif path.startswith("/iscsi/"):
            ls_iscsi(path[len("/iscsi/"):])
        elif path.startswith("/iscsi"):
            ls_iscsi()
        return 0
    if args:
        # one command on the command line: targetcli /iscsi/... saveconfig
        out = []
        run(args, out)
        print("\n".join(out))
        return 0

    out = []
    for line in sys.stdin.read().splitlines():
        run(line, out)
    print("\n".join(out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
