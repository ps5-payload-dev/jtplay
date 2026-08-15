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

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include <curl/curl.h>
#include <microhttpd.h>

#include "http.h"


/**
 * Number of bytes buffered between curl and MHD. The buffer starts out at
 * HTTP_BUFFER_SIZE and only grows when curl delivers a chunk that is larger
 * than what is currently allocated, e.g., with HTTP/2, where the size
 * requested via CURLOPT_BUFFERSIZE is not honored.
 **/
#define HTTP_BUFFER_SIZE (64 * 1024)
#define HTTP_BUFFER_MAX  (1024 * 1024)


/**
 * Size of the chunks curl receives, and of the blocks handed to MHD.
 **/
#define HTTP_BLOCK_SIZE (32 * 1024L)


/**
 * Number of seconds an upstream transfer may remain idle before it is
 * considered dead. Transfers that are paused, i.e., where the browser is
 * the slow party, do not count towards this.
 **/
#define HTTP_STALL_TIMEOUT 30


/**
 * State for a single upstream http transfer.
 **/
typedef struct http_stream {
  CURL *easy;
  CURLM *multi;

  struct curl_slist *request_headers;
  struct curl_slist *response_headers;

  /**
   * Tokens listed in the Connection header, which name additional
   * hop-by-hop headers, see RFC 9110.
   **/
  const char* request_connection;
  char* response_connection;

  /**
   * Data received from curl but not yet handed to MHD, stored as
   * buffer_size bytes at buffer_head.
   **/
  unsigned char *buffer;
  size_t buffer_cap;
  size_t buffer_head;
  size_t buffer_size;

  /**
   * Number of bytes already handed to MHD. Also used to check that MHD
   * consumes the callback sequentially, as required by this streaming
   * implementation.
   **/
  uint64_t position;

  /**
   * Point in time when curl last made progress.
   **/
  time_t timestamp;

  int paused;
  int done;
  int running;

  CURLcode result;
  CURLMcode mresult;
  long status;
  curl_off_t content_length;
} http_stream_t;


/**
 * Check if a header describes the connection itself, in which case it must
 * not be forwarded between the browser and the upstream server.
 **/
static int
http_is_hop_by_hop_header(const char* name) {
  return !strcasecmp(name, "Connection") ||
    !strcasecmp(name, "Keep-Alive") ||
    !strcasecmp(name, "Proxy-Authenticate") ||
    !strcasecmp(name, "Proxy-Authorization") ||
    !strcasecmp(name, "TE") ||
    !strcasecmp(name, "Trailer") ||
    !strcasecmp(name, "Transfer-Encoding") ||
    !strcasecmp(name, "Upgrade");
}


/**
 * Check if a name appears in a comma separated list of tokens.
 **/
static int
http_is_listed_token(const char* list, const char* name) {
  const char* end;
  const char* ptr;
  size_t len;

  if(!list) {
    return 0;
  }

  len = strlen(name);
  ptr = list;

  while(*ptr) {
    while(*ptr == ',' || *ptr == ' ' || *ptr == '\t') {
      ptr++;
    }
    if(!*ptr) {
      break;
    }

    for(end=ptr; *end && *end != ','; end++);
    while(end > ptr && (end[-1] == ' ' || end[-1] == '\t')) {
      end--;
    }

    if((size_t)(end - ptr) == len && !strncasecmp(ptr, name, len)) {
      return 1;
    }

    while(*ptr && *ptr != ',') {
      ptr++;
    }
  }

  return 0;
}


/**
 * Check if the upstream transfer has failed.
 **/
static int
http_stream_failed(http_stream_t *stream) {
  return stream->mresult != CURLM_OK || stream->result != CURLE_OK;
}


/**
 * Obtain a description of why the upstream transfer failed.
 **/
static const char*
http_stream_strerror(http_stream_t *stream) {
  if(stream->mresult != CURLM_OK) {
    return curl_multi_strerror(stream->mresult);
  }

  return curl_easy_strerror(stream->result);
}


/**
 * Callback function used to add one browser request header to the curl
 * request.
 **/
static enum MHD_Result
http_request_header_cb(void *cls, enum MHD_ValueKind kind,
		       const char* key, const char* value) {
  http_stream_t *stream = (http_stream_t*)cls;
  struct curl_slist *list;
  char* header;

  if(kind != MHD_HEADER_KIND) {
    return MHD_YES;
  }

  if(http_is_hop_by_hop_header(key) ||
     http_is_listed_token(stream->request_connection, key)) {
    return MHD_YES;
  }

  // curl generates Host itself from CURLOPT_URL, and Content-Length is not
  // needed since only GET requests are supported. Cookie is left out to
  // match the upstream Set-Cookie being dropped, i.e., the proxy is
  // stateless. Origin is left out since the proxy, not the browser, is the
  // origin the upstream server talks to.
  if(!strcasecmp(key, "Host") ||
     !strcasecmp(key, "Content-Length") ||
     !strcasecmp(key, "Cookie") ||
     !strcasecmp(key, "Origin")) {
    return MHD_YES;
  }

  if(asprintf(&header, "%s: %s", key, value) < 0) {
    return MHD_NO;
  }

  if(!(list=curl_slist_append(stream->request_headers, header))) {
    free(header);
    return MHD_NO;
  }

  stream->request_headers = list;
  free(header);

  return MHD_YES;
}


/**
 * Throw away the response headers collected for an intermediate redirect.
 **/
static void
http_clear_response_headers(http_stream_t *stream) {
  curl_slist_free_all(stream->response_headers);
  stream->response_headers = 0;

  free(stream->response_connection);
  stream->response_connection = 0;
}


/**
 * Callback function used to collect upstream response headers.
 *
 * Note: CURLOPT_HEADERFUNCTION is called for every response in a redirect
 * chain, so the header list is reset whenever a new status line arrives.
 **/
static size_t
http_header_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
  http_stream_t *stream = (http_stream_t*)userdata;
  size_t len = size * nmemb;
  struct curl_slist *list;
  const char* value;
  size_t value_len;
  size_t name_len;
  char* header;
  char* colon;
  char* name;

  stream->timestamp = time(0);

  // a new status line starts a new set of response headers
  if(len >= 5 && !memcmp(ptr, "HTTP/", 5)) {
    http_clear_response_headers(stream);
    return len;
  }

  // an empty line terminates the header block. Nothing needs to be done
  // here; the final status is available via CURLINFO_RESPONSE_CODE once
  // the transfer has started.
  if(len == 2 && ptr[0] == '\r' && ptr[1] == '\n') {
    return len;
  }

  if(!(colon=memchr(ptr, ':', len))) {
    return len;
  }

  name_len = colon - ptr;
  value = colon + 1;
  value_len = len - name_len - 1;

  while(value_len && (*value == ' ' || *value == '\t')) {
    value++;
    value_len--;
  }

  while(value_len && (value[value_len-1] == '\r' ||
		      value[value_len-1] == '\n')) {
    value_len--;
  }

  if(!(name=malloc(name_len + 1))) {
    return 0;
  }

  memcpy(name, ptr, name_len);
  name[name_len] = 0;

  // remember which additional headers the upstream server considers
  // hop-by-hop. They are filtered out when the response is assembled,
  // since the Connection header may well arrive last.
  if(!strcasecmp(name, "Connection")) {
    free(stream->response_connection);
    stream->response_connection = strndup(value, value_len);
  }

  // Content-Length is derived by MHD from the response size, and neither
  // hop-by-hop headers nor upstream CORS headers may be copied. The latter
  // would end up alongside the ones added by this proxy, and a response
  // carrying two Access-Control-Allow-Origin headers is rejected outright
  // by the browser.
  if(http_is_hop_by_hop_header(name) ||
     !strncasecmp(name, "Access-Control-", 15) ||
     !strcasecmp(name, "Content-Length") ||
     !strcasecmp(name, "Set-Cookie")) {
    free(name);
    return len;
  }

  free(name);

  if(!(header=malloc(name_len + value_len + 3))) {
    return 0;
  }

  memcpy(header, ptr, name_len);
  header[name_len] = ':';
  header[name_len+1] = ' ';
  memcpy(header + name_len + 2, value, value_len);
  header[name_len+value_len+2] = 0;

  if(!(list=curl_slist_append(stream->response_headers, header))) {
    free(header);
    return 0;
  }

  stream->response_headers = list;
  free(header);

  return len;
}


/**
 * Callback function used to receive data from curl.
 *
 * Note: this is deliberately bounded. When the MHD callback has not consumed
 * enough data, the curl transfer is paused rather than buffered indefinitely.
 * A chunk that does not fit in an empty buffer can not be dealt with that
 * way, since curl redelivers it unchanged once the transfer resumes, so the
 * buffer is grown for those instead.
 **/
static size_t
http_write_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
  http_stream_t *stream = (http_stream_t*)userdata;
  size_t len = size * nmemb;
  unsigned char *buffer;
  size_t cap;

  // reclaim the space consumed by MHD before considering a resize
  if(len > stream->buffer_cap - stream->buffer_head - stream->buffer_size &&
     stream->buffer_head) {
    memmove(stream->buffer, stream->buffer + stream->buffer_head,
	    stream->buffer_size);
    stream->buffer_head = 0;
  }

  if(len > stream->buffer_cap - stream->buffer_head - stream->buffer_size) {
    // let MHD drain what has already been buffered before growing
    if(stream->buffer_size) {
      stream->paused = 1;
      return CURL_WRITEFUNC_PAUSE;
    }

    if(len > HTTP_BUFFER_MAX) {
      return 0;
    }

    for(cap=stream->buffer_cap*2; cap<len; cap*=2);
    if(cap > HTTP_BUFFER_MAX) {
      return 0;
    }

    if(!(buffer=realloc(stream->buffer, cap))) {
      return 0;
    }

    stream->buffer = buffer;
    stream->buffer_cap = cap;
  }

  memcpy(stream->buffer + stream->buffer_head + stream->buffer_size, ptr, len);
  stream->buffer_size += len;
  stream->timestamp = time(0);

  return len;
}


/**
 * Consume curl's completion messages.
 **/
static void
http_check_messages(http_stream_t *stream) {
  CURLMsg *msg;
  int messages;

  while((msg=curl_multi_info_read(stream->multi, &messages))) {
    if(msg->easy_handle != stream->easy) {
      continue;
    }

    if(msg->msg == CURLMSG_DONE) {
      stream->done = 1;
      stream->running = 0;
      stream->result = msg->data.result;
    }
  }
}


/**
 * Run curl until some body data is available, curl has finished, or an
 * error occurs.
 *
 * Note: this runs in the existing MHD connection thread, no additional
 * thread is created.
 **/
static int
http_pump(http_stream_t *stream) {
  CURLMcode res;
  int running;
  int numfds;

  while(1) {
    if((res=curl_multi_perform(stream->multi, &running)) != CURLM_OK) {
      stream->done = 1;
      stream->running = 0;
      stream->mresult = res;
      return -1;
    }

    http_check_messages(stream);

    if(stream->buffer_size || stream->done || stream->paused) {
      return 0;
    }

    if(!running) {
      stream->done = 1;
      stream->running = 0;
      http_check_messages(stream);
      return 0;
    }

    // wait for upstream network activity without blocking the whole
    // process. Since srv.c uses one MHD thread per connection, blocking
    // this particular connection thread is fine.
    if((res=curl_multi_poll(stream->multi, 0, 0, 1000, &numfds)) != CURLM_OK) {
      stream->done = 1;
      stream->running = 0;
      stream->mresult = res;
      return -1;
    }

    // give up on a server that accepts the connection and then goes quiet.
    // CURLOPT_LOW_SPEED_TIME is not used for this, as it would also fire
    // while the transfer is paused waiting for a browser that has simply
    // buffered enough for now.
    if(time(0) - stream->timestamp > HTTP_STALL_TIMEOUT) {
      stream->done = 1;
      stream->running = 0;
      stream->result = CURLE_OPERATION_TIMEDOUT;
      return -1;
    }
  }
}


/**
 * Callback function used to transmit upstream data to a http request.
 **/
static ssize_t
http_read_cb(void *cls, uint64_t pos, char *buf, size_t max) {
  http_stream_t *stream = (http_stream_t*)cls;
  CURLcode res;
  size_t len;

  // this is a forward-only stream. Seeking is represented by the browser
  // making another /http request with a new Range header.
  if(pos != stream->position) {
    return MHD_CONTENT_READER_END_WITH_ERROR;
  }

  while(!stream->buffer_size && !stream->done) {
    if(stream->paused) {
      if((res=curl_easy_pause(stream->easy, CURLPAUSE_CONT)) != CURLE_OK) {
	stream->result = res;
	return MHD_CONTENT_READER_END_WITH_ERROR;
      }
      stream->paused = 0;
      stream->timestamp = time(0);
    }

    if(http_pump(stream) < 0) {
      return MHD_CONTENT_READER_END_WITH_ERROR;
    }
  }

  if(!stream->buffer_size) {
    if(stream->done && !http_stream_failed(stream)) {
      return MHD_CONTENT_READER_END_OF_STREAM;
    }
    return MHD_CONTENT_READER_END_WITH_ERROR;
  }

  len = stream->buffer_size;
  if(len > max) {
    len = max;
  }

  memcpy(buf, stream->buffer + stream->buffer_head, len);

  stream->buffer_head += len;
  stream->buffer_size -= len;
  stream->position += len;

  if(!stream->buffer_size) {
    stream->buffer_head = 0;
  }

  return len;
}


/**
 * Callback function used to close an upstream transfer that has been
 * transmitted via a http request.
 **/
static void
http_close_cb(void *cls) {
  http_stream_t *stream = (http_stream_t*)cls;

  if(stream->multi && stream->easy) {
    curl_multi_remove_handle(stream->multi, stream->easy);
  }
  if(stream->easy) {
    curl_easy_cleanup(stream->easy);
  }
  if(stream->multi) {
    curl_multi_cleanup(stream->multi);
  }

  curl_slist_free_all(stream->request_headers);
  curl_slist_free_all(stream->response_headers);

  free(stream->response_connection);
  free(stream->buffer);
  free(stream);
}


/**
 * Copy upstream response headers into a MHD response.
 **/
static void
http_copy_response_headers(struct MHD_Response *resp, http_stream_t *stream) {
  struct curl_slist *header;
  const char* value;
  char* colon;

  for(header=stream->response_headers; header; header=header->next) {
    if(!(colon=strchr(header->data, ':'))) {
      continue;
    }

    *colon = 0;
    value = colon + 1;

    while(*value == ' ' || *value == '\t') {
      value++;
    }

    if(!http_is_listed_token(stream->response_connection, header->data)) {
      MHD_add_response_header(resp, header->data, value);
    }

    *colon = ':';
  }

  // browser -> localhost -> remote server. The browser sees localhost as
  // the origin of this response, so expose the media/range headers to the
  // javascript running on the page.
  MHD_add_response_header(resp, MHD_HTTP_HEADER_ACCESS_CONTROL_ALLOW_ORIGIN,
			  "*");
  MHD_add_response_header(resp, "Access-Control-Expose-Headers", "*");
}


/**
 * Respond to a http request with a plain text error.
 **/
static enum MHD_Result
http_response_error(struct MHD_Connection *conn, unsigned int status,
		    const char* msg) {
  struct MHD_Response *resp;
  enum MHD_Result ret;

  if(!(resp=MHD_create_response_from_buffer(strlen(msg), (void*)msg,
					    MHD_RESPMEM_MUST_COPY))) {
    return MHD_NO;
  }

  MHD_add_response_header(resp, MHD_HTTP_HEADER_CONTENT_TYPE, "text/plain");
  MHD_add_response_header(resp, MHD_HTTP_HEADER_ACCESS_CONTROL_ALLOW_ORIGIN,
			  "*");

  ret = MHD_queue_response(conn, status, resp);
  MHD_destroy_response(resp);

  return ret;
}


/**
 * Respond to a http request of a remote http resource.
 **/
enum MHD_Result
http_request(struct MHD_Connection *conn, const char* url) {
  struct MHD_Response *resp;
  curl_off_t content_length;
  http_stream_t *stream;
  const char* target;
  enum MHD_Result ret;
  CURLMcode mres;
  CURLcode cres;
  char msg[256];
  long status;

  // /http?url=...
  target = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "url");
  if(!target || !target[0]) {
    return http_response_error(conn, MHD_HTTP_BAD_REQUEST,
			       "missing url parameter\n");
  }

  // keep this endpoint a http(s) proxy rather than allowing arbitrary
  // curl protocols
  if(strncasecmp(target, "http://", 7) &&
     strncasecmp(target, "https://", 8)) {
    return http_response_error(conn, MHD_HTTP_BAD_REQUEST,
			       "only http:// and https:// URLs are "
			       "supported\n");
  }

  if(!(stream=calloc(1, sizeof(http_stream_t)))) {
    return http_response_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
			       "out of memory\n");
  }

  if(!(stream->buffer=malloc(HTTP_BUFFER_SIZE))) {
    free(stream);
    return http_response_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
			       "out of memory\n");
  }

  stream->buffer_cap = HTTP_BUFFER_SIZE;
  stream->timestamp = time(0);

  stream->multi = curl_multi_init();
  stream->easy = curl_easy_init();

  if(!stream->multi || !stream->easy) {
    http_close_cb(stream);
    return http_response_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
			       "curl initialization failed\n");
  }

  curl_easy_setopt(stream->easy, CURLOPT_URL, target);
  curl_easy_setopt(stream->easy, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(stream->easy, CURLOPT_MAXREDIRS, 10L);
  curl_easy_setopt(stream->easy, CURLOPT_PROTOCOLS_STR, "http,https");
  curl_easy_setopt(stream->easy, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
  curl_easy_setopt(stream->easy, CURLOPT_ACCEPT_ENCODING, NULL);
  curl_easy_setopt(stream->easy, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(stream->easy, CURLOPT_BUFFERSIZE, HTTP_BLOCK_SIZE);
  curl_easy_setopt(stream->easy, CURLOPT_CONNECTTIMEOUT, 15L);
  curl_easy_setopt(stream->easy, CURLOPT_WRITEFUNCTION, http_write_cb);
  curl_easy_setopt(stream->easy, CURLOPT_WRITEDATA, stream);
  curl_easy_setopt(stream->easy, CURLOPT_HEADERFUNCTION, http_header_cb);
  curl_easy_setopt(stream->easy, CURLOPT_HEADERDATA, stream);
  curl_easy_setopt(stream->easy, CURLOPT_SSL_VERIFYPEER, 0L);

  stream->request_connection =
    MHD_lookup_connection_value(conn, MHD_HEADER_KIND,
				MHD_HTTP_HEADER_CONNECTION);

  if(MHD_get_connection_values(conn, MHD_HEADER_KIND,
			       http_request_header_cb, stream) == MHD_NO) {
    http_close_cb(stream);
    return http_response_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
			       "failed to collect request headers\n");
  }

  stream->request_connection = 0;

  if(stream->request_headers) {
    curl_easy_setopt(stream->easy, CURLOPT_HTTPHEADER,
		     stream->request_headers);
  }

  curl_easy_setopt(stream->easy, CURLOPT_HTTPGET, 1L);

  if((mres=curl_multi_add_handle(stream->multi, stream->easy)) != CURLM_OK) {
    stream->mresult = mres;
    http_close_cb(stream);
    return http_response_error(conn, MHD_HTTP_BAD_GATEWAY,
			       "failed to create upstream request\n");
  }

  stream->running = 1;

  // pump until the first body data has been received or the request has
  // completed. This is the only potentially blocking part of http_request,
  // and it blocks only this MHD connection thread.
  http_pump(stream);

  cres = curl_easy_getinfo(stream->easy, CURLINFO_RESPONSE_CODE, &status);
  if(cres != CURLE_OK) {
    status = MHD_HTTP_BAD_GATEWAY;
  }
  stream->status = status;

  // ask curl whether the final response has a known body length, where -1
  // means unknown, which is what we want for chunked/live data
  cres = curl_easy_getinfo(stream->easy, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T,
			   &content_length);
  if(cres != CURLE_OK) {
    content_length = -1;
  }
  stream->content_length = content_length;

  // if curl failed before producing anything, return a gateway error
  // rather than handing a dead stream to MHD
  if(stream->done && http_stream_failed(stream) && !stream->buffer_size) {
    snprintf(msg, sizeof(msg), "upstream request failed: %s\n",
	     http_stream_strerror(stream));
    http_close_cb(stream);
    return http_response_error(conn, MHD_HTTP_BAD_GATEWAY, msg);
  }

  if(content_length >= 0) {
    resp = MHD_create_response_from_callback((uint64_t)content_length,
					     HTTP_BLOCK_SIZE, http_read_cb,
					     stream, http_close_cb);
  } else {
    resp = MHD_create_response_from_callback(MHD_SIZE_UNKNOWN,
					     HTTP_BLOCK_SIZE, http_read_cb,
					     stream, http_close_cb);
  }

  if(!resp) {
    http_close_cb(stream);
    return http_response_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
			       "failed to create response\n");
  }

  http_copy_response_headers(resp, stream);

  if(status < 100 || status > 599) {
    status = MHD_HTTP_BAD_GATEWAY;
  }

  ret = MHD_queue_response(conn, (unsigned int)status, resp);
  MHD_destroy_response(resp);

  return ret;
}

