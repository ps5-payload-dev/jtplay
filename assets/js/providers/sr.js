// SPDX-License-Identifier: GPL-3.0-or-later

export default function init(ctx) {
    const API_URL = "https://api.sr.se/api/v2";
    const VIZ_URL = "/viz/master.m3u8";

    return async function discover() {
	return [{
	    id: "https://sr.se",
	    name: "Sveriges Radio",
	    detail: "Live radio from Swedish public service",
	    icon: "📻",
	    browse: async function(id) {
		const res = await fetch(API_URL + "/channels" +
					"?format=json&pagination=false" +
					"&audioquality=hi" +
					"&liveaudiotemplateid=5");
		if (!res.ok) {
		    throw new Error(res.status);
		}

		const channels = (await res.json()).channels || [];
		return channels.map((ch) => ({
		    id: ch.liveaudio.url,
		    type: "audio",
		    name: ch.name,
		    description: ch.tagline,
		    image: ch.image
		}));
	    },
	    resolve: async function(id) {
		return VIZ_URL + "?uri=" + encodeURIComponent(id);
	    }
	}];
    }
}
