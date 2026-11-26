/*
  Copyright (c) 2026 gluster-block contributors
  This file is part of gluster-block.

  This file is licensed to you under your choice of the GNU Lesser
  General Public License, version 3 or any later version (LGPLv3 or
  later), or the GNU General Public License, version 2 (GPLv2), in all
  cases as published by the Free Software Foundation.
*/

/*
 * Client of the local gluster-blockd (RPC program GLUSTER_BLOCK_CLI on its
 * unix socket), shared by the gluster-block cli and gluster-block-apid.
 * Link with rpc/rpcl/libgbrpcxdr.la for the rpcgen client stubs.
 */

# ifndef _GB_CLIENT_H
# define _GB_CLIENT_H 1

# include  <stddef.h>
# include  <rpc/rpc.h>


typedef struct gbClient {
  CLIENT *clnt;
  int     fd;
} gbClient;


/*
 * Connect to gluster-blockd on the unix socket @path; @timeout_sec bounds
 * every call on the connection (0: the rpcgen stubs' TIMEOUT). Returns 0, or
 * -1 with a message in @err; @daemonDown tells that nothing listens.
 */
int gbClientConnect(gbClient *c, const char *path, unsigned int timeout_sec,
                    char *err, size_t len, int *daemonDown);

void gbClientClose(gbClient *c);

# endif /* _GB_CLIENT_H */
