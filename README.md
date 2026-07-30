# JTPlay

A audio/video player for jailbroken PS5s where the majority of the code
was produced by claude.ai

## JavaScript plugins

Besides UPnP/DLNA servers and the local filesystem, media sources can be
added with JavaScript. Every `*.js` file in the plugin directory
(`--plugins DIR`, default `<assets>/../plugins`) is evaluated by QuickJS and
becomes one provider; the script returns an object implementing the same
interface as `browse::Provider` and `browse::Source`:

```js
return {
  name: "My provider",
  discover() {
    return [
      {
        name: "My source",         // row title
        detail: "somewhere",       // row subtitle (optional)
        icon: "📻",                // emoji glyph (optional)
        root: "/",                 // root folder id (optional)
        browse(id) {               // list the children of 'id'
          return [
            {
              id: "bbc1",                        // optional; defaults to uri
              type: "audio",                     // folder|audio|video|image
              name: "A stream",                  // required
              description: "Some station",       // optional
              image: "https://example.com/a.jpg",// optional
              uri: "https://example.com/stream",
            },
          ];
        },
      },
    ];
  },
};
```

The script body runs inside a function (like a CommonJS module), so it ends
with `return` rather than an `export`, and top-level `const`/`let` stay
private to the plugin. In `browse()`, `this` is the source object, so a
plugin can keep per-source state on it.

Each script gets its own QuickJS runtime with the standard objects (`JSON`,
`Math`, `Date`, `RegExp`, ...), `console.log()`/`warn()`/`error()` writing to
the RmlUi log, and the `http` object below. There is no event loop:
`discover()` and `browse()` must return their result synchronously (an
`async` function returns a promise and is reported as an error), and a call
that spends more than 10 seconds *executing* is interrupted.

An entry has six fields and no more: `id`, `type`, `name`, `description`,
`image` and `uri` (see `browse::Entry` in `src/browse/source.h`). Only `name`
is required; a `folder` entry is browsed by handing its `id` back to
`browse()`, anything else is opened by its `uri`. A source that serves signed
or otherwise short-lived URIs can leave `uri` out and add a
`resolve(id, entry)` function returning a fresh one (as a string, or as an
entry object with a `uri`) at the moment playback starts.

See `plugins/sr.js` (Sveriges Radio live channels) for a working example.

### Fetching over the network

Plugins talk to the network through `http`, backed by libcurl, so https,
redirects and compressed responses work. The calls block (plugins already
run on the browsing worker thread) and return the response directly:

```js
const res = http.get("https://api.example.com/things", {
  headers: { Accept: "application/json" },
  timeout: 20,        // seconds; default 15, capped at 60
});
if (!res.ok) throw new Error("HTTP " + res.status);
const things = JSON.parse(res.body);
```

```js
http.post(url, body, options);   // options as above
http.request({ method, url, headers, body, timeout, redirect, binary });
```

A response is `{ ok, status, url, headers, body }`, where `url` is the final
URL after redirects and `headers` keys are lower-cased. Following redirects
can be turned off with `redirect: false`, and `binary: true` returns the body
as an `ArrayBuffer` instead of a string (a string body is decoded as UTF-8).

An HTTP error status is *not* an exception: check `res.ok`. Transport
failures (DNS, TLS, timeout) throw an `Error`, so wrap calls in `try`/`catch`
if a source should survive an unreachable server. Only `http` and `https`
URLs are permitted, and a response over 8 MiB is refused. Time spent waiting
on a request does not count towards the 10-second watchdog.

If the build's libcurl has no default CA bundle, point it at one with the
`SSL_CERT_FILE` (or `CURL_CA_BUNDLE`) environment variable, otherwise https
requests fail with a certificate error.

## Building

Besides SDL2, RmlUi, FFmpeg and tinyxml2, the build needs
[QuickJS](https://bellard.org/quickjs/) and libcurl.
