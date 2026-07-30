// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <chrono>
#include <mutex>

#include "browse/js_http.h"
#include "browse/js_source.h"

namespace browse {
  namespace {

    // A plugin listing is metadata, not media; anything this large is a
    // mistake, and the body is held outside the JS heap so the runtime's
    // own memory limit would not catch it.
    constexpr size_t kMaxResponseBytes = 8u << 20;
    constexpr long kConnectTimeoutMs = 10000;
    constexpr long kMaxTimeoutMs = 60000;

    struct Sink {
      std::string data;
      bool overflow = false;
    };

    size_t WriteBody(char* ptr, size_t size, size_t nmemb, void* userdata) {
      Sink* sink = static_cast<Sink*>(userdata);
      const size_t n = size * nmemb;
      if (sink->data.size() + n > kMaxResponseBytes) {
	sink->overflow = true;
	return 0; // aborts the transfer with CURLE_WRITE_ERROR
      }
      sink->data.append(ptr, n);
      return n;
    }

    size_t WriteHeader(char* ptr, size_t size, size_t nmemb, void* userdata) {
      auto* headers =
	static_cast<std::map<std::string, std::string>*>(userdata);
      const size_t n = size * nmemb;
      std::string line(ptr, n);
      while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
	line.pop_back();

      // A new status line means a redirect; the headers seen so far belong
      // to the response we are not returning.
      if (line.compare(0, 5, "HTTP/") == 0) {
	headers->clear();
	return n;
      }
      const size_t colon = line.find(':');
      if (colon == std::string::npos)
	return n;

      std::string key = line.substr(0, colon);
      std::transform(key.begin(), key.end(), key.begin(),
		     [](unsigned char c) { return std::tolower(c); });
      size_t begin = line.find_first_not_of(" \t", colon + 1);
      std::string value =
	begin == std::string::npos ? std::string() : line.substr(begin);

      auto it = headers->find(key);
      if (it == headers->end())
	(*headers)[key] = std::move(value);
      else
	it->second += ", " + value; // repeated header (Set-Cookie, Link, ...)
      return n;
    }

    JsPlugin* PluginFor(JSContext* ctx) {
      return static_cast<JsPlugin*>(JS_GetContextOpaque(ctx));
    }

    // Throws a plain Error, the way a plugin author expects to catch it.
    JSValue ThrowError(JSContext* ctx, const std::string& message) {
      JSValue error = JS_NewError(ctx);
      JS_SetPropertyStr(ctx, error, "message",
			JS_NewStringLen(ctx, message.data(), message.size()));
      return JS_Throw(ctx, error);
    }

    bool StringArg(JSContext* ctx, JSValue v, std::string& out) {
      size_t len = 0;
      const char* s = JS_ToCStringLen(ctx, &len, v);
      if (!s)
	return false;
      out.assign(s, len);
      JS_FreeCString(ctx, s);
      return true;
    }

    // Copies the {name: value} object at 'headers' into the request.
    bool ReadHeaders(JSContext* ctx, JSValue headers,
		     HttpClient::Request& req) {
      JSPropertyEnum* props = nullptr;
      uint32_t count = 0;
      if (JS_GetOwnPropertyNames(ctx, &props, &count, headers,
				 JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0)
	return false;

      bool ok = true;
      for (uint32_t i = 0; i < count && ok; i++) {
	const char* name = JS_AtomToCString(ctx, props[i].atom);
	JSValue value = JS_GetProperty(ctx, headers, props[i].atom);
	std::string text;
	if (!name || JS_IsException(value) || !StringArg(ctx, value, text))
	  ok = false;
	else
	  req.headers.emplace_back(name, text);
	JS_FreeValue(ctx, value);
	if (name)
	  JS_FreeCString(ctx, name);
      }
      //      JS_FreePropertyEnum(ctx, props, count);
      return ok;
    }

    // Reads the options object shared by request()/get()/post().
    bool ReadOptions(JSContext* ctx, JSValue options,
		     HttpClient::Request& req, bool& binary) {
      if (JS_IsUndefined(options) || JS_IsNull(options))
	return true;
      if (!JS_IsObject(options)) {
	JS_ThrowTypeError(ctx, "http: options must be an object");
	return false;
      }

      JSValue headers = JS_GetPropertyStr(ctx, options, "headers");
      bool ok = true;
      if (JS_IsObject(headers))
	ok = ReadHeaders(ctx, headers, req);
      JS_FreeValue(ctx, headers);
      if (!ok)
	return false;

      JSValue timeout = JS_GetPropertyStr(ctx, options, "timeout");
      if (JS_IsNumber(timeout)) {
	double seconds = 0.0;
	JS_ToFloat64(ctx, &seconds, timeout);
	if (seconds > 0.0)
	  req.timeout_ms = std::min((long)(seconds * 1000.0), kMaxTimeoutMs);
      }
      JS_FreeValue(ctx, timeout);

      JSValue redirect = JS_GetPropertyStr(ctx, options, "redirect");
      if (JS_IsBool(redirect))
	req.follow_redirects = JS_ToBool(ctx, redirect) != 0;
      JS_FreeValue(ctx, redirect);

      JSValue want_binary = JS_GetPropertyStr(ctx, options, "binary");
      if (JS_IsBool(want_binary))
	binary = JS_ToBool(ctx, want_binary) != 0;
      JS_FreeValue(ctx, want_binary);

      return true;
    }

    JSValue BuildResponse(JSContext* ctx, const HttpClient::Response& res,
			  bool binary) {
      JSValue out = JS_NewObject(ctx);
      JS_SetPropertyStr(ctx, out, "status", JS_NewInt32(ctx, (int)res.status));
      JS_SetPropertyStr(ctx, out, "ok",
			JS_NewBool(ctx, res.status >= 200 && res.status < 300));
      JS_SetPropertyStr(ctx, out, "url",
			JS_NewStringLen(ctx, res.url.data(), res.url.size()));

      JSValue headers = JS_NewObject(ctx);
      for (const auto& header : res.headers)
	JS_SetPropertyStr(ctx, headers, header.first.c_str(),
			  JS_NewStringLen(ctx, header.second.data(),
					  header.second.size()));
      JS_SetPropertyStr(ctx, out, "headers", headers);

      // A text body goes through JS_NewStringLen, which expects UTF-8;
      // 'binary: true' hands back an ArrayBuffer instead.
      if (binary)
	JS_SetPropertyStr(ctx, out, "body",
			  JS_NewArrayBufferCopy(
			    ctx, (const uint8_t*)res.body.data(),
			    res.body.size()));
      else
	JS_SetPropertyStr(ctx, out, "body",
			  JS_NewStringLen(ctx, res.body.data(),
					  res.body.size()));
      return out;
    }

    JSValue Perform(JSContext* ctx, HttpClient::Request& req, bool binary) {
      JsPlugin* plugin = PluginFor(ctx);
      HttpClient* client = plugin ? plugin->Http() : nullptr;
      if (!client)
	return ThrowError(ctx, "http: client unavailable");

      HttpClient::Response res;
      std::string error;

      // Time spent blocked in libcurl is not script time; without this the
      // watchdog would kill plugins for waiting on a slow server.
      const auto started = std::chrono::steady_clock::now();
      const bool ok = client->Perform(req, res, error);
      plugin->ExtendDeadline(std::chrono::steady_clock::now() - started);

      if (!ok)
	return ThrowError(ctx, error);
      return BuildResponse(ctx, res, binary);
    }

    // http.request({ method, url, headers, body, timeout, redirect, binary })
    JSValue HttpRequest(JSContext* ctx, JSValue this_val, int argc,
			JSValue* argv) {
      (void)this_val;
      if (argc < 1 || !JS_IsObject(argv[0]))
	return JS_ThrowTypeError(ctx, "http.request: expected an options "
				 "object");

      HttpClient::Request req;
      bool binary = false;

      JSValue url = JS_GetPropertyStr(ctx, argv[0], "url");
      const bool have_url = JS_IsString(url) && StringArg(ctx, url, req.url);
      JS_FreeValue(ctx, url);
      if (!have_url || req.url.empty())
	return JS_ThrowTypeError(ctx, "http.request: 'url' is required");

      JSValue method = JS_GetPropertyStr(ctx, argv[0], "method");
      if (JS_IsString(method) && !StringArg(ctx, method, req.method)) {
	JS_FreeValue(ctx, method);
	return JS_EXCEPTION;
      }
      JS_FreeValue(ctx, method);
      std::transform(req.method.begin(), req.method.end(), req.method.begin(),
		     [](unsigned char c) { return std::toupper(c); });

      JSValue body = JS_GetPropertyStr(ctx, argv[0], "body");
      if (!JS_IsUndefined(body) && !JS_IsNull(body)) {
	if (!StringArg(ctx, body, req.body)) {
	  JS_FreeValue(ctx, body);
	  return JS_EXCEPTION;
	}
	req.has_body = true;
      }
      JS_FreeValue(ctx, body);

      if (!ReadOptions(ctx, argv[0], req, binary))
	return JS_EXCEPTION;
      return Perform(ctx, req, binary);
    }

    // http.get(url, options?)
    JSValue HttpGet(JSContext* ctx, JSValue this_val, int argc,
		    JSValue* argv) {
      (void)this_val;
      HttpClient::Request req;
      bool binary = false;
      if (argc < 1 || !JS_IsString(argv[0]) || !StringArg(ctx, argv[0], req.url))
	return JS_ThrowTypeError(ctx, "http.get: expected a url string");
      if (argc > 1 && !ReadOptions(ctx, argv[1], req, binary))
	return JS_EXCEPTION;
      return Perform(ctx, req, binary);
    }

    // http.post(url, body, options?)
    JSValue HttpPost(JSContext* ctx, JSValue this_val, int argc,
		     JSValue* argv) {
      (void)this_val;
      HttpClient::Request req;
      req.method = "POST";
      bool binary = false;
      if (argc < 1 || !JS_IsString(argv[0]) || !StringArg(ctx, argv[0], req.url))
	return JS_ThrowTypeError(ctx, "http.post: expected a url string");
      if (argc > 1 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1])) {
	if (!StringArg(ctx, argv[1], req.body))
	  return JS_EXCEPTION;
	req.has_body = true;
      }
      if (argc > 2 && !ReadOptions(ctx, argv[2], req, binary))
	return JS_EXCEPTION;
      return Perform(ctx, req, binary);
    }

    std::once_flag g_curl_init;
  }

  HttpClient::HttpClient() {
    std::call_once(g_curl_init, [] { ::curl_global_init(CURL_GLOBAL_DEFAULT); });
    curl_ = ::curl_easy_init();
  }

  HttpClient::~HttpClient() {
    if (curl_)
      ::curl_easy_cleanup(curl_);
  }

  bool HttpClient::Perform(const Request& req, Response& res,
			   std::string& error) {
    if (!curl_) {
      error = "http: curl unavailable";
      return false;
    }
    ::curl_easy_reset(curl_); // keeps the connection cache, drops old options

    Sink sink;
    char message[CURL_ERROR_SIZE] = {0};

    ::curl_easy_setopt(curl_, CURLOPT_URL, req.url.c_str());
    ::curl_easy_setopt(curl_, CURLOPT_ERRORBUFFER, message);
    ::curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, WriteBody);
    ::curl_easy_setopt(curl_, CURLOPT_WRITEDATA, &sink);
    ::curl_easy_setopt(curl_, CURLOPT_HEADERFUNCTION, WriteHeader);
    ::curl_easy_setopt(curl_, CURLOPT_HEADERDATA, &res.headers);
    ::curl_easy_setopt(curl_, CURLOPT_USERAGENT, "jtplay");
    ::curl_easy_setopt(curl_, CURLOPT_ACCEPT_ENCODING, ""); // gzip if built in
    ::curl_easy_setopt(curl_, CURLOPT_FOLLOWLOCATION,
		       req.follow_redirects ? 1L : 0L);
    ::curl_easy_setopt(curl_, CURLOPT_MAXREDIRS, 5L);
    ::curl_easy_setopt(curl_, CURLOPT_TIMEOUT_MS, req.timeout_ms);
    ::curl_easy_setopt(curl_, CURLOPT_CONNECTTIMEOUT_MS, kConnectTimeoutMs);
    // The worker thread must not be hit by libcurl's signal-based timeouts.
    ::curl_easy_setopt(curl_, CURLOPT_NOSIGNAL, 1L);

    // A plugin has no business opening file:// or scp:// URLs.
#if LIBCURL_VERSION_NUM >= 0x075500
    ::curl_easy_setopt(curl_, CURLOPT_PROTOCOLS_STR, "http,https");
    ::curl_easy_setopt(curl_, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
#else
    ::curl_easy_setopt(curl_, CURLOPT_PROTOCOLS,
		       (long)(CURLPROTO_HTTP | CURLPROTO_HTTPS));
    ::curl_easy_setopt(curl_, CURLOPT_REDIR_PROTOCOLS,
		       (long)(CURLPROTO_HTTP | CURLPROTO_HTTPS));
#endif

    // Builds without a baked-in CA bundle (the PS5 toolchain among them)
    // need to be told where the certificates live.
    for (const char* name : {"SSL_CERT_FILE", "CURL_CA_BUNDLE"}) {
      if (const char* path = ::getenv(name)) {
	if (*path) {
	  ::curl_easy_setopt(curl_, CURLOPT_CAINFO, path);
	  break;
	}
      }
    }

    if (req.method == "GET") {
      ::curl_easy_setopt(curl_, CURLOPT_HTTPGET, 1L);
    } else if (req.method == "HEAD") {
      ::curl_easy_setopt(curl_, CURLOPT_NOBODY, 1L);
    } else {
      ::curl_easy_setopt(curl_, CURLOPT_CUSTOMREQUEST, req.method.c_str());
    }
    if (req.has_body) {
      ::curl_easy_setopt(curl_, CURLOPT_POSTFIELDSIZE_LARGE,
			 (curl_off_t)req.body.size());
      ::curl_easy_setopt(curl_, CURLOPT_COPYPOSTFIELDS, req.body.data());
    }

    struct curl_slist* headers = nullptr;
    for (const auto& header : req.headers)
      headers = ::curl_slist_append(headers,
				    (header.first + ": " + header.second).c_str());
    if (headers)
      ::curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, headers);

    const CURLcode code = ::curl_easy_perform(curl_);
    if (headers)
      ::curl_slist_free_all(headers);

    if (code != CURLE_OK) {
      if (sink.overflow)
	error = "http: response larger than " +
	  std::to_string(kMaxResponseBytes / (1024 * 1024)) + " MiB";
      else
	error = std::string("http: ") +
	  (message[0] ? message : ::curl_easy_strerror(code));
      return false;
    }

    ::curl_easy_getinfo(curl_, CURLINFO_RESPONSE_CODE, &res.status);
    const char* final_url = nullptr;
    ::curl_easy_getinfo(curl_, CURLINFO_EFFECTIVE_URL, &final_url);
    res.url = final_url ? final_url : req.url;
    res.body = std::move(sink.data);
    return true;
  }

  void InstallHttp(JSContext* ctx) {
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue http = JS_NewObject(ctx);

    JS_SetPropertyStr(ctx, http, "request",
		      JS_NewCFunction(ctx, HttpRequest, "request", 1));
    JS_SetPropertyStr(ctx, http, "get",
		      JS_NewCFunction(ctx, HttpGet, "get", 2));
    JS_SetPropertyStr(ctx, http, "post",
		      JS_NewCFunction(ctx, HttpPost, "post", 3));

    JS_SetPropertyStr(ctx, global, "http", http);
    JS_FreeValue(ctx, global);
  }

}
