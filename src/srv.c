/* Copyright (C) 2026 John Törnblom

This program is free software; you can redistribute it and/or modify it
under the terms of the GNU General Public License as published by the
Free Software Foundation; either version 3, or (at your option) any
later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; see the file COPYING. If not, see
<http://www.gnu.org/licenses/>.  */

#include <stdio.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include <microhttpd.h>

#include "asset.h"
#include "fs.h"
#include "mdns.h"
#include "smb.h"
#include "ssdp.h"
#include "viz.h"


/**
 *
 **/
static enum MHD_Result
srv_on_request(void *cls, struct MHD_Connection *conn,
	       const char *url, const char *method,
	       const char *version, const char *upload_data,
	       size_t *upload_data_size, void **con_cls) {
  if(strcmp(method, MHD_HTTP_METHOD_GET)) {
    return MHD_NO;
  }

  if(!strcmp("/mdns", url)) {
    return mdns_request(conn, url);
  }
  if(!strcmp("/ssdp", url)) {
    return ssdp_request(conn, url);
  }
  if(!strncmp("/smb", url, 4)) {
    return smb_request(conn, url);
  }
  if(!strncmp("/fs/", url, 4)) {
    return fs_request(conn, url);
  }
  if(!strncmp("/viz", url, 4)) {
    return viz_request(conn, url);
  }
  if(!strcmp("/", url) || !url[0]) {
    return asset_request(conn, "/index.html");
  }
  return asset_request(conn, url);
}



int
srv_serve(uint16_t port) {
  struct sockaddr_in server_addr;
  struct sockaddr_in client_addr;
  struct MHD_Daemon *httpd;
  socklen_t addr_len;
  int connfd;
  int srvfd;

  if((srvfd=socket(AF_INET, SOCK_STREAM, 0)) < 0) {
    perror("socket");
    return -1;
  }

  if(setsockopt(srvfd, SOL_SOCKET, SO_REUSEADDR, &(int){1}, sizeof(int)) < 0) {
    perror("setsockopt");
    close(srvfd);
    return -1;
  }

  memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sin_family = AF_INET;
  server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
  server_addr.sin_port = htons(port);

  if(bind(srvfd, (struct sockaddr*)&server_addr, sizeof(server_addr)) != 0) {
    perror("bind");
    close(srvfd);
    return -1;
  }

  if(listen(srvfd, 5) != 0) {
    perror("listen");
    close(srvfd);
    return -1;
  }

  if(!(httpd=MHD_start_daemon(MHD_USE_THREAD_PER_CONNECTION | MHD_USE_ITC |
			      MHD_USE_NO_LISTEN_SOCKET | MHD_USE_DEBUG |
			      MHD_USE_INTERNAL_POLLING_THREAD,
			      0, NULL, NULL, &srv_on_request, NULL,
                              MHD_OPTION_NOTIFY_COMPLETED, NULL,
                              NULL, MHD_OPTION_END))) {
    perror("MHD_start_daemon");
    close(srvfd);
    return -1;
  }

  while(1) {
    addr_len = sizeof(client_addr);
    if((connfd=accept(srvfd, (struct sockaddr*)&client_addr, &addr_len)) < 0) {
      perror("accept");
      break;
    }

    if(MHD_add_connection(httpd, connfd, (struct sockaddr*)&client_addr,
			  addr_len) != MHD_YES) {
      perror("MHD_add_connection");
      break;
    }
  }

  MHD_stop_daemon(httpd);

  return close(srvfd);
}

