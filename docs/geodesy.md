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
