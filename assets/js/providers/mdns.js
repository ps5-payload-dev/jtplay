// SPDX-License-Identifier: GPL-3.0-or-later
//
// Servers announced over mDNS, browsed through the smb proxy in srv.c.
//
// Credentials go to the proxy as query parameters, which it forwards to
// libsmb2. A share that turns them down comes back as 401 or 403, and that
// is what makes this plugin ask: it throws Auth.required() and the shell
// takes care of the dialog and the retry.
//
// One login covers a whole server rather than a folder, so the realm is the
// server uri. Browsing deeper into a share reuses what the viewer already
// typed instead of asking again per directory.

export default function init(ctx) {
    const ICON = "icons/computer.png";

    // Server uri -> credentials the shell handed us for it. Holding these for
    // the session means only the first request to a server pays for being
    // refused first; everything after goes out authenticated straight away.
    const sessions = {};

    // The server a path belongs to, which is also its realm. Ids are absolute
    // smb: uris, so the origin is the part before the path.
    function realmOf(id) {
	const uri = new URL(id);
	return uri.protocol + "//" + uri.host;
    }

    function needSignIn(id, user) {
	const realm = realmOf(id);
	// Whatever we were using is no good, so drop it rather than sending it
	// again on the retry.
	delete sessions[realm];
	return Auth.required({
	    realm: realm,
	    title: "Sign in to " + new URL(id).hostname,
	    prompt: "This server asks for a username and password.",
	    user: user || ""
	});
    }

    // Credentials to use for `id`: whatever the shell just handed us, or what
    // it handed us earlier in the session. Credentials are tagged with the
    // realm they were issued for, so a password for one server is never sent
    // to another.
    function credsFor(id, creds) {
	const realm = realmOf(id);
	if (creds && creds.realm === realm) {
	    sessions[realm] = creds;
	    return creds;
	}
	return sessions[realm] || null;
    }

    function resolveId(id, creds) {
	if (!id) {
            throw new Error("resolveId: id is empty");
	}
	const uri = new URL(id);
	let url = "";

	if(uri.protocol != "smb:") {
	    throw new Error(uri.protocol + " Unsupported protocol");
	}
	url += "/smb";

	if(uri.pathname != "/") {
	    url += uri.pathname;
	}

	url += "?addr=" + encodeURIComponent(uri.hostname);
	url += "&port=" + encodeURIComponent(uri.port);

	// Passwords are arbitrary text and routinely contain characters that
	// mean something in a query string, so both parts are escaped.
	const use = credsFor(id, creds);
	if(use && use.user) {
	    url += "&user=" + encodeURIComponent(use.user);
	}
	if(use && use.pass) {
	    url += "&pass=" + encodeURIComponent(use.pass);
	}
	return url;
    }

    async function fetchListing(id, creds) {
	var res = await fetch(resolveId(id, creds));
	if(!res.ok) {
	    if(Auth.refused(res)) {
		throw needSignIn(id, creds && creds.user);
	    } else {
		throw new Error("SMB: " + res.status);
	    }
	}

	var items = (await res.json()) || [];
	return items
	    .filter((item) => item.name != "." && item.name != "IPC$")
	    .sort((a, b) => a.name.localeCompare(b.name))
	    .map((item) => ({
		id: id + "/" + item.name,
		type: item.mode == "d" ? "folder" : "file",
		name: item.name
	    }));
    }

    return async function discover() {
	var res = await fetch("/mdns");
	if(!res.ok) {
	    return [];
	}

	var servers = (await res.json()) || [];
	return servers.map(function(srv) {
	    const [name, prot] = srv.domain.split("._");
	    const uri = prot + '://' + srv.address + ":" + srv.port;

	    return {
		id: uri,
		name: name,
		detail: uri,
		icon: ICON,
		browse: async function(id, creds) {
		    if(!id) {
			return fetchListing(uri, creds);
		    } else {
			return fetchListing(id, creds);
		    }
		},
		resolve: async function(id, creds) {
		    return resolveId(id, creds);
		},
		// The shell drops credentials on sign-out or refusal; drop the
		// cached copy too, or the session outlives them.
		signOut: function(realm) {
		    delete sessions[realm];
		}
	    };
	});
    }
}
