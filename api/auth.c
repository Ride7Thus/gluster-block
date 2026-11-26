/*
  Copyright (c) 2026 gluster-block contributors
  This file is part of gluster-block.

  This file is licensed to you under your choice of the GNU Lesser
  General Public License, version 3 or any later version (LGPLv3 or
  later), or the GNU General Public License, version 2 (GPLv2), in all
  cases as published by the Free Software Foundation.
*/

/*
 * Who may use the TCP (TLS) listener of gluster-block-apid.
 *
 * The unix socket is root only, its clients are admins. On TCP a client is
 * known by its certificate, signed by GB_API_TLS_CA (mutual TLS, meant for
 * operators), or by a bearer token (meant for scripts). GB_API_TOKENS_FILE
 * gives them roles:
 *
 *   # kind  name                     role   secret
 *   token   backup-script            read   sha256:<hex of the sha256 of the token>
 *   cert    operator.example.com     admin
 *
 * read may GET, admin may do everything. The file is read at start and on
 * SIGHUP (systemctl reload gluster-block-apid); a token is made with
 * 'gluster-block-apid --new-token <name> <role>', only its hash is kept.
 */

# ifndef _GNU_SOURCE
#  define _GNU_SOURCE
# endif

# include  "config.h"
# include  "apid.h"

# include  <errno.h>
# include  <pthread.h>
# include  <stdio.h>
# include  <stdlib.h>
# include  <string.h>
# include  <strings.h>

# include  <gnutls/gnutls.h>
# include  <gnutls/crypto.h>
# include  <gnutls/x509.h>
# include  <microhttpd.h>


# define  API_SHA256_LEN    32
# define  API_TOKEN_BYTES   32
# define  API_TOKEN_PREFIX  "gbt_"


typedef struct apiPrincipal {
  char           kind;                    /* 't' token, 'c' certificate */
  char           name[256];
  apiRole        role;
  unsigned char  hash[API_SHA256_LEN];    /* tokens */
} apiPrincipal;

static pthread_mutex_t apiAuthLock = PTHREAD_MUTEX_INITIALIZER;
static apiPrincipal *apiPrincipals;
static size_t apiNPrincipals;


static int
apiRoleParse(const char *s, apiRole *role)
{
  if (!strcmp(s, "admin")) {
    *role = API_ROLE_ADMIN;
  } else if (!strcmp(s, "read")) {
    *role = API_ROLE_READ;
  } else {
    return -1;
  }
  return 0;
}


const char *
apiRoleName(apiRole role)
{
  return role == API_ROLE_ADMIN ? "admin" : role == API_ROLE_READ ? "read" : "none";
}


static int
apiHexDecode(const char *hex, unsigned char *out, size_t len)
{
  size_t i;


  if (strlen(hex) != len * 2) {
    return -1;
  }
  for (i = 0; i < len; i++) {
    unsigned int b;

    if (sscanf(hex + 2 * i, "%2x", &b) != 1) {
      return -1;
    }
    out[i] = b;
  }
  return 0;
}


static void
apiHexEncode(const unsigned char *in, size_t len, char *out)
{
  size_t i;


  for (i = 0; i < len; i++) {
    sprintf(out + 2 * i, "%02x", in[i]);
  }
  out[2 * len] = '\0';
}


int
apiAuthLoad(const char *path)
{
  apiPrincipal *list = NULL, *tmp;
  size_t n = 0;
  char line[1024];
  int lineno = 0;
  int bad = 0;
  FILE *f;


  f = fopen(path, "r");
  if (!f) {
    if (errno == ENOENT) {
      apiLog(API_LOG_INFO, "no %s: only the unix socket has clients", path);
    } else {
      apiLog(API_LOG_ERROR, "can not read %s: %s", path, strerror(errno));
    }
  } else {
    while (fgets(line, sizeof(line), f)) {
      char kind[16], name[256], role[16], secret[128];
      int fields;
      apiPrincipal p;

      lineno++;
      if (line[strspn(line, " \t")] == '#' || line[strspn(line, " \t\r\n")] == '\0') {
        continue;
      }
      memset(&p, 0, sizeof(p));
      fields = sscanf(line, "%15s %255s %15s %127s", kind, name, role, secret);
      if (fields < 3 || apiRoleParse(role, &p.role)) {
        apiLog(API_LOG_ERROR, "%s:%d: expected '<token|cert> <name> <admin|read> "
               "[sha256:<hex>]', line ignored", path, lineno);
        bad++;
        continue;
      }
      snprintf(p.name, sizeof(p.name), "%s", name);
      if (!strcmp(kind, "token")) {
        p.kind = 't';
        if (fields != 4 || strncmp(secret, "sha256:", 7) ||
            apiHexDecode(secret + 7, p.hash, sizeof(p.hash))) {
          apiLog(API_LOG_ERROR, "%s:%d: token %s needs sha256:<64 hex digits>, "
                 "line ignored", path, lineno, name);
          bad++;
          continue;
        }
      } else if (!strcmp(kind, "cert")) {
        p.kind = 'c';
      } else {
        apiLog(API_LOG_ERROR, "%s:%d: unknown kind '%s', line ignored", path,
               lineno, kind);
        bad++;
        continue;
      }
      tmp = realloc(list, (n + 1) * sizeof(*list));
      if (!tmp) {
        break;
      }
      list = tmp;
      list[n++] = p;
    }
    fclose(f);
  }

  pthread_mutex_lock(&apiAuthLock);
  free(apiPrincipals);
  apiPrincipals = list;
  apiNPrincipals = n;
  pthread_mutex_unlock(&apiAuthLock);

  apiLog(API_LOG_INFO, "%zu clients of the TLS listener from %s%s", n, path,
         bad ? " (some lines ignored)" : "");
  return bad ? -1 : 0;
}


/* constant time, the hashes are not secret but the habit is good */
static bool
apiHashEqual(const unsigned char *a, const unsigned char *b)
{
  unsigned char d = 0;
  size_t i;


  for (i = 0; i < API_SHA256_LEN; i++) {
    d |= a[i] ^ b[i];
  }
  return d == 0;
}


static bool
apiAuthToken(const char *token, char *who, size_t len, apiRole *role)
{
  unsigned char hash[API_SHA256_LEN];
  bool found = false;
  size_t i;


  if (gnutls_hash_fast(GNUTLS_DIG_SHA256, token, strlen(token), hash) < 0) {
    return false;
  }
  pthread_mutex_lock(&apiAuthLock);
  for (i = 0; i < apiNPrincipals && !found; i++) {
    if (apiPrincipals[i].kind == 't' && apiHashEqual(apiPrincipals[i].hash, hash)) {
      snprintf(who, len, "token:%s", apiPrincipals[i].name);
      *role = apiPrincipals[i].role;
      found = true;
    }
  }
  pthread_mutex_unlock(&apiAuthLock);
  return found;
}


/* common name of a client certificate the CA signed, "" when there is none */
static void
apiPeerCertName(struct MHD_Connection *conn, char *cn, size_t len)
{
  const union MHD_ConnectionInfo *ci;
  gnutls_session_t session;
  const gnutls_datum_t *certs;
  gnutls_x509_crt_t crt;
  unsigned int ncerts = 0, status = 0;
  size_t cnlen = len;


  cn[0] = '\0';
  ci = MHD_get_connection_info(conn, MHD_CONNECTION_INFO_GNUTLS_SESSION);
  if (!ci || !ci->tls_session) {
    return;
  }
  session = (gnutls_session_t)ci->tls_session;

  certs = gnutls_certificate_get_peers(session, &ncerts);
  if (!certs || !ncerts) {
    return;
  }
  /* signed by GB_API_TLS_CA, valid now */
  if (gnutls_certificate_verify_peers2(session, &status) < 0 || status) {
    apiLog(API_LOG_WARNING, "client certificate rejected (status 0x%x)", status);
    return;
  }
  if (gnutls_x509_crt_init(&crt) < 0) {
    return;
  }
  if (gnutls_x509_crt_import(crt, &certs[0], GNUTLS_X509_FMT_DER) >= 0 &&
      gnutls_x509_crt_get_dn_by_oid(crt, GNUTLS_OID_X520_COMMON_NAME, 0, 0,
                                    cn, &cnlen) >= 0) {
    cn[len - 1] = '\0';
  } else {
    cn[0] = '\0';
  }
  gnutls_x509_crt_deinit(crt);
}


bool
apiAuthenticate(struct MHD_Connection *conn, char *who, size_t len,
                apiRole *role)
{
  const char *hdr;
  char cn[256];
  size_t i;
  bool found = false;


  *role = API_ROLE_NONE;
  snprintf(who, len, "anonymous");

  apiPeerCertName(conn, cn, sizeof(cn));
  if (cn[0]) {
    snprintf(who, len, "cert:%s", cn);
    pthread_mutex_lock(&apiAuthLock);
    for (i = 0; i < apiNPrincipals && !found; i++) {
      if (apiPrincipals[i].kind == 'c' && !strcmp(apiPrincipals[i].name, cn)) {
        *role = apiPrincipals[i].role;
        found = true;
      }
    }
    pthread_mutex_unlock(&apiAuthLock);
    if (found) {
      return true;
    }
    /* a valid certificate without a role: a token may still be given */
  }

  hdr = MHD_lookup_connection_value(conn, MHD_HEADER_KIND,
                                    MHD_HTTP_HEADER_AUTHORIZATION);
  if (hdr && !strncasecmp(hdr, "Bearer ", 7)) {
    const char *token = hdr + 7;

    while (*token == ' ') {
      token++;
    }
    if (apiAuthToken(token, who, len, role)) {
      return true;
    }
    snprintf(who, len, "bad-token");
  }
  return false;
}


int
apiNewToken(const char *name, const char *role)
{
  unsigned char raw[API_TOKEN_BYTES], hash[API_SHA256_LEN];
  char token[sizeof(API_TOKEN_PREFIX) + 2 * API_TOKEN_BYTES];
  char hex[2 * API_SHA256_LEN + 1];
  apiRole r;


  if (apiRoleParse(role, &r) || !*name || strpbrk(name, " \t#")) {
    fprintf(stderr, "usage: gluster-block-apid --new-token <name> <admin|read>\n");
    return 1;
  }
  if (gnutls_rnd(GNUTLS_RND_KEY, raw, sizeof(raw)) < 0) {
    fprintf(stderr, "gluster-block-apid: no random bytes\n");
    return 1;
  }
  strcpy(token, API_TOKEN_PREFIX);
  apiHexEncode(raw, sizeof(raw), token + strlen(API_TOKEN_PREFIX));
  if (gnutls_hash_fast(GNUTLS_DIG_SHA256, token, strlen(token), hash) < 0) {
    return 1;
  }
  apiHexEncode(hash, sizeof(hash), hex);

  printf("token: %s\n"
         "add this line to %s and run 'systemctl reload gluster-block-apid':\n"
         "token %s %s sha256:%s\n", token, GB_API_TOKENS_DEF, name, role, hex);
  return 0;
}
