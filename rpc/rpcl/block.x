/* rpcgen generates code that results in unused-variable warnings */
#ifdef RPC_XDR
%#include "rpc-pragmas.h"
#endif

/*
 * Revision of the GLUSTER_BLOCK_CLI program: the highest procedure number.
 * 'version' reports it, gluster-block-apid refuses a daemon that is older
 * than what it was built for.
 */
const GB_CLI_RPC_REV = 17;

enum JsonResponseFormat {
  GB_JSON_NONE           = 0,

  GB_JSON_PLAIN          = 1,
  GB_JSON_SPACED         = 2,
  GB_JSON_PRETTY         = 3,
  GB_JSON_DEFAULT        = 4,

  GB_JSON_MAX
};

struct blockCreate {
  char      ipaddr[255];
  char      volume[255];
  char      gbid[127];                   /* uuid */
  char      passwd[127];                 /* uuid */
  u_quad_t  size;
  char      block_name[255];
  string    block_hosts<>;               /* for multiple tpg's creation */
  bool      auth_mode;
};

struct blockCreate2 {
  char      ipaddr[255];
  char      volume[255];
  char      gbid[127];                   /* uuid */
  char      passwd[127];                 /* uuid */
  u_quad_t  size;
  char      block_name[255];
  char      prio_path[255];
  string    block_hosts<>;               /* for multiple tpg's creation */
  bool      auth_mode;
  u_int     rb_size;                     /* TCMU Ring Buffer size in kernel */
  opaque    xdata<>;                     /* future reserve */
};

struct blockModify {
  char      volume[255];
  char      block_name[255];
  char      gbid[127];
  char      passwd[127];
  bool      auth_mode;
};

struct blockModifySize {
  char      volume[255];
  char      block_name[255];
  char      gbid[127];
  u_quad_t  size;
};

struct blockReplace {
  char      volume[255];
  char      block_name[255];
  char      gbid[127];
  char      ipaddr[255];
  char      ripaddr[255];
};

struct blockCreateCli {
  char      volume[255];
  u_quad_t  size;
  u_int     rb_size;              /* TCMU Ring Buffer size in kernel */
  u_int     blk_size;             /* TCMU hw block size in kernel */
  u_int     io_timeout;           /* Cmds timeout in tcmu-runner */
  u_int     mpath;                /* HA request count */
  bool      auth_mode;
  bool      prealloc;
  bool      zerofill;
  bool      async;                /* return IN-PROGRESS, finish in background */
  bool      thin_confirmed;       /* user accepted thin file on dispersed vol */
  char      storage[255];
  char      block_name[255];
  string    block_hosts<>;
  string    cmd<>;
  enum JsonResponseFormat     json_resp;
};

struct blockDeleteCli {
  char      block_name[255];
  char      volume[255];
  bool      unlink;
  bool      force;
  string    cmd<>;
  enum JsonResponseFormat     json_resp;
};

struct blockDelete {
  char      block_name[255];
  char      gbid[127];
};

struct blockReloadCli {
  char      block_name[255];
  char      volume[255];
  bool      force;
  string    cmd<>;
  enum JsonResponseFormat     json_resp;
};

struct blockReload {
  char      block_name[255];
  char      gbid[127];
};

struct blockInfoCli {
  char      block_name[255];
  char      volume[255];
  enum JsonResponseFormat     json_resp;
};

struct blockListCli {
  char      volume[255];
  u_quad_t  offset;      /* dentry d_name offset */
  enum JsonResponseFormat     json_resp;
};

struct blockModifyCli {
  char      block_name[255];
  char      volume[255];
  bool      auth_mode;
  string     cmd<>;
  enum JsonResponseFormat     json_resp;
};

struct blockModifySizeCli {
  char      block_name[255];
  char      volume[255];
  u_quad_t  size;
  bool      force;
  bool      async;                /* return IN-PROGRESS, finish in background */
  string    cmd<>;
  enum JsonResponseFormat     json_resp;
};

struct blockStatusCli {
  char      volume[255];
  char      op_id[38];            /* empty: whole volume, else one operation */
  enum JsonResponseFormat     json_resp;
};

struct blockReplaceCli {
  char      volume[255];
  char      block_name[255];
  char      old_node[255];
  char      new_node[255];
  bool      force;
  string    cmd<>;
  enum JsonResponseFormat     json_resp;
};

struct blockModifyPrio {
  char      volume[255];
  char      block_name[255];
  char      gbid[127];
  char      prio_path[255];       /* host whose TPG gets the AO ALUA group */
};

struct blockModifyPrioCli {
  char      volume[255];
  char      block_name[255];
  char      prio_path[255];
  bool      force;                /* skip hosts that do not answer */
  string    cmd<>;
  enum JsonResponseFormat     json_resp;
};

struct blockRebalanceCli {
  char      volume[255];
  bool      dry_run;              /* show the moves, change nothing */
  bool      async;                /* return IN-PROGRESS, finish in background */
  string    cmd<>;
  enum JsonResponseFormat     json_resp;
};

struct blockHostTpg {
  char      volume[255];
  char      host[255];            /* the host as written in the metadata */
  string    blocks<>;             /* "<block_name> <gbid>\n" per block */
  bool      enable;               /* enable or disable the TPG of the host */
};

struct blockHostMaintCli {
  char      volume[255];
  char      host[255];
  bool      start;                /* start or stop the maintenance */
  bool      force;                /* start: skip the checks and the I/O wait */
  bool      confirmed;            /* the user accepted what force skips */
  bool      rebalance;            /* stop: rebalance the prio paths after */
  bool      async;                /* return IN-PROGRESS, finish in background */
  string    cmd<>;
  enum JsonResponseFormat     json_resp;
};

struct blockHostStatus {
  char      volume[255];
  char      host[255];            /* the host as written in the metadata */
  string    blocks<>;             /* "<block_name> <gbid>\n" per block */
};

struct blockHostsCli {
  char      volume[255];
  char      host[255];            /* empty: all hosts of the volume */
  enum JsonResponseFormat     json_resp;
};

struct blockVersionCli {
  char      volume[255];          /* empty: this daemon only */
  enum JsonResponseFormat     json_resp;
};

struct blockVolumesCli {
  enum JsonResponseFormat     json_resp;
};

struct blockGenConfigCli {
  string    volume<>;
  char      addr[255];
  enum JsonResponseFormat     json_resp;
};

struct blockResponse {
  int       exit;       /* exit code of the command */
  string    out<>;      /* output; TODO: return respective objects */
  u_quad_t  offset;     /* dentry d_name offset */
  opaque    xdata<>;    /* future reserve */
};

program GLUSTER_BLOCK {
  version GLUSTER_BLOCK_VERS {
    blockResponse BLOCK_CREATE(blockCreate) = 1;
    blockResponse BLOCK_DELETE(blockDelete) = 2;
    blockResponse BLOCK_MODIFY(blockModify) = 3;
    blockResponse BLOCK_VERSION() = 4;
    blockResponse BLOCK_REPLACE(blockReplace) = 5;
    blockResponse BLOCK_MODIFY_SIZE(blockModifySize) = 6;

    blockResponse BLOCK_CREATE_V2(blockCreate2) = 7;
    blockResponse BLOCK_RELOAD(blockReload) = 8;
    blockResponse BLOCK_HOST_STATUS(blockHostStatus) = 9;
    blockResponse BLOCK_MODIFY_PRIO(blockModifyPrio) = 10;
    blockResponse BLOCK_HOST_TPG(blockHostTpg) = 11;
  } = 1;
} = 21215311; /* B2 L12 O15 C3 K11 */

program GLUSTER_BLOCK_CLI {
  version GLUSTER_BLOCK_CLI_VERS {
    blockResponse BLOCK_CREATE_CLI(blockCreateCli) = 1;
    blockResponse BLOCK_LIST_CLI(blockListCli) = 2;
    blockResponse BLOCK_INFO_CLI(blockInfoCli) = 3;
    blockResponse BLOCK_DELETE_CLI(blockDeleteCli) = 4;
    blockResponse BLOCK_MODIFY_CLI(blockModifyCli) = 5;
    blockResponse BLOCK_REPLACE_CLI(blockReplaceCli) = 6;
    blockResponse BLOCK_MODIFY_SIZE_CLI(blockModifySizeCli) = 7;
    blockResponse BLOCK_GEN_CONFIG_CLI(blockGenConfigCli) = 8;
    blockResponse BLOCK_RELOAD_CLI(blockReloadCli) = 9;
    blockResponse BLOCK_STATUS_CLI(blockStatusCli) = 10;
    blockResponse BLOCK_HOSTS_CLI(blockHostsCli) = 11;
    blockResponse BLOCK_MODIFY_PRIO_CLI(blockModifyPrioCli) = 12;
    blockResponse BLOCK_REBALANCE_CLI(blockRebalanceCli) = 13;
    blockResponse BLOCK_HOST_MAINT_CLI(blockHostMaintCli) = 14;
    blockResponse BLOCK_VERSION_CLI(blockVersionCli) = 15;
    blockResponse BLOCK_VOLUMES_CLI(blockVolumesCli) = 16;
    /* delete in background, like create: IN-PROGRESS and an operation id */
    blockResponse BLOCK_DELETE_ASYNC_CLI(blockDeleteCli) = 17;
  } = 1;
} = 212153113; /* B2 L12 O15 C3 K11 C3 */
