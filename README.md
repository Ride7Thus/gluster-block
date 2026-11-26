# gluster-block
gluster-block is a CLI utility, which aims at making Gluster backed block
storage creation and maintenance as simple as possible.

### Demo
------
[![asciicast](https://asciinema.org/a/237565.svg)](https://asciinema.org/a/237565)

### Install
------
<b>EL8 / EL9</b> (CentOS Stream 9, Rocky Linux 8/9, AlmaLinux 8/9): install the
build dependencies (enables PowerTools/CRB, EPEL and, on EL9, the CentOS Storage
SIG Gluster repository) and build the RPMs
<pre>
# git clone https://github.com/Ride7Thus/gluster-block.git
# cd gluster-block/
# extras/install_deps.sh
# extras/make_rpms.sh
# dnf install build/rpmbuild/RPMS/*/gluster-block-*.rpm
</pre>

With SELinux enforcing install `gluster-block-selinux` too (pulled in
automatically when selinux-policy-targeted is installed): targetclid, which
runs the targetcli commands of gluster-blockd, is confined and may not write
the configshell log in /var/log/gluster-block without it.

<b>Debian 12 / Ubuntu 22.04 / Ubuntu 24.04</b>: install the build dependencies
and build the .deb packages
<pre>
# git clone https://github.com/Ride7Thus/gluster-block.git
# cd gluster-block/
# extras/install_deps.sh
# extras/make_debs.sh
# apt-get install ./build/debs/gluster-block_*.deb
</pre>
or build from source
<pre>
# git clone https://github.com/Ride7Thus/gluster-block.git
# cd gluster-block/
# extras/install_deps.sh
# ./autogen.sh
# ./configure --prefix=/usr --sysconfdir=/etc --localstatedir=/var
# make -j install
# systemctl daemon-reload
# apt-get install glusterfs-server tcmu-runner targetcli-fb python3-rtslib-fb rpcbind
</pre>
Do not pass `--enable-tirpc=no`: the C library has no Sun RPC since glibc
2.26, gluster-block needs libtirpc. The settings of gluster-blockd are read
from `/etc/sysconfig/gluster-blockd` on every distribution.

CI builds the packages for every supported distribution: GitHub Actions
(`.github/workflows/rpm-build.yml`, `.github/workflows/deb-build.yml`) and
GitLab CI (`.gitlab-ci.yml`, runners tagged `common`). In GitLab the packages
are in the artifacts of the `rpm:<distro>` and `deb:<distro>` jobs under
`rpms/<distro>/` and `debs/<distro>/`, the `packages:all` job has all of them.

Or build from source
<pre>
# ./autogen.sh && ./configure && make -j install
</pre>

<b>Older releases</b>
<pre>
# dnf install gcc autoconf automake make file libtool libuuid-devel json-c-devel glusterfs-api-devel glusterfs-server tcmu-runner targetcli

On Fedora27 and Centos7 [Which use legacy glibc RPC], pass '--enable-tirpc=no' flag at configure time
# ./autogen.sh && ./configure --enable-tirpc=no && make -j install

On Fedora28 and higher [Which use TIRPC], in addition to above, we should also install
# dnf install rpcgen libtirpc-devel

And pass '--enable-tirpc=yes'(default) flag or nothing at configure time
# ./autogen.sh && ./configure && make -j install
</pre>

### Usage
------
**Prerequisites:** *this guide assumes that the following are already present*
- [x] *A block hosting gluster volume with name 'hosting-volume'*
- [x] *Open 24007(for glusterd) 24010(gluster-blockd) 3260(iscsi targets) 111(rpcbind) ports and glusterfs service in your firewall*

<b>Daemon</b>: gluster-blockd runs on all the nodes
```script
# gluster-blockd --help
gluster-blockd (0.4)
usage:
  gluster-blockd [--glfs-lru-count <COUNT>]
                 [--log-level <LOGLEVEL>]
                 [--no-remote-rpc]

commands:
  --glfs-lru-count <COUNT>
        Glfs objects cache capacity [max: 512] [default: 5]
  --log-level <LOGLEVEL>
        Logging severity. Valid options are,
        TRACE, DEBUG, INFO, WARNING, ERROR, CRIT and NONE [default: INFO]
  --no-remote-rpc
        Ignore remote rpc communication, capabilities check and
        other node sanity checks
  --help
        Show this message and exit.
  --version
        Show version info and exit.
```

You can run gluster-blockd as systemd service, note '/etc/sysconfig/gluster-blockd' is the configuration file which gets dynamicaly reloaded on changing any options.
<pre>
# cat /etc/sysconfig/gluster-blockd
# systemctl daemon-reload
# systemctl start gluster-blockd
</pre>

<b>CLI</b>: you can choose to run gluster-block(cli) from any node which has gluster-blockd running
```script
# gluster-block --help
gluster-block (0.4)
usage:
  gluster-block [timeout <seconds>] [--wait] [--yes] <command> <volname[/blockname]> [<args>] [--json*]

  create, delete and modify size return as soon as the request is
  accepted (RESULT: IN-PROGRESS) and finish in background, use 'status'
  to track them. With --wait (or GB_CLI_WAIT=yes) the cli waits for the
  result instead.

commands:
  create  <volname/blockname> [ha <count>]
                              [auth <enable|disable>]
                              [prealloc <full|no>]
                              [zerofill <yes|no>]
                              [storage <filename>]
                              [ring-buffer <size-in-MB-units>]
                              [block-size <size-in-Byte-units>]
                              [io-timeout <N-in-Second>]
                              <host1[,host2,...]> [size]
        create block device [defaults: ha 1, auth disable, prealloc full, zerofill no,
                             size in bytes,
                             ring-buffer and block-size default size dependends on kernel,
                             io-timeout 43s]

  list    <volname>
        list available block devices.

  status  <volname> [<operation-id>]
        current or last long running operation on the volume, or the
        result of the given operation (exit 115 while it runs).

  hosts   <volname> [<host>]
        state of the hosts that export the block devices of the volume:
        gluster-blockd, tcmu-runner, loaded blocks, enabled TPGs and
        prio paths; with <host> the state of each block on that host.

  hosts   <volname> rebalance [dry-run]
        spread the prio paths of the volume evenly over the hosts that
        answer, moving them off hosts that do not first. Runs in
        background, dry-run only shows the moves.

  hosts   <volname> <host> maintenance start [force]
        take the host out of service for the volume: move its prio
        paths away, disable its TPGs. Refused when a block would lose
        its last working path or the host does not answer, unless
        force. Runs in background.

  hosts   <volname> <host> maintenance stop [force] [rebalance]
        bring the host back: sync its ALUA groups, enable its TPGs;
        rebalance spreads the prio paths again, force skips the
        tcmu-runner check. Runs in background.

  volumes
        gluster volumes of the cluster: type, status, bricks and the
        number of block devices of the volumes that host them.

  version [<volname>]
        alone the version of the cli; with --json* or a volume the
        version, cli RPC revision and capabilities of gluster-blockd,
        with a volume also of every host of the volume and the
        capabilities all hosts that answer share.

  info    <volname/blockname>
        details about block device.

  delete  <volname/blockname> [unlink-storage <yes|no>] [force]
        delete block device.

  modify  <volname/blockname> [auth <enable|disable>] [size <size> [force]]
                              [prio-path <host> [force]]
        modify block device. prio-path moves the active/optimized path
        to <host> on all hosts of the block, force skips hosts that do
        not answer.

  replace <volname/blockname> <old-node> <new-node> [force]
        replace operations.

  reload <volname/blockname> [force]
        reload a block device.

  genconfig <volname[,volume2,volume3,...]> enable-tpg <host>
        generate the block volumes target configuration.

  help
        show this message and exit.

common cli options: (fixed formats)
  timeout <seconds>
        it is the time in seconds that cli can wait for daemon to respond.
        [default: timeout 300]
  --wait
        wait until a background create, delete, modify size, rebalance
        or maintenance finishes and print its result.
        [default: return RESULT: IN-PROGRESS at once]
  --yes
        do not ask for confirmation, e.g. 'prealloc no' on a dispersed
        volume.
  --json*
        used to request the output result in json format [default: plain text]
        supported JSON formats: --json|--json-plain|--json-spaced|--json-pretty
```

#### Example:

The hosts involved:

* 192.168.1.11, 192.168.1.12, 192.168.1.13: All nodes run gluster-blockd.service and glusterd.service (three nodes to achieve mutipath for HA)
* 192.168.1.14: Initiator, iSCSI client

Preparation:

* Create a gluster trusted storage pool of the 3 nodes
  192.168.1.11, 192.168.1.12, and 192.168.1.13.
    * Read more about [trusted storage pools](https://gluster.readthedocs.io/en/latest/Administrator%20Guide/Storage%20Pools/).
* Create a block hosting gluster volume called `hosting-volume` on the gluster cluster.
    * Read More on how to [create a gluster volume](https://gluster.readthedocs.io/en/latest/Administrator%20Guide/Setting%20Up%20Volumes/#creating-replicated-volumes).
    * We recommend replica 3 volume with group profile applied on it.
      * Helpful command: `# gluster vol set <block-hosting-volume> group gluster-block`
    * A dispersed (erasure coded) volume works as well, see
      [Dispersed volumes](#dispersed-volumes).

##### Dispersed volumes

gluster-block works on a dispersed (erasure coded) block hosting volume when
two conditions are met:

* The block file is preallocated: create it with `prealloc full` (the
  default). A thin file (`prealloc no`) allocates whole stripes while it is
  written and tcmu-runner fails on it, so the CLI asks for confirmation
  (or `--yes`) before it creates one. `modify size` preallocates the added
  space the same way.
* Mandatory locking is enabled on the volume, without it tcmu-runner can not
  open the block file and `targetcli` fails with
  `UserBackedStorageObject creation failed`:
  ```
  # gluster vol set <block-hosting-volume> group gluster-block
  # gluster vol set <block-hosting-volume> locks.mandatory-locking optimal
  ```

Preallocating a large block on a dispersed volume can take a long time, so
`create` and `modify size` run in the background and return
`RESULT: IN-PROGRESS` at once. Follow them with
`gluster-block status <volname>`: PHASE shows the current step with its
progress (allocating space or filling zeros) and FINISHED shows the
estimated time left. Use `--wait` to block until the operation ends.

You can execute gluster-block CLI from any of the 3 nodes where glusterd and gluster-blockd are running.


##### On the Server/Target node[s]
<pre>
Create 1G gluster block storage with name 'block-volume'
<b># gluster-block create hosting-volume/block-volume ha 3 192.168.1.11,192.168.1.12,192.168.1.13 1GiB</b>
IQN: iqn.2016-12.org.gluster-block:aafea465-9167-4880-b37c-2c36db8562ea
PORTAL(S): 192.168.1.11:3260 192.168.1.12:3260 192.168.1.13:3260
RESULT: SUCCESS

Enable Authentication (this can be part of create as well)
<b># gluster-block modify hosting-volume/block-volume auth enable</b>
IQN: iqn.2016-12.org.gluster-block:aafea465-9167-4880-b37c-2c36db8562ea
USERNAME: aafea465-9167-4880-b37c-2c36db8562ea
PASSWORD: 4a5c9b84-3a6d-44b4-9668-c9a6d699a5e9
SUCCESSFUL ON:  192.168.1.11 192.168.1.12 192.168.1.13
RESULT: SUCCESS

<b># gluster-block list hosting-volume</b>
block-volume

<b># gluster-block info hosting-volume/block-volume</b>
NAME: block-volume
VOLUME: hosting-volume
GBID: 6b60c53c-8ce0-4d8d-a42c-5b546bca3d09
SIZE: 1.0 GiB
HA: 3
EXPORTED NODE(S): 192.168.1.11 192.168.1.12 192.168.1.13
</pre>
<b>NOTE:</b> Block targets created using gluster-block utility will use TPG: 1 and LUN: 0.

Check what the hosts of the volume really export. Every host is asked in
parallel and reads its LIO configuration from configfs, a host that does not
answer is reported after 3 seconds.
<pre>
<b># gluster-block hosts hosting-volume</b>
VOLUME: hosting-volume
HOST          DAEMON       TCMU-RUNNER  TARGET    BLOCKS  LOADED  ENABLED  PRIO
192.168.1.11  0.5.27       running      active         1       1        1     1
192.168.1.12  0.5.27       running      active         1       1        1     0
192.168.1.13  unreachable  -            -              1       -        -     0

PROBLEMS:
  192.168.1.13: gluster-blockd unreachable (connection timed out after 3s)

<b># gluster-block hosts hosting-volume 192.168.1.12</b>
VOLUME: hosting-volume
HOST: 192.168.1.12
DAEMON: 0.5.27
TCMU-RUNNER: running
TARGET: active
BLOCKS: 1
LOADED: 1
TPG-ENABLED: 1
PRIO-PATHS: 0

BLOCK         META                  BACKSTORE  TPG  ENABLED  ALUA  PRIO
block-volume  CONFIGSUCCESS         loaded       2  yes      ano   no

PROBLEMS: none
</pre>
BLOCKS counts the blocks the metadata says the host exports, LOADED those
with a loaded tcmu backstore, ENABLED those whose TPG of the host is enabled
and PRIO those whose prio (active/optimized ALUA) path is the host. PROBLEMS
lists a host that does not answer, a stopped tcmu-runner, a block that is not
loaded, a disabled TPG, an ALUA group that does not match the prio path and
failed states in the metadata. With `--json` the result has `HEALTHY`.

Move the prio (active/optimized) path of a block to another of its hosts, for
example before taking the current one down. All hosts of the block put the
LUN of the TPG of the new host into the AO ALUA group and the others into the
ANO group, the initiators switch at their next path check. If one host fails
the others are put back. A host that does not answer stops the move, `force`
skips it, reload the block there once it is back.
<pre>
<b># gluster-block modify hosting-volume/block-volume prio-path 192.168.1.12</b>
NAME: block-volume
PRIO PATH: 192.168.1.12
PREVIOUS PRIO PATH: 192.168.1.11
SUCCESSFUL ON: 192.168.1.11 192.168.1.12 192.168.1.13
RESULT: SUCCESS
</pre>

Spread the prio paths of all blocks of the volume evenly over the hosts that
answer. Prio paths on a host that does not answer are moved first, that host
is left out (reload its blocks once it is back). It runs in background like
create, `--wait` waits for the result and `status` shows the progress;
`dry-run` only prints the moves.
<pre>
<b># gluster-block hosts hosting-volume rebalance dry-run</b>
VOLUME: hosting-volume
MOVES: 2 (dry run)
  block-1: 192.168.1.11 -> 192.168.1.12
  block-4: 192.168.1.11 -> 192.168.1.13
PRIO PATHS: 192.168.1.11=2 192.168.1.12=2 192.168.1.13=2
RESULT: SUCCESS
</pre>

##### Host maintenance

Take one host of the volume out of service (reboot, upgrade, hardware work)
without I/O errors on the initiators, then bring it back. It works on the
blocks of one block hosting volume, run it for every volume the host serves.

`maintenance start`:
1. refuses (unless `force`) when a block of the host has no other host that
   answers and is not in maintenance, or when the host itself does not answer;
2. marks the host `ENTERING`, moves the prio paths of its blocks to the other
   host of the block with the fewest prio paths;
3. waits until the initiators do no more I/O through the LUNs of the host:
   multipath follows the new ALUA states at its next path check. The LUN
   counters of the host (commands, MB read and written) are sampled every
   2 seconds; a LUN is idle when for `GB_MAINT_IO_IDLE_SEC` (10) seconds
   nothing was read or written and only the path checker sent commands
   (its sessions stay, they are dropped when the TPG is disabled). If the
   I/O does not stop within `GB_MAINT_IO_WAIT_SEC` (120) seconds the start
   stops, the TPGs stay enabled, the host stays `ENTERING` and LUNS WITH I/O
   lists the blocks with their initiators; run the start again once the
   I/O moved (both values in `/etc/sysconfig/gluster-blockd`);
4. disables the TPGs of the host and saves the target configuration, so a
   reboot keeps them disabled, then marks the host `MAINTENANCE`.

`force` skips the checks and the wait. It first shows what it would skip
(blocks without another path, LUNs with I/O right now and their initiators)
and asks to confirm; `--yes` answers for scripts.

`hosts <volname> <host>` shows the initiators logged in to each LUN of the
host (SESSIONS), with `--json` also the SCSI command and MB counters.

While a host is in maintenance `create` and `replace` onto it and
`modify prio-path` to it are refused, `hosts <volname> rebalance` leaves it
out. `hosts` shows it with STATE `MAINTENANCE`; it being down or its TPGs
being disabled is no problem then.

`maintenance stop` needs the host to answer and to run tcmu-runner (`force`
skips the tcmu-runner check), marks it `LEAVING`, puts the current ALUA
groups on it (prio paths moved while it was out), enables its TPGs and clears
the mark; blocks whose metadata reports a failed operation on the host are
listed under NEEDS ATTENTION. With `rebalance` the prio paths of the volume
are spread again. Both run in background, `--wait` waits for the result and
`status` shows the progress. A start or stop that failed half way leaves the
host `ENTERING` or `LEAVING`; run it again.
<pre>
<b># gluster-block --wait hosts hosting-volume 192.168.1.12 maintenance start</b>
HOST: 192.168.1.12
VOLUME: hosting-volume
MAINTENANCE: start
PRIO PATHS MOVED:
  block-2: 192.168.1.12 -> 192.168.1.11
TPGS DISABLED: 3 of 3
STATE: MAINTENANCE
RESULT: SUCCESS

<b># gluster-block --wait hosts hosting-volume 192.168.1.12 maintenance stop rebalance</b>
HOST: 192.168.1.12
VOLUME: hosting-volume
MAINTENANCE: stop
TPGS ENABLED: 3 of 3
REBALANCE:
  block-2: 192.168.1.11 -> 192.168.1.12
STATE: ACTIVE
RESULT: SUCCESS
</pre>

##### On the Client/Initiator node
<pre>
# dnf install iscsi-initiator-utils device-mapper-multipath
# systemctl start iscsid.service
# systemctl enable iscsid.service
# lsblk (note the available devices)

You can skip configuring multipath, if you choose not to enable mpath.
Below we set mapth in Active/Passive mode; Note currently Active/Active is not supported.
# modprobe dm_multipath
# mpathconf --enable

Please add the below configuration at the end of /etc/multipath.conf file.

For tcmu-runner version < 1.4.0, use:
# LIO iSCSI
devices {
        device {
                vendor "LIO-ORG"
                user_friendly_names "yes" # names like mpatha
                path_grouping_policy "failover" # one path per group
                path_selector "round-robin 0"
                failback immediate
                path_checker "tur"
                prio "const"
                no_path_retry 120
                rr_weight "uniform"
        }
}

For tcmu-runner version >= 1.4.0, use:
# LIO iSCSI
devices {
        device {
                vendor "LIO-ORG"
                user_friendly_names "yes" # names like mpatha
                path_grouping_policy "failover" # one path per group
                hardware_handler "1 alua"
                path_selector "round-robin 0"
                failback immediate
                path_checker "tur"
                prio "alua"
                no_path_retry 120
                rr_weight "uniform"
        }
}

# systemctl restart multipathd
# systemctl enable multipathd

Discovery ...
# iscsiadm -m discovery -t st -p 192.168.1.11

Update Credentials (Skip this step incase if you have not enabled auth)
# iscsiadm -m node -T "iqn.2016-12.org.gluster-block:aafea465-9167-4880-b37c-2c36db8562ea" -o update
 -n node.session.auth.authmethod -v CHAP -n node.session.auth.username -v aafea465-9167-4880-b37c-2c36db8562ea -n node
.session.auth.password -v 4a5c9b84-3a6d-44b4-9668-c9a6d699a5e9

Login ...
# iscsiadm -m node -T "iqn.2016-12.org.gluster-block:aafea465-9167-4880-b37c-2c36db8562ea" -l

# lsblk (note the new devices, let's say sdb, sdc and sdd multipath to mpatha)
# mkfs.xfs /dev/mapper/mpatha
# mount /dev/mapper/mpatha /mnt
</pre>

##### On the Server/Target node[s] to resize the block volume
<pre>
Resizing 1G gluster block volume 'block-volume' to 2G
<b># gluster-block modify hosting-volume/block-volume size 2GiB</b>
IQN: iqn.2016-12.org.gluster-block:aafea465-9167-4880-b37c-2c36db8562ea
SIZE: 2.0 GiB
SUCCESSFUL ON: 192.168.1.11 192.168.1.12 192.168.1.13
RESULT: SUCCESS
</pre>

##### On Initiator side, commands to refresh the device after block volume resizing
<pre>
Rescan the devices
# iscsiadm -m node -R

Rescan the multipath
# multipathd -k'resize map mpatha'

Grow the filesystem
# xfs_growfs  /mnt
</pre>

##### Deleting the block volume
<pre>
On client node
# umount /mnt
# iscsiadm -m node -u

On the server node (delete runs in background like create, --wait waits)
<b># gluster-block --wait delete hosting-volume/block-volume</b>
SUCCESSFUL ON: 192.168.1.11 192.168.1.12 192.168.1.13
RESULT: SUCCESS
</pre>

<b>NOTE:</b> gluster-block cannot track iSCSI targets created manually using targetcli.

##### Volumes and versions

`volumes` lists the gluster volumes with their type, status and bricks; for
the started ones it tells whether they host block devices and how many. A
volume hosts blocks once the first block is created on it: only `create`
makes the metadata, `list` and `hosts` show nothing on another volume and
the other commands answer that the block does not exist.
<pre>
<b># gluster-block volumes</b>
VOLUME                   TYPE                     STATUS   BLOCKS   BRICKS
hosting-volume           Replicate                Started  2        1 x 3 = 3
ec-volume                Disperse                 Started  -        1 x (2 + 1) = 3
</pre>

`version` alone prints the version of the cli. With a volume (or `--json`) it
asks gluster-blockd: its version, the revision of its cli RPC program (what
gluster-block-apid checks) and its capabilities, with a volume also those of
every host of the volume and the capabilities all hosts that answer share. A
host that does not answer is listed, it does not narrow the common ones.
<pre>
<b># gluster-block version hosting-volume</b>
VERSION: 0.5.34
CLI-RPC-REV: 17
CAPABILITIES: create create_ha create_prealloc create_auth delete ... host_maintenance
HOST: 192.168.1.11 up 0.5.34
HOST: 192.168.1.12 up 0.5.34
HOST: 192.168.1.13 up 0.5.33
COMMON-CAPABILITIES: create create_ha create_prealloc ... host_maintenance
</pre>

### HTTP API

The `gluster-block-api` package (EL only for now) has `gluster-block-apid`, a
bridge between HTTP clients and the local gluster-blockd. The Cockpit plugin
uses it; any node can be asked, its gluster-blockd coordinates the other
hosts of a block as it does for the cli. The API is described in
[docs/api/openapi.yaml](docs/api/openapi.yaml) (installed with the package):
JSON with snake_case keys, sizes in bytes, `null` for what is unknown, errors
as `{"error": {"code", "message", "errno"}}` with the HTTP status of the
errno (404 not found, 409 exists or needs a confirmation, 423 the volume is
busy with another operation, 502 a host does not answer, 503 gluster-blockd
does not answer or is older than gluster-block-apid). API v1 only changes
compatibly, CI checks it.

It listens on the unix socket `/run/gluster-block/api.sock`, mode 0600 for
root: the socket is the access control, its clients may do everything.
Settings are in
`/etc/sysconfig/gluster-block-apid`, the log is
`/var/log/gluster-block/gluster-block-apid.log`.
<pre>
<b># systemctl enable --now gluster-block-apid</b>
<b># curl -s --unix-socket /run/gluster-block/api.sock http://localhost/api/v1/volumes/hosting-volume/blocks/block-volume</b>
{"name":"block-volume","volume":"hosting-volume","gbid":"6b60c53c-...","size_bytes":1073741824,"ha":3,
 "prio_path":"192.168.1.11","exported_on":["192.168.1.11","192.168.1.12","192.168.1.13"],...}
</pre>

Endpoints, all under `/api/v1`:

| Endpoint | |
|---|---|
| `GET /version[?volume=<vol>]` | versions, capabilities and the `features` a client may offer |
| `GET /volumes`, `/volumes/<vol>` | gluster volumes |
| `GET /volumes/<vol>/blocks[?detail=1]`, `/blocks/<block>` | block devices |
| `POST /volumes/<vol>/blocks` | create (`name`, `size_bytes`, `hosts`, optional `auth`, `prealloc`, ...) |
| `PATCH /volumes/<vol>/blocks/<block>` | one of `size_bytes` (+`force` to shrink), `auth: {enabled}`, `prio_path` |
| `DELETE /volumes/<vol>/blocks/<block>[?unlink_storage=false&force=true]` | delete |
| `POST /volumes/<vol>/rebalance` | spread the prio paths (`dry_run` for the plan) |
| `POST /volumes/<vol>/hosts/<host>/maintenance` | `action` start or stop, `force`, `rebalance` |
| `GET /volumes/<vol>/hosts`, `/hosts/<host>` | state of the hosts, of each block on a host |
| `GET /volumes/<vol>/operations`, `/operations/<id>` | long operations, poll one until it is no longer `running` |
| `GET /healthz` | 200 when gluster-blockd answers and is compatible |

Create, delete, resize, rebalance and maintenance answer 202 with the
operation and its `Location`. A create of a block that exists with the same
size and hosts answers 200 with the block, a delete of a block that does not
exist 204, so a client may simply repeat a request. A thin block on a
dispersed volume or a forced maintenance answers 409 `confirm_required` with
what it would do: send the request again with `"confirm": true`.
<pre>
<b># curl -s -X POST --unix-socket /run/gluster-block/api.sock -H 'Content-Type: application/json' \
    -d '{"name": "block-2", "size_bytes": 10737418240, "hosts": ["192.168.1.11", "192.168.1.12", "192.168.1.13"]}' \
    http://localhost/api/v1/volumes/hosting-volume/blocks</b>
{"id":"3f0c...","volume":"hosting-volume","operation":"create","block":"block-2","state":"running",...}
<b># curl -s --unix-socket /run/gluster-block/api.sock http://localhost/api/v1/volumes/hosting-volume/operations/3f0c...</b>
{"id":"3f0c...","state":"running","phase":"allocating storage","step":{"percent":42,...},...}
</pre>

Remote clients (operators, scripts) use the TCP listener: set
`GB_API_LISTEN`, `GB_API_TLS_CERT` and `GB_API_TLS_KEY` in
`/etc/sysconfig/gluster-block-apid`, it is TLS only. A client shows a
certificate signed by `GB_API_TLS_CA` or a bearer token; the tokens file
(`/etc/gluster-block/api-tokens`, mode 0600) gives them the role `read` (GET
only) or `admin`. Anonymous clients get 401, except for `GET /healthz`.
<pre>
<b># gluster-block-apid --new-token backup-script read</b>
token: gbt_6b1c...
add this line to /etc/gluster-block/api-tokens and run 'systemctl reload gluster-block-apid':
token backup-script read sha256:9f2a...
<b># echo 'cert operator.example.com admin' >> /etc/gluster-block/api-tokens</b>
<b># systemctl reload gluster-block-apid</b>
<b># curl -s --cacert ca.crt -H 'Authorization: Bearer gbt_6b1c...' https://node1:8443/api/v1/volumes</b>
</pre>
Only the hash of a token is kept. Every change is logged as `AUDIT` with the
client in `gluster-block-apid.log`, and as `api <client>: ...` in the
cmd_history.log of gluster-blockd.

A restarted gluster-blockd of an older version is refused with 503
`daemon_incompatible` until it is upgraded: both come from the same build,
an upgrade of the packages restarts both.

------

## About Gluster
[Gluster](http://gluster.readthedocs.io/en/latest/) is a well known scale-out distributed storage system, flexible in its design and easy to use. One of its key goals is to provide high availability of data. Gluster is very easy to setup and use. Addition and removal of storage servers from a Gluster cluster is intuitive. These capabilities along with other data services that Gluster provides makes it a reliable software defined storage platform.

We can access glusterfs via [FUSE](https://en.wikipedia.org/wiki/Filesystem_in_Userspace) module. However to perform a single filesystem operation various context switches are required which can often exhibit performance issues. [Libgfapi](http://blog.gluster.org/2014/04/play-with-libgfapi-and-its-python-bindings/) is a userspace library for accessing data in Glusterfs. It can perform I/O on gluster volumes without the FUSE module, kernel VFS layer and hence requires no context switches. It exposes a filesystem like API for accessing gluster volumes. Samba, NFS-Ganesha, QEMU and now the tcmu-runner all use libgfapi to integrate with Gluster.

> A unique distributed storage solution build on traditional filesystems

### How we provide block storage in gluster ?

![untitled diagram](https://cloud.githubusercontent.com/assets/12432241/21478518/235e533c-cb72-11e6-9c5a-e351513a34b7.png)

1. Create a file in the gluster volume (Block Hosting Volume)
2. We expose the file in the gluster volume as tcmu backstore using tcmu-runner, exporting the target file as iSCSI LUN and
3. From the initiator we login to the exported LUN and play with the block device

#### Background
The [SCSI](http://searchstorage.techtarget.com/definition/SCSI) subsystem uses a form of client-server model.  The Client/Initiator request I/O happen through target which is a storage device. The SCSI target subsystem enables a computer node to behave as a SCSI storage device, responding to storage requests by other SCSI initiator nodes.

In simple terms SCSI is a set of standards for physically connecting and transferring data between computers and peripheral devices.

The most common implementation of the SCSI target subsystem is an iSCSIserver, [iSCSI](http://searchstorage.techtarget.com/definition/iSCSI) transports block level data between the iSCSI initiator and the target which resides on the actual storage device. iSCSi protocol wraps up the SCSI commands and sends it over TCP/IP layer. Up on receiving the packets at the other end it disassembles them to form the same SCSI commands, hence on the OS’es it seen as local SCSI device.

> In other words iSCSI is SCSI over TCP/IP.

The [LIO](http://linux-iscsi.org/wiki/LIO) project began with the iSCSI design as its core objective, and created a generic SCSI target subsystem to support iSCSI. LIO is the SCSI target in the Linux kernel. It is entirely kernel code, and allows exported SCSI logical units (LUNs) to be backed by regular files or block devices.

> LIO is Linux IO target, is an implementation of iSCSI target.

[TCM](https://www.kernel.org/doc/Documentation/target/tcmu-design.txt) is another name for LIO, an in-kernel iSCSI target (server). As we know existing TCM targets run in the kernel. TCMU (TCM in Userspace) allows userspace programs to be written which act as iSCSI targets. These enables wider variety of backstores without kernel code. Hence the TCMU userspace-passthrough backstore allows a userspace process to handle requests to a LUN. TCMU utilizes the traditional UIO subsystem, which is designed to allow device driver development in userspace.

> One such backstore with best clustered network storage capabilities is GlusterFS

Any TCMU userspace-passthrough can utilize the TCMU framework handling the messy details of the TCMU interface.
One such passthrough is [Tcmu-runner](https://github.com/open-iscsi/tcmu-runner) (Thanks to Andy Grover). Tcmu-runner has a glusterfs handler that can interact with the backed file in gluster volume over gluster libgfapi interface and can show it as a target (over network).

Some responsibilities of userspace-passthrough include,

Discovering and configuring TCMU UIO devices
waiting for the events on the device and
managing the command ring buffers

[TargetCli](https://github.com/Datera/targetcli) is the general management platform for the LIO/TCM/TCMU. TargetCli with its shell interface is used to configure LIO.
> Think it like a shell which makes life easy in configuring LIO core

------
### How to quickly bringup gluster-block environment locally ?
<pre>
Fedora:
# dnf -y install qemu libvirt libvirt-devel ruby-devel gcc vagrant ansible

CentOS:
# yum -y install qemu libvirt libvirt-devel ruby-devel gcc qemu-kvm ansible

Note: Please download and install vagrant package for CentOS from:
https://www.vagrantup.com/downloads.html


Start and enable libvirtd service
# systemctl start libvirtd
# systemctl enable libvirtd

Now install vagrant libvirt plugin
# vagrant plugin install vagrant-libvirt

Make sure you are in gluster-block root directory
# vagrant up
</pre>

### Managing the vagrant Vm's
<pre>

To check VMs status
# vagrant status

To ssh and get access to a VM
# vagrant ssh {name}

To stop the VMs
# vagrant halt

To destroy the VMs
# vagrant destroy

Check more commands with
# vagrant list-commands
</pre>

------

## License
gluster-block is licensed to you under your choice of the GNU Lesser General Public License, version 3 or any later version ([LGPLv3](https://opensource.org/licenses/lgpl-3.0.html) or later), or the GNU General Public License, version 2 ([GPLv2](https://opensource.org/licenses/GPL-2.0)), in all cases as published by the Free Software Foundation.

## Maintainers
See [MAINTAINERS](https://github.com/gluster/gluster-block/blob/master/MAINTAINERS.md) file

## Community
* Please join our [mailing list](https://lists.gluster.org/mailman/listinfo/gluster-devel)
* To ask a question or start a discussion, you can also raise an [issue](https://github.com/gluster/gluster-block/issues)
* IRC: #gluster-devel on Freenode
