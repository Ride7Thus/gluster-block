/*
  Copyright (c) 2026 gluster-block contributors
  This file is part of gluster-block.

  This file is licensed to you under your choice of the GNU Lesser
  General Public License, version 3 or any later version (LGPLv3 or
  later), or the GNU General Public License, version 2 (GPLv2), in all
  cases as published by the Free Software Foundation.
*/


# include  "config.h"
# include  "gb-client.h"
# include  "block.h"

# include  <errno.h>
# include  <stdio.h>
# include  <string.h>
# include  <unistd.h>
# include  <sys/socket.h>
# include  <sys/un.h>


int
gbClientConnect(gbClient *c, const char *path, unsigned int timeout_sec,
                char *err, size_t len, int *daemonDown)
{
  struct sockaddr_un saun;
  struct timeval tv;
  int sockfd = RPC_ANYSOCK;


  c->clnt = NULL;
  c->fd = RPC_ANYSOCK;
  if (daemonDown) {
    *daemonDown = 0;
  }

  memset(&saun, 0, sizeof(saun));
  if (strlen(path) >= sizeof(saun.sun_path)) {
    snprintf(err, len, "%s: path length is more than %zu chars", path,
             sizeof(saun.sun_path) - 1);
    return -1;
  }
  saun.sun_family = AF_UNIX;
  memcpy(saun.sun_path, path, strlen(path) + 1);

#ifndef HAVE_LIBTIRPC
  if ((sockfd = socket(AF_UNIX, SOCK_STREAM, 0)) < 0) {
    snprintf(err, len, "%s: socket creation failed (%s)", path,
             strerror(errno));
    return -1;
  }
  if (connect(sockfd, (struct sockaddr *) &saun, sizeof(saun)) < 0) {
    if (errno == ENOENT || errno == ECONNREFUSED) {
      snprintf(err, len, "Connection failed. Please check if "
               "gluster-block daemon is operational.");
      if (daemonDown) {
        *daemonDown = 1;
      }
    } else {
      snprintf(err, len, "%s: connect failed (%s)", path, strerror(errno));
    }
    close(sockfd);
    return -1;
  }
#else
  /* clntunix_create() connects itself, check first to tell why it fails */
  if (access(path, F_OK)) {
    snprintf(err, len, "Connection failed. Please check if "
             "gluster-block daemon is operational.");
    if (daemonDown) {
      *daemonDown = 1;
    }
    return -1;
  }
#endif  /* HAVE_LIBTIRPC */

  c->clnt = clntunix_create(&saun, GLUSTER_BLOCK_CLI, GLUSTER_BLOCK_CLI_VERS,
                            &sockfd, 0, 0);
  if (!c->clnt) {
    if (rpc_createerr.cf_error.re_errno == ECONNREFUSED && daemonDown) {
      *daemonDown = 1;
    }
    snprintf(err, len, "%s, unix addr %s",
             clnt_spcreateerror("client create failed"), path);
    if (sockfd != RPC_ANYSOCK) {
      close(sockfd);
    }
    return -1;
  }
  c->fd = sockfd;

  if (timeout_sec) {
    tv.tv_sec = timeout_sec;
    tv.tv_usec = 0;
    clnt_control(c->clnt, CLSET_TIMEOUT, (char *)&tv);
  }

  return 0;
}


void
gbClientClose(gbClient *c)
{
  if (c->clnt) {
    clnt_destroy(c->clnt);
    c->clnt = NULL;
  }
  /* a socket we created is not closed by clnt_destroy() */
  if (c->fd != RPC_ANYSOCK) {
    close(c->fd);
    c->fd = RPC_ANYSOCK;
  }
}
