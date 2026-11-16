# spirula-geo: how the fork sits on Spirula Studio

The fork adds a Survey screen (control points, photo marks, measuring,
drawing, export) without changing how Spirula Studio builds, trains, meshes or
edits. This page is the map for carrying it onto each new Spirula version.

## Branches

- `master` -- an untouched copy of upstream's `master`. Updated with GitHub's
  "Sync fork" button, never committed to.
- `geo` -- the fork: upstream plus the files below. Builds and releases come
  from here.
- `upstream-check` -- a trial branch written by `.github/workflows/upstream-check.yml`
  (weekly, or by hand): `geo` with the newest upstream merged in. A conflict
  opens an issue naming the files; a clean merge is built for Windows, and a
  failed build opens an issue too. `geo` itself is never touched by it.

## What is the fork's own

Everything under these paths is new; upstream has no file there, so an
upstream change can never conflict with it:

| Path | What |
|---|---|
| `src/app/gui/geo/` | the Survey screen: `SurveyScreen.cpp` (screen and menu, GuiApp members), `SurveySession.*` (tabs, tools, hook over the editor), `SurveyControl.cpp` (control points, photo marks, placement), `SurveyTools.cpp` (measure, draw, move, export), `GeoState.h` |
| `src/data/Geo*.{h,cpp}` | survey math and files: `GeoProject` (geo.json, catalogs), `GeoSolve` (triangulation, Sim3, residuals), `GeoCrs` (coordinate systems), `GeoVectors` (drawing, classifier, DXF/LAS/... writers) |
| `src/data/tests/geo_core_test.cpp` | tests of the above; also run on its own by CI before every build |
| `src/i18n/catalog/Geo.h` | the fork's interface text, English and Russian |
| `docs/geodesy.md`, `docs/fork.md` | the fork's documentation |
| `.github/workflows/geo-windows.yml`, `upstream-check.yml` | builds, releases, the upstream trial |

## Hook points in upstream files

Every line the fork adds to an upstream file carries a `spirula-geo` comment;
`git grep -n spirula-geo -- src cmake` lists them. After a merge from upstream,
these are the only places that can conflict or need a second look.

| File | Hook | Why |
|---|---|---|
| `cmake/SsApps.cmake` | `${SS_SRC}/app/gui/geo/*.cpp` in the GUI source glob | compiles the screen |
| `src/app/gui/GuiApp.h` | `Survey` in `enum class Screen`; `class SurveySession;`; the `_survey` member and three member declarations, after `_compare` | the screen is a GuiApp screen, so it reuses model opening, the log and the layout |
| `src/app/gui/GuiApp.cpp` | `case Screen::Survey: draw_survey()`; `draw_survey_menu()` in the menu bar; `survey_detach()` before `_compare.destroy_gl()` | showing the screen, the "Survey" menu, releasing GL in order |
| `src/app/gui/edit/EditSession.h` | `friend class SurveySession;`, `set_overlay_tool()`, `_overlay_tool`, one clause in `owns_left_button()` | the tools work through the editor's document, placement and saving |
| `src/app/gui/edit/EditSession.cpp` | the overlay tool sees `on_viewport_input` first; its overlay draws last in `draw_viewport_overlay` | pointer and drawing over the 3D view |

What the fork relies on from upstream, beyond the hooks, and so what to check
when upstream refactors it: `EditSession`'s private members used through the
friendship (`_doc`, `_panel`, `_tool`, `_save_*`, `_ask_overwrite`,
`place_saved`, `saved_to_shared`, `xform_frame`, `view`, `draw_panel`),
`ViewportPanel::request_pick` / `take_pick`, `CompareView::begin_edit` /
`edit()`, `resolve_marker_dataset`, `parse_dataset`, `load_picture`,
`meshing::write_mesh`, and the `ui::` wrappers.

## Carrying the fork to a new Spirula version

1. On GitHub, "Sync fork" on `master`.
2. Merge `master` into `geo` (`git merge master`). Conflicts can only be in the
   hook points above; keep upstream's change and put the hook back.
3. `git grep -n spirula-geo -- src cmake` shows every hook is still there.
4. Push. CI runs `geo_core_test`, then the Windows build and release.
5. Open a model on the Survey screen and try a placement, a measure, a drawn
   line and an export.

## Data that outlives versions

The fork keeps its own files beside a project, none of them in Spirula's
formats, so an upstream change cannot break them: `geo.json` (catalog,
marks, coordinate system, offset), `vectors.json` (the drawing),
`<model>.geo.json` (a saved model's offset), `crs.json` and
`geo_settings.json` (the user's coordinate systems and classifier path, in
the config directory). Each has a `version` key. docs/geodesy.md has the
formats.

## Rules the fork keeps

- No component under a non-commercial or non-free licence; the fork is GPL-3
  like upstream.
- No personal data built in: the classifier and the coordinate system are the
  user's choice; the built-in coordinate system catalog is public parameters.
- Interface text through `Geo.h`, English and Russian; the other languages
  show English.
- Upstream's `AGENTS.md` rules (comments, UI text, lints) hold in the fork's
  files too.
