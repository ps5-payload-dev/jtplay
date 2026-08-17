// SPDX-License-Identifier: GPL-3.0-or-later

export default function init(ctx) {
    const FS_URL = "/fs/";
    const REMUX_URL = "/remux/";

    // The browser here plays mp4 and webm and turns down most of what else
    // ends up on a disk, usually over the wrapper rather than the streams
    // inside it. The remuxer rewraps those as they are served, leaving the
    // codecs alone, so a file is worth asking about before its url is handed
    // over. Put "config": {"remux": false} in the manifest entry to go back to
    // serving everything straight off /fs.
    const remuxing = !ctx.config || ctx.config.remux !== false;

    // What browse() last called each entry. resolve() is given an id and
    // nothing else, and only audio and video are worth asking about: a probe
    // of a jpeg finds a picture it calls a video stream and would talk itself
    // into rewrapping the thing.
    const types = new Map();

    function itemType(item) {
	if(item.mode == "d") {
	    return "folder";
	}

	if(item.mime.startsWith("audio/")) {
	    return "audio";
	}
	if(item.mime.startsWith("image/")) {
	    return "image";
	}
	if(item.mime.startsWith("video/")) {
	    return "video";
	}
	return "file";
    }

    // Where to play a file from: off the disk as it stands, or through the
    // remuxer. Asking costs one open of the file, at the moment it is picked.
    async function playbackUrl(id) {
	const direct = FS_URL + encodeURIComponent(id);
	const type = types.get(id);
	let info;

	if(!remuxing || (type !== "audio" && type !== "video")) {
	    return direct;
	}

	try {
	    const res = await fetch(REMUX_URL + "probe.json?uri=" +
				    encodeURIComponent(id));
	    if(!res.ok) {
		return direct;
	    }
	    info = await res.json();
	} catch(err) {
	    // An older build has no /remux at all, and a file served
	    // untouched is what would have happened before any of this.
	    console.warn("remux probe failed, serving directly", err);
	    return direct;
	}

	// Already something the browser takes, so nothing gains by sitting in
	// the middle of it.
	if(info.direct) {
	    return direct;
	}
	if(info.url) {
	    return info.url;
	}

	// Rewrapping cannot help; the codecs themselves would have to change.
	throw new Error(info.reason || "unsupported media");
    }

    return async function discover() {
	return [{
	    id: "/",
	    name: "Local filesystem",
	    icon: "icons/disk.png",
	    browse: async function(id) {
		const res = await fetch(FS_URL + id);
		if (!res.ok) {
		    throw new Error(res.status);
		}

		const listing = (await res.json()) || [];
		return listing
		    .filter((item) => item.name != ".")
		    .sort((a, b) => a.name.localeCompare(b.name))
		    .map((item) => {
			const entry = {
			    id: id + '/' + item.name,
			    type: itemType(item),
			    name: item.name
			};

			types.set(entry.id, entry.type);
			return entry;
		    });
	    },
	    resolve: playbackUrl
	}];
    }
}
