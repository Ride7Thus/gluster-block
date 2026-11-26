/*
  Copyright (c) 2026 gluster-block contributors
  This file is part of gluster-block.

  This file is licensed to you under your choice of the GNU Lesser
  General Public License, version 3 or any later version (LGPLv3 or
  later), or the GNU General Public License, version 2 (GPLv2), in all
  cases as published by the Free Software Foundation.
*/

/*
 * Long running operations and volume status.
 *
 * Creating or growing a preallocated block may take hours on a dispersed
 * volume. Such an operation used to keep the single threaded CLI service of
 * gluster-blockd and the volume wide metadata lock busy until it finished,
 * so the CLI waited until its RPC timeout and every other command, even
 * 'list' and 'info', hung behind it.
 *
 * Now:
 *  - create and modify-size run in a worker thread with a private glfs
 *    object. The CLI request returns as soon as the request is validated
 *    and the operation is recorded (RESULT: IN-PROGRESS + OPERATION-ID).
 *    Validation errors are still returned synchronously.
 *  - the operation holds the metadata lock until it finishes and records
 *    its progress in the "user.block.op-status" xattr of the lock file,
 *    which is visible from every node of the cluster.
 *  - other modifying operations fail at once with EBUSY while an operation
 *    is in progress on the volume; read only operations (list, info,
 *    genconfig, status) go on without the lock.
 *  - 'gluster-block status <volume>' reports the current or last operation
 *    of the volume, 'gluster-block --wait ...' polls the result of an
 *    operation by its id.
 */


# include  "block_common.h"

# include  <ctype.h>
# include  <dirent.h>
# include  <limits.h>
# include  <time.h>
# include  <unistd.h>


# define  GB_OPSTATUS_XATTR     "user.block.op-status"
# define  GB_OPSTATUS_BUFLEN    4096
# define  GB_LOCK_WAIT_SEC      60          /* wait for short operations */
# define  GB_LOCK_RETRY_USEC    200000
# define  GB_ASYNC_KEEP_SEC     86400       /* keep finished results a day */
# define  GB_ASYNC_KEEP_MAX     64
# define  GB_PROGRESS_SYNC_SEC  2           /* how often progress is stored */
# define  GB_ETA_MIN_SEC        5           /* measure this long before ETA */
# define  GB_OPSDIR             "/block-ops"  /* results, shared by all nodes */
# define  GB_OPRESULT_MAX       1048576
# define  GB_OPRESULT_GRACE     60          /* finished, result being stored */

# define  GB_OPSTATE_RUNNING     "IN-PROGRESS"
# define  GB_OPSTATE_SUCCESS     "SUCCESS"
# define  GB_OPSTATE_FAIL        "FAIL"
# define  GB_OPSTATE_INTERRUPTED "INTERRUPTED"


typedef struct gbOpStatus {
  char       state[16];
  char       op[32];
  char       id[UUID_BUF_SIZE];
  char       block[255];
  char       gbid[UUID_BUF_SIZE];
  char       node[256];
  char       phase[64];
  int        async;
  size_t     size;
  long long  started;
  long long  updated;
  long long  finished;
  int        errCode;
  char       msg[1024];

  /* measurable step inside the phase: allocating space, zero filling */
  char       step[64];
  long long  stepStarted;
  size_t     stepTotal;    /* bytes the step has to process */
  size_t     stepDone;     /* processed, written by the operation */
  int        stepByAlloc;  /* done = st_blocks of the file - stepBase */
  size_t     stepBase;     /* allocated bytes when the step started */
  int        stepCount;    /* total and done count items, not bytes */
} gbOpStatus;


typedef struct gbOpCtx {
  struct glfs_fd *lkfd;
  gbOpStatus st;
} gbOpCtx;


typedef struct gbAsyncOp {
  struct list_head list;
  char   id[UUID_BUF_SIZE];
  char   op[32];
  char   volume[255];
  char   block[255];

  pthread_t       tid;
  pthread_mutex_t mtx;
  pthread_cond_t  cv;
  bool   detached;   /* worker reached the long running part */
  bool   done;       /* worker finished, reply is set */
  bool   released;   /* RPC thread returned IN-PROGRESS, op is in registry */

  gbAsyncFn  fn;
  void      *arg;
  void     (*freeArg)(void *);
  blockResponse *reply;

  time_t started;
  time_t finished;
} gbAsyncOp;


static __thread gbOpCtx   *gbCurOp;      /* operation of this thread */
static __thread gbAsyncOp *gbCurAsync;   /* set in async worker threads */
static __thread struct glfs *gbPendingFini; /* released after the reply */

static LIST_HEAD(gbAsyncOps);
static pthread_mutex_t gbAsyncLock = PTHREAD_MUTEX_INITIALIZER;


/* ---------------------------------------------------------------------- */
/* operation status record                                                */

static void
gbCopyOneLine(char *dst, const char *src, size_t len)
{
  size_t i;

  if (!src) {
    dst[0] = '\0';
    return;
  }
  snprintf(dst, len, "%s", src);
  for (i = 0; dst[i]; i++) {
    if (dst[i] == '\n' || dst[i] == '\r') {
      dst[i] = ' ';
    }
  }
  /* trim trailing spaces left from newlines */
  while (i && dst[i - 1] == ' ') {
    dst[--i] = '\0';
  }
}


static int
gbOpStatusSerialize(const gbOpStatus *st, char *buf, size_t len)
{
  return snprintf(buf, len,
                  "STATE: %s\nOPERATION: %s\nID: %s\nBLOCK: %s\nGBID: %s\n"
                  "NODE: %s\nPHASE: %s\nASYNC: %d\nSIZE: %zu\nSTARTED: %lld\n"
                  "UPDATED: %lld\nFINISHED: %lld\nERRCODE: %d\nMSG: %s\n"
                  "STEP: %s\nSTEP-STARTED: %lld\nSTEP-TOTAL: %zu\n"
                  "STEP-DONE: %zu\nSTEP-BYALLOC: %d\nSTEP-BASE: %zu\n"
                  "STEP-COUNT: %d\n",
                  st->state, st->op, st->id, st->block, st->gbid, st->node,
                  st->phase, st->async, st->size, st->started, st->updated,
                  st->finished, st->errCode, st->msg, st->step,
                  st->stepStarted, st->stepTotal, st->stepDone,
                  st->stepByAlloc, st->stepBase, st->stepCount);
}


static void
gbOpStatusParse(char *buf, gbOpStatus *st)
{
  char *line, *save = NULL, *val;

  memset(st, 0, sizeof(*st));
  for (line = strtok_r(buf, "\n", &save); line;
       line = strtok_r(NULL, "\n", &save)) {
    val = strchr(line, ':');
    if (!val) {
      continue;
    }
    *val++ = '\0';
    while (*val == ' ') {
      val++;
    }

    if (!strcmp(line, "STATE")) {
      GB_STRCPYSTATIC(st->state, val);
    } else if (!strcmp(line, "OPERATION")) {
      GB_STRCPYSTATIC(st->op, val);
    } else if (!strcmp(line, "ID")) {
      GB_STRCPYSTATIC(st->id, val);
    } else if (!strcmp(line, "BLOCK")) {
      GB_STRCPYSTATIC(st->block, val);
    } else if (!strcmp(line, "GBID")) {
      GB_STRCPYSTATIC(st->gbid, val);
    } else if (!strcmp(line, "NODE")) {
      GB_STRCPYSTATIC(st->node, val);
    } else if (!strcmp(line, "PHASE")) {
      GB_STRCPYSTATIC(st->phase, val);
    } else if (!strcmp(line, "ASYNC")) {
      sscanf(val, "%d", &st->async);
    } else if (!strcmp(line, "SIZE")) {
      sscanf(val, "%zu", &st->size);
    } else if (!strcmp(line, "STARTED")) {
      sscanf(val, "%lld", &st->started);
    } else if (!strcmp(line, "UPDATED")) {
      sscanf(val, "%lld", &st->updated);
    } else if (!strcmp(line, "FINISHED")) {
      sscanf(val, "%lld", &st->finished);
    } else if (!strcmp(line, "ERRCODE")) {
      sscanf(val, "%d", &st->errCode);
    } else if (!strcmp(line, "MSG")) {
      GB_STRCPYSTATIC(st->msg, val);
    } else if (!strcmp(line, "STEP")) {
      GB_STRCPYSTATIC(st->step, val);
    } else if (!strcmp(line, "STEP-STARTED")) {
      sscanf(val, "%lld", &st->stepStarted);
    } else if (!strcmp(line, "STEP-TOTAL")) {
      sscanf(val, "%zu", &st->stepTotal);
    } else if (!strcmp(line, "STEP-DONE")) {
      sscanf(val, "%zu", &st->stepDone);
    } else if (!strcmp(line, "STEP-BYALLOC")) {
      sscanf(val, "%d", &st->stepByAlloc);
    } else if (!strcmp(line, "STEP-BASE")) {
      sscanf(val, "%zu", &st->stepBase);
    } else if (!strcmp(line, "STEP-COUNT")) {
      sscanf(val, "%d", &st->stepCount);
    }
  }
}


static int
gbOpStatusWrite(struct glfs_fd *lkfd, gbOpStatus *st)
{
  char buf[GB_OPSTATUS_BUFLEN];
  int len;

  st->updated = time(NULL);
  len = gbOpStatusSerialize(st, buf, sizeof(buf));
  if (len < 0 || (size_t)len >= sizeof(buf)) {
    len = strlen(buf);
  }

  if (glfs_fsetxattr(lkfd, GB_OPSTATUS_XATTR, buf, len, 0)) {
    LOG("mgmt", GB_LOG_WARNING, "glfs_fsetxattr(%s) for %s of %s failed[%s]",
        GB_OPSTATUS_XATTR, st->op, st->block, strerror(errno));
    return -1;
  }
  return 0;
}


/* returns 0 and fills @st if a record exists, -1 otherwise */
static int
gbOpStatusReadFd(struct glfs_fd *lkfd, gbOpStatus *st)
{
  char buf[GB_OPSTATUS_BUFLEN] = {0, };
  ssize_t ret;

  ret = glfs_fgetxattr(lkfd, GB_OPSTATUS_XATTR, buf, sizeof(buf) - 1);
  if (ret <= 0) {
    return -1;
  }
  buf[ret] = '\0';
  gbOpStatusParse(buf, st);
  return 0;
}


static bool
gbOpStatusRunning(const gbOpStatus *st)
{
  return !strcmp(st->state, GB_OPSTATE_RUNNING);
}


void
blockOpBegin(struct glfs_fd *lkfd, const char *op, const char *block,
             const char *gbid, size_t size)
{
  char host[HOST_NAME_MAX + 1] = {0, };

  if (gbCurOp) {
    /* previous operation of this thread did not unlock properly */
    GB_FREE(gbCurOp);
  }
  if (GB_ALLOC(gbCurOp) < 0) {
    return;
  }

  gbCurOp->lkfd = lkfd;
  GB_STRCPYSTATIC(gbCurOp->st.state, GB_OPSTATE_RUNNING);
  GB_STRCPYSTATIC(gbCurOp->st.op, op);
  GB_STRCPYSTATIC(gbCurOp->st.block, block);
  if (gbid) {
    GB_STRCPYSTATIC(gbCurOp->st.gbid, gbid);
  }
  if (gethostname(host, sizeof(host) - 1)) {
    snprintf(host, sizeof(host), "unknown");
  }
  GB_STRCPYSTATIC(gbCurOp->st.node, host);
  GB_STRCPYSTATIC(gbCurOp->st.phase, "validating");
  if (gbCurAsync) {
    GB_STRCPYSTATIC(gbCurOp->st.id, gbCurAsync->id);
    gbCurOp->st.async = 1;
  }
  gbCurOp->st.size = size;
  gbCurOp->st.started = time(NULL);

  gbOpStatusWrite(lkfd, &gbCurOp->st);
}


void
blockOpPhase(const char *phase)
{
  if (!gbCurOp) {
    return;
  }
  GB_STRCPYSTATIC(gbCurOp->st.phase, phase);
  gbCurOp->st.step[0] = '\0';
  gbOpStatusWrite(gbCurOp->lkfd, &gbCurOp->st);
  LOG("mgmt", GB_LOG_INFO, "%s of block %s: %s", gbCurOp->st.op,
      gbCurOp->st.block, phase);
}


/*
 * Start a step of @total bytes. With @byAlloc the progress is read from the
 * allocated size of the backing file (fallocate reports nothing while it
 * runs), @base is what was allocated before; else the operation reports it
 * with blockOpStepProgress().
 */
void
blockOpStep(const char *step, size_t total, bool byAlloc, size_t base)
{
  if (!gbCurOp) {
    return;
  }
  GB_STRCPYSTATIC(gbCurOp->st.step, step);
  gbCurOp->st.stepStarted = time(NULL);
  gbCurOp->st.stepTotal = total;
  gbCurOp->st.stepDone = 0;
  gbCurOp->st.stepByAlloc = byAlloc;
  gbCurOp->st.stepBase = base;
  gbCurOp->st.stepCount = 0;
  gbOpStatusWrite(gbCurOp->lkfd, &gbCurOp->st);
  LOG("mgmt", GB_LOG_INFO, "%s of block %s: %s %zu bytes", gbCurOp->st.op,
      gbCurOp->st.block, step, total);
}


/* a step of @total items (blocks), progress with blockOpStepProgress() */
void
blockOpStepCount(const char *step, size_t total)
{
  if (!gbCurOp) {
    return;
  }
  GB_STRCPYSTATIC(gbCurOp->st.step, step);
  gbCurOp->st.stepStarted = time(NULL);
  gbCurOp->st.stepTotal = total;
  gbCurOp->st.stepDone = 0;
  gbCurOp->st.stepByAlloc = 0;
  gbCurOp->st.stepBase = 0;
  gbCurOp->st.stepCount = 1;
  gbOpStatusWrite(gbCurOp->lkfd, &gbCurOp->st);
  LOG("mgmt", GB_LOG_INFO, "%s of %s: %s, %zu", gbCurOp->st.op,
      gbCurOp->st.block, step, total);
}


/* bytes done in the current step, stored every GB_PROGRESS_SYNC_SEC */
void
blockOpStepProgress(size_t done)
{
  time_t now;

  if (!gbCurOp || !gbCurOp->st.step[0]) {
    return;
  }
  gbCurOp->st.stepDone = done;
  now = time(NULL);
  if (now - gbCurOp->st.updated >= GB_PROGRESS_SYNC_SEC ||
      done >= gbCurOp->st.stepTotal) {
    gbOpStatusWrite(gbCurOp->lkfd, &gbCurOp->st);
  }
}


/* called from GB_METAUNLOCK, before the lock is released */
void
blockOpEnd(struct glfs_fd *lkfd, int errCode, const char *errMsg)
{
  if (!gbCurOp || gbCurOp->lkfd != lkfd) {
    return;
  }

  GB_STRCPYSTATIC(gbCurOp->st.state,
                  errCode ? GB_OPSTATE_FAIL : GB_OPSTATE_SUCCESS);
  GB_STRCPYSTATIC(gbCurOp->st.phase, "done");
  gbCurOp->st.step[0] = '\0';
  gbCurOp->st.errCode = errCode < 0 ? GB_DEFAULT_ERRCODE : errCode;
  gbCopyOneLine(gbCurOp->st.msg, errCode ? errMsg : NULL,
                sizeof(gbCurOp->st.msg));
  gbCurOp->st.finished = time(NULL);
  gbOpStatusWrite(lkfd, &gbCurOp->st);

  GB_FREE(gbCurOp);
}


/* ---------------------------------------------------------------------- */
/* metadata lock                                                          */

/*
 * Take the volume wide metadata lock.
 *
 * Returns 0 when the lock is held, 1 when @readonly is set and the caller
 * may go on without the lock, -1 on failure with @errCode and @errMsg set.
 *
 * While a long running operation is recorded as in progress, modifying
 * callers fail at once with EBUSY instead of hanging behind it, and read
 * only callers do not wait at all. Short operations of other nodes are
 * still waited for, up to GB_LOCK_WAIT_SEC.
 */
int
glusterBlockMetaLock(struct glfs_fd *lkfd, const char *volume, bool readonly,
                     int *errCode, char **errMsg)
{
  struct flock lock = {0, };
  gbOpStatus st;
  time_t deadline = time(NULL) + GB_LOCK_WAIT_SEC;
  int err;


  while (1) {
    memset(&lock, 0, sizeof(lock));
    lock.l_type = F_WRLCK;
    lock.l_whence = SEEK_SET;
    if (!glfs_posix_lock(lkfd, F_SETLK, &lock)) {
      break;
    }

    err = errno;
    if (err != EAGAIN && err != EACCES) {
      LOG("mgmt", GB_LOG_ERROR, "glfs_posix_lock() on volume %s failed[%s]",
          volume, strerror(err));
      *errCode = err;
      if (!*errMsg) {
        GB_ASPRINTF(errMsg, "Not able to acquire lock on %s[%s]", volume,
                    strerror(err));
      }
      return -1;
    }

    if (!gbOpStatusReadFd(lkfd, &st) && gbOpStatusRunning(&st)) {
      if (readonly) {
        LOG("mgmt", GB_LOG_DEBUG, "volume %s is busy with %s of %s, reading "
            "without lock", volume, st.op, st.block);
        return 1;
      }
      LOG("mgmt", GB_LOG_ERROR, "volume %s is busy: %s of block %s is in "
          "progress on %s", volume, st.op, st.block, st.node);
      *errCode = EBUSY;
      if (!*errMsg) {
        GB_ASPRINTF(errMsg, "Volume %s is busy: %s of block %s is in "
                    "progress on %s (%s).\nhint: run 'gluster-block status "
                    "%s' to track it", volume, st.op, st.block, st.node,
                    st.phase, volume);
      }
      return -1;
    }

    if (time(NULL) >= deadline) {
      if (readonly) {
        LOG("mgmt", GB_LOG_WARNING, "lock on volume %s is busy for more "
            "than %d seconds, reading without lock", volume,
            GB_LOCK_WAIT_SEC);
        return 1;
      }
      LOG("mgmt", GB_LOG_ERROR, "lock on volume %s is busy for more than %d "
          "seconds", volume, GB_LOCK_WAIT_SEC);
      *errCode = EBUSY;
      if (!*errMsg) {
        GB_ASPRINTF(errMsg, "Volume %s is busy with another operation, "
                    "try again later", volume);
      }
      return -1;
    }

    usleep(GB_LOCK_RETRY_USEC);
  }

  /* we own the lock: a record still in progress belongs to a dead daemon */
  if (!readonly && !gbOpStatusReadFd(lkfd, &st) && gbOpStatusRunning(&st)) {
    LOG("mgmt", GB_LOG_WARNING, "%s of block %s on volume %s started on %s "
        "was interrupted", st.op, st.block, volume, st.node);
    GB_STRCPYSTATIC(st.state, GB_OPSTATE_INTERRUPTED);
    st.finished = time(NULL);
    snprintf(st.msg, sizeof(st.msg), "gluster-blockd on %s stopped before "
             "the operation finished, check the block with "
             "'gluster-block info'", st.node);
    gbOpStatusWrite(lkfd, &st);
  }

  return 0;
}


/* ---------------------------------------------------------------------- */
/* background operations                                                  */

bool
gbAsyncInWorker(void)
{
  return gbCurAsync != NULL;
}


/* the long running part starts, let the CLI request return */
void
gbAsyncDetach(void)
{
  gbAsyncOp *op = gbCurAsync;

  if (!op) {
    return;
  }
  LOCK(op->mtx);
  if (!op->detached) {
    op->detached = true;
    pthread_cond_signal(&op->cv);
  }
  UNLOCK(op->mtx);
}


struct glfs *
blockVolumeGet(char *volume, int *errCode, char **errMsg, bool *priv)
{
  /*
   * Background operations get their own glfs object: the cached one may be
   * released by the LRU while they run, and the metadata code relies on
   * glfs_chdir(), whose working directory is shared by all users of a glfs
   * object. It also makes the posix lock conflict with the CLI thread.
   */
  *priv = gbAsyncInWorker();
  if (*priv) {
    return glusterBlockVolumeInitPrivate(volume, errCode, errMsg);
  }
  return glusterBlockVolumeInit(volume, errCode, errMsg);
}


void
blockVolumePut(struct glfs *glfs, bool priv)
{
  if (!glfs || !priv) {
    return;
  }
  if (gbCurAsync && !gbPendingFini) {
    /* glfs_fini() takes seconds, do it once the reply is handed over */
    gbPendingFini = glfs;
    return;
  }
  glfs_fini(glfs);
}


static void
gbAsyncOpFree(gbAsyncOp *op)
{
  if (!op) {
    return;
  }
  if (op->reply) {
    GB_FREE(op->reply->out);
    GB_FREE(op->reply->xdata.xdata_val);
    GB_FREE(op->reply);
  }
  pthread_mutex_destroy(&op->mtx);
  pthread_cond_destroy(&op->cv);
  GB_FREE(op);
}


/* drop old results, gbAsyncLock must be held */
static void
gbAsyncPrune(void)
{
  struct list_head *pos, *q;
  gbAsyncOp *op;
  time_t now = time(NULL);
  size_t count = 0;
  bool done;

  /* newest first */
  list_for_each_safe(pos, q, &gbAsyncOps) {
    op = list_entry(pos, gbAsyncOp, list);
    LOCK(op->mtx);
    done = op->done;
    UNLOCK(op->mtx);
    if (!done) {
      continue;
    }
    if (++count > GB_ASYNC_KEEP_MAX || now - op->finished > GB_ASYNC_KEEP_SEC) {
      list_del(pos);
      gbAsyncOpFree(op);
    }
  }
}


/*
 * Results of background operations are kept on the volume in
 * /block-ops/<operation id>, so 'gluster-block status <volume> <id>' and
 * --wait work from any node, also after the daemon that ran the operation
 * was restarted. Each file is written once by its operation, no lock needed.
 */
static bool
gbOpIdValid(const char *id)
{
  size_t i;

  for (i = 0; id[i]; i++) {
    if (!isxdigit((unsigned char)id[i]) && id[i] != '-') {
      return false;
    }
  }
  return i == 36;
}


static void
gbOpResultPrune(struct glfs *glfs)
{
  struct glfs_fd *dfd;
  struct dirent *entry;
  struct stat sb;
  char path[PATH_MAX];
  time_t now = time(NULL);

  dfd = glfs_opendir(glfs, GB_OPSDIR);
  if (!dfd) {
    return;
  }
  while ((entry = glfs_readdir(dfd))) {
    if (!gbOpIdValid(entry->d_name)) {
      continue;
    }
    snprintf(path, sizeof(path), "%s/%s", GB_OPSDIR, entry->d_name);
    if (!glfs_stat(glfs, path, &sb) && now - sb.st_mtime > GB_ASYNC_KEEP_SEC &&
        glfs_unlink(glfs, path)) {
      LOG("mgmt", GB_LOG_WARNING, "glfs_unlink(%s) failed[%s]", path,
          strerror(errno));
    }
  }
  glfs_closedir(dfd);
}


static void
gbOpResultStore(struct glfs *glfs, const char *id, int exitCode,
                const char *out)
{
  struct glfs_fd *fd;
  char path[PATH_MAX];
  char *buf = NULL;
  int len;

  if (glfs_mkdir(glfs, GB_OPSDIR, 0700) && errno != EEXIST) {
    LOG("mgmt", GB_LOG_WARNING, "glfs_mkdir(%s) failed[%s], result of %s "
        "is only kept on this node", GB_OPSDIR, strerror(errno), id);
    return;
  }

  snprintf(path, sizeof(path), "%s/%s", GB_OPSDIR, id);
  fd = glfs_creat(glfs, path, O_WRONLY | O_TRUNC, S_IRUSR | S_IWUSR);
  if (!fd) {
    LOG("mgmt", GB_LOG_WARNING, "glfs_creat(%s) failed[%s], result is only "
        "kept on this node", path, strerror(errno));
    return;
  }

  len = GB_ASPRINTF(&buf, "EXIT: %d\n%s", exitCode, out ? out : "");
  if (len > 0 && glfs_write(fd, buf, len, 0) != len) {
    LOG("mgmt", GB_LOG_WARNING, "glfs_write(%s) failed[%s]", path,
        strerror(errno));
  }
  GB_FREE(buf);
  if (glfs_close(fd)) {
    LOG("mgmt", GB_LOG_WARNING, "glfs_close(%s) failed[%s]", path,
        strerror(errno));
  }

  gbOpResultPrune(glfs);
}


/* 0 and the stored result, or -1 if there is none */
static int
gbOpResultLoad(struct glfs *glfs, const char *id, int *exitCode, char **out)
{
  struct glfs_fd *fd;
  struct stat sb;
  char path[PATH_MAX];
  char *buf = NULL, *body;
  ssize_t ret;
  size_t len, done = 0;
  int rc = -1;

  snprintf(path, sizeof(path), "%s/%s", GB_OPSDIR, id);
  fd = glfs_open(glfs, path, O_RDONLY);
  if (!fd) {
    return -1;
  }
  if (glfs_fstat(fd, &sb) || sb.st_size <= 0) {
    goto out;
  }
  len = sb.st_size > GB_OPRESULT_MAX ? GB_OPRESULT_MAX : sb.st_size;
  if (GB_ALLOC_N(buf, len + 1) < 0) {
    goto out;
  }
  while (done < len) {
    ret = glfs_read(fd, buf + done, len - done, 0);
    if (ret <= 0) {
      break;
    }
    done += ret;
  }
  buf[done] = '\0';

  if (sscanf(buf, "EXIT: %d", exitCode) != 1) {
    goto out;
  }
  body = strchr(buf, '\n');
  if (GB_STRDUP(*out, body ? body + 1 : "") < 0) {
    goto out;
  }
  rc = 0;

 out:
  GB_FREE(buf);
  glfs_close(fd);
  return rc;
}


static void *
gbAsyncWorker(void *data)
{
  gbAsyncOp *op = data;
  blockResponse *reply;
  bool released;
  void *arg;
  void (*freeArg)(void *);
  char desc[sizeof(op->op) + sizeof(op->volume) + sizeof(op->block) +
            sizeof(op->id) + 16];
  char id[sizeof(op->id)];
  int exitCode;


  gbCurAsync = op;
  reply = op->fn(op->arg);
  gbCurAsync = NULL;
  exitCode = reply ? reply->exit : -1;

  /*
   * Once done is set @op belongs to the RPC thread (not released) or to the
   * registry (released), so copy what is needed afterwards.
   */
  LOCK(op->mtx);
  released = op->released;
  arg = op->arg;
  freeArg = op->freeArg;
  snprintf(desc, sizeof(desc), "%s of %s/%s (id %s)", op->op, op->volume,
           op->block, op->id);
  GB_STRCPYSTATIC(id, op->id);
  if (released) {
    op->arg = NULL;
  }
  op->reply = reply;
  op->finished = time(NULL);
  op->done = true;
  pthread_cond_signal(&op->cv);
  UNLOCK(op->mtx);

  if (released) {
    if (!exitCode) {
      LOG("mgmt", GB_LOG_INFO, "background %s succeeded", desc);
    } else {
      LOG("mgmt", GB_LOG_ERROR, "background %s failed with %d", desc,
          exitCode);
    }
    /* the RPC thread is gone, the argument copy is ours to free */
    freeArg(arg);

    /* the id was handed out, publish the result for the other nodes; the
     * private glfs object of the operation is still alive */
    if (gbPendingFini) {
      gbOpResultStore(gbPendingFini, id, exitCode,
                      reply ? reply->out : NULL);
    }
  }

  if (gbPendingFini) {
    glfs_fini(gbPendingFini);
    gbPendingFini = NULL;
  }

  return NULL;
}


static blockResponse *
gbAsyncAcceptedResponse(gbAsyncOp *op, int json)
{
  blockResponse *reply;
  json_object *json_obj;

  if (GB_ALLOC(reply) < 0) {
    return NULL;
  }

  if (json) {
    json_obj = json_object_new_object();
    json_object_object_add(json_obj, "OPERATION-ID", json_object_new_string(op->id));
    json_object_object_add(json_obj, "OPERATION", json_object_new_string(op->op));
    json_object_object_add(json_obj, "NAME", json_object_new_string(op->block));
    json_object_object_add(json_obj, "VOLUME", json_object_new_string(op->volume));
    json_object_object_add(json_obj, "RESULT", GB_JSON_OBJ_TO_STR(GB_OPSTATE_RUNNING));
    GB_ASPRINTF(&reply->out, "%s\n",
                json_object_to_json_string_ext(json_obj,
                                               mapJsonFlagToJsonCstring(json)));
    json_object_put(json_obj);
  } else {
    GB_ASPRINTF(&reply->out,
                "OPERATION-ID: %s\nOPERATION: %s\nNAME: %s\nVOLUME: %s\n"
                "RESULT: %s\n", op->id, op->op, op->block, op->volume,
                GB_OPSTATE_RUNNING);
  }

  /* the id for 'gluster-block --wait', NUL terminated */
  if (GB_STRDUP(reply->xdata.xdata_val, op->id) >= 0) {
    reply->xdata.xdata_len = strlen(op->id) + 1;
  }
  reply->exit = 0;

  return reply;
}


blockResponse *
gbAsyncRun(const char *opname, const char *volume, const char *block,
           int json, gbAsyncFn fn, void *arg, void (*freeArg)(void *))
{
  gbAsyncOp *op = NULL;
  blockResponse *reply;
  uuid_t uuid;


  if (GB_ALLOC(op) < 0) {
    goto sync;
  }
  uuid_generate(uuid);
  uuid_unparse(uuid, op->id);
  GB_STRCPYSTATIC(op->op, opname);
  GB_STRCPYSTATIC(op->volume, volume);
  GB_STRCPYSTATIC(op->block, block);
  op->fn = fn;
  op->arg = arg;
  op->freeArg = freeArg;
  op->started = time(NULL);
  INIT_LIST_HEAD(&op->list);
  pthread_mutex_init(&op->mtx, NULL);
  pthread_cond_init(&op->cv, NULL);

  if (pthread_create(&op->tid, NULL, gbAsyncWorker, op)) {
    LOG("mgmt", GB_LOG_ERROR, "pthread_create() for %s of %s/%s failed[%s], "
        "running it synchronously", opname, volume, block, strerror(errno));
    gbAsyncOpFree(op);
    goto sync;
  }

  LOCK(op->mtx);
  while (!op->detached && !op->done) {
    pthread_cond_wait(&op->cv, &op->mtx);
  }

  if (op->done) {
    /* failed or finished before the long running part; the worker may
     * still be releasing its glfs object, do not wait for it */
    reply = op->reply;
    op->reply = NULL;
    UNLOCK(op->mtx);
    pthread_detach(op->tid);
    freeArg(arg);
    gbAsyncOpFree(op);
    return reply;
  }

  reply = gbAsyncAcceptedResponse(op, json);
  op->released = true;    /* from now on the worker frees @arg */
  UNLOCK(op->mtx);
  pthread_detach(op->tid);

  LOG("mgmt", GB_LOG_INFO, "%s of %s/%s continues in background, id %s",
      opname, volume, block, op->id);

  /* only the (single threaded) CLI service thread touches the registry
   * list, so @op cannot be pruned before it is added */
  LOCK(gbAsyncLock);
  list_add(&op->list, &gbAsyncOps);
  gbAsyncPrune();
  UNLOCK(gbAsyncLock);

  return reply;

 sync:
  reply = fn(arg);
  freeArg(arg);
  return reply;
}


/* ---------------------------------------------------------------------- */
/* status                                                                 */

static void
gbFormatTime(long long t, char *buf, size_t len)
{
  time_t tt = (time_t)t;
  struct tm tm;

  if (!t || !localtime_r(&tt, &tm)) {
    snprintf(buf, len, "-");
    return;
  }
  strftime(buf, len, "%Y-%m-%d %H:%M:%S %z", &tm);
}


static void
gbFormatDuration(long long sec, char *buf, size_t len)
{
  if (sec < 0) {
    sec = 0;
  }
  if (sec >= 3600) {
    snprintf(buf, len, "%lldh %02lldm %02llds", sec / 3600, (sec / 60) % 60,
             sec % 60);
  } else if (sec >= 60) {
    snprintf(buf, len, "%lldm %02llds", sec / 60, sec % 60);
  } else {
    snprintf(buf, len, "%llds", sec);
  }
}


/* is anybody holding the metadata lock? */
static bool
gbMetaLockHeld(struct glfs_fd *lkfd)
{
  struct flock lock = {0, };

  lock.l_type = F_WRLCK;
  lock.l_whence = SEEK_SET;
  if (glfs_posix_lock(lkfd, F_GETLK, &lock)) {
    return true;    /* unknown, trust the record */
  }
  return lock.l_type != F_UNLCK;
}


static blockResponse *
blockStatusVolume(blockStatusCli *blk, blockResponse *reply);


/*
 * Result of one operation, for --wait and 'status <volume> <id>'.
 * exit EINPROGRESS with the volume status while it runs, else the result
 * the operation returned (or a summary when only that is left).
 */
static blockResponse *
blockStatusOpId(blockStatusCli *blk, blockResponse *reply)
{
  struct list_head *pos;
  gbAsyncOp *op;
  struct glfs *glfs;
  struct glfs_fd *lkfd;
  char path[PATH_MAX];
  char *errMsg = NULL;
  int errCode = 0, exitCode;
  bool found = false, running = false;
  gbOpStatus st;


  if (!gbOpIdValid(blk->op_id)) {
    reply->exit = EINVAL;
    GB_ASPRINTF(&reply->out, "invalid operation id '%s'\n", blk->op_id);
    return reply;
  }

  /* started by this daemon */
  LOCK(gbAsyncLock);
  list_for_each(pos, &gbAsyncOps) {
    op = list_entry(pos, gbAsyncOp, list);
    if (strcmp(op->id, blk->op_id)) {
      continue;
    }
    found = true;
    LOCK(op->mtx);
    if (!op->done) {
      running = true;
    } else if (op->reply && op->reply->out) {
      reply->exit = op->reply->exit;
      GB_STRDUP(reply->out, op->reply->out);
    } else {
      reply->exit = GB_DEFAULT_ERRCODE;
      GB_ASPRINTF(&reply->out, "%s of %s/%s failed, please check the log "
                  "files\n", op->op, op->volume, op->block);
    }
    UNLOCK(op->mtx);
    break;
  }
  UNLOCK(gbAsyncLock);

  if (found && !running) {
    return reply;
  }

  if (!found) {
    /* started on another node, or before a restart of this daemon */
    glfs = glusterBlockVolumeInit(blk->volume, &errCode, &errMsg);
    if (!glfs) {
      blockFormatErrorResponse(STATUS_SRV, blk->json_resp,
                               errCode ? errCode : GB_DEFAULT_ERRCODE,
                               errMsg ? errMsg : GB_DEFAULT_ERRMSG, reply);
      GB_FREE(errMsg);
      return reply;
    }

    if (!gbOpResultLoad(glfs, blk->op_id, &exitCode, &reply->out)) {
      reply->exit = exitCode;
      return reply;
    }

    snprintf(path, sizeof(path), "%s/%s", GB_METADIR, GB_TXLOCKFILE);
    lkfd = glfs_open(glfs, path, O_RDWR);
    if (lkfd && !gbOpStatusReadFd(lkfd, &st) && !strcmp(st.id, blk->op_id)) {
      if (gbOpStatusRunning(&st)) {
        if (gbMetaLockHeld(lkfd)) {
          running = true;
        } else {
          reply->exit = GB_DEFAULT_ERRCODE;
          GB_ASPRINTF(&reply->out, "%s of %s/%s was interrupted: "
                      "gluster-blockd on %s stopped before it finished, check "
                      "the block with 'gluster-block info'\nRESULT:FAIL\n",
                      st.op, blk->volume, st.block, st.node);
        }
      } else if (time(NULL) - st.finished < GB_OPRESULT_GRACE) {
        running = true;   /* finished, the result is being stored */
      } else {
        reply->exit = st.errCode;
        GB_ASPRINTF(&reply->out, "%s%s%s of %s/%s: RESULT: %s\n",
                    st.msg, st.msg[0] ? "\n" : "", st.op, blk->volume,
                    st.block, st.state);
      }
    } else {
      reply->exit = ENOENT;
      GB_ASPRINTF(&reply->out, "operation %s is not known on volume %s, "
                  "results are kept for %d hours\n", blk->op_id, blk->volume,
                  GB_ASYNC_KEEP_SEC / 3600);
    }
    if (lkfd) {
      glfs_close(lkfd);
    }
  }

  if (running) {
    blockStatusVolume(blk, reply);
    if (!reply->exit) {
      reply->exit = EINPROGRESS;
    }
  }

  return reply;
}


static blockResponse *
blockStatusVolume(blockStatusCli *blk, blockResponse *reply)
{
  struct glfs *glfs;
  struct glfs_fd *lkfd = NULL;
  struct stat sb;
  char path[PATH_MAX];
  char started[64], updated[64], finished[192], elapsed[64];
  char *allocated = NULL, *total = NULL, *progress = NULL;
  char *stepDoneStr = NULL, *stepTotalStr = NULL, *stepProgress = NULL;
  char phaseText[256], remaining[64];
  char *errMsg = NULL;
  int errCode = 0;
  bool have = false, stale = false, haveUsed = false;
  size_t used = 0, stepDone = 0;
  int stepPct = -1;
  long long now = time(NULL), etaTime = 0, left = -1, stepElapsed;
  gbOpStatus st;
  const char *state, *volstate;
  json_object *json_obj = NULL;


  LOG("mgmt", GB_LOG_DEBUG, "status cli request, volume=%s", blk->volume);

  glfs = glusterBlockVolumeInit(blk->volume, &errCode, &errMsg);
  if (!glfs) {
    goto fail;
  }

  snprintf(path, sizeof(path), "%s/%s", GB_METADIR, GB_TXLOCKFILE);
  lkfd = glfs_open(glfs, path, O_RDWR);
  if (lkfd && !gbOpStatusReadFd(lkfd, &st)) {
    have = true;
    if (gbOpStatusRunning(&st) && !gbMetaLockHeld(lkfd)) {
      stale = true;
    }
  } else if (!lkfd && errno != ENOENT) {
    errCode = errno;
    GB_ASPRINTF(&errMsg, "Not able to open %s on volume %s[%s]", path,
                blk->volume, strerror(errCode));
    goto fail;
  }

  if (have) {
    state = stale ? GB_OPSTATE_INTERRUPTED : st.state;
    volstate = (gbOpStatusRunning(&st) && !stale) ? "BUSY" : "IDLE";

    gbFormatTime(st.started, started, sizeof(started));
    gbFormatTime(st.updated, updated, sizeof(updated));
    gbFormatTime(st.finished, finished, sizeof(finished));
    gbFormatDuration((st.finished ? st.finished : (long long)time(NULL)) -
                     st.started, elapsed, sizeof(elapsed));

    /* allocation progress of the backing file */
    if (!strcmp(volstate, "BUSY") && st.gbid[0] && st.size) {
      snprintf(path, sizeof(path), "%s/%s", GB_STOREDIR, st.gbid);
      if (!glfs_stat(glfs, path, &sb)) {
        haveUsed = true;
        used = (size_t)sb.st_blocks * 512;
        if (used > st.size) {
          used = st.size;
        }
        allocated = glusterBlockFormatSize("mgmt", used);
        total = glusterBlockFormatSize("mgmt", st.size);
        GB_ASPRINTF(&progress, "%s of %s (%d%%)", allocated ? allocated : "-",
                    total ? total : "-", (int)(used * 100.0 / st.size));
      }
    }

    /* progress and remaining time of the current step */
    snprintf(phaseText, sizeof(phaseText), "%s", st.phase);
    if (!strcmp(volstate, "BUSY")) {
      if (st.step[0] && st.stepTotal) {
        if (st.stepByAlloc) {
          stepDone = haveUsed && used > st.stepBase ? used - st.stepBase : 0;
        } else {
          stepDone = st.stepDone;
        }
        if (stepDone > st.stepTotal) {
          stepDone = st.stepTotal;
        }
        stepPct = (int)(stepDone * 100.0 / st.stepTotal);
        stepElapsed = now - st.stepStarted;
        if (stepDone && stepElapsed >= GB_ETA_MIN_SEC) {
          left = (long long)((double)(st.stepTotal - stepDone) * stepElapsed /
                             stepDone);
          etaTime = now + left;
        }
        if (!strcmp(st.step, st.phase)) {
          snprintf(phaseText, sizeof(phaseText), "%s (%d%%)", st.phase,
                   stepPct);
        } else {
          snprintf(phaseText, sizeof(phaseText), "%s (%s, %d%%)", st.phase,
                   st.step, stepPct);
        }

        if (st.stepCount) {
          GB_ASPRINTF(&stepDoneStr, "%zu", stepDone);
          GB_ASPRINTF(&stepTotalStr, "%zu", st.stepTotal);
        } else {
          stepDoneStr = glusterBlockFormatSize("mgmt", stepDone);
          stepTotalStr = glusterBlockFormatSize("mgmt", st.stepTotal);
        }
        GB_ASPRINTF(&stepProgress, "%s of %s (%d%%)",
                    stepDoneStr ? stepDoneStr : "-",
                    stepTotalStr ? stepTotalStr : "-", stepPct);
      }

      /* FINISHED shows the estimate while the operation runs */
      if (etaTime) {
        char when[64];

        gbFormatTime(etaTime, when, sizeof(when));
        gbFormatDuration(left, remaining, sizeof(remaining));
        snprintf(finished, sizeof(finished), "~ %s (%s left)", when,
                 remaining);
      } else if (st.step[0]) {
        snprintf(finished, sizeof(finished), "- (estimating)");
      }
    }
  } else {
    state = "-";
    volstate = "IDLE";
  }

  reply->exit = 0;
  if (blk->json_resp) {
    json_obj = json_object_new_object();
    json_object_object_add(json_obj, "VOLUME", json_object_new_string(blk->volume));
    json_object_object_add(json_obj, "STATUS", GB_JSON_OBJ_TO_STR(volstate));
    if (have) {
      json_object *o = json_object_new_object();
      json_object_object_add(o, "OPERATION", json_object_new_string(st.op));
      json_object_object_add(o, "OPERATION-ID", json_object_new_string(st.id));
      json_object_object_add(o, "NAME", json_object_new_string(st.block));
      json_object_object_add(o, "GBID", json_object_new_string(st.gbid));
      json_object_object_add(o, "NODE", json_object_new_string(st.node));
      json_object_object_add(o, "STATE", GB_JSON_OBJ_TO_STR(state));
      json_object_object_add(o, "PHASE", json_object_new_string(st.phase));
      json_object_object_add(o, "SIZE", json_object_new_int64(st.size));
      if (progress) {
        json_object_object_add(o, "ALLOCATED", GB_JSON_OBJ_TO_STR(progress));
      }
      if (stepPct >= 0) {
        json_object_object_add(o, "STEP", json_object_new_string(st.step));
        json_object_object_add(o, "STEP-DONE", json_object_new_int64(stepDone));
        json_object_object_add(o, "STEP-TOTAL",
                               json_object_new_int64(st.stepTotal));
        json_object_object_add(o, "STEP-PERCENT", json_object_new_int(stepPct));
      }
      if (etaTime) {
        json_object_object_add(o, "ESTIMATED-FINISH",
                               json_object_new_int64(etaTime));
        json_object_object_add(o, "REMAINING-SEC", json_object_new_int64(left));
      }
      json_object_object_add(o, "STARTED", json_object_new_int64(st.started));
      json_object_object_add(o, "UPDATED", json_object_new_int64(st.updated));
      json_object_object_add(o, "FINISHED", json_object_new_int64(st.finished));
      json_object_object_add(o, "ELAPSED", json_object_new_string(elapsed));
      json_object_object_add(o, "errCode", json_object_new_int(st.errCode));
      json_object_object_add(o, "errMsg", json_object_new_string(st.msg));
      json_object_object_add(json_obj, strcmp(volstate, "BUSY") ?
                             "LAST-OPERATION" : "OPERATION", o);
    }
    json_object_object_add(json_obj, "RESULT", GB_JSON_OBJ_TO_STR("SUCCESS"));
    GB_ASPRINTF(&reply->out, "%s\n",
                json_object_to_json_string_ext(json_obj,
                                     mapJsonFlagToJsonCstring(blk->json_resp)));
    json_object_put(json_obj);
  } else if (have) {
    GB_ASPRINTF(&reply->out,
                "VOLUME: %s\nSTATUS: %s\n%s:\n"
                "  OPERATION: %s\n  OPERATION-ID: %s\n  NAME: %s\n"
                "  GBID: %s\n  NODE: %s\n  STATE: %s\n  PHASE: %s\n"
                "%s%s%s%s%s%s"
                "  STARTED: %s\n  UPDATED: %s\n  FINISHED: %s\n"
                "  ELAPSED: %s\n%s%s%s",
                blk->volume, volstate,
                strcmp(volstate, "BUSY") ? "LAST OPERATION" : "OPERATION",
                st.op, st.id[0] ? st.id : "-", st.block,
                st.gbid[0] ? st.gbid : "-", st.node, state, phaseText,
                stepProgress ? "  PROGRESS: " : "",
                stepProgress ? stepProgress : "", stepProgress ? "\n" : "",
                progress ? "  ALLOCATED: " : "", progress ? progress : "",
                progress ? "\n" : "",
                started, updated, finished, elapsed,
                st.msg[0] ? "  ERROR: " : "", st.msg, st.msg[0] ? "\n" : "");
  } else {
    GB_ASPRINTF(&reply->out, "VOLUME: %s\nSTATUS: %s\n", blk->volume,
                volstate);
  }

  goto out;

 fail:
  blockFormatErrorResponse(STATUS_SRV, blk->json_resp,
                           errCode ? errCode : GB_DEFAULT_ERRCODE,
                           errMsg ? errMsg : GB_DEFAULT_ERRMSG, reply);

 out:
  if (lkfd && glfs_close(lkfd)) {
    LOG("mgmt", GB_LOG_ERROR, "glfs_close(%s) on volume %s failed[%s]",
        GB_TXLOCKFILE, blk->volume, strerror(errno));
  }
  GB_FREE(allocated);
  GB_FREE(total);
  GB_FREE(progress);
  GB_FREE(stepDoneStr);
  GB_FREE(stepTotalStr);
  GB_FREE(stepProgress);
  GB_FREE(errMsg);

  return reply;
}


static blockResponse *
block_status_cli_1_svc_st(blockStatusCli *blk, struct svc_req *rqstp)
{
  blockResponse *reply = NULL;

  if (GB_ALLOC(reply) < 0) {
    return NULL;
  }

  if (blk->op_id[0]) {
    return blockStatusOpId(blk, reply);
  }
  return blockStatusVolume(blk, reply);
}


bool_t
block_status_cli_1_svc(blockStatusCli *blk, blockResponse *reply,
                       struct svc_req *rqstp)
{
  int ret;

  GB_RPC_CALL(status_cli, blk, reply, rqstp, ret);
  return ret;
}
