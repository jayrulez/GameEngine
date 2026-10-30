# Model viewer guide (for agents)

The model viewer keeps a **library** of model files (glTF, GLB, FBX, OBJ) for a user picking and
organising models for their games. You work it through these tools; the user works the same
library through the viewer's window, and sees what you change as you change it. Every tool is one
of the viewer's commands: the window has nothing you lack.

Start with `viewer_info` (is the library writable, where it is), then `collection_tree` and
`tag_list` to see how the user organises things, before changing anything.

## The library

- An **entry** is a model file in the library: an id, its path, its tags, a note, and its numbers
  (rigged, clips, triangles) once the viewer has drawn it. `entry_add` makes one from a path (the
  existing one, when the file is there already) and returns its id; most tools take ids.
- A **collection** groups entries and nests: a game, its acts, a props list. An entry may be in
  any number of collections.
- A **member** is an entry in a collection, with the **name** it has there: the id the game will
  know it by (`hero`, `crate_small`). A name is unique, in any case, across a top-level collection
  and everything under it, so a game's ids never clash; two games may each have a `hero`. A rename
  that would clash fails and names the model holding the name.
- **Tags** are free text on entries (`boss`, `low poly`), matched in any case.
- Deleting a collection takes the collections under it and their memberships; it never removes
  an entry. `entry_remove` takes a model out of the library and every collection.

## Finding models

`search` takes a query whose terms must all hold:

- a word matches a member's name, the file's name, the note or a tag (`knight`);
- `tag:boss`, or `tag:"low poly"` for a tag with a space;
- `collection:act1`: a member of that collection or one under it;
- `rigged`, `animated`: what the viewer counted when it drew the model (unknown until then);
- `missing`: the file is no longer where the entry says (`entry_relink` points it at its new place).

Give a `scope` (a collection's id) to search that collection and those under it, in their order;
none searches the whole library.

## Changes, undo and the one writer

- Every change is written at once and can be undone: `history_list` shows the changes newest
  first, `history_undo` steps back over the last one (yours or the user's). Tell the user what you
  changed; they can undo it from the window too.
- Only one viewer writes a library. If `viewer_info` says it is read-only, another viewer holds it:
  work through that viewer's MCP port instead. Reads still work.
- A failed tool changed nothing.

## The tools

| Tool | What it does |
| --- | --- |
| `viewer_info` | The viewer: its process, its version, window or headless, where the library is, whether it is writable. |
| `entry_add` | Adds a model file to the library (its entry when it has one); returns its id. |
| `entry_remove` | Removes a model from the library, and from every collection it is in. |
| `entry_relink` | Points a model at its file's new place (or a changed file). |
| `entry_get` | A model: its file, tags, note, numbers, and the collections it is in with its name in each. |
| `entry_note_set` | Sets a model's note. |
| `collection_create` | Creates a collection, at the top or under another; returns its id. |
| `collection_rename` | Renames a collection. |
| `collection_move` | Moves a collection under another, or to the top; refused inside itself or where its names would clash. |
| `collection_delete` | Deletes a collection and those under it; the models stay. |
| `collection_tree` | The collections, nested, with their ids and member counts. |
| `collection_members` | A collection's models in order, with their names there (and those under it, by default). |
| `member_add` | Adds models to a collection. |
| `member_remove` | Takes models out of a collection. |
| `member_rename` | Names a model in a collection: the id a game knows it by. |
| `member_move` | Moves a model to another place in a collection's order. |
| `tag_add` | Tags models. |
| `tag_remove` | Takes a tag off models. |
| `tag_rename` | Renames a tag on every model that has it. |
| `tag_delete` | Takes a tag off every model. |
| `tag_list` | Every tag, with how many models have it. |
| `search` | Finds models: the query above, a scope, a limit. |
| `export_csv` | A collection as CSV (with the collections under it by default), returned or written to a file: collection path, name, file, tags, note. |
| `export_copy_files` | Copies a collection's model files into a folder (a subfolder per sub-collection), each renamed to its name there, with the images and buffers it needs; reports what was copied, what was missing and what was there already and different (never overwritten). |
| `history_undo` | Steps back over the last change. |
| `history_list` | The changes undo would step back over, the last first. |

The viewer's preferences and the cache (in a window or headless; the window applies a change at
once, except where it says "from the next start"):

| Tool | What it does |
| --- | --- |
| `settings_get` | The preferences: where screenshots go and their names, the thumbnail size, the cache folder and its size, the view's defaults, the density, the grids' tile size, how many recents are kept. |
| `settings_set` | Changes the preferences named, the rest as they were; refused, changing nothing, when one is out of its range. |
| `recents_list` | The models opened and the folders browsed lately, newest first. |
| `cache_clear` | Deletes the cooked models and the thumbnails (each made again when next needed); refused while a model is open in a tab. Ask the user first. |

The window's tools (a viewer started with `--headless` has no window, and says so):

| Tool | What it does |
| --- | --- |
| `model_open` | Opens a model file in a tab of its own (or brings its tab to the front); `ready` is false while its file is read and cooked. |
| `model_close` | Closes a model's tab (the one in front by default). |
| `tabs_list` | The model tabs: each one's place, file, whether it is in front and whether it is ready; and whether Browse or Library is. |
| `tabs_select` | Brings a tab to the front (-1: Browse, -2: Library, where the user sees the library). |
| `view_screenshot` | Saves the 3D view (no panels) as a PNG within a frame, by default in the screenshot folder named by the pattern; returns the file. |
| `view_camera` | Frames the model, or looks at it from a yaw and pitch in degrees. |
| `view_light` | Sets exposure, ambient, sky, the turntable and the skeleton overlay; returns them all. |
| `clip_list` | The clips of the model in front, and the playback. |
| `clip_select` | Plays a clip of the model in front, from its start. |
| `clip_play` | Plays or pauses, sets looping and speed. |
| `clip_scrub` | Shows a moment of the clip, paused. |
| `browse_folder` | Browses a folder: the Browse tab with every model under it. |
| `thumbnail_get` | A model's thumbnail file, or `queued` (it is drawn soon: ask again). |

To show the user something, open it (`model_open`), wait until `tabs_list` says it is `ready` (a
large file takes a moment; a file that does not open loses its tab), pick the clip and angle, and
take a `view_screenshot`; the file is written within a frame. The view and clip tools refuse a
model still cooking.

## Running it

- With the window: `Tools.ModelViewer --mcp` (or `--mcp-port <port>`; 7406 by default) serves these
  tools over HTTP at `http://127.0.0.1:<port>/mcp` with the bearer token in the user data folder's
  `mcp-token` (the editor's, shared).
- Without a window: `Tools.ModelViewer --headless` serves them over stdio, for organising a library
  from a terminal.
