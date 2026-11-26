/*
  Copyright (c) 2026 gluster-block contributors
  This file is part of gluster-block.

  This file is licensed to you under your choice of the GNU Lesser
  General Public License, version 3 or any later version (LGPLv3 or
  later), or the GNU General Public License, version 2 (GPLv2), in all
  cases as published by the Free Software Foundation.
*/

/*
 * Shared by the host management of a block hosting volume: prio paths
 * (block_prio.c), host maintenance (block_maint.c) and host state
 * (block_hosts.c).
 */

# ifndef _BLOCK_HA_H
# define _BLOCK_HA_H 1

# include  "block_common.h"


/* maintenance state of a host of a volume, "" (ACTIVE) when not set */
# define  GB_HOST_STATE_ACTIVE       "ACTIVE"
# define  GB_HOST_STATE_ENTERING     "ENTERING"      /* maintenance start runs or failed */
# define  GB_HOST_STATE_MAINTENANCE  "MAINTENANCE"
# define  GB_HOST_STATE_LEAVING      "LEAVING"       /* maintenance stop runs or failed */

/* xattrs user.block.host.<addr> of this file keep the states */
# define  GB_HOSTS_FILENAME          "hosts.info"
# define  GB_HOSTS_FILE              GB_METADIR "/" GB_HOSTS_FILENAME
# define  GB_HOSTS_ATTR_PREFIX       "user.block.host"


/* answer of a host to the probe, shared by all blocks of an operation */
typedef struct gbReach {
  char  addr[255];
  bool  reachable;
  char  error[256];
  char  state[32];      /* maintenance state, "" when active */
} gbReach;


typedef struct gbPrioResult {
  char *prio;           /* new prio path */
  char *previous;
  char *success;        /* hosts, space separated */
  char *skipped;        /* unreachable hosts left out with force */
  char *failed;         /* "host: reason" joined by ", " */
  char *rolledback;
  bool  unchanged;
} gbPrioResult;


typedef struct gbRbBlock {
  char      name[255];
  MetaInfo *info;
  int       cur;        /* reach index of the planned prio path, -1 none */
  int       from;       /* reach index of the prio path now, -1 none */
  int       done;       /* move: 0 not run, 1 done, -1 failed */
  char     *msg;
} gbRbBlock;


typedef struct gbRebalance {
  gbReach   *reach;
  size_t     nreach;
  size_t    *load;
  gbRbBlock *blocks;
  size_t     nblocks;
  char      *skipped;   /* "block: reason\n" */
} gbRebalance;


int gbJoin(char **out, const char *sep, const char *fmt, ...)
  __attribute__((format(printf, 3, 4)));

gbReach *gbReachFind(gbReach *reach, size_t nreach, const char *addr);

void *gbReachProbe(void *data);

int gbPrioMove(struct glfs *glfs, char *volume, char *block, MetaInfo *info,
               const char *prio, bool force, gbReach *reach, size_t nreach,
               void *capsBlk, gbPrioResult *r, char **errMsg);

void gbPrioResultFree(gbPrioResult *r);

/* all blocks of the volume, with @withNoPrio also those without ALUA */
int gbRebalanceRead(struct glfs *glfs, char *volume, gbRebalance *rb,
                    bool withNoPrio, int *errCode, char **errMsg);

void gbRebalanceProbe(gbRebalance *rb);

void gbRebalanceFree(gbRebalance *rb);

int gbReachIndex(gbRebalance *rb, const char *addr);

bool gbRbCandidate(gbRebalance *rb, gbRbBlock *b, int idx);

void gbRebalancePlan(gbRebalance *rb);

bool gbRbMoves(gbRbBlock *b);

bool gbRbHasHost(gbRbBlock *b, const char *addr);

size_t gbRbValidHosts(gbRbBlock *b);


/* maintenance state of @addr on the volume, "" when active */
void blockHostStateGet(struct glfs *glfs, const char *addr, char *state,
                       size_t len);

/* NULL or "" removes the state (ACTIVE) */
int blockHostStateSet(struct glfs *glfs, const char *volume, const char *addr,
                      const char *state);

/* the maintenance state of @addr, NULL when it is active */
const char *blockHostInMaintenance(struct glfs *glfs, const char *addr,
                                   char *buf, size_t len);

/* gluster-blockd of @addr answers: 0 and tcmu-runner state, else -1 */
int blockHostServices(const char *volume, const char *addr, char *tcmu,
                      size_t tlen, char *err, size_t elen);


/* I/O through the LUN of a block on one host, see blockHostLunIo() */
typedef struct gbLunIo {
  char   name[255];             /* in */
  char   gbid[UUID_BUF_SIZE];   /* in */
  bool   reported;
  bool   enabled;               /* TPG of the host enabled */
  size_t cmds;                  /* SCSI commands, path checker included */
  size_t rmb;                   /* MB read */
  size_t wmb;                   /* MB written */
  char   sessions[1024];        /* initiators logged in, "-" if none */
} gbLunIo;

/* sample the LUN counters of @n blocks on @addr: 0, or -1 if it does not answer */
int blockHostLunIo(const char *volume, const char *addr, gbLunIo *io, size_t n,
                   char *err, size_t elen);

# endif /* _BLOCK_HA_H */
