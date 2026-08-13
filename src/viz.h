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
 * Respond to a request for a music visualization, e.g.,
 * /viz/master.m3u8?uri=http://live1.sr.se/p3-aac-320
 *
 * The audio is fetched from the uri given as a query argument, and muxed
 * into an mpegts stream together with a video rendition of its spectrum.
 **/
enum MHD_Result viz_request(struct MHD_Connection *conn, const char* url);
