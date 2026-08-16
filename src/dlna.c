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

#include <ctype.h>
#include <limits.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>

#include <curl/curl.h>
#include <libxml/parser.h>
#include <libxml/tree.h>

#include "dlna.h"


/**
 * SSDP constants, see the UPnP Device Architecture specification.
 **/
#define SSDP_ADDR "239.255.255.250"
#define SSDP_PORT 1900
#define SSDP_TTL  4
#define SSDP_MX   3

/**
 * Search target used when probing the network. Devices respond once per
 * service they provide, e.g., upnp:rootdevice, uuid:<id>, and
 * urn:schemas-upnp-org:device:MediaServer:1.
 **/
#define SSDP_ST "ssdp:all"

/**
 * Number of seconds between two rounds of M-SEARCH requests.
 **/
#define SSDP_INTERVAL 30

/**
 * TTL used for responses that omit a CACHE-CONTROL header.
 **/
#define SSDP_DEFAULT_TTL 180

/**
 * The service types worth talking to. A media server announces the device,
 * and the ContentDirectory service it is reached through, separately.
 **/
#define DLNA_DEVICE_URN  "urn:schemas-upnp-org:device:MediaServer:"
#define DLNA_SERVICE_URN "urn:schemas-upnp-org:service:ContentDirectory:"

/**
 * Number of seconds a parsed device description is reused before the
 * description is read again.
 **/
#define DLNA_DEVICE_TTL 300

/**
 * Limits on what is accepted from, and asked of, a media server. Browsing
 * is paged, since asking a server with a large library for everything at
 * once is how a server ends up refusing to answer at all.
 **/
#define DLNA_BODY_MAX   (8 * 1024 * 1024)
#define DLNA_PAGE_SIZE  200
#define DLNA_MAX_ITEMS  5000
#define DLNA_MAX_PAGES  64

/**
 * Seconds before a request to a media server is abandoned.
 **/
#define DLNA_CONNECT_TIMEOUT 5
#define DLNA_TIMEOUT         20


/**
 * Data structure used to keep track of services.
 **/
typedef struct service_seq {
  char usn[256];
  char st[256];
  char server[256];
  char location[512];
  char addr[INET_ADDRSTRLEN];
  uint16_t port;
  time_t ttl;
  struct service_seq* next;
} service_seq_t;


/**
 * Data structure used to keep track of media servers, i.e., what a device
 * description says about the service announced at a location.
 **/
typedef struct device_seq {
  char location[512];
  char udn[256];
  char name[256];
  char manufacturer[128];
  char model[128];
  char service[128];
  char control[512];
  char icon[512];
  char addr[INET_ADDRSTRLEN];
  uint16_t port;
  time_t ttl;
  struct device_seq* next;
} device_seq_t;


/**
 * The location of a service, copied out of the service list so it can be
 * used without holding the lock.
 **/
typedef struct dlna_target {
  char location[512];
  char addr[INET_ADDRSTRLEN];
  uint16_t port;
} dlna_target_t;


/**
 * Global state variables.
 **/
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_dev_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t g_thread;
static bool g_running = false;
static service_seq_t* g_service_seq = 0;
static device_seq_t* g_device_seq = 0;


/**
 * Check if the discovery thread should keep running.
 **/
static bool
ssdp_is_running(void) {
  bool running;

  pthread_mutex_lock(&g_lock);
  running = g_running;
  pthread_mutex_unlock(&g_lock);

  return running;
}


/**
 * Remove unresponsive services.
 **/
static void
ssdp_purge_services(void) {
  service_seq_t* prev = 0;
  service_seq_t* curr = 0;

  pthread_mutex_lock(&g_lock);

  curr = g_service_seq;
  while(curr) {
    if(curr->ttl > time(0)) {
      prev = curr;
      curr = curr->next;
    } else if(prev) {
      prev->next = curr->next;
      free(curr);
      curr = prev->next;
    } else {
      g_service_seq = curr->next;
      free(curr);
      curr = g_service_seq;
    }
  }

  pthread_mutex_unlock(&g_lock);
}


/**
 * Remove all known services.
 **/
static void
ssdp_flush_services(void) {
  service_seq_t* curr;
  service_seq_t* next;

  pthread_mutex_lock(&g_lock);

  curr = g_service_seq;
  g_service_seq = 0;

  pthread_mutex_unlock(&g_lock);

  while(curr) {
    next = curr->next;
    free(curr);
    curr = next;
  }
}


/**
 * Remove all cached device descriptions.
 **/
static void
dlna_flush_devices(void) {
  device_seq_t* curr;
  device_seq_t* next;

  pthread_mutex_lock(&g_dev_lock);

  curr = g_device_seq;
  g_device_seq = 0;

  pthread_mutex_unlock(&g_dev_lock);

  while(curr) {
    next = curr->next;
    free(curr);
    curr = next;
  }
}


/**
 * Copy the value of an HTTP-style header into a buffer.
 **/
static int
ssdp_header_value(const char* resp, const char* name, char* buf, size_t size) {
  size_t namelen = strlen(name);
  const char* line;
  const char* end;
  const char* val;
  size_t linelen;
  size_t len;

  buf[0] = 0;

  for(line=resp; line && *line;) {
    end = strpbrk(line, "\r\n");
    linelen = end ? (size_t)(end - line) : strlen(line);

    if(linelen > namelen && !strncasecmp(line, name, namelen)) {
      val = line + namelen;

      while(*val == ' ' || *val == '\t') {
        val++;
      }

      if(*val == ':') {
        val++;
        while(*val == ' ' || *val == '\t') {
          val++;
        }

        if((size_t)(val - line) >= linelen) {
          return 0;
        }

        len = linelen - (size_t)(val - line);
        while(len && (val[len-1] == ' ' || val[len-1] == '\t')) {
          len--;
        }
        if(len >= size) {
          len = size - 1;
        }

        memcpy(buf, val, len);
        buf[len] = 0;

        return 0;
      }
    }

    if(!end) {
      break;
    }
    line = end + strspn(end, "\r\n");
  }

  return -1;
}


/**
 * Parse the max-age directive of a CACHE-CONTROL header.
 **/
static time_t
ssdp_cache_ttl(const char* resp) {
  char buf[128] = {0};
  const char* p;

  if(ssdp_header_value(resp, "CACHE-CONTROL", buf, sizeof(buf))) {
    return SSDP_DEFAULT_TTL;
  }

  for(p=buf; *p; p++) {
    if(strncasecmp(p, "max-age", 7)) {
      continue;
    }

    p += 7;
    while(*p == ' ' || *p == '\t') {
      p++;
    }
    if(*p != '=') {
      continue;
    }

    p++;
    while(*p == ' ' || *p == '\t') {
      p++;
    }

    if(atoi(p) > 0) {
      return (time_t)atoi(p);
    }
    break;
  }

  return SSDP_DEFAULT_TTL;
}


/**
 * Parse the port number of a URL
 **/
static uint16_t
ssdp_location_port(const char* url) {
  const char* p;

  if(!(p=strstr(url, "://"))) {
    return 0;
  }

  for(p+=3; *p && *p != '/' && *p != ':'; p++);

  if(*p == ':') {
    return (uint16_t)atoi(p + 1);
  }

  if(!strncmp("https://", url, 8)) {
    return 443;
  }

  return 80;
}


/**
 * Forget a service that announced its departure.
 **/
static void
ssdp_service_lost(const char* usn) {
  service_seq_t* ss;

  pthread_mutex_lock(&g_lock);

  for(ss=g_service_seq; ss; ss=ss->next) {
    if(!strcmp(ss->usn, usn)) {
      ss->ttl = 0;
      break;
    }
  }

  pthread_mutex_unlock(&g_lock);

  ssdp_purge_services();
}


/**
 * Remember a service announced via SSDP.
 **/
static void
ssdp_service_found(const char* resp, const char* addr) {
  service_seq_t* ss = 0;
  char location[512];
  char server[256];
  char usn[256];
  char nts[64];
  char st[256];
  time_t ttl;

  if(ssdp_header_value(resp, "USN", usn, sizeof(usn)) || !usn[0]) {
    return;
  }

  // NOTIFY messages use NT instead of ST, and NTS to signal their intent
  if(ssdp_header_value(resp, "ST", st, sizeof(st))) {
    ssdp_header_value(resp, "NT", st, sizeof(st));
  }
  if(!ssdp_header_value(resp, "NTS", nts, sizeof(nts))) {
    if(!strncasecmp(nts, "ssdp:byebye", 11)) {
      ssdp_service_lost(usn);
      return;
    }
  }

  if(ssdp_header_value(resp, "LOCATION", location, sizeof(location)) ||
     !location[0]) {
    return;
  }
  ssdp_header_value(resp, "SERVER", server, sizeof(server));

  ttl = ssdp_cache_ttl(resp);

  pthread_mutex_lock(&g_lock);

  for(ss=g_service_seq; ss; ss=ss->next) {
    if(!strcmp(ss->usn, usn)) {
      break;
    }
  }

  if(!ss) {
    if(!(ss=malloc(sizeof(service_seq_t)))) {
      pthread_mutex_unlock(&g_lock);
      return;
    }
    snprintf(ss->usn, sizeof(ss->usn), "%s", usn);
    ss->next = g_service_seq;
    g_service_seq = ss;
  }

  snprintf(ss->st, sizeof(ss->st), "%s", st);
  snprintf(ss->server, sizeof(ss->server), "%s", server);
  snprintf(ss->location, sizeof(ss->location), "%s", location);
  snprintf(ss->addr, sizeof(ss->addr), "%s", addr);
  ss->port = ssdp_location_port(location);
  ss->ttl = time(0) + ttl;

  pthread_mutex_unlock(&g_lock);
}


/**
 * Open a socket for SSDP traffic.
 **/
static int
ssdp_socket_open(void) {
  struct sockaddr_in sin;
  struct ip_mreq mreq;
  int ttl = SSDP_TTL;
  struct timeval tv;
  int reuse = 1;
  int fd;

  if((fd=socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) < 0) {
    perror("socket");
    return -1;
  }

  if(setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
    perror("setsockopt");
  }
  if(setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse)) < 0) {
    perror("setsockopt");
  }

  memset(&sin, 0, sizeof(sin));
  sin.sin_family = AF_INET;
  sin.sin_port = htons(SSDP_PORT);
  sin.sin_addr.s_addr = htonl(INADDR_ANY);

  if(bind(fd, (struct sockaddr*)&sin, sizeof(sin)) < 0) {
    perror("bind");
  } else {
    memset(&mreq, 0, sizeof(mreq));
    mreq.imr_multiaddr.s_addr = inet_addr(SSDP_ADDR);
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);

    if(setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
      perror("setsockopt");
    }
  }

  if(setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl)) < 0) {
    perror("setsockopt");
    close(fd);
    return -1;
  }

  tv.tv_sec = 1;
  tv.tv_usec = 0;
  if(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
    perror("setsockopt");
    close(fd);
    return -1;
  }

  return fd;
}


/**
 * Multicast an M-SEARCH request.
 **/
static int
ssdp_send_msearch(int fd) {
  struct sockaddr_in sin;
  char req[512];
  int len;

  len = snprintf(req, sizeof(req),
                 "M-SEARCH * HTTP/1.1\r\n"
                 "HOST: " SSDP_ADDR ":%d\r\n"
                 "MAN: \"ssdp:discover\"\r\n"
                 "MX: %d\r\n"
                 "ST: " SSDP_ST "\r\n"
                 "\r\n", SSDP_PORT, SSDP_MX);

  memset(&sin, 0, sizeof(sin));
  sin.sin_family = AF_INET;
  sin.sin_port = htons(SSDP_PORT);
  sin.sin_addr.s_addr = inet_addr(SSDP_ADDR);

  if(sendto(fd, req, len, 0, (struct sockaddr*)&sin, sizeof(sin)) < 0) {
    perror("sendto");
    return -1;
  }

  return 0;
}


/**
 * Thread for running SSDP service discovery.
 **/
static void*
ssdp_discovery_thread(void* args) {
  char addr[INET_ADDRSTRLEN];
  struct sockaddr_in sin;
  time_t deadline;
  char buf[4096];
  socklen_t len;
  ssize_t size;
  int fd;

  if((fd=ssdp_socket_open()) < 0) {
    return 0;
  }

  while(ssdp_is_running()) {
    ssdp_send_msearch(fd);

    deadline = time(0) + SSDP_INTERVAL;
    while(ssdp_is_running() && time(0) < deadline) {
      len = sizeof(sin);
      size = recvfrom(fd, buf, sizeof(buf) - 1, 0,
                      (struct sockaddr*)&sin, &len);
      if(size < 0) {
        if(errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
          continue;
        }
        perror("recvfrom");
        break;
      }

      buf[size] = 0;
      if(!inet_ntop(AF_INET, &sin.sin_addr, addr, sizeof(addr))) {
        continue;
      }

      ssdp_service_found(buf, addr);
    }

    ssdp_purge_services();
  }

  close(fd);

  ssdp_flush_services();
  dlna_flush_devices();

  pthread_mutex_lock(&g_lock);
  g_running = false;
  pthread_mutex_unlock(&g_lock);

  return 0;
}


int
dlna_discovery_stop(void) {
  bool stop;

  pthread_mutex_lock(&g_lock);
  stop = g_running;
  g_running = false;
  pthread_mutex_unlock(&g_lock);

  if(!stop) {
    return -1;
  }

  if(pthread_join(g_thread, 0)) {
    return -1;
  }

  return 0;
}


int
dlna_discovery_start(void) {
  bool start;

  pthread_mutex_lock(&g_lock);
  start = !g_running;
  g_running = true;
  pthread_mutex_unlock(&g_lock);

  if(!start) {
    return -1;
  }

  return pthread_create(&g_thread, 0, ssdp_discovery_thread, 0);
}



/**
 * A growable string, used to assemble both the requests sent to a media
 * server and the JSON handed back to the browser.
 **/
typedef struct strbuf {
  char* ptr;
  size_t len;
  size_t cap;
} strbuf_t;


/**
 * How the descriptions and responses of a media server are parsed. Recovery
 * is on because plenty of servers emit XML that is not quite well formed,
 * and one stray tag should not cost a whole library listing. Nothing is
 * fetched over the network on the parser's behalf, and entities are left
 * unexpanded, so a hostile doctype has nothing to do.
 **/
#define DLNA_XML_FLAGS (XML_PARSE_NOERROR | XML_PARSE_NOWARNING | \
                        XML_PARSE_NONET | XML_PARSE_RECOVER | \
                        XML_PARSE_NOBLANKS)


/**
 * Make room for len more bytes, plus the terminator.
 **/
static int
sb_grow(strbuf_t* sb, size_t len) {
  size_t cap;
  char* ptr;

  if(sb->cap > sb->len + len) {
    return 0;
  }

  cap = sb->cap ? sb->cap : 256;
  while(cap <= sb->len + len) {
    cap *= 2;
  }

  if(!(ptr=realloc(sb->ptr, cap))) {
    return -1;
  }

  sb->ptr = ptr;
  sb->cap = cap;

  return 0;
}


static int
sb_add(strbuf_t* sb, const char* data, size_t len) {
  if(sb_grow(sb, len)) {
    return -1;
  }

  memcpy(sb->ptr + sb->len, data, len);
  sb->len += len;
  sb->ptr[sb->len] = 0;

  return 0;
}


static int
sb_str(strbuf_t* sb, const char* str) {
  return sb_add(sb, str, strlen(str));
}


static int
sb_fmt(strbuf_t* sb, const char* fmt, ...) {
  va_list ap;
  int len;

  va_start(ap, fmt);
  len = vsnprintf(0, 0, fmt, ap);
  va_end(ap);

  if(len < 0 || sb_grow(sb, (size_t)len)) {
    return -1;
  }

  va_start(ap, fmt);
  vsnprintf(sb->ptr + sb->len, (size_t)len + 1, fmt, ap);
  va_end(ap);

  sb->len += (size_t)len;

  return 0;
}


/**
 * Append a JSON string, quotes included.
 **/
static int
sb_json(strbuf_t* sb, const char* str) {
  char esc[8];

  if(sb_add(sb, "\"", 1)) {
    return -1;
  }

  for(; str && *str; str++) {
    switch(*str) {
    case '"':
      if(sb_add(sb, "\\\"", 2)) return -1;
      break;

    case '\\':
      if(sb_add(sb, "\\\\", 2)) return -1;
      break;

    case '\n':
      if(sb_add(sb, "\\n", 2)) return -1;
      break;

    case '\r':
      if(sb_add(sb, "\\r", 2)) return -1;
      break;

    case '\t':
      if(sb_add(sb, "\\t", 2)) return -1;
      break;

    default:
      if((unsigned char)*str < 0x20) {
        snprintf(esc, sizeof(esc), "\\u%04x", (unsigned char)*str);
        if(sb_add(sb, esc, strlen(esc))) return -1;
      } else if(sb_add(sb, str, 1)) {
        return -1;
      }
      break;
    }
  }

  return sb_add(sb, "\"", 1);
}


/**
 * Append text with the five predefined XML entities escaped. Object ids are
 * server-generated and routinely contain an ampersand.
 **/
static int
sb_xml(strbuf_t* sb, const char* str) {
  for(; str && *str; str++) {
    switch(*str) {
    case '&':
      if(sb_str(sb, "&amp;")) return -1;
      break;
    case '<':
      if(sb_str(sb, "&lt;")) return -1;
      break;
    case '>':
      if(sb_str(sb, "&gt;")) return -1;
      break;
    case '"':
      if(sb_str(sb, "&quot;")) return -1;
      break;
    case '\'':
      if(sb_str(sb, "&apos;")) return -1;
      break;
    default:
      if(sb_add(sb, str, 1)) return -1;
      break;
    }
  }

  return 0;
}


static void
sb_free(strbuf_t* sb) {
  free(sb->ptr);
  sb->ptr = 0;
  sb->len = 0;
  sb->cap = 0;
}


/**
 * Find the first element with the given name below a node, at any depth.
 *
 * Note: libxml2 keeps the namespace prefix in ->ns, so ->name is already
 * the local name and dc:title answers to "title", which is what makes this
 * work across servers that disagree about prefixes.
 **/
static xmlNodePtr
xml_find(xmlNodePtr node, const char* name) {
  xmlNodePtr child;
  xmlNodePtr hit;

  for(child=node ? node->children : 0; child; child=child->next) {
    if(child->type != XML_ELEMENT_NODE) {
      continue;
    }
    if(!xmlStrcmp(child->name, (const xmlChar*)name)) {
      return child;
    }
    if((hit=xml_find(child, name))) {
      return hit;
    }
  }

  return 0;
}


/**
 * Copy the text of a node into a fixed buffer, trimmed. Entities and CDATA
 * are already resolved by this point.
 **/
static void
xml_text(xmlNodePtr node, char* buf, size_t size) {
  const char* ptr;
  xmlChar* val;
  size_t len;

  buf[0] = 0;

  if(!node || !(val=xmlNodeGetContent(node))) {
    return;
  }

  ptr = (const char*)val;
  while(*ptr && isspace((unsigned char)*ptr)) {
    ptr++;
  }

  len = strlen(ptr);
  while(len && isspace((unsigned char)ptr[len-1])) {
    len--;
  }

  if(len >= size) {
    len = size - 1;
  }

  memcpy(buf, ptr, len);
  buf[len] = 0;

  xmlFree(val);
}


/**
 * Copy the text of the first descendant element with the given name.
 **/
static int
xml_child_text(xmlNodePtr node, const char* name, char* buf, size_t size) {
  xmlNodePtr child;

  buf[0] = 0;

  if(!(child=xml_find(node, name))) {
    return -1;
  }

  xml_text(child, buf, size);

  return 0;
}


/**
 * Copy the value of an attribute.
 **/
static int
xml_attr(xmlNodePtr node, const char* name, char* buf, size_t size) {
  xmlChar* val;

  buf[0] = 0;

  if(!node || !(val=xmlGetNoNsProp(node, (const xmlChar*)name))) {
    return -1;
  }

  snprintf(buf, size, "%s", (const char*)val);
  xmlFree(val);

  return 0;
}


/**
 * Check if a node is an element with the given name.
 **/
static int
xml_is(xmlNodePtr node, const char* name) {
  return node && node->type == XML_ELEMENT_NODE &&
    !xmlStrcmp(node->name, (const xmlChar*)name);
}


/**
 * Parse a document held in memory.
 **/
static xmlDocPtr
xml_parse(const char* buf, size_t len, const char* url) {
  if(!buf || !len || len > (size_t)INT_MAX) {
    return 0;
  }

  return xmlReadMemory(buf, (int)len, url, 0, DLNA_XML_FLAGS);
}


/**
 * Check if a url ends in something the browser will render as an image.
 * The shell ignores an icon url it does not recognize, so an icon that is
 * not obviously an image is not worth announcing.
 **/
static int
dlna_is_image_url(const char* url) {
  static const char* ext[] = {".png", ".jpg", ".jpeg", ".gif", ".webp", 0};
  const char* end;
  size_t elen;
  size_t len;
  int i;

  if(!url || !url[0]) {
    return 0;
  }

  end = strpbrk(url, "?#");
  len = end ? (size_t)(end - url) : strlen(url);

  for(i=0; ext[i]; i++) {
    elen = strlen(ext[i]);
    if(len > elen && !strncasecmp(url + len - elen, ext[i], elen)) {
      return 1;
    }
  }

  return 0;
}


/**
 * Resolve a url found in a device description against the url the
 * description itself was read from. Both absolute and relative forms are
 * seen in the wild, and some servers give the control url as a bare path.
 **/
static void
dlna_url_resolve(const char* base, const char* rel, char* buf, size_t size) {
  const char* path;
  const char* slash;
  const char* end;

  buf[0] = 0;

  if(!rel || !rel[0]) {
    return;
  }

  if(!strncasecmp(rel, "http://", 7) || !strncasecmp(rel, "https://", 8)) {
    snprintf(buf, size, "%s", rel);
    return;
  }

  if(!(path=strstr(base, "://"))) {
    snprintf(buf, size, "%s", rel);
    return;
  }

  for(path+=3; *path && *path != '/'; path++);

  if(rel[0] == '/') {
    snprintf(buf, size, "%.*s%s", (int)(path - base), base, rel);
    return;
  }

  // relative to the directory the description lives in
  end = strpbrk(path, "?#");
  slash = end ? end : path + strlen(path);
  while(slash > path && *slash != '/') {
    slash--;
  }

  if(*slash != '/') {
    snprintf(buf, size, "%.*s/%s", (int)(path - base), base, rel);
  } else {
    snprintf(buf, size, "%.*s/%s", (int)(slash - base), base, rel);
  }
}


/**
 * Callback function used to collect a response from a media server.
 **/
static size_t
dlna_write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
  strbuf_t* sb = (strbuf_t*)userdata;
  size_t len = size * nmemb;

  if(sb->len + len > DLNA_BODY_MAX) {
    return 0;
  }

  if(sb_add(sb, ptr, len)) {
    return 0;
  }

  return len;
}


/**
 * Read a url from a media server, or invoke an action on it when a body is
 * given. This blocks, which is fine; srv.c runs a thread per connection.
 **/
static int
dlna_http(const char* url, const char* action, const char* body,
          strbuf_t* out) {
  struct curl_slist* headers = 0;
  struct curl_slist* list;
  long status = 0;
  CURLcode res;
  char buf[256];
  CURL* easy;

  if(!(easy=curl_easy_init())) {
    return -1;
  }

  curl_easy_setopt(easy, CURLOPT_URL, url);
  curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(easy, CURLOPT_MAXREDIRS, 5L);
  curl_easy_setopt(easy, CURLOPT_PROTOCOLS_STR, "http,https");
  curl_easy_setopt(easy, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
  curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(easy, CURLOPT_SSL_VERIFYPEER, 0L);
  curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT, (long)DLNA_CONNECT_TIMEOUT);
  curl_easy_setopt(easy, CURLOPT_TIMEOUT, (long)DLNA_TIMEOUT);
  curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, dlna_write_cb);
  curl_easy_setopt(easy, CURLOPT_WRITEDATA, out);
  curl_easy_setopt(easy, CURLOPT_USERAGENT, "jtplay/1.0 UPnP/1.0 DLNADOC/1.50");

  if(body) {
    // an embedded media server is easily upset by a continuation it never
    // asked for, and curl volunteers one for bodies above 1k
    if((list=curl_slist_append(headers, "Expect:"))) {
      headers = list;
    }
    if((list=curl_slist_append(headers,
                               "Content-Type: text/xml; charset=\"utf-8\""))) {
      headers = list;
    }

    snprintf(buf, sizeof(buf), "SOAPAction: \"%s\"", action);
    if((list=curl_slist_append(headers, buf))) {
      headers = list;
    }

    curl_easy_setopt(easy, CURLOPT_POST, 1L);
    curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE, (long)strlen(body));
    curl_easy_setopt(easy, CURLOPT_COPYPOSTFIELDS, body);
    curl_easy_setopt(easy, CURLOPT_HTTPHEADER, headers);
  }

  res = curl_easy_perform(easy);
  curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &status);

  curl_slist_free_all(headers);
  curl_easy_cleanup(easy);

  if(res != CURLE_OK) {
    fprintf(stderr, "dlna: %s: %s\n", url, curl_easy_strerror(res));
    return -1;
  }

  if(status != 200) {
    fprintf(stderr, "dlna: %s: HTTP %ld\n", url, status);
    return -1;
  }

  return 0;
}


/**
 * Look for the ContentDirectory service anywhere in a device description.
 *
 * Note: a description may hold several devices, but a media server that
 * embeds another one offering its own ContentDirectory is not something
 * this looks for; the first service found is the one used.
 **/
static int
dlna_scan_services(xmlNodePtr node, char* type, size_t tsize,
                   char* control, size_t csize) {
  xmlNodePtr child;
  char buf[256];

  for(child=node ? node->children : 0; child; child=child->next) {
    if(child->type != XML_ELEMENT_NODE) {
      continue;
    }

    if(xml_is(child, "service") &&
       !xml_child_text(child, "serviceType", buf, sizeof(buf)) &&
       strstr(buf, DLNA_SERVICE_URN) &&
       !xml_child_text(child, "controlURL", control, csize) &&
       control[0]) {
      snprintf(type, tsize, "%s", buf);
      return 0;
    }

    if(!dlna_scan_services(child, type, tsize, control, csize)) {
      return 0;
    }
  }

  return -1;
}


/**
 * Look for an icon the shell can display.
 **/
static int
dlna_scan_icons(xmlNodePtr node, char* url, size_t size) {
  xmlNodePtr child;

  for(child=node ? node->children : 0; child; child=child->next) {
    if(child->type != XML_ELEMENT_NODE) {
      continue;
    }

    if(xml_is(child, "icon") &&
       !xml_child_text(child, "url", url, size) &&
       dlna_is_image_url(url)) {
      return 0;
    }

    if(!dlna_scan_icons(child, url, size)) {
      return 0;
    }
  }

  url[0] = 0;

  return -1;
}


/**
 * Read and parse the device description a service announced.
 **/
static int
dlna_describe(const dlna_target_t* target, device_seq_t* dev) {
  strbuf_t body = {0};
  xmlNodePtr root;
  xmlDocPtr doc;
  char base[512];
  char rel[512];

  if(dlna_http(target->location, 0, 0, &body) || !body.ptr) {
    sb_free(&body);
    return -1;
  }

  doc = xml_parse(body.ptr, body.len, target->location);
  sb_free(&body);

  if(!doc) {
    return -1;
  }

  if(!(root=xmlDocGetRootElement(doc))) {
    xmlFreeDoc(doc);
    return -1;
  }

  memset(dev, 0, sizeof(*dev));

  // URLBase is gone from UPnP 1.1, but plenty of servers still send it
  xml_child_text(root, "URLBase", base, sizeof(base));
  if(!base[0]) {
    snprintf(base, sizeof(base), "%s", target->location);
  }

  xml_child_text(root, "UDN", dev->udn, sizeof(dev->udn));
  xml_child_text(root, "friendlyName", dev->name, sizeof(dev->name));
  xml_child_text(root, "manufacturer", dev->manufacturer,
                 sizeof(dev->manufacturer));
  xml_child_text(root, "modelName", dev->model, sizeof(dev->model));

  if(dlna_scan_services(root, dev->service, sizeof(dev->service),
                        rel, sizeof(rel))) {
    xmlFreeDoc(doc);
    return -1;
  }
  dlna_url_resolve(base, rel, dev->control, sizeof(dev->control));

  if(!dlna_scan_icons(root, rel, sizeof(rel))) {
    dlna_url_resolve(base, rel, dev->icon, sizeof(dev->icon));
  }

  xmlFreeDoc(doc);

  if(!dev->udn[0] || !dev->control[0]) {
    return -1;
  }

  if(!dev->name[0]) {
    snprintf(dev->name, sizeof(dev->name), "%s", target->addr);
  }

  snprintf(dev->location, sizeof(dev->location), "%s", target->location);
  snprintf(dev->addr, sizeof(dev->addr), "%s", target->addr);
  dev->port = target->port;
  dev->ttl = time(0) + DLNA_DEVICE_TTL;

  return 0;
}


/**
 * Copy out what is known about a media server, if it is still current.
 **/
static int
dlna_device_by_location(const char* location, device_seq_t* out) {
  device_seq_t* dev;
  int found = -1;

  pthread_mutex_lock(&g_dev_lock);

  for(dev=g_device_seq; dev; dev=dev->next) {
    if(!strcmp(dev->location, location) && dev->ttl > time(0)) {
      memcpy(out, dev, sizeof(*out));
      out->next = 0;
      found = 0;
      break;
    }
  }

  pthread_mutex_unlock(&g_dev_lock);

  return found;
}


static int
dlna_device_by_udn(const char* udn, device_seq_t* out) {
  device_seq_t* dev;
  int found = -1;

  pthread_mutex_lock(&g_dev_lock);

  for(dev=g_device_seq; dev; dev=dev->next) {
    if(!strcmp(dev->udn, udn)) {
      memcpy(out, dev, sizeof(*out));
      out->next = 0;
      found = 0;
      break;
    }
  }

  pthread_mutex_unlock(&g_dev_lock);

  return found;
}


static void
dlna_device_store(const device_seq_t* src) {
  device_seq_t* dev;
  device_seq_t* next;

  pthread_mutex_lock(&g_dev_lock);

  for(dev=g_device_seq; dev; dev=dev->next) {
    if(!strcmp(dev->location, src->location)) {
      break;
    }
  }

  if(dev) {
    // overwriting in place, so the rest of the list has to survive the copy
    next = dev->next;
    memcpy(dev, src, sizeof(*src));
    dev->next = next;
  } else {
    if(!(dev=malloc(sizeof(device_seq_t)))) {
      pthread_mutex_unlock(&g_dev_lock);
      return;
    }
    memcpy(dev, src, sizeof(*src));
    dev->next = g_device_seq;
    g_device_seq = dev;
  }

  pthread_mutex_unlock(&g_dev_lock);
}


/**
 * Copy the locations that announced a media server, so the list can be
 * worked through without holding the lock while talking to the network.
 **/
static size_t
dlna_targets(dlna_target_t** out) {
  dlna_target_t* list;
  service_seq_t* ss;
  size_t count = 0;
  size_t used = 0;
  size_t i;

  *out = 0;

  pthread_mutex_lock(&g_lock);

  for(ss=g_service_seq; ss; ss=ss->next) {
    count++;
  }

  if(!count || !(list=calloc(count, sizeof(dlna_target_t)))) {
    pthread_mutex_unlock(&g_lock);
    return 0;
  }

  for(ss=g_service_seq; ss; ss=ss->next) {
    if(!strstr(ss->st, DLNA_DEVICE_URN) &&
       !strstr(ss->st, DLNA_SERVICE_URN)) {
      continue;
    }

    // a device answers once per service it provides, so the same
    // description is announced several times over
    for(i=0; i<used; i++) {
      if(!strcmp(list[i].location, ss->location)) {
        break;
      }
    }
    if(i < used) {
      continue;
    }

    snprintf(list[used].location, sizeof(list[used].location), "%s",
             ss->location);
    snprintf(list[used].addr, sizeof(list[used].addr), "%s", ss->addr);
    list[used].port = ss->port;
    used++;
  }

  pthread_mutex_unlock(&g_lock);

  if(!used) {
    free(list);
    return 0;
  }

  *out = list;

  return used;
}


/**
 * Read the description of every announced media server that has not been
 * read recently.
 **/
static void
dlna_refresh(void) {
  dlna_target_t* targets;
  device_seq_t dev;
  size_t count;
  size_t i;

  count = dlna_targets(&targets);

  for(i=0; i<count; i++) {
    if(!dlna_device_by_location(targets[i].location, &dev)) {
      continue;
    }
    if(!dlna_describe(&targets[i], &dev)) {
      dlna_device_store(&dev);
    }
  }

  free(targets);
}


/**
 * Media types the console's browser can hand straight to a media element.
 * A server that offers both the original and a transcode lists them side by
 * side, so preferring one of these is what picks the transcode of, say, a
 * matroska file that would otherwise not play at all.
 **/
static int
dlna_mime_is_playable(const char* mime) {
  static const char* known[] = {
    "video/mp4", "video/webm", "video/quicktime", "video/3gpp",
    "video/mpeg", "audio/mpeg", "audio/mp4", "audio/aac", "audio/x-m4a",
    "audio/mp4a-latm", "audio/wav", "audio/x-wav", "audio/flac",
    "audio/x-flac", "audio/ogg", "audio/vorbis",
    "application/vnd.apple.mpegurl", "application/x-mpegurl", 0
  };
  int i;

  for(i=0; known[i]; i++) {
    if(!strcasecmp(mime, known[i])) {
      return 1;
    }
  }

  return 0;
}


/**
 * Pull the media type out of a protocolInfo, which reads
 * <protocol>:<network>:<mime>:<extras>.
 **/
static void
dlna_res_mime(const char* proto, char* buf, size_t size) {
  const char* ptr = proto;
  const char* end;
  size_t len;
  int i;

  buf[0] = 0;

  for(i=0; i<2; i++) {
    if(!(ptr=strchr(ptr, ':'))) {
      return;
    }
    ptr++;
  }

  end = strchr(ptr, ':');
  len = end ? (size_t)(end - ptr) : strlen(ptr);

  if(len >= size) {
    len = size - 1;
  }

  memcpy(buf, ptr, len);
  buf[len] = 0;
}


/**
 * Choose the resource to play out of the ones an item offers.
 **/
static void
dlna_pick_res(xmlNodePtr item, char* uri, size_t size) {
  xmlNodePtr child;
  char proto[256];
  char mime[128];
  char url[1024];
  int best = 0;
  int score;

  uri[0] = 0;

  for(child=item ? item->children : 0; child; child=child->next) {
    if(!xml_is(child, "res")) {
      continue;
    }

    xml_text(child, url, sizeof(url));
    if(!url[0]) {
      continue;
    }

    // the console fetches the media itself, so anything that is not plain
    // http is of no use here
    if(xml_attr(child, "protocolInfo", proto, sizeof(proto)) ||
       strncasecmp(proto, "http-get:", 9)) {
      continue;
    }

    dlna_res_mime(proto, mime, sizeof(mime));
    score = dlna_mime_is_playable(mime) ? 2 : 1;

    if(score > best) {
      best = score;
      snprintf(uri, size, "%s", url);
    }
  }
}


/**
 * Map a upnp:class onto one of the types the shell knows about.
 **/
static const char*
dlna_entry_type(const char* class_name, int container) {
  if(container || !strncmp(class_name, "object.container", 16)) {
    return "folder";
  }
  if(strstr(class_name, "videoItem")) {
    return "video";
  }
  if(strstr(class_name, "audioItem")) {
    return "audio";
  }
  if(strstr(class_name, "imageItem")) {
    return "image";
  }

  return "file";
}


/**
 * Write one DIDL-Lite object as an entry the shell can show.
 **/
static int
dlna_entry_json(strbuf_t* json, xmlNodePtr node, int container) {
  char class_name[128];
  char desc[544];
  char artist[256];
  char album[256];
  char date[64];
  char title[512];
  char image[1024];
  char uri[1024];
  char id[512];

  if(xml_attr(node, "id", id, sizeof(id)) || !id[0]) {
    return 0;
  }

  xml_child_text(node, "title", title, sizeof(title));
  if(!title[0]) {
    snprintf(title, sizeof(title), "%s", id);
  }

  xml_child_text(node, "class", class_name, sizeof(class_name));

  if(sb_str(json, "  {\"id\":") || sb_json(json, id) ||
     sb_str(json, ",\"type\":") ||
     sb_json(json, dlna_entry_type(class_name, container)) ||
     sb_str(json, ",\"name\":") || sb_json(json, title)) {
    return -1;
  }

  // whatever says most about the item in one line, since that is all the
  // shell has room for
  xml_child_text(node, "description", desc, sizeof(desc));
  if(!desc[0]) {
    xml_child_text(node, "artist", artist, sizeof(artist));
    xml_child_text(node, "album", album, sizeof(album));
    xml_child_text(node, "date", date, sizeof(date));

    if(artist[0] && album[0]) {
      snprintf(desc, sizeof(desc), "%s - %s", artist, album);
    } else if(artist[0]) {
      snprintf(desc, sizeof(desc), "%s", artist);
    } else if(album[0]) {
      snprintf(desc, sizeof(desc), "%s", album);
    } else if(date[0]) {
      snprintf(desc, sizeof(desc), "%s", date);
    }
  }

  if(desc[0] && (sb_str(json, ",\"description\":") || sb_json(json, desc))) {
    return -1;
  }

  xml_child_text(node, "albumArtURI", image, sizeof(image));
  if(image[0] && (sb_str(json, ",\"image\":") || sb_json(json, image))) {
    return -1;
  }

  if(!container) {
    dlna_pick_res(node, uri, sizeof(uri));
    if(uri[0] && (sb_str(json, ",\"uri\":") || sb_json(json, uri))) {
      return -1;
    }
  }

  return sb_str(json, "}");
}


/**
 * Turn the DIDL-Lite of a Browse response into a list of entries.
 **/
static int
dlna_didl_json(strbuf_t* json, const char* didl, size_t len, int* first,
               int* count) {
  xmlNodePtr child;
  xmlNodePtr root;
  xmlDocPtr doc;
  int container;
  int ret = 0;

  *count = 0;

  if(!(doc=xml_parse(didl, len, "didl.xml"))) {
    return 0;
  }

  if(!(root=xmlDocGetRootElement(doc))) {
    xmlFreeDoc(doc);
    return 0;
  }

  for(child=root->children; child; child=child->next) {
    container = xml_is(child, "container");

    if(!container && !xml_is(child, "item")) {
      continue;
    }

    if(*first) {
      ret = sb_str(json, "\n");
    } else {
      ret = sb_str(json, ",\n");
    }
    if(ret) {
      break;
    }
    *first = 0;

    if((ret=dlna_entry_json(json, child, container))) {
      break;
    }

    (*count)++;
  }

  xmlFreeDoc(doc);

  return ret;
}


/**
 * Invoke ContentDirectory:Browse and hand back the DIDL-Lite it answered
 * with, unescaped.
 **/
static int
dlna_soap_browse(const device_seq_t* dev, const char* object, const char* flag,
                 unsigned start, unsigned count, strbuf_t* didl,
                 unsigned* total) {
  strbuf_t body = {0};
  strbuf_t resp = {0};
  xmlNodePtr result;
  xmlNodePtr root;
  char action[256];
  xmlDocPtr doc;
  xmlChar* val;
  char buf[32];
  int ret = -1;

  if(sb_str(&body,
            "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
            "<s:Envelope "
            "xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
            "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
            "<s:Body><u:Browse xmlns:u=\"") ||
     sb_xml(&body, dev->service) ||
     sb_str(&body, "\"><ObjectID>") ||
     sb_xml(&body, object) ||
     sb_str(&body, "</ObjectID><BrowseFlag>") ||
     sb_str(&body, flag) ||
     sb_str(&body, "</BrowseFlag><Filter>*</Filter><StartingIndex>") ||
     sb_fmt(&body, "%u", start) ||
     sb_str(&body, "</StartingIndex><RequestedCount>") ||
     sb_fmt(&body, "%u", count) ||
     sb_str(&body, "</RequestedCount><SortCriteria></SortCriteria>"
            "</u:Browse></s:Body></s:Envelope>")) {
    sb_free(&body);
    return -1;
  }

  snprintf(action, sizeof(action), "%s#Browse", dev->service);

  if(dlna_http(dev->control, action, body.ptr, &resp) || !resp.ptr) {
    sb_free(&body);
    sb_free(&resp);
    return -1;
  }

  sb_free(&body);

  if(!(doc=xml_parse(resp.ptr, resp.len, "soap.xml"))) {
    sb_free(&resp);
    return -1;
  }

  sb_free(&resp);

  if(!(root=xmlDocGetRootElement(doc))) {
    xmlFreeDoc(doc);
    return -1;
  }

  // the Result of a Browse is an entire DIDL-Lite document escaped into one
  // element, so what comes out of it here is markup that has to be parsed
  // in its own right
  if((result=xml_find(root, "Result")) && (val=xmlNodeGetContent(result))) {
    ret = sb_str(didl, (const char*)val);
    xmlFree(val);
  }

  if(total) {
    xml_child_text(root, "TotalMatches", buf, sizeof(buf));
    *total = (unsigned)strtoul(buf, 0, 10);
  }

  xmlFreeDoc(doc);

  return ret;
}


/**
 * Respond with a JSON body, which the buffer is handed over for.
 **/
static enum MHD_Result
dlna_respond(struct MHD_Connection* conn, unsigned status, strbuf_t* sb) {
  enum MHD_Result ret = MHD_NO;
  struct MHD_Response* resp;

  if(!sb->ptr) {
    return MHD_NO;
  }

  if(!(resp=MHD_create_response_from_buffer(sb->len, sb->ptr,
                                            MHD_RESPMEM_MUST_FREE))) {
    sb_free(sb);
    return MHD_NO;
  }

  // ownership of the buffer now sits with MHD
  sb->ptr = 0;
  sb->len = 0;
  sb->cap = 0;

  MHD_add_response_header(resp, MHD_HTTP_HEADER_CONTENT_TYPE,
                          "application/json");
  MHD_add_response_header(resp, MHD_HTTP_HEADER_ACCESS_CONTROL_ALLOW_ORIGIN,
                          "*");

  ret = MHD_queue_response(conn, status, resp);
  MHD_destroy_response(resp);

  return ret;
}


static enum MHD_Result
dlna_error(struct MHD_Connection* conn, unsigned status, const char* msg) {
  strbuf_t sb = {0};

  if(sb_str(&sb, "{\"error\":") || sb_json(&sb, msg) || sb_str(&sb, "}\n")) {
    sb_free(&sb);
    return MHD_NO;
  }

  return dlna_respond(conn, status, &sb);
}


/**
 * List the media servers found on the network.
 **/
static enum MHD_Result
dlna_servers(struct MHD_Connection* conn) {
  dlna_target_t* targets;
  device_seq_t dev;
  strbuf_t sb = {0};
  bool first = true;
  size_t count;
  size_t i;

  dlna_refresh();

  count = dlna_targets(&targets);

  if(sb_str(&sb, "[")) {
    free(targets);
    sb_free(&sb);
    return MHD_NO;
  }

  for(i=0; i<count; i++) {
    if(dlna_device_by_location(targets[i].location, &dev)) {
      continue;
    }

    if(!first && sb_str(&sb, ",")) {
      break;
    }
    first = false;

    if(sb_str(&sb, "\n  {\"udn\":") || sb_json(&sb, dev.udn) ||
       sb_str(&sb, ",\"name\":") || sb_json(&sb, dev.name) ||
       sb_str(&sb, ",\"manufacturer\":") || sb_json(&sb, dev.manufacturer) ||
       sb_str(&sb, ",\"model\":") || sb_json(&sb, dev.model) ||
       sb_str(&sb, ",\"icon\":") || sb_json(&sb, dev.icon) ||
       sb_str(&sb, ",\"location\":") || sb_json(&sb, dev.location) ||
       sb_str(&sb, ",\"address\":") || sb_json(&sb, dev.addr) ||
       sb_fmt(&sb, ",\"port\": %d}", dev.port)) {
      break;
    }
  }

  free(targets);

  if(sb_str(&sb, "\n]\n")) {
    sb_free(&sb);
    return MHD_NO;
  }

  return dlna_respond(conn, MHD_HTTP_OK, &sb);
}


/**
 * List the children of an object, or the object itself.
 **/
static enum MHD_Result
dlna_browse(struct MHD_Connection* conn, int metadata) {
  const char* object;
  const char* udn;
  strbuf_t json = {0};
  strbuf_t didl = {0};
  device_seq_t dev;
  unsigned total = 0;
  unsigned start = 0;
  int first = 1;
  int count = 0;
  int got = 0;
  int page;

  udn = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "udn");
  object = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "id");

  if(!udn || !udn[0]) {
    return dlna_error(conn, MHD_HTTP_BAD_REQUEST, "missing udn parameter");
  }

  // the root of a ContentDirectory is always object 0
  if(!object || !object[0]) {
    object = "0";
  }

  if(dlna_device_by_udn(udn, &dev)) {
    dlna_refresh();
    if(dlna_device_by_udn(udn, &dev)) {
      return dlna_error(conn, MHD_HTTP_NOT_FOUND, "no such media server");
    }
  }

  if(sb_str(&json, "[")) {
    sb_free(&json);
    return MHD_NO;
  }

  // a server with a large library will not answer a request for all of it
  // at once, so ask for a page at a time until it stops adding anything
  for(page=0; page<DLNA_MAX_PAGES; page++) {
    didl.len = 0;

    if(dlna_soap_browse(&dev, object, metadata ? "BrowseMetadata"
                        : "BrowseDirectChildren", start, DLNA_PAGE_SIZE,
                        &didl, &total)) {
      sb_free(&json);
      sb_free(&didl);
      return dlna_error(conn, MHD_HTTP_BAD_GATEWAY,
                        "the media server did not answer");
    }

    if(dlna_didl_json(&json, didl.ptr, didl.len, &first, &count)) {
      sb_free(&json);
      sb_free(&didl);
      return MHD_NO;
    }

    got += count;
    start += (unsigned)count;

    if(metadata || count <= 0 || got >= DLNA_MAX_ITEMS) {
      break;
    }
    if(total) {
      if((unsigned)got >= total) {
        break;
      }
    } else if(count < DLNA_PAGE_SIZE) {
      // nothing says there is more, and the page was not even full
      break;
    }
  }

  sb_free(&didl);

  if(sb_str(&json, "\n]\n")) {
    sb_free(&json);
    return MHD_NO;
  }

  return dlna_respond(conn, MHD_HTTP_OK, &json);
}


enum MHD_Result
dlna_request(struct MHD_Connection *conn, const char* url) {
  if(!strcmp("/dlna", url) || !strcmp("/dlna/", url)) {
    return dlna_servers(conn);
  }

  if(!strcmp("/dlna/browse", url)) {
    return dlna_browse(conn, 0);
  }

  if(!strcmp("/dlna/metadata", url)) {
    return dlna_browse(conn, 1);
  }

  return dlna_error(conn, MHD_HTTP_NOT_FOUND, "no such endpoint");
}
