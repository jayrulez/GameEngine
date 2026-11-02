# The import window

Status: AGREED 2026-11-01 over a mockup (the model, image and sound views of one three-file drop);
W1 to W7 BUILT 2026-11-02 (the week's notes, Plans/week-2026-10-24.md, list the commits and what
was left out).
Continues Plans/import-workflow.md, whose P1 to P5 built today's review window (prepare, review,
commit; one window per drop; per-resource choice and names; re-import memory). Its open items were
previews and richer settings, and its rulings stand: previews are live, not rasterized pictures;
lists and checkboxes, no filter grids.

Goal: what is about to be imported can be seen before it is, and set where it is seen. Today the
window is a file list beside a column of checkboxes and names: nothing shows the model, the
picture or the sound, and an importer can offer only on/off toggles.

## What it is now

Editor.App's ImportDialog, one window for every import (a drop, the Assets panel's Import..., a
picker's Import...): the destination group at the top; with several files, a list of them with a
state badge; the selected file's importer's toggles; the resources it would make, by kind, each
with a checkbox and a name. Every importer describes its file (DescribeImport); the window lays
out what it is given.

## The design (agreed)

The window stays generic: nothing in it names a file type. Each part comes from the importer the
file resolves to, or from what its domain registers.

- **Three panes.** Files on the left, the selected file's preview in the middle, its settings on
  the right; the destination ("Into") and the count in the footer, beside Cancel and
  "Import N Files". Larger than today and resizable.
- **Files, grouped by importer.** One section per importer the drop's files resolve to, titled by
  the importer's own Label() ("Model", "Texture", "Audio") with its count. Each file: its checkbox,
  its asset type's icon, its name, its size, and its state when it needs attention (a choice to
  make, a re-import). Changing a file's "Import as" moves it to that importer's section.
- **Preview, from a registry.** A domain registers a preview for the asset types it imports, as it
  registers pages. The window asks for the selected file's preview and shows it, with a line of
  facts under it that the preview supplies. A type with no preview shows its facts alone.
  - A model: a live 3D view of the source (the model viewer's loading into a PreviewViewport):
    orbit, zoom, frame; the skeleton and wireframe; a clip picker, play and a timeline. Facts:
    meshes, triangles, materials, textures, bones, clips, size.
  - An image: the picture over a checkerboard; the channels (RGB, R, G, B, A); fit, 1:1 and zoom;
    the pixel under the pointer (ImageInspectView). Facts: size, channels and depth, alpha, mip
    levels.
  - A sound: the waveform with a playhead and play (WaveformView); the parts the settings cut,
    shaded; "Hear it as imported" plays it with the settings applied. Facts: length, channels,
    rate, depth, peak.
  - A font: a sample line in it.
- **Settings, the importer's own.** "Import as" (the importers that claim the extension), then
  what the importer declares, drawn by the property editors the inspector uses: choices and
  numbers, not only toggles. A setting may say what it will do to this file ("Cuts 0.19 s from
  the end", "+3.1 dB"); one that cannot apply is shown disabled with why ("It is mono already").
  Then what the file makes, by kind, collapsed to "13 of 13", each kind opening to its items with
  a checkbox and a name; an item the preview can show (a clip) has its play. "Use these for every
  <label> in this import" copies the settings to the drop's other files of that importer.
- **New settings, where the importer has them:** a model's scale and up axis; a texture's usage
  (Color, Normal map, Mask, HDR: guessed from the file's name, with the reason shown), colour
  space, compression, wrap and mipmaps, which the texture asset has today but only its page sets.
- **Reimport** from an asset's page opens the same window on that asset's file, with the settings
  and selection it was imported with.

## Phases

Each phase is one commit or a few, each with its tests, run in the editor and screenshotted before
it is handed to the user for the visual check.

- **W1. The layout.** The three panes, the files grouped by importer label, the footer, the size;
  today's settings (toggles, resources by kind, collapsed, with names) in the right pane; an empty
  preview pane that shows the file's name and size until W2.
- **W2. The preview registry and the image preview.** The registration (editor.core, keyed by asset
  type), the window hosting it, the facts line; the image preview over ImageInspectView for the
  importers that make pictures (texture, image, heightfield, splatmap, vegetation mask).
- **W3. The sound preview.** WaveformView with play and the playhead; the facts.
- **W4. The model preview.** The source loaded as the model viewer loads it, into a PreviewViewport
  in the window; clips, skeleton, wireframe; the facts.
- **W5. Settings an importer declares.** ImportOptions declares reflected settings, drawn by the
  property editors; toggles become one kind of them. A setting's effect line and its disabled
  reason. "Use these for every <label>".
- **W6. The new settings.** Model scale and up axis (the importer applies them); the texture's
  settings at import, with usage guessed from the name; the sound's trim and normalize shown on the
  waveform and in "Hear it as imported".
- **W7. Reimport.** From an asset's page, the window on its file with what it was imported with.
