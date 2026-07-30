// SPDX-License-Identifier: GPL-3.0-or-later
//
// Sveriges Radio live channels, read from the public API. Doubles as the
// worked example for http.get(): one request, cached on the source object,
// with the channels grouped into folders by channel type.

const API = "https://api.sr.se/api/v2/channels" +
  "?format=json&size=100&pagination=false&audioquality=hi";

function fetchChannels() {
  const res = http.get(API, { headers: { Accept: "application/json" } });
  if (!res.ok)
    throw new Error("Sveriges Radio API returned HTTP " + res.status);

  const channels = JSON.parse(res.body).channels || [];
  return channels
    .filter((c) => c.liveaudio && c.liveaudio.url)
    .map((c) => ({
      name: c.name || "?",
      url: c.liveaudio.url,
      image: c.image,
      tagline: c.tagline,
      type: c.channeltype || "Kanaler",
    }));
}

function toEntry(channel) {
  return {
    title: channel.name,
    kind: "audio",
    id: channel.url,
    url: channel.url,
    artist: "Sveriges Radio",
    album: channel.tagline,
    genre: "Radio",
    art: channel.image,
    format: channel.url.indexOf(".mp3") >= 0 ? "audio/mpeg" : "audio/aac",
  };
}

return {
  name: "Sveriges Radio",

  discover() {
    // No request here: discovery runs on every rescan, and a source that
    // exists is more useful than one that vanishes when the API is down.
    return [
      {
        name: "Sveriges Radio",
        detail: "Live radio from Swedish public service",
        icon: "📻",

        // One fetch per source, kept on the source object.
        channels() {
          if (!this.cache)
            this.cache = fetchChannels();
          return this.cache;
        },

        browse(id) {
          const channels = this.channels();

          if (id !== "/") {
            const type = id.slice("type:".length);
            return channels.filter((c) => c.type === type).map(toEntry);
          }

          const types = [];
          for (const channel of channels)
            if (types.indexOf(channel.type) < 0)
              types.push(channel.type);

          return types.map((type) => ({
            title: type,
            kind: "folder",
            id: "type:" + type,
            children: channels.filter((c) => c.type === type).length,
          }));
        },
      },
    ];
  },
};
