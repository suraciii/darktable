# Darktable MCP

A headless [Model Context Protocol](https://modelcontextprotocol.io) server that
exposes darktable's raw-development engine to AI agents over stdio (JSON-RPC 2.0).
It is a sibling command-line binary to `darktable-cli`, linking `libdarktable`
directly, so it gets real module introspection, the in-process pixelpipe, and
native styles/history — no XMP hand-editing or SQL injection.

## Building

`darktable-mcp` is built by default (CMake option `USE_MCP`, default `ON`,
disable with `-DUSE_MCP=OFF` or `./build.sh --disable-mcp`). It depends on
`json-glib-1.0` (already a darktable dependency).

```sh
# built as part of a normal build
./build.sh

# or just the target
cmake -B build .
cmake --build build --target darktable-mcp -j
```

The binary lands at `build/bin/darktable-mcp`.

## Running

```sh
darktable-mcp [--read-only] [--core <darktable core options...>]
```

`--read-only` is the server's own flag and goes **before** `--core`. Everything
after `--core` is forwarded verbatim to `dt_init`, so all darktable core options
work exactly as they do for `darktable`/`darktable-cli`: `--configdir`,
`--cachedir`, `--library`, `--conf key=value`, `-d <domain>`, etc.

Give neither `--library` nor `--configdir` and two defaults are injected
together, because both mean "touch nothing":

- `--library :memory:` — a throwaway catalog (loose files are imported ad-hoc).
- `--conf write_sidecar_files=never` — no `.xmp` written next to your files.

Name **either** and neither is injected. A `--configdir` selects its own
`library.db`, the way it does everywhere else in darktable, so pointing the
server at a config directory gives you that catalog rather than an empty one.
You have then opted into persistence: your own `write_sidecar_files` preference
applies and edits reach the sidecar.

### `--read-only`

Renders that carry a `stack` **modify the image** (see [Develop](#develop)).
`--read-only` refuses every tool that would change the library — stacks,
`reset_history`, `apply_style`, `save_style`, `import_style`, `set_rating`,
`set_color_label` — while leaving reads and plain renders untouched:

```sh
darktable-mcp --read-only --core --library /path/to/library.db
```

Use it to point an agent at a real catalog without letting it edit. An in-memory
library also prevents persistence, but costs you access to the catalog you
wanted to work on.

Rendering is not free of side effects in darktable: the first time the pipeline
runs on an image whose history is empty, darktable writes out the modules
`plugins/darkroom/workflow` auto-applies and syncs the sidecar. `--read-only`
takes that back — it suppresses the `.xmp` for the duration and clears the
auto-applied history afterwards, so the image ends the request with the empty
history and the flags it started with. Two things it does not undo, because
neither is an edit: reading the raw fills the cached sensor columns
(`raw_black`, `raw_maximum`) on the image row, and loading an image that
*already* has a history rewrites those same rows and their hash as it reads
them.

Rendering a loose file by `input.path` still works, because that import is
scratch and is undone before the request returns (see [Input](#input)) - its net
effect on the library is nothing. The one thing it leaves is the shared
`darktable|format|<ext>` tag *definition* in `data.db`, which names no image:
darktable never deletes those either, so removing an image in the GUI leaves the
same row (`dt_image_remove()`, `image.c:1591`). `import_images` is refused,
since that one is meant to persist.

**stdout carries only JSON-RPC** (darktable's own logging is redirected to
stderr), so a client can speak the protocol cleanly.

### Library modes

- **Ad-hoc (default `:memory:`)** — a throwaway catalog. Only when neither
  `--library` nor `--configdir` is given.
- **Real catalog** — `--core --library /path/to/library.db`, or just
  `--core --configdir /path/to/dir` to use that directory's `library.db`. The
  library tools then see existing images, their edits, and saved styles.

> darktable takes a PID lock on `library.db`/`data.db`, so catalog mode must run
> while the GUI is **not** holding that library (or against a copy).

## Connecting to Claude

`darktable-mcp` is a standard stdio MCP server. Give it a dedicated config/cache
directory: the server locks whichever library it opens, and a directory of its
own means that is never the one the GUI is holding, so both can run at once. The
example below also gives the server a catalog of its own — import images into it
and the library tools can see them:

```sh
mkdir -p ~/.config/darktable-mcp
```

### Claude Code

```sh
claude mcp add darktable -- \
  /path/to/build/bin/darktable-mcp \
  --core --configdir ~/.config/darktable-mcp \
         --cachedir  ~/.config/darktable-mcp/cache
```

Default scope is local (this project); add `-s user` for all projects or
`-s project` to write a shared `.mcp.json`. Check with `claude mcp list` (or
`/mcp` in a session); remove with `claude mcp remove darktable`.

### Claude Desktop

Add the same server to `claude_desktop_config.json` (Settings → Developer → Edit
Config) and restart the app:

```json
{
  "mcpServers": {
    "darktable": {
      "command": "/path/to/build/bin/darktable-mcp",
      "args": ["--core",
               "--configdir", "/home/you/.config/darktable-mcp",
               "--cachedir",  "/home/you/.config/darktable-mcp/cache"]
    }
  }
}
```

Then ask in natural language, e.g. *"Using darktable, render this raw and show
it"* or *"measure image_stats with agx target_black at 0.008 vs 0.0008"*. For a
real catalog, swap the args to `--core --library /path/to/library.db` (GUI must
be closed for that library).

## Protocol

Standard MCP handshake over stdio, both newline-delimited JSON (default) and
`Content-Length:` framing are accepted. Methods: `initialize`,
`notifications/initialized`, `tools/list`, `tools/call`, `ping`, `shutdown`.

```jsonc
// → request
{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}
// ← reply
{"jsonrpc":"2.0","id":1,"result":{"protocolVersion":"2025-06-18",
  "capabilities":{"tools":{"listChanged":false}},
  "serverInfo":{"name":"darktable-mcp","version":"0.1.0"}}}
```

## Tools

### Introspection

| Tool | Input | Output |
|------|-------|--------|
| `list_modules` | – | `[{operation, version, have_introspection, doc_url}]` |
| `module_schema` | `{operation}` | recursive schema including nested structs, arrays, string constraints, ranges, enum symbols/descriptions, and explicit unsupported kinds |
| `decode_params` | `{operation, blob_hex, params_version}` | `{operation, version, fields:{…}}`; size and version must both match |
| `encode_params` | `{operation, fields:{…}}` | `{operation, blob_hex}` — strict typed validation, no coercion |
| `image_parameters` | `{input:{path\|imgid}}` | initialized values, reset defaults, instance identities, and `iop_order`; non-introspected instances carry null values and an explicit unsupported reason |
`doc_url` is the module's page in the darktable usermanual — fetch it for prose
docs on what each parameter means.

### Input

`render`, `image_stats` and `export_images` take either an `imgid` or a
`path`, and the two mean different things.

| `input` | behaviour |
|---------|-----------|
| `{imgid}` | the normal route. Edits persist and the XMP sidecar follows. |
| `{path}` not in the catalog | **scratch**: the file is imported so darktable can develop it, then the image rows are removed again - the sidecar duplicates darktable files alongside the image included - and the film roll with them. No history, no sidecar, nothing kept. The file on disk is never touched. |
| `{path}` already in the catalog | refused, naming the `imgid` so you can decide. |

darktable cannot develop a bare file — history, sidecars and module defaults are
all keyed to a row in the library — so a path always has to become an image
first. Scratch import is how that happens without the library growing a row
every time an agent looks at a file.

A path that is *already* imported is refused rather than resolved silently: the
same request would otherwise sometimes mean "throwaway" and sometimes mean "your
real image, edits persist", and cleaning up afterwards would delete the image,
its history and its tags. The error names the `imgid` and the two ways on —
`input.imgid` to work on it properly, adding `history_end: 0` to that same call
to render it ignoring its existing edits. Both routes need the `imgid`: a path
is refused before `history_end` is even read.

**Prefer `import_images` and then work by `imgid`.** That is the normal darktable
workflow, it is the only route where edits are kept, and it makes what the agent
did visible in your catalog afterwards. Path input is for looking at a file, not
for working on one.

### Develop

| Tool | Input | Output |
|------|-------|--------|
| `render` | `{input:{path\|imgid}, width?, height?, stack?, disable_tone_mappers?, history_end?}` | MCP image content (base64 PNG) |
| `image_stats` | same as `render` | per-channel `{min, max, mean, p1, p50, p99, clip_lo, clip_hi}` |

A `stack` is an array of `{operation, params:{…} | blob_hex, params_version?,
multi_priority?, enabled?}` applied to an isolated image state. Omitted fields
retain their initialized or current values. Binary parameters require the
current version from `module_schema`; equal-sized older versions are refused.
Invalid types, unknown fields or enums, fractional integers, malformed arrays,
and unsupported strings fail without changing the source image's history.

`disable_tone_mappers:true` disables the auto-applied tone mapper before applying
the stack. `baseline:"raw-development"` instead starts from native RAW camera
initialization and suppresses artistic defaults. This baseline requires
`history_end:-1` and a supported RAW input.

A stack entry may carry `before` or `after` naming another module. Native
ordering rules determine whether the move is legal. An absent `multi_priority`
instance is created only when the module supports multiple instances.
`image_parameters` reports initialized values, reset defaults, and the actual
instance order without committing history.

`render`, `image_stats`, and `export_images` share preparation. For `input.imgid`,
requested edits commit only after the output succeeds. Failure restores the
prior catalog state, including an undeveloped image's empty history. For
`input.path`, the image and its request history remain temporary. External XMP
loading and writing are suppressed for these processing requests.

`history_end` selects the base history before applying intent. With a stack,
successful catalog processing replaces the selected history with the result;
without intent, it only selects what to process. Read-only catalog processing
refuses edits but permits inspection and output from unchanged history.

### Configuration

| Tool | Input | Output |
|------|-------|--------|
| `list_conf` | `{prefix?}` | declared settings `[{key, value, type, default, min?, max?, values?}]` |
| `get_conf` | `{key}` | one setting, same shape |

Read-only by design. Several settings change what `render` and `export_images`
produce — `plugins/darkroom/workflow` decides which modules are auto-applied,
`write_sidecar_files` decides whether an edit reaches the `.xmp` — so being able
to read them explains results that otherwise look wrong. There is no `set_conf`:
changing a setting mid-session would silently invalidate every result gathered
before it, with nothing recording that it happened. Restart with
`--conf key=value` instead, which makes the change a visible session boundary.

### Library (catalog)

| Tool | Input | Output |
|------|-------|--------|
| `import_images` | `{paths[]?, folder?, recursive?}` | `{images:[{path, status, imgid?, error?}], imported, already, failed}` |
| `list_images` | `{limit?, film_roll?, folder?, rating?, color?, rejected?}` | `[{imgid, path, rating, color_labels?, rejected?}]` |
| `list_film_rolls` | – | `[{film_roll, name, folder, images}]` |
| `get_metadata` | `{imgid}` | camera and lens, EXIF (ISO, shutter, aperture, focal length, capture time), and the sensor's `raw` black level and white point |
| `get_history` | `{imgid}` | the edit stack, decoded per module `[{num, operation, version, enabled, iop_order, multi_priority, fields}]` |
| `reset_history` | `{imgid}` | `{ok:true}` — clears the image's edits back to its imported state |
| `set_rating` | `{imgids[], rating?, reject?}` | `{ok:true}` |
| `set_color_label` | `{imgids[], color, toggle?}` | `{ok:true}` |
| `list_styles` | `{filter?, limit?}` | `{total, styles:[{name, description}]}` |
| `apply_style` | `{name, imgid \| imgids[], overwrite?}` | `{ok:true}` |
| `save_style` | `{name, description?, imgid}` | `{ok:true}` |
| `import_style` | `{path}` | `{ok:true}` (`.dtstyle` → styles DB) |
| `export_images` | `{input \| imgids[], out_path?, out_dir?, format?, quality?, width?, height?, upscale?, high_quality?, …}` | `{ok, exported, paths[]}` — writes files; `format:"scene-linear-tiff"` forces full-size float32 scene-linear TIFF |

**Importing.** `import_images` is how files enter the catalog: pass `paths`, or a
`folder` (with `recursive` for subdirectories) to add a whole shoot. Re-importing
is idempotent — an image already in the library comes back as `already` with its
existing `imgid`, never a duplicate. A walked folder is filtered to files
darktable can read, so sidecars and stray files are skipped quietly; a path you
name explicitly is always attempted, so you get a reason when it fails.

**Browsing.** A film roll is darktable's unit for an imported folder, and it is
what a photographer names a shoot by, so `list_film_rolls` is how an agent
discovers what to filter on: `film_roll` matches one exactly, while `folder` is
a substring that can span a parent directory holding several rolls. `rating` is
a minimum, and `color` and `rejected` narrow further.

**Culling.** `set_rating` and `set_color_label` take a list, because rating and
labeling a shoot one image per round trip is the slow way to do it. Rejection
is a flag beside the stars rather than a rating value, so pass `reject:true`
rather than a number, and a rejected frame keeps whatever stars it had. Both
tools are idempotent: darktable's GUI turns a repeated rating back off, which is
a keyboard affordance no API caller asked for, so the bridge suppresses it.

**Styles.** A catalog can hold hundreds of them, and the full list runs to tens
of kilobytes, so `list_styles` takes a `filter` substring (matched against name
and description, as darktable's own style list does) and a `limit`. `total`
always counts every match, so a truncated list is visible as one.

**Batch.** `apply_style` and `export_images` take either the singular
`imgid`/`out_path` or the plural `imgids`/`out_dir`; given both, the list wins.
Batch exports are named after their source files, as `darktable-cli` does with a
directory target. Basenames repeat across film rolls and between a raw and its
JPEG, so a clash is settled by darktable's own conflict setting,
`plugins/imageio/storage/disk/overwrite`: by default a free `_01` name is taken
rather than the existing file overwritten. The setting decides what happens to
files that were on disk before the export; two sources of one batch that resolve
to the same target always take separate names whatever it says, since letting
one image overwrite another's output would lose it. The directory is created if
it does not exist. A batch stops at its first failure. Outputs and history are
published only after every selected image prepares and writes successfully.
Failure restores prior outputs and history; the error names unattempted images.

**Where exports go.** Name `out_path` (one image) or `out_dir` (several), or
name neither and darktable decides, exactly as its own export module would:
`plugins/imageio/storage/disk/file_directory`, whose default is
`$(FILE_FOLDER)/darktable_exported/$(FILE_NAME)` — a `darktable_exported/`
folder beside each source file, never the server's working directory, which a
caller has no way to know. The reply lists the resolved `paths`, so an agent
that let darktable choose still learns where the files went.

**Skipped targets.** Set `plugins/imageio/storage/disk/overwrite` to 3 and an
existing file is left alone, so nothing is written for that image. The reply
always carries `skipped`, and names those targets in `skipped_paths` when it is
not zero: an export that wrote nothing otherwise reads exactly like one that
succeeded. Only a file that was already there is skipped, never a name another
image of the same call has just taken. An `out_path` you named yourself is never
subject to the policy.

**Export before render.** `export_images` accepts the same `stack`, baseline,
and history selection as `render`, and needs no prior render to materialize
intent. Omit `width` and `height` for full resolution. For
`format:"scene-linear-tiff"`, provide `icc_file` naming a profile registered in
`configdir/color/out`; the native writer uses float32 scene-linear RGB and
refuses a missing profile instead of silently choosing sRGB. Negative and
over-range samples remain representable.

**Formats.** `format` picks the output module — `jpeg`, `png`, `tiff`, `webp`,
`jxl`, `avif`, `exr`, `pfm`, `ppm`, `j2k`, whichever your build has (`jpg` and
`tif` are accepted as aliases). Omit it and the extension on `out_path` decides;
with nothing to go on, the format darktable's own export is set to
(`plugins/lighttable/export/format_name`, which ships as `jpeg`). `quality` applies to the lossy formats; everything else a
format offers comes from its own `plugins/imageio/format/…` settings, readable
with `list_conf`. `upscale` allows an export larger than the source, and
`high_quality` selects the slower, better resampling.

`get_metadata`'s `raw` block is filled in by `rawprepare`, so it appears only
once the image has been through the pipeline at least once — render it first if
you need the black level and white point.

## Example

Measure the shadow-floor effect of an `agx` parameter on a raw, with the default
tone mapper disabled:

```jsonc
{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{
  "name":"image_stats",
  "arguments":{
    "input":{"path":"/photos/DSCF0001.RAF"},
    "width":512,"height":512,
    "disable_tone_mappers":true,
    "stack":[{"operation":"agx",
              "params":{"curve_target_display_black_ratio":0.0008},
              "enabled":true}]}}}
```

The result's text is JSON like
`{"width":512,"height":341,"channels":{"g":{"p1":6,"p50":71,…},…}}`.

## Architecture

```text
src/mcp/
  main.c            process lifecycle: dt_init boot, --core passthrough, stdio loop
  mcp_jsonrpc.c/.h  transport: framing, JSON-RPC dispatch, error objects
  mcp_tools.c/.h    tool registry + JSON <-> C marshalling
  dt_bridge.c/.h    the ONLY unit that calls libdarktable
```

`dt_bridge.c` isolates every libdarktable call, so API churn (e.g. a bumped
module param version) stays contained. Parameters are always addressed through
introspection (`get_p` / `get_introspection_linear`), never fixed byte offsets,
so the server stays correct across module versions.

## Customizing tool descriptions & schemas

Each tool's presentation — `name`, `description`, `inputSchema` — lives in
`mcp_tools.json` in darktable's data folder (installed to
`share/darktable/mcp_tools.json`, alongside `noiseprofiles.json` etc.), loaded at
startup via `dt_loc_get_datadir()`. Only the tool *behaviour* (the handler) is
compiled into the binary, matched to a metadata entry by `name`.

So you can reword a description (to steer which tool the model picks) or tighten
an `inputSchema`, then restart the server — no rebuild. Adding a genuinely new
tool still needs a C handler in `mcp_tools.c`. A JSON entry whose `name` has no
handler is ignored with a warning on stderr.

## Notes & limitations

- **Headless rendering** goes through an in-memory export format module + plain
  cairo, not `dt_imageio_preview` (that helper builds its surface via a GUI-only
  cairo wrapper and crashes without a GUI).
- **First render** of a raw runs demosaic + the full pipe and can take a few
  seconds; give clients a generous timeout.
- **Version upgrades:** binary parameter blobs must match both the current
  version and size. Older desktop history layouts are explicitly refused;
  this boundary performs no legacy migration.
- **Successful catalog edits persist.** `input.imgid` commits requested edits
  after output success. `input.path` discards request state. `--read-only`
  refuses catalog edits and does not substitute scratch semantics.
- **Inspection and output without edits preserve history.** Engine initialization
  occurs on an isolated row; it does not materialize auto-applied history on the
  catalog source. Processing ignores ambient XMP and suppresses sidecar writes.
- **A crash mid-request can strand a scratch row**, since cleanup runs when the
  request finishes. It shows up as an unexpected image in the catalog.
- Not included: driving a live/open darktable GUI (this is a background worker).
