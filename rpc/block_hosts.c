/*
  Copyright (c) 2026 gluster-block contributors
  This file is part of gluster-block.

  This file is licensed to you under your choice of the GNU Lesser
  General Public License, version 3 or any later version (LGPLv3 or
  later), or the GNU General Public License, version 2 (GPLv2), in all
  cases as published by the Free Software Foundation.
*/

/*
 * 'gluster-block hosts <volume> [<host>]': the state of the hosts that
 * export the block devices of a block hosting volume.
 *
 * The node that gets the cli request reads the metadata of all blocks of the
 * volume, groups them by host and asks every host in parallel
 * (BLOCK_HOST_STATUS) what it really exports: gluster-blockd answers at all,
 * tcmu-runner runs, the backstore of each block is loaded, the TPG of the
 * host is enabled and which ALUA group its LUN is in. The host reads it from
 * configfs, targetcli would take seconds per block.
 */

# ifndef _GNU_SOURCE
#  define _GNU_SOURCE     /* vasprintf */
# endif

# include  <glob.h>
# include  <poll.h>
# include  <fcntl.h>
# include  <stdarg.h>
# include  <sys/socket.h>
# include  <netinet/in.h>

# include  "config.h"
# include  "block_common.h"
# include  "block_ha.h"


# define  GB_CONFIGFS_CORE       "/sys/kernel/config/target/core"
# define  GB_CONFIGFS_ISCSI      "/sys/kernel/config/target/iscsi"
# define  GB_ALUA_GP_PREFIX      "glfs_tg_pt_gp_"

# define  GB_HOSTS_CONNECT_MS    3000   /* a dead host must not hang the cli */
# define  GB_HOSTS_CALL_SEC      60

# define  GB_TCMU_RUNNING_CHECK  "pgrep -x tcmu-runner > " DEVNULLPATH
# define  GB_TARGET_ACTIVE_CHECK "systemctl is-active --quiet gluster-block-target.service"


typedef enum gbHostDaemon {
  GB_HOST_UNKNOWN = 0,
  GB_HOST_UP,
  GB_HOST_UNREACHABLE,
  GB_HOST_OLD_VERSION,  /* gluster-blockd without BLOCK_HOST_STATUS */
  GB_HOST_ERROR,
} gbHostDaemon;

static const char *const gbHostDaemonLookup[] = {
  [GB_HOST_UNKNOWN]     = "unknown",
  [GB_HOST_UP]          = "up",
  [GB_HOST_UNREACHABLE] = "unreachable",
  [GB_HOST_OLD_VERSION] = "old-version",
  [GB_HOST_ERROR]       = "error",
};


typedef struct gbHostBlock {
  char   name[255];
  char   gbid[UUID_BUF_SIZE];
  char   meta[32];       /* status of the host in the block metadata */
  bool   exported;       /* the metadata says the host exports it */
  bool   prio;           /* the host is the prio path of the block */

  bool   reported;       /* the host answered for this block */
  bool   backstore;      /* tcmu backstore loaded */
  size_t tpg;            /* tpg of the host, 0 if there is none */
  bool   enabled;        /* that tpg is enabled */
  char   alua[64];       /* alua group of its lun, "-" without ALUA */
  size_t cmds;           /* SCSI commands received through that lun */
  size_t rmb;            /* MB read through it */
  size_t wmb;            /* MB written through it */
  char   sessions[1024]; /* initiators logged in to the tpg, "-" if none */
} gbHostBlock;


typedef struct gbHost {
  char         addr[255];
  const char  *volume;
  char         state[32];      /* maintenance state, "" when active */
  size_t       nblocks;
  gbHostBlock *blocks;

  gbHostDaemon daemon;
  char         version[64];
  char         tcmu[16];
  char         target[16];
  char         error[256];

  size_t       nexported;
  size_t       nloaded;
  size_t       nenabled;
  size_t       nprio;
  size_t       nproblems;

  bool         wantCaps;       /* also ask the capabilities (BLOCK_VERSION) */
  bool         capsKnown;
  char       **caps;           /* names of the capabilities it supports */
  size_t       ncaps;
} gbHost;


typedef struct gbHosts {
  const char *volume;
  size_t  nhosts;
  gbHost *hosts;
} gbHosts;


static int
gbAppend(char **buf, const char *fmt, ...)
{
  va_list ap;
  char *add = NULL;
  char *joined = NULL;
  int ret;


  va_start(ap, fmt);
  ret = vasprintf(&add, fmt, ap);
  va_end(ap);
  if (ret < 0) {
    return -1;
  }

  if (!*buf) {
    *buf = add;
    return 0;
  }

  ret = asprintf(&joined, "%s%s", *buf, add);
  free(add);
  if (ret < 0) {
    return -1;
  }
  free(*buf);
  *buf = joined;

  return 0;
}


/* ------------------------------------------------------------------------
 * Remote side: what does this node export for the given blocks
 * ------------------------------------------------------------------------ */

static bool
gbConfigfsReadBool(const char *path)
{
  char buf[16] = {0};
  int fd;
  ssize_t n;


  fd = open(path, O_RDONLY);
  if (fd < 0) {
    return false;
  }
  n = read(fd, buf, sizeof(buf) - 1);
  close(fd);

  return n > 0 && buf[0] == '1';
}


static void
gbConfigfsReadString(const char *path, char *out, size_t len)
{
  int fd;
  ssize_t n;
  char *nl;


  snprintf(out, len, "-");
  fd = open(path, O_RDONLY);
  if (fd < 0) {
    return;
  }
  n = read(fd, out, len - 1);
  close(fd);
  if (n <= 0) {
    snprintf(out, len, "-");
    return;
  }
  out[n] = '\0';
  if ((nl = strchr(out, '\n'))) {
    *nl = '\0';
  }
  if (!out[0]) {
    snprintf(out, len, "-");
  }
}


static size_t
gbConfigfsReadSize(const char *dir, const char *file)
{
  char path[PATH_MAX];
  char buf[32] = {0};
  size_t val = 0;
  int fd;


  snprintf(path, sizeof(path), "%s/%s", dir, file);
  fd = open(path, O_RDONLY);
  if (fd < 0) {
    return 0;
  }
  if (read(fd, buf, sizeof(buf) - 1) > 0) {
    sscanf(buf, "%zu", &val);
  }
  close(fd);

  return val;
}


/* initiators logged in to the tpg, joined by ',', "-" if none */
static void
gbConfigfsReadSessions(const char *tpgdir, char *out, size_t len)
{
  char path[PATH_MAX];
  char buf[4096] = {0};
  ssize_t n;
  size_t o = 0;
  char *line, *save = NULL;
  int fd;


  snprintf(out, len, "-");
  snprintf(path, sizeof(path), "%s/dynamic_sessions", tpgdir);
  fd = open(path, O_RDONLY);
  if (fd < 0) {
    return;
  }
  n = read(fd, buf, sizeof(buf) - 1);
  close(fd);
  if (n <= 0) {
    return;
  }
  buf[n] = '\0';

  for (line = strtok_r(buf, "\n ", &save); line;
       line = strtok_r(NULL, "\n ", &save)) {
    int w = snprintf(out + o, len - o, "%s%s", o ? "," : "", line);

    if (w < 0 || (size_t)w >= len - o) {
      break;
    }
    o += w;
  }
  if (!o) {
    snprintf(out, len, "-");
  }
}


/* is there a network portal of 'host' in the tpg directory */
bool
blockConfigfsTpgHasPortal(const char *tpgdir, const char *host)
{
  char *pattern = NULL;
  glob_t g;
  bool found = false;


  /* np/<ip>:<port>, IPv6 as np/[<ip>]:<port> */
  if (GB_ASPRINTF(&pattern, "%s/np/%s:*", tpgdir, host) == -1) {
    return false;
  }
  if (!glob(pattern, 0, NULL, &g)) {
    found = g.gl_pathc > 0;
    globfree(&g);
  }
  GB_FREE(pattern);
  if (found) {
    return true;
  }

  if (GB_ASPRINTF(&pattern, "%s/np/[[]%s[]]:*", tpgdir, host) == -1) {
    return false;
  }
  if (!glob(pattern, 0, NULL, &g)) {
    found = g.gl_pathc > 0;
    globfree(&g);
  }
  GB_FREE(pattern);

  return found;
}


static int
gbHostStatusBlock(const char *host, const char *name, const char *gbid,
                  char **out)
{
  char *pattern = NULL;
  char *path = NULL;
  char *tpgdir = NULL;
  char alua[64] = "-";
  char sessions[1024] = "-";
  char *stats = NULL;
  size_t cmds = 0, rmb = 0, wmb = 0;
  glob_t g;
  size_t i;
  size_t tpg = 0;
  size_t ntpg_enabled = 0;
  size_t tpg_enabled = 0;
  bool backstore = false;
  bool enabled = false;
  int ret = -1;


  /* the tcmu backstore: core/user_<N>/<block_name> */
  if (GB_ASPRINTF(&pattern, GB_CONFIGFS_CORE "/user_*/%s", name) == -1) {
    goto out;
  }
  if (!glob(pattern, GLOB_ONLYDIR, NULL, &g)) {
    backstore = g.gl_pathc > 0;
    globfree(&g);
  }
  GB_FREE(pattern);

  /*
   * Every node has all tpgs of the target, one per host, and enables only
   * its own one. Find it by the portal of the host, fall back to the only
   * enabled one when the portal is written differently (name vs address).
   */
  if (GB_ASPRINTF(&pattern, GB_CONFIGFS_ISCSI "/%s%s/tpgt_*",
                  GB_TGCLI_IQN_PREFIX, gbid) == -1) {
    goto out;
  }
  if (!glob(pattern, GLOB_ONLYDIR, NULL, &g)) {
    for (i = 0; i < g.gl_pathc; i++) {
      size_t n = 0;
      char *num = strrchr(g.gl_pathv[i], '_');

      if (!num || sscanf(num + 1, "%zu", &n) != 1) {
        continue;
      }

      GB_FREE(path);
      if (GB_ASPRINTF(&path, "%s/enable", g.gl_pathv[i]) == -1) {
        globfree(&g);
        goto out;
      }
      if (gbConfigfsReadBool(path)) {
        ntpg_enabled++;
        tpg_enabled = n;
      }

      if (!tpg && blockConfigfsTpgHasPortal(g.gl_pathv[i], host)) {
        tpg = n;
        GB_FREE(tpgdir);
        if (GB_STRDUP(tpgdir, g.gl_pathv[i]) < 0) {
          globfree(&g);
          goto out;
        }
      }
    }
    if (!tpg && ntpg_enabled == 1) {
      tpg = tpg_enabled;
      for (i = 0; i < g.gl_pathc; i++) {
        char *num = strrchr(g.gl_pathv[i], '_');
        size_t n = 0;

        if (num && sscanf(num + 1, "%zu", &n) == 1 && n == tpg) {
          GB_FREE(tpgdir);
          if (GB_STRDUP(tpgdir, g.gl_pathv[i]) < 0) {
            globfree(&g);
            goto out;
          }
          break;
        }
      }
    }
    globfree(&g);
  }

  if (tpgdir) {
    GB_FREE(path);
    if (GB_ASPRINTF(&path, "%s/enable", tpgdir) == -1) {
      goto out;
    }
    enabled = gbConfigfsReadBool(path);

    GB_FREE(path);
    if (GB_ASPRINTF(&path, "%s/lun/lun_0/alua_tg_pt_gp", tpgdir) == -1) {
      goto out;
    }
    gbConfigfsReadString(path, alua, sizeof(alua));
    if (!strncmp(alua, GB_ALUA_GP_PREFIX, strlen(GB_ALUA_GP_PREFIX))) {
      memmove(alua, alua + strlen(GB_ALUA_GP_PREFIX),
              strlen(alua + strlen(GB_ALUA_GP_PREFIX)) + 1);
    } else if (!strcmp(alua, "default_tg_pt_gp")) {
      snprintf(alua, sizeof(alua), "-");
    }

    /* what the initiators do through this path: commands (also the path
     * checker's TEST UNIT READY) and MB moved, and who is logged in */
    if (GB_ASPRINTF(&stats, "%s/lun/lun_0/statistics/scsi_tgt_port",
                    tpgdir) == -1) {
      goto out;
    }
    cmds = gbConfigfsReadSize(stats, "in_cmds");
    rmb = gbConfigfsReadSize(stats, "read_mbytes");
    wmb = gbConfigfsReadSize(stats, "write_mbytes");
    gbConfigfsReadSessions(tpgdir, sessions, sizeof(sessions));
  }

  if (gbAppend(out, "BLOCK %s backstore=%d tpg=%zu enabled=%d alua=%s "
               "cmds=%zu rmb=%zu wmb=%zu sessions=%s\n",
               gbid, backstore, tpg, enabled, alua, cmds, rmb, wmb,
               sessions) == -1) {
    goto out;
  }
  ret = 0;

 out:
  GB_FREE(pattern);
  GB_FREE(path);
  GB_FREE(tpgdir);
  GB_FREE(stats);
  return ret;
}


static blockResponse *
block_host_status_1_svc_st(blockHostStatus *blk, struct svc_req *rqstp)
{
  blockResponse *reply = NULL;
  char *blocks = NULL;
  char *line;
  char *saveptr = NULL;
  char *out = NULL;
  int ret;


  LOG("mgmt", GB_LOG_DEBUG, "host status request, volume=%s host=%s",
      blk->volume, blk->host);

  if (GB_ALLOC(reply) < 0) {
    return NULL;
  }

  ret = gbRunner(GB_TCMU_RUNNING_CHECK);
  if (gbAppend(&out, "VERSION %s\nTCMU-RUNNER %s\n", PACKAGE_VERSION,
               ret == 0 ? "running" : (ret == 1 ? "stopped" : "unknown")) == -1) {
    goto fail;
  }

  /* 0 active, 3 inactive/failed, 4 no such unit, other: no systemd */
  ret = gbRunner(GB_TARGET_ACTIVE_CHECK " 2> " DEVNULLPATH);
  if (gbAppend(&out, "TARGET %s\n",
               ret == 0 ? "active" : (ret == 3 ? "inactive" : "unknown")) == -1) {
    goto fail;
  }

  if (blk->blocks && GB_STRDUP(blocks, blk->blocks) < 0) {
    goto fail;
  }
  for (line = blocks ? strtok_r(blocks, "\n", &saveptr) : NULL; line;
       line = strtok_r(NULL, "\n", &saveptr)) {
    char name[255] = {0};
    char gbid[UUID_BUF_SIZE] = {0};

    if (sscanf(line, "%254s %37s", name, gbid) != 2) {
      continue;
    }
    if (gbHostStatusBlock(blk->host, name, gbid, &out)) {
      goto fail;
    }
  }
  GB_FREE(blocks);

  reply->exit = 0;
  reply->out = out;
  return reply;

 fail:
  GB_FREE(blocks);
  free(out);
  reply->exit = ENOMEM;
  GB_ASPRINTF(&reply->out, "host status: out of memory");
  return reply;
}


bool_t
block_host_status_1_svc(blockHostStatus *blk, blockResponse *reply,
                        struct svc_req *rqstp)
{
  int ret;

  GB_RPC_CALL(host_status, blk, reply, rqstp, ret);
  return ret;
}


/* ------------------------------------------------------------------------
 * Cli side: collect the hosts of the volume and ask them
 * ------------------------------------------------------------------------ */

static void
gbHostCapsFree(gbHost *h)
{
  size_t i;


  for (i = 0; i < h->ncaps; i++) {
    GB_FREE(h->caps[i]);
  }
  GB_FREE(h->caps);
  h->ncaps = 0;
  h->capsKnown = false;
}


static int
gbHostCapsAdd(gbHost *h, const char *cap)
{
  if (GB_REALLOC_N(h->caps, h->ncaps + 1) < 0) {
    return -1;
  }
  if (GB_STRDUP(h->caps[h->ncaps], cap) < 0) {
    return -1;
  }
  h->ncaps++;
  return 0;
}


/* capabilities of the host, on the connection of gbHostQuery() */
static void
gbHostQueryCaps(gbHost *h, CLIENT *clnt)
{
  blockResponse reply = {0,};
  enum clnt_stat stat;
  gbCapObj *caps;
  size_t i, n;


  stat = block_version_1(NULL, &reply, clnt);
  if (stat == RPC_PROCUNAVAIL) {
    /* older than capabilities: what every such version could do */
    for (i = 0; gbOldCapabilities[i]; i++) {
      if (gbHostCapsAdd(h, gbOldCapabilities[i])) {
        gbHostCapsFree(h);
        return;
      }
    }
    h->capsKnown = true;
    return;
  }
  if (stat != RPC_SUCCESS) {
    LOG("mgmt", GB_LOG_WARNING, "%son host %s",
        clnt_sperror(clnt, "capabilities call failed"), h->addr);
    return;
  }

  caps = (gbCapObj *)reply.xdata.xdata_val;
  n = reply.xdata.xdata_len / sizeof(gbCapObj);
  for (i = 0; caps && i < n; i++) {
    caps[i].cap[sizeof(caps[i].cap) - 1] = '\0';
    if (caps[i].status && gbHostCapsAdd(h, caps[i].cap)) {
      gbHostCapsFree(h);
      break;
    }
  }
  if (i == n) {
    h->capsKnown = true;
  }
  clnt_freeres(clnt, (xdrproc_t)xdr_blockResponse, (char *)&reply);
}


static void
gbHostsFree(gbHosts *hs)
{
  size_t i;


  if (!hs) {
    return;
  }
  for (i = 0; i < hs->nhosts; i++) {
    gbHostCapsFree(&hs->hosts[i]);
    GB_FREE(hs->hosts[i].blocks);
  }
  GB_FREE(hs->hosts);
  hs->nhosts = 0;
}


static gbHost *
gbHostsGet(gbHosts *hs, const char *addr)
{
  size_t i;


  for (i = 0; i < hs->nhosts; i++) {
    if (!strcmp(hs->hosts[i].addr, addr)) {
      return &hs->hosts[i];
    }
  }

  if (GB_REALLOC_N(hs->hosts, hs->nhosts + 1) < 0) {
    return NULL;
  }
  memset(&hs->hosts[hs->nhosts], 0, sizeof(gbHost));
  GB_STRCPYSTATIC(hs->hosts[hs->nhosts].addr, addr);
  hs->hosts[hs->nhosts].volume = hs->volume;

  return &hs->hosts[hs->nhosts++];
}


static int
gbHostsAddBlock(gbHosts *hs, const char *name, MetaInfo *info)
{
  size_t i;
  gbHost *h;
  gbHostBlock *b;


  for (i = 0; i < info->nhosts; i++) {
    /* the block was moved away from this host (replace) */
    if (blockMetaStatusEnumParse(info->list[i]->status) == GB_CLEANUP_SUCCESS) {
      continue;
    }

    h = gbHostsGet(hs, info->list[i]->addr);
    if (!h || GB_REALLOC_N(h->blocks, h->nblocks + 1) < 0) {
      return -1;
    }
    b = &h->blocks[h->nblocks++];
    memset(b, 0, sizeof(*b));
    GB_STRCPYSTATIC(b->name, name);
    GB_STRCPYSTATIC(b->gbid, info->gbid);
    GB_STRCPYSTATIC(b->meta, info->list[i]->status);
    b->exported = blockhostIsValid(info->list[i]->status);
    b->prio = info->prio_path[0] && !strcmp(info->prio_path, h->addr);
    snprintf(b->alua, sizeof(b->alua), "-");
  }

  return 0;
}


static int
gbHostsRead(struct glfs *glfs, char *volume, gbHosts *hs,
            int *errCode, char **errMsg)
{
  struct glfs_fd *tgmdfd = NULL;
  struct dirent *entry;
  MetaInfo *info = NULL;
  int ret = -1;


  tgmdfd = glfs_opendir(glfs, GB_METADIR);
  if (!tgmdfd) {
    *errCode = errno;
    GB_ASPRINTF(errMsg, "Not able to open metadata directory for volume "
                "%s[%s]", volume, strerror(*errCode));
    LOG("mgmt", GB_LOG_ERROR, "glfs_opendir(%s): on volume %s failed[%s]",
        GB_METADIR, volume, strerror(*errCode));
    goto out;
  }

  while ((entry = glfs_readdir(tgmdfd))) {
    if (strchr(entry->d_name, '.')) {
      continue;   /* ".", "..", prio.info, meta.lock */
    }

    if (GB_ALLOC(info) < 0) {
      *errCode = ENOMEM;
      goto out;
    }
    if (blockGetMetaInfo(glfs, entry->d_name, info, errCode)) {
      LOG("mgmt", GB_LOG_WARNING, "skip block %s/%s: no metadata[%s]",
          volume, entry->d_name, strerror(*errCode));
      blockFreeMetaInfo(info);
      info = NULL;
      *errCode = 0;
      continue;
    }
    if (gbHostsAddBlock(hs, entry->d_name, info)) {
      *errCode = ENOMEM;
      goto out;
    }
    blockFreeMetaInfo(info);
    info = NULL;
  }

  *errCode = 0;
  ret = 0;

 out:
  if (info) {
    blockFreeMetaInfo(info);
  }
  if (tgmdfd && glfs_closedir(tgmdfd) != 0) {
    LOG("mgmt", GB_LOG_ERROR, "glfs_closedir(%s): on volume %s failed[%s]",
        GB_METADIR, volume, strerror(errno));
  }
  return ret;
}


/* connect with a timeout, clnttcp_create() would wait for the kernel */
static int
gbHostConnect(const char *addr, struct sockaddr_in *sin, char *err, size_t len)
{
  struct addrinfo hints, *res = NULL;
  struct pollfd pfd;
  socklen_t slen = sizeof(int);
  int soerr = 0;
  int flags;
  int fd = -1;
  int ret;


  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  ret = getaddrinfo(addr, GB_TCP_PORT_STR, &hints, &res);
  if (ret) {
    snprintf(err, len, "can not resolve: %s", gai_strerror(ret));
    return -1;
  }
  memcpy(sin, res->ai_addr, sizeof(*sin));
  freeaddrinfo(res);

  fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    snprintf(err, len, "socket: %s", strerror(errno));
    return -1;
  }
  flags = fcntl(fd, F_GETFL, 0);
  fcntl(fd, F_SETFL, flags | O_NONBLOCK);

  ret = connect(fd, (struct sockaddr *)sin, sizeof(*sin));
  if (ret < 0 && errno != EINPROGRESS) {
    snprintf(err, len, "%s", strerror(errno));
    goto fail;
  }
  if (ret < 0) {
    pfd.fd = fd;
    pfd.events = POLLOUT;
    ret = poll(&pfd, 1, GB_HOSTS_CONNECT_MS);
    if (ret == 0) {
      snprintf(err, len, "connection timed out after %ds",
               GB_HOSTS_CONNECT_MS / 1000);
      goto fail;
    }
    if (ret < 0 || getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &slen) < 0) {
      snprintf(err, len, "%s", strerror(errno));
      goto fail;
    }
    if (soerr) {
      snprintf(err, len, "%s", strerror(soerr));
      goto fail;
    }
  }
  fcntl(fd, F_SETFL, flags);

  return fd;

 fail:
  close(fd);
  return -1;
}


/* can a connection to gluster-blockd of 'addr' be made within 3 seconds */
int
blockHostReachable(const char *addr, char *err, size_t len)
{
  struct sockaddr_in sin;
  int fd;


  fd = gbHostConnect(addr, &sin, err, len);
  if (fd < 0) {
    return 0;
  }
  close(fd);

  return 1;
}


static void
gbHostParseStatus(gbHost *h, char *out)
{
  char *line;
  char *saveptr = NULL;
  size_t i;


  for (line = strtok_r(out, "\n", &saveptr); line;
       line = strtok_r(NULL, "\n", &saveptr)) {
    char gbid[UUID_BUF_SIZE] = {0};
    char alua[64] = {0};
    char sessions[1024] = "-";
    char *io;
    int backstore = 0;
    int enabled = 0;
    size_t tpg = 0;
    size_t cmds = 0, rmb = 0, wmb = 0;

    if (sscanf(line, "VERSION %63s", h->version) == 1 ||
        sscanf(line, "TCMU-RUNNER %15s", h->tcmu) == 1 ||
        sscanf(line, "TARGET %15s", h->target) == 1) {
      continue;
    }
    if (sscanf(line, "BLOCK %37s backstore=%d tpg=%zu enabled=%d alua=%63s",
               gbid, &backstore, &tpg, &enabled, alua) != 5) {
      continue;
    }
    /* I/O counters and sessions, not sent by the first version */
    if ((io = strstr(line, " cmds="))) {
      sscanf(io, " cmds=%zu rmb=%zu wmb=%zu sessions=%1023s", &cmds, &rmb,
             &wmb, sessions);
    }
    for (i = 0; i < h->nblocks; i++) {
      if (!strcmp(h->blocks[i].gbid, gbid)) {
        h->blocks[i].reported = true;
        h->blocks[i].backstore = backstore;
        h->blocks[i].tpg = tpg;
        h->blocks[i].enabled = enabled;
        GB_STRCPYSTATIC(h->blocks[i].alua, alua);
        h->blocks[i].cmds = cmds;
        h->blocks[i].rmb = rmb;
        h->blocks[i].wmb = wmb;
        GB_STRCPYSTATIC(h->blocks[i].sessions, sessions);
      }
    }
  }
}


static void *
gbHostQuery(void *data)
{
  gbHost *h = data;
  blockHostStatus req = {{0},};
  blockResponse reply = {0,};
  struct sockaddr_in sin;
  struct timeval tv = {GB_HOSTS_CALL_SEC, 0};
  CLIENT *clnt = NULL;
  enum clnt_stat stat;
  char *blocks = NULL;
  size_t i;
  int fd;


  fd = gbHostConnect(h->addr, &sin, h->error, sizeof(h->error));
  if (fd < 0) {
    h->daemon = GB_HOST_UNREACHABLE;
    return NULL;
  }

  clnt = clnttcp_create(&sin, GLUSTER_BLOCK, GLUSTER_BLOCK_VERS, &fd, 0, 0);
  if (!clnt) {
    h->daemon = GB_HOST_UNREACHABLE;
    snprintf(h->error, sizeof(h->error), "%s",
             clnt_spcreateerror("client create failed"));
    close(fd);
    return NULL;
  }
  clnt_control(clnt, CLSET_TIMEOUT, (char *)&tv);

  for (i = 0; i < h->nblocks; i++) {
    if (gbAppend(&blocks, "%s %s\n", h->blocks[i].name,
                 h->blocks[i].gbid) == -1) {
      h->daemon = GB_HOST_ERROR;
      snprintf(h->error, sizeof(h->error), "out of memory");
      goto out;
    }
  }
  GB_STRCPYSTATIC(req.volume, h->volume ? h->volume : "");
  GB_STRCPYSTATIC(req.host, h->addr);
  req.blocks = blocks ? blocks : (char *)"";

  stat = block_host_status_1(&req, &reply, clnt);
  if (stat == RPC_PROCUNAVAIL) {
    h->daemon = GB_HOST_OLD_VERSION;
    snprintf(h->error, sizeof(h->error),
             "gluster-blockd is too old to report the host status");
    if (h->wantCaps) {
      gbHostQueryCaps(h, clnt);
    }
    goto out;
  }
  if (stat != RPC_SUCCESS) {
    h->daemon = GB_HOST_ERROR;
    snprintf(h->error, sizeof(h->error), "%s",
             clnt_sperror(clnt, "host status call failed"));
    goto out;
  }
  if (reply.exit) {
    h->daemon = GB_HOST_ERROR;
    snprintf(h->error, sizeof(h->error), "%s",
             reply.out ? reply.out : "host status failed");
  } else {
    h->daemon = GB_HOST_UP;
    if (reply.out) {
      gbHostParseStatus(h, reply.out);
    }
  }
  clnt_freeres(clnt, (xdrproc_t)xdr_blockResponse, (char *)&reply);
  if (h->wantCaps && h->daemon == GB_HOST_UP) {
    gbHostQueryCaps(h, clnt);
  }

 out:
  free(blocks);
  clnt_destroy(clnt);
  close(fd);    /* not closed by clnt_destroy(), we created it */
  return NULL;
}


int
blockHostServices(const char *volume, const char *addr, char *tcmu,
                  size_t tlen, char *err, size_t elen)
{
  gbHost h;


  memset(&h, 0, sizeof(h));
  GB_STRCPYSTATIC(h.addr, addr);
  h.volume = volume;
  gbHostQuery(&h);

  snprintf(tcmu, tlen, "%s", h.tcmu[0] ? h.tcmu : "unknown");
  snprintf(err, elen, "%s", h.error);
  return h.daemon == GB_HOST_UP ? 0 : -1;
}


int
blockHostLunIo(const char *volume, const char *addr, gbLunIo *io, size_t n,
               char *err, size_t elen)
{
  gbHost h;
  size_t i;


  memset(&h, 0, sizeof(h));
  GB_STRCPYSTATIC(h.addr, addr);
  h.volume = volume;
  if (n && GB_ALLOC_N(h.blocks, n) < 0) {
    snprintf(err, elen, "out of memory");
    return -1;
  }
  h.nblocks = n;
  for (i = 0; i < n; i++) {
    GB_STRCPYSTATIC(h.blocks[i].name, io[i].name);
    GB_STRCPYSTATIC(h.blocks[i].gbid, io[i].gbid);
  }

  gbHostQuery(&h);

  for (i = 0; i < n; i++) {
    io[i].reported = h.blocks[i].reported;
    io[i].enabled = h.blocks[i].enabled;
    io[i].cmds = h.blocks[i].cmds;
    io[i].rmb = h.blocks[i].rmb;
    io[i].wmb = h.blocks[i].wmb;
    GB_STRCPYSTATIC(io[i].sessions, h.blocks[i].sessions);
  }
  snprintf(err, elen, "%s", h.error);
  GB_FREE(h.blocks);

  return h.daemon == GB_HOST_UP ? 0 : -1;
}


static void
gbHostsQuery(gbHosts *hs)
{
  pthread_t *tid = NULL;
  size_t i;


  if (GB_ALLOC_N(tid, hs->nhosts) < 0) {
    for (i = 0; i < hs->nhosts; i++) {
      gbHostQuery(&hs->hosts[i]);
    }
    return;
  }
  for (i = 0; i < hs->nhosts; i++) {
    pthread_create(&tid[i], NULL, gbHostQuery, &hs->hosts[i]);
  }
  for (i = 0; i < hs->nhosts; i++) {
    pthread_join(tid[i], NULL);
  }
  GB_FREE(tid);
}


/* problem of the host itself, NULL if there is none */
static const char *
gbHostProblem(gbHost *h, char *buf, size_t len)
{
  if (!strcmp(h->state, GB_HOST_STATE_ENTERING) ||
      !strcmp(h->state, GB_HOST_STATE_LEAVING)) {
    snprintf(buf, len, "maintenance %s did not finish (%s), run it again",
             strcmp(h->state, GB_HOST_STATE_ENTERING) ? "stop" : "start",
             h->state);
    return buf;
  }
  if (!strcmp(h->state, GB_HOST_STATE_MAINTENANCE)) {
    return NULL;    /* down or without tcmu-runner on purpose */
  }
  if (h->daemon == GB_HOST_OLD_VERSION) {
    snprintf(buf, len, "%s", h->error);
    return buf;
  }
  if (h->daemon != GB_HOST_UP) {
    snprintf(buf, len, "gluster-blockd %s (%s)", gbHostDaemonLookup[h->daemon],
             h->error);
    return buf;
  }
  if (strcmp(h->tcmu, "running")) {
    snprintf(buf, len, "tcmu-runner is %s", h->tcmu);
    return buf;
  }
  return NULL;
}


/* problems of one block on one host, NULL if it is fine */
static const char *
gbHostBlockProblem(gbHost *h, gbHostBlock *b, char *buf, size_t len)
{
  bool maint = !strcmp(h->state, GB_HOST_STATE_MAINTENANCE);

  if (!b->exported) {
    snprintf(buf, len, "metadata state %s", b->meta);
    return buf;
  }
  if (maint && b->prio) {
    snprintf(buf, len, "prio path on a host in maintenance");
    return buf;
  }
  if (h->daemon != GB_HOST_UP || !b->reported) {
    return NULL;   /* reported once for the host */
  }
  if (!b->backstore) {
    snprintf(buf, len, "backstore not loaded");
    return buf;
  }
  if (!b->tpg) {
    snprintf(buf, len, "no target portal group for this host");
    return buf;
  }
  if (maint && b->enabled) {
    snprintf(buf, len, "TPG %zu enabled on a host in maintenance", b->tpg);
    return buf;
  }
  if (!maint && !b->enabled) {
    snprintf(buf, len, "TPG %zu disabled", b->tpg);
    return buf;
  }
  if (strcmp(b->alua, "-") &&
      (b->prio != !strcmp(b->alua, "ao"))) {
    snprintf(buf, len, "ALUA group %s but %s the prio path", b->alua,
             b->prio ? "is" : "is not");
    return buf;
  }
  switch (blockMetaStatusEnumParse(b->meta)) {
  case GB_AUTH_ENFORCE_FAIL:
  case GB_AUTH_CLEAR_ENFORCE_FAIL:
  case GB_RP_FAIL:
  case GB_RS_FAIL:
    snprintf(buf, len, "metadata state %s", b->meta);
    return buf;
  }

  return NULL;
}


static int
gbHostBlockCmp(const void *a, const void *b)
{
  return strcmp(((const gbHostBlock *)a)->name, ((const gbHostBlock *)b)->name);
}


static void
gbHostsSummarize(gbHosts *hs)
{
  char buf[512];
  size_t i, j;


  for (i = 0; i < hs->nhosts; i++) {
    gbHost *h = &hs->hosts[i];

    if (h->nblocks > 1) {
      qsort(h->blocks, h->nblocks, sizeof(*h->blocks), gbHostBlockCmp);
    }

    if (gbHostProblem(h, buf, sizeof(buf))) {
      h->nproblems++;
    }
    for (j = 0; j < h->nblocks; j++) {
      gbHostBlock *b = &h->blocks[j];

      if (b->exported) {
        h->nexported++;
      }
      if (b->exported && b->prio) {
        h->nprio++;
      }
      if (b->exported && b->reported && b->backstore) {
        h->nloaded++;
      }
      if (b->exported && b->reported && b->tpg && b->enabled) {
        h->nenabled++;
      }
      if (gbHostBlockProblem(h, b, buf, sizeof(buf))) {
        h->nproblems++;
      }
    }
  }
}


static const char *
gbHostDaemonText(gbHost *h, char *buf, size_t len)
{
  if (h->daemon == GB_HOST_UP) {
    snprintf(buf, len, "%s", h->version[0] ? h->version : "up");
  } else {
    snprintf(buf, len, "%s", gbHostDaemonLookup[h->daemon]);
  }
  return buf;
}


static json_object *
gbHostToJson(gbHost *h, bool detail)
{
  json_object *jh = json_object_new_object();
  json_object *jp = json_object_new_array();
  json_object *jb = NULL;
  char buf[512];
  size_t j;
  bool up = h->daemon == GB_HOST_UP;


  json_object_object_add(jh, "HOST", GB_JSON_OBJ_TO_STR(h->addr));
  json_object_object_add(jh, "DAEMON",
                         GB_JSON_OBJ_TO_STR(gbHostDaemonLookup[h->daemon]));
  if (up) {
    json_object_object_add(jh, "VERSION", GB_JSON_OBJ_TO_STR(h->version));
    json_object_object_add(jh, "TCMU-RUNNER", GB_JSON_OBJ_TO_STR(h->tcmu));
    json_object_object_add(jh, "TARGET", GB_JSON_OBJ_TO_STR(h->target));
  }
  json_object_object_add(jh, "BLOCKS", json_object_new_int64(h->nexported));
  if (up) {
    json_object_object_add(jh, "LOADED", json_object_new_int64(h->nloaded));
    json_object_object_add(jh, "TPG-ENABLED",
                           json_object_new_int64(h->nenabled));
  }
  json_object_object_add(jh, "PRIO-PATHS", json_object_new_int64(h->nprio));

  json_object_object_add(jh, "STATE", GB_JSON_OBJ_TO_STR(
                         h->state[0] ? h->state : GB_HOST_STATE_ACTIVE));
  if (gbHostProblem(h, buf, sizeof(buf))) {
    json_object_array_add(jp, json_object_new_string(buf));
  }
  if (detail) {
    jb = json_object_new_array();
  }
  for (j = 0; j < h->nblocks; j++) {
    gbHostBlock *b = &h->blocks[j];
    const char *problem = gbHostBlockProblem(h, b, buf, sizeof(buf));
    char *msg = NULL;

    if (problem && GB_ASPRINTF(&msg, "%s: %s", b->name, problem) != -1) {
      json_object_array_add(jp, GB_JSON_OBJ_TO_STR(msg));
      GB_FREE(msg);
    }
    if (detail) {
      json_object *o = json_object_new_object();

      json_object_object_add(o, "NAME", GB_JSON_OBJ_TO_STR(b->name));
      json_object_object_add(o, "GBID", GB_JSON_OBJ_TO_STR(b->gbid));
      json_object_object_add(o, "META", GB_JSON_OBJ_TO_STR(b->meta));
      json_object_object_add(o, "PRIO", json_object_new_boolean(b->prio));
      if (b->reported) {
        json_object_object_add(o, "BACKSTORE",
                               json_object_new_boolean(b->backstore));
        json_object_object_add(o, "TPG", json_object_new_int64(b->tpg));
        json_object_object_add(o, "TPG-ENABLED",
                               json_object_new_boolean(b->enabled));
        json_object_object_add(o, "ALUA", GB_JSON_OBJ_TO_STR(b->alua));
        json_object_object_add(o, "SCSI-CMDS", json_object_new_int64(b->cmds));
        json_object_object_add(o, "READ-MB", json_object_new_int64(b->rmb));
        json_object_object_add(o, "WRITE-MB", json_object_new_int64(b->wmb));
        if (strcmp(b->sessions, "-")) {
          char *s = strdup(b->sessions), *p;

          for (p = s; p && *p; p++) {
            if (*p == ',') {
              *p = ' ';
            }
          }
          blockStr2arrayAddToJsonObj(o, s ? s : (char *)"", "SESSIONS");
          free(s);
        }
      }
      json_object_object_add(o, "PROBLEM", GB_JSON_OBJ_TO_STR(problem));
      json_object_array_add(jb, o);
    }
  }
  json_object_object_add(jh, "PROBLEMS", jp);
  if (detail) {
    json_object_object_add(jh, "BLOCK-LIST", jb);
  }

  return jh;
}


static int
gbHostsFormatJson(blockHostsCli *blk, gbHosts *hs, gbHost *only,
                  blockResponse *reply)
{
  json_object *jo = json_object_new_object();
  json_object *ja = json_object_new_array();
  size_t i;
  bool healthy = true;
  int ret;


  json_object_object_add(jo, "VOLUME", GB_JSON_OBJ_TO_STR(blk->volume));
  for (i = 0; i < hs->nhosts; i++) {
    if (only && only != &hs->hosts[i]) {
      continue;
    }
    if (hs->hosts[i].nproblems) {
      healthy = false;
    }
    json_object_array_add(ja, gbHostToJson(&hs->hosts[i], !!only));
  }
  json_object_object_add(jo, "HOSTS", ja);
  json_object_object_add(jo, "HEALTHY", json_object_new_boolean(healthy));
  json_object_object_add(jo, "RESULT", GB_JSON_OBJ_TO_STR("SUCCESS"));

  ret = GB_ASPRINTF(&reply->out, "%s\n",
                    json_object_to_json_string_ext(jo,
                                   mapJsonFlagToJsonCstring(blk->json_resp)));
  json_object_put(jo);

  return ret == -1 ? -1 : 0;
}


static int
gbHostsFormatPlain(blockHostsCli *blk, gbHosts *hs, gbHost *only,
                   blockResponse *reply)
{
  char *out = NULL;
  char *problems = NULL;
  char daemon[64];
  char buf[512];
  size_t i, j;
  int w = (int)strlen("HOST");
  int dw = (int)strlen("unreachable");


  for (i = 0; i < hs->nhosts; i++) {
    if ((int)strlen(hs->hosts[i].addr) > w) {
      w = (int)strlen(hs->hosts[i].addr);
    }
    gbHostDaemonText(&hs->hosts[i], daemon, sizeof(daemon));
    if ((int)strlen(daemon) > dw) {
      dw = (int)strlen(daemon);
    }
  }

  if (gbAppend(&out, "VOLUME: %s\n", blk->volume) == -1) {
    goto fail;
  }

  if (!only) {
    if (gbAppend(&out, "%-*s  %-11s  %-*s  %-11s  %-8s  %6s  %6s  %7s  %4s\n",
                 w, "HOST", "STATE", dw, "DAEMON", "TCMU-RUNNER", "TARGET",
                 "BLOCKS", "LOADED", "ENABLED", "PRIO") == -1) {
      goto fail;
    }
  }

  for (i = 0; i < hs->nhosts; i++) {
    gbHost *h = &hs->hosts[i];
    bool up = h->daemon == GB_HOST_UP;
    char loaded[32] = "-";
    char enabled[32] = "-";

    if (only && only != h) {
      continue;
    }
    if (up) {
      snprintf(loaded, sizeof(loaded), "%zu", h->nloaded);
      snprintf(enabled, sizeof(enabled), "%zu", h->nenabled);
    }
    gbHostDaemonText(h, daemon, sizeof(daemon));

    if (only) {
      if (gbAppend(&out, "HOST: %s\nSTATE: %s\nDAEMON: %s\nTCMU-RUNNER: %s\n"
                   "TARGET: %s\nBLOCKS: %zu\nLOADED: %s\nTPG-ENABLED: %s\n"
                   "PRIO-PATHS: %zu\n",
                   h->addr, h->state[0] ? h->state : GB_HOST_STATE_ACTIVE,
                   daemon, up ? h->tcmu : "-", up ? h->target : "-",
                   h->nexported, loaded, enabled, h->nprio) == -1) {
        goto fail;
      }
    } else if (gbAppend(&out, "%-*s  %-11s  %-*s  %-11s  %-8s  %6zu  %6s  %7s  %4zu\n",
                        w, h->addr,
                        h->state[0] ? h->state : GB_HOST_STATE_ACTIVE,
                        dw, daemon, up ? h->tcmu : "-",
                        up ? h->target : "-", h->nexported, loaded, enabled,
                        h->nprio) == -1) {
      goto fail;
    }

    if (gbHostProblem(h, buf, sizeof(buf)) &&
        gbAppend(&problems, "  %s: %s\n", h->addr, buf) == -1) {
      goto fail;
    }
    for (j = 0; j < h->nblocks; j++) {
      const char *problem = gbHostBlockProblem(h, &h->blocks[j], buf,
                                               sizeof(buf));

      if (problem && gbAppend(&problems, "  %s: %s: %s\n", h->addr,
                              h->blocks[j].name, problem) == -1) {
        goto fail;
      }
    }
  }

  if (only) {
    int bw = (int)strlen("BLOCK");

    for (j = 0; j < only->nblocks; j++) {
      if ((int)strlen(only->blocks[j].name) > bw) {
        bw = (int)strlen(only->blocks[j].name);
      }
    }
    if (gbAppend(&out, "\n%-*s  %-20s  %-9s  %3s  %-7s  %-4s  %-4s  %s\n", bw,
                 "BLOCK", "META", "BACKSTORE", "TPG", "ENABLED", "ALUA", "PRIO",
                 "SESSIONS") == -1) {
      goto fail;
    }
    for (j = 0; j < only->nblocks; j++) {
      gbHostBlock *b = &only->blocks[j];
      char tpg[32] = "-";

      if (b->reported && b->tpg) {
        snprintf(tpg, sizeof(tpg), "%zu", b->tpg);
      }
      if (gbAppend(&out, "%-*s  %-20s  %-9s  %3s  %-7s  %-4s  %-4s  %s\n", bw,
                   b->name, b->meta,
                   b->reported ? (b->backstore ? "loaded" : "missing") : "-",
                   tpg,
                   b->reported && b->tpg ? (b->enabled ? "yes" : "no") : "-",
                   b->reported ? b->alua : "-",
                   b->prio ? "yes" : "no",
                   b->reported && b->sessions[0] ? b->sessions : "-") == -1) {
        goto fail;
      }
    }
  }

  if (gbAppend(&out, "%s%s", problems ? "\nPROBLEMS:\n" : "\nPROBLEMS: none\n",
               problems ? problems : "") == -1) {
    goto fail;
  }

  free(problems);
  reply->out = out;
  return 0;

 fail:
  free(problems);
  free(out);
  return -1;
}


static blockResponse *
block_hosts_cli_1_svc_st(blockHostsCli *blk, struct svc_req *rqstp)
{
  blockResponse *reply = NULL;
  struct glfs *glfs;
  struct glfs_fd *lkfd = NULL;
  gbHosts hs = {blk->volume, 0, NULL};
  gbHost *only = NULL;
  size_t i;
  int errCode = -1;
  char *errMsg = NULL;


  LOG("mgmt", GB_LOG_INFO, "hosts cli request, volume=%s host=%s",
      blk->volume, blk->host[0] ? blk->host : "*");

  if (GB_ALLOC(reply) < 0) {
    return NULL;
  }

  errCode = 0;
  glfs = glusterBlockVolumeInit(blk->volume, &errCode, &errMsg);
  if (!glfs) {
    LOG("mgmt", GB_LOG_ERROR, "glusterBlockVolumeInit(%s) failed",
        blk->volume);
    goto optfail;
  }

  lkfd = glusterBlockOpenMetaLockFile(glfs, blk->volume, &errCode, &errMsg);
  if (!lkfd && errCode != ENOENT) {
    LOG("mgmt", GB_LOG_ERROR, "%s %s", FAILED_CREATING_META, blk->volume);
    goto optfail;
  }

  if (lkfd) {
    GB_METALOCK_RO_OR_GOTO(lkfd, blk->volume, errCode, errMsg, optfail);

    gbHostsRead(glfs, blk->volume, &hs, &errCode, &errMsg);
    for (i = 0; i < hs.nhosts; i++) {
      blockHostStateGet(glfs, hs.hosts[i].addr, hs.hosts[i].state,
                        sizeof(hs.hosts[i].state));
    }

    GB_METAUNLOCK(lkfd, blk->volume, errCode, errMsg);
    if (errCode) {
      goto optfail;
    }
  } else {
    /* the volume hosts no blocks: no hosts */
    errCode = 0;
    GB_FREE(errMsg);
  }

  if (blk->host[0]) {
    for (i = 0; i < hs.nhosts; i++) {
      if (!strcmp(hs.hosts[i].addr, blk->host)) {
        only = &hs.hosts[i];
      }
    }
    if (!only) {
      errCode = ENOENT;
      GB_ASPRINTF(&errMsg, "host %s does not export block devices of "
                  "volume %s", blk->host, blk->volume);
      goto optfail;
    }
  }

  /* the metadata is read, ask the hosts without holding the lock */
  if (only) {
    gbHostQuery(only);
  } else {
    gbHostsQuery(&hs);
  }
  gbHostsSummarize(&hs);

  if (blk->json_resp) {
    errCode = gbHostsFormatJson(blk, &hs, only, reply) ? ENOMEM : 0;
  } else {
    errCode = gbHostsFormatPlain(blk, &hs, only, reply) ? ENOMEM : 0;
  }

 optfail:
  LOG("mgmt", ((!!errCode) ? GB_LOG_ERROR : GB_LOG_INFO),
      "hosts cli return %s, volume=%s", errCode ? "failure" : "success",
      blk->volume);

  if (lkfd && glfs_close(lkfd) != 0) {
    LOG("mgmt", GB_LOG_ERROR, "glfs_close(%s): on volume %s failed[%s]",
        GB_TXLOCKFILE, blk->volume, strerror(errno));
  }

  if (errCode) {
    if (errCode < 0) {
      errCode = GB_DEFAULT_ERRCODE;
    }
    GB_FREE(reply->out);
    blockFormatErrorResponse(HOSTS_SRV, blk->json_resp, errCode,
                             errMsg ? errMsg : GB_DEFAULT_ERRMSG, reply);
  }
  reply->exit = errCode;

  gbHostsFree(&hs);
  GB_FREE(errMsg);

  return reply;
}


/*
 * 'version <volume>': add to @jo the version and the capabilities of every
 * host of the volume (HOSTS) and the capabilities this daemon and all hosts
 * that answer have (COMMON-CAPABILITIES). Hosts that do not answer are
 * listed, they do not narrow the common capabilities.
 */
int
blockVolumeHostsCaps(char *volume, json_object *jo, int *errCode,
                     char **errMsg)
{
  struct glfs *glfs;
  struct glfs_fd *lkfd = NULL;
  gbHosts hs = {volume, 0, NULL};
  json_object *jhosts, *jcommon;
  bool *common = NULL;
  size_t i, j, k;
  int ret = -1;


  glfs = glusterBlockVolumeInit(volume, errCode, errMsg);
  if (!glfs) {
    return -1;
  }
  lkfd = glusterBlockOpenMetaLockFile(glfs, volume, errCode, errMsg);
  if (!lkfd && *errCode != ENOENT) {
    return -1;
  }
  if (lkfd) {
    GB_METALOCK_RO_OR_GOTO(lkfd, volume, *errCode, *errMsg, out);
    gbHostsRead(glfs, volume, &hs, errCode, errMsg);
    GB_METAUNLOCK(lkfd, volume, *errCode, *errMsg);
    if (*errCode) {
      goto out;
    }
  } else {
    /* the volume hosts no blocks: no hosts, only this daemon */
    *errCode = 0;
    GB_FREE(*errMsg);
  }

  for (i = 0; i < hs.nhosts; i++) {
    hs.hosts[i].wantCaps = true;
  }
  gbHostsQuery(&hs);

  if (GB_ALLOC_N(common, GB_CAP_MAX) < 0) {
    *errCode = ENOMEM;
    goto out;
  }
  for (k = 0; k < GB_CAP_MAX; k++) {
    common[k] = globalCapabilities[k].status;
  }

  jhosts = json_object_new_array();
  for (i = 0; i < hs.nhosts; i++) {
    gbHost *h = &hs.hosts[i];
    json_object *jh = json_object_new_object();
    json_object *jc = json_object_new_array();

    json_object_object_add(jh, "HOST", GB_JSON_OBJ_TO_STR(h->addr));
    json_object_object_add(jh, "DAEMON",
                           GB_JSON_OBJ_TO_STR(gbHostDaemonLookup[h->daemon]));
    json_object_object_add(jh, "VERSION", GB_JSON_OBJ_TO_STR(h->version));
    for (j = 0; j < h->ncaps; j++) {
      json_object_array_add(jc, GB_JSON_OBJ_TO_STR(h->caps[j]));
    }
    json_object_object_add(jh, "CAPABILITIES", jc);
    json_object_object_add(jh, "ERROR", GB_JSON_OBJ_TO_STR(h->error));
    json_object_array_add(jhosts, jh);

    if (!h->capsKnown) {
      continue;
    }
    for (k = 0; k < GB_CAP_MAX; k++) {
      bool has = false;

      for (j = 0; j < h->ncaps && !has; j++) {
        has = !strcmp(h->caps[j], gbCapabilitiesLookup[k]);
      }
      common[k] = common[k] && has;
    }
  }

  jcommon = json_object_new_array();
  for (k = 0; k < GB_CAP_MAX; k++) {
    if (common[k]) {
      json_object_array_add(jcommon, GB_JSON_OBJ_TO_STR(gbCapabilitiesLookup[k]));
    }
  }
  json_object_object_add(jo, "VOLUME", GB_JSON_OBJ_TO_STR(volume));
  json_object_object_add(jo, "HOSTS", jhosts);
  json_object_object_add(jo, "COMMON-CAPABILITIES", jcommon);
  ret = 0;

 out:
  if (lkfd && glfs_close(lkfd) != 0) {
    LOG("mgmt", GB_LOG_ERROR, "glfs_close(%s): on volume %s failed[%s]",
        GB_TXLOCKFILE, volume, strerror(errno));
  }
  GB_FREE(common);
  gbHostsFree(&hs);
  return ret;
}


bool_t
block_hosts_cli_1_svc(blockHostsCli *blk, blockResponse *reply,
                      struct svc_req *rqstp)
{
  int ret;

  GB_RPC_CALL(hosts_cli, blk, reply, rqstp, ret);
  return ret;
}
