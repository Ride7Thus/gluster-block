/*
  Copyright (c) 2026 gluster-block contributors
  This file is part of gluster-block.

  This file is licensed to you under your choice of the GNU Lesser
  General Public License, version 3 or any later version (LGPLv3 or
  later), or the GNU General Public License, version 2 (GPLv2), in all
  cases as published by the Free Software Foundation.
*/

/*
 * 'gluster-block modify <volume/block> prio-path <host> [force]'
 *
 * Every host of a block has all TPGs of its target, one per host, and the
 * LUN of each TPG is in one of two ALUA groups: glfs_tg_pt_gp_ao
 * (active/optimized, the prio path) or glfs_tg_pt_gp_ano. Moving the prio
 * path puts the LUN of the TPG of the new host into the AO group and all
 * others into the ANO group on every host, so all of them report the same
 * states to the initiator, then records PRIOPATH in the metadata.
 */

# ifndef _GNU_SOURCE
#  define _GNU_SOURCE
# endif

# include  <glob.h>
# include  <stdarg.h>

# include  "block_common.h"
# include  "block_ha.h"


# define  GB_CONFIGFS_CORE       "/sys/kernel/config/target/core"
# define  GB_CONFIGFS_ISCSI      "/sys/kernel/config/target/iscsi"
# define  GB_ALUA_AO             "glfs_tg_pt_gp_ao"
# define  GB_ALUA_ANO            "glfs_tg_pt_gp_ano"
# define  GB_ALUA_SET_DONE       "Parameter alua_tg_pt_gp_name is now"


/* ------------------------------------------------------------------------
 * Remote side: regroup the LUNs of the target on this host
 * ------------------------------------------------------------------------ */

static void
gbReadLine(const char *path, char *out, size_t len)
{
  FILE *fp;


  out[0] = '\0';
  fp = fopen(path, "r");
  if (!fp) {
    return;
  }
  if (fgets(out, len, fp)) {
    out[strcspn(out, "\n")] = '\0';
  }
  fclose(fp);
}


static char *
gbRunAll(const char *cmd)
{
  FILE *fp;
  char *out = NULL;
  size_t len = 0;
  size_t n;
  char buf[4096];


  fp = popen(cmd, "r");
  if (!fp) {
    return NULL;
  }
  while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
    char *tmp = realloc(out, len + n + 1);

    if (!tmp) {
      free(out);
      pclose(fp);
      return NULL;
    }
    out = tmp;
    memcpy(out + len, buf, n);
    len += n;
    out[len] = '\0';
  }
  pclose(fp);

  return out ? out : strdup("");
}


static size_t
gbCountStr(const char *hay, const char *needle)
{
  size_t n = 0;


  while (hay && (hay = strstr(hay, needle))) {
    n++;
    hay += strlen(needle);
  }
  return n;
}


static blockResponse *
block_modify_prio_1_svc_st(blockModifyPrio *blk, struct svc_req *rqstp)
{
  blockResponse *reply = NULL;
  char *pattern = NULL;
  char *cmds = NULL;
  char *exec = NULL;
  char *out = NULL;
  char path[PATH_MAX];
  char cur[256];
  glob_t g;
  size_t i;
  size_t ncmds = 0;
  bool found = false;
  bool alua = false;


  LOG("mgmt", GB_LOG_INFO,
      "modify prio request, volume=%s blockname=%s filename=%s prio=%s",
      blk->volume, blk->block_name, blk->gbid, blk->prio_path);

  if (GB_ALLOC(reply) < 0) {
    return NULL;
  }
  reply->exit = -1;

  if (blockCheckBlockLoadedStatus(blk->block_name, blk->gbid, reply)) {
    if (!reply->out) {
      GB_ASPRINTF(&reply->out, "block %s is not loaded", blk->block_name);
    }
    if (reply->exit != GB_BLOCK_NOT_FOUND) {
      reply->exit = GB_BLOCK_NOT_LOADED;
    }
    goto out;
  }
  GB_FREE(reply->out);

  /* blocks created without load balancing have no ALUA groups */
  if (GB_ASPRINTF(&pattern, GB_CONFIGFS_CORE "/user_*/%s/alua/" GB_ALUA_AO,
                  blk->block_name) == -1) {
    goto nomem;
  }
  if (!glob(pattern, GLOB_ONLYDIR, NULL, &g)) {
    alua = g.gl_pathc > 0;
    globfree(&g);
  }
  GB_FREE(pattern);
  if (!alua) {
    reply->exit = EOPNOTSUPP;
    GB_ASPRINTF(&reply->out, "block %s has no ALUA groups, it was created "
                "without load balancing", blk->block_name);
    goto out;
  }

  if (GB_ASPRINTF(&pattern, GB_CONFIGFS_ISCSI "/%s%s/tpgt_*",
                  GB_TGCLI_IQN_PREFIX, blk->gbid) == -1) {
    goto nomem;
  }
  if (glob(pattern, GLOB_ONLYDIR, NULL, &g)) {
    GB_FREE(pattern);
    reply->exit = ENOENT;
    GB_ASPRINTF(&reply->out, "target of block %s has no TPGs", blk->block_name);
    goto out;
  }
  GB_FREE(pattern);

  for (i = 0; i < g.gl_pathc; i++) {
    char *num = strrchr(g.gl_pathv[i], '_');
    size_t tpg = 0;
    const char *want;
    char *tmp;

    if (!num || sscanf(num + 1, "%zu", &tpg) != 1) {
      continue;
    }
    if (blockConfigfsTpgHasPortal(g.gl_pathv[i], blk->prio_path)) {
      want = GB_ALUA_AO;
      found = true;
    } else {
      want = GB_ALUA_ANO;
    }

    snprintf(path, sizeof(path), "%s/lun/lun_0/alua_tg_pt_gp", g.gl_pathv[i]);
    gbReadLine(path, cur, sizeof(cur));
    if (!strcmp(cur, want)) {
      continue;
    }

    tmp = cmds;
    if (GB_ASPRINTF(&cmds, "%s%s/%s%s/tpg%zu/luns/lun0 set alua "
                    "alua_tg_pt_gp_name=%s\n", tmp ? tmp : "",
                    GB_TGCLI_ISCSI_PATH, GB_TGCLI_IQN_PREFIX, blk->gbid, tpg,
                    want) == -1) {
      GB_FREE(tmp);
      globfree(&g);
      goto nomem;
    }
    GB_FREE(tmp);
    ncmds++;
  }
  globfree(&g);

  if (!found) {
    reply->exit = ENOENT;
    GB_ASPRINTF(&reply->out, "no TPG of block %s has a portal of %s",
                blk->block_name, blk->prio_path);
    goto out;
  }

  if (!ncmds) {
    reply->exit = 0;
    GB_ASPRINTF(&reply->out, "prio path of %s is already %s",
                blk->block_name, blk->prio_path);
    goto out;
  }

  if (GB_ASPRINTF(&exec, "targetcli <<EOF\n%s" GB_TGCLI_GLFS_SAVE "\nexit\nEOF",
                  cmds, blk->block_name) == -1) {
    goto nomem;
  }
  LOG("mgmt", GB_LOG_DEBUG, "command: %s", exec);

  out = gbRunAll(exec);
  if (!out) {
    goto nomem;
  }
  if (gbCountStr(out, GB_ALUA_SET_DONE) != ncmds) {
    LOG("mgmt", GB_LOG_ERROR, "setting ALUA groups of %s/%s failed, "
        "targetcli:\n%s", blk->volume, blk->block_name, out);
    reply->exit = -1;
    GB_ASPRINTF(&reply->out, "setting ALUA groups of %s failed",
                blk->block_name);
    goto out;
  }

  reply->exit = 0;
  GB_ASPRINTF(&reply->out, "prio path of %s set to %s", blk->block_name,
              blk->prio_path);
  goto out;

 nomem:
  reply->exit = ENOMEM;
  GB_FREE(reply->out);
  GB_ASPRINTF(&reply->out, "modify prio: out of memory");

 out:
  GB_FREE(pattern);
  GB_FREE(cmds);
  GB_FREE(exec);
  free(out);

  return reply;
}


bool_t
block_modify_prio_1_svc(blockModifyPrio *blk, blockResponse *reply,
                        struct svc_req *rqstp)
{
  int ret;

  GB_RPC_CALL(modify_prio, blk, reply, rqstp, ret);
  return ret;
}


/* ------------------------------------------------------------------------
 * Cli side
 * ------------------------------------------------------------------------ */

typedef struct gbPrioHost {
  char   *addr;
  bool    reachable;
  char    error[256];
  blockRemoteObj obj;      /* remote call, obj.exit and obj.reply */
  bool    applied;
} gbPrioHost;


static void *
gbPrioProbe(void *data)
{
  gbPrioHost *h = data;

  h->reachable = blockHostReachable(h->addr, h->error, sizeof(h->error));
  return NULL;
}


static void *
gbPrioRemote(void *data)
{
  gbPrioHost *h = data;
  bool rpc_sent = false;

  GB_FREE(h->obj.reply);
  h->obj.exit = glusterBlockCallRPC_1(h->addr, h->obj.obj, MODIFY_PRIO_SRV,
                                      &rpc_sent, &h->obj.reply);
  if (h->obj.exit && !h->obj.reply) {
    GB_ASPRINTF(&h->obj.reply, "%s", rpc_sent ? "remote call failed" :
                "can not connect to gluster-blockd");
  }
  return NULL;
}


static void
gbPrioParallel(gbPrioHost *hs, size_t n, void *(*fn)(void *), bool all)
{
  pthread_t *tid = NULL;
  size_t i;


  if (GB_ALLOC_N(tid, n) < 0) {
    for (i = 0; i < n; i++) {
      if (all || hs[i].reachable) {
        fn(&hs[i]);
      }
    }
    return;
  }
  for (i = 0; i < n; i++) {
    if (all || hs[i].reachable) {
      pthread_create(&tid[i], NULL, fn, &hs[i]);
    }
  }
  for (i = 0; i < n; i++) {
    if (all || hs[i].reachable) {
      pthread_join(tid[i], NULL);
    }
  }
  GB_FREE(tid);
}


int
gbJoin(char **out, const char *sep, const char *fmt, ...)
{
  va_list ap;
  char *add = NULL;
  char *joined = NULL;


  va_start(ap, fmt);
  if (vasprintf(&add, fmt, ap) < 0) {
    va_end(ap);
    return -1;
  }
  va_end(ap);

  if (asprintf(&joined, "%s%s%s", *out ? *out : "", *out ? sep : "", add) < 0) {
    free(add);
    return -1;
  }
  free(add);
  free(*out);
  *out = joined;
  return 0;
}




static void
gbPrioFormat(blockModifyPrioCli *blk, int errCode, char *errMsg,
             gbPrioResult *r, blockResponse *reply)
{
  json_object *jo;


  if (errCode && !r->failed) {
    blockFormatErrorResponse(MODIFY_PRIO_SRV, blk->json_resp, errCode,
                             errMsg ? errMsg : GB_DEFAULT_ERRMSG, reply);
    return;
  }

  reply->exit = errCode;
  if (blk->json_resp) {
    jo = json_object_new_object();
    json_object_object_add(jo, "NAME", GB_JSON_OBJ_TO_STR(blk->block_name));
    json_object_object_add(jo, "VOLUME", GB_JSON_OBJ_TO_STR(blk->volume));
    json_object_object_add(jo, "PRIO PATH",
                           GB_JSON_OBJ_TO_STR(errCode ? r->previous : r->prio));
    if (!errCode && !r->unchanged) {
      json_object_object_add(jo, "PREVIOUS PRIO PATH",
                             GB_JSON_OBJ_TO_STR(r->previous));
    }
    if (r->success) {
      blockStr2arrayAddToJsonObj(jo, r->success, "SUCCESSFUL ON");
    }
    if (r->skipped) {
      blockStr2arrayAddToJsonObj(jo, r->skipped, "SKIPPED ON");
    }
    if (r->failed) {
      json_object_object_add(jo, "FAILED ON", GB_JSON_OBJ_TO_STR(r->failed));
    }
    if (r->rolledback) {
      blockStr2arrayAddToJsonObj(jo, r->rolledback, "ROLLED BACK ON");
    }
    if (errMsg) {
      json_object_object_add(jo, "errMsg", GB_JSON_OBJ_TO_STR(errMsg));
    }
    json_object_object_add(jo, "RESULT",
                           GB_JSON_OBJ_TO_STR(errCode ? "FAIL" : "SUCCESS"));
    GB_ASPRINTF(&reply->out, "%s\n", json_object_to_json_string_ext(jo,
                                    mapJsonFlagToJsonCstring(blk->json_resp)));
    json_object_put(jo);
    return;
  }

  GB_ASPRINTF(&reply->out,
              "NAME: %s\nPRIO PATH: %s%s\n%s%s%s%s%s%s%s%s%s%s%s%s%s%s%sRESULT: %s\n",
              blk->block_name, errCode ? r->previous : r->prio,
              r->unchanged ? " (unchanged)" : "",
              !errCode && !r->unchanged ? "PREVIOUS PRIO PATH: " : "",
              !errCode && !r->unchanged ? r->previous : "",
              !errCode && !r->unchanged ? "\n" : "",
              r->success ? "SUCCESSFUL ON: " : "", r->success ? r->success : "",
              r->success ? "\n" : "",
              r->skipped ? "SKIPPED ON: " : "", r->skipped ? r->skipped : "",
              r->skipped ? " (reload the block there once they are back)\n" : "",
              r->failed ? "FAILED ON: " : "", r->failed ? r->failed : "",
              r->failed ? "\n" : "",
              r->rolledback ? "ROLLED BACK ON: " : "",
              r->rolledback ? r->rolledback : "",
              r->rolledback ? "\n" : "",
              errCode ? "FAIL" : "SUCCESS");
}




gbReach *
gbReachFind(gbReach *reach, size_t nreach, const char *addr)
{
  size_t i;

  for (i = 0; i < nreach; i++) {
    if (!strcmp(reach[i].addr, addr)) {
      return &reach[i];
    }
  }
  return NULL;
}


/*
 * Move the prio path of one block to @prio, the volume metadata lock is held.
 * @reach: hosts probed already (rebalance), NULL to probe the hosts here.
 * @capsBlk: check the capability of the hosts for this request, NULL if the
 * caller did. Returns the error code, @r and @errMsg describe it.
 */
int
gbPrioMove(struct glfs *glfs, char *volume, char *block, MetaInfo *info,
           const char *prio, bool force, gbReach *reach, size_t nreach,
           void *capsBlk, gbPrioResult *r, char **errMsg)
{
  blockModifyPrio mobj = {{0},};
  blockModifyPrio rbobj = {{0},};
  blockServerDefPtr list = NULL;
  gbPrioHost *hs = NULL;
  size_t nhs = 0;
  size_t i;
  bool target = false;
  bool unreach_new = false;
  char *unreach = NULL;
  int errCode = 0;


  if (!info->prio_path[0]) {
    errCode = EOPNOTSUPP;
    GB_ASPRINTF(errMsg, "block %s/%s has no prio path, it was created "
                "without load balancing", volume, block);
    goto out;
  }
  r->previous = info->prio_path;
  r->prio = (char *)prio;

  if (GB_ALLOC_N(hs, info->nhosts) < 0) {
    errCode = ENOMEM;
    goto out;
  }
  for (i = 0; i < info->nhosts; i++) {
    if (!blockhostIsValid(info->list[i]->status)) {
      continue;
    }
    if (!strcmp(info->list[i]->addr, prio)) {
      target = true;
    }
    hs[nhs].addr = info->list[i]->addr;
    hs[nhs].obj.obj = &mobj;
    nhs++;
  }
  if (!target) {
    errCode = EINVAL;
    GB_ASPRINTF(errMsg, "host %s does not export block %s/%s", prio, volume,
                block);
    goto out;
  }

  if (!strcmp(info->prio_path, prio)) {
    r->unchanged = true;
    goto out;
  }

  /* who answers at all: a dead host would make every step wait */
  if (reach) {
    for (i = 0; i < nhs; i++) {
      gbReach *rh = gbReachFind(reach, nreach, hs[i].addr);

      hs[i].reachable = rh && rh->reachable;
      snprintf(hs[i].error, sizeof(hs[i].error), "%s",
               rh ? rh->error : "not probed");
    }
  } else {
    gbPrioParallel(hs, nhs, gbPrioProbe, true);
  }
  for (i = 0; i < nhs; i++) {
    if (hs[i].reachable) {
      continue;
    }
    if (!strcmp(hs[i].addr, prio)) {
      unreach_new = true;
    }
    if (gbJoin(&unreach, ", ", "%s (%s)", hs[i].addr, hs[i].error) ||
        gbJoin(&r->skipped, " ", "%s", hs[i].addr)) {
      errCode = ENOMEM;
      goto out;
    }
  }
  if (unreach_new) {
    errCode = EHOSTUNREACH;
    GB_ASPRINTF(errMsg, "the new prio path %s does not answer: %s", prio,
                unreach);
    GB_FREE(r->skipped);
    goto out;
  }
  if (unreach && !force) {
    errCode = EHOSTUNREACH;
    GB_ASPRINTF(errMsg, "hosts of block %s/%s do not answer: %s; use 'force' "
                "to move the prio path without them", volume, block, unreach);
    GB_FREE(r->skipped);
    goto out;
  }

  /* all hosts that take part must know the request */
  if (capsBlk) {
    if (GB_ALLOC(list) < 0 || GB_ALLOC_N(list->hosts, nhs) < 0) {
      errCode = ENOMEM;
      goto out;
    }
    for (i = 0; i < nhs; i++) {
      if (hs[i].reachable &&
          GB_STRDUP(list->hosts[list->nhosts++], hs[i].addr) < 0) {
        errCode = ENOMEM;
        goto out;
      }
    }
    errCode = glusterBlockCheckCapabilities(capsBlk, MODIFY_PRIO_SRV, list,
                                            NULL, errMsg);
    if (errCode) {
      LOG("mgmt", GB_LOG_ERROR, "glusterBlockCheckCapabilities() for block %s "
          "on volume %s failed", block, volume);
      GB_FREE(r->skipped);
      goto out;
    }
  }

  GB_STRCPYSTATIC(mobj.volume, volume);
  GB_STRCPYSTATIC(mobj.block_name, block);
  GB_STRCPYSTATIC(mobj.gbid, info->gbid);
  GB_STRCPYSTATIC(mobj.prio_path, prio);

  gbPrioParallel(hs, nhs, gbPrioRemote, false);

  for (i = 0; i < nhs; i++) {
    if (!hs[i].reachable) {
      continue;
    }
    if (hs[i].obj.exit) {
      if (gbJoin(&r->failed, ", ", "%s: %s", hs[i].addr,
                 hs[i].obj.reply ? hs[i].obj.reply : "failed")) {
        errCode = ENOMEM;
        goto out;
      }
    } else {
      hs[i].applied = true;
      if (gbJoin(&r->success, " ", "%s", hs[i].addr)) {
        errCode = ENOMEM;
        goto out;
      }
    }
  }

  if (r->failed) {
    /* the hosts must agree on the states, put the old prio path back */
    errCode = EIO;
    GB_ASPRINTF(errMsg, "moving the prio path of %s/%s to %s failed", volume,
                block, prio);
    rbobj = mobj;
    GB_STRCPYSTATIC(rbobj.prio_path, info->prio_path);
    for (i = 0; i < nhs; i++) {
      hs[i].obj.obj = &rbobj;
      hs[i].reachable = hs[i].applied;
    }
    gbPrioParallel(hs, nhs, gbPrioRemote, false);
    GB_FREE(r->success);
    for (i = 0; i < nhs; i++) {
      if (!hs[i].applied) {
        continue;
      }
      if (hs[i].obj.exit) {
        LOG("mgmt", GB_LOG_ERROR, "rollback of the prio path of %s/%s on %s "
            "failed: %s", volume, block, hs[i].addr,
            hs[i].obj.reply ? hs[i].obj.reply : "");
        gbJoin(&r->failed, ", ", "%s: rollback failed", hs[i].addr);
      } else {
        gbJoin(&r->rolledback, " ", "%s", hs[i].addr);
      }
    }
    goto out;
  }

  GB_METAUPDATE_OR_GOTO(lock, glfs, block, volume, errCode, *errMsg, out,
                        "PRIOPATH: %s\n", prio);
  blockDecPrioAttr(glfs, volume, info->prio_path);
  blockIncPrioAttr(glfs, volume, (char *)prio);
  errCode = 0;

 out:
  if (hs) {
    for (i = 0; i < nhs; i++) {
      GB_FREE(hs[i].obj.reply);
    }
  }
  GB_FREE(hs);
  blockServerDefFree(list);
  free(unreach);

  return errCode;
}


void
gbPrioResultFree(gbPrioResult *r)
{
  free(r->success);
  free(r->skipped);
  free(r->failed);
  free(r->rolledback);
  memset(r, 0, sizeof(*r));
}


static blockResponse *
block_modify_prio_cli_1_svc_st(blockModifyPrioCli *blk, struct svc_req *rqstp)
{
  blockResponse *reply = NULL;
  struct glfs *glfs;
  struct glfs_fd *lkfd = NULL;
  MetaInfo *info = NULL;
  gbPrioResult r = {0,};
  int errCode = -1;
  char *errMsg = NULL;


  LOG("mgmt", GB_LOG_INFO,
      "modify prio cli request, volume=%s blockname=%s prio=%s force=%d",
      blk->volume, blk->block_name, blk->prio_path, blk->force);

  if (GB_ALLOC(reply) < 0) {
    return NULL;
  }
  if (GB_ALLOC(info) < 0) {
    errCode = ENOMEM;
    goto initfail;
  }

  errCode = 0;
  glfs = glusterBlockVolumeInit(blk->volume, &errCode, &errMsg);
  if (!glfs) {
    LOG("mgmt", GB_LOG_ERROR, "glusterBlockVolumeInit(%s) for block %s failed",
        blk->volume, blk->block_name);
    goto initfail;
  }

  lkfd = glusterBlockCreateMetaLockFile(glfs, blk->volume, &errCode, &errMsg);
  if (!lkfd) {
    LOG("mgmt", GB_LOG_ERROR, "%s %s for block %s",
        FAILED_CREATING_META, blk->volume, blk->block_name);
    goto initfail;
  }

  GB_METALOCK_OR_GOTO(lkfd, blk->volume, errCode, errMsg, initfail);
  LOG("cmdlog", GB_LOG_INFO, "%s", blk->cmd);

  if (glfs_access(glfs, blk->block_name, F_OK)) {
    errCode = errno;
    GB_ASPRINTF(&errMsg, errCode == ENOENT ? "block %s/%s doesn't exist" :
                "block %s/%s is not accessible", blk->volume, blk->block_name);
    goto out;
  }

  if (blockGetMetaInfo(glfs, blk->block_name, info, &errCode)) {
    GB_ASPRINTF(&errMsg, "Not able to get metadata information for %s/%s[%s]",
                blk->volume, blk->block_name, strerror(errCode));
    goto out;
  }

  {
    char st[32];

    if (blockHostInMaintenance(glfs, blk->prio_path, st, sizeof(st))) {
      errCode = EBUSY;
      GB_ASPRINTF(&errMsg, "host %s is in maintenance (%s)", blk->prio_path,
                  st);
      goto out;
    }
  }

  errCode = gbPrioMove(glfs, blk->volume, blk->block_name, info,
                       blk->prio_path, blk->force, NULL, 0, blk, &r, &errMsg);

 out:
  GB_METAUNLOCK(lkfd, blk->volume, errCode, errMsg);

 initfail:
  LOG("mgmt", ((!!errCode) ? GB_LOG_ERROR : GB_LOG_INFO),
      "modify prio cli return %s, volume=%s blockname=%s",
      errCode ? "failure" : "success", blk->volume, blk->block_name);

  if (lkfd && glfs_close(lkfd) != 0) {
    LOG("mgmt", GB_LOG_ERROR, "glfs_close(%s): on volume %s failed[%s]",
        GB_TXLOCKFILE, blk->volume, strerror(errno));
  }

  if (errCode < 0) {
    errCode = GB_DEFAULT_ERRCODE;
  }
  if (!r.previous) {
    r.previous = (char *)"";
    r.prio = blk->prio_path;
  }
  gbPrioFormat(blk, errCode, errMsg, &r, reply);
  LOG("cmdlog", ((!!errCode) ? GB_LOG_ERROR : GB_LOG_INFO), "%s",
      reply->out ? reply->out : "*Nil*");

  gbPrioResultFree(&r);
  GB_FREE(errMsg);
  /* r.previous pointed into info */
  blockFreeMetaInfo(info);

  return reply;
}


bool_t
block_modify_prio_cli_1_svc(blockModifyPrioCli *blk, blockResponse *reply,
                            struct svc_req *rqstp)
{
  int ret;

  GB_RPC_CALL(modify_prio_cli, blk, reply, rqstp, ret);
  return ret;
}


/* ------------------------------------------------------------------------
 * 'gluster-block rebalance <volume> [dry-run]'
 *
 * Spread the prio paths of the volume over the hosts that answer: first
 * move them off hosts that do not answer, then from the host with the most
 * prio paths to the one of the same block with the fewest while that
 * lowers the difference. Hosts that do not answer are left out (they get
 * the new states with a reload once back).
 * ------------------------------------------------------------------------ */





void
gbRebalanceFree(gbRebalance *rb)
{
  size_t i;

  for (i = 0; i < rb->nblocks; i++) {
    blockFreeMetaInfo(rb->blocks[i].info);
    free(rb->blocks[i].msg);
  }
  GB_FREE(rb->blocks);
  GB_FREE(rb->reach);
  GB_FREE(rb->load);
  free(rb->skipped);
}


int
gbReachIndex(gbRebalance *rb, const char *addr)
{
  size_t i;

  for (i = 0; i < rb->nreach; i++) {
    if (!strcmp(rb->reach[i].addr, addr)) {
      return (int)i;
    }
  }
  return -1;
}


void *
gbReachProbe(void *data)
{
  gbReach *r = data;

  r->reachable = blockHostReachable(r->addr, r->error, sizeof(r->error));
  return NULL;
}


static int
gbRbBlockCmp(const void *a, const void *b)
{
  return strcmp(((const gbRbBlock *)a)->name, ((const gbRbBlock *)b)->name);
}


int
gbRebalanceRead(struct glfs *glfs, char *volume, gbRebalance *rb,
                bool withNoPrio, int *errCode, char **errMsg)
{
  struct glfs_fd *dfd;
  struct dirent *entry;
  MetaInfo *info = NULL;
  size_t i;


  dfd = glfs_opendir(glfs, GB_METADIR);
  if (!dfd) {
    *errCode = errno;
    GB_ASPRINTF(errMsg, "Not able to open metadata directory for volume "
                "%s[%s]", volume, strerror(*errCode));
    return -1;
  }

  while ((entry = glfs_readdir(dfd))) {
    gbRbBlock *b;

    if (strchr(entry->d_name, '.')) {
      continue;
    }
    if (GB_ALLOC(info) < 0) {
      goto nomem;
    }
    if (blockGetMetaInfo(glfs, entry->d_name, info, errCode)) {
      LOG("mgmt", GB_LOG_WARNING, "rebalance: skip block %s/%s, no "
          "metadata[%s]", volume, entry->d_name, strerror(*errCode));
      blockFreeMetaInfo(info);
      info = NULL;
      continue;
    }
    if (!info->prio_path[0] && !withNoPrio) {
      gbJoin(&rb->skipped, "", "%s: no prio path, created without load "
             "balancing\n", entry->d_name);
      blockFreeMetaInfo(info);
      info = NULL;
      continue;
    }

    if (GB_REALLOC_N(rb->blocks, rb->nblocks + 1) < 0) {
      goto nomem;
    }
    b = &rb->blocks[rb->nblocks++];
    memset(b, 0, sizeof(*b));
    GB_STRCPYSTATIC(b->name, entry->d_name);
    b->info = info;
    info = NULL;

    for (i = 0; i < b->info->nhosts; i++) {
      if (!blockhostIsValid(b->info->list[i]->status) ||
          gbReachIndex(rb, b->info->list[i]->addr) >= 0) {
        continue;
      }
      if (GB_REALLOC_N(rb->reach, rb->nreach + 1) < 0) {
        goto nomem;
      }
      memset(&rb->reach[rb->nreach], 0, sizeof(gbReach));
      GB_STRCPYSTATIC(rb->reach[rb->nreach].addr, b->info->list[i]->addr);
      rb->nreach++;
    }
  }

  glfs_closedir(dfd);
  if (rb->nblocks > 1) {
    qsort(rb->blocks, rb->nblocks, sizeof(*rb->blocks), gbRbBlockCmp);
  }
  for (i = 0; i < rb->nreach; i++) {
    blockHostStateGet(glfs, rb->reach[i].addr, rb->reach[i].state,
                      sizeof(rb->reach[i].state));
  }
  *errCode = 0;
  return 0;

 nomem:
  if (info) {
    blockFreeMetaInfo(info);
  }
  glfs_closedir(dfd);
  *errCode = ENOMEM;
  return -1;
}


bool
gbRbHasHost(gbRbBlock *b, const char *addr)
{
  size_t i;

  for (i = 0; i < b->info->nhosts; i++) {
    if (blockhostIsValid(b->info->list[i]->status) &&
        !strcmp(b->info->list[i]->addr, addr)) {
      return true;
    }
  }
  return false;
}


/* hosts that export the block by its metadata */
size_t
gbRbValidHosts(gbRbBlock *b)
{
  size_t i, n = 0;

  for (i = 0; i < b->info->nhosts; i++) {
    if (blockhostIsValid(b->info->list[i]->status)) {
      n++;
    }
  }
  return n;
}


void
gbRebalanceProbe(gbRebalance *rb)
{
  pthread_t *tid = NULL;
  size_t i;

  if (GB_ALLOC_N(tid, rb->nreach) < 0) {
    for (i = 0; i < rb->nreach; i++) {
      gbReachProbe(&rb->reach[i]);
    }
    return;
  }
  for (i = 0; i < rb->nreach; i++) {
    pthread_create(&tid[i], NULL, gbReachProbe, &rb->reach[i]);
  }
  for (i = 0; i < rb->nreach; i++) {
    pthread_join(tid[i], NULL);
  }
  GB_FREE(tid);
}


/* a host that answers and is not in maintenance may take the prio path */
bool
gbRbCandidate(gbRebalance *rb, gbRbBlock *b, int idx)
{
  size_t i;

  if (idx < 0 || !rb->reach[idx].reachable || rb->reach[idx].state[0] ||
      !b->info->prio_path[0]) {
    return false;
  }
  for (i = 0; i < b->info->nhosts; i++) {
    if (blockhostIsValid(b->info->list[i]->status) &&
        !strcmp(b->info->list[i]->addr, rb->reach[idx].addr)) {
      return true;
    }
  }
  return false;
}


void
gbRebalancePlan(gbRebalance *rb)
{
  size_t i, iter;
  int j;


  for (i = 0; i < rb->nblocks; i++) {
    gbRbBlock *b = &rb->blocks[i];

    b->from = gbReachIndex(rb, b->info->prio_path);
    b->cur = gbRbCandidate(rb, b, b->from) ? b->from : -1;
    if (b->cur >= 0) {
      rb->load[b->cur]++;
    }
  }

  /* off the hosts that do not answer */
  for (i = 0; i < rb->nblocks; i++) {
    gbRbBlock *b = &rb->blocks[i];
    int best = -1;

    if (b->cur >= 0 || !b->info->prio_path[0] || !gbRbValidHosts(b)) {
      continue;
    }
    for (j = 0; j < (int)rb->nreach; j++) {
      if (gbRbCandidate(rb, b, j) && (best < 0 || rb->load[j] < rb->load[best])) {
        best = j;
      }
    }
    if (best >= 0) {
      b->cur = best;
      rb->load[best]++;
    }
  }

  /* level the load, every move lowers the difference by at least 2 */
  for (iter = 0; iter < rb->nblocks * (rb->nreach + 1); iter++) {
    gbRbBlock *bb = NULL;
    int bto = -1;
    size_t gain = 0;

    for (i = 0; i < rb->nblocks; i++) {
      gbRbBlock *b = &rb->blocks[i];

      if (b->cur < 0) {
        continue;
      }
      for (j = 0; j < (int)rb->nreach; j++) {
        if (j == b->cur || !gbRbCandidate(rb, b, j) ||
            rb->load[j] + 1 >= rb->load[b->cur]) {
          continue;
        }
        if (rb->load[b->cur] - rb->load[j] > gain ||
            /* same gain: prefer the block that has to move anyway */
            (rb->load[b->cur] - rb->load[j] == gain && bb &&
             bb->cur == bb->from && b->cur != b->from)) {
          gain = rb->load[b->cur] - rb->load[j];
          bb = b;
          bto = j;
        }
      }
    }
    if (!bb) {
      break;
    }
    rb->load[bb->cur]--;
    rb->load[bto]++;
    bb->cur = bto;
  }
}


bool
gbRbMoves(gbRbBlock *b)
{
  return b->cur >= 0 && b->cur != b->from;
}


static void
gbRebalanceFormat(blockRebalanceCli *blk, gbRebalance *rb, int errCode,
                  char *errMsg, blockResponse *reply)
{
  json_object *jo, *ja, *jp;
  char *out = NULL;
  size_t i, nmoves = 0, nfailed = 0;


  if (errCode && !rb->nblocks) {
    blockFormatErrorResponse(REBALANCE_SRV, blk->json_resp, errCode,
                             errMsg ? errMsg : GB_DEFAULT_ERRMSG, reply);
    return;
  }
  for (i = 0; i < rb->nblocks; i++) {
    if (gbRbMoves(&rb->blocks[i])) {
      nmoves++;
    }
    if (rb->blocks[i].done < 0) {
      nfailed++;
    }
  }
  reply->exit = errCode;

  if (blk->json_resp) {
    jo = json_object_new_object();
    ja = json_object_new_array();
    jp = json_object_new_object();
    json_object_object_add(jo, "VOLUME", GB_JSON_OBJ_TO_STR(blk->volume));
    json_object_object_add(jo, "DRY-RUN", json_object_new_boolean(blk->dry_run));
    for (i = 0; i < rb->nblocks; i++) {
      gbRbBlock *b = &rb->blocks[i];
      json_object *o;

      if (!gbRbMoves(b) && b->cur >= 0) {
        continue;
      }
      o = json_object_new_object();
      json_object_object_add(o, "NAME", GB_JSON_OBJ_TO_STR(b->name));
      json_object_object_add(o, "FROM", GB_JSON_OBJ_TO_STR(b->info->prio_path));
      json_object_object_add(o, "TO",
          GB_JSON_OBJ_TO_STR(b->cur >= 0 ? rb->reach[b->cur].addr : ""));
      json_object_object_add(o, "RESULT", GB_JSON_OBJ_TO_STR(
          b->cur < 0 ? "NO-HOST" : blk->dry_run ? "PLANNED" :
          b->done > 0 ? "SUCCESS" : b->done < 0 ? "FAIL" : "NOT-RUN"));
      if (b->msg) {
        json_object_object_add(o, "errMsg", GB_JSON_OBJ_TO_STR(b->msg));
      }
      json_object_array_add(ja, o);
    }
    json_object_object_add(jo, "MOVES", ja);
    if (rb->skipped) {
      blockStr2arrayAddToJsonObj(jo, rb->skipped, "SKIPPED");
    }
    for (i = 0; i < rb->nreach; i++) {
      json_object_object_add(jp, rb->reach[i].addr,
                             rb->reach[i].state[0] ?
                             GB_JSON_OBJ_TO_STR("maintenance") :
                             rb->reach[i].reachable ?
                             json_object_new_int64(rb->load[i]) :
                             GB_JSON_OBJ_TO_STR("down"));
    }
    json_object_object_add(jo, "PRIO-PATHS", jp);
    if (errMsg) {
      json_object_object_add(jo, "errMsg", GB_JSON_OBJ_TO_STR(errMsg));
    }
    json_object_object_add(jo, "RESULT",
                           GB_JSON_OBJ_TO_STR(errCode ? "FAIL" : "SUCCESS"));
    GB_ASPRINTF(&reply->out, "%s\n", json_object_to_json_string_ext(jo,
                                    mapJsonFlagToJsonCstring(blk->json_resp)));
    json_object_put(jo);
    return;
  }

  gbJoin(&out, "", "VOLUME: %s\nMOVES: %zu%s\n", blk->volume, nmoves,
         blk->dry_run ? " (dry run)" : nmoves ? "" : " (balanced)");
  for (i = 0; i < rb->nblocks; i++) {
    gbRbBlock *b = &rb->blocks[i];

    if (b->cur < 0 && b->info->prio_path[0] && gbRbValidHosts(b)) {
      gbJoin(&out, "", "  %s: %s -> none, no other host of the block "
             "answers\n", b->name, b->info->prio_path);
    } else if (gbRbMoves(b)) {
      gbJoin(&out, "", "  %s: %s -> %s%s%s%s\n", b->name, b->info->prio_path,
             rb->reach[b->cur].addr,
             blk->dry_run ? "" : b->done > 0 ? ": done" :
             b->done < 0 ? ": FAILED" : ": not run",
             b->msg ? " " : "", b->msg ? b->msg : "");
    }
  }
  if (rb->skipped) {
    char *line, *save = NULL, *copy = strdup(rb->skipped);

    gbJoin(&out, "", "SKIPPED:\n");
    for (line = copy ? strtok_r(copy, "\n", &save) : NULL; line;
         line = strtok_r(NULL, "\n", &save)) {
      gbJoin(&out, "", "  %s\n", line);
    }
    free(copy);
  }
  for (i = 0; i < rb->nreach; i++) {
    if (!rb->reach[i].reachable) {
      gbJoin(&out, "", "HOST DOWN: %s (%s), reload its blocks once it is back\n",
             rb->reach[i].addr, rb->reach[i].error);
    }
  }
  gbJoin(&out, "", "PRIO PATHS:");
  for (i = 0; i < rb->nreach; i++) {
    if (rb->reach[i].state[0]) {
      gbJoin(&out, "", " %s=maintenance", rb->reach[i].addr);
    } else if (rb->reach[i].reachable) {
      gbJoin(&out, "", " %s=%zu", rb->reach[i].addr, rb->load[i]);
    } else {
      gbJoin(&out, "", " %s=down", rb->reach[i].addr);
    }
  }
  if (errMsg) {
    gbJoin(&out, "", "\n%s", errMsg);
  }
  gbJoin(&out, "", "\nRESULT: %s\n", errCode ? "FAIL" : "SUCCESS");
  reply->out = out;
}


static blockResponse *
blockRebalanceRun(blockRebalanceCli *blk)
{
  blockResponse *reply = NULL;
  struct glfs *glfs = NULL;
  struct glfs_fd *lkfd = NULL;
  blockServerDefPtr list = NULL;
  blockModifyPrioCli capsReq = {{0},};
  gbRebalance rb = {0,};
  bool privGlfs = false;
  size_t i, nmoves = 0, k = 0;
  int errCode = -1;
  char *errMsg = NULL;


  LOG("mgmt", GB_LOG_INFO, "rebalance cli request, volume=%s dry-run=%d",
      blk->volume, blk->dry_run);

  if (GB_ALLOC(reply) < 0) {
    return NULL;
  }

  errCode = 0;
  glfs = blockVolumeGet(blk->volume, &errCode, &errMsg, &privGlfs);
  if (!glfs) {
    goto initfail;
  }

  lkfd = glusterBlockCreateMetaLockFile(glfs, blk->volume, &errCode, &errMsg);
  if (!lkfd) {
    goto initfail;
  }

  GB_METALOCK_OR_GOTO(lkfd, blk->volume, errCode, errMsg, initfail);
  LOG("cmdlog", GB_LOG_INFO, "%s", blk->cmd);
  if (!blk->dry_run) {
    blockOpBegin(lkfd, "rebalance", "*", NULL, 0);
  }

  if (gbRebalanceRead(glfs, blk->volume, &rb, false, &errCode, &errMsg)) {
    goto out;
  }
  if (!rb.nblocks) {
    goto out;
  }

  if (GB_ALLOC_N(rb.load, rb.nreach) < 0) {
    errCode = ENOMEM;
    goto out;
  }
  blockOpPhase("probing hosts");
  gbRebalanceProbe(&rb);

  gbRebalancePlan(&rb);
  for (i = 0; i < rb.nblocks; i++) {
    if (gbRbMoves(&rb.blocks[i])) {
      nmoves++;
    }
  }
  if (blk->dry_run || !nmoves) {
    goto out;
  }

  /* the hosts that answer must know the request */
  if (GB_ALLOC(list) < 0 || GB_ALLOC_N(list->hosts, rb.nreach) < 0) {
    errCode = ENOMEM;
    goto out;
  }
  for (i = 0; i < rb.nreach; i++) {
    if (rb.reach[i].reachable &&
        GB_STRDUP(list->hosts[list->nhosts++], rb.reach[i].addr) < 0) {
      errCode = ENOMEM;
      goto out;
    }
  }
  capsReq.json_resp = blk->json_resp;
  errCode = glusterBlockCheckCapabilities(&capsReq, MODIFY_PRIO_SRV, list,
                                          NULL, &errMsg);
  if (errCode) {
    goto out;
  }

  blockOpPhase("moving prio paths");
  blockOpStepCount("moving prio paths", nmoves);
  gbAsyncDetach();

  for (i = 0; i < rb.nblocks; i++) {
    gbRbBlock *b = &rb.blocks[i];
    gbPrioResult r = {0,};
    char *msg = NULL;
    int ret;

    if (!gbRbMoves(b)) {
      continue;
    }
    ret = gbPrioMove(glfs, blk->volume, b->name, b->info,
                     rb.reach[b->cur].addr, true, rb.reach, rb.nreach, NULL,
                     &r, &msg);
    if (ret) {
      b->done = -1;
      if (r.failed) {
        GB_ASPRINTF(&b->msg, "(%s%s%s)", r.failed,
                    r.rolledback ? ", rolled back on " : "",
                    r.rolledback ? r.rolledback : "");
      } else {
        GB_ASPRINTF(&b->msg, "(%s)", msg ? msg : "failed");
      }
      errCode = EIO;
    } else {
      b->done = 1;
    }
    gbPrioResultFree(&r);
    GB_FREE(msg);
    blockOpStepProgress(++k);
  }

  if (errCode) {
    GB_ASPRINTF(&errMsg, "prio paths of some blocks could not be moved");
  }

 out:
  GB_METAUNLOCK(lkfd, blk->volume, errCode, errMsg);

 initfail:
  LOG("mgmt", ((!!errCode) ? GB_LOG_ERROR : GB_LOG_INFO),
      "rebalance cli return %s, volume=%s", errCode ? "failure" : "success",
      blk->volume);
  if (lkfd && glfs_close(lkfd) != 0) {
    LOG("mgmt", GB_LOG_ERROR, "glfs_close(%s): on volume %s failed[%s]",
        GB_TXLOCKFILE, blk->volume, strerror(errno));
  }
  if (errCode < 0) {
    errCode = GB_DEFAULT_ERRCODE;
  }

  gbRebalanceFormat(blk, &rb, errCode, errMsg, reply);
  LOG("cmdlog", ((!!errCode) ? GB_LOG_ERROR : GB_LOG_INFO), "%s",
      reply->out ? reply->out : "*Nil*");

  if (glfs) {
    blockVolumePut(glfs, privGlfs);
  }
  blockServerDefFree(list);
  gbRebalanceFree(&rb);
  GB_FREE(errMsg);

  return reply;
}


static void
blockRebalanceCliFree(void *data)
{
  blockRebalanceCli *blk = data;

  if (!blk) {
    return;
  }
  GB_FREE(blk->cmd);
  GB_FREE(blk);
}


static blockResponse *
blockRebalanceRunAsync(void *data)
{
  return blockRebalanceRun(data);
}


static blockResponse *
block_rebalance_cli_1_svc_st(blockRebalanceCli *blk, struct svc_req *rqstp)
{
  blockRebalanceCli *copy = NULL;

  if (blk->dry_run || !blk->async) {
    return blockRebalanceRun(blk);
  }

  /* the RPC arguments are freed once we return, the worker needs a copy */
  if (GB_ALLOC(copy) < 0) {
    return blockRebalanceRun(blk);
  }
  memcpy(copy, blk, sizeof(*copy));
  copy->cmd = NULL;
  if (GB_STRDUP(copy->cmd, blk->cmd ? blk->cmd : "") < 0) {
    blockRebalanceCliFree(copy);
    return blockRebalanceRun(blk);
  }

  return gbAsyncRun("rebalance", blk->volume, "*", blk->json_resp,
                    blockRebalanceRunAsync, copy, blockRebalanceCliFree);
}


bool_t
block_rebalance_cli_1_svc(blockRebalanceCli *blk, blockResponse *reply,
                          struct svc_req *rqstp)
{
  int ret;

  GB_RPC_CALL(rebalance_cli, blk, reply, rqstp, ret);
  return ret;
}
