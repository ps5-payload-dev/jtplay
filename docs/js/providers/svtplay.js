// SPDX-License-Identifier: GPL-3.0-or-later
//
// SVT Play, the video service of Swedish public service television.
// Browser port of jtplay's plugins/svtplay.js.
//
// Listings come from SVT's Contento GraphQL endpoint, which only accepts
// *persisted* queries: the client sends the sha256 of a query document the
// server already knows, never the document itself. The hashes below are the
// ones svtplay.se uses; when SVT retires one the API answers with
// "PersistedQueryNotFound" and that listing stops working until the hash is
// refreshed. Playback goes through the separate video API, which mints
// short-lived manifest URLs, hence resolve() rather than a uri per entry.

(function() {
    var VIDEO_API = "https://api.svt.se/video/";
    var QUERY_API = "https://api.svt.se/contento/graphql";
    var IMAGE_API = "https://www.svtstatic.se/image";
    var CLIENT_UA = "svtplaywebb-play-render-prod-client";

    // [operationName, sha256 of the query document].
    var OP = {
	CHANNELS:  ["ChannelsQuery",
                    "210be4b72f03223b990f031d9a2e3501ff9284f8d2c66b01b255a807775f0b19"],
	PROGRAMS:  ["ProgramsListing",
                    "17252e11da632f5c0d1b924b32be9191f6854723a0f50fb2adb35f72bb670efa"],
	GENRES:    ["MainGenres",
                    "65b3d9bccd1adf175d2ad6b1aaa482bb36f382f7bad6c555750f33322bc2b489"],
	CATEGORY:  ["CategoryPageQuery",
                    "00be06320342614f4b186e9c7710c29a7fc235a1936bde08a6ab0f427131bfaf"],
	GRID:      ["GridPage",
                    "a8248fc130da34208aba94c4d5cc7bd44187b5f36476d8d05e03724321aafb40"],
	GRID_LIVE: ["GridPage",
                    "1e2d15ff7ffa578d33ebf1287d3f7af7fd47125552b564e96fd277a744345a69"],
	DETAILS:   ["DetailsPageQuery",
                    "e240d515657bbb54f33cf158cea581f6303b8f01f3022ea3f9419fbe3a5614b0"],
	VIDEO_ID:  ["DetailsPageQuery",
                    "5be42eb4028ed8f2680ce2302f6887df3fed2dcb6f61ac091ff5a37a3d0bf477"]
    };

    // The fixed rows of the start page: [id, name, description, selectionId].
    var START_ROWS = [
	["/live",       "Live nu",       "S\u00e4nds just nu",    "live_start"],
	["/popular",    "Popul\u00e4rt", "Mest sedda",            "popular_start"],
	["/latest",     "Senaste",       "Nyss tillagt",          "latest_start"],
	["/lastchance", "Sista chansen", "F\u00f6rsvinner snart", "lastchance_start"]
    ];

    var SHOW_TYPES = ["TvShow", "KidsTvShow", "TvSeries"];
    var VIDEO_TYPES = ["Episode", "Clip", "Single", "Trailer", "Variant"];

    // Subheadings that only repeat what the row already says.
    var NOISE = /^(Idag|Ikv\u00e4ll|Ig\u00e5r|I morgon)\b|\b(sek|min|tim)$/i;

    // Only HLS plays in the browser (natively or via hls.js); DASH would need
    // a different player, so it is not on the list.
    var FORMATS = [
	"hls", "hls-ts-avc", "hls-cmaf-full", "hls-cmaf-live", "hls-cmaf",
	"hls-ts-avc-51", "hls-ts-full"
    ];

    // Listings are browsed back and forth, and A-O is one 1500 item response,
    // so keep answers around briefly.
    var CACHE_TTL_MS = 10 * 60 * 1000;
    var cache = {};

    async function cached(key, produce) {
	var hit = cache[key];
	var now = Date.now();
	if (hit && now - hit.time < CACHE_TTL_MS) {
	    return hit.value;
	}
	var value = await produce();
	cache[key] = {time: now, value: value};
	return value;
    }

    async function query(op, variables) {
	var extensions = {
	    persistedQuery: {version: 1, sha256Hash: op[1]}
	};
	var url = QUERY_API +
            "?ua=" + CLIENT_UA +
            "&operationName=" + op[0] +
            "&variables=" + encodeURIComponent(JSON.stringify(variables || {})) +
            "&extensions=" + encodeURIComponent(JSON.stringify(extensions));

	var res = await fetch(url, {headers: {Accept: "application/json"}});
	if (!res.ok) {
	    throw new Error(op[0] + ": HTTP " + res.status);
	}

	var json = await res.json();
	if (json.errors && json.errors.length) {
	    // A stale hash lands here as "PersistedQueryNotFound".
	    throw new Error(op[0] + ": " + json.errors[0].message);
	}
	if (!json.data) {
	    throw new Error(op[0] + ": no data in response");
	}
	return json.data;
    }

    // Descriptions and headings arrive with markup in them now and then.
    function text(s) {
	if (!s) {
	    return "";
	}
	return String(s)
	    .replace(/<[^>]*>/g, "")
	    .replace(/&amp;/g, "&")
	    .replace(/&quot;/g, '"')
	    .replace(/&#39;/g, "'")
	    .replace(/&lt;/g, "<")
	    .replace(/&gt;/g, ">")
	    .replace(/\s+/g, " ")
	    .trim();
    }

    function imageUrl(img, width) {
	if (!img || !img.id) {
	    return "";
	}
	return IMAGE_API + "/wide/" + (width || 720) + "/" + img.id + "/" +
	    (img.changed || 0) + "?quality=70";
    }

    function pickImage(obj, fallback) {
	var images = obj && obj.images;
	if (images) {
	    var img = images.wide || images.cleanWide || images.landscape;
	    if (img) {
		return imageUrl(img);
	    }
	}
	if (obj && obj.image) {
	    return imageUrl(obj.image);
	}
	return fallback || "";
    }

    // Fold the case and push the three Swedish vowels past Z so that plain
    // string comparison sorts A-\u00d6 correctly.
    var COLLATE = {"\u00c5": "Z1", "\u00c4": "Z2", "\u00d6": "Z3",
                   "\u00c6": "Z2", "\u00d8": "Z3"};

    function sortKey(name) {
	return name.toUpperCase().replace(/[\u00c5\u00c4\u00d6\u00c6\u00d8]/g,
					  function(c) { return COLLATE[c]; });
    }

    function isShow(t)  { return SHOW_TYPES.indexOf(t) >= 0; }
    function isVideo(t) { return VIDEO_TYPES.indexOf(t) >= 0; }

    function videoId(item) {
	var svtId = item.videoSvtId || item.svtId;
	if (svtId) {
	    return "video:" + svtId;
	}
	var path = item.urls && item.urls.svtplay;
	return path ? "path:" + path : "";
    }

    // Converts one Teaser (or a bare item) into an entry. Returns null for
    // anything unplayable or unbrowsable.
    function toEntry(node, parentImage) {
	if (!node) {
	    return null;
	}

	var teaser = node.__typename === "Teaser" ? node : null;
	var item = teaser ? teaser.item : (node.item || node);
	if (!item) {
	    return null;
	}

	var typename = item.__typename;
	var name = text(teaser ? teaser.heading : "") || text(item.name);
	if (!name) {
	    return null;
	}

	var sub = teaser ? text(teaser.subHeading) : "";
	if (sub && sub !== name && !NOISE.test(sub)) {
	    name = name + " - " + sub;
	}

	var description =
            text(teaser ? teaser.description : "") ||
            text(item.longDescription) ||
            text(item.description);
	var image = pickImage(teaser, "") || pickImage(item, parentImage);

	if (isShow(typename)) {
	    var path = item.urls && item.urls.svtplay;
	    if (!path) {
		return null;
	    }
	    return {id: "show:" + path, type: "folder", name: name,
		    description: description, image: image};
	}

	if (isVideo(typename)) {
	    var id = videoId(item);
	    if (!id) {
		return null;
	    }
	    return {id: id, type: "video", name: name,
		    description: description, image: image};
	}

	if (typename === "Genre") {
	    return {id: "genre:" + item.id, type: "folder", name: name,
		    description: description, image: image};
	}

	console.log("skipping unsupported type: " + typename);
	return null;
    }

    function toEntries(nodes, parentImage) {
	var out = [];
	(nodes || []).forEach(function(node) {
	    var entry = toEntry(node, parentImage);
	    if (entry) {
		out.push(entry);
	    }
	});
	return out;
    }

    async function fetchChannels() {
	var res = await query(OP.CHANNELS);
	var channels = (res.channels && res.channels.channels) || [];

	return channels.map(function(ch) {
	    var running = ch.running || {};
	    return {
		id: "video:" + ch.id,
		type: "video",
		name: ch.name,
		description: running.name
		    ? text(running.name) + " - " + text(running.description)
		    : "Live",
		image: pickImage(running, "")
	    };
	});
    }

    async function fetchSelection(selectionId) {
	var op = selectionId === "live_start" ? OP.GRID_LIVE : OP.GRID;
	var res = await query(op, {
	    selectionId: selectionId,
	    includeFullOppetArkiv: true
	});
	return toEntries((res.selectionById || {}).items);
    }

    // The whole A-O catalogue in one request; the letters are our own grouping
    // so that a folder holds tens of entries rather than a thousand.
    function allPrograms() {
	return cached("programs", async function() {
	    var res = await query(OP.PROGRAMS);
	    var selections = (res.programAtillO && res.programAtillO.selections) || [];
	    var entries = [];
	    selections.forEach(function(selection) {
		toEntries(selection.items).forEach(function(e) { entries.push(e); });
	    });
	    entries.sort(function(a, b) {
		var ka = sortKey(a.name);
		var kb = sortKey(b.name);
		return ka < kb ? -1 : (ka > kb ? 1 : 0);
	    });
	    return entries;
	});
    }

    function programLetter(name) {
	var first = name.charAt(0).toUpperCase();
	return /^[A-Z\u00c5\u00c4\u00d6]$/.test(first) ? first : "#";
    }

    async function fetchProgramLetters() {
	var seen = {};
	var letters = [];
	(await allPrograms()).forEach(function(entry) {
	    var letter = programLetter(entry.name);
	    if (!seen[letter]) {
		seen[letter] = 0;
		letters.push(letter);
	    }
	    seen[letter]++;
	});

	letters.sort(function(a, b) {
	    if (a === "#") return 1;
	    if (b === "#") return -1;
	    return sortKey(a) < sortKey(b) ? -1 : 1;
	});

	return letters.map(function(letter) {
	    return {
		id: "programs:" + letter,
		type: "folder",
		name: letter === "#" ? "0-9 och \u00f6vrigt" : letter,
		description: seen[letter] + " program"
	    };
	});
    }

    async function fetchProgramsByLetter(letter) {
	return (await allPrograms()).filter(function(entry) {
	    return programLetter(entry.name) === letter;
	});
    }

    async function fetchGenres() {
	var res = await query(OP.GENRES);
	var genres = (res.genresInMain && res.genresInMain.genres) || [];
	return genres.map(function(g) {
	    return {id: "genre:" + g.id, type: "folder", name: g.name,
		    description: "", image: pickImage(g, "")};
	});
    }

    async function fetchGenre(id) {
	var res = await query(OP.CATEGORY, {
	    id: id,
	    tab: "all",
	    includeFullOppetArkiv: true
	});

	var tabs = (res.categoryPage || {}).lazyLoadedTabs || [];
	var items = [];

	tabs.forEach(function(tab) {
	    (tab.selections || []).forEach(function(selection) {
		if (selection.selectionType === "all" || selection.id === "all") {
		    items = items.concat(selection.items || []);
		}
	    });
	});
	if (!items.length) {
	    tabs.forEach(function(tab) {
		(tab.selections || []).forEach(function(selection) {
		    items = items.concat(selection.items || []);
		});
	    });
	}

	var seen = {};
	return toEntries(items).filter(function(entry) {
	    if (seen[entry.id]) {
		return false;
	    }
	    seen[entry.id] = true;
	    return true;
	});
    }

    // A show: its seasons, its clips, and whatever else SVT groups with it.
    async function fetchShow(path, folderId) {
	var res = await cached("show:" + path, function() {
	    return query(OP.DETAILS, {path: path, includeFullOppetArkiv: true});
	});

	var page = res.detailsPageByPath;
	if (!page) {
	    throw new Error("no such show: " + path);
	}

	var showImage = pickImage(page, "");
	var groups = (page.associatedContent || []).filter(function(group) {
	    // "upcoming" is unplayable and "related" is a different show.
	    return group.id !== "upcoming" && group.id !== "related" &&
		(group.items || []).length > 0;
	});

	if (folderId) {
	    var group = groups.filter(function(g) { return g.id === folderId; })[0];
	    if (!group) {
		throw new Error("no such section: " + folderId);
	    }
	    return toEntries(group.items, showImage);
	}

	if (groups.length === 1) {
	    return toEntries(groups[0].items, showImage);
	}

	return groups.map(function(group) {
	    return {
		id: "show:" + path + "|" + group.id,
		type: "folder",
		name: text(group.name) || group.id.replace(/-/g, " "),
		description: group.items.length + " videor",
		image: showImage
	    };
	});
    }

    function svtIdForPath(path) {
	return cached("svtid:" + path, async function() {
	    var res = await query(OP.VIDEO_ID, {path: path});
	    var page = res.detailsPageByPath;
	    var svtId = page && page.video && page.video.svtId;
	    if (!svtId) {
		throw new Error("no video on " + path);
	    }
	    return svtId;
	});
    }

    // videoReferences carry either a ready manifest url or a 'resolve'
    // endpoint that redirects to one; the latter is what live channels use.
    async function referenceUrl(ref) {
	if (ref.resolve) {
	    var res = await fetch(ref.resolve, {headers: {Accept: "application/json"}});
	    if (res.ok) {
		var location = (await res.json()).location;
		if (location) {
		    return location;
		}
	    }
	    console.warn("could not resolve " + ref.format + ", using its url");
	}
	return ref.url || "";
    }

    async function resolveStream(svtId) {
	var res = await fetch(VIDEO_API + svtId,
                              {headers: {Accept: "application/json"}});
	if (!res.ok) {
	    // 403 is what a geo blocked title abroad looks like.
	    throw new Error(res.status === 403
			    ? "not available from here"
			    : "video API: HTTP " + res.status);
	}

	var video = await res.json();
	var refs = video.videoReferences || [];
	if (!refs.length && video.variants && video.variants.default) {
	    refs = video.variants.default.videoReferences || [];
	}
	if (!refs.length) {
	    throw new Error("nothing to play in " + svtId);
	}

	// Best supported format wins.
	var best = null;
	var bestRank = FORMATS.length;
	refs.forEach(function(ref) {
	    var format = String(ref.format || ref.playerType || "").toLowerCase();
	    var rank = FORMATS.indexOf(format);
	    if (rank >= 0 && rank < bestRank) {
		best = ref;
		bestRank = rank;
	    }
	});

	if (!best) {
	    // SVT renames formats now and then, so before giving up take anything
	    // that looks like an HLS manifest.
	    best = refs.filter(function(ref) {
		return String(ref.url || "").indexOf(".m3u8") > 0;
	    })[0];
	}

	if (!best) {
	    throw new Error(video.rights && video.rights.drmCopyProtection
			    ? "copy protected"
			    : "no supported stream format for " + svtId);
	}

	var url = await referenceUrl(best);
	if (!url) {
	    throw new Error("empty stream url for " + svtId);
	}
	console.log("playing " + best.format + ": " + url);
	return url;
    }

    window.Providers.push({
	name: "SVT Play",
	detail: "TV from Swedish public service",
	icon: "\uD83D\uDCFA", // 📺

	browse: function(id) {
	    if (!id) {
		var rows = START_ROWS.map(function(row) {
		    return {id: row[0], type: "folder", name: row[1], description: row[2]};
		});
		return Promise.resolve([{
		    id: "channels",
		    type: "folder",
		    name: "Kanaler",
		    description: "Direkts\u00e4ndning"
		}].concat(rows, [{
		    id: "programs",
		    type: "folder",
		    name: "Program A-\u00d6",
		    description: "Hela utbudet"
		}, {
		    id: "genres",
		    type: "folder",
		    name: "Genrer",
		    description: "Bl\u00e4ddra efter kategori"
		}]));
	    }

	    if (id === "channels") {
		return fetchChannels();
	    }
	    if (id === "programs") {
		return fetchProgramLetters();
	    }
	    if (id === "genres") {
		return fetchGenres();
	    }

	    var row = START_ROWS.filter(function(r) { return r[0] === id; })[0];
	    if (row) {
		return fetchSelection(row[3]);
	    }

	    if (id.indexOf("programs:") === 0) {
		return fetchProgramsByLetter(id.substring("programs:".length));
	    }
	    if (id.indexOf("genre:") === 0) {
		return fetchGenre(id.substring("genre:".length));
	    }
	    if (id.indexOf("show:") === 0) {
		var rest = id.substring("show:".length);
		var bar = rest.indexOf("|");
		return bar < 0
		    ? fetchShow(rest)
		    : fetchShow(rest.substring(0, bar), rest.substring(bar + 1));
	    }

	    console.warn("cannot browse " + id);
	    return Promise.resolve([]);
	},

	resolve: async function(id) {
	    if (id.indexOf("video:") === 0) {
		return resolveStream(id.substring("video:".length));
	    }
	    if (id.indexOf("path:") === 0) {
		return resolveStream(await svtIdForPath(id.substring("path:".length)));
	    }
	    return resolveStream(id);
	}
    });
})();
