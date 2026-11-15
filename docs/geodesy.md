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
  top-left pixel, the COLMAP convention.

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
