// SPDX-License-Identifier: GPL-3.0-or-later
//
// The 'http' global available to plugin scripts, backed by libcurl (so
// https, redirects, and content encodings all work). Requests are blocking:
// plugins run on the app's worker thread, which is already where the DLNA
// source does its blocking I/O.

#pragma once

#include <map>
#include <string>
#include <utility>
#include <vector>

#include <curl/curl.h>
#include <quickjs.h>

namespace browse {

  // One libcurl easy handle, reused across requests so that a plugin keeps
  // its connections (and TLS sessions) alive between calls. Not thread
  // safe: callers hold the plugin mutex.
  class HttpClient {
  public:
    struct Request {
      std::string method = "GET";
      std::string url;
      std::vector<std::pair<std::string, std::string>> headers;
      std::string body;
      bool has_body = false;
      bool follow_redirects = true;
      long timeout_ms = 15000;
    };

    struct Response {
      long status = 0;
      std::string url;                            // after redirects
      std::map<std::string, std::string> headers; // keys lower-cased
      std::string body;
    };

    HttpClient();
    ~HttpClient();

    HttpClient(const HttpClient&) = delete;
    HttpClient& operator=(const HttpClient&) = delete;

    // Returns false only on transport errors (DNS, TLS, timeout, ...);
    // HTTP error statuses are a successful request with res.status set.
    bool Perform(const Request& req, Response& res, std::string& error);

  private:
    CURL* curl_ = nullptr;
  };

  // Installs 'http' (get/post/request) on the context's global object. The
  // client comes from the JsPlugin behind JS_GetContextOpaque().
  void InstallHttp(JSContext* ctx);

}
