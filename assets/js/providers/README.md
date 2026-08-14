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
		icon: "icons/tv.png",
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
| `browse`  | yes      | `(id, creds) -> entries`, called with `""` for the root         |
| `id`      | no       | stable id within the plugin; keeps the cursor in place across a refresh, so prefer something durable over an array index |
| `detail`  | no       | one line under the name                                         |
| `icon`    | no       | url of an image to show next to the name, relative to the page or absolute; `icons/` holds the ones the shell ships. Emoji are not used - the console's browser has no font for most of them |
| `resolve` | no       | `(id, creds) -> url`, for entries without a `uri` (short-lived manifest urls, signed links, ...) |
| `signOut` | no       | `(realm)`, told when the shell drops credentials; see below     |

Both `browse` and `resolve` may be async and may throw; failures become an
on-screen message rather than taking down the app. A plugin with exactly one
provider may return it bare instead of in a list.

## Credentials

`creds` is `null` unless the provider has asked for a sign-in. Nothing knows
in advance which parts of a source need a login, so a provider that finds one
does need it says so by throwing:

```js
throw Auth.required({
    realm:  "smb://192.168.1.10:445",   // what the password unlocks
    title:  "Sign in to MyNAS",         // dialog heading
    prompt: "This server asks for a username and password."
});
```

The shell puts up the sign-in dialog, then calls again with
`{user, pass, realm}` as the second argument. Throw again for the same realm
and the saved credentials are dropped and the dialog returns, this time saying
they were refused. Cancelling the dialog abandons the call quietly.

Realms rather than providers are the unit of authentication, so one provider
may use several - a public folder, a share behind one password, another behind
a different one - and signing in to one does not unlock the rest. Pick a realm
that matches what the password actually covers: per server usually, not per
folder, or the viewer is asked again on every directory.

Credentials come back tagged with the realm they were issued for, so a
provider serving more than one server can check `creds.realm` before putting a
password on the wire. The shell never volunteers credentials for a realm
nobody named.

Hold on to what you derive from credentials for the session - a url, a token,
a header - so only the first call pays for being refused first. If you do,
implement `signOut(realm)` and drop the cached copy there: the shell calls it
when the viewer signs out (square on the sources screen) and when a stored
password turns out to be stale, and without it the cached session would
outlive the credentials it came from.

`Auth.refused(res)` is a shorthand for the 401 and 403 that mean "not with
those credentials"; the smb proxy in `src/` returns both.

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
