# Media plugins

A plugin is an ES module listed in [manifest.json](manifest.json). It default
exports an `init()` function, which is called once per load and returns a
*discovery* function. Discovery is async, so it may use `fetch()` to work out
what the plugin can offer right now, and it returns the list of media
providers to show on the sources screen. One plugin can return as many
providers as it likes - one per server, account, library, channel group - so
nothing has to be known at load time.

```js
export default function init(ctx) {
    return async function discover() {
	var res = await fetch("https://example.com/servers");

	return (await res.json()).map(function(srv) {
	    return {
		id: srv.id,
		name: srv.title,
		detail: srv.location,
		icon: "\uD83D\uDCFA",
		browse: async function(id) { ... },
		resolve: async function(id) { ... }
	    };
	});
    };
}
```

`ctx` is `{id, name, url, config}`, taken from the plugin's manifest entry.

## Provider

| field     | required | meaning                                                        |
|-----------|----------|----------------------------------------------------------------|
| `name`    | yes      | shown on the sources screen                                     |
| `browse`  | yes      | `(id) -> entries`, called with `""` for the root                |
| `id`      | no       | stable id within the plugin; keeps the cursor in place across a refresh, so prefer something durable over an array index |
| `detail`  | no       | one line under the name                                         |
| `icon`    | no       | one emoji                                                       |
| `resolve` | no       | `(id) -> url`, for entries without a `uri` (short-lived manifest urls, signed links, ...) |

Both `browse` and `resolve` may be async and may throw; failures become an
on-screen message rather than taking down the app. A plugin with exactly one
provider may return it bare instead of in a list.

## Entry

`browse()` returns objects with `id`, `type` (`folder`, `audio` or `video`),
`name`, and optionally `description`, `image`, and `uri`. An entry with a
`uri` plays directly; otherwise the provider's `resolve()` is asked for a url
when the entry is selected.

## Manifest

```json
{
    "version": 1,
    "plugins": [
	{"id": "sr", "name": "Sveriges Radio", "src": "sr.js", "enabled": true}
    ]
}
```

Only `src` is required; it is resolved relative to the manifest, so a plugin
may also be hosted elsewhere by giving an absolute url. `id` defaults to the
filename, `enabled` defaults to true, and an optional `config` object is
handed to `init()` as `ctx.config`. An entry may also be written as a plain
string (`"sr.js"`).

Triangle on the sources screen re-reads the manifest, re-imports every module
and re-runs discovery, so an edit here shows up without reloading the page.
A plugin that fails to load, fails to initialize or throws during discovery is
reported on screen and skipped; the rest still load.
