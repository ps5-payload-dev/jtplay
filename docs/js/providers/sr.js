// SPDX-License-Identifier: GPL-3.0-or-later

export default function init(ctx) {
    const NAME = "Sveriges Radio";
    const DETAILS = "Live radio from Swedish public service";
    const URL = "https://api.sr.se/api/v2/channels?format=json&pagination=false&audioquality=hi";
    const ICON = "📻";

    return async function discover() {
	return [{
	    name: NAME,
	    detail: DETAILS,
	    icon: ICON,
	    browse: async function(id) {
		const res = await fetch(URL);
		if (!res.ok) {
		    throw new Error(NAME + ": " + res.status);
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
