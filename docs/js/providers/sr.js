// SPDX-License-Identifier: GPL-3.0-or-later
//
// Sveriges Radio: live channels from the open SR API. Browser port of
// jtplay's plugins/sr.js; http.get became fetch(), so browse() is async.

(function() {
  var API_URL = "https://api.sr.se/api/v2";

  async function fetchChannels() {
    var res = await fetch(API_URL +
      "/channels?format=json&pagination=false&audioquality=hi");
    if (!res.ok) {
      throw new Error("SR API: HTTP " + res.status);
    }
    var channels = (await res.json()).channels || [];

    return channels.map(function(ch) {
      return {
        id: String(ch.id),
        type: "audio",
        name: ch.name,
        description: ch.tagline,
        image: ch.image,
        uri: ch.liveaudio.url
      };
    });
  }

  window.Providers.push({
    name: "Sveriges Radio",
    detail: "Live radio from Swedish public service",
    icon: "\uD83D\uDCFB", // 📻

    browse: function(id) {
      return fetchChannels();
    }
  });
})();
