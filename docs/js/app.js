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

  // ------------------------------------------------------------------
  // State
  // ------------------------------------------------------------------

  var state = {
    view: "sources",     // 'sources' | 'browse' | 'watch'
    selSource: 0,
    // Browse history: [{provider, id, crumb, entries, sel, scroll}].
    stack: [],
    busy: 0,
    watch: null,         // {entry, audio, live}
    hls: null,
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
  }

  function icon(entry) {
    if (entry.type === "folder") return "\uD83D\uDCC1"; // 📁
    if (entry.type === "audio")  return "\uD83C\uDFB5"; // 🎵
    return "\uD83C\uDFAC";                              // 🎬
  }

  function renderSources() {
    var list = $("source-list");
    list.innerHTML = "";
    window.Providers.forEach(function(p, i) {
      var row = document.createElement("div");
      row.className = "source-row" + (i === state.selSource ? " selected" : "");
      row.innerHTML =
        '<div class="srcicon"></div>' +
        '<div class="srcinfo"><div class="srcname"></div>' +
        '<div class="srcdetail"></div></div>';
      row.children[0].textContent = p.icon;
      row.children[1].children[0].textContent = p.name;
      row.children[1].children[1].textContent = p.detail;
      list.appendChild(row);
    });
    $("source-count").textContent = window.Providers.length;
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
  }

  // ------------------------------------------------------------------
  // Browsing
  // ------------------------------------------------------------------

  function openFolder(provider, id, crumb) {
    busy(true);
    Promise.resolve(provider.browse(id))
      .then(function(entries) {
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
    if (state.hls) {
      state.hls.destroy();
      state.hls = null;
    }

    var isHls = url.indexOf(".m3u8") > 0;
    var native = media.canPlayType("application/vnd.apple.mpegurl");

    if (isHls && !native && window.Hls && window.Hls.isSupported()) {
      state.hls = new Hls();
      state.hls.on(Hls.Events.ERROR, function(ev, data) {
        if (data.fatal) {
          toast("Playback error: " + data.details);
          stopWatch();
        }
      });
      state.hls.loadSource(url);
      state.hls.attachMedia(media);
    } else {
      media.src = url;
    }
    media.play();
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
    Promise.resolve(provider.resolve(entry.id))
      .then(play)
      .catch(function(err) {
        console.error(err);
        toast("" + (err.message || err));
      })
      .then(function() { busy(false); });
  }

  function stopWatch() {
    if (state.hls) {
      state.hls.destroy();
      state.hls = null;
    }
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
    if (state.watch && media.error) {
      toast("Playback error");
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
    var n = window.Providers.length;
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
      var p = window.Providers[state.selSource];
      openFolder(p, "", p.name);
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

  renderSources();
  showView("sources");
})();
