// SPDX-License-Identifier: GPL-3.0-or-later

export default function init(ctx) {
    const ICON = "icons/tv.png";

    async function api(path, udn, id) {
	const url = path +
	      "?udn=" + encodeURIComponent(udn) +
	      "&id=" + encodeURIComponent(id || "0");
	const res = await fetch(url);
	let body = null;

	try {
	    body = await res.json();
	} catch (err) {
	    body = null;
	}

	// the proxy says what went wrong, and it reads better than a status
	if(!res.ok) {
	    throw new Error(body && body.error ? body.error
			    : "DLNA: " + res.status);
	}

	return body || [];
    }

    // Servers rarely say much about themselves beyond a friendly name, so
    // fall back to something that tells two boxes apart.
    function detail(srv) {
	if(srv.model && srv.address) {
	    return srv.model + " (" + srv.address + ")";
	}
	return srv.model || srv.manufacturer || srv.address || "";
    }

    return async function discover() {
	const res = await fetch("/dlna");
	if(!res.ok) {
	    throw new Error("DLNA: " + res.status);
	}

	const servers = (await res.json()) || [];

	return servers.map((srv) => ({
	    // the UDN is what the server calls itself, and it outlives a
	    // refresh, so the cursor stays put
	    id: srv.udn,
	    name: srv.name,
	    detail: detail(srv),
	    icon: srv.icon || ICON,
	    browse: async function(id) {
		return api("/dlna/browse", srv.udn, id);
	    },
	    // Entries normally arrive with a uri already on them. This is
	    // for the ones that do not, where asking the server about that
	    // one object is the last chance to find something playable.
	    resolve: async function(id) {
		const [entry] = await api("/dlna/metadata", srv.udn, id);
		if(!entry || !entry.uri) {
		    throw new Error("Nothing playable in " +
				    (entry && entry.name ? entry.name : id));
		}
		return entry.uri;
	    }
	}));
    }
}
