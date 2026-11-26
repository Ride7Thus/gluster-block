/*
  Copyright (c) 2026 gluster-block contributors
  This file is part of gluster-block.

  This file is licensed to you under your choice of the GNU Lesser
  General Public License, version 3 or any later version (LGPLv3 or
  later), or the GNU General Public License, version 2 (GPLv2), in all
  cases as published by the Free Software Foundation.
*/

/*
 * gluster-block-apid: process, configuration, logging and the HTTP servers.
 *
 * - the unix socket (0600, root) is always there: the Cockpit bridge runs as
 *   root with superuser and connects to it, its clients are admins;
 * - the TCP listener (GB_API_LISTEN) is off by default and TLS only; its
 *   clients show a certificate signed by GB_API_TLS_CA or a bearer token,
 *   GB_API_TOKENS_FILE gives them a role (auth.c).
 *
 * Every request that changes something is logged as AUDIT with who sent it.
 */

# ifndef _GNU_SOURCE
#  define _GNU_SOURCE
# endif

# include  "config.h"
# include  "apid.h"

# include  <errno.h>
# include  <fcntl.h>
# include  <getopt.h>
# include  <libgen.h>
# include  <limits.h>
# include  <netdb.h>
# include  <pthread.h>
# include  <signal.h>
# include  <stdarg.h>
# include  <stdio.h>
# include  <stdlib.h>
# include  <string.h>
# include  <strings.h>
# include  <sys/socket.h>
# include  <sys/stat.h>
# include  <sys/time.h>
# include  <sys/un.h>
# include  <netinet/in.h>
# include  <time.h>
# include  <unistd.h>

# include  <microhttpd.h>

# if MHD_VERSION >= 0x00097002
typedef enum MHD_Result apiMhdResult;
# else
typedef int apiMhdResult;
# endif


apiConfig apiConf = {
  .socket        = API_SOCKET_DEF,
  .daemonSocket  = GB_DAEMON_SOCKET,
  .logDir        = GB_LOGDIR_DEF,
  .logLevel      = API_LOG_INFO,
  .daemonTimeout = API_DAEMON_TIMEOUT,
  .logStderr     = false,
  .tokensFile    = GB_API_TOKENS_DEF,
};

static FILE *apiLogFile;
static pthread_mutex_t apiLogLock = PTHREAD_MUTEX_INITIALIZER;

static const char *const apiLogNames[] = {
  [API_LOG_NONE]    = "NONE",
  [API_LOG_ERROR]   = "ERROR",
  [API_LOG_WARNING] = "WARNING",
  [API_LOG_INFO]    = "INFO",
  [API_LOG_DEBUG]   = "DEBUG",
};


/* which listener a request came in on */
typedef struct apiListener {
  bool tls;
} apiListener;

static apiListener apiUnixListener = {false};
static apiListener apiTlsListener = {true};


void
apiLog(apiLogLevel level, const char *fmt, ...)
{
  char ts[64];
  struct timeval tv;
  struct tm tm;
  va_list ap;
  FILE *f;


  if (level > apiConf.logLevel || level == API_LOG_NONE) {
    return;
  }
  gettimeofday(&tv, NULL);
  gmtime_r(&tv.tv_sec, &tm);
  strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm);

  pthread_mutex_lock(&apiLogLock);
  f = apiLogFile ? apiLogFile : stderr;
  fprintf(f, "[%s.%06ld] %s: ", ts, (long)tv.tv_usec, apiLogNames[level]);
  va_start(ap, fmt);
  vfprintf(f, fmt, ap);
  va_end(ap);
  fputc('\n', f);
  fflush(f);
  if (apiConf.logStderr && f != stderr) {
    fprintf(stderr, "%s: ", apiLogNames[level]);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
  }
  pthread_mutex_unlock(&apiLogLock);
}


static int
apiLogLevelParse(const char *s, apiLogLevel *out)
{
  size_t i;


  for (i = 0; i < sizeof(apiLogNames) / sizeof(apiLogNames[0]); i++) {
    if (!strcasecmp(s, apiLogNames[i])) {
      *out = i;
      return 0;
    }
  }
  /* the levels of gluster-blockd */
  if (!strcasecmp(s, "CRIT")) {
    *out = API_LOG_ERROR;
    return 0;
  }
  if (!strcasecmp(s, "TRACE")) {
    *out = API_LOG_DEBUG;
    return 0;
  }
  return -1;
}


static void
apiCopy(char *dst, size_t len, const char *src)
{
  snprintf(dst, len, "%s", src);
}


/*
 * /etc/sysconfig/gluster-block-apid: KEY=VALUE lines, as systemd reads it
 * with EnvironmentFile. Variables already in the environment win.
 */
static void
apiLoadSysconfig(const char *path)
{
  FILE *f = fopen(path, "r");
  char line[1024];


  if (!f) {
    return;
  }
  while (fgets(line, sizeof(line), f)) {
    char *key = line, *val, *end;

    while (*key == ' ' || *key == '\t') {
      key++;
    }
    if (*key == '#' || *key == '\n' || !*key) {
      continue;
    }
    val = strchr(key, '=');
    if (!val) {
      continue;
    }
    *val++ = '\0';
    end = val + strlen(val);
    while (end > val && (end[-1] == '\n' || end[-1] == ' ' || end[-1] == '\t')) {
      *--end = '\0';
    }
    if ((*val == '"' || *val == '\'') && end > val + 1 && end[-1] == *val) {
      end[-1] = '\0';
      val++;
    }
    setenv(key, val, 0);
  }
  fclose(f);
}


static void
apiEnvCopy(const char *name, char *dst, size_t len)
{
  const char *v = getenv(name);


  if (v && *v) {
    apiCopy(dst, len, v);
  }
}


static void
apiConfigFromEnv(void)
{
  const char *v;


  apiEnvCopy("GB_API_SOCKET", apiConf.socket, sizeof(apiConf.socket));
  if ((v = getenv("GB_API_LOG_LEVEL")) && *v &&
      apiLogLevelParse(v, &apiConf.logLevel)) {
    fprintf(stderr, "gluster-block-apid: unknown GB_API_LOG_LEVEL '%s'\n", v);
  }
  apiEnvCopy("GB_API_LOG_DIR", apiConf.logDir, sizeof(apiConf.logDir));
  if ((v = getenv("GB_API_DAEMON_TIMEOUT")) && *v) {
    unsigned long t = strtoul(v, NULL, 10);

    if (t > 0 && t < 86400) {
      apiConf.daemonTimeout = t;
    }
  }
  apiEnvCopy("GB_API_LISTEN", apiConf.listen, sizeof(apiConf.listen));
  apiEnvCopy("GB_API_TLS_CERT", apiConf.tlsCert, sizeof(apiConf.tlsCert));
  apiEnvCopy("GB_API_TLS_KEY", apiConf.tlsKey, sizeof(apiConf.tlsKey));
  apiEnvCopy("GB_API_TLS_CA", apiConf.tlsCa, sizeof(apiConf.tlsCa));
  apiEnvCopy("GB_API_TOKENS_FILE", apiConf.tokensFile, sizeof(apiConf.tokensFile));
}


static void
apiUsage(FILE *f)
{
  fprintf(f,
    "gluster-block-apid (%s): HTTP API of gluster-block\n"
    "usage:\n"
    "  gluster-block-apid [--socket <path>] [--log-level <level>] [--log-stderr]\n"
    "  gluster-block-apid --new-token <name> <admin|read>\n"
    "\n"
    "  --socket <path>\n"
    "        unix socket of the API [default: %s, GB_API_SOCKET]\n"
    "  --log-level <NONE|ERROR|WARNING|INFO|DEBUG>\n"
    "        [default: INFO, GB_API_LOG_LEVEL]\n"
    "  --log-stderr\n"
    "        also log to stderr\n"
    "  --new-token <name> <admin|read>\n"
    "        print a new bearer token for the TLS listener and the line that\n"
    "        gives it its role in %s\n"
    "  --help, --version\n"
    "\n"
    "Settings are read from %s.\n",
    PACKAGE_VERSION, API_SOCKET_DEF, GB_API_TOKENS_DEF, GB_API_SYSCONFIG);
}


/* (re)open the log file, stderr when it can not be opened */
static void
apiOpenLog(void)
{
  char path[PATH_MAX];
  FILE *f = NULL, *old;


  if (mkdir(apiConf.logDir, 0755) && errno != EEXIST) {
    fprintf(stderr, "gluster-block-apid: mkdir(%s) failed: %s, logging to "
            "stderr\n", apiConf.logDir, strerror(errno));
  } else {
    snprintf(path, sizeof(path), "%s/%s", apiConf.logDir, API_LOGFILE);
    f = fopen(path, "a");
    if (!f) {
      fprintf(stderr, "gluster-block-apid: open(%s) failed: %s, logging to "
              "stderr\n", path, strerror(errno));
    }
  }

  pthread_mutex_lock(&apiLogLock);
  old = apiLogFile;
  apiLogFile = f;
  pthread_mutex_unlock(&apiLogLock);
  if (old) {
    fclose(old);
  }
}


/* ------------------------------------------------------------------------
 * HTTP
 * ------------------------------------------------------------------------ */

const char *
apiQuery(apiRequest *req, const char *key)
{
  return MHD_lookup_connection_value(req->conn, MHD_GET_ARGUMENT_KIND, key);
}


bool
apiQueryBool(apiRequest *req, const char *key)
{
  const char *v = apiQuery(req, key);


  /* "?detail" alone counts as true */
  return v && (!*v || !strcasecmp(v, "true") || !strcmp(v, "1") ||
               !strcasecmp(v, "yes"));
}


void
apiRespond(apiResponse *resp, unsigned int status, json_object *body)
{
  if (resp->body) {
    json_object_put(resp->body);
  }
  resp->status = status;
  resp->body = body;
}


void
apiError(apiResponse *resp, unsigned int status, const char *code, int err,
         const char *fmt, ...)
{
  json_object *jo = json_object_new_object();
  json_object *je = json_object_new_object();
  char *msg = NULL;
  va_list ap;


  va_start(ap, fmt);
  if (vasprintf(&msg, fmt, ap) < 0) {
    msg = NULL;
  }
  va_end(ap);

  json_object_object_add(je, "code", json_object_new_string(code));
  json_object_object_add(je, "message", json_object_new_string(msg ? msg : code));
  if (err) {
    json_object_object_add(je, "errno", json_object_new_int(err));
  }
  json_object_object_add(jo, "error", je);
  free(msg);

  apiRespond(resp, status, jo);
}


/* uid and pid of the peer of a unix socket connection */
static void
apiPeer(struct MHD_Connection *conn, char *buf, size_t len)
{
  const union MHD_ConnectionInfo *ci;
  struct ucred cred;
  socklen_t clen = sizeof(cred);


  snprintf(buf, len, "unix");
  ci = MHD_get_connection_info(conn, MHD_CONNECTION_INFO_CONNECTION_FD);
  if (ci && !getsockopt(ci->connect_fd, SOL_SOCKET, SO_PEERCRED, &cred, &clen)) {
    snprintf(buf, len, "unix:uid=%u,pid=%d", (unsigned)cred.uid, (int)cred.pid);
  }
}


/* address of a TCP peer, for the logs */
static void
apiTcpPeer(struct MHD_Connection *conn, char *buf, size_t len)
{
  const union MHD_ConnectionInfo *ci;
  char host[64] = "?";     /* numeric addresses only */
  socklen_t slen;


  ci = MHD_get_connection_info(conn, MHD_CONNECTION_INFO_CLIENT_ADDRESS);
  if (ci && ci->client_addr) {
    slen = ci->client_addr->sa_family == AF_INET6 ?
           sizeof(struct sockaddr_in6) : sizeof(struct sockaddr_in);
    getnameinfo(ci->client_addr, slen, host, sizeof(host), NULL, 0,
                NI_NUMERICHOST);
  }
  snprintf(buf, len, "%s", host);
}


static int
apiSplitPath(apiRequest *req, char *path)
{
  char *save = NULL, *tok;


  req->nseg = 0;
  for (tok = strtok_r(path, "/", &save); tok; tok = strtok_r(NULL, "/", &save)) {
    if (req->nseg == API_MAX_SEGMENTS) {
      return -1;
    }
    req->seg[req->nseg++] = tok;
  }
  return 0;
}


typedef struct apiConnState {
  struct timeval start;
  char          *body;
  size_t         len;
  bool           tooBig;
} apiConnState;


static bool
apiIsRead(const char *method)
{
  return !strcmp(method, "GET") || !strcmp(method, "HEAD");
}


/* who may do what: the unix socket is root only, TCP needs a client */
static bool
apiAuthorize(apiListener *l, struct MHD_Connection *conn, apiRequest *req,
             char *who, size_t len, apiResponse *resp, bool *challenge)
{
  char peer[64];
  char id[300];


  *challenge = false;
  if (!l->tls) {
    apiPeer(conn, who, len);
    req->role = API_ROLE_ADMIN;
    return true;
  }

  apiTcpPeer(conn, peer, sizeof(peer));
  if (!apiAuthenticate(conn, id, sizeof(id), &req->role)) {
    snprintf(who, len, "%s@%s", id, peer);
    /* a load balancer may ask whether the node serves without a client */
    if (!strcmp(req->url, "/healthz") && apiIsRead(req->method)) {
      req->role = API_ROLE_READ;
      return true;
    }
    *challenge = true;
    apiError(resp, MHD_HTTP_UNAUTHORIZED, "unauthorized", 0,
             "a client certificate or a bearer token with a role in %s is "
             "needed", apiConf.tokensFile);
    return false;
  }
  snprintf(who, len, "%s@%s", id, peer);
  if (req->role < API_ROLE_ADMIN && !apiIsRead(req->method)) {
    apiError(resp, MHD_HTTP_FORBIDDEN, "forbidden", 0,
             "%s has the role %s, %s needs admin", id,
             apiRoleName(req->role), req->method);
    return false;
  }
  return true;
}


static apiMhdResult
apiHandler(void *cls, struct MHD_Connection *conn, const char *url,
           const char *method, const char *version, const char *upload_data,
           size_t *upload_data_size, void **con_cls)
{
  apiListener *l = cls;
  apiConnState *cs = *con_cls;
  apiRequest req;
  apiResponse resp;
  struct MHD_Response *mr;
  struct timeval end;
  const char *text;
  char *path = NULL;
  char who[384];
  bool challenge = false;
  apiMhdResult ret;
  long ms;


  if (!cs) {
    /* first call: headers only */
    cs = calloc(1, sizeof(*cs));
    if (!cs) {
      return MHD_NO;
    }
    gettimeofday(&cs->start, NULL);
    *con_cls = cs;
    return MHD_YES;
  }
  if (*upload_data_size) {
    if (!cs->tooBig) {
      if (cs->len + *upload_data_size > API_BODY_MAX) {
        cs->tooBig = true;
        free(cs->body);
        cs->body = NULL;
        cs->len = 0;
      } else {
        char *b = realloc(cs->body, cs->len + *upload_data_size + 1);

        if (!b) {
          return MHD_NO;
        }
        memcpy(b + cs->len, upload_data, *upload_data_size);
        cs->len += *upload_data_size;
        b[cs->len] = '\0';
        cs->body = b;
      }
    }
    *upload_data_size = 0;
    return MHD_YES;
  }

  memset(&req, 0, sizeof(req));
  memset(&resp, 0, sizeof(resp));
  req.method = method;
  req.url = url;
  req.conn = conn;

  if (!apiAuthorize(l, conn, &req, who, sizeof(who), &resp, &challenge)) {
    goto send;
  }
  req.identity = who;

  if (cs->tooBig) {
    apiError(&resp, MHD_HTTP_PAYLOAD_TOO_LARGE, "invalid_request", 0,
             "request body larger than %d bytes", API_BODY_MAX);
    goto send;
  }
  if (cs->len) {
    enum json_tokener_error jerr;
    json_tokener *tok = json_tokener_new();

    req.body = tok ? json_tokener_parse_ex(tok, cs->body, cs->len) : NULL;
    jerr = tok ? json_tokener_get_error(tok) : json_tokener_error_parse_eof;
    if (tok) {
      json_tokener_free(tok);
    }
    if (jerr != json_tokener_success || !req.body ||
        !json_object_is_type(req.body, json_type_object)) {
      if (req.body) {
        json_object_put(req.body);
        req.body = NULL;
      }
      apiError(&resp, MHD_HTTP_BAD_REQUEST, "invalid_request", 0,
               "the request body must be a JSON object");
      goto send;
    }
  }

  if (!strcmp(url, "/healthz")) {
    path = strdup("healthz");
  } else if (!strncmp(url, API_PREFIX, strlen(API_PREFIX)) &&
             (url[strlen(API_PREFIX)] == '/' || !url[strlen(API_PREFIX)])) {
    path = strdup(url + strlen(API_PREFIX));
  }

  if (!path) {
    apiError(&resp, MHD_HTTP_NOT_FOUND, "not_found", 0,
             "no such resource: %s (the API is under %s)", url, API_PREFIX);
  } else if (apiSplitPath(&req, path)) {
    apiError(&resp, MHD_HTTP_NOT_FOUND, "not_found", 0, "no such resource: %s", url);
  } else {
    apiV1Handle(&req, &resp);
  }
  free(path);

 send:
  if (resp.body) {
    text = json_object_to_json_string_ext(resp.body, JSON_C_TO_STRING_PLAIN);
    mr = MHD_create_response_from_buffer(strlen(text), (void *)text,
                                         MHD_RESPMEM_MUST_COPY);
  } else {
    mr = MHD_create_response_from_buffer(0, (void *)"", MHD_RESPMEM_PERSISTENT);
  }
  if (!mr) {
    json_object_put(resp.body);
    json_object_put(req.body);
    return MHD_NO;
  }
  if (resp.body) {
    MHD_add_response_header(mr, MHD_HTTP_HEADER_CONTENT_TYPE, "application/json");
  }
  MHD_add_response_header(mr, MHD_HTTP_HEADER_CACHE_CONTROL, "no-store");
  if (resp.allow) {
    MHD_add_response_header(mr, MHD_HTTP_HEADER_ALLOW, resp.allow);
  }
  if (resp.location[0]) {
    MHD_add_response_header(mr, MHD_HTTP_HEADER_LOCATION, resp.location);
  }
  if (challenge) {
    MHD_add_response_header(mr, MHD_HTTP_HEADER_WWW_AUTHENTICATE,
                            "Bearer realm=\"gluster-block\"");
  }
  ret = MHD_queue_response(conn, resp.status ? resp.status : 500, mr);
  MHD_destroy_response(mr);

  gettimeofday(&end, NULL);
  ms = (end.tv_sec - cs->start.tv_sec) * 1000 +
       (end.tv_usec - cs->start.tv_usec) / 1000;
  if (!apiIsRead(method)) {
    /* what changed and who asked */
    apiLog(API_LOG_INFO, "AUDIT %s %s %s %s -> %u %ldms",
           req.identity ? req.identity : who, method, url,
           req.body ? json_object_to_json_string_ext(req.body, JSON_C_TO_STRING_PLAIN) : "-",
           resp.status, ms);
  } else {
    apiLog(resp.status >= 500 ? API_LOG_WARNING : API_LOG_INFO,
           "%s %s %s %u %ldms", req.identity ? req.identity : who, method, url,
           resp.status, ms);
  }

  json_object_put(resp.body);
  json_object_put(req.body);
  return ret;
}


static void
apiCompleted(void *cls, struct MHD_Connection *conn, void **con_cls,
             enum MHD_RequestTerminationCode toe)
{
  apiConnState *cs = *con_cls;


  if (cs) {
    free(cs->body);
    free(cs);
  }
  *con_cls = NULL;
}


static void
apiMhdLog(void *cls, const char *fmt, va_list ap)
{
  char buf[1024];


  vsnprintf(buf, sizeof(buf), fmt, ap);
  buf[strcspn(buf, "\n")] = '\0';
  /* TCP options and buffer pushing do not apply to the unix socket */
  if (strstr(buf, "TCP_NODELAY") || strstr(buf, "TCP_CORK") ||
      strstr(buf, "TCP_NOPUSH") || strstr(buf, "Failed to push the data")) {
    apiLog(API_LOG_DEBUG, "libmicrohttpd: %s", buf);
    return;
  }
  apiLog(API_LOG_WARNING, "libmicrohttpd: %s", buf);
}


static int
apiListenUnix(const char *path)
{
  struct sockaddr_un sa;
  char dir[sizeof(sa.sun_path)];
  mode_t old;
  int fd;


  if (strlen(path) >= sizeof(sa.sun_path)) {
    apiLog(API_LOG_ERROR, "socket path %s is too long", path);
    return -1;
  }
  apiCopy(dir, sizeof(dir), path);
  if (mkdir(dirname(dir), 0755) && errno != EEXIST) {
    apiLog(API_LOG_ERROR, "mkdir(%s) failed: %s", dir, strerror(errno));
    return -1;
  }
  if (unlink(path) && errno != ENOENT) {
    apiLog(API_LOG_ERROR, "unlink(%s) failed: %s", path, strerror(errno));
    return -1;
  }

  fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    apiLog(API_LOG_ERROR, "socket() failed: %s", strerror(errno));
    return -1;
  }
  memset(&sa, 0, sizeof(sa));
  sa.sun_family = AF_UNIX;
  memcpy(sa.sun_path, path, strlen(path) + 1);

  /* root only: the socket is the access control of the API */
  old = umask(0177);
  if (bind(fd, (struct sockaddr *)&sa, sizeof(sa))) {
    umask(old);
    apiLog(API_LOG_ERROR, "bind(%s) failed: %s", path, strerror(errno));
    close(fd);
    return -1;
  }
  umask(old);
  if (chmod(path, 0600) || listen(fd, 64)) {
    apiLog(API_LOG_ERROR, "listen(%s) failed: %s", path, strerror(errno));
    close(fd);
    return -1;
  }
  return fd;
}


/* "host:port", "[v6]:port", "*:port" or ":port" (all addresses) */
static int
apiListenTcp(const char *spec)
{
  struct addrinfo hints, *res = NULL, *ai;
  char host[128], port[16];
  const char *colon = strrchr(spec, ':');
  int one = 1, zero = 0;
  int fd = -1, ret;


  if (!colon || !colon[1] || (size_t)(colon - spec) >= sizeof(host)) {
    apiLog(API_LOG_ERROR, "GB_API_LISTEN '%s' must be <address>:<port>", spec);
    return -1;
  }
  snprintf(port, sizeof(port), "%s", colon + 1);
  snprintf(host, sizeof(host), "%.*s", (int)(colon - spec), spec);
  if (host[0] == '[' && host[strlen(host) - 1] == ']') {
    memmove(host, host + 1, strlen(host));
    host[strlen(host) - 1] = '\0';
  }

  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE;
  ret = getaddrinfo(!host[0] || !strcmp(host, "*") ? NULL : host, port,
                    &hints, &res);
  if (ret) {
    apiLog(API_LOG_ERROR, "GB_API_LISTEN '%s': %s", spec, gai_strerror(ret));
    return -1;
  }
  /* all addresses: prefer the IPv6 socket that takes IPv4 too */
  for (ai = res; ai; ai = ai->ai_next) {
    if (fd < 0 && (!host[0] || !strcmp(host, "*")) && ai->ai_family != AF_INET6) {
      continue;
    }
    fd = socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC, ai->ai_protocol);
    if (fd < 0) {
      continue;
    }
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (ai->ai_family == AF_INET6) {
      setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof(zero));
    }
    if (!bind(fd, ai->ai_addr, ai->ai_addrlen) && !listen(fd, 64)) {
      break;
    }
    close(fd);
    fd = -1;
  }
  if (fd < 0 && (!host[0] || !strcmp(host, "*"))) {
    /* no IPv6: the first address that works */
    for (ai = res; ai && fd < 0; ai = ai->ai_next) {
      fd = socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC, ai->ai_protocol);
      if (fd >= 0) {
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        if (bind(fd, ai->ai_addr, ai->ai_addrlen) || listen(fd, 64)) {
          close(fd);
          fd = -1;
        }
      }
    }
  }
  freeaddrinfo(res);
  if (fd < 0) {
    apiLog(API_LOG_ERROR, "can not listen on %s: %s", spec, strerror(errno));
  }
  return fd;
}


/* whole file in memory, NUL terminated, for the TLS options */
static char *
apiReadFile(const char *path)
{
  FILE *f = fopen(path, "r");
  char *buf = NULL;
  long len;


  if (!f) {
    apiLog(API_LOG_ERROR, "can not read %s: %s", path, strerror(errno));
    return NULL;
  }
  if (fseek(f, 0, SEEK_END) == 0 && (len = ftell(f)) > 0 && len < 1048576 &&
      fseek(f, 0, SEEK_SET) == 0 && (buf = calloc(1, len + 1)) &&
      fread(buf, 1, len, f) != (size_t)len) {
    free(buf);
    buf = NULL;
  }
  fclose(f);
  if (!buf) {
    apiLog(API_LOG_ERROR, "can not read %s", path);
  }
  return buf;
}


static char *apiTlsCertPem, *apiTlsKeyPem, *apiTlsCaPem;


static struct MHD_Daemon *
apiStartTls(int *fdp)
{
  struct MHD_Daemon *mhd;
  int fd;


  if (!apiConf.tlsCert[0] || !apiConf.tlsKey[0]) {
    apiLog(API_LOG_ERROR, "GB_API_LISTEN needs GB_API_TLS_CERT and "
           "GB_API_TLS_KEY: the TCP listener is TLS only");
    return NULL;
  }
  if (!MHD_is_feature_supported(MHD_FEATURE_TLS)) {
    apiLog(API_LOG_ERROR, "libmicrohttpd is built without TLS");
    return NULL;
  }
  apiTlsCertPem = apiReadFile(apiConf.tlsCert);
  apiTlsKeyPem = apiReadFile(apiConf.tlsKey);
  if (!apiTlsCertPem || !apiTlsKeyPem) {
    return NULL;
  }
  if (apiConf.tlsCa[0] && !(apiTlsCaPem = apiReadFile(apiConf.tlsCa))) {
    return NULL;
  }
  if (!apiTlsCaPem) {
    apiLog(API_LOG_WARNING, "no GB_API_TLS_CA: clients of %s need a token",
           apiConf.listen);
  }
  apiAuthLoad(apiConf.tokensFile);

  fd = apiListenTcp(apiConf.listen);
  if (fd < 0) {
    return NULL;
  }

  mhd = MHD_start_daemon(MHD_USE_AUTO_INTERNAL_THREAD | MHD_USE_ERROR_LOG |
                         MHD_USE_TLS,
                         0, NULL, NULL, &apiHandler, &apiTlsListener,
                         MHD_OPTION_LISTEN_SOCKET, fd,
                         MHD_OPTION_THREAD_POOL_SIZE, (unsigned int)API_THREADS,
                         MHD_OPTION_CONNECTION_TIMEOUT,
                         (unsigned int)(apiConf.daemonTimeout + 30),
                         MHD_OPTION_NOTIFY_COMPLETED, &apiCompleted, NULL,
                         MHD_OPTION_EXTERNAL_LOGGER, &apiMhdLog, NULL,
                         MHD_OPTION_HTTPS_MEM_CERT, apiTlsCertPem,
                         MHD_OPTION_HTTPS_MEM_KEY, apiTlsKeyPem,
                         MHD_OPTION_HTTPS_PRIORITIES,
                         "NORMAL:-VERS-TLS1.0:-VERS-TLS1.1",
                         apiTlsCaPem ? MHD_OPTION_HTTPS_MEM_TRUST : MHD_OPTION_END,
                         apiTlsCaPem,
                         MHD_OPTION_END);
  if (!mhd) {
    apiLog(API_LOG_ERROR, "can not start the TLS listener on %s (certificate "
           "and key?)", apiConf.listen);
    close(fd);
    return NULL;
  }
  *fdp = fd;
  apiLog(API_LOG_INFO, "TLS listener on %s%s", apiConf.listen,
         apiTlsCaPem ? ", client certificates of GB_API_TLS_CA" : "");
  return mhd;
}


int
main(int argc, char *argv[])
{
  static const struct option opts[] = {
    {"socket",     required_argument, NULL, 's'},
    {"log-level",  required_argument, NULL, 'l'},
    {"log-stderr", no_argument,       NULL, 'e'},
    {"new-token",  required_argument, NULL, 't'},
    {"help",       no_argument,       NULL, 'h'},
    {"version",    no_argument,       NULL, 'v'},
    {NULL, 0, NULL, 0}
  };
  struct MHD_Daemon *mhd, *tls = NULL;
  sigset_t set;
  int sig = 0;
  int fd, tlsfd = -1;
  int c;


  apiLoadSysconfig(GB_API_SYSCONFIG);
  apiConfigFromEnv();

  while ((c = getopt_long(argc, argv, "", opts, NULL)) != -1) {
    switch (c) {
    case 's':
      apiCopy(apiConf.socket, sizeof(apiConf.socket), optarg);
      break;
    case 'l':
      if (apiLogLevelParse(optarg, &apiConf.logLevel)) {
        fprintf(stderr, "gluster-block-apid: unknown log level '%s'\n", optarg);
        return EXIT_FAILURE;
      }
      break;
    case 'e':
      apiConf.logStderr = true;
      break;
    case 't':
      if (optind >= argc) {
        apiUsage(stderr);
        return EXIT_FAILURE;
      }
      return apiNewToken(optarg, argv[optind]);
    case 'h':
      apiUsage(stdout);
      return EXIT_SUCCESS;
    case 'v':
      printf("gluster-block-apid (%s), API %s\n", PACKAGE_VERSION, API_VERSION);
      return EXIT_SUCCESS;
    default:
      apiUsage(stderr);
      return EXIT_FAILURE;
    }
  }
  if (optind < argc) {
    apiUsage(stderr);
    return EXIT_FAILURE;
  }

  apiOpenLog();
  signal(SIGPIPE, SIG_IGN);

  /* the threads of libmicrohttpd inherit the mask, signals come to sigwait */
  sigemptyset(&set);
  sigaddset(&set, SIGTERM);
  sigaddset(&set, SIGINT);
  sigaddset(&set, SIGHUP);
  pthread_sigmask(SIG_BLOCK, &set, NULL);

  fd = apiListenUnix(apiConf.socket);
  if (fd < 0) {
    return EXIT_FAILURE;
  }

  mhd = MHD_start_daemon(MHD_USE_AUTO_INTERNAL_THREAD | MHD_USE_ERROR_LOG,
                         0, NULL, NULL, &apiHandler, &apiUnixListener,
                         MHD_OPTION_LISTEN_SOCKET, fd,
                         MHD_OPTION_THREAD_POOL_SIZE, (unsigned int)API_THREADS,
                         MHD_OPTION_CONNECTION_TIMEOUT,
                         (unsigned int)(apiConf.daemonTimeout + 30),
                         MHD_OPTION_NOTIFY_COMPLETED, &apiCompleted, NULL,
                         MHD_OPTION_EXTERNAL_LOGGER, &apiMhdLog, NULL,
                         MHD_OPTION_END);
  if (!mhd) {
    apiLog(API_LOG_ERROR, "can not start the HTTP server on %s", apiConf.socket);
    close(fd);
    unlink(apiConf.socket);
    return EXIT_FAILURE;
  }

  if (apiConf.listen[0]) {
    tls = apiStartTls(&tlsfd);
    if (!tls) {
      /* asked for and not there: fail, do not run half configured */
      MHD_stop_daemon(mhd);
      close(fd);
      unlink(apiConf.socket);
      return EXIT_FAILURE;
    }
  }

  apiLog(API_LOG_INFO, "gluster-block-apid %s (API %s) listens on %s%s%s, "
         "gluster-blockd on %s", PACKAGE_VERSION, API_VERSION, apiConf.socket,
         tls ? " and " : "", tls ? apiConf.listen : "", apiConf.daemonSocket);

  while (sigwait(&set, &sig) == 0) {
    if (sig == SIGHUP) {
      /* logrotate and reload: reopen the log, read the tokens again */
      apiOpenLog();
      if (tls) {
        apiAuthLoad(apiConf.tokensFile);
      }
      continue;
    }
    break;
  }

  apiLog(API_LOG_INFO, "stopping on signal %d", sig);
  if (tls) {
    MHD_stop_daemon(tls);
    close(tlsfd);
  }
  MHD_stop_daemon(mhd);
  close(fd);
  unlink(apiConf.socket);
  if (apiLogFile) {
    fclose(apiLogFile);
  }
  return EXIT_SUCCESS;
}
