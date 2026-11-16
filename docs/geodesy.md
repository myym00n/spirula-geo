# Geodesy: survey control, coordinates and drawing

This fork adds survey work to the editor: a catalog of control points in a
plane survey system (x north, y east, h up, metres), their marks on the
photos, one similarity fit from the model to the survey frame, and the
residuals that say how good it is.

## Files

`geo.json` sits in the dataset directory:

- `points`: name, x, y, h (null when unknown), role (`control`, `check`,
  `off`), optional code;
- `marks`: point name -> image file name -> `[u, v]`, pixels of the image as
  displayed (EXIF quarter turns applied), origin at the top-left corner of the
  top-left pixel, the COLMAP convention;
- `crs` and `crs_def`: the coordinate system's name and definition, empty when
  none is chosen;
- `offset`: written once the model has been placed (see Frames).

A mooncloud `project.json` imports directly: its `gcps` become points and its
`observations` become marks, matched by image file name.

## Frames

Each marked point is triangulated in the dataset's camera frame and carried
into the editor's saved coordinates. Control points with a height fix one
`Sim3` by Umeyama. Survey coordinates are millions of metres, so the model is
placed at the survey frame minus a round `offset` (to 100 m): splat positions
stay float32 with millimetre resolution, and every coordinate shown or
exported adds the offset back in double. The local frame is (east, north, up).

## Residuals

For every triangulated point the computed survey coordinates minus the
catalog's: plan (dx, dy) and height (dh). RMS is reported separately for
control points (what the fit used) and check points (what it did not), and
each point carries its worst reprojection error and the image it came from.

## Coordinate systems

Placement uses the control points alone: a catalog in any plane system gives
a model in that system, and a DXF from it lands in place in CAD with no
system named. A system is needed only to go between the plane and latitude /
longitude (a catalog measured in WGS 84, the check line under the chooser)
and for the `.prj` written beside exports.

The chooser lists a built-in catalog (MSK-52 zone 2, Pulkovo 1942
Gauss-Kruger zones, WGS 84 and ETRS89 UTM, a few national grids) and the
user's own systems, kept in `crs.json` in the app's config directory. A
system is added as a PROJ string, WKT 1 or 2 (a `.prj` file) or an EPSG code
the built-in table knows. Supported: Transverse Mercator (`tmerc`, `utm`) and
Lambert conformal conic (`lcc`), with a 3- or 7-parameter `towgs84` in PROJ's
position-vector sense. No grid shifts and no geoid: heights pass through
unchanged. Against PROJ the plan agrees to under a millimetre.

## The Survey screen

The "Survey" menu opens a model on its own screen: the same 3D view as the
viewer, with mooncloud's pages and tools around it. Spirula's own screens and
editor are unchanged (docs/fork.md).

- 1. Control points: the catalog, the coordinate system, placement and
  residuals.
- 2. Points on photos: the photo with its marks; click to mark, drag a mark
  to move it, a loupe under the pointer, Del removes the mark.
- 3. Plan, measurements, contours: the classifier, the drawn features.
- 4. 3D model: the tools -- View (a click reads coordinates), Measure, Point,
  Move, Contour, and Spirula's own Select and Clean with its panel and save.
  Trunk and Surface are still to come.
- Basemap: still to come.
- 5. Report and export: the model as LAS, PLY (double) or text points, or a
  mesh as OBJ, in full coordinates; the drawing as DXF (R12, layers from the
  classifier), CSV, GeoJSON or KML. A `.prj` is written beside when a system
  is chosen.

Points under the pointer come from the view's own depth pick, so the cursor
follows the surface that is drawn. A drawn vertex within ten pixels wins over
the surface. Moving a vertex shows it at its new place until Done (or Enter);
picking another vertex first asks whether to keep the move.

The drawing is `vectors.json` beside `geo.json` (or `<model>.vectors.json`
without a dataset). Its vertices are survey coordinates (x north, y east, h)
once the model is placed, so moving or re-saving the model does not move the
drawing. A drawing made before the first placement is carried along by it.

### Classifier

A file the user loads; the path is remembered in `geo_settings.json` in the
config directory. CSV or text, one code per line:

    code;name;kind;layer;colour
    1;Pole;point;Poles;1
    2;Kerb;line;Kerbs;#00ff00
    3;Building;polygon;Buildings;5

`kind` is point, line or polygon (Russian words work too); `colour` is an
AutoCAD colour index or `#rrggbb`. A DXF can be loaded instead: each of its
layers becomes a code with the layer's colour.

### A placed model saved without its dataset

Saving a placed model writes `<model>.geo.json` beside it: the offset, the
coordinate system and, for a trained model, where the dataset sits in the
saved file (the run's `scene_transform.json` still describes the file before
the move). Opening the model reads it back.
