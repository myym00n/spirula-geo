// GeoProject.h -- survey control for a dataset: a catalog of points in a plane
// survey system (x north, y east, h up) and where each is marked on the photos.
// Lives beside the dataset as geo.json; docs/geodesy.md.
#pragma once

#include <map>
#include <string>
#include <vector>

namespace spirula {
namespace geo {

struct Crs;

enum class Role { Control = 0, Check, Off };

struct GeoPoint {
    std::string name;
    double x = 0.0, y = 0.0, h = 0.0;   // north, east, height; metres
    bool has_h = true;
    Role role = Role::Control;
    std::string code;
};

// Pixel position on the image as stored, origin at the top-left CORNER of the
// top-left pixel (COLMAP / Metashape convention).
struct GeoMark {
    double u = 0.0, v = 0.0;
};

struct GeoProject {
    std::vector<GeoPoint> points;
    // point name -> image file name (no directory) -> mark
    std::map<std::string, std::map<std::string, GeoMark>> marks;
    std::string crs_name, crs_def;      // catalog entry; see GeoCrs.h
    // Set once the model has been placed: saved coordinates + offset = (east,
    // north, up) in the survey frame.
    bool placed = false;
    double offset[3] = {0, 0, 0};

    GeoPoint* find(const std::string& name);
    const GeoPoint* find(const std::string& name) const;
    int mark_count(const std::string& name) const;
    void remove_point(const std::string& name);
};

std::string geo_path(const std::string& dataset_dir);
bool read_text(const std::string& path, std::string& out, std::string& error);   // drops a BOM
bool write_text(const std::string& path, const std::string& text, std::string& error);   // atomic
bool load_geo(const std::string& path, GeoProject& out, std::string& error);
bool save_geo(const std::string& path, const GeoProject& p, std::string& error);

// "name x y h [code]" per line; space, tab, ';' or ',' between fields, decimal
// comma accepted when ';' or tab separates. Returns points added or updated.
// With `latlon_into`, x y are WGS 84 latitude and longitude, converted into it.
int import_catalog(const std::string& path, GeoProject& p, std::string& error,
                   const Crs* latlon_into = nullptr);
// gcps + observations from a mooncloud project.json.
int import_mooncloud(const std::string& path, GeoProject& p, std::string& error);
bool export_points_csv(const std::string& path,
                       const std::vector<GeoPoint>& pts, std::string& error);

const char* role_key(Role r);
Role role_from_key(const std::string& k);

}  // namespace geo
}  // namespace spirula
