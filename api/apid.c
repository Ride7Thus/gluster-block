/*
  Copyright (c) 2026 gluster-block contributors
  This file is part of gluster-block.

  This file is licensed to you under your choice of the GNU Lesser
  General Public License, version 3 or any later version (LGPLv3 or
  later), or the GNU General Public License, version 2 (GPLv2), in all
  cases as published by the Free Software Foundation.
*/

/*
 * gluster-block-apid: process, configuration, logging and the HTTP server.
 *
 * It listens on a unix socket only (0600, root): the Cockpit bridge runs as
 * root with superuser and connects to it, nothing else is needed for the
 * access control.
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
apiConfigFromEnv(void)
{
  const char *v;


  if ((v = getenv("GB_API_SOCKET")) && *v) {
    apiCopy(apiConf.socket, sizeof(apiConf.socket), v);
  }
  if ((v = getenv("GB_API_LOG_LEVEL")) && *v &&
      apiLogLevelParse(v, &apiConf.logLevel)) {
    fprintf(stderr, "gluster-block-apid: unknown GB_API_LOG_LEVEL '%s'\n", v);
  }
  if ((v = getenv("GB_API_LOG_DIR")) && *v) {
    apiCopy(apiConf.logDir, sizeof(apiConf.logDir), v);
  }
  if ((v = getenv("GB_API_DAEMON_TIMEOUT")) && *v) {
    unsigned long t = strtoul(v, NULL, 10);

    if (t > 0 && t < 86400) {
      apiConf.daemonTimeout = t;
    }
  }
}


static void
apiUsage(FILE *f)
{
  fprintf(f,
    "gluster-block-apid (%s): HTTP API of gluster-block\n"
    "usage:\n"
    "  gluster-block-apid [--socket <path>] [--log-level <level>] [--log-stderr]\n"
    "\n"
    "  --socket <path>\n"
    "        unix socket of the API [default: %s, GB_API_SOCKET]\n"
    "  --log-level <NONE|ERROR|WARNING|INFO|DEBUG>\n"
    "        [default: INFO, GB_API_LOG_LEVEL]\n"
    "  --log-stderr\n"
    "        also log to stderr\n"
    "  --help, --version\n"
    "\n"
    "Settings are read from %s.\n",
    PACKAGE_VERSION, API_SOCKET_DEF, GB_API_SYSCONFIG);
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
} apiConnState;


static apiMhdResult
apiHandler(void *cls, struct MHD_Connection *conn, const char *url,
           const char *method, const char *version, const char *upload_data,
           size_t *upload_data_size, void **con_cls)
{
  apiConnState *cs = *con_cls;
  apiRequest req;
  apiResponse resp = {0, NULL, NULL};
  struct MHD_Response *mr;
  struct timeval end;
  const char *text;
  char *path = NULL;
  char peer[64];
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
    /* no request bodies yet: read and drop */
    *upload_data_size = 0;
    return MHD_YES;
  }

  memset(&req, 0, sizeof(req));
  req.method = method;
  req.url = url;
  req.conn = conn;
  apiPeer(conn, peer, sizeof(peer));
  req.identity = peer;

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

  if (resp.body) {
    text = json_object_to_json_string_ext(resp.body, JSON_C_TO_STRING_PLAIN);
    mr = MHD_create_response_from_buffer(strlen(text), (void *)text,
                                         MHD_RESPMEM_MUST_COPY);
  } else {
    mr = MHD_create_response_from_buffer(0, (void *)"", MHD_RESPMEM_PERSISTENT);
  }
  if (!mr) {
    json_object_put(resp.body);
    return MHD_NO;
  }
  if (resp.body) {
    MHD_add_response_header(mr, MHD_HTTP_HEADER_CONTENT_TYPE, "application/json");
  }
  MHD_add_response_header(mr, MHD_HTTP_HEADER_CACHE_CONTROL, "no-store");
  if (resp.allow) {
    MHD_add_response_header(mr, MHD_HTTP_HEADER_ALLOW, resp.allow);
  }
  ret = MHD_queue_response(conn, resp.status ? resp.status : 500, mr);
  MHD_destroy_response(mr);

  gettimeofday(&end, NULL);
  ms = (end.tv_sec - cs->start.tv_sec) * 1000 +
       (end.tv_usec - cs->start.tv_usec) / 1000;
  apiLog(resp.status >= 500 ? API_LOG_WARNING : API_LOG_INFO,
         "%s %s %s %u %ldms", peer, method, url, resp.status, ms);

  json_object_put(resp.body);
  return ret;
}


static void
apiCompleted(void *cls, struct MHD_Connection *conn, void **con_cls,
             enum MHD_RequestTerminationCode toe)
{
  free(*con_cls);
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


int
main(int argc, char *argv[])
{
  static const struct option opts[] = {
    {"socket",     required_argument, NULL, 's'},
    {"log-level",  required_argument, NULL, 'l'},
    {"log-stderr", no_argument,       NULL, 'e'},
    {"help",       no_argument,       NULL, 'h'},
    {"version",    no_argument,       NULL, 'v'},
    {NULL, 0, NULL, 0}
  };
  struct MHD_Daemon *mhd;
  sigset_t set;
  int sig = 0;
  int fd;
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
                         0, NULL, NULL, &apiHandler, NULL,
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
  apiLog(API_LOG_INFO, "gluster-block-apid %s (API %s) listens on %s, "
         "gluster-blockd on %s", PACKAGE_VERSION, API_VERSION, apiConf.socket,
         apiConf.daemonSocket);

  while (sigwait(&set, &sig) == 0) {
    if (sig == SIGHUP) {
      /* logrotate: reopen the log file */
      apiOpenLog();
      continue;
    }
    break;
  }

  apiLog(API_LOG_INFO, "stopping on signal %d", sig);
  MHD_stop_daemon(mhd);
  close(fd);
  unlink(apiConf.socket);
  if (apiLogFile) {
    fclose(apiLogFile);
  }
  return EXIT_SUCCESS;
}
