// geo_core_test.cpp -- spirula-geo's survey math and files: coordinate systems
// against PROJ's own numbers, project and drawing round trips, the writers.
#include "data/GeoCrs.h"
#include "data/GeoProject.h"
#include "data/GeoVectors.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using namespace spirula::geo;

namespace {

int g_failures = 0;

void check(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) g_failures++;
}

struct Ref {
    const char* def;
    double lat, lon, north, east;   // PROJ 9: EPSG:4326 -> def, always_xy
};

// Produced once with pyproj; the plan must agree to a millimetre.
const Ref kRefs[] = {
    {"+proj=tmerc +lat_0=0 +lon_0=44.55 +k=1 +x_0=2250000 +y_0=-5714743.504 +ellps=krass "
     "+towgs84=23.57,-140.95,-79.8,0,0.35,0.79,-0.22 +units=m",
     56.3269, 44.0059, 530441.7641, 2216449.1046},
    // PROJ's own pick for EPSG:28408 differs by 0.7 m: the reference is its
    // tmerc with the GOST R 51794-2008 shift, which is what the table uses.
    {"EPSG:28408", 55.75, 37.62, 6205521.3477, 8037154.4677},
    {"EPSG:32637", 55.75, 37.62, 6179118.1468, 413380.7203},
    {"EPSG:2154", 48.8566, 2.3522, 6862035.2594, 652469.0227},
    {"EPSG:2180", 52.2297, 21.0122, 486757.2095, 637382.2044},
};

}  // namespace

int main() {
    const fs::path dir = fs::temp_directory_path() / "spirula_geo_core_test";
    fs::create_directories(dir);
    std::string err;

    // ---- 1. Coordinate systems ----
    for (const Ref& r : kRefs) {
        Crs c;
        const bool parsed = parse_crs(r.def, c, err);
        check(parsed, std::string("parse ") + std::string(r.def).substr(0, 24));
        if (!parsed) continue;
        double x, y, lat, lon;
        wgs84_to_crs(c, r.lat, r.lon, x, y);
        check(std::hypot(x - r.north, y - r.east) < 1e-3, "forward to 1 mm");
        crs_to_wgs84(c, r.north, r.east, lat, lon);
        check(std::hypot((lat - r.lat) * 111e3, (lon - r.lon) * 111e3 * std::cos(r.lat * 0.0174533)) < 1e-3,
              "inverse to 1 mm");
        // The WKT written for .prj reads back as the same system.
        Crs w;
        check(parse_crs(crs_to_wkt(c, "t"), w, err), "WKT round trip parses");
        double x2, y2;
        wgs84_to_crs(w, r.lat, r.lon, x2, y2);
        check(std::hypot(x2 - x, y2 - y) < 1e-4, "WKT round trip agrees");
    }
    {
        Crs c;
        check(!parse_crs("+proj=merc +datum=WGS84", c, err), "an unsupported projection is refused");
        check(!parse_crs("EPSG:99999", c, err), "an unknown EPSG code is refused");
    }

    // ---- 2. Catalog import and the project file ----
    {
        const std::string cat = (dir / "cat.txt").u8string();
        std::ofstream(cat) << "name;x;y;h\nP1;530441,764;2216449,105;101,25\nP2\t530500.1\t2216500.2\t99\n";
        GeoProject p;
        check(import_catalog(cat, p, err) == 2, "catalog: two points, decimal comma and tab");
        check(p.find("P1") && std::fabs(p.find("P1")->x - 530441.764) < 1e-9, "catalog: P1 x");
        p.placed = true;
        p.offset[0] = 2216400;
        p.model_frame = true;
        p.file_from_dataset.s = 2.5;
        p.crs_name = "MSK";
        const std::string gj = (dir / "geo.json").u8string();
        check(save_geo(gj, p, err), "geo.json written");
        GeoProject q;
        check(load_geo(gj, q, err), "geo.json read");
        check(q.placed && q.offset[0] == 2216400 && q.model_frame && q.file_from_dataset.s == 2.5 &&
                  q.points.size() == 2 && q.crs_name == "MSK",
              "geo.json round trip");
    }

    // ---- 3. Drawing ----
    {
        std::vector<Feature> f(1);
        f[0].kind = FeatureKind::Polygon;
        f[0].code = "3";
        f[0].name = "B1";
        f[0].v = {{100, 200, 1}, {100, 210, 1}, {110, 210, 2}, {110, 200, 2}};
        const std::string vj = (dir / "vectors.json").u8string();
        check(save_vectors(vj, f, true, err), "vectors.json written");
        std::vector<Feature> g;
        bool survey = false;
        check(load_vectors(vj, g, survey, err) && survey && g.size() == 1 && g[0].v.size() == 4,
              "vectors.json round trip");
        check(std::fabs(plan_area(f[0].v) - 100.0) < 1e-9, "plan area");
        check(std::fabs(plan_length(f[0].v, true) - 40.0) < 1e-9, "closed plan length");

        std::vector<OutFeature> out(1);
        out[0].kind = FeatureKind::Polygon;
        out[0].layer = "Buildings";
        out[0].xyh = f[0].v;
        const std::string dxf = (dir / "plan.dxf").u8string();
        check(write_dxf(dxf, out, DxfOptions{}, err), "DXF written");
        std::vector<ClassCode> layers;
        check(load_classifier(dxf, layers, err) && layers.size() == 1 && layers[0].layer == "Buildings",
              "DXF layers read back as a classifier");
    }

    // ---- 4. Pickets of a DXF sketch ----
    {
        const std::string dxf = (dir / "sketch.dxf").u8string();
        std::ofstream(dxf) << "0\nSECTION\n2\nENTITIES\n"
                              "0\nPOINT\n8\nPK\n10\n2216449.10\n20\n530441.76\n30\n0.0\n"
                              "0\nTEXT\n8\nPK\n10\n2216449.40\n20\n530442.00\n40\n0.5\n1\npt5(pole)\n"
                              "0\nTEXT\n8\nPK\n10\n2216449.40\n20\n530441.20\n40\n0.5\n1\n101,25\n"
                              "0\nPOINT\n8\nPK\n10\n2216500.00\n20\n530500.00\n30\n99.5\n"
                              "0\nENDSEC\n0\nEOF\n";
        std::vector<Picket> pk;
        check(read_dxf_pickets(dxf, pk, err) && pk.size() == 2, "DXF pickets: two points");
        if (pk.size() == 2) {
            check(pk[0].name == "pt5" && pk[0].code == "pole" && pk[0].has_h && std::fabs(pk[0].h - 101.25) < 1e-9,
                  "DXF pickets: name, code and height from the labels");
            check(std::fabs(pk[0].x - 530441.76) < 1e-6 && std::fabs(pk[0].y - 2216449.10) < 1e-6,
                  "DXF pickets: x is north, y is east");
            check(pk[1].name.empty() && pk[1].has_h && pk[1].h == 99.5, "DXF pickets: height from the point");
        }
    }

    // ---- 5. Model points in full coordinates ----
    {
        const float xyz[] = {1.234f, 2.345f, 3.456f};
        PointsOut p;
        p.xyz = xyz;
        p.n = 1;
        p.offset[0] = 2216400;
        p.offset[1] = 530400;
        const std::string las = (dir / "m.las").u8string();
        check(write_las(las, p, err), "LAS written");
        check(fs::file_size(fs::u8path(las)) == 227 + 26, "LAS: header and one record");
    }

    fs::remove_all(dir);
    std::printf("%s\n", g_failures ? "FAILED" : "all passed");
    return g_failures ? 1 : 0;
}
