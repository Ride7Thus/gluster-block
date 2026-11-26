/*
  Copyright (c) 2026 gluster-block contributors
  This file is part of gluster-block.

  This file is licensed to you under your choice of the GNU Lesser
  General Public License, version 3 or any later version (LGPLv3 or
  later), or the GNU General Public License, version 2 (GPLv2), in all
  cases as published by the Free Software Foundation.
*/

/*
 * 'gluster-block volumes': the gluster volumes of the cluster and whether
 * they host block devices.
 *
 * The volumes come from 'gluster volume info' of the local glusterd. A
 * started volume hosts blocks when it has the metadata directory; it is only
 * looked at, not created as 'list' or 'info' would do.
 */

# include  "block_common.h"

# include  <sys/wait.h>


# define  GB_GLUSTER_VOLINFO   "gluster --mode=script volume info 2>&1"
# define  GB_VOLINFO_MAX       (16 * 1024 * 1024)


typedef struct gbVolume {
  char    name[256];
  char    type[64];
  char    status[32];
  char    bricks_count[64];      /* as gluster prints it: "1 x (2 + 1) = 3" */
  char  **bricks;
  size_t  nbricks;
  int     hosting;               /* 1 yes, 0 no, -1 unknown */
  size_t  nblocks;
  char    error[256];
} gbVolume;


static void
gbVolumesFree(gbVolume *vols, size_t n)
{
  size_t i, j;


  for (i = 0; i < n; i++) {
    for (j = 0; j < vols[i].nbricks; j++) {
      GB_FREE(vols[i].bricks[j]);
    }
    GB_FREE(vols[i].bricks);
  }
  GB_FREE(vols);
}


/* whole output of @cmd, and its exit status in @status */
static char *
gbVolumesRun(const char *cmd, int *status)
{
  FILE *fp;
  char *buf = NULL, *tmp;
  size_t len = 0, cap = 0;
  size_t n;


  fp = popen(cmd, "r");
  if (!fp) {
    *status = -1;
    return NULL;
  }
  do {
    if (len + 4096 + 1 > cap) {
      cap = cap ? cap * 2 : 65536;
      if (cap > GB_VOLINFO_MAX || !(tmp = realloc(buf, cap))) {
        break;
      }
      buf = tmp;
    }
    n = fread(buf + len, 1, cap - len - 1, fp);
    len += n;
  } while (n > 0);
  if (buf) {
    buf[len] = '\0';
  }
  *status = pclose(fp);
  return buf;
}


static void
gbTrim(char *s)
{
  size_t n = strlen(s);


  while (n && isspace((unsigned char)s[n - 1])) {
    s[--n] = '\0';
  }
}


static int
gbVolumesParse(char *text, gbVolume **out, size_t *count)
{
  gbVolume *vols = NULL, *v = NULL;
  size_t n = 0;
  char *line, *save = NULL, *val;


  for (line = strtok_r(text, "\n", &save); line;
       line = strtok_r(NULL, "\n", &save)) {
    gbTrim(line);
    val = strchr(line, ':');
    if (!val) {
      continue;
    }
    *val++ = '\0';
    while (*val == ' ') {
      val++;
    }

    if (!strcmp(line, "Volume Name")) {
      if (GB_REALLOC_N(vols, n + 1) < 0) {
        goto nomem;
      }
      v = &vols[n++];
      memset(v, 0, sizeof(*v));
      v->hosting = -1;
      GB_STRCPYSTATIC(v->name, val);
      continue;
    }
    if (!v) {
      continue;
    }
    if (!strcmp(line, "Type")) {
      GB_STRCPYSTATIC(v->type, val);
    } else if (!strcmp(line, "Status")) {
      GB_STRCPYSTATIC(v->status, val);
    } else if (!strcmp(line, "Number of Bricks")) {
      GB_STRCPYSTATIC(v->bricks_count, val);
    } else if (!strncmp(line, "Brick", 5) && isdigit((unsigned char)line[5])) {
      if (GB_REALLOC_N(v->bricks, v->nbricks + 1) < 0) {
        goto nomem;
      }
      /* "host:/path", maybe followed by " (arbiter)" */
      if (GB_STRDUP(v->bricks[v->nbricks], val) < 0) {
        goto nomem;
      }
      v->nbricks++;
    }
  }

  *out = vols;
  *count = n;
  return 0;

 nomem:
  gbVolumesFree(vols, n);
  return -1;
}


/* does the started volume host blocks, without creating anything on it */
static void
gbVolumeProbe(gbVolume *v)
{
  struct glfs *glfs;
  struct glfs_fd *dfd;
  struct dirent *entry;
  int errCode = 0;
  char *errMsg = NULL;


  if (strcmp(v->status, "Started")) {
    return;
  }

  glfs = glusterBlockVolumeInit(v->name, &errCode, &errMsg);
  if (!glfs) {
    snprintf(v->error, sizeof(v->error), "%s",
             errMsg ? errMsg : "can not connect to the volume");
    GB_FREE(errMsg);
    return;
  }

  dfd = glfs_opendir(glfs, GB_METADIR);
  if (!dfd) {
    if (errno == ENOENT) {
      v->hosting = 0;
    } else {
      snprintf(v->error, sizeof(v->error), "can not read %s: %s",
               GB_METADIR, strerror(errno));
    }
    return;
  }
  v->hosting = 1;
  while ((entry = glfs_readdir(dfd))) {
    if (!strchr(entry->d_name, '.')) {
      v->nblocks++;
    }
  }
  glfs_closedir(dfd);
}


static int
gbVolumesFormatJson(blockVolumesCli *blk, gbVolume *vols, size_t n,
                    blockResponse *reply)
{
  json_object *jo = json_object_new_object();
  json_object *ja = json_object_new_array();
  size_t i, j;


  for (i = 0; i < n; i++) {
    json_object *jv = json_object_new_object();
    json_object *jb = json_object_new_array();

    json_object_object_add(jv, "NAME", GB_JSON_OBJ_TO_STR(vols[i].name));
    json_object_object_add(jv, "TYPE", GB_JSON_OBJ_TO_STR(vols[i].type));
    json_object_object_add(jv, "STATUS", GB_JSON_OBJ_TO_STR(vols[i].status));
    json_object_object_add(jv, "BRICK-COUNT",
                           GB_JSON_OBJ_TO_STR(vols[i].bricks_count));
    for (j = 0; j < vols[i].nbricks; j++) {
      json_object_array_add(jb, GB_JSON_OBJ_TO_STR(vols[i].bricks[j]));
    }
    json_object_object_add(jv, "BRICKS", jb);
    json_object_object_add(jv, "BLOCK-HOSTING", vols[i].hosting < 0 ? NULL :
                           json_object_new_boolean(vols[i].hosting));
    if (vols[i].hosting > 0) {
      json_object_object_add(jv, "BLOCKS",
                             json_object_new_int64(vols[i].nblocks));
    }
    json_object_object_add(jv, "ERROR", GB_JSON_OBJ_TO_STR(vols[i].error));
    json_object_array_add(ja, jv);
  }
  json_object_object_add(jo, "VOLUMES", ja);
  json_object_object_add(jo, "RESULT", GB_JSON_OBJ_TO_STR("SUCCESS"));

  GB_ASPRINTF(&reply->out, "%s\n", json_object_to_json_string_ext(jo,
              mapJsonFlagToJsonCstring(blk->json_resp)));
  json_object_put(jo);

  return reply->out ? 0 : -1;
}


static int
gbVolumesFormatPlain(gbVolume *vols, size_t n, blockResponse *reply)
{
  char *out = NULL, *tmp;
  size_t i;


  if (GB_ASPRINTF(&out, "%-24s %-24s %-8s %-8s %s\n", "VOLUME", "TYPE",
                  "STATUS", "BLOCKS", "BRICKS") == -1) {
    return -1;
  }
  for (i = 0; i < n; i++) {
    char blocks[32];

    if (vols[i].hosting > 0) {
      snprintf(blocks, sizeof(blocks), "%zu", vols[i].nblocks);
    } else {
      snprintf(blocks, sizeof(blocks), "%s", vols[i].hosting ? "?" : "-");
    }
    if (GB_ASPRINTF(&tmp, "%s%-24s %-24s %-8s %-8s %s%s%s\n", out,
                    vols[i].name, vols[i].type, vols[i].status, blocks,
                    vols[i].bricks_count, vols[i].error[0] ? "  " : "",
                    vols[i].error) == -1) {
      GB_FREE(out);
      return -1;
    }
    GB_FREE(out);
    out = tmp;
  }
  reply->out = out;
  return 0;
}


static blockResponse *
block_volumes_cli_1_svc_st(blockVolumesCli *blk, struct svc_req *rqstp)
{
  blockResponse *reply = NULL;
  gbVolume *vols = NULL;
  size_t n = 0, i;
  char *text = NULL;
  char *errMsg = NULL;
  int status = 0;
  int errCode = 0;


  LOG("mgmt", GB_LOG_INFO, "%s", "volumes cli request");

  if (GB_ALLOC(reply) < 0) {
    return NULL;
  }

  text = gbVolumesRun(GB_GLUSTER_VOLINFO, &status);
  if (!text || status) {
    errCode = GB_DEFAULT_ERRCODE;
    if (status > 0 && WIFEXITED(status) && WEXITSTATUS(status) != 0) {
      errCode = WEXITSTATUS(status);
    }
    if (text) {
      gbTrim(text);
    }
    GB_ASPRINTF(&errMsg, "'gluster volume info' failed: %s",
                text && text[0] ? text : "no output");
    goto out;
  }

  /* "No volumes present" has no "Volume Name:" line, n stays 0 */
  if (gbVolumesParse(text, &vols, &n)) {
    errCode = ENOMEM;
    goto out;
  }
  for (i = 0; i < n; i++) {
    gbVolumeProbe(&vols[i]);
  }

  if (blk->json_resp) {
    errCode = gbVolumesFormatJson(blk, vols, n, reply) ? ENOMEM : 0;
  } else {
    errCode = gbVolumesFormatPlain(vols, n, reply) ? ENOMEM : 0;
  }

 out:
  LOG("mgmt", ((!!errCode) ? GB_LOG_ERROR : GB_LOG_INFO),
      "volumes cli return %s, %zu volumes", errCode ? "failure" : "success", n);
  if (errCode) {
    GB_FREE(reply->out);
    blockFormatErrorResponse(VOLUMES_SRV, blk->json_resp, errCode,
                             errMsg ? errMsg : GB_DEFAULT_ERRMSG, reply);
  }
  reply->exit = errCode;

  free(text);
  GB_FREE(errMsg);
  gbVolumesFree(vols, n);

  return reply;
}


bool_t
block_volumes_cli_1_svc(blockVolumesCli *blk, blockResponse *reply,
                        struct svc_req *rqstp)
{
  int ret;

  GB_RPC_CALL(volumes_cli, blk, reply, rqstp, ret);
  return ret;
}
