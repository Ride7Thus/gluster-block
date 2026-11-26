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


# define  API_VERSION          "1.0"
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


typedef struct apiConfig {
  char         socket[108];      /* unix socket of the API */
  char         daemonSocket[108];
  char         logDir[256];
  apiLogLevel  logLevel;
  unsigned int daemonTimeout;
  bool         logStderr;
} apiConfig;

extern apiConfig apiConf;


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
} apiRequest;

typedef struct apiResponse {
  unsigned int status;
  json_object *body;                  /* owned, may be NULL (204) */
  const char  *allow;                 /* Allow header of a 405 */
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
