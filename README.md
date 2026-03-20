# tree-sitter-sky

Tree-sitter grammar for the [Sky programming language](https://github.com/anzellai/sky) — an Elm-inspired language that compiles to Go.

## Usage with Helix

Add to your `languages.toml`:

```toml
[[language]]
name = "sky"
scope = "source.sky"
file-types = ["sky", "skyi"]
auto-format = true
formatter = { command = "sky", args = ["fmt", "-"] }
language-servers = ["sky-lsp"]
indent = { tab-width = 4, unit = "    " }

[[grammar]]
name = "sky"
source = { git = "https://github.com/anzellai/tree-sitter-sky" }

[language-server.sky-lsp]
command = "sky-lsp"
args = ["--stdio"]
```

Then fetch and build:

```bash
hx --grammar fetch
hx --grammar build
```

Copy `queries/highlights.scm` to your Helix runtime:

```bash
mkdir -p ~/.config/helix/runtime/queries/sky
cp queries/highlights.scm ~/.config/helix/runtime/queries/sky/
cp queries/locals.scm ~/.config/helix/runtime/queries/sky/
```

## Development

```bash
npm install
npm run generate
npm run test
```

## License

MIT
