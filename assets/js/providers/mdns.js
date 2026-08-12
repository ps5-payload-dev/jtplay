// SPDX-License-Identifier: GPL-3.0-or-later

export default function init(ctx) {
    const ICON = "💻";

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

	url += "?addr=" + uri.hostname;
	url += "&port=" + uri.port;

	if(creds && creds.user) {
	    url += "&user=" + creds.user;
	}
	if(creds && creds.pass) {
	    url += "&pass=" + creds.pass;
	}
	console.log(url);
	return url;
    }

    async function fetchListing(id, creds) {
	console.log(id);
	var res = await fetch(resolveId(id, creds));
	if(!res.ok) {
	    if(res.status == 401 || res.status == 403) {
		throw Auth.required({
		    realm: id,
		    title: id,
		    prompt: "Promt",
		    user: ""
		});
	    } else {
		throw new Error("SMB: " + res.status);
	    }
	}

	var items = (await res.json()) || [];
	return items.map(function(item) {
	    const name = item.name;
	    const type = item.mode == "d" ? "folder" : "file";
	    const uri = id + "/" + item.name;
	    const desc = uri;
	    
	    return {
		id: uri,
		type: type,
		name: name
	    };
	}).filter(function(item) {
	    return item.name != "." && item.name != "IPC$";
	});
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
		browse: async function(id) {
		    if(!id) {
			return fetchListing(uri);
		    } else {
			return fetchListing(id);
		    }
		},
		resolve: async function(id) {
		    return resolveId(id);
		}
	    };
	});
    }
}
