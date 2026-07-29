# JTPlay

A audio/video player for jailbroken PS5s where the majority of the code
was produced by claude.ai

## Lua plugins

Besides UPnP/DLNA servers and the local filesystem, media sources can be
added with Lua scripts. Every `*.lua` file in the plugin directory
(`--plugins DIR`, default `<assets>/../plugins`) becomes one provider; the
script returns a table implementing the same interface as `browse::Provider`
and `browse::Source`:

```lua
return {
  name = "My provider",
  discover = function()
    return {
      {
        name = "My source",       -- row title
        detail = "somewhere",     -- row subtitle (optional)
        icon = "📻",              -- emoji glyph (optional)
        root = "/",               -- root folder id (optional)
        browse = function(id)     -- list the children of 'id'
          return {
            { title = "A stream", kind = "audio",
              url = "https://example.com/stream" },
          }
        end,
      },
    }
  end,
}
```

See `src/browse/lua_source.h` for the full set of entry fields and
`plugins/sr.lua` (Sveriges Radio live channels) for a working example.

