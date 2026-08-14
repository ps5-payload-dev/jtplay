// SPDX-License-Identifier: GPL-3.0-or-later
//
// Browser port of jtplay's application shell. Two browse views (sources and
// entries) plus a watch mode where the <video> element shows through, with
// the same auto-hiding transport bar / persistent audio card as the RmlUi
// original. Everything is driven by the PS5 controller:
//
//   X (Enter)      select / play-pause
//   O (Escape)     back / stop
//   d-pad          navigate / seek
//   triangle (F1)  reload plugins and rediscover providers (sources view)
//   square (F2)    sign out of the selected source (sources view)
//   options (F3)   toggle the info bar in watch mode

(function() {
  "use strict";

  var KEY = {
    CROSS: 13,     // X
    CIRCLE: 27,    // O
    LEFT: 37, UP: 38, RIGHT: 39, DOWN: 40,
    TRIANGLE: 112, // F1
    SQUARE: 113,   // F2
    OPTIONS: 114   // F3
  };

  var SEEK_SMALL = 15;   // left/right, seconds
  var SEEK_BIG = 60;     // up/down, seconds
  var INFO_HIDE_MS = 4000;

  var $ = function(id) { return document.getElementById(id); };
  var media = $("media");
  var Auth = window.Auth;

  // ------------------------------------------------------------------
  // State
  // ------------------------------------------------------------------

  var state = {
    view: "sources",     // 'sources' | 'browse' | 'watch'
    providers: [],       // discovered by the plugins, see plugins.js
    loading: false,      // a discovery pass is in flight
    selSource: 0,
    // Browse history: [{provider, id, crumb, entries, sel, scroll}].
    stack: [],
    busy: 0,
    watch: null,         // {entry, audio, live}
    playSeq: 0,          // bumped on every load, so stale errors are ignored
    infoTimer: 0,
    toastTimer: 0
  };

  function top() {
    return state.stack[state.stack.length - 1];
  }

  // ------------------------------------------------------------------
  // Chrome: clock, busy, toast
  // ------------------------------------------------------------------

  function tickClock() {
    var d = new Date();
    var mm = d.getMinutes();
    $("clock").textContent = d.getHours() + ":" + (mm < 10 ? "0" + mm : mm);
  }
  setInterval(tickClock, 1000);
  tickClock();

  function busy(on) {
    state.busy += on ? 1 : -1;
    $("busy").className = state.busy > 0 ? "busy on" : "busy";
  }

  function toast(msg) {
    var el = $("toast");
    el.textContent = msg;
    el.className = "on";
    clearTimeout(state.toastTimer);
    state.toastTimer = setTimeout(function() { el.className = ""; }, 4000);
  }

  // ------------------------------------------------------------------
  // Rendering
  // ------------------------------------------------------------------

  function showView(name) {
    state.view = name;
    $("view-sources").className = "content" + (name === "sources" ? " active" : "");
    $("view-browse").className = "content" + (name === "browse" ? " active" : "");
    // Refreshing is only offered where the provider list is on screen.
    $("hint-refresh").className = "chip" + (name === "sources" ? "" : " hidden");
    updateSourceHints();
  }

  function icon(entry) {
      if (entry.type === "folder") return "📁";
      if (entry.type === "audio")  return "🎵";
      if (entry.type === "video")  return "🎬";
      else return "📄"
  }

  function renderSources() {
    var list = $("source-list");
    list.innerHTML = "";

    if (!state.providers.length) {
      var empty = document.createElement("div");
      empty.className = "empty";
      empty.textContent = state.loading
        ? "Looking for media providers\u2026"
        : "No media providers. Press \u25B3 to try again.";
      list.appendChild(empty);
    }

    state.providers.forEach(function(p, i) {
      var row = document.createElement("div");
      row.className = "source-row" + (i === state.selSource ? " selected" : "");
      row.innerHTML =
        '<div class="srcicon"></div>' +
        '<div class="srcinfo"><div class="srcname"></div>' +
        '<div class="srcdetail"></div></div>' +
        '<div class="srclock"></div>';
      row.children[0].textContent = p.icon;
      row.children[1].children[0].textContent = p.name;
      row.children[1].children[1].textContent = p.detail;

      // Which parts of a source need a login is the plugin's business and is
      // only discovered by opening them, so there is nothing to say here
      // until we are actually holding credentials for one.
      var lock = row.children[2];
      var accounts = Auth.accounts(p);
      if (!accounts.length) {
        lock.className = "srclock hidden";
      } else {
        lock.className = "srclock";
        lock.textContent = "\uD83D\uDC64 " + (accounts.length === 1 // 👤
          ? accounts[0].user
          : accounts.length + " logins");
      }

      list.appendChild(row);
    });
    $("source-count").textContent = state.providers.length || "";
    updateSourceHints();
  }

  // Signing out is only offered on a source that actually has a login, so
  // the hint comes and goes with the selection.
  function updateSourceHints() {
    var p = state.providers[state.selSource];
    var signedIn = !!(p && Auth.accounts(p).length);
    $("hint-signout").className = "chip" +
      (state.view === "sources" && signedIn ? "" : " hidden");
  }

  // Drops every login the selected source holds, which is the only thing a
  // single button can sensibly mean when one source can hold several.
  function signOutSource() {
    var p = state.providers[state.selSource];
    if (!p) {
      return;
    }
    var count = Auth.accounts(p).length;
    if (!count) {
      return;
    }
    Auth.signOut(p);
    renderSources();
    toast(count === 1
      ? "Signed out of " + p.name
      : "Signed out of " + count + " logins on " + p.name);
  }

  function renderEntries() {
    var page = top();
    var list = $("entry-list");
    list.innerHTML = "";

    if (!page.entries.length) {
      var empty = document.createElement("div");
      empty.className = "empty";
      empty.textContent = "This folder is empty.";
      list.appendChild(empty);
    }

    page.entries.forEach(function(e, i) {
      var row = document.createElement("div");
      row.className = "entry-row" +
        (e.type === "folder" ? " folder" : "") +
        (i === page.sel ? " selected" : "");
      row.innerHTML = '<div class="eicon"></div><div class="etitle"></div>';
      row.children[0].textContent = icon(e);
      row.children[1].textContent = e.name;
      list.appendChild(row);
    });

    $("browse-title").textContent = page.provider.name;
    $("entry-count").textContent = page.entries.length || "";
    $("crumb").textContent = page.crumb;
    updateSelection();
  }

  function updateSelection() {
    var page = top();
    var rows = $("entry-list").children;
    for (var i = 0; i < rows.length; i++) {
      if (rows[i].className.indexOf("entry-row") < 0) continue;
      rows[i].className = "entry-row" +
        (page.entries[i].type === "folder" ? " folder" : "") +
        (i === page.sel ? " selected" : "");
      if (i === page.sel && rows[i].scrollIntoView) {
        rows[i].scrollIntoView({block: "nearest"});
      }
    }
    renderDetails(page.entries[page.sel]);
  }

  function renderDetails(entry) {
    var body = $("detail-body");
    if (!entry || (!entry.description && !entry.image)) {
      body.className = "detail-body hidden";
      return;
    }
    body.className = "detail-body";
    $("detail-desc").textContent = entry.description || "";
    $("detail-art").src = entry.image || "";
  }

  function updateSourceSelection() {
    var rows = $("source-list").children;
    for (var i = 0; i < rows.length; i++) {
      rows[i].className = "source-row" + (i === state.selSource ? " selected" : "");
      if (i === state.selSource && rows[i].scrollIntoView) {
        rows[i].scrollIntoView({block: "nearest"});
      }
    }
    updateSourceHints();
  }

  // ------------------------------------------------------------------
  // Plugins
  // ------------------------------------------------------------------

  // Reload every plugin from the manifest and re-run their discovery
  // functions. The cursor stays on the same provider when it comes back.
  function refreshProviders(initial) {
    if (state.loading) {
      return;
    }

    var prev = state.providers[state.selSource];
    var prevKey = prev ? prev.key : null;

    state.loading = true;
    busy(true);
    renderSources();

    window.Plugins.reload().then(function(result) {
      state.loading = false;
      busy(false);

      state.providers = result.providers;
      state.selSource = 0;
      for (var i = 0; i < state.providers.length; i++) {
        if (state.providers[i].key === prevKey) {
          state.selSource = i;
          break;
        }
      }
      renderSources();

      if (result.errors.length) {
        var first = result.errors[0];
        toast(first.plugin + ": " + (first.error.message || first.error) +
              (result.errors.length > 1
               ? " (+" + (result.errors.length - 1) + " more)" : ""));
      } else if (!initial) {
        toast(result.providers.length + " providers from " +
              result.plugins + " plugins");
      }
    });
  }

  // ------------------------------------------------------------------
  // Browsing
  // ------------------------------------------------------------------

  // Returned in place of a result when the viewer backs out of the sign-in
  // dialog, so the caller can go quiet instead of reporting an error.
  var CANCELLED = {cancelled: true};

  // Calls into a plugin, starting with no credentials at all.
  //
  // Plugin code is third-party, so a synchronous throw has to end up as a
  // rejection rather than escaping into the key handler. On top of that, a
  // plugin decides for itself whether what is being opened needs a login and
  // says so by throwing Auth.required({realm, ...}). We answer with
  // credentials for exactly the realm named - the saved ones if there are
  // any, otherwise whatever the dialog collects - and call again. Credentials
  // that come back refused are dropped and asked for afresh.
  //
  // Nothing here knows how a plugin authenticates, so a query parameter, a
  // header, a token endpoint and a library that never asks can all sit behind
  // the same source.
  function call(provider, fn, arg) {
    function attempt(creds) {
      return new Promise(function(resolve) { resolve(fn(arg, creds)); })
        .catch(function(err) {
          if (!Auth.isRequired(err)) {
            throw err;
          }

          // Were we already carrying credentials for this very realm? Then
          // those are the ones being turned down, and the viewer has to
          // supply better ones.
          var refused = !!(creds && creds.realm === err.realm);
          if (refused) {
            Auth.forget(err.realm);
            Auth.notifySignOut(provider, err.realm);
            renderSources();
          } else {
            var saved = Auth.stored(err.realm);
            if (saved) {
              return attempt(saved);
            }
          }

          return Auth.ask(provider, err, refused).then(function(got) {
            if (!got) {
              return CANCELLED;
            }
            renderSources();
            return attempt(got);
          });
        });
    }
    return attempt(null);
  }

  function openFolder(provider, id, crumb) {
    busy(true);
    call(provider, provider.browse, id)
      .then(function(entries) {
        if (entries === CANCELLED) {
          return;
        }
        state.stack.push({provider: provider, id: id, crumb: crumb,
                          entries: entries || [], sel: 0});
        showView("browse");
        renderEntries();
      })
      .catch(function(err) {
        console.error(err);
        toast("" + (err.message || err));
      })
      .then(function() { busy(false); });
  }

  function goBack() {
    state.stack.pop();
    if (!state.stack.length) {
      showView("sources");
      $("crumb").textContent = "";
    } else {
      renderEntries();
    }
  }

  // ------------------------------------------------------------------
  // Playback
  // ------------------------------------------------------------------

  function playUrl(url) {
    var seq = ++state.playSeq;

    media.pause();
    media.src = url;
    media.load();

    var p = media.play();
    if (p && p.catch) {
      p.catch(function(err) {
        // A load that got superseded rejects with AbortError; ignore those.
        if (seq !== state.playSeq || !state.watch) {
          return;
        }
        if (err && err.name === "AbortError") {
          return;
        }
        console.error(err);
        toast("Could not start playback");
        stopWatch();
      });
    }
  }

  function mediaErrorText() {
    var err = media.error;
    if (!err) {
      return "Playback error";
    }
    switch (err.code) {
    case 1: return "Playback aborted";
    case 2: return "Network error";
    case 3: return "Cannot decode this stream";
    case 4: return "Unsupported format or source";
    }
    return "Playback error";
  }

  function startWatch(entry) {
    var play = function(url) {
      state.watch = {entry: entry, audio: entry.type === "audio"};
      document.body.className = "watching" +
        (state.watch.audio ? " audio" : " info");
      state.view = "watch";

      $("watch-name").textContent = entry.name;
      $("watch-thumb").src = entry.image || "";
      $("np-title").textContent = entry.name;
      $("np-meta").textContent = entry.description || "";
      $("np-art").src = entry.image || "";

      playUrl(url);
      if (!state.watch.audio) {
        pokeInfo();
      }
    };

    if (entry.uri) {
      play(entry.uri);
      return;
    }

    var provider = top().provider;
    busy(true);
    call(provider, provider.resolve, entry.id)
      .then(function(url) {
        if (url !== CANCELLED) {
          play(url);
        }
      })
      .catch(function(err) {
        console.error(err);
        toast("" + (err.message || err));
      })
      .then(function() { busy(false); });
  }

  function stopWatch() {
    state.playSeq++;
    media.pause();
    media.removeAttribute("src");
    media.load();
    state.watch = null;
    document.body.className = "";
    state.view = state.stack.length ? "browse" : "sources";
  }

  // The transport bar shows on any key press and hides again after a few
  // seconds of inactivity, like jtplay's info bar.
  function pokeInfo() {
    if (!state.watch || state.watch.audio) {
      return;
    }
    document.body.className = "watching info" +
      (media.paused ? " paused" : "");
    clearTimeout(state.infoTimer);
    state.infoTimer = setTimeout(function() {
      if (state.watch && !media.paused) {
        document.body.className = "watching";
      }
    }, INFO_HIDE_MS);
  }

  function toggleInfo() {
    if (document.body.className.indexOf("info") >= 0) {
      clearTimeout(state.infoTimer);
      document.body.className = "watching";
    } else {
      pokeInfo();
    }
  }

  function fmtTime(s) {
    if (!isFinite(s)) {
      return "";
    }
    s = Math.max(0, Math.floor(s));
    var h = Math.floor(s / 3600);
    var m = Math.floor((s % 3600) / 60);
    var sec = s % 60;
    var pad = function(n) { return n < 10 ? "0" + n : "" + n; };
    return (h ? h + ":" + pad(m) : "" + m) + ":" + pad(sec);
  }

  function seekable() {
    return state.watch && isFinite(media.duration) && media.duration > 0;
  }

  function updateProgress() {
    if (!state.watch) {
      return;
    }
    var can = seekable();
    $("watch-progress").className = "watch-progress" + (can ? "" : " hidden");
    $("hint-seek").className = "chip" + (can ? "" : " hidden");
    if (can) {
      $("watch-progress-bar").style.width =
        (100 * media.currentTime / media.duration) + "%";
      $("watch-time").textContent =
        fmtTime(media.currentTime) + " / " + fmtTime(media.duration);
    } else {
      $("watch-time").textContent = "LIVE";
    }
    if (state.watch.audio) {
      document.body.className = "watching audio" +
        (media.paused ? " paused" : "");
    }
  }
  setInterval(updateProgress, 500);

  media.addEventListener("ended", function() {
    if (state.watch) {
      stopWatch();
    }
  });
  media.addEventListener("error", function() {
    // Tearing the element down at stop time can fire one last error; only
    // react while something is actually loaded.
    if (state.watch && media.error && media.getAttribute("src")) {
      toast(mediaErrorText());
      stopWatch();
    }
  });

  function seek(delta) {
    if (seekable()) {
      media.currentTime = Math.max(
        0, Math.min(media.duration - 1, media.currentTime + delta));
      pokeInfo();
    }
  }

  // ------------------------------------------------------------------
  // Input
  // ------------------------------------------------------------------

  function onKeySources(code) {
    if (code === KEY.TRIANGLE) {
      refreshProviders(false);
      return;
    }

    var n = state.providers.length;
    if (!n) {
      return;
    }
    switch (code) {
    case KEY.UP:
      state.selSource = (state.selSource + n - 1) % n;
      updateSourceSelection();
      break;
    case KEY.DOWN:
      state.selSource = (state.selSource + 1) % n;
      updateSourceSelection();
      break;
    case KEY.CROSS:
      var p = state.providers[state.selSource];
      openFolder(p, "", p.name);
      break;
    case KEY.SQUARE:
      signOutSource();
      break;
    }
  }

  function onKeyBrowse(code) {
    var page = top();
    var n = page.entries.length;
    switch (code) {
    case KEY.UP:
      if (n) { page.sel = (page.sel + n - 1) % n; updateSelection(); }
      break;
    case KEY.DOWN:
      if (n) { page.sel = (page.sel + 1) % n; updateSelection(); }
      break;
    case KEY.LEFT:
      if (n) { page.sel = Math.max(0, page.sel - 10); updateSelection(); }
      break;
    case KEY.RIGHT:
      if (n) { page.sel = Math.min(n - 1, page.sel + 10); updateSelection(); }
      break;
    case KEY.CROSS:
      var e = page.entries[page.sel];
      if (!e) {
        break;
      }
      if (e.type === "folder") {
        openFolder(page.provider, e.id, page.crumb + " / " + e.name);
      } else {
        startWatch(e);
      }
      break;
    case KEY.CIRCLE:
      goBack();
      break;
    }
  }

  function onKeyWatch(code) {
    switch (code) {
    case KEY.CROSS:
      if (media.paused) { media.play(); } else { media.pause(); }
      pokeInfo();
      break;
    case KEY.CIRCLE:
      stopWatch();
      break;
    case KEY.LEFT:  seek(-SEEK_SMALL); break;
    case KEY.RIGHT: seek(+SEEK_SMALL); break;
    case KEY.DOWN:  seek(-SEEK_BIG);   break;
    case KEY.UP:    seek(+SEEK_BIG);   break;
    case KEY.OPTIONS:
      toggleInfo();
      break;
    default:
      pokeInfo();
    }
  }

  document.addEventListener("keydown", function(e) {
    var code = e.keyCode;
    if (code === KEY.CROSS || code === KEY.CIRCLE ||
        (code >= KEY.LEFT && code <= KEY.DOWN) ||
        (code >= KEY.TRIANGLE && code <= KEY.OPTIONS)) {
      e.preventDefault();
    }
    switch (state.view) {
    case "sources": onKeySources(code); break;
    case "browse":  onKeyBrowse(code);  break;
    case "watch":   onKeyWatch(code);   break;
    }
  });

  // ------------------------------------------------------------------
  // Boot
  // ------------------------------------------------------------------

  showView("sources");
  refreshProviders(true);
})();
