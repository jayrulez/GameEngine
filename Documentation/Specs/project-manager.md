# The project manager

Status: P1 to P4 BUILT (2026-11-01); P5 to come. Agreed with the user over a mockup. Replaces the project manager of
editor-gui.md section 5.1 (layer 13) where they differ.

Goal: the screen the editor opens on without a project looks and behaves like a finished tool:
projects found and opened quickly, a project's state plain at a glance, a new project made without
surprises about its folder, and the sample games downloaded from GitHub straight into the list.

## What it is now

A narrow column: the logo, "engine 0.1.0", New Project and Open Folder, "Recent projects" with a
List/Grid toggle, a status line, and one row per recent project (picture, name, full path, the
project's engine version, Open, a menu). No search, sort or keyboard use; missing projects mixed in
with the rest; the list's only order is recency. New Project asks for a name and a parent folder
and checks nothing until Create.

## The design (agreed)

### The page

- **Header:** the logo; two tabs, **Projects** and **Samples**; the editor's version and a
  Documentation link on the right.
- **Projects toolbar:** New Project... (primary), Open Folder..., a search box (Ctrl+F) matching a
  project's name or folder, Sort (Last opened, Name), and List/Grid.
- **A row:** the picture; the name; the folder, shortened with `~` (the full path in its tooltip);
  when it was last opened, said relatively ("2 hours ago", "Yesterday", then a date), the exact
  time in its tooltip; the project's own engine version (muted when it is this editor's, the
  warning colour and a "Made with a newer engine" badge when newer, the info colour and "Upgrades
  when opened" when older); a star; Open; the menu.
- **Favourites:** the star pins a project; pinned projects come first whatever the sort.
- **Grid:** 16:9 picture cards (name, last opened, version, star, menu); clicking the picture opens.
- **Missing projects** have their own section under the rest: a folder icon marked broken, the
  name and old folder, **Locate...** (pick the folder where it is now) and **Remove from list**, and
  **Remove missing from list** for all of them. Nothing on this page deletes files.
- **Footer:** a status line (what just happened) and the shortcuts.
- **Keyboard:** no selection (ruled 2026-10-31: every action stays one click on its project); a
  focus ring that Up and Down move, Enter opens the focused project, Del removes it from the list;
  Ctrl+F searches, Ctrl+N is New Project, Ctrl+O is Open Folder.
- **A folder dropped** on the window opens that project, or says it holds no Project.xml.
- **First run** (no projects): "Welcome", one line on what the list is, and two large cards, New
  Project and Open Folder, each with a line on what it does and its shortcut.

### New Project

Name; Location with Browse...; **Create a folder named after the project in it** (on by default).
Under them one check line, kept current as the fields change, coloured by what it says:

- green: "Creates `<folder>`, and opens it";
- amber: the location does not exist yet (it will be made), or the name starts or ends with a
  space;
- red, Create disabled: no name, a name holding `/ \ : * ? " < > |`, no location, a target folder
  that exists and holds files (with the folder option off, the location itself must be empty).

The location starts where the last project was made or downloaded.

### Samples

One card per sample game of this editor's version: its picture, a one-line description that says
what it is (taken from the README; never by reference to another sample), its size, and
**Download...**. Download opens a dialog shaped like New Project (Location, Browse..., **Create a
folder named `<Sample>` in it**, the same check line, Cancel and Download). While it downloads, the
card shows a progress bar and Cancel (a cancelled download keeps nothing); when it is done, the
sample is one of the user's projects and the card says "In your projects". Samples are never
opened from this tab. With no network, the tab says it could not reach GitHub, with Retry.

Where they come from: each release (release.yml) attaches one zip per sample (its game: not
`Tools/`, `Cooked/`, `.cache/`, `Editor/` or `Dist/`) and a `samples.json` naming each sample's
name, description, picture, size, SHA-256 and folder. The editor reads the `samples.json` of the
release that matches its own engine version, downloads a zip over the engine's HTTP client
(HTTPS, redirects, the system's certificate roots), checks its SHA-256, and unpacks it.

## Phases

Each phase is one commit or a few, each with its tests, run in the editor and screenshotted before
it is handed to the user for the visual check.

- **P1. What the list stores.** A wall-clock time in Core (`sys` backends: Linux, Windows, the
  web), which Core lacks today. RecentProjectEntry gains `lastOpened` (Unix seconds, set by
  TouchRecentProject) and `favourite`; RecentProjectsSettings gains `sort` and `lastLocation`;
  each appended so older stores read. The controller gains favourite, sort, search, missing and
  locate operations. A relative-time formatter ("2 hours ago") with its tests.
- **P2. The page.** ProjectManager.xml and ProjectManagerView rebuilt to the design above: header
  and tabs (Samples empty until P5), toolbar, rows and tiles, the missing section, the footer,
  keyboard focus, folder drop, the first-run state. Built: the tabs stay on the first run, so the
  samples are one click away there; the search filters the missing section too; Documentation
  opens the docs site.
- **P3. New Project.** The folder option, the check line, the remembered location; the check
  itself a pure function with its tests (the dialog only shows it).
- **P4. What downloading needs.** A zip reader (deflate through zlib, vendored for it and for
  content compression later), with tests over zips made by the system's tools; the HTTP client's
  progress callback. Built: `foundation.zip` (stored and deflate through zlib, CRC-checked, a name
  outside the folder refusing the archive, no Zip64), and the HTTP client streams a response
  (`onHead`, `onBody`) so a download is written as it arrives and its progress is its bytes
  against the head's length.
- **P5. Samples.** release.yml packs the samples and `samples.json`; the Samples tab, its
  download dialog (P3's check), progress and cancel, the SHA-256 check, unpacking, and the entry
  in the list.
