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

#pragma once

#include <microhttpd.h>


/**
 * Start discovery of DLNA services on the local network.
 **/
int dlna_discovery_start(void);


/**
 * Stop discovery of DLNA services on the local network.
 **/
int dlna_discovery_stop(void);


/**
 * Respond to a request for the DLNA API.
 *
 * /dlna                       media servers found on the network
 * /dlna/browse?udn=&id=       children of an object on one of them
 * /dlna/metadata?udn=&id=     that object itself
 **/
enum MHD_Result dlna_request(struct MHD_Connection *conn,
                             const char* url);
