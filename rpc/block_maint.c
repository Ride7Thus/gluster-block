/*
  Copyright (c) 2026 gluster-block contributors
  This file is part of gluster-block.

  This file is licensed to you under your choice of the GNU Lesser
  General Public License, version 3 or any later version (LGPLv3 or
  later), or the GNU General Public License, version 2 (GPLv2), in all
  cases as published by the Free Software Foundation.
*/

/*
 * 'gluster-block hosts <volume> <host> maintenance start [force]'
 * 'gluster-block hosts <volume> <host> maintenance stop [force] [rebalance]'
 *
 * start: check that every block of the host keeps another path, mark the
 *        host ENTERING, move the prio paths off it, wait until the
 *        initiators do no more I/O through its LUNs, disable the TPGs of
 *        the host and save the configuration (a reboot keeps them
 *        disabled), mark it MAINTENANCE. force skips the checks and the
 *        wait, after the user confirmed what it skips.
 * stop:  check the host answers and runs tcmu-runner, mark it LEAVING, put
 *        the current ALUA groups on it (prio paths moved meanwhile), enable
 *        its TPGs, clear the mark, optionally rebalance the volume.
 *
 * The state is kept per host of the volume in xattrs of block-meta/hosts.info.
 */

# ifndef _GNU_SOURCE
#  define _GNU_SOURCE
# endif

# include  <glob.h>

# include  "block_common.h"
# include  "block_ha.h"


# define  GB_CONFIGFS_ISCSI        "/sys/kernel/config/target/iscsi"
/*
 * A LUN is idle when, over the last GB_MAINT_IO_IDLE_SEC, nothing was read
 * or written through it and it got no more commands than the path checker
 * of multipath sends (TEST UNIT READY every 5-20 s, REPORT TARGET PORT
 * GROUPS now and then): GB_MAINT_IO_IDLE_CMDS per logged in initiator per
 * 10 s.
 */
# define  GB_MAINT_IO_IDLE_ENV     "GB_MAINT_IO_IDLE_SEC"
# define  GB_MAINT_IO_IDLE_DEF     10
# define  GB_MAINT_IO_WAIT_ENV     "GB_MAINT_IO_WAIT_SEC"
# define  GB_MAINT_IO_WAIT_DEF     120
# define  GB_MAINT_IO_IDLE_CMDS    3
# define  GB_MAINT_IO_POLL_SEC     2


/* ------------------------------------------------------------------------
 * Host state
 * ------------------------------------------------------------------------ */

void
blockHostStateGet(struct glfs *glfs, const char *addr, char *state, size_t len)
{
  char attr[512];
  ssize_t n;


  state[0] = '\0';
  snprintf(attr, sizeof(attr), "%s.%s", GB_HOSTS_ATTR_PREFIX, addr);
  n = glfs_getxattr(glfs, GB_HOSTS_FILE, attr, state, len - 1);
  if (n < 0) {
    if (errno != ENOENT && errno != ENODATA) {
      LOG("gfapi", GB_LOG_WARNING, "glfs_getxattr(%s, %s) failed[%s]",
          GB_HOSTS_FILE, attr, strerror(errno));
    }
    state[0] = '\0';
    return;
  }
  state[n] = '\0';
}


int
blockHostStateSet(struct glfs *glfs, const char *volume, const char *addr,
                  const char *state)
{
  struct glfs_fd *fd;
  char attr[512];


  snprintf(attr, sizeof(attr), "%s.%s", GB_HOSTS_ATTR_PREFIX, addr);

  if (!state || !state[0]) {
    if (glfs_removexattr(glfs, GB_HOSTS_FILE, attr) &&
        errno != ENODATA && errno != ENOENT) {
      LOG("gfapi", GB_LOG_ERROR, "glfs_removexattr(%s, %s) on volume %s "
          "failed[%s]", GB_HOSTS_FILE, attr, volume, strerror(errno));
      return -1;
    }
    LOG("mgmt", GB_LOG_INFO, "host %s of volume %s is %s", addr, volume,
        GB_HOST_STATE_ACTIVE);
    return 0;
  }

  fd = glfs_creat(glfs, GB_HOSTS_FILE, O_RDWR | O_SYNC, S_IRUSR | S_IWUSR);
  if (!fd) {
    LOG("gfapi", GB_LOG_ERROR, "glfs_creat(%s) on volume %s failed[%s]",
        GB_HOSTS_FILE, volume, strerror(errno));
    return -1;
  }
  glfs_close(fd);

  if (glfs_setxattr(glfs, GB_HOSTS_FILE, attr, state, strlen(state), 0)) {
    LOG("gfapi", GB_LOG_ERROR, "glfs_setxattr(%s, %s) on volume %s failed[%s]",
        GB_HOSTS_FILE, attr, volume, strerror(errno));
    return -1;
  }
  LOG("mgmt", GB_LOG_INFO, "host %s of volume %s is %s", addr, volume, state);

  return 0;
}


const char *
blockHostInMaintenance(struct glfs *glfs, const char *addr, char *buf,
                       size_t len)
{
  blockHostStateGet(glfs, addr, buf, len);
  return buf[0] ? buf : NULL;
}


/* ------------------------------------------------------------------------
 * Remote side: enable or disable the TPG of this host for the given blocks
 * ------------------------------------------------------------------------ */

static bool
gbTpgReadEnable(const char *tpgdir)
{
  char path[PATH_MAX];
  char buf[8] = {0};
  FILE *fp;


  snprintf(path, sizeof(path), "%s/enable", tpgdir);
  fp = fopen(path, "r");
  if (!fp) {
    return false;
  }
  if (!fgets(buf, sizeof(buf), fp)) {
    buf[0] = '\0';
  }
  fclose(fp);
  return buf[0] == '1';
}


/* the tpg directory of @host for target @gbid, NULL if none */
static char *
gbTpgOfHost(const char *gbid, const char *host, size_t *tpg)
{
  char *pattern = NULL;
  char *dir = NULL;
  glob_t g;
  size_t i;


  if (GB_ASPRINTF(&pattern, GB_CONFIGFS_ISCSI "/%s%s/tpgt_*",
                  GB_TGCLI_IQN_PREFIX, gbid) == -1) {
    return NULL;
  }
  if (!glob(pattern, GLOB_ONLYDIR, NULL, &g)) {
    for (i = 0; i < g.gl_pathc; i++) {
      char *num = strrchr(g.gl_pathv[i], '_');

      if (num && sscanf(num + 1, "%zu", tpg) == 1 &&
          blockConfigfsTpgHasPortal(g.gl_pathv[i], host)) {
        dir = strdup(g.gl_pathv[i]);
        break;
      }
    }
    globfree(&g);
  }
  GB_FREE(pattern);

  return dir;
}


static blockResponse *
block_host_tpg_1_svc_st(blockHostTpg *blk, struct svc_req *rqstp)
{
  blockResponse *reply = NULL;
  char *blocks = NULL;
  char *line;
  char *saveptr = NULL;
  char *cmds = NULL;
  char *out = NULL;
  char *exec = NULL;
  char **dirs = NULL;
  char (*names)[255] = NULL;
  size_t ndirs = 0;
  size_t i;
  bool failed = false;
  const char *verb = blk->enable ? "enable" : "disable";


  LOG("mgmt", GB_LOG_INFO, "host tpg request, volume=%s host=%s %s",
      blk->volume, blk->host, verb);

  if (GB_ALLOC(reply) < 0) {
    return NULL;
  }

  if (blk->blocks && GB_STRDUP(blocks, blk->blocks) < 0) {
    goto nomem;
  }
  for (line = blocks ? strtok_r(blocks, "\n", &saveptr) : NULL; line;
       line = strtok_r(NULL, "\n", &saveptr)) {
    char name[255] = {0};
    char gbid[UUID_BUF_SIZE] = {0};
    size_t tpg = 0;
    char *dir;

    if (sscanf(line, "%254s %37s", name, gbid) != 2) {
      continue;
    }
    dir = gbTpgOfHost(gbid, blk->host, &tpg);
    if (!dir) {
      gbJoin(&out, "", "BLOCK %s fail no TPG with a portal of %s\n", name,
             blk->host);
      failed = true;
      continue;
    }
    if (gbTpgReadEnable(dir) == blk->enable) {
      gbJoin(&out, "", "BLOCK %s ok already %sd\n", name, verb);
      free(dir);
      continue;
    }

    if (GB_REALLOC_N(dirs, ndirs + 1) < 0 ||
        GB_REALLOC_N(names, ndirs + 1) < 0) {
      free(dir);
      goto nomem;
    }
    dirs[ndirs] = dir;
    GB_STRCPYSTATIC(names[ndirs], name);
    ndirs++;
    if (gbJoin(&cmds, "", "%s/%s%s/tpg%zu %s\n" GB_TGCLI_GLFS_SAVE "\n",
               GB_TGCLI_ISCSI_PATH, GB_TGCLI_IQN_PREFIX, gbid, tpg, verb,
               name)) {
      goto nomem;
    }
  }

  if (cmds) {
    if (GB_ASPRINTF(&exec, "targetcli <<EOF > " DEVNULLPATH " 2>&1\n%sexit\nEOF",
                    cmds) == -1) {
      goto nomem;
    }
    LOG("mgmt", GB_LOG_DEBUG, "command: %s", exec);
    gbRunner(exec);

    /* configfs tells what happened, the targetcli messages vary */
    for (i = 0; i < ndirs; i++) {
      if (gbTpgReadEnable(dirs[i]) == blk->enable) {
        gbJoin(&out, "", "BLOCK %s ok %sd\n", names[i], verb);
      } else {
        gbJoin(&out, "", "BLOCK %s fail TPG not %sd\n", names[i], verb);
        failed = true;
      }
    }
  }

  reply->exit = failed ? EIO : 0;
  reply->out = out ? out : strdup("");
  out = NULL;
  goto done;

 nomem:
  reply->exit = ENOMEM;
  GB_ASPRINTF(&reply->out, "host tpg: out of memory");

 done:
  for (i = 0; i < ndirs; i++) {
    free(dirs[i]);
  }
  GB_FREE(dirs);
  GB_FREE(names);
  GB_FREE(blocks);
  GB_FREE(cmds);
  GB_FREE(exec);
  free(out);

  return reply;
}


bool_t
block_host_tpg_1_svc(blockHostTpg *blk, blockResponse *reply,
                     struct svc_req *rqstp)
{
  int ret;

  GB_RPC_CALL(host_tpg, blk, reply, rqstp, ret);
  return ret;
}


/* ------------------------------------------------------------------------
 * Cli side
 * ------------------------------------------------------------------------ */

typedef struct gbMaintReport {
  bool    already;
  char   *moved;        /* "block: from -> to\n" */
  char   *notmoved;     /* "block: reason\n" */
  size_t  ntpg;         /* TPGs enabled / disabled */
  size_t  ntpgtotal;
  char   *tpgfail;      /* "block: reason\n" */
  char   *nopath;       /* blocks without another path, space separated */
  char   *attention;    /* "block: state\n" */
  char   *rebalance;    /* "block: from -> to[: FAILED ...]\n" */
  char   *notes;        /* "text\n" */
  char   *busy;         /* "block: I/O ..., initiators ...\n" */
  char    state[32];    /* state of the host at the end */
} gbMaintReport;


static void
gbMaintReportFree(gbMaintReport *r)
{
  free(r->moved);
  free(r->notmoved);
  free(r->tpgfail);
  free(r->nopath);
  free(r->attention);
  free(r->rebalance);
  free(r->notes);
  free(r->busy);
}


static int
gbMaintEnvSec(const char *name, int def)
{
  const char *env = getenv(name);
  int sec;

  if (env && sscanf(env, "%d", &sec) == 1 && sec >= 0) {
    return sec;
  }
  return def;
}


static size_t
gbSessionCount(const char *sessions)
{
  size_t n = 1;

  if (!sessions[0] || !strcmp(sessions, "-")) {
    return 0;
  }
  for (; *sessions; sessions++) {
    if (*sessions == ',') {
      n++;
    }
  }
  return n;
}


typedef struct gbIoSample {
  time_t t;
  size_t cmds;
  size_t rmb;
  size_t wmb;
} gbIoSample;


/* the LUNs of the blocks of @h, for blockHostLunIo() */
static gbLunIo *
gbMaintLuns(gbRebalance *rb, gbReach *h, size_t *n)
{
  gbLunIo *io = NULL;
  size_t i;

  *n = 0;
  for (i = 0; i < rb->nblocks; i++) {
    if (!gbRbHasHost(&rb->blocks[i], h->addr)) {
      continue;
    }
    if (GB_REALLOC_N(io, *n + 1) < 0) {
      GB_FREE(io);
      *n = 0;
      return NULL;
    }
    memset(&io[*n], 0, sizeof(*io));
    GB_STRCPYSTATIC(io[*n].name, rb->blocks[i].name);
    GB_STRCPYSTATIC(io[*n].gbid, rb->blocks[i].info->gbid);
    (*n)++;
  }
  return io;
}


/*
 * I/O through the LUNs of @h between two samples @window seconds apart,
 * "block: ...\n" per LUN that did more than the path checker, NULL if none.
 * Used for the warning of force.
 */
static char *
gbMaintIoNow(blockHostMaintCli *blk, gbRebalance *rb, gbReach *h, int window)
{
  gbLunIo *a = NULL, *b = NULL;
  char err[256];
  char *busy = NULL;
  size_t n = 0, i;


  a = gbMaintLuns(rb, h, &n);
  if (!a || blockHostLunIo(blk->volume, h->addr, a, n, err, sizeof(err))) {
    goto out;
  }
  sleep(window);
  if (GB_ALLOC_N(b, n) < 0) {
    goto out;
  }
  memcpy(b, a, n * sizeof(*a));
  if (blockHostLunIo(blk->volume, h->addr, b, n, err, sizeof(err))) {
    goto out;
  }
  for (i = 0; i < n; i++) {
    size_t cmds = b[i].cmds - a[i].cmds;

    if (!b[i].reported || !b[i].enabled) {
      continue;
    }
    if (b[i].rmb != a[i].rmb || b[i].wmb != a[i].wmb ||
        cmds > GB_MAINT_IO_IDLE_CMDS) {
      gbJoin(&busy, "", "%s: %zu commands, %zu MB read, %zu MB written in "
             "%d s, initiators %s\n", b[i].name, cmds, b[i].rmb - a[i].rmb,
             b[i].wmb - a[i].wmb, window, b[i].sessions);
    }
  }

 out:
  GB_FREE(a);
  GB_FREE(b);
  return busy;
}


/*
 * Wait until the initiators do no I/O through the LUNs of @h any more, they
 * follow the moved prio paths at the next path check of multipath.
 * 0 when all are idle (or the host stopped answering), EBUSY on timeout
 * with the busy LUNs in r->busy.
 */
static int
gbMaintWaitIdle(blockHostMaintCli *blk, gbRebalance *rb, gbReach *h,
                gbMaintReport *r)
{
  int window = gbMaintEnvSec(GB_MAINT_IO_IDLE_ENV, GB_MAINT_IO_IDLE_DEF);
  int timeout = gbMaintEnvSec(GB_MAINT_IO_WAIT_ENV, GB_MAINT_IO_WAIT_DEF);
  size_t maxs = (size_t)(timeout + window) / GB_MAINT_IO_POLL_SEC + 4;
  gbIoSample *hist = NULL;      /* [lun * maxs + sample] */
  gbLunIo *io = NULL;
  char err[256];
  time_t start = time(NULL);
  size_t n = 0, ns = 0, i, s, nidle;
  int ret = 0;


  io = gbMaintLuns(rb, h, &n);
  if (!n) {
    return 0;
  }
  if (!io || GB_ALLOC_N(hist, n * maxs) < 0) {
    GB_FREE(io);
    return 0;     /* no memory to watch, fall back to the failover */
  }

  blockOpPhase("waiting for I/O to stop");
  blockOpStepCount("idle LUNs", n);

  while (1) {
    time_t now;

    if (blockHostLunIo(blk->volume, h->addr, io, n, err, sizeof(err))) {
      gbJoin(&r->notes, "", "host stopped answering while waiting for its "
             "I/O to stop (%s)\n", err);
      break;
    }
    now = time(NULL);
    if (ns < maxs) {
      for (i = 0; i < n; i++) {
        hist[i * maxs + ns] = (gbIoSample){now, io[i].cmds, io[i].rmb,
                                           io[i].wmb};
      }
      ns++;
    }

    nidle = 0;
    GB_FREE(r->busy);
    for (i = 0; i < n; i++) {
      gbIoSample *old = NULL;
      size_t allow, cmds;

      if (!io[i].reported || !io[i].enabled) {
        nidle++;            /* nothing can reach it */
        continue;
      }
      /* the newest sample at least @window old */
      for (s = 0; s < ns; s++) {
        if (now - hist[i * maxs + s].t >= window) {
          old = &hist[i * maxs + s];
        }
      }
      if (!old) {
        continue;           /* not watched long enough yet */
      }
      cmds = io[i].cmds - old->cmds;
      allow = GB_MAINT_IO_IDLE_CMDS * (gbSessionCount(io[i].sessions) ?
                                       gbSessionCount(io[i].sessions) : 1);
      allow = allow * (size_t)(window > 10 ? window : 10) / 10;
      if (io[i].rmb == old->rmb && io[i].wmb == old->wmb && cmds <= allow) {
        nidle++;
      } else {
        gbJoin(&r->busy, "", "%s: %zu commands, %zu MB read, %zu MB written "
               "in %lld s, initiators %s\n", io[i].name, cmds,
               io[i].rmb - old->rmb, io[i].wmb - old->wmb,
               (long long)(now - old->t), io[i].sessions);
      }
    }
    blockOpStepProgress(nidle);

    if (nidle == n) {
      GB_FREE(r->busy);
      break;
    }
    if (now - start >= timeout) {
      if (!r->busy) {
        gbJoin(&r->busy, "", "*: not watched long enough\n");
      }
      ret = EBUSY;
      break;
    }
    sleep(GB_MAINT_IO_POLL_SEC);
  }

  GB_FREE(io);
  GB_FREE(hist);
  return ret;
}


/* enable or disable the TPG of @host for all its blocks, 0 if all did */
static int
gbMaintTpg(gbRebalance *rb, char *volume, const char *host, bool enable,
           gbMaintReport *r)
{
  blockHostTpg req = {{0},};
  char *blocks = NULL;
  char *out = NULL;
  char *line, *save = NULL;
  bool rpc_sent = false;
  size_t i;
  int ret;


  for (i = 0; i < rb->nblocks; i++) {
    if (gbRbHasHost(&rb->blocks[i], host)) {
      gbJoin(&blocks, "", "%s %s\n", rb->blocks[i].name,
             rb->blocks[i].info->gbid);
      r->ntpgtotal++;
    }
  }
  if (!blocks) {
    return 0;
  }

  GB_STRCPYSTATIC(req.volume, volume);
  GB_STRCPYSTATIC(req.host, host);
  req.blocks = blocks;
  req.enable = enable;

  ret = glusterBlockCallRPC_1((char *)host, &req, HOST_TPG_SRV, &rpc_sent, &out);
  if (!out) {
    gbJoin(&r->tpgfail, "", "*: %s\n", rpc_sent ? "remote call failed" :
           "can not connect to gluster-blockd");
    free(blocks);
    return -1;
  }

  for (line = strtok_r(out, "\n", &save); line;
       line = strtok_r(NULL, "\n", &save)) {
    char name[255] = {0};
    char res[16] = {0};
    int off = 0;

    if (sscanf(line, "BLOCK %254s %15s %n", name, res, &off) < 2) {
      continue;
    }
    if (!strcmp(res, "ok")) {
      r->ntpg++;
    } else {
      gbJoin(&r->tpgfail, "", "%s: %s\n", name, off ? line + off : "failed");
    }
  }
  free(out);
  free(blocks);

  return (ret || r->tpgfail) ? -1 : 0;
}


/* metadata state of @addr in the block, "" if it is not a host of it */
static const char *
gbMetaHostStatus(MetaInfo *info, const char *addr)
{
  size_t i;

  for (i = 0; i < info->nhosts; i++) {
    if (!strcmp(info->list[i]->addr, addr)) {
      return info->list[i]->status;
    }
  }
  return "";
}


static int
gbMaintStart(struct glfs *glfs, blockHostMaintCli *blk, gbRebalance *rb,
             int hidx, gbMaintReport *r, char **errMsg)
{
  gbReach *h = &rb->reach[hidx];
  size_t i, nmoves = 0, k = 0;
  int j;


  if (!strcmp(h->state, GB_HOST_STATE_MAINTENANCE)) {
    r->already = true;
    GB_STRCPYSTATIC(r->state, h->state);
    return 0;
  }

  /* every block of the host keeps a path */
  for (i = 0; i < rb->nblocks; i++) {
    gbRbBlock *b = &rb->blocks[i];
    size_t others = 0;

    if (!gbRbHasHost(b, h->addr)) {
      continue;
    }
    for (j = 0; j < (int)rb->nreach; j++) {
      if (j != hidx && rb->reach[j].reachable && !rb->reach[j].state[0] &&
          gbRbHasHost(b, rb->reach[j].addr)) {
        others++;
      }
    }
    if (!others) {
      gbJoin(&r->nopath, " ", "%s", b->name);
    }
  }
  if (r->nopath && !blk->force) {
    GB_ASPRINTF(errMsg, "blocks would lose their last working path: %s; use "
                "'force' to take the host out anyway", r->nopath);
    return EBUSY;
  }
  if (!h->reachable && !blk->force) {
    GB_ASPRINTF(errMsg, "host %s does not answer (%s), its TPGs can not be "
                "disabled; use 'force' to mark it in maintenance anyway",
                h->addr, h->error);
    return EHOSTUNREACH;
  }

  /* force skips checks and the I/O wait: tell what that means first */
  if (blk->force && !blk->confirmed) {
    char *busy = h->reachable ? gbMaintIoNow(blk, rb, h, 2) : NULL;
    char *warn = NULL;

    gbJoin(&warn, "", "WARNING: 'force' takes %s out of service for volume %s "
           "without waiting for the initiators to stop their I/O on it:\n",
           h->addr, blk->volume);
    if (!h->reachable) {
      gbJoin(&warn, "", "  - the host does not answer (%s), its TPGs stay "
             "as they are\n", h->error);
    }
    if (r->nopath) {
      gbJoin(&warn, "", "  - these blocks lose their last working path, "
             "their initiators get I/O errors: %s\n", r->nopath);
    }
    if (busy) {
      char *line, *save = NULL;

      gbJoin(&warn, "", "  - LUNs with I/O right now, it fails over to the "
             "other paths (or errors if there are none):\n");
      for (line = strtok_r(busy, "\n", &save); line;
           line = strtok_r(NULL, "\n", &save)) {
        gbJoin(&warn, "", "      %s\n", line);
      }
      free(busy);
    }
    *errMsg = warn;
    return GB_CONFIRM_REQUIRED;
  }

  if (blockHostStateSet(glfs, blk->volume, h->addr, GB_HOST_STATE_ENTERING)) {
    GB_ASPRINTF(errMsg, "can not record the maintenance state of %s",
                h->addr);
    return EIO;
  }
  GB_STRCPYSTATIC(h->state, GB_HOST_STATE_ENTERING);
  GB_STRCPYSTATIC(r->state, GB_HOST_STATE_ENTERING);

  /* prio paths off the host, to the host of the block with the fewest */
  for (i = 0; i < rb->nblocks; i++) {
    gbRbBlock *b = &rb->blocks[i];
    int from = gbReachIndex(rb, b->info->prio_path);

    if (gbRbCandidate(rb, b, from)) {
      rb->load[from]++;
    }
    if (b->info->prio_path[0] && !strcmp(b->info->prio_path, h->addr) &&
        gbRbHasHost(b, h->addr)) {
      nmoves++;
    }
  }

  blockOpPhase("moving prio paths");
  blockOpStepCount("moving prio paths", nmoves);
  gbAsyncDetach();

  for (i = 0; i < rb->nblocks && nmoves; i++) {
    gbRbBlock *b = &rb->blocks[i];
    gbPrioResult pr = {0,};
    char *msg = NULL;
    int best = -1;

    /* not exported by the host any more (replaced, being deleted) */
    if (!b->info->prio_path[0] || strcmp(b->info->prio_path, h->addr) ||
        !gbRbHasHost(b, h->addr)) {
      continue;
    }
    for (j = 0; j < (int)rb->nreach; j++) {
      if (gbRbCandidate(rb, b, j) &&
          (best < 0 || rb->load[j] < rb->load[best])) {
        best = j;
      }
    }
    if (best < 0) {
      gbJoin(&r->notmoved, "", "%s: no other host of the block answers\n",
             b->name);
    } else if (gbPrioMove(glfs, blk->volume, b->name, b->info,
                          rb->reach[best].addr, true, rb->reach, rb->nreach,
                          NULL, &pr, &msg)) {
      gbJoin(&r->notmoved, "", "%s: %s\n", b->name,
             pr.failed ? pr.failed : (msg ? msg : "failed"));
    } else {
      rb->load[best]++;
      gbJoin(&r->moved, "", "%s: %s -> %s\n", b->name, h->addr,
             rb->reach[best].addr);
    }
    gbPrioResultFree(&pr);
    GB_FREE(msg);
    blockOpStepProgress(++k);
  }

  /* multipath of the initiators follows the ALUA states at its next check */
  if (h->reachable && !blk->force &&
      gbMaintWaitIdle(blk, rb, h, r) == EBUSY) {
    GB_ASPRINTF(errMsg, "the initiators still do I/O through the LUNs of %s "
                "after %d s, its TPGs stay enabled and the host stays %s; "
                "stop that I/O (or check multipath on the initiators) and run "
                "maintenance start again, or use 'force'", h->addr,
                gbMaintEnvSec(GB_MAINT_IO_WAIT_ENV, GB_MAINT_IO_WAIT_DEF),
                GB_HOST_STATE_ENTERING);
    return EBUSY;
  }
  if (blk->force) {
    gbJoin(&r->notes, "", "force: TPGs disabled without waiting for the I/O "
           "to stop\n");
  }

  if (h->reachable) {
    blockOpPhase("disabling TPGs");
    if (gbMaintTpg(rb, blk->volume, h->addr, false, r)) {
      GB_ASPRINTF(errMsg, "disabling the TPGs of %s failed, the host stays "
                  "%s; run maintenance start again", h->addr,
                  GB_HOST_STATE_ENTERING);
      return EIO;
    }
  } else {
    gbJoin(&r->notes, "", "host does not answer, its TPGs were not disabled; "
           "they are restored when it boots, start maintenance again once it "
           "is back\n");
  }

  if (blockHostStateSet(glfs, blk->volume, h->addr,
                        GB_HOST_STATE_MAINTENANCE)) {
    GB_ASPRINTF(errMsg, "can not record the maintenance state of %s",
                h->addr);
    return EIO;
  }
  GB_STRCPYSTATIC(h->state, GB_HOST_STATE_MAINTENANCE);
  GB_STRCPYSTATIC(r->state, GB_HOST_STATE_MAINTENANCE);

  return 0;
}


static int
gbMaintStop(struct glfs *glfs, blockHostMaintCli *blk, gbRebalance *rb,
            int hidx, gbMaintReport *r, char **errMsg)
{
  gbReach *h = &rb->reach[hidx];
  blockModifyPrio mobj = {{0},};
  char tcmu[32];
  char err[256];
  size_t i, k = 0, nmoves = 0;


  if (!h->state[0]) {
    GB_ASPRINTF(errMsg, "host %s is not in maintenance", h->addr);
    return EINVAL;
  }
  if (!h->reachable) {
    GB_ASPRINTF(errMsg, "host %s does not answer (%s)", h->addr, h->error);
    return EHOSTUNREACH;
  }
  if (blockHostServices(blk->volume, h->addr, tcmu, sizeof(tcmu), err,
                        sizeof(err))) {
    GB_ASPRINTF(errMsg, "host %s does not answer (%s)", h->addr, err);
    return EHOSTUNREACH;
  }
  if (strcmp(tcmu, "running") && !blk->force) {
    GB_ASPRINTF(errMsg, "tcmu-runner is %s on host %s; start it or use "
                "'force'", tcmu, h->addr);
    return EBUSY;
  }

  if (blockHostStateSet(glfs, blk->volume, h->addr, GB_HOST_STATE_LEAVING)) {
    GB_ASPRINTF(errMsg, "can not record the maintenance state of %s",
                h->addr);
    return EIO;
  }
  GB_STRCPYSTATIC(h->state, GB_HOST_STATE_LEAVING);
  GB_STRCPYSTATIC(r->state, GB_HOST_STATE_LEAVING);
  gbAsyncDetach();

  /* the prio paths may have moved while the host was out */
  blockOpPhase("syncing ALUA groups");
  for (i = 0; i < rb->nblocks; i++) {
    gbRbBlock *b = &rb->blocks[i];
    char *out = NULL;
    bool rpc_sent = false;

    if (!gbRbHasHost(b, h->addr)) {
      continue;
    }
    switch (blockMetaStatusEnumParse(gbMetaHostStatus(b->info, h->addr))) {
    case GB_CONFIG_FAIL:
    case GB_CLEANUP_FAIL:
    case GB_AUTH_ENFORCE_FAIL:
    case GB_AUTH_CLEAR_ENFORCE_FAIL:
    case GB_RP_FAIL:
    case GB_RS_FAIL:
      gbJoin(&r->attention, "", "%s: %s, run the failed operation again\n",
             b->name, gbMetaHostStatus(b->info, h->addr));
      break;
    }
    if (!b->info->prio_path[0]) {
      continue;
    }
    GB_STRCPYSTATIC(mobj.volume, blk->volume);
    GB_STRCPYSTATIC(mobj.block_name, b->name);
    GB_STRCPYSTATIC(mobj.gbid, b->info->gbid);
    GB_STRCPYSTATIC(mobj.prio_path, b->info->prio_path);
    if (glusterBlockCallRPC_1(h->addr, &mobj, MODIFY_PRIO_SRV, &rpc_sent,
                              &out)) {
      gbJoin(&r->attention, "", "%s: ALUA groups not updated (%s)\n", b->name,
             out ? out : "remote call failed");
    }
    GB_FREE(out);
  }

  blockOpPhase("enabling TPGs");
  if (gbMaintTpg(rb, blk->volume, h->addr, true, r)) {
    GB_ASPRINTF(errMsg, "enabling the TPGs of %s failed, the host stays %s; "
                "run maintenance stop again", h->addr, GB_HOST_STATE_LEAVING);
    return EIO;
  }

  if (blockHostStateSet(glfs, blk->volume, h->addr, NULL)) {
    GB_ASPRINTF(errMsg, "can not clear the maintenance state of %s", h->addr);
    return EIO;
  }
  h->state[0] = '\0';
  GB_STRCPYSTATIC(r->state, GB_HOST_STATE_ACTIVE);

  if (!blk->rebalance) {
    return 0;
  }

  blockOpPhase("rebalancing prio paths");
  memset(rb->load, 0, rb->nreach * sizeof(*rb->load));
  gbRebalancePlan(rb);
  for (i = 0; i < rb->nblocks; i++) {
    if (gbRbMoves(&rb->blocks[i])) {
      nmoves++;
    }
  }
  blockOpStepCount("rebalancing prio paths", nmoves);
  for (i = 0; i < rb->nblocks; i++) {
    gbRbBlock *b = &rb->blocks[i];
    gbPrioResult pr = {0,};
    char *msg = NULL;

    if (!gbRbMoves(b)) {
      continue;
    }
    if (gbPrioMove(glfs, blk->volume, b->name, b->info,
                   rb->reach[b->cur].addr, true, rb->reach, rb->nreach, NULL,
                   &pr, &msg)) {
      gbJoin(&r->rebalance, "", "%s: %s -> %s: FAILED (%s)\n", b->name,
             b->info->prio_path, rb->reach[b->cur].addr,
             pr.failed ? pr.failed : (msg ? msg : "failed"));
    } else {
      gbJoin(&r->rebalance, "", "%s: %s -> %s\n", b->name, b->info->prio_path,
             rb->reach[b->cur].addr);
    }
    gbPrioResultFree(&pr);
    GB_FREE(msg);
    blockOpStepProgress(++k);
  }

  return 0;
}


static void
gbMaintLines(char **out, const char *title, const char *lines)
{
  char *copy, *line, *save = NULL;

  if (!lines) {
    return;
  }
  gbJoin(out, "", "%s:\n", title);
  copy = strdup(lines);
  for (line = copy ? strtok_r(copy, "\n", &save) : NULL; line;
       line = strtok_r(NULL, "\n", &save)) {
    gbJoin(out, "", "  %s\n", line);
  }
  free(copy);
}


static void
gbMaintJsonLines(json_object *jo, const char *key, const char *lines)
{
  json_object *ja;
  char *copy, *line, *save = NULL;

  if (!lines) {
    return;
  }
  ja = json_object_new_array();
  copy = strdup(lines);
  for (line = copy ? strtok_r(copy, "\n", &save) : NULL; line;
       line = strtok_r(NULL, "\n", &save)) {
    json_object_array_add(ja, GB_JSON_OBJ_TO_STR(line));
  }
  free(copy);
  json_object_object_add(jo, key, ja);
}


static void
gbMaintFormat(blockHostMaintCli *blk, gbMaintReport *r, int errCode,
              char *errMsg, bool acted, blockResponse *reply)
{
  const char *what = blk->start ? "start" : "stop";
  const char *tpgword = blk->start ? "DISABLED" : "ENABLED";
  char tpgs[64];
  char *out = NULL;
  json_object *jo;


  if (errCode && !acted) {
    blockFormatErrorResponse(HOST_MAINT_SRV, blk->json_resp, errCode,
                             errMsg ? errMsg : GB_DEFAULT_ERRMSG, reply);
    return;
  }
  reply->exit = errCode;
  snprintf(tpgs, sizeof(tpgs), "%zu of %zu", r->ntpg, r->ntpgtotal);

  if (blk->json_resp) {
    jo = json_object_new_object();
    json_object_object_add(jo, "HOST", GB_JSON_OBJ_TO_STR(blk->host));
    json_object_object_add(jo, "VOLUME", GB_JSON_OBJ_TO_STR(blk->volume));
    json_object_object_add(jo, "MAINTENANCE", GB_JSON_OBJ_TO_STR(what));
    json_object_object_add(jo, "ALREADY", json_object_new_boolean(r->already));
    gbMaintJsonLines(jo, "PRIO PATHS MOVED", r->moved);
    gbMaintJsonLines(jo, "PRIO PATHS NOT MOVED", r->notmoved);
    if (r->ntpgtotal) {
      json_object_object_add(jo, blk->start ? "TPGS DISABLED" : "TPGS ENABLED",
                             json_object_new_string(tpgs));
    }
    gbMaintJsonLines(jo, "LUNS WITH I/O", r->busy);
    gbMaintJsonLines(jo, "TPG FAILURES", r->tpgfail);
    if (r->nopath) {
      blockStr2arrayAddToJsonObj(jo, r->nopath, "WITHOUT OTHER PATH");
    }
    gbMaintJsonLines(jo, "NEEDS ATTENTION", r->attention);
    gbMaintJsonLines(jo, "REBALANCE", r->rebalance);
    gbMaintJsonLines(jo, "NOTES", r->notes);
    json_object_object_add(jo, "STATE", json_object_new_string(r->state));
    if (errMsg) {
      json_object_object_add(jo, "errMsg", GB_JSON_OBJ_TO_STR(errMsg));
    }
    json_object_object_add(jo, "RESULT",
                           GB_JSON_OBJ_TO_STR(errCode ? "FAIL" : "SUCCESS"));
    GB_ASPRINTF(&reply->out, "%s\n", json_object_to_json_string_ext(jo,
                                    mapJsonFlagToJsonCstring(blk->json_resp)));
    json_object_put(jo);
    return;
  }

  gbJoin(&out, "", "HOST: %s\nVOLUME: %s\nMAINTENANCE: %s%s\n", blk->host,
         blk->volume, what, r->already ? " (already in maintenance)" : "");
  gbMaintLines(&out, "PRIO PATHS MOVED", r->moved);
  gbMaintLines(&out, "PRIO PATHS NOT MOVED", r->notmoved);
  if (r->ntpgtotal) {
    gbJoin(&out, "", "TPGS %s: %s\n", tpgword, tpgs);
  }
  gbMaintLines(&out, "LUNS WITH I/O", r->busy);
  gbMaintLines(&out, "TPG FAILURES", r->tpgfail);
  if (r->nopath) {
    gbJoin(&out, "", "WITHOUT OTHER PATH: %s\n", r->nopath);
  }
  gbMaintLines(&out, "NEEDS ATTENTION", r->attention);
  gbMaintLines(&out, "REBALANCE", r->rebalance);
  gbMaintLines(&out, "NOTES", r->notes);
  gbJoin(&out, "", "STATE: %s\n", r->state);
  if (errMsg) {
    gbJoin(&out, "", "%s\n", errMsg);
  }
  gbJoin(&out, "", "RESULT: %s\n", errCode ? "FAIL" : "SUCCESS");
  reply->out = out;
}


static blockResponse *
blockHostMaintRun(blockHostMaintCli *blk)
{
  blockResponse *reply = NULL;
  struct glfs *glfs = NULL;
  struct glfs_fd *lkfd = NULL;
  blockServerDefPtr list = NULL;
  gbRebalance rb = {0,};
  gbMaintReport r = {0,};
  bool privGlfs = false;
  bool acted = false;
  char initial[32] = "";
  size_t i;
  int hidx;
  int errCode = -1;
  char *errMsg = NULL;


  LOG("mgmt", GB_LOG_INFO, "host maintenance %s cli request, volume=%s "
      "host=%s force=%d rebalance=%d", blk->start ? "start" : "stop",
      blk->volume, blk->host, blk->force, blk->rebalance);

  if (GB_ALLOC(reply) < 0) {
    return NULL;
  }

  errCode = 0;
  glfs = blockVolumeGet(blk->volume, &errCode, &errMsg, &privGlfs);
  if (!glfs) {
    goto initfail;
  }
  lkfd = glusterBlockOpenMetaLockFile(glfs, blk->volume, &errCode, &errMsg);
  if (!lkfd) {
    goto initfail;
  }

  GB_METALOCK_OR_GOTO(lkfd, blk->volume, errCode, errMsg, initfail);
  LOG("cmdlog", GB_LOG_INFO, "%s", blk->cmd);
  blockOpBegin(lkfd, blk->start ? "maintenance-start" : "maintenance-stop",
               blk->host, NULL, 0);

  if (gbRebalanceRead(glfs, blk->volume, &rb, true, &errCode, &errMsg)) {
    goto out;
  }
  hidx = gbReachIndex(&rb, blk->host);
  if (hidx < 0) {
    errCode = ENOENT;
    GB_ASPRINTF(&errMsg, "host %s does not export block devices of volume %s",
                blk->host, blk->volume);
    goto out;
  }
  GB_STRCPYSTATIC(r.state, rb.reach[hidx].state[0] ? rb.reach[hidx].state :
                  GB_HOST_STATE_ACTIVE);
  GB_STRCPYSTATIC(initial, r.state);
  if (GB_ALLOC_N(rb.load, rb.nreach) < 0) {
    errCode = ENOMEM;
    goto out;
  }

  blockOpPhase("probing hosts");
  gbRebalanceProbe(&rb);

  /* the hosts that answer must know the requests */
  if (GB_ALLOC(list) < 0 || GB_ALLOC_N(list->hosts, rb.nreach) < 0) {
    errCode = ENOMEM;
    goto out;
  }
  for (i = 0; i < rb.nreach; i++) {
    if (rb.reach[i].reachable &&
        GB_STRDUP(list->hosts[list->nhosts++], rb.reach[i].addr) < 0) {
      errCode = ENOMEM;
      goto out;
    }
  }
  errCode = glusterBlockCheckCapabilities(blk, HOST_MAINT_SRV, list, NULL,
                                          &errMsg);
  if (errCode) {
    goto out;
  }

  if (blk->start) {
    errCode = gbMaintStart(glfs, blk, &rb, hidx, &r, &errMsg);
  } else {
    errCode = gbMaintStop(glfs, blk, &rb, hidx, &r, &errMsg);
  }
  /* something was changed: report it even on failure */
  acted = !errCode || strcmp(r.state, initial) || r.moved || r.ntpg;

 out:
  GB_METAUNLOCK(lkfd, blk->volume, errCode, errMsg);

 initfail:
  LOG("mgmt", ((!!errCode) ? GB_LOG_ERROR : GB_LOG_INFO),
      "host maintenance %s cli return %s, volume=%s host=%s",
      blk->start ? "start" : "stop", errCode ? "failure" : "success",
      blk->volume, blk->host);
  if (lkfd && glfs_close(lkfd) != 0) {
    LOG("mgmt", GB_LOG_ERROR, "glfs_close(%s): on volume %s failed[%s]",
        GB_TXLOCKFILE, blk->volume, strerror(errno));
  }
  if (errCode < 0) {
    errCode = GB_DEFAULT_ERRCODE;
  }

  gbMaintFormat(blk, &r, errCode, errMsg, acted, reply);
  LOG("cmdlog", ((!!errCode) ? GB_LOG_ERROR : GB_LOG_INFO), "%s",
      reply->out ? reply->out : "*Nil*");

  if (glfs) {
    blockVolumePut(glfs, privGlfs);
  }
  blockServerDefFree(list);
  gbRebalanceFree(&rb);
  gbMaintReportFree(&r);
  GB_FREE(errMsg);

  return reply;
}


static void
blockHostMaintCliFree(void *data)
{
  blockHostMaintCli *blk = data;

  if (!blk) {
    return;
  }
  GB_FREE(blk->cmd);
  GB_FREE(blk);
}


static blockResponse *
blockHostMaintRunAsync(void *data)
{
  return blockHostMaintRun(data);
}


static blockResponse *
block_host_maint_cli_1_svc_st(blockHostMaintCli *blk, struct svc_req *rqstp)
{
  blockHostMaintCli *copy = NULL;

  if (!blk->async) {
    return blockHostMaintRun(blk);
  }

  /* the RPC arguments are freed once we return, the worker needs a copy */
  if (GB_ALLOC(copy) < 0) {
    return blockHostMaintRun(blk);
  }
  memcpy(copy, blk, sizeof(*copy));
  copy->cmd = NULL;
  if (GB_STRDUP(copy->cmd, blk->cmd ? blk->cmd : "") < 0) {
    blockHostMaintCliFree(copy);
    return blockHostMaintRun(blk);
  }

  return gbAsyncRun(blk->start ? "maintenance-start" : "maintenance-stop",
                    blk->volume, blk->host, blk->json_resp,
                    blockHostMaintRunAsync, copy, blockHostMaintCliFree);
}


bool_t
block_host_maint_cli_1_svc(blockHostMaintCli *blk, blockResponse *reply,
                           struct svc_req *rqstp)
{
  int ret;

  GB_RPC_CALL(host_maint_cli, blk, reply, rqstp, ret);
  return ret;
}
