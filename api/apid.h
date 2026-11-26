/*
  Copyright (c) 2026 gluster-block contributors
  This file is part of gluster-block.

  This file is licensed to you under your choice of the GNU Lesser
  General Public License, version 3 or any later version (LGPLv3 or
  later), or the GNU General Public License, version 2 (GPLv2), in all
  cases as published by the Free Software Foundation.
*/

/*
 * gluster-block-apid: HTTP API of gluster-block (docs/api/openapi.yaml).
 *
 * A bridge between HTTP clients (the Cockpit plugin, operators, scripts) and
 * the local gluster-blockd: every request is one or a few calls of the
 * GLUSTER_BLOCK_CLI RPC program on the unix socket of the daemon, the same
 * the gluster-block cli makes. The JSON of the daemon is turned into the
 * stable JSON of API v1, the daemon does all the work and the checks.
 */

# ifndef _APID_H
# define _APID_H 1

# include  <stdbool.h>
# include  <stddef.h>
# include  <json-c/json.h>


# define  API_VERSION          "1.1"
# define  API_PREFIX           "/api/v1"

# define  API_SOCKET_DEF       "/run/gluster-block/api.sock"
# define  API_LOGFILE          "gluster-block-apid.log"
# define  API_DAEMON_TIMEOUT   120        /* seconds for one daemon call */
# define  API_VOLUMES_TTL      15         /* seconds the volume list is kept */
# define  API_THREADS          8


typedef enum apiLogLevel {
  API_LOG_NONE = 0,
  API_LOG_ERROR,
  API_LOG_WARNING,
  API_LOG_INFO,
  API_LOG_DEBUG,
} apiLogLevel;


# define  API_BODY_MAX         65536      /* bytes of a request body */
# define  GB_API_TOKENS_DEF    "/etc/gluster-block/api-tokens"


typedef struct apiConfig {
  char         socket[108];      /* unix socket of the API */
  char         daemonSocket[108];
  char         logDir[256];
  apiLogLevel  logLevel;
  unsigned int daemonTimeout;
  bool         logStderr;

  /* TCP listener, TLS only, off when listen is empty */
  char         listen[128];      /* host:port, [::]:port, *:port */
  char         tlsCert[256];
  char         tlsKey[256];
  char         tlsCa[256];       /* client certificates (mutual TLS) */
  char         tokensFile[256];
} apiConfig;

extern apiConfig apiConf;


typedef enum apiRole {
  API_ROLE_NONE = 0,
  API_ROLE_READ,                 /* GET only */
  API_ROLE_ADMIN,
} apiRole;


void apiLog(apiLogLevel level, const char *fmt, ...)
  __attribute__((format(printf, 2, 3)));


/* ------------------------------------------------------------------------
 * HTTP request and response (apid.c)
 * ------------------------------------------------------------------------ */

# define  API_MAX_SEGMENTS     8

typedef struct apiRequest {
  const char *method;
  const char *url;
  char       *seg[API_MAX_SEGMENTS];  /* path below API_PREFIX, split on '/' */
  size_t      nseg;
  void       *conn;                   /* struct MHD_Connection */
  const char *identity;               /* who asks, for the logs */
  apiRole     role;
  json_object *body;                  /* JSON request body, NULL if none */
} apiRequest;

typedef struct apiResponse {
  unsigned int status;
  json_object *body;                  /* owned, may be NULL (204) */
  const char  *allow;                 /* Allow header of a 405 */
  char         location[512];         /* Location header of a 201/202 */
} apiResponse;

/* query argument, NULL when it is not given */
const char *apiQuery(apiRequest *req, const char *key);

/* query argument as boolean: true, 1, yes */
bool apiQueryBool(apiRequest *req, const char *key);

void apiRespond(apiResponse *resp, unsigned int status, json_object *body);

/* {"error": {"code", "message", "errno"}} */
void apiError(apiResponse *resp, unsigned int status, const char *code,
              int err, const char *fmt, ...)
  __attribute__((format(printf, 5, 6)));


/* ------------------------------------------------------------------------
 * clients of the TLS listener (auth.c)
 * ------------------------------------------------------------------------ */

/* (re)read the tokens file: -1 when lines were ignored */
int apiAuthLoad(const char *path);

/* certificate or bearer token of the connection: true with @who and @role */
struct MHD_Connection;
bool apiAuthenticate(struct MHD_Connection *conn, char *who, size_t len,
                     apiRole *role);

const char *apiRoleName(apiRole role);

/* --new-token: print a new token and its line for the tokens file */
int apiNewToken(const char *name, const char *role);


/* ------------------------------------------------------------------------
 * gluster-blockd (backend.c)
 * ------------------------------------------------------------------------ */

typedef struct apiCall {
  int    rpc;            /* 0: answered, -1: no connection, else clnt_stat */
  bool   daemonDown;     /* nothing listens on the socket */
  int    exit;           /* exit code of the command */
  char  *out;            /* its output */
  char   err[512];       /* why there is no answer */
  json_object *json;     /* the output parsed, when it is JSON */
} apiCall;

void apiCallFree(apiCall *c);

int apiDaemonVersion(const char *volume, apiCall *c);
int apiDaemonVolumes(apiCall *c);
int apiDaemonList(const char *volume, apiCall *c);
int apiDaemonInfo(const char *volume, const char *block, apiCall *c);
int apiDaemonHosts(const char *volume, const char *host, apiCall *c);
int apiDaemonStatus(const char *volume, const char *opid, apiCall *c);

typedef struct apiCreateArgs {
  const char  *volume;
  const char  *block;
  const char  *hosts;            /* comma separated */
  unsigned long long size;
  unsigned int ha;
  bool         auth;
  bool         prealloc;
  bool         zerofill;
  bool         confirmed;        /* thin block on a dispersed volume */
  unsigned int ringBuffer;
  unsigned int blockSize;
  unsigned int ioTimeout;
  const char  *storage;
} apiCreateArgs;

int apiDaemonCreate(const apiCreateArgs *a, const char *cmd, apiCall *c);
int apiDaemonDelete(const char *volume, const char *block, bool unlink,
                    bool force, const char *cmd, apiCall *c);
int apiDaemonResize(const char *volume, const char *block,
                    unsigned long long size, bool force, const char *cmd,
                    apiCall *c);
int apiDaemonAuth(const char *volume, const char *block, bool enable,
                  const char *cmd, apiCall *c);
int apiDaemonPrio(const char *volume, const char *block, const char *host,
                  bool force, const char *cmd, apiCall *c);
int apiDaemonRebalance(const char *volume, bool dryRun, const char *cmd,
                       apiCall *c);
int apiDaemonMaint(const char *volume, const char *host, bool start,
                   bool force, bool rebalance, bool confirmed,
                   const char *cmd, apiCall *c);

/* the volume list changed (a first block, the last one deleted) */
void apiVolumesInvalidate(void);

/*
 * Turn a failed call into an error response: no daemon (503), daemon too
 * old (503), the command failed (HTTP status from its errno). Returns true
 * when @c failed and @resp is set.
 */
bool apiCallFailed(apiCall *c, apiResponse *resp);

/* the daemon answers and is not older than this apid (cached unless @force) */
bool apiDaemonCompatible(apiResponse *resp, bool force);

/* the volume list of the daemon, cached API_VOLUMES_TTL seconds; a new
 * reference, or NULL with @resp set */
json_object *apiVolumesCached(apiResponse *resp, bool refresh);


/* ------------------------------------------------------------------------
 * API v1 (v1.c)
 * ------------------------------------------------------------------------ */

void apiV1Handle(apiRequest *req, apiResponse *resp);

/* JSON helpers */
const char *apiJsonStr(json_object *o, const char *key);
long long apiJsonInt(json_object *o, const char *key, long long def);
json_object *apiJsonGet(json_object *o, const char *key);
json_object *apiStrOrNull(const char *s);
char *apiLower(const char *s, char *buf, size_t len);

# endif /* _APID_H */
