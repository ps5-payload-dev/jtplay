// SPDX-License-Identifier: GPL-3.0-or-later
//
// Plugin loader.
//
// Plugins are ES modules listed in providers/manifest.json. A plugin is
// *initialized* once per load: its default export is called and returns a
// discovery function. The discovery function is async, so it may use fetch()
// to work out what the plugin can actually offer right now, and it returns a
// list of media providers. One plugin can therefore contribute several
// sources - one per server, account, channel group, library, ... - instead of
// hardcoding a single one at load time.
//
//   export default function init(ctx) {          // ctx: {id, name, url, config}
//     return async function discover() {
//       var res = await fetch("https://example.com/servers");
//       return (await res.json()).map(function(srv) {
//         return {
//           id: srv.id,                           // stable within the plugin
//           name: srv.title,
//           detail: srv.description,
//           icon: "\uD83D\uDCFA",
//           browse: async function(id) { ... },   // [] of entries
//           resolve: async function(id) { ... }   // playable url, optional
//         };
//       });
//     };
//   }
//
// Plugins.reload() re-reads the manifest and re-imports every module from
// scratch (cache busted), then re-runs discovery. That is what triangle on
// the sources screen does. Nothing is registered globally by the plugins
// themselves; everything a plugin offers comes back through discovery.

(function() {
  "use strict";

  var MANIFEST_URL = "js/providers/manifest.json";
  var DEFAULT_ICON = "\uD83D\uDCE1"; // 📡

  var pending = null;

  // Appended to every request so a refresh really re-fetches the manifest and
  // re-evaluates the modules instead of reusing the module map / http cache.
  function bust(url, ver) {
    return url + (url.indexOf("?") < 0 ? "?" : "&") + "v=" + ver;
  }

  function idFromSrc(src) {
    return src.replace(/[?#].*$/, "").replace(/^.*\//, "").replace(/\.m?js$/i, "");
  }

  function normalizeEntry(item, base, index) {
    if (typeof item === "string") {
      item = {src: item};
    }
    if (!item || typeof item !== "object" || typeof item.src !== "string") {
      throw new Error("manifest entry #" + index + " has no src");
    }
    var id = item.id || idFromSrc(item.src);
    return {
      id: id,
      name: item.name || id,
      url: new URL(item.src, base).href,
      config: item.config || {},
      enabled: item.enabled !== false
    };
  }

  async function loadManifest(ver) {
    var base = new URL(MANIFEST_URL, window.location.href);
    var res = await fetch(bust(base.href, ver), {cache: "no-store"});
    if (!res.ok) {
      throw new Error("HTTP " + res.status);
    }
    var data = await res.json();
    var list = Array.isArray(data) ? data : (data && data.plugins) || [];
    if (!Array.isArray(list)) {
      throw new Error("manifest: 'plugins' is not a list");
    }
    // A single malformed entry only costs that entry.
    var entries = [];
    var errors = [];
    list.forEach(function(item, i) {
      try {
        var entry = normalizeEntry(item, base, i);
        if (entry.enabled) {
          entries.push(entry);
        }
      } catch (err) {
        console.error(err);
        errors.push({plugin: "manifest", error: err});
      }
    });
    return {entries: entries, errors: errors};
  }

  // Providers come from third-party code, so check the shape here rather than
  // letting the UI trip over a missing field later on.
  function normalizeProvider(p, entry, index) {
    var where = entry.id + " provider #" + index;
    if (!p || typeof p !== "object") {
      throw new Error(where + " is not an object");
    }
    if (!p.name) {
      throw new Error(where + " has no name");
    }
    if (typeof p.browse !== "function") {
      throw new Error(where + " has no browse()");
    }
    var id = p.id == null ? String(index) : String(p.id);
    var name = String(p.name);
    return {
      key: entry.id + "/" + id,   // survives a refresh, used to keep the cursor
      plugin: entry.id,
      id: id,
      name: name,
      detail: p.detail == null ? "" : String(p.detail),
      icon: p.icon || DEFAULT_ICON,
      browse: p.browse.bind(p),
      resolve: typeof p.resolve === "function"
        ? p.resolve.bind(p)
        : function(eid) {
            return Promise.reject(
              new Error(name + " cannot resolve " + eid));
          },
      // Optional: told which realm went when the shell drops credentials, so
      // a provider caching something derived from them can let it go.
      signOut: typeof p.signOut === "function" ? p.signOut.bind(p) : null
    };
  }

  async function loadPlugin(entry, ver) {
    var mod = await import(bust(entry.url, ver));
    var init = mod["default"] || mod.init;
    if (typeof init !== "function") {
      throw new Error("no default exported init()");
    }

    var discover = await init({
      id: entry.id,
      name: entry.name,
      url: entry.url,
      config: entry.config
    });
    if (typeof discover !== "function") {
      throw new Error("init() did not return a discovery function");
    }

    var found = await discover();
    if (!found) {
      found = [];
    }
    if (!Array.isArray(found)) {
      found = [found];   // a plugin with exactly one provider may return it bare
    }
    return found.map(function(p, i) { return normalizeProvider(p, entry, i); });
  }

  // Full rediscovery: manifest, modules, providers. Never rejects; whatever
  // failed comes back in .errors so one broken plugin cannot take out the
  // rest. Concurrent calls share the one run in flight.
  function reload() {
    if (pending) {
      return pending;
    }

    var ver = Date.now();
    pending = (async function() {
      var entries = [];
      var providers = [];
      var errors = [];

      try {
        var manifest = await loadManifest(ver);
        entries = manifest.entries;
        errors = manifest.errors;
      } catch (err) {
        console.error(err);
        errors.push({plugin: "manifest", error: err});
      }

      var results = await Promise.all(entries.map(function(entry) {
        return loadPlugin(entry, ver).then(
          function(list) { return {entry: entry, providers: list}; },
          function(err) { return {entry: entry, error: err}; });
      }));

      var loaded = 0;
      results.forEach(function(r) {
        if (r.error) {
          console.error("plugin " + r.entry.id + ": ", r.error);
          errors.push({plugin: r.entry.name, error: r.error});
        } else {
          providers = providers.concat(r.providers);
          loaded++;
        }
      });

      var result = {
        providers: providers,
        errors: errors,
        plugins: loaded
      };
      Plugins.providers = providers;
      Plugins.errors = errors;
      return result;
    })();

    var done = function() { pending = null; };
    pending.then(done, done);
    return pending;
  }

  var Plugins = window.Plugins = {
    reload: reload,
    providers: [],
    errors: []
  };
})();
