# Backlog

Planned work and ideas accepted for later.

## Host maintenance and high availability (branch `ha-maintenance`)

Take one host of a block hosting volume out of service (reboot, upgrade,
hardware work) and bring it back without I/O errors on the initiators.

Decisions:

- Scope is one block hosting volume: the hosts of a block are given at
  create, maintenance works on the blocks of that volume on the host.
- A host is drained by moving the prio (active/optimized ALUA) paths away,
  then disabling its TPGs (`tpg disable` + `targetcli saveconfig`), not by
  the ALUA standby state.
- Every phase documents its commands in README.md.

Done and documented in README.md: `hosts`, `modify ... prio-path`,
`hosts ... rebalance`, `hosts ... maintenance start|stop`.

### 1. Gluster checks

Before `maintenance start` refuse (unless `force`) when gluster would lose
the data path: a dispersed k+m volume may lose at most m bricks per
subvolume, replica 3 must keep client quorum, pending heals must be done.
Before `maintenance stop` wait for the heal of the bricks of the host.

### 2. Reboot and crash

- A host taken out with `force` while it was down still has its TPGs
  enabled in its saved configuration: gluster-blockd should disable the
  TPGs of the volumes where its host is marked MAINTENANCE when it starts.
- `reload` and `genconfig ... enable-tpg` must keep the TPGs of a host in
  maintenance disabled.

### 3. Documentation

README: the multipath settings of the initiator for the maintenance
(`no_path_retry queue`, ALUA prio, path checker interval).

### 4. Automatic failover of the prio path (later)

When a host goes down without maintenance, the blocks whose prio path was
on it keep the active/optimized ALUA group on a dead path until an admin
runs `rebalance`. gluster-blockd could detect the host failure (glusterd
peer disconnected, gluster-blockd of the host not answering) and move the
prio paths of the volume to the remaining hosts on its own, then rebalance
them again once the host is back.

Wait until the manual maintenance has been run on a real cluster.
