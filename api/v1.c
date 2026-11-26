/*
  Copyright (c) 2026 gluster-block contributors
  This file is part of gluster-block.

  This file is licensed to you under your choice of the GNU Lesser
  General Public License, version 3 or any later version (LGPLv3 or
  later), or the GNU General Public License, version 2 (GPLv2), in all
  cases as published by the Free Software Foundation.
*/

/*
 * API v1: routes, and the JSON of v1 made from the JSON of gluster-blockd.
 *
 * The daemon's JSON is the format of the cli (upper case keys, sizes as
 * text, "-" for nothing); v1 is the contract with the API clients and must
 * not change incompatibly (docs/api/openapi.yaml, checked in CI): lower case
 * snake_case keys, sizes in bytes, null for what is unknown or not set.
 */

# ifndef _GNU_SOURCE
#  define _GNU_SOURCE
# endif

# include  "config.h"
# include  "apid.h"
# include  "block.h"

# include  <arpa/inet.h>
# include  <ctype.h>
# include  <errno.h>
# include  <limits.h>
# include  <netinet/in.h>
# include  <stdio.h>
# include  <stdlib.h>
# include  <string.h>
# include  <strings.h>

# include  <microhttpd.h>


# define  API_IQN_PREFIX     "iqn.2016-12.org.gluster-block:"
# define  API_ISCSI_PORT     3260


/* ------------------------------------------------------------------------
 * JSON helpers
 * ------------------------------------------------------------------------ */

json_object *
apiJsonGet(json_object *o, const char *key)
{
  json_object *v = NULL;


  if (!o || !json_object_is_type(o, json_type_object) ||
      !json_object_object_get_ex(o, key, &v)) {
    return NULL;
  }
  return v;
}


const char *
apiJsonStr(json_object *o, const char *key)
{
  json_object *v = apiJsonGet(o, key);


  return v && json_object_is_type(v, json_type_string) ?
         json_object_get_string(v) : NULL;
}


long long
apiJsonInt(json_object *o, const char *key, long long def)
{
  json_object *v = apiJsonGet(o, key);
  char *end;
  long long n;


  if (!v) {
    return def;
  }
  if (json_object_is_type(v, json_type_int)) {
    return json_object_get_int64(v);
  }
  if (json_object_is_type(v, json_type_string)) {
    n = strtoll(json_object_get_string(v), &end, 10);
    if (end != json_object_get_string(v)) {
      return n;
    }
  }
  return def;
}


static bool
apiJsonBool(json_object *o, const char *key, bool def)
{
  json_object *v = apiJsonGet(o, key);


  if (!v) {
    return def;
  }
  return json_object_get_boolean(v);
}


/* string, or null when it is empty or "-" */
json_object *
apiStrOrNull(const char *s)
{
  if (!s || !*s || !strcmp(s, "-")) {
    return NULL;
  }
  return json_object_new_string(s);
}


char *
apiLower(const char *s, char *buf, size_t len)
{
  size_t i;


  for (i = 0; s && s[i] && i + 1 < len; i++) {
    buf[i] = s[i] == '-' || s[i] == ' ' ? '_' : tolower((unsigned char)s[i]);
  }
  buf[i] = '\0';
  return buf;
}


static json_object *
apiLowerOrNull(const char *s)
{
  char buf[64];


  if (!s || !*s || !strcmp(s, "-")) {
    return NULL;
  }
  return json_object_new_string(apiLower(s, buf, sizeof(buf)));
}


static json_object *
apiIntOrNull(json_object *o, const char *key, bool zeroIsNull)
{
  json_object *v = apiJsonGet(o, key);
  long long n;


  if (!v) {
    return NULL;
  }
  n = apiJsonInt(o, key, 0);
  if (zeroIsNull && !n) {
    return NULL;
  }
  return json_object_new_int64(n);
}


/* array of the strings of @key, [] when it is missing */
static json_object *
apiStrArray(json_object *o, const char *key)
{
  json_object *arr = apiJsonGet(o, key);
  json_object *out = json_object_new_array();
  size_t i, n;


  if (!arr || !json_object_is_type(arr, json_type_array)) {
    return out;
  }
  n = json_object_array_length(arr);
  for (i = 0; i < n; i++) {
    json_object *e = json_object_array_get_idx(arr, i);

    if (json_object_is_type(e, json_type_string)) {
      json_object_array_add(out, json_object_new_string(json_object_get_string(e)));
    }
  }
  return out;
}


/*
 * Any object of the daemon in v1 style, for results whose content depends
 * on the operation: keys in snake_case, "PORTAL(S)" -> "portals", RESULT and
 * the error keys dropped.
 */
static json_object *
apiV1Generic(json_object *o)
{
  json_object *out;
  size_t i, n;


  if (!o) {
    return NULL;
  }
  switch (json_object_get_type(o)) {
  case json_type_object: {
    out = json_object_new_object();
    json_object_object_foreach(o, key, val) {
      char buf[128];
      size_t j = 0, k;

      if (!strcmp(key, "RESULT") || !strcmp(key, "errCode") ||
          !strcmp(key, "errMsg")) {
        continue;
      }
      /* keys that are data, host names and addresses: as they are */
      if (strpbrk(key, ".:abcdefghijklmnopqrstuvwxyz")) {
        json_object_object_add(out, key, apiV1Generic(val));
        continue;
      }
      for (k = 0; key[k] && j + 1 < sizeof(buf); k++) {
        if (key[k] == '(' || key[k] == ')') {
          continue;
        }
        buf[j++] = isalnum((unsigned char)key[k]) ?
                   tolower((unsigned char)key[k]) : '_';
      }
      buf[j] = '\0';
      json_object_object_add(out, buf, apiV1Generic(val));
    }
    return out;
  }
  case json_type_array:
    out = json_object_new_array();
    n = json_object_array_length(o);
    for (i = 0; i < n; i++) {
      json_object_array_add(out, apiV1Generic(json_object_array_get_idx(o, i)));
    }
    return out;
  default:
    return json_object_get(o);
  }
}


/* ------------------------------------------------------------------------
 * names in the path
 * ------------------------------------------------------------------------ */

static bool
apiNameOk(const char *s, const char *extra, size_t max)
{
  size_t n = strlen(s);
  size_t i;


  if (!n || n >= max) {
    return false;
  }
  for (i = 0; i < n; i++) {
    if (!isalnum((unsigned char)s[i]) && !strchr(extra, s[i])) {
      return false;
    }
  }
  return true;
}

/* gluster volume names: letters, digits, '-', '_' */
# define  apiVolumeOk(s)   apiNameOk((s), "-_", 255)
/* block names, as the cli accepts them */
# define  apiBlockOk(s)    apiNameOk((s), "-_", 255)
/* host names and IPv4/IPv6 addresses */
# define  apiHostOk(s)     apiNameOk((s), "-_.:", 255)
/* operation ids are uuids */
# define  apiOpIdOk(s)     apiNameOk((s), "-", 38)


static bool
apiCheckNames(apiRequest *req, apiResponse *resp)
{
  if (req->nseg >= 2 && !apiVolumeOk(req->seg[1])) {
    apiError(resp, MHD_HTTP_BAD_REQUEST, "invalid_request", EINVAL,
             "invalid volume name '%s'", req->seg[1]);
    return false;
  }
  if (req->nseg >= 4) {
    const char *what = req->seg[2];
    bool ok = !strcmp(what, "blocks") ? apiBlockOk(req->seg[3]) :
              !strcmp(what, "hosts") ? apiHostOk(req->seg[3]) :
              !strcmp(what, "operations") ? apiOpIdOk(req->seg[3]) : true;

    if (!ok) {
      apiError(resp, MHD_HTTP_BAD_REQUEST, "invalid_request", EINVAL,
               "invalid %s name '%s'", what, req->seg[3]);
      return false;
    }
  }
  return true;
}


/* ------------------------------------------------------------------------
 * features: what a client may offer, from the capabilities of the hosts
 * ------------------------------------------------------------------------ */

typedef struct apiFeature {
  const char *name;
  const char *caps[3];
} apiFeature;

static const apiFeature apiFeatures[] = {
  {"volumes",          {NULL}},
  {"block_create",     {"create", NULL}},
  {"block_delete",     {"delete", NULL}},
  /* delete in background with progress: the local daemon, cli RPC rev 17 */
  {"async_delete",     {"delete", NULL}},
  {"block_resize",     {"modify_size", NULL}},
  {"block_auth",       {"create_auth", "modify_auth", NULL}},
  {"prio_path",        {"modify_prio", NULL}},
  {"rebalance",        {"modify_prio", NULL}},
  {"host_maintenance", {"host_maintenance", "modify_prio", NULL}},
  {NULL,               {NULL}},
};


static bool
apiHasCap(json_object *caps, const char *cap)
{
  size_t i, n = caps ? json_object_array_length(caps) : 0;


  for (i = 0; i < n; i++) {
    if (!strcmp(json_object_get_string(json_object_array_get_idx(caps, i)), cap)) {
      return true;
    }
  }
  return false;
}


static json_object *
apiV1Features(json_object *caps)
{
  json_object *out = json_object_new_array();
  size_t i, j;


  for (i = 0; apiFeatures[i].name; i++) {
    bool ok = true;

    for (j = 0; apiFeatures[i].caps[j] && ok; j++) {
      ok = apiHasCap(caps, apiFeatures[i].caps[j]);
    }
    if (ok) {
      json_object_array_add(out, json_object_new_string(apiFeatures[i].name));
    }
  }
  return out;
}


/* ------------------------------------------------------------------------
 * objects of v1
 * ------------------------------------------------------------------------ */

static json_object *
apiV1Volume(json_object *v)
{
  json_object *o = json_object_new_object();
  json_object *bricks = json_object_new_array();
  json_object *hosting = apiJsonGet(v, "BLOCK-HOSTING");
  json_object *arr = apiJsonGet(v, "BRICKS");
  size_t i, n = arr ? json_object_array_length(arr) : 0;


  json_object_object_add(o, "name", apiStrOrNull(apiJsonStr(v, "NAME")));
  json_object_object_add(o, "type", apiLowerOrNull(apiJsonStr(v, "TYPE")));
  json_object_object_add(o, "status", apiLowerOrNull(apiJsonStr(v, "STATUS")));
  json_object_object_add(o, "brick_count", apiStrOrNull(apiJsonStr(v, "BRICK-COUNT")));
  for (i = 0; i < n; i++) {
    const char *b = json_object_get_string(json_object_array_get_idx(arr, i));
    json_object *jb = json_object_new_object();
    const char *colon = strchr(b, ':');
    const char *sp = strchr(b, ' ');
    char *host = colon ? strndup(b, colon - b) : NULL;
    char *path = colon ? strndup(colon + 1, sp && sp > colon ? (size_t)(sp - colon - 1) :
                                 strlen(colon + 1)) : NULL;

    json_object_object_add(jb, "host", apiStrOrNull(host));
    json_object_object_add(jb, "path", apiStrOrNull(path ? path : b));
    json_object_object_add(jb, "arbiter",
                           json_object_new_boolean(strstr(b, "(arbiter)") != NULL));
    json_object_array_add(bricks, jb);
    free(host);
    free(path);
  }
  json_object_object_add(o, "bricks", bricks);
  json_object_object_add(o, "block_hosting",
                         hosting ? json_object_new_boolean(json_object_get_boolean(hosting)) : NULL);
  json_object_object_add(o, "blocks", apiIntOrNull(v, "BLOCKS", false));
  json_object_object_add(o, "error", apiStrOrNull(apiJsonStr(v, "ERROR")));
  return o;
}


static const char *
apiCreateState(const char *entry)
{
  if (!entry || !*entry) {
    return NULL;
  }
  if (!strcasecmp(entry, "INPROGRESS")) {
    return "in_progress";
  }
  if (!strcasecmp(entry, "SUCCESS")) {
    return "done";
  }
  if (!strcasecmp(entry, "FAIL")) {
    return "failed";
  }
  return NULL;
}


static json_object *
apiV1Block(json_object *b, bool showPassword)
{
  json_object *o = json_object_new_object();
  json_object *auth = json_object_new_object();
  json_object *hosts = json_object_new_array();
  json_object *portals = json_object_new_array();
  json_object *resize = json_object_new_array();
  json_object *arr, *exported;
  const char *gbid = apiJsonStr(b, "GBID");
  const char *passwd = apiJsonStr(b, "PASSWORD");
  const char *entry = apiCreateState(apiJsonStr(b, "ENTRY"));
  char iqn[128];
  size_t i, n;


  json_object_object_add(o, "name", apiStrOrNull(apiJsonStr(b, "NAME")));
  json_object_object_add(o, "volume", apiStrOrNull(apiJsonStr(b, "VOLUME")));
  json_object_object_add(o, "gbid", apiStrOrNull(gbid));
  snprintf(iqn, sizeof(iqn), "%s%s", API_IQN_PREFIX, gbid ? gbid : "");
  json_object_object_add(o, "iqn", gbid ? json_object_new_string(iqn) : NULL);
  json_object_object_add(o, "size_bytes", apiIntOrNull(b, "SIZE-BYTES", false));
  json_object_object_add(o, "ha", apiIntOrNull(b, "HA", false));
  json_object_object_add(o, "prio_path", apiStrOrNull(apiJsonStr(b, "PRIO-PATH")));

  arr = apiJsonGet(b, "HOST-STATES");
  n = arr ? json_object_array_length(arr) : 0;
  for (i = 0; i < n; i++) {
    json_object *h = json_object_array_get_idx(arr, i);
    json_object *jh = json_object_new_object();

    json_object_object_add(jh, "host", apiStrOrNull(apiJsonStr(h, "HOST")));
    json_object_object_add(jh, "state", apiStrOrNull(apiJsonStr(h, "STATE")));
    json_object_object_add(jh, "exported",
                           json_object_new_boolean(apiJsonBool(h, "EXPORTED", false)));
    json_object_array_add(hosts, jh);
  }
  json_object_object_add(o, "hosts", hosts);

  exported = apiStrArray(b, "EXPORTED ON");
  n = json_object_array_length(exported);
  for (i = 0; i < n; i++) {
    char portal[300];

    snprintf(portal, sizeof(portal), "%s:%d",
             json_object_get_string(json_object_array_get_idx(exported, i)),
             API_ISCSI_PORT);
    json_object_array_add(portals, json_object_new_string(portal));
  }
  json_object_object_add(o, "exported_on", exported);
  json_object_object_add(o, "failed_on", apiStrArray(b, "ENCOUNTERED FAILURES ON"));
  arr = apiJsonGet(b, "RESIZE FAILED ON");
  if (arr && json_object_is_type(arr, json_type_object)) {
    json_object_object_foreach(arr, host, size) {
      json_object *jr = json_object_new_object();

      json_object_object_add(jr, "host", json_object_new_string(host));
      json_object_object_add(jr, "size", apiStrOrNull(json_object_get_string(size)));
      json_object_array_add(resize, jr);
    }
  }
  json_object_object_add(o, "resize_failed_on", resize);
  json_object_object_add(o, "portals", portals);

  json_object_object_add(o, "io_timeout_sec", apiIntOrNull(b, "IO-TIMEOUT-SEC", true));
  json_object_object_add(o, "ring_buffer_mb", apiIntOrNull(b, "RING-BUFFER-MB", true));
  json_object_object_add(o, "block_size", apiIntOrNull(b, "BLOCK-SIZE", true));
  json_object_object_add(o, "prealloc", apiStrOrNull(apiJsonStr(b, "PREALLOC")));
  json_object_object_add(o, "create_state", entry ? json_object_new_string(entry) : NULL);

  json_object_object_add(auth, "enabled", json_object_new_boolean(passwd && *passwd));
  json_object_object_add(auth, "username", passwd && *passwd ? apiStrOrNull(gbid) : NULL);
  if (showPassword) {
    json_object_object_add(auth, "password", apiStrOrNull(passwd));
  }
  json_object_object_add(o, "auth", auth);

  return o;
}


static json_object *
apiV1HostBlock(json_object *b)
{
  json_object *o = json_object_new_object();
  bool reported = apiJsonGet(b, "BACKSTORE") != NULL;


  json_object_object_add(o, "name", apiStrOrNull(apiJsonStr(b, "NAME")));
  json_object_object_add(o, "gbid", apiStrOrNull(apiJsonStr(b, "GBID")));
  json_object_object_add(o, "meta_state", apiStrOrNull(apiJsonStr(b, "META")));
  json_object_object_add(o, "prio", json_object_new_boolean(apiJsonBool(b, "PRIO", false)));
  json_object_object_add(o, "reported", json_object_new_boolean(reported));
  json_object_object_add(o, "backstore_loaded", reported ?
                         json_object_new_boolean(apiJsonBool(b, "BACKSTORE", false)) : NULL);
  json_object_object_add(o, "tpg", apiIntOrNull(b, "TPG", true));
  json_object_object_add(o, "tpg_enabled", reported ?
                         json_object_new_boolean(apiJsonBool(b, "TPG-ENABLED", false)) : NULL);
  json_object_object_add(o, "alua", apiLowerOrNull(apiJsonStr(b, "ALUA")));
  json_object_object_add(o, "scsi_cmds", apiIntOrNull(b, "SCSI-CMDS", false));
  json_object_object_add(o, "read_mb", apiIntOrNull(b, "READ-MB", false));
  json_object_object_add(o, "write_mb", apiIntOrNull(b, "WRITE-MB", false));
  json_object_object_add(o, "sessions", apiStrArray(b, "SESSIONS"));
  json_object_object_add(o, "problem", apiStrOrNull(apiJsonStr(b, "PROBLEM")));
  return o;
}


static json_object *
apiV1Host(json_object *h, bool detail)
{
  json_object *o = json_object_new_object();
  json_object *arr;
  size_t i, n;


  json_object_object_add(o, "host", apiStrOrNull(apiJsonStr(h, "HOST")));
  json_object_object_add(o, "daemon", apiStrOrNull(apiJsonStr(h, "DAEMON")));
  json_object_object_add(o, "version", apiStrOrNull(apiJsonStr(h, "VERSION")));
  json_object_object_add(o, "tcmu_runner", apiLowerOrNull(apiJsonStr(h, "TCMU-RUNNER")));
  json_object_object_add(o, "target_service", apiLowerOrNull(apiJsonStr(h, "TARGET")));
  json_object_object_add(o, "maintenance", apiLowerOrNull(apiJsonStr(h, "STATE")));
  json_object_object_add(o, "blocks", apiIntOrNull(h, "BLOCKS", false));
  json_object_object_add(o, "loaded", apiIntOrNull(h, "LOADED", false));
  json_object_object_add(o, "tpg_enabled", apiIntOrNull(h, "TPG-ENABLED", false));
  json_object_object_add(o, "prio_paths", apiIntOrNull(h, "PRIO-PATHS", false));
  json_object_object_add(o, "problems", apiStrArray(h, "PROBLEMS"));
  if (detail) {
    json_object *list = json_object_new_array();

    arr = apiJsonGet(h, "BLOCK-LIST");
    n = arr ? json_object_array_length(arr) : 0;
    for (i = 0; i < n; i++) {
      json_object_array_add(list, apiV1HostBlock(json_object_array_get_idx(arr, i)));
    }
    json_object_object_add(o, "block_states", list);
  }
  return o;
}


static const char *
apiOpState(const char *s)
{
  if (!s) {
    return NULL;
  }
  if (!strcmp(s, "IN-PROGRESS")) {
    return "running";
  }
  if (!strcmp(s, "SUCCESS")) {
    return "succeeded";
  }
  if (!strcmp(s, "FAIL")) {
    return "failed";
  }
  if (!strcmp(s, "INTERRUPTED")) {
    return "interrupted";
  }
  return NULL;
}


static json_object *
apiV1Operation(const char *volume, json_object *s)
{
  json_object *o = json_object_new_object();
  const char *state = apiOpState(apiJsonStr(s, "STATE"));
  long long err = apiJsonInt(s, "errCode", 0);


  json_object_object_add(o, "id", apiStrOrNull(apiJsonStr(s, "OPERATION-ID")));
  json_object_object_add(o, "volume", apiStrOrNull(volume));
  json_object_object_add(o, "operation", apiStrOrNull(apiJsonStr(s, "OPERATION")));
  json_object_object_add(o, "block", apiStrOrNull(apiJsonStr(s, "NAME")));
  json_object_object_add(o, "gbid", apiStrOrNull(apiJsonStr(s, "GBID")));
  json_object_object_add(o, "node", apiStrOrNull(apiJsonStr(s, "NODE")));
  json_object_object_add(o, "state", state ? json_object_new_string(state) : NULL);
  json_object_object_add(o, "phase", apiStrOrNull(apiJsonStr(s, "PHASE")));
  json_object_object_add(o, "size_bytes", apiIntOrNull(s, "SIZE", true));
  json_object_object_add(o, "allocated", apiStrOrNull(apiJsonStr(s, "ALLOCATED")));
  if (apiJsonGet(s, "STEP")) {
    json_object *step = json_object_new_object();

    json_object_object_add(step, "name", apiStrOrNull(apiJsonStr(s, "STEP")));
    json_object_object_add(step, "done", apiIntOrNull(s, "STEP-DONE", false));
    json_object_object_add(step, "total", apiIntOrNull(s, "STEP-TOTAL", false));
    json_object_object_add(step, "percent", apiIntOrNull(s, "STEP-PERCENT", false));
    json_object_object_add(step, "remaining_sec", apiIntOrNull(s, "REMAINING-SEC", false));
    json_object_object_add(o, "step", step);
  } else {
    json_object_object_add(o, "step", NULL);
  }
  json_object_object_add(o, "started_at", apiIntOrNull(s, "STARTED", true));
  json_object_object_add(o, "updated_at", apiIntOrNull(s, "UPDATED", true));
  json_object_object_add(o, "finished_at", apiIntOrNull(s, "FINISHED", true));
  if (err || (apiJsonStr(s, "errMsg") && *apiJsonStr(s, "errMsg") &&
              state && strcmp(state, "succeeded"))) {
    json_object *je = json_object_new_object();

    json_object_object_add(je, "errno", json_object_new_int64(err));
    json_object_object_add(je, "message", apiStrOrNull(apiJsonStr(s, "errMsg")));
    json_object_object_add(o, "error", je);
  } else {
    json_object_object_add(o, "error", NULL);
  }
  return o;
}


/* ------------------------------------------------------------------------
 * handlers
 * ------------------------------------------------------------------------ */

static void
apiV1Health(apiRequest *req, apiResponse *resp)
{
  json_object *jo;


  if (!apiDaemonCompatible(resp, true)) {
    return;
  }
  jo = json_object_new_object();
  json_object_object_add(jo, "status", json_object_new_string("ok"));
  apiRespond(resp, MHD_HTTP_OK, jo);
}


static void
apiV1Version(apiRequest *req, apiResponse *resp)
{
  const char *volume = apiQuery(req, "volume");
  json_object *jo, *jd, *caps;
  apiCall c;


  if (volume && !apiVolumeOk(volume)) {
    apiError(resp, MHD_HTTP_BAD_REQUEST, "invalid_request", EINVAL,
             "invalid volume name '%s'", volume);
    return;
  }
  if (!apiDaemonCompatible(resp, false)) {
    return;
  }
  apiDaemonVersion(volume, &c);
  if (apiCallFailed(&c, resp)) {
    apiCallFree(&c);
    return;
  }

  jo = json_object_new_object();
  json_object_object_add(jo, "api_version", json_object_new_string(API_VERSION));
  json_object_object_add(jo, "apid_version", json_object_new_string(PACKAGE_VERSION));

  jd = json_object_new_object();
  json_object_object_add(jd, "version", apiStrOrNull(apiJsonStr(c.json, "VERSION")));
  json_object_object_add(jd, "cli_rpc_rev", apiIntOrNull(c.json, "CLI-RPC-REV", false));
  json_object_object_add(jd, "capabilities", apiStrArray(c.json, "CAPABILITIES"));
  json_object_object_add(jo, "daemon", jd);

  caps = apiJsonGet(c.json, "CAPABILITIES");
  if (volume) {
    json_object *jv = json_object_new_object();
    json_object *hosts = json_object_new_array();
    json_object *arr = apiJsonGet(c.json, "HOSTS");
    size_t i, n = arr ? json_object_array_length(arr) : 0;

    for (i = 0; i < n; i++) {
      json_object *h = json_object_array_get_idx(arr, i);
      json_object *jh = json_object_new_object();

      json_object_object_add(jh, "host", apiStrOrNull(apiJsonStr(h, "HOST")));
      json_object_object_add(jh, "daemon", apiStrOrNull(apiJsonStr(h, "DAEMON")));
      json_object_object_add(jh, "version", apiStrOrNull(apiJsonStr(h, "VERSION")));
      json_object_object_add(jh, "capabilities", apiStrArray(h, "CAPABILITIES"));
      json_object_object_add(jh, "error", apiStrOrNull(apiJsonStr(h, "ERROR")));
      json_object_array_add(hosts, jh);
    }
    json_object_object_add(jv, "name", json_object_new_string(volume));
    json_object_object_add(jv, "hosts", hosts);
    json_object_object_add(jv, "capabilities", apiStrArray(c.json, "COMMON-CAPABILITIES"));
    json_object_object_add(jo, "volume", jv);
    caps = apiJsonGet(c.json, "COMMON-CAPABILITIES");
  }
  json_object_object_add(jo, "features", apiV1Features(caps));

  apiCallFree(&c);
  apiRespond(resp, MHD_HTTP_OK, jo);
}


/* the volume as the volume list has it, NULL with @resp set if it is unknown */
static json_object *
apiFindVolume(const char *name, apiResponse *resp, bool refresh)
{
  json_object *vols = apiVolumesCached(resp, refresh);
  json_object *arr = apiJsonGet(vols, "VOLUMES");
  json_object *found = NULL;
  size_t i, n = arr ? json_object_array_length(arr) : 0;


  if (!vols) {
    return NULL;
  }
  for (i = 0; i < n; i++) {
    json_object *v = json_object_array_get_idx(arr, i);
    const char *vn = apiJsonStr(v, "NAME");

    if (vn && !strcmp(vn, name)) {
      found = json_object_get(v);
      break;
    }
  }
  json_object_put(vols);

  if (!found) {
    apiError(resp, MHD_HTTP_NOT_FOUND, "volume_not_found", ENOENT,
             "gluster volume %s does not exist", name);
  }
  return found;
}


/*
 * Requests on the blocks of a volume: it must exist and host blocks. Asking
 * gluster-blockd about another volume would create its metadata directory.
 */
static bool
apiBlockHostingVolume(const char *name, apiResponse *resp)
{
  json_object *v;
  json_object *hosting;
  bool ok;
  int pass;


  for (pass = 0; pass < 2; pass++) {
    v = apiFindVolume(name, resp, pass > 0);
    if (!v) {
      if (pass == 0 && resp->status == MHD_HTTP_NOT_FOUND) {
        continue;    /* maybe created since the list was read */
      }
      return false;
    }
    hosting = apiJsonGet(v, "BLOCK-HOSTING");
    ok = hosting && json_object_get_boolean(hosting);
    if (ok) {
      json_object_put(v);
      apiRespond(resp, 0, NULL);
      return true;
    }
    if (pass == 1 || !hosting) {
      if (!hosting) {
        apiError(resp, MHD_HTTP_CONFLICT, "volume_not_ready", 0,
                 "volume %s is %s%s%s", name,
                 apiJsonStr(v, "STATUS") ? apiJsonStr(v, "STATUS") : "not reachable",
                 apiJsonStr(v, "ERROR") && *apiJsonStr(v, "ERROR") ? ": " : "",
                 apiJsonStr(v, "ERROR") ? apiJsonStr(v, "ERROR") : "");
      } else {
        apiError(resp, MHD_HTTP_NOT_FOUND, "not_block_hosting", ENOENT,
                 "volume %s hosts no block devices", name);
      }
      json_object_put(v);
      return false;
    }
    json_object_put(v);
  }
  return false;
}


static void
apiV1Volumes(apiRequest *req, apiResponse *resp)
{
  json_object *vols = apiVolumesCached(resp, apiQueryBool(req, "refresh"));
  json_object *arr = apiJsonGet(vols, "VOLUMES");
  json_object *jo, *out;
  size_t i, n = arr ? json_object_array_length(arr) : 0;


  if (!vols) {
    return;
  }
  jo = json_object_new_object();
  out = json_object_new_array();
  for (i = 0; i < n; i++) {
    json_object_array_add(out, apiV1Volume(json_object_array_get_idx(arr, i)));
  }
  json_object_object_add(jo, "volumes", out);
  json_object_put(vols);
  apiRespond(resp, MHD_HTTP_OK, jo);
}


static void
apiV1VolumeOne(apiRequest *req, apiResponse *resp)
{
  json_object *v = apiFindVolume(req->seg[1], resp, apiQueryBool(req, "refresh"));


  if (v) {
    apiRespond(resp, MHD_HTTP_OK, apiV1Volume(v));
    json_object_put(v);
  }
}


static void
apiV1Blocks(apiRequest *req, apiResponse *resp)
{
  const char *volume = req->seg[1];
  bool detail = apiQueryBool(req, "detail");
  json_object *jo, *out, *names;
  apiCall c;
  size_t i, n;


  if (!apiBlockHostingVolume(volume, resp)) {
    return;
  }
  apiDaemonList(volume, &c);
  if (apiCallFailed(&c, resp)) {
    apiCallFree(&c);
    return;
  }

  jo = json_object_new_object();
  out = json_object_new_array();
  names = apiJsonGet(c.json, "blocks");
  n = names ? json_object_array_length(names) : 0;
  for (i = 0; i < n; i++) {
    const char *name = json_object_get_string(json_object_array_get_idx(names, i));
    apiCall ic;

    if (!detail) {
      json_object *b = json_object_new_object();

      json_object_object_add(b, "name", json_object_new_string(name));
      json_object_array_add(out, b);
      continue;
    }
    apiDaemonInfo(volume, name, &ic);
    if (ic.rpc == 0 && ic.exit == 0 && ic.json) {
      json_object_array_add(out, apiV1Block(ic.json, false));
    } else {
      /* deleted meanwhile, or its metadata is broken: name and error */
      json_object *b = json_object_new_object();
      apiResponse er = {0, NULL, NULL};

      json_object_object_add(b, "name", json_object_new_string(name));
      if (apiCallFailed(&ic, &er) && er.body) {
        json_object_object_add(b, "error",
                               json_object_get(apiJsonGet(er.body, "error")));
      }
      json_object_put(er.body);
      json_object_array_add(out, b);
    }
    apiCallFree(&ic);
  }
  json_object_object_add(jo, "volume", json_object_new_string(volume));
  json_object_object_add(jo, "blocks", out);
  apiCallFree(&c);
  apiRespond(resp, MHD_HTTP_OK, jo);
}


static void
apiV1Block1(apiRequest *req, apiResponse *resp)
{
  apiCall c;


  if (!apiBlockHostingVolume(req->seg[1], resp)) {
    return;
  }
  apiDaemonInfo(req->seg[1], req->seg[3], &c);
  if (!apiCallFailed(&c, resp)) {
    if (c.json) {
      apiRespond(resp, MHD_HTTP_OK,
                 apiV1Block(c.json, apiQueryBool(req, "show_password")));
    } else {
      apiError(resp, MHD_HTTP_BAD_GATEWAY, "daemon_error", 0,
               "gluster-blockd answered info without JSON");
    }
  } else if (resp->status == MHD_HTTP_NOT_FOUND) {
    apiError(resp, MHD_HTTP_NOT_FOUND, "block_not_found", ENOENT,
             "block %s/%s does not exist", req->seg[1], req->seg[3]);
  }
  apiCallFree(&c);
}


static void
apiV1Hosts(apiRequest *req, apiResponse *resp)
{
  const char *host = req->nseg > 3 ? req->seg[3] : NULL;
  json_object *jo, *out, *arr;
  apiCall c;
  size_t i, n;


  if (!apiBlockHostingVolume(req->seg[1], resp)) {
    return;
  }
  apiDaemonHosts(req->seg[1], host, &c);
  if (apiCallFailed(&c, resp)) {
    if (host && resp->status == MHD_HTTP_NOT_FOUND) {
      apiError(resp, MHD_HTTP_NOT_FOUND, "host_not_found", ENOENT,
               "host %s exports no block devices of volume %s", host, req->seg[1]);
    }
    apiCallFree(&c);
    return;
  }

  arr = apiJsonGet(c.json, "HOSTS");
  n = arr ? json_object_array_length(arr) : 0;
  if (host) {
    if (!n) {
      apiError(resp, MHD_HTTP_NOT_FOUND, "host_not_found", ENOENT,
               "host %s exports no block devices of volume %s", host, req->seg[1]);
    } else {
      json_object *h = apiV1Host(json_object_array_get_idx(arr, 0), true);

      json_object_object_add(h, "volume", json_object_new_string(req->seg[1]));
      apiRespond(resp, MHD_HTTP_OK, h);
    }
    apiCallFree(&c);
    return;
  }

  jo = json_object_new_object();
  out = json_object_new_array();
  for (i = 0; i < n; i++) {
    json_object_array_add(out, apiV1Host(json_object_array_get_idx(arr, i), false));
  }
  json_object_object_add(jo, "volume", json_object_new_string(req->seg[1]));
  json_object_object_add(jo, "healthy",
                         json_object_new_boolean(apiJsonBool(c.json, "HEALTHY", false)));
  json_object_object_add(jo, "hosts", out);
  apiCallFree(&c);
  apiRespond(resp, MHD_HTTP_OK, jo);
}


static void
apiV1Operations(apiRequest *req, apiResponse *resp)
{
  json_object *jo, *cur, *last;
  const char *st;
  apiCall c;


  if (!apiBlockHostingVolume(req->seg[1], resp)) {
    return;
  }
  apiDaemonStatus(req->seg[1], NULL, &c);
  if (apiCallFailed(&c, resp)) {
    apiCallFree(&c);
    return;
  }
  st = apiJsonStr(c.json, "STATUS");
  cur = apiJsonGet(c.json, "OPERATION");
  last = apiJsonGet(c.json, "LAST-OPERATION");

  jo = json_object_new_object();
  json_object_object_add(jo, "volume", json_object_new_string(req->seg[1]));
  json_object_object_add(jo, "busy", json_object_new_boolean(st && !strcmp(st, "BUSY")));
  json_object_object_add(jo, "current", cur ? apiV1Operation(req->seg[1], cur) : NULL);
  json_object_object_add(jo, "last", last ? apiV1Operation(req->seg[1], last) : NULL);
  apiCallFree(&c);
  apiRespond(resp, MHD_HTTP_OK, jo);
}


/*
 * One operation: running (the daemon answers EINPROGRESS with the volume
 * status), or done with the result it returned, or failed.
 */
static void
apiV1Operation1(apiRequest *req, apiResponse *resp)
{
  const char *volume = req->seg[1];
  const char *id = req->seg[3];
  json_object *op = NULL, *meta = NULL;
  apiCall c, vc;


  if (!apiBlockHostingVolume(volume, resp)) {
    return;
  }
  apiDaemonStatus(volume, id, &c);
  if (c.rpc == 0 && c.exit == EINPROGRESS) {
    meta = apiJsonGet(c.json, "OPERATION");
    if (!meta || !apiJsonStr(meta, "OPERATION-ID") ||
        strcmp(apiJsonStr(meta, "OPERATION-ID"), id)) {
      /* finished, its result is being stored: still running for the client */
      op = json_object_new_object();
      json_object_object_add(op, "id", json_object_new_string(id));
      json_object_object_add(op, "volume", json_object_new_string(volume));
      json_object_object_add(op, "state", json_object_new_string("running"));
    } else {
      op = apiV1Operation(volume, meta);
    }
    json_object_object_add(op, "result", NULL);
    apiCallFree(&c);
    apiRespond(resp, MHD_HTTP_OK, op);
    return;
  }
  if (c.rpc || c.exit == ENOENT || c.exit == EINVAL) {
    if (apiCallFailed(&c, resp) && resp->status == MHD_HTTP_NOT_FOUND) {
      apiError(resp, MHD_HTTP_NOT_FOUND, "operation_not_found", ENOENT,
               "operation %s is not known on volume %s", id, volume);
    }
    apiCallFree(&c);
    return;
  }

  /* done: the metadata from the volume status if it is the last operation */
  memset(&vc, 0, sizeof(vc));
  apiDaemonStatus(volume, NULL, &vc);
  meta = vc.rpc == 0 && !vc.exit ? apiJsonGet(vc.json, "LAST-OPERATION") : NULL;
  if (meta && apiJsonStr(meta, "OPERATION-ID") &&
      !strcmp(apiJsonStr(meta, "OPERATION-ID"), id)) {
    op = apiV1Operation(volume, meta);
  } else {
    op = json_object_new_object();
    json_object_object_add(op, "id", json_object_new_string(id));
    json_object_object_add(op, "volume", json_object_new_string(volume));
  }
  json_object_object_add(op, "state",
                         json_object_new_string(c.exit ? "failed" : "succeeded"));
  if (c.exit) {
    json_object *je = json_object_new_object();
    const char *msg = c.json ? apiJsonStr(c.json, "errMsg") : c.out;

    json_object_object_add(je, "errno",
                           json_object_new_int64(c.json ? apiJsonInt(c.json, "errCode", c.exit) : c.exit));
    json_object_object_add(je, "message", apiStrOrNull(msg));
    json_object_object_add(op, "error", je);
    json_object_object_add(op, "result", NULL);
  } else {
    json_object_object_add(op, "error", NULL);
    if (c.json) {
      json_object_object_add(op, "result", apiV1Generic(c.json));
    } else {
      json_object *jr = json_object_new_object();

      json_object_object_add(jr, "message", apiStrOrNull(c.out));
      json_object_object_add(op, "result", jr);
    }
  }
  apiCallFree(&c);
  apiCallFree(&vc);
  apiRespond(resp, MHD_HTTP_OK, op);
}


/* ------------------------------------------------------------------------
 * changes
 * ------------------------------------------------------------------------ */

/* the body has only these keys */
static bool
apiBodyKeys(apiRequest *req, apiResponse *resp, const char *const *allowed)
{
  size_t i;


  if (!req->body) {
    return true;
  }
  json_object_object_foreach(req->body, key, val) {
    bool ok = false;

    (void)val;
    for (i = 0; allowed[i] && !ok; i++) {
      ok = !strcmp(key, allowed[i]);
    }
    if (!ok) {
      apiError(resp, MHD_HTTP_BAD_REQUEST, "invalid_request", EINVAL,
               "unknown field '%s' in the request body", key);
      return false;
    }
  }
  return true;
}


/* 0 absent, 1 present with @out set, -1 wrong type (@resp set) */
static int
apiBodyGet(apiRequest *req, apiResponse *resp, const char *key,
           json_type type, json_object **out)
{
  json_object *v = apiJsonGet(req->body, key);


  *out = NULL;
  if (!v) {
    return 0;
  }
  if (!json_object_is_type(v, type)) {
    apiError(resp, MHD_HTTP_BAD_REQUEST, "invalid_request", EINVAL,
             "field '%s' must be %s", key,
             type == json_type_int ? "an integer" :
             type == json_type_boolean ? "true or false" :
             type == json_type_string ? "a string" :
             type == json_type_array ? "an array" : "an object");
    return -1;
  }
  *out = v;
  return 1;
}


static int
apiBodyBool(apiRequest *req, apiResponse *resp, const char *key, bool *out)
{
  json_object *v;
  int r = apiBodyGet(req, resp, key, json_type_boolean, &v);


  if (r == 1) {
    *out = json_object_get_boolean(v);
  }
  return r;
}


/* integer in [min, max] */
static int
apiBodyInt(apiRequest *req, apiResponse *resp, const char *key,
           long long min, long long max, long long *out)
{
  json_object *v;
  int r = apiBodyGet(req, resp, key, json_type_int, &v);


  if (r == 1) {
    *out = json_object_get_int64(v);
    if (*out < min || *out > max) {
      apiError(resp, MHD_HTTP_BAD_REQUEST, "invalid_request", EINVAL,
               "field '%s' must be between %lld and %lld", key, min, max);
      return -1;
    }
  }
  return r;
}


static bool
apiQueryBoolDef(apiRequest *req, const char *key, bool def)
{
  const char *v = apiQuery(req, key);


  if (!v) {
    return def;
  }
  return apiQueryBool(req, key);
}


/* the block hosts of gluster-block are IP addresses */
static bool
apiAddrOk(const char *s)
{
  unsigned char buf[sizeof(struct in6_addr)];


  return inet_pton(AF_INET, s, buf) == 1 || inet_pton(AF_INET6, s, buf) == 1;
}


/* what goes into the cmdlog of gluster-blockd */
static void
apiCmd(apiRequest *req, char *buf, size_t len)
{
  snprintf(buf, len, "api %s: %s %s %s", req->identity ? req->identity : "-",
           req->method, req->url,
           req->body ? json_object_to_json_string_ext(req->body, JSON_C_TO_STRING_PLAIN) : "");
}


/*
 * The daemon accepted a long operation: 202 with the operation and its
 * Location. When it finished at once (no operation id), 200 with the
 * result.
 */
static void
apiAccepted(apiRequest *req, apiResponse *resp, const char *volume, apiCall *c)
{
  const char *id = apiJsonStr(c->json, "OPERATION-ID");
  json_object *op = json_object_new_object();


  json_object_object_add(op, "id", apiStrOrNull(id));
  json_object_object_add(op, "volume", json_object_new_string(volume));
  json_object_object_add(op, "operation", apiStrOrNull(apiJsonStr(c->json, "OPERATION")));
  json_object_object_add(op, "block", apiStrOrNull(apiJsonStr(c->json, "NAME")));
  if (id) {
    json_object_object_add(op, "state", json_object_new_string("running"));
    json_object_object_add(op, "error", NULL);
    json_object_object_add(op, "result", NULL);
    snprintf(resp->location, sizeof(resp->location), "%s/volumes/%s/operations/%s",
             API_PREFIX, volume, id);
    apiRespond(resp, MHD_HTTP_ACCEPTED, op);
    return;
  }
  json_object_object_add(op, "state", json_object_new_string("succeeded"));
  json_object_object_add(op, "error", NULL);
  json_object_object_add(op, "result", c->json ? apiV1Generic(c->json) : NULL);
  apiRespond(resp, MHD_HTTP_OK, op);
}


/* the block as it is now, for the answer of a change; 200 */
static void
apiRespondBlock(apiResponse *resp, const char *volume, const char *block)
{
  apiCall c;


  apiDaemonInfo(volume, block, &c);
  if (!apiCallFailed(&c, resp) && c.json) {
    apiRespond(resp, MHD_HTTP_OK, apiV1Block(c.json, false));
  }
  apiCallFree(&c);
}


/* the volume exists and is started; @hosting tells whether it has blocks */
static bool
apiStartedVolume(const char *name, apiResponse *resp, bool *hosting)
{
  json_object *v = apiFindVolume(name, resp, false);
  const char *st;
  json_object *h;


  if (!v) {
    if (resp->status != MHD_HTTP_NOT_FOUND || !(v = apiFindVolume(name, resp, true))) {
      return false;
    }
  }
  st = apiJsonStr(v, "STATUS");
  if (!st || strcmp(st, "Started")) {
    apiError(resp, MHD_HTTP_CONFLICT, "volume_not_ready", 0,
             "volume %s is %s", name, st ? st : "not reachable");
    json_object_put(v);
    return false;
  }
  h = apiJsonGet(v, "BLOCK-HOSTING");
  *hosting = h && json_object_get_boolean(h);
  json_object_put(v);
  apiRespond(resp, 0, NULL);
  return true;
}


static const char *const apiCreateKeys[] = {
  "name", "size_bytes", "hosts", "ha", "auth", "prealloc", "zerofill",
  "ring_buffer_mb", "block_size", "io_timeout_sec", "storage", "confirm", NULL
};


/* create of a block that exists: the same request again is no error */
static void
apiCreateExisting(apiRequest *req, apiResponse *resp, const apiCreateArgs *a,
                  apiCall *c)
{
  apiCall ic;
  json_object *arr;
  size_t i, n, matched = 0;
  bool same;


  apiDaemonInfo(a->volume, a->block, &ic);
  if (ic.rpc || ic.exit || !ic.json) {
    apiCallFailed(c, resp);
    apiCallFree(&ic);
    return;
  }
  same = (unsigned long long)apiJsonInt(ic.json, "SIZE-BYTES", -1) == a->size &&
         apiJsonInt(ic.json, "HA", -1) == a->ha;
  arr = apiJsonGet(ic.json, "HOST-STATES");
  n = arr ? json_object_array_length(arr) : 0;
  for (i = 0; i < n && same; i++) {
    const char *h = apiJsonStr(json_object_array_get_idx(arr, i), "HOST");
    char *hosts = strdup(a->hosts), *tok, *save = NULL;
    bool found = false;

    for (tok = hosts ? strtok_r(hosts, ",", &save) : NULL; tok && !found;
         tok = strtok_r(NULL, ",", &save)) {
      found = h && !strcmp(tok, h);
    }
    free(hosts);
    same = found;
    matched += found;
  }
  same = same && matched == a->ha;
  if (same) {
    apiRespond(resp, MHD_HTTP_OK, apiV1Block(ic.json, false));
  } else {
    apiError(resp, MHD_HTTP_CONFLICT, "already_exists", EEXIST,
             "block %s/%s exists with another size or other hosts",
             a->volume, a->block);
  }
  apiCallFree(&ic);
}


static void
apiV1BlockCreate(apiRequest *req, apiResponse *resp)
{
  const char *volume = req->seg[1];
  apiCreateArgs a;
  json_object *v, *hosts;
  char *hostlist = NULL;
  char cmd[2048];
  long long n;
  bool hosting;
  size_t i, nh;
  apiCall c;


  memset(&a, 0, sizeof(a));
  a.volume = volume;
  a.prealloc = true;
  if (!req->body) {
    apiError(resp, MHD_HTTP_BAD_REQUEST, "invalid_request", EINVAL,
             "a JSON body with name, size_bytes and hosts is needed");
    return;
  }
  if (!apiBodyKeys(req, resp, apiCreateKeys)) {
    return;
  }
  if (apiBodyGet(req, resp, "name", json_type_string, &v) != 1 ||
      !apiBlockOk(json_object_get_string(v))) {
    if (resp->status != MHD_HTTP_BAD_REQUEST) {
      apiError(resp, MHD_HTTP_BAD_REQUEST, "invalid_request", EINVAL,
               "name: letters, digits, '-' and '_' are needed");
    }
    return;
  }
  a.block = json_object_get_string(v);
  if (apiBodyInt(req, resp, "size_bytes", 1, LLONG_MAX, &n) != 1) {
    if (resp->status != MHD_HTTP_BAD_REQUEST) {
      apiError(resp, MHD_HTTP_BAD_REQUEST, "invalid_request", EINVAL,
               "size_bytes is needed");
    }
    return;
  }
  a.size = n;

  if (apiBodyGet(req, resp, "hosts", json_type_array, &hosts) != 1 ||
      !(nh = json_object_array_length(hosts))) {
    if (resp->status != MHD_HTTP_BAD_REQUEST) {
      apiError(resp, MHD_HTTP_BAD_REQUEST, "invalid_request", EINVAL,
               "hosts: the IP addresses of the hosts that export the block");
    }
    return;
  }
  for (i = 0; i < nh; i++) {
    json_object *h = json_object_array_get_idx(hosts, i);
    const char *s = json_object_is_type(h, json_type_string) ?
                    json_object_get_string(h) : NULL;
    char *tmp;
    size_t j;

    if (!s || !apiAddrOk(s)) {
      apiError(resp, MHD_HTTP_BAD_REQUEST, "invalid_request", EINVAL,
               "hosts[%zu]: an IP address is needed, gluster-block does not "
               "take host names", i);
      free(hostlist);
      return;
    }
    for (j = 0; j < i; j++) {
      if (!strcmp(s, json_object_get_string(json_object_array_get_idx(hosts, j)))) {
        apiError(resp, MHD_HTTP_BAD_REQUEST, "invalid_request", EINVAL,
                 "host %s is given twice", s);
        free(hostlist);
        return;
      }
    }
    if (asprintf(&tmp, "%s%s%s", hostlist ? hostlist : "", hostlist ? "," : "", s) < 0) {
      free(hostlist);
      apiError(resp, MHD_HTTP_INTERNAL_SERVER_ERROR, "operation_failed", ENOMEM,
               "out of memory");
      return;
    }
    free(hostlist);
    hostlist = tmp;
  }
  a.hosts = hostlist;
  a.ha = nh;

  if (apiBodyInt(req, resp, "ha", 1, 64, &n) < 0) {
    goto out;
  } else if (apiJsonGet(req->body, "ha") && (size_t)n != nh) {
    apiError(resp, MHD_HTTP_BAD_REQUEST, "invalid_request", EINVAL,
             "ha must be the number of hosts (%zu)", nh);
    goto out;
  }
  if (apiBodyBool(req, resp, "auth", &a.auth) < 0 ||
      apiBodyBool(req, resp, "zerofill", &a.zerofill) < 0 ||
      apiBodyBool(req, resp, "confirm", &a.confirmed) < 0) {
    goto out;
  }
  if (apiBodyGet(req, resp, "prealloc", json_type_string, &v) < 0) {
    goto out;
  } else if (v) {
    if (!strcmp(json_object_get_string(v), "no")) {
      a.prealloc = false;
    } else if (strcmp(json_object_get_string(v), "full")) {
      apiError(resp, MHD_HTTP_BAD_REQUEST, "invalid_request", EINVAL,
               "prealloc must be \"full\" or \"no\"");
      goto out;
    }
  }
  if (a.zerofill && !a.prealloc) {
    apiError(resp, MHD_HTTP_BAD_REQUEST, "invalid_request", EINVAL,
             "zerofill needs prealloc \"full\"");
    goto out;
  }
  if (apiBodyInt(req, resp, "ring_buffer_mb", 1, 1024, &n) < 0) {
    goto out;
  }
  a.ringBuffer = apiJsonGet(req->body, "ring_buffer_mb") ? n : 0;
  if (apiBodyInt(req, resp, "block_size", 512, 1048576, &n) < 0) {
    goto out;
  } else if (apiJsonGet(req->body, "block_size")) {
    if (n % 512) {
      apiError(resp, MHD_HTTP_BAD_REQUEST, "invalid_request", EINVAL,
               "block_size must be a multiple of 512");
      goto out;
    }
    a.blockSize = n;
  }
  if (apiBodyInt(req, resp, "io_timeout_sec", 1, 3600, &n) < 0) {
    goto out;
  }
  a.ioTimeout = apiJsonGet(req->body, "io_timeout_sec") ? n : 0;
  if (apiBodyGet(req, resp, "storage", json_type_string, &v) < 0) {
    goto out;
  } else if (v) {
    a.storage = json_object_get_string(v);
    if (strlen(a.storage) >= 255 || strchr(a.storage, '/')) {
      apiError(resp, MHD_HTTP_BAD_REQUEST, "invalid_request", EINVAL,
               "storage: the name of a file in /block-store");
      goto out;
    }
  }

  /* a first block makes the volume a block hosting one */
  if (!apiStartedVolume(volume, resp, &hosting)) {
    goto out;
  }

  apiCmd(req, cmd, sizeof(cmd));
  apiDaemonCreate(&a, cmd, &c);
  if (c.rpc == 0 && c.exit == EEXIST) {
    apiCreateExisting(req, resp, &a, &c);
  } else if (!apiCallFailed(&c, resp)) {
    apiVolumesInvalidate();
    apiAccepted(req, resp, volume, &c);
  }
  apiCallFree(&c);

 out:
  free(hostlist);
}


static void
apiV1BlockDelete(apiRequest *req, apiResponse *resp)
{
  const char *volume = req->seg[1];
  const char *block = req->seg[3];
  char cmd[2048];
  bool hosting;
  apiCall c;


  if (req->body && json_object_object_length(req->body)) {
    apiError(resp, MHD_HTTP_BAD_REQUEST, "invalid_request", EINVAL,
             "delete takes no body: unlink_storage and force are query arguments");
    return;
  }
  if (!apiStartedVolume(volume, resp, &hosting)) {
    return;
  }
  if (!hosting) {
    /* no blocks at all: it is gone, which is what was asked */
    apiRespond(resp, MHD_HTTP_NO_CONTENT, NULL);
    return;
  }

  apiCmd(req, cmd, sizeof(cmd));
  apiDaemonDelete(volume, block, apiQueryBoolDef(req, "unlink_storage", true),
                  apiQueryBool(req, "force"), cmd, &c);
  if (c.rpc == 0 && c.exit == ENOENT) {
    apiRespond(resp, MHD_HTTP_NO_CONTENT, NULL);
  } else if (!apiCallFailed(&c, resp)) {
    apiVolumesInvalidate();
    apiAccepted(req, resp, volume, &c);
  }
  apiCallFree(&c);
}


static const char *const apiPatchKeys[] = {
  "size_bytes", "force", "auth", "prio_path", NULL
};


static void
apiV1BlockPatch(apiRequest *req, apiResponse *resp)
{
  const char *volume = req->seg[1];
  const char *block = req->seg[3];
  json_object *auth = NULL, *prio = NULL;
  long long size = 0;
  bool force = false, enable = false;
  int changes = 0;
  char cmd[2048];
  apiCall c;


  if (!req->body || !apiBodyKeys(req, resp, apiPatchKeys)) {
    if (!req->body) {
      apiError(resp, MHD_HTTP_BAD_REQUEST, "invalid_request", EINVAL,
               "a JSON body with size_bytes, auth or prio_path is needed");
    }
    return;
  }
  if (apiBodyInt(req, resp, "size_bytes", 1, LLONG_MAX, &size) < 0 ||
      apiBodyBool(req, resp, "force", &force) < 0 ||
      apiBodyGet(req, resp, "auth", json_type_object, &auth) < 0 ||
      apiBodyGet(req, resp, "prio_path", json_type_string, &prio) < 0) {
    return;
  }
  changes = !!apiJsonGet(req->body, "size_bytes") + !!auth + !!prio;
  if (changes != 1) {
    apiError(resp, MHD_HTTP_BAD_REQUEST, "invalid_request", EINVAL,
             "one change per request: size_bytes, auth or prio_path");
    return;
  }
  if (auth) {
    json_object *e = apiJsonGet(auth, "enabled");

    if (json_object_object_length(auth) != 1 || !e ||
        !json_object_is_type(e, json_type_boolean)) {
      apiError(resp, MHD_HTTP_BAD_REQUEST, "invalid_request", EINVAL,
               "auth must be {\"enabled\": true|false}");
      return;
    }
    if (apiJsonGet(req->body, "force")) {
      apiError(resp, MHD_HTTP_BAD_REQUEST, "invalid_request", EINVAL,
               "force is for size_bytes and prio_path");
      return;
    }
    enable = json_object_get_boolean(e);
  }
  if (prio && !apiAddrOk(json_object_get_string(prio))) {
    apiError(resp, MHD_HTTP_BAD_REQUEST, "invalid_request", EINVAL,
             "prio_path: the IP address of one of the hosts of the block");
    return;
  }
  if (!apiBlockHostingVolume(volume, resp)) {
    return;
  }

  if (!prio && !auth && !force) {
    /* shrinking cuts off data: the daemon refuses with a generic error */
    apiDaemonInfo(volume, block, &c);
    if (!c.rpc && !c.exit && c.json &&
        apiJsonInt(c.json, "SIZE-BYTES", 0) > size) {
      apiError(resp, MHD_HTTP_CONFLICT, "force_required", 0,
               "block %s/%s has %lld bytes, shrinking it to %lld loses the "
               "data at its end: send \"force\": true", volume, block,
               apiJsonInt(c.json, "SIZE-BYTES", 0), size);
      apiCallFree(&c);
      return;
    }
    apiCallFree(&c);
  }

  apiCmd(req, cmd, sizeof(cmd));
  if (prio) {
    apiDaemonPrio(volume, block, json_object_get_string(prio), force, cmd, &c);
  } else if (auth) {
    apiDaemonAuth(volume, block, enable, cmd, &c);
  } else {
    apiDaemonResize(volume, block, size, force, cmd, &c);
  }
  if (!apiCallFailed(&c, resp)) {
    if (prio || auth) {
      apiRespondBlock(resp, volume, block);
    } else {
      apiAccepted(req, resp, volume, &c);
    }
  } else if (resp->status == MHD_HTTP_NOT_FOUND) {
    apiError(resp, MHD_HTTP_NOT_FOUND, "block_not_found", ENOENT,
             "block %s/%s does not exist", volume, block);
  }
  apiCallFree(&c);
}


static const char *const apiRebalanceKeys[] = {"dry_run", NULL};


static void
apiV1Rebalance(apiRequest *req, apiResponse *resp)
{
  const char *volume = req->seg[1];
  bool dryRun = false;
  char cmd[2048];
  apiCall c;


  if (!apiBodyKeys(req, resp, apiRebalanceKeys) ||
      apiBodyBool(req, resp, "dry_run", &dryRun) < 0 ||
      !apiBlockHostingVolume(volume, resp)) {
    return;
  }
  apiCmd(req, cmd, sizeof(cmd));
  apiDaemonRebalance(volume, dryRun, cmd, &c);
  if (!apiCallFailed(&c, resp)) {
    if (apiJsonStr(c.json, "OPERATION-ID")) {
      apiAccepted(req, resp, volume, &c);
    } else {
      /* dry run: the plan */
      apiRespond(resp, MHD_HTTP_OK, apiV1Generic(c.json));
    }
  }
  apiCallFree(&c);
}


static const char *const apiMaintKeys[] = {
  "action", "force", "rebalance", "confirm", NULL
};


static void
apiV1Maintenance(apiRequest *req, apiResponse *resp)
{
  const char *volume = req->seg[1];
  const char *host = req->seg[3];
  json_object *action;
  bool force = false, rebalance = false, confirm = false, start;
  char cmd[2048];
  apiCall c;


  if (!req->body || !apiBodyKeys(req, resp, apiMaintKeys)) {
    if (!req->body) {
      apiError(resp, MHD_HTTP_BAD_REQUEST, "invalid_request", EINVAL,
               "a JSON body with action \"start\" or \"stop\" is needed");
    }
    return;
  }
  if (apiBodyGet(req, resp, "action", json_type_string, &action) != 1 ||
      (strcmp(json_object_get_string(action), "start") &&
       strcmp(json_object_get_string(action), "stop"))) {
    if (resp->status != MHD_HTTP_BAD_REQUEST) {
      apiError(resp, MHD_HTTP_BAD_REQUEST, "invalid_request", EINVAL,
               "action must be \"start\" or \"stop\"");
    }
    return;
  }
  start = !strcmp(json_object_get_string(action), "start");
  if (apiBodyBool(req, resp, "force", &force) < 0 ||
      apiBodyBool(req, resp, "rebalance", &rebalance) < 0 ||
      apiBodyBool(req, resp, "confirm", &confirm) < 0) {
    return;
  }
  if (start && rebalance) {
    apiError(resp, MHD_HTTP_BAD_REQUEST, "invalid_request", EINVAL,
             "rebalance is for stop");
    return;
  }
  if (!apiBlockHostingVolume(volume, resp)) {
    return;
  }

  apiCmd(req, cmd, sizeof(cmd));
  apiDaemonMaint(volume, host, start, force, rebalance, confirm, cmd, &c);
  if (!apiCallFailed(&c, resp)) {
    apiAccepted(req, resp, volume, &c);
  }
  apiCallFree(&c);
}


static void
apiV1Index(apiRequest *req, apiResponse *resp)
{
  json_object *jo = json_object_new_object();


  json_object_object_add(jo, "api_version", json_object_new_string(API_VERSION));
  json_object_object_add(jo, "apid_version", json_object_new_string(PACKAGE_VERSION));
  apiRespond(resp, MHD_HTTP_OK, jo);
}


/* ------------------------------------------------------------------------
 * routes
 * ------------------------------------------------------------------------ */

typedef void (*apiHandlerFn)(apiRequest *req, apiResponse *resp);

typedef struct apiRoute {
  const char  *pattern[API_MAX_SEGMENTS];  /* "*" matches one segment */
  size_t       nseg;
  apiHandlerFn get;
  apiHandlerFn post;
  apiHandlerFn patch;
  apiHandlerFn del;
  bool         daemon;                     /* needs a compatible daemon */
  const char  *allow;                      /* the methods above, for 405 */
} apiRoute;

static const apiRoute apiRoutes[] = {
  {{NULL},                              0, apiV1Index, NULL, NULL, NULL, false, "GET"},
  {{"healthz"},                         1, apiV1Health, NULL, NULL, NULL, false, "GET"},
  {{"version"},                         1, apiV1Version, NULL, NULL, NULL, false, "GET"},
  {{"volumes"},                         1, apiV1Volumes, NULL, NULL, NULL, true, "GET"},
  {{"volumes", "*"},                    2, apiV1VolumeOne, NULL, NULL, NULL, true, "GET"},
  {{"volumes", "*", "blocks"},          3, apiV1Blocks, apiV1BlockCreate, NULL, NULL,
                                           true, "GET, POST"},
  {{"volumes", "*", "blocks", "*"},     4, apiV1Block1, NULL, apiV1BlockPatch,
                                           apiV1BlockDelete, true, "GET, PATCH, DELETE"},
  {{"volumes", "*", "rebalance"},       3, NULL, apiV1Rebalance, NULL, NULL, true, "POST"},
  {{"volumes", "*", "hosts"},           3, apiV1Hosts, NULL, NULL, NULL, true, "GET"},
  {{"volumes", "*", "hosts", "*"},      4, apiV1Hosts, NULL, NULL, NULL, true, "GET"},
  {{"volumes", "*", "hosts", "*", "maintenance"},
                                        5, NULL, apiV1Maintenance, NULL, NULL, true, "POST"},
  {{"volumes", "*", "operations"},      3, apiV1Operations, NULL, NULL, NULL, true, "GET"},
  {{"volumes", "*", "operations", "*"}, 4, apiV1Operation1, NULL, NULL, NULL, true, "GET"},
};


void
apiV1Handle(apiRequest *req, apiResponse *resp)
{
  const apiRoute *r = NULL;
  apiHandlerFn fn;
  size_t i, j;


  for (i = 0; i < sizeof(apiRoutes) / sizeof(apiRoutes[0]) && !r; i++) {
    if (apiRoutes[i].nseg != req->nseg) {
      continue;
    }
    for (j = 0; j < req->nseg; j++) {
      if (strcmp(apiRoutes[i].pattern[j], "*") &&
          strcmp(apiRoutes[i].pattern[j], req->seg[j])) {
        break;
      }
    }
    if (j == req->nseg) {
      r = &apiRoutes[i];
    }
  }

  if (!r) {
    apiError(resp, MHD_HTTP_NOT_FOUND, "not_found", 0, "no such resource: %s",
             req->url);
    return;
  }
  fn = !strcmp(req->method, "GET") ? r->get :
       !strcmp(req->method, "POST") ? r->post :
       !strcmp(req->method, "PATCH") ? r->patch :
       !strcmp(req->method, "DELETE") ? r->del : NULL;
  if (!fn) {
    resp->allow = r->allow;
    apiError(resp, MHD_HTTP_METHOD_NOT_ALLOWED, "method_not_allowed", 0,
             "%s is not allowed on %s", req->method, req->url);
    return;
  }
  if (req->body && fn == r->get) {
    apiError(resp, MHD_HTTP_BAD_REQUEST, "invalid_request", 0,
             "GET takes no request body");
    return;
  }
  if (!apiCheckNames(req, resp)) {
    return;
  }
  if (r->daemon && !apiDaemonCompatible(resp, false)) {
    return;
  }
  fn(req, resp);
}
