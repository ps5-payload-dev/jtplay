// SPDX-License-Identifier: GPL-3.0-or-later

export default function init(ctx) {
    const FS_URL = "/fs/";

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
		    .map((item) => ({
			id: id + '/' + item.name,
			type: item.mode == "d" ? "folder" : "file",
			name: item.name
		    }));
	    },
	    resolve: async function(id) {
		return FS_URL + encodeURIComponent(id);
	    }
	}];
    }
}
