// SPDX-License-Identifier: GPL-3.0-or-later

export default function init(ctx) {
    const VIZ_URL = "http://127.0.0.1:8088/viz/master.m3u8";
    const API_URL = "https://api.sr.se/api/v2";

    const LIVE_TEMPLATE_MP3 = 2;
    const LIVE_TEMPLATE_AAC_PLS = 3;
    const LIVE_TEMPLATE_AAC_M3U = 4;
    const LIVE_TEMPLATE_AAC = 5;
    const LIVE_TEMPLATE_HLS = 10;
    const LIVE_TEMPLATE_DASH = 11;
    const LIVE_TEMPLATE_IOS = 12;

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
					"&liveaudiotemplateid=" + LIVE_TEMPLATE_AAC);
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
