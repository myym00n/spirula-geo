// GeoCrs.h -- plane survey coordinate systems: a built-in catalog plus systems
// the user adds as a PROJ string, WKT (.prj) or EPSG code. Transverse Mercator,
// UTM and Lambert conic, with a 7-parameter shift to WGS 84. Plan only:
// heights pass through, there is no geoid. docs/geodesy.md.
#pragma once

#include <string>
#include <vector>

namespace spirula {
namespace geo {

struct CrsEntry {
    std::string name;
    std::string def;    // as the user gave it: PROJ, WKT or EPSG:n
};

struct Crs {
    enum class Kind { Geographic, TransverseMercator, LambertConic };
    Kind kind = Kind::Geographic;
    double a = 6378137.0, f = 1.0 / 298.257223563;
    double lat0 = 0, lon0 = 0, lat1 = 0, lat2 = 0;   // degrees; lat1/2 LCC only
    double k0 = 1, x0 = 0, y0 = 0;                   // false easting, northing
    double to_meter = 1;
    // PROJ +towgs84 order and sense (position vector): m, arcsec, ppm.
    double towgs84[7] = {0, 0, 0, 0, 0, 0, 0};
    std::string datum, ellipsoid;                    // names, for WKT out
};

bool parse_crs(const std::string& def, Crs& out, std::string& error);

// x north, y east in the system's units <-> WGS 84 degrees.
void crs_to_wgs84(const Crs& c, double x, double y, double& lat, double& lon);
void wgs84_to_crs(const Crs& c, double lat, double lon, double& x, double& y);

std::string crs_to_wkt(const Crs& c, const std::string& name);   // ESRI .prj

const std::vector<CrsEntry>& builtin_crs();
bool load_crs_list(const std::string& path, std::vector<CrsEntry>& out, std::string& error);
bool save_crs_list(const std::string& path, const std::vector<CrsEntry>& list, std::string& error);

}  // namespace geo
}  // namespace spirula
