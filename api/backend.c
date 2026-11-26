/*
  Copyright (c) 2026 gluster-block contributors
  This file is part of gluster-block.

  This file is licensed to you under your choice of the GNU Lesser
  General Public License, version 3 or any later version (LGPLv3 or
  later), or the GNU General Public License, version 2 (GPLv2), in all
  cases as published by the Free Software Foundation.
*/

/*
 * Calls of gluster-blockd: one connection per call on its unix socket, the
 * GLUSTER_BLOCK_CLI program the cli uses, always with a JSON answer.
 *
 * gluster-blockd serves the cli socket in one thread: calls of concurrent
 * HTTP requests wait for each other there, as concurrent cli commands do.
 */

# ifndef _GNU_SOURCE
#  define _GNU_SOURCE
# endif

# include  "config.h"
# include  "apid.h"
# include  "gb-client.h"
# include  "block.h"

# include  <errno.h>
# include  <pthread.h>
# include  <stdio.h>
# include  <stdlib.h>
# include  <string.h>
# include  <time.h>

# include  <microhttpd.h>


/* gluster-block's own exit codes (utils/common.h) */
# define  API_GB_CONFIRM_REQUIRED  228
# define  API_GB_DEFAULT_ERRCODE   255

# define  API_COMPAT_RECHECK_SEC   30


/* timeout of the rpcgen client stubs, every call sets its own */
struct timeval TIMEOUT = {API_DAEMON_TIMEOUT, 0};

static void apiDaemonRecheck(void);


void
apiCallFree(apiCall *c)
{
  free(c->out);
  c->out = NULL;
  if (c->json) {
    json_object_put(c->json);
    c->json = NULL;
  }
}


static void
apiCallParse(apiCall *c)
{
  enum json_tokener_error jerr = json_tokener_success;
  json_tokener *tok;


  if (!c->out || !*c->out) {
    return;
  }
  tok = json_tokener_new();
  if (!tok) {
    return;
  }
  c->json = json_tokener_parse_ex(tok, c->out, strlen(c->out));
  jerr = json_tokener_get_error(tok);
  if (jerr != json_tokener_success || !json_object_is_type(c->json, json_type_object)) {
    if (c->json) {
      json_object_put(c->json);
    }
    c->json = NULL;
  }
  json_tokener_free(tok);
}


/*
 * One call: @call is the rpcgen client stub, @obj its argument. The reply is
 * copied into @c, so it outlives the connection.
 */
# define API_RPC(c, call, obj, what)                                          \
  do {                                                                         \
    gbClient gbc_;                                                             \
    blockResponse reply_;                                                      \
    enum clnt_stat st_;                                                        \
                                                                               \
    memset((c), 0, sizeof(*(c)));                                              \
    memset(&reply_, 0, sizeof(reply_));                                        \
    if (gbClientConnect(&gbc_, apiConf.daemonSocket, apiConf.daemonTimeout,   \
                        (c)->err, sizeof((c)->err), &daemonDown_)) {           \
      (c)->rpc = -1;                                                           \
      (c)->daemonDown = daemonDown_;                                           \
      apiLog(API_LOG_WARNING, "%s: %s", what, (c)->err);                       \
      break;                                                                   \
    }                                                                          \
    st_ = call(obj, &reply_, gbc_.clnt);                                       \
    if (st_ != RPC_SUCCESS) {                                                  \
      (c)->rpc = st_;                                                          \
      snprintf((c)->err, sizeof((c)->err), "%s",                               \
               clnt_sperror(gbc_.clnt, what));                                 \
      apiLog(API_LOG_WARNING, "%s", (c)->err);                                 \
    } else {                                                                   \
      (c)->exit = reply_.exit;                                                 \
      (c)->out = strdup(reply_.out ? reply_.out : "");                         \
      apiCallParse(c);                                                         \
      clnt_freeres(gbc_.clnt, (xdrproc_t)xdr_blockResponse, (char *)&reply_);  \
    }                                                                          \
    gbClientClose(&gbc_);                                                      \
  } while (0)


static void
apiCopyName(char *dst, size_t len, const char *src)
{
  snprintf(dst, len, "%s", src ? src : "");
}


int
apiDaemonVersion(const char *volume, apiCall *c)
{
  blockVersionCli obj;
  int daemonDown_ = 0;


  memset(&obj, 0, sizeof(obj));
  apiCopyName(obj.volume, sizeof(obj.volume), volume);
  obj.json_resp = GB_JSON_PLAIN;
  API_RPC(c, block_version_cli_1, &obj, "version");
  return c->rpc;
}


int
apiDaemonVolumes(apiCall *c)
{
  blockVolumesCli obj;
  int daemonDown_ = 0;


  memset(&obj, 0, sizeof(obj));
  obj.json_resp = GB_JSON_PLAIN;
  API_RPC(c, block_volumes_cli_1, &obj, "volumes");
  return c->rpc;
}


int
apiDaemonList(const char *volume, apiCall *c)
{
  blockListCli obj;
  int daemonDown_ = 0;


  memset(&obj, 0, sizeof(obj));
  apiCopyName(obj.volume, sizeof(obj.volume), volume);
  obj.json_resp = GB_JSON_PLAIN;
  API_RPC(c, block_list_cli_1, &obj, "list");
  return c->rpc;
}


int
apiDaemonInfo(const char *volume, const char *block, apiCall *c)
{
  blockInfoCli obj;
  int daemonDown_ = 0;


  memset(&obj, 0, sizeof(obj));
  apiCopyName(obj.volume, sizeof(obj.volume), volume);
  apiCopyName(obj.block_name, sizeof(obj.block_name), block);
  obj.json_resp = GB_JSON_PLAIN;
  API_RPC(c, block_info_cli_1, &obj, "info");
  return c->rpc;
}


int
apiDaemonHosts(const char *volume, const char *host, apiCall *c)
{
  blockHostsCli obj;
  int daemonDown_ = 0;


  memset(&obj, 0, sizeof(obj));
  apiCopyName(obj.volume, sizeof(obj.volume), volume);
  apiCopyName(obj.host, sizeof(obj.host), host);
  obj.json_resp = GB_JSON_PLAIN;
  API_RPC(c, block_hosts_cli_1, &obj, "hosts");
  return c->rpc;
}


int
apiDaemonStatus(const char *volume, const char *opid, apiCall *c)
{
  blockStatusCli obj;
  int daemonDown_ = 0;


  memset(&obj, 0, sizeof(obj));
  apiCopyName(obj.volume, sizeof(obj.volume), volume);
  apiCopyName(obj.op_id, sizeof(obj.op_id), opid);
  obj.json_resp = GB_JSON_PLAIN;
  API_RPC(c, block_status_cli_1, &obj, "status");
  return c->rpc;
}


/*
 * Changes. @cmd goes into the cmdlog of gluster-blockd like the command line
 * of the cli: what was asked through the API and by whom.
 */

int
apiDaemonCreate(const apiCreateArgs *a, const char *cmd, apiCall *c)
{
  blockCreateCli obj;
  int daemonDown_ = 0;


  memset(&obj, 0, sizeof(obj));
  apiCopyName(obj.volume, sizeof(obj.volume), a->volume);
  apiCopyName(obj.block_name, sizeof(obj.block_name), a->block);
  apiCopyName(obj.storage, sizeof(obj.storage), a->storage);
  obj.block_hosts = (char *)a->hosts;
  obj.size = a->size;
  obj.mpath = a->ha;
  obj.auth_mode = a->auth;
  obj.prealloc = a->prealloc;
  obj.zerofill = a->zerofill;
  obj.thin_confirmed = a->confirmed;
  obj.rb_size = a->ringBuffer;
  obj.blk_size = a->blockSize;
  obj.io_timeout = a->ioTimeout;
  obj.async = true;
  obj.cmd = (char *)cmd;
  obj.json_resp = GB_JSON_PLAIN;
  API_RPC(c, block_create_cli_1, &obj, "create");
  return c->rpc;
}


int
apiDaemonDelete(const char *volume, const char *block, bool unlink,
                bool force, const char *cmd, apiCall *c)
{
  blockDeleteCli obj;
  int daemonDown_ = 0;


  memset(&obj, 0, sizeof(obj));
  apiCopyName(obj.volume, sizeof(obj.volume), volume);
  apiCopyName(obj.block_name, sizeof(obj.block_name), block);
  obj.unlink = unlink;
  obj.force = force;
  obj.cmd = (char *)cmd;
  obj.json_resp = GB_JSON_PLAIN;
  API_RPC(c, block_delete_async_cli_1, &obj, "delete");
  return c->rpc;
}


int
apiDaemonResize(const char *volume, const char *block, unsigned long long size,
                bool force, const char *cmd, apiCall *c)
{
  blockModifySizeCli obj;
  int daemonDown_ = 0;


  memset(&obj, 0, sizeof(obj));
  apiCopyName(obj.volume, sizeof(obj.volume), volume);
  apiCopyName(obj.block_name, sizeof(obj.block_name), block);
  obj.size = size;
  obj.force = force;
  obj.async = true;
  obj.cmd = (char *)cmd;
  obj.json_resp = GB_JSON_PLAIN;
  API_RPC(c, block_modify_size_cli_1, &obj, "modify size");
  return c->rpc;
}


int
apiDaemonAuth(const char *volume, const char *block, bool enable,
              const char *cmd, apiCall *c)
{
  blockModifyCli obj;
  int daemonDown_ = 0;


  memset(&obj, 0, sizeof(obj));
  apiCopyName(obj.volume, sizeof(obj.volume), volume);
  apiCopyName(obj.block_name, sizeof(obj.block_name), block);
  obj.auth_mode = enable;
  obj.cmd = (char *)cmd;
  obj.json_resp = GB_JSON_PLAIN;
  API_RPC(c, block_modify_cli_1, &obj, "modify auth");
  return c->rpc;
}


int
apiDaemonPrio(const char *volume, const char *block, const char *host,
              bool force, const char *cmd, apiCall *c)
{
  blockModifyPrioCli obj;
  int daemonDown_ = 0;


  memset(&obj, 0, sizeof(obj));
  apiCopyName(obj.volume, sizeof(obj.volume), volume);
  apiCopyName(obj.block_name, sizeof(obj.block_name), block);
  apiCopyName(obj.prio_path, sizeof(obj.prio_path), host);
  obj.force = force;
  obj.cmd = (char *)cmd;
  obj.json_resp = GB_JSON_PLAIN;
  API_RPC(c, block_modify_prio_cli_1, &obj, "modify prio-path");
  return c->rpc;
}


int
apiDaemonRebalance(const char *volume, bool dryRun, const char *cmd, apiCall *c)
{
  blockRebalanceCli obj;
  int daemonDown_ = 0;


  memset(&obj, 0, sizeof(obj));
  apiCopyName(obj.volume, sizeof(obj.volume), volume);
  obj.dry_run = dryRun;
  obj.async = true;
  obj.cmd = (char *)cmd;
  obj.json_resp = GB_JSON_PLAIN;
  API_RPC(c, block_rebalance_cli_1, &obj, "rebalance");
  return c->rpc;
}


int
apiDaemonMaint(const char *volume, const char *host, bool start, bool force,
               bool rebalance, bool confirmed, const char *cmd, apiCall *c)
{
  blockHostMaintCli obj;
  int daemonDown_ = 0;


  memset(&obj, 0, sizeof(obj));
  apiCopyName(obj.volume, sizeof(obj.volume), volume);
  apiCopyName(obj.host, sizeof(obj.host), host);
  obj.start = start;
  obj.force = force;
  obj.rebalance = rebalance;
  obj.confirmed = confirmed;
  obj.async = true;
  obj.cmd = (char *)cmd;
  obj.json_resp = GB_JSON_PLAIN;
  API_RPC(c, block_host_maint_cli_1, &obj, "maintenance");
  return c->rpc;
}


/* HTTP status and error code of a failed command */
static unsigned int
apiErrnoStatus(int err, const char **code)
{
  switch (err) {
  case ENOENT:
    *code = "not_found";
    return MHD_HTTP_NOT_FOUND;
  case EBUSY:
    *code = "volume_busy";
    return MHD_HTTP_LOCKED;
  case EEXIST:
    *code = "already_exists";
    return MHD_HTTP_CONFLICT;
  case API_GB_CONFIRM_REQUIRED:
    *code = "confirm_required";
    return MHD_HTTP_CONFLICT;
  case EINVAL:
  case ERANGE:
  case ENAMETOOLONG:
    *code = "invalid_request";
    return MHD_HTTP_BAD_REQUEST;
  case EPERM:
  case EACCES:
    *code = "not_permitted";
    return MHD_HTTP_FORBIDDEN;
  case ENOSPC:
    *code = "no_space";
    return MHD_HTTP_INSUFFICIENT_STORAGE;
  case ENOTCONN:
  case EHOSTUNREACH:
  case EHOSTDOWN:
  case ECONNREFUSED:
  case ETIMEDOUT:
    *code = "host_unreachable";
    return MHD_HTTP_BAD_GATEWAY;
  default:
    *code = "operation_failed";
    return MHD_HTTP_INTERNAL_SERVER_ERROR;
  }
}


bool
apiCallFailed(apiCall *c, apiResponse *resp)
{
  const char *code = NULL;
  const char *msg = NULL;
  unsigned int status;
  int err;


  if (c->rpc) {
    apiDaemonRecheck();
  }
  if (c->rpc == -1) {
    apiError(resp, MHD_HTTP_SERVICE_UNAVAILABLE, "daemon_unavailable", 0,
             "gluster-blockd does not answer: %s", c->err);
    return true;
  }
  if (c->rpc == RPC_PROCUNAVAIL) {
    apiError(resp, MHD_HTTP_SERVICE_UNAVAILABLE, "daemon_incompatible", 0,
             "gluster-blockd is older than gluster-block-apid %s, restart "
             "or upgrade gluster-blockd", PACKAGE_VERSION);
    return true;
  }
  if (c->rpc == RPC_TIMEDOUT) {
    apiError(resp, MHD_HTTP_GATEWAY_TIMEOUT, "daemon_timeout", 0,
             "gluster-blockd did not answer within %u seconds",
             apiConf.daemonTimeout);
    return true;
  }
  if (c->rpc) {
    apiError(resp, MHD_HTTP_BAD_GATEWAY, "daemon_error", 0, "%s", c->err);
    return true;
  }
  if (!c->exit) {
    return false;
  }

  err = c->exit;
  if (c->json) {
    msg = apiJsonStr(c->json, "errMsg");
    err = (int)apiJsonInt(c->json, "errCode", err);
  }
  if (!msg || !*msg) {
    msg = c->out && *c->out ? c->out : "the operation failed, see the logs "
          "of gluster-blockd";
  }
  status = apiErrnoStatus(err, &code);
  apiError(resp, status, code, err == API_GB_DEFAULT_ERRCODE ? 0 : err, "%s", msg);
  /* strip the trailing newline of plain output */
  if (resp->body) {
    json_object *je = apiJsonGet(resp->body, "error");
    const char *m = apiJsonStr(je, "message");
    size_t n = m ? strlen(m) : 0;

    if (n && m[n - 1] == '\n') {
      char *copy = strndup(m, n - 1);

      if (copy) {
        json_object_object_add(je, "message", json_object_new_string(copy));
        free(copy);
      }
    }
  }
  return true;
}


/* ------------------------------------------------------------------------
 * compatibility of gluster-blockd, checked at start and again on failures
 * ------------------------------------------------------------------------ */

static pthread_mutex_t apiCompatLock = PTHREAD_MUTEX_INITIALIZER;
static time_t apiCompatAt;
static bool   apiCompatOk;
static char   apiCompatWhy[512];


/* a call found no daemon: ask again before the next request */
static void
apiDaemonRecheck(void)
{
  pthread_mutex_lock(&apiCompatLock);
  apiCompatOk = false;
  pthread_mutex_unlock(&apiCompatLock);
}


bool
apiDaemonCompatible(apiResponse *resp, bool force)
{
  apiCall c;
  time_t now = time(NULL);
  bool ok;
  long long rev;


  pthread_mutex_lock(&apiCompatLock);
  if (!force && apiCompatOk && now - apiCompatAt < API_COMPAT_RECHECK_SEC) {
    pthread_mutex_unlock(&apiCompatLock);
    return true;
  }
  pthread_mutex_unlock(&apiCompatLock);

  apiDaemonVersion(NULL, &c);
  if (apiCallFailed(&c, resp)) {
    apiCallFree(&c);
    pthread_mutex_lock(&apiCompatLock);
    apiCompatOk = false;
    pthread_mutex_unlock(&apiCompatLock);
    return false;
  }

  rev = apiJsonInt(c.json, "CLI-RPC-REV", 0);
  ok = rev >= GB_CLI_RPC_REV;
  pthread_mutex_lock(&apiCompatLock);
  apiCompatOk = ok;
  apiCompatAt = now;
  if (!ok) {
    snprintf(apiCompatWhy, sizeof(apiCompatWhy),
             "gluster-blockd %s has cli RPC revision %lld, gluster-block-apid "
             "%s needs %d: restart or upgrade gluster-blockd",
             apiJsonStr(c.json, "VERSION") ? apiJsonStr(c.json, "VERSION") : "?",
             rev, PACKAGE_VERSION, GB_CLI_RPC_REV);
    apiError(resp, MHD_HTTP_SERVICE_UNAVAILABLE, "daemon_incompatible", 0,
             "%s", apiCompatWhy);
    apiLog(API_LOG_ERROR, "%s", apiCompatWhy);
  }
  pthread_mutex_unlock(&apiCompatLock);
  apiCallFree(&c);

  return ok;
}


/* ------------------------------------------------------------------------
 * volume list, the most expensive call (one glfs per volume)
 * ------------------------------------------------------------------------ */

static pthread_mutex_t apiVolLock = PTHREAD_MUTEX_INITIALIZER;
static char  *apiVolText;
static time_t apiVolAt;


void
apiVolumesInvalidate(void)
{
  pthread_mutex_lock(&apiVolLock);
  free(apiVolText);
  apiVolText = NULL;
  pthread_mutex_unlock(&apiVolLock);
}


json_object *
apiVolumesCached(apiResponse *resp, bool refresh)
{
  json_object *jo = NULL;
  apiCall c;
  time_t now = time(NULL);


  /* keep the text, every request parses its own objects */
  pthread_mutex_lock(&apiVolLock);
  if (!refresh && apiVolText && now - apiVolAt < API_VOLUMES_TTL) {
    jo = json_tokener_parse(apiVolText);
  }
  pthread_mutex_unlock(&apiVolLock);
  if (jo) {
    return jo;
  }

  apiDaemonVolumes(&c);
  if (apiCallFailed(&c, resp)) {
    apiCallFree(&c);
    return NULL;
  }
  if (!c.json) {
    apiError(resp, MHD_HTTP_BAD_GATEWAY, "daemon_error", 0,
             "gluster-blockd answered the volume list without JSON");
    apiCallFree(&c);
    return NULL;
  }

  pthread_mutex_lock(&apiVolLock);
  free(apiVolText);
  apiVolText = strdup(c.out);
  apiVolAt = now;
  pthread_mutex_unlock(&apiVolLock);

  jo = c.json;
  c.json = NULL;
  apiCallFree(&c);
  return jo;
}
