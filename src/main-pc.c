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

#include <signal.h>
#include <stdint.h>

#include <curl/curl.h>
#include <libxml/parser.h>

#include "mdns.h"
#include "srv.h"
#include "dlna.h"


int
main(int argc, char** argv) {
  const uint16_t port = 8088;

  signal(SIGPIPE, SIG_IGN);

  xmlInitParser();

  if(curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
    fprintf(stderr, "curl_global_init failed\n");
    return -1;
  }

  mdns_discovery_start();
  dlna_discovery_start();

  srv_serve(port);

  mdns_discovery_stop();
  dlna_discovery_stop();

  xmlCleanupParser();

  return 0;
}
