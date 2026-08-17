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
 * Respond to a request for a remuxed rendition of a media resource, e.g.,
 * /remux/master.m3u8?uri=file:///data/movie.mkv
 *
 * The console's browser only plays a handful of containers, and the ones it
 * refuses often hold streams it would have played happily had they arrived in
 * a different wrapper. The remuxer rewraps such a source on the fly, copying
 * the coded packets untouched, so nothing is transcoded and the cost is a
 * demux and a remux rather than a decode and an encode.
 *
 * Sources are addressed by uri, and anything libavformat can open is fair
 * game; file, http and https are the ones that matter here.
 *
 *   /remux/play?uri=       redirects to whichever of the below suits the
 *                          source, and is the one endpoint a caller that
 *                          does not want to know any of this can use
 *   /remux/probe.json?uri= what the source holds, whether the browser could
 *                          play it as-is, and the url to play it with
 *   /remux/master.m3u8?uri= hls playlist, segmented on the source's own
 *                          keyframes so seeking works
 *   /remux/segment.ts?uri=&start=&dur=
 *                          one mpeg-ts segment of the above
 *   /remux/stream.ts?uri=  the whole source as a single mpeg-ts stream, for
 *                          live sources and ones with no usable index
 *   /remux/stream.mp4?uri= the whole source as a fragmented mp4
 *   /remux/stream.webm?uri= the whole source as webm, for vp9 and opus
 **/
enum MHD_Result remux_request(struct MHD_Connection *conn, const char* url);
