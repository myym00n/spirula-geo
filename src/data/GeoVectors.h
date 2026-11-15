// GeoVectors.h -- what is drawn on the model: points, lines and polygons with
// a code from the user's classifier, and the writers that take them, and the
// model itself, out in full survey coordinates. docs/geodesy.md.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace spirula {
namespace geo {

struct Crs;

enum class FeatureKind { Point = 0, Line, Polygon };

// Vertices are survey coordinates (x north, y east, h) once the model is
// placed, so a re-saved or moved model does not take the drawing with it;
// before that, the model's own coordinates in the same order.
struct Feature {
    FeatureKind kind = FeatureKind::Point;
    std::string code, name;
    std::vector<std::array<double, 3>> v;
};

struct ClassCode {
    std::string code, name, layer;
    FeatureKind kind = FeatureKind::Point;
    int aci = 7;                       // AutoCAD colour index
};

std::string vectors_path(const std::string& dataset_dir, const std::string& model_path);
// `survey`: whether the vertices are survey coordinates yet.
bool load_vectors(const std::string& path, std::vector<Feature>& out, bool& survey, std::string& error);
bool save_vectors(const std::string& path, const std::vector<Feature>& f, bool survey, std::string& error);

// CSV or text, one code per line: code;name;kind;layer;colour. kind is point,
// line or polygon (Russian words too); colour an ACI number or #rrggbb. A DXF
// gives its layer table instead, each layer a code of any kind.
bool load_classifier(const std::string& path, std::vector<ClassCode>& out, std::string& error);
const ClassCode* find_code(const std::vector<ClassCode>& c, const std::string& code);
FeatureKind kind_from_key(const std::string& k, FeatureKind def);
const char* kind_key(FeatureKind k);

// A feature as written: survey coordinates, x north, y east, h.
struct OutFeature {
    FeatureKind kind = FeatureKind::Point;
    std::string code, name, layer;
    int aci = 7;
    std::vector<std::array<double, 3>> xyh;
};

struct DxfOptions {
    bool height_labels = true;         // point heights as text beside the point
    bool name_labels = true;
    double text_height = 0.5;          // drawing units (m)
};
// AutoCAD R12 ASCII: X east, Y north, Z height. Non-ASCII text in cp1251.
bool write_dxf(const std::string& path, const std::vector<OutFeature>& f,
               const DxfOptions& o, std::string& error);
// name;x;y;h;code per vertex; lines and polygons numbered name.1, name.2...
bool write_vectors_csv(const std::string& path, const std::vector<OutFeature>& f, std::string& error);
// With a coordinate system, WGS 84 longitude/latitude; without one, null.
bool write_geojson(const std::string& path, const std::vector<OutFeature>& f, const Crs* crs,
                   std::string& error);
bool write_kml(const std::string& path, const std::vector<OutFeature>& f, const Crs& crs,
               std::string& error);

// Model points in local coordinates (east, north, up) plus `offset` added in
// double. rgb may be empty.
struct PointsOut {
    const float* xyz = nullptr;        // local, float
    const uint8_t* rgb = nullptr;
    const uint8_t* keep = nullptr;     // null = all
    int64_t n = 0;
    double offset[3] = {0, 0, 0};
};
bool write_las(const std::string& path, const PointsOut& p, std::string& error);
bool write_ply_double(const std::string& path, const PointsOut& p, std::string& error);
bool write_xyz(const std::string& path, const PointsOut& p, std::string& error);

// Adds `offset` to every "v" line of an OBJ, in place.
bool obj_add_offset(const std::string& path, const double offset[3], std::string& error);

// Plan length, 3D length, plan area of a closed ring (shoelace), on x north /
// y east / h triples.
double plan_length(const std::vector<std::array<double, 3>>& v, bool closed);
double slope_length(const std::vector<std::array<double, 3>>& v, bool closed);
double plan_area(const std::vector<std::array<double, 3>>& v);

}  // namespace geo
}  // namespace spirula
