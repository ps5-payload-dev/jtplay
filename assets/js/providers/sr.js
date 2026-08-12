// SPDX-License-Identifier: GPL-3.0-or-later

export default function init(ctx) {
    const API_URL = "https://api.sr.se/api/v2/channels?format=json&pagination=false&audioquality=hi";

    return async function discover() {
	return [{
	    id: "https://sr.se",
	    name: "Sveriges Radio",
	    detail: "Live radio from Swedish public service",
	    icon: "📻",
	    browse: async function(id) {
		const res = await fetch(API_URL);
		if (!res.ok) {
		    throw new Error(res.status);
		}

		const channels = (await res.json()).channels || [];
		return channels.map((ch) => ({
		    id: String(ch.id),
		    type: "audio",
		    name: ch.name,
		    description: ch.tagline,
		    image: ch.image,
		    uri: ch.liveaudio.url
		}));
	    }
	}];
    }
}
