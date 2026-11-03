# SourcePath P3 (inspector picker) + P4 (lint)

> ARCHIVED 2026-11-02, partly built; what it still owes is in Documentation/Plans/weekly_backlog.md
> ("Left over from the specs archived 2026-11-02"). Non-authoritative: the original spec, kept as
> the record of what was built and why; the code and the tests are the truth.

Size: S. Modules: `Code/Draconic/Foundation/Draconic.VFS` (SourcePath lives
here), editor inspector (`Draconic.Editor.Scene` InspectorView), plus a lint
pass across editor code.

## Context

The typed path plan (memory `path-type-plan`): P1+P2 SHIPPED - `SourcePath` is
a distinct type in draconic.vfs and `Asset::fileName` is typed (wire
compatible). Remaining:

- P3: the inspector renders SourcePath fields as a proper file picker (it was
  deferred to ride the reflection track's attribute plumbing).
- P4: a lint that finds raw `String` fields/params that semantically carry
  source paths and should be `SourcePath`.

## P3 - inspector picker

1. Reflection: ensure `SourcePath` is a reflected value type
   (DRACONIC_REFLECT_VALUE in an implementation unit) so InspectorView can
   dispatch on it like other leaf types.
2. InspectorView: add an explicit dispatch entry for SourcePath fields (RULE
   from memory `inspector-ref-picker-table`: without an explicit entry, NO
   editor renders - this bit resource::Ref<T> before, fixed 8e9b9ff). Render:
   read-only text of the current path + a "..." button opening the existing
   editor file-picker dialog rooted at the project's source dir; picking writes
   back through the reflection setter so undo/redo and dirty-marking work like
   every other inspector edit.
3. Filter: if the field's declaring component/asset exposes an extension-filter
   attribute, respect it; otherwise all files.

## P4 - lint

1. A standalone script `Tools/lint-source-paths.py` (or a doctest-style repo
   test if a lint precedent exists in-repo - check first, follow precedent):
   greps reflected fields and Asset-derived classes for `String` members whose
   names match (fileName|filePath|sourcePath|.*Path) and are not SourcePath /
   not on an allowlist. Failing entries are printed with file:line.
2. Seed the allowlist with the legitimately-String cases found during
   implementation (URLs, virtual scheme paths, display strings). Keep it in
   the lint file, commented per entry.
3. Wire it into the build only if a lint-on-build precedent exists; otherwise
   it is run manually + noted in CONVENTIONS (do not invent CI - the project
   deliberately has none).

## Tests

- P3: inspector dispatch test in the editor test target that mirrors the
  existing Ref<T>-picker coverage: a reflected test component with a
  SourcePath field renders a picker row (the InspectorView tests have the
  precedent pattern).
- Round-trip: setting the field through the reflection path marks the asset
  dirty and survives save/load of the asset (XML source form).
- P4: the lint runs clean on the current tree at land time.

## Acceptance

- A SourcePath field on any component/asset shows a working picker in the
  editor with no per-type editor code beyond the single dispatch entry.
- Lint reports zero violations at land; adding a `String scenePath` reflected
  field makes it fail (demonstrated in the PR description, not committed).

---

## State (appended 2026-08-03; original content above is unchanged)

**PARTIAL / GATED.** P1+P2 shipped (typed SourcePath in vfs; Asset::fileName typed).
P3 (inspector ref-picker for SourcePath) rides the reflection track's attribute
plumbing, so it is BLOCKED on reflection-track (the Nested mechanism / attribute
work, in progress now). P4 (lint) pending.
