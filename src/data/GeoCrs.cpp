#include "data/GeoCrs.h"

#include "data/GeoProject.h"
#include "data/Json.h"
#include "data/JsonWrite.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>

namespace spirula {
namespace geo {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kDeg = kPi / 180.0;
constexpr double kSec = kDeg / 3600.0;

const char* const kPulkovoShift = "23.92,-141.27,-80.9,0,0.35,0.82,-0.12";   // GOST R 51794-2008
const char* const kDhdnShift = "598.1,73.7,418.2,0.202,0.045,-2.455,6.7";

struct Ellipsoid {
    const char* key;
    const char* name;
    double a, rf;
};
const Ellipsoid kEllipsoids[] = {
    {"wgs84", "WGS 84", 6378137.0, 298.257223563},
    {"grs80", "GRS 1980", 6378137.0, 298.257222101},
    {"krass", "Krassowsky 1940", 6378245.0, 298.3},
    {"bessel", "Bessel 1841", 6377397.155, 299.1528128},
    {"intl", "International 1924", 6378388.0, 297.0},
    {"clrk66", "Clarke 1866", 6378206.4, 294.9786982139},
    {"clrk80", "Clarke 1880 (RGS)", 6378249.145, 293.465},
    {"airy", "Airy 1830", 6377563.396, 299.3249646},
    {"gsk2011", "GSK-2011", 6378136.5, 298.2564151},
    {"pz90", "PZ-90", 6378136.0, 298.257839303},
};

std::string lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

// "Latitude of natural origin" and "latitude_of_origin" compare equal.
std::string squash(const std::string& s) {
    std::string out;
    for (char c : s)
        if (std::isalnum((unsigned char)c)) out += (char)std::tolower((unsigned char)c);
    return out;
}

bool to_num(const std::string& s, double& v) {
    if (s.empty()) return false;
    char* end = nullptr;
    v = std::strtod(s.c_str(), &end);
    return end && *end == 0 && std::isfinite(v);
}

const Ellipsoid* ellipsoid_by_key(const std::string& k) {
    const std::string q = squash(k);
    for (const auto& e : kEllipsoids)
        if (q == e.key || q == squash(e.name)) return &e;
    if (q == "krassowsky1940" || q == "krasovsky1940") return &kEllipsoids[2];
    return nullptr;
}

void set_ellipsoid(Crs& c, double a, double rf, const std::string& name) {
    c.a = a;
    c.f = rf > 0 ? 1.0 / rf : 0.0;
    c.ellipsoid = name;
}

bool parse_shift(const std::string& list, Crs& c) {
    double v[7] = {0, 0, 0, 0, 0, 0, 0};
    std::stringstream ss(list);
    std::string item;
    int n = 0;
    while (std::getline(ss, item, ',')) {
        if (n >= 7 || !to_num(item, v[n])) return false;
        n++;
    }
    if (n != 3 && n != 7) return false;
    std::copy(v, v + 7, c.towgs84);
    return true;
}

// WKT without TOWGS84 names its datum only; the common ones get their shift.
void shift_from_datum_name(Crs& c) {
    for (double t : c.towgs84)
        if (t != 0) return;
    const std::string d = squash(c.datum);
    if (d.find("pulkovo1942") != std::string::npos) parse_shift(kPulkovoShift, c);
    else if (d.find("dhdn") != std::string::npos || d.find("deutscheshauptdreiecksnetz") != std::string::npos)
        parse_shift(kDhdnShift, c);
}

// ---- PROJ strings ----

bool parse_proj(const std::string& def, Crs& c, std::string& error);

bool parse_epsg(int code, Crs& c, std::string& error) {
    char buf[256];
    const char* d = nullptr;
    if (code == 4326) {
        d = "+proj=longlat +datum=WGS84";
    } else if (code == 4284) {
        std::snprintf(buf, sizeof buf, "+proj=longlat +ellps=krass +towgs84=%s", kPulkovoShift);
        d = buf;
    } else if (code >= 28402 && code <= 28432) {
        const int z = code - 28400;
        std::snprintf(buf, sizeof buf, "+proj=tmerc +lon_0=%d +x_0=%d500000 +ellps=krass +towgs84=%s",
                      z * 6 - 3, z, kPulkovoShift);
        d = buf;
    } else if ((code >= 32601 && code <= 32660) || (code >= 32701 && code <= 32760)) {
        std::snprintf(buf, sizeof buf, "+proj=utm +zone=%d %s+datum=WGS84", code % 100,
                      code > 32700 ? "+south " : "");
        d = buf;
    } else if (code >= 25828 && code <= 25838) {
        std::snprintf(buf, sizeof buf, "+proj=utm +zone=%d +ellps=GRS80", code - 25800);
        d = buf;
    } else if (code >= 31466 && code <= 31469) {
        const int z = code - 31464;
        std::snprintf(buf, sizeof buf, "+proj=tmerc +lon_0=%d +x_0=%d500000 +ellps=bessel +towgs84=%s",
                      z * 3, z, kDhdnShift);
        d = buf;
    } else if (code == 2154) {
        d = "+proj=lcc +lat_0=46.5 +lon_0=3 +lat_1=49 +lat_2=44 +x_0=700000 +y_0=6600000 +ellps=GRS80";
    } else if (code == 2180) {
        d = "+proj=tmerc +lon_0=19 +k=0.9993 +x_0=500000 +y_0=-5300000 +ellps=GRS80";
    }
    if (!d) {
        error = "EPSG:" + std::to_string(code) + " is not built in; paste its PROJ string or WKT";
        return false;
    }
    return parse_proj(d, c, error);
}

bool parse_proj(const std::string& def, Crs& c, std::string& error) {
    std::map<std::string, std::string> kv;
    std::stringstream ss(def);
    std::string tok;
    while (ss >> tok) {
        if (tok[0] != '+') {
            error = "unexpected '" + tok + "'";
            return false;
        }
        const size_t eq = tok.find('=');
        kv[lower(tok.substr(1, eq == std::string::npos ? std::string::npos : eq - 1))] =
            eq == std::string::npos ? "" : tok.substr(eq + 1);
    }
    if (auto it = kv.find("init"); it != kv.end()) {
        const std::string v = lower(it->second);
        if (v.rfind("epsg:", 0) == 0) return parse_epsg(std::atoi(v.c_str() + 5), c, error);
    }
    auto num = [&](const char* k, double& v) {
        auto it = kv.find(k);
        if (it == kv.end()) return true;
        if (to_num(it->second, v)) return true;
        error = std::string("+") + k + " is not a number";
        return false;
    };
    const std::string proj = lower(kv["proj"]);
    if (proj == "tmerc" || proj == "etmerc" || proj == "utm") c.kind = Crs::Kind::TransverseMercator;
    else if (proj == "lcc") c.kind = Crs::Kind::LambertConic;
    else if (proj == "longlat" || proj == "latlong" || proj == "lonlat" || proj == "latlon")
        c.kind = Crs::Kind::Geographic;
    else {
        error = proj.empty() ? "no +proj" : "projection '" + proj + "' is not supported (tmerc, utm, lcc)";
        return false;
    }

    set_ellipsoid(c, 6378137.0, 298.257223563, "WGS 84");
    c.datum = "WGS_1984";
    if (auto it = kv.find("datum"); it != kv.end()) {
        const std::string d = lower(it->second);
        if (d == "wgs84") {
        } else if (d == "nad83") {
            set_ellipsoid(c, 6378137.0, 298.257222101, "GRS 1980");
            c.datum = "NAD83";
        } else if (d == "potsdam") {
            set_ellipsoid(c, 6377397.155, 299.1528128, "Bessel 1841");
            parse_shift(kDhdnShift, c);
            c.datum = "DHDN";
        } else {
            error = "datum '" + it->second + "' is unknown; give +ellps and +towgs84";
            return false;
        }
    }
    if (auto it = kv.find("ellps"); it != kv.end()) {
        const Ellipsoid* e = ellipsoid_by_key(it->second);
        if (!e) {
            error = "ellipsoid '" + it->second + "' is unknown; give +a and +rf";
            return false;
        }
        set_ellipsoid(c, e->a, e->rf, e->name);
        c.datum = std::string("D_") + e->name;
    }
    double a = c.a, rf = c.f > 0 ? 1.0 / c.f : 0.0, b = 0, f = -1;
    if (!num("a", a) || !num("rf", rf) || !num("b", b) || !num("f", f)) return false;
    if (kv.count("b") && b > 0) rf = a != b ? a / (a - b) : 0.0;
    if (kv.count("f") && f >= 0) rf = f > 0 ? 1.0 / f : 0.0;
    if (kv.count("a") || kv.count("rf") || kv.count("b") || kv.count("f")) set_ellipsoid(c, a, rf, "custom");
    if (auto it = kv.find("towgs84"); it != kv.end() && !parse_shift(it->second, c)) {
        error = "+towgs84 needs 3 or 7 numbers";
        return false;
    }
    if (auto it = kv.find("nadgrids"); it != kv.end() && it->second != "@null") {
        error = "grid shifts (+nadgrids) are not supported; use +towgs84";
        return false;
    }
    if (auto it = kv.find("pm"); it != kv.end() && lower(it->second) != "greenwich" && it->second != "0") {
        error = "only the Greenwich prime meridian is supported";
        return false;
    }

    c.lat0 = c.lon0 = 0;
    c.k0 = 1;
    c.x0 = c.y0 = 0;
    if (proj == "utm") {
        double zone = 0;
        if (!num("zone", zone) || zone < 1 || zone > 60) {
            if (error.empty()) error = "+proj=utm needs +zone=1..60";
            return false;
        }
        c.lon0 = zone * 6 - 183;
        c.k0 = 0.9996;
        c.x0 = 500000;
        c.y0 = kv.count("south") ? 10000000 : 0;
    }
    if (!num("lat_0", c.lat0) || !num("lon_0", c.lon0) || !num("k", c.k0) || !num("k_0", c.k0) ||
        !num("x_0", c.x0) || !num("y_0", c.y0))
        return false;
    c.lat1 = c.lat0;
    if (!num("lat_1", c.lat1)) return false;
    c.lat2 = c.lat1;
    if (!num("lat_2", c.lat2)) return false;
    c.to_meter = 1;
    if (auto it = kv.find("units"); it != kv.end()) {
        const std::string u = lower(it->second);
        if (u == "m") c.to_meter = 1;
        else if (u == "km") c.to_meter = 1000;
        else if (u == "ft") c.to_meter = 0.3048;
        else if (u == "us-ft") c.to_meter = 1200.0 / 3937.0;
        else {
            error = "units '" + it->second + "' are not supported";
            return false;
        }
    }
    if (!num("to_meter", c.to_meter)) return false;
    if (c.kind == Crs::Kind::LambertConic && std::fabs(c.lat1 + c.lat2) < 1e-9) {
        error = "+lat_1 and +lat_2 are opposite; no cone";
        return false;
    }
    return true;
}

// ---- WKT 1 and 2 ----

struct Node {
    std::string key;                            // empty for a leaf
    std::string text;                           // leaf value, unquoted
    bool quoted = false;
    std::vector<std::unique_ptr<Node>> kids;

    const Node* child(std::initializer_list<const char*> keys) const {
        for (const auto& k : kids)
            for (const char* want : keys)
                if (k->key == want) return k.get();
        return nullptr;
    }
    const Node* find(std::initializer_list<const char*> keys) const {
        if (const Node* n = child(keys)) return n;
        for (const auto& k : kids)
            if (const Node* n = k->find(keys)) return n;
        return nullptr;
    }
    std::string str(size_t i) const {
        size_t n = 0;
        for (const auto& k : kids)
            if (k->key.empty() && n++ == i) return k->text;
        return {};
    }
    double num(size_t i, double def = 0) const {
        double v;
        return to_num(str(i), v) ? v : def;
    }
};

struct WktReader {
    const std::string& s;
    size_t i = 0;
    void ws() {
        while (i < s.size() && std::isspace((unsigned char)s[i])) i++;
    }
    std::unique_ptr<Node> value(int depth) {
        if (depth > 64) throw std::runtime_error("WKT nests too deep");
        ws();
        auto n = std::make_unique<Node>();
        if (i < s.size() && s[i] == '"') {
            i++;
            while (i < s.size()) {
                if (s[i] == '"') {
                    if (i + 1 < s.size() && s[i + 1] == '"') {
                        n->text += '"';
                        i += 2;
                        continue;
                    }
                    break;
                }
                n->text += s[i++];
            }
            if (i >= s.size()) throw std::runtime_error("unclosed quote in WKT");
            i++;
            n->quoted = true;
            return n;
        }
        std::string word;
        while (i < s.size() && s[i] != ',' && s[i] != '[' && s[i] != '(' && s[i] != ']' && s[i] != ')' &&
               !std::isspace((unsigned char)s[i]))
            word += s[i++];
        ws();
        if (i < s.size() && (s[i] == '[' || s[i] == '(')) {
            const char close = s[i] == '[' ? ']' : ')';
            i++;
            for (char& ch : word) ch = (char)std::toupper((unsigned char)ch);
            n->key = word;
            ws();
            if (i < s.size() && s[i] == close) {
                i++;
                return n;
            }
            while (true) {
                n->kids.push_back(value(depth + 1));
                ws();
                if (i < s.size() && s[i] == ',') {
                    i++;
                    continue;
                }
                if (i < s.size() && s[i] == close) {
                    i++;
                    break;
                }
                throw std::runtime_error("malformed WKT near character " + std::to_string(i));
            }
            return n;
        }
        if (word.empty()) throw std::runtime_error("malformed WKT near character " + std::to_string(i));
        n->text = word;
        return n;
    }
};

// A WKT 2 PARAMETER's own unit factor, else `def`.
double unit_factor(const Node& p, double def) {
    if (const Node* u = p.child({"ANGLEUNIT", "LENGTHUNIT", "SCALEUNIT", "UNIT"})) return u->num(1, def);
    return def;
}

bool parse_wkt(const std::string& def, Crs& c, std::string& error) {
    std::unique_ptr<Node> root;
    try {
        WktReader r{def};
        root = r.value(0);
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
    const Node* crs = root.get();
    const Node* bound_tf = nullptr;
    if (crs->key == "BOUNDCRS") {
        bound_tf = crs->child({"ABRIDGEDTRANSFORMATION"});
        const Node* src = crs->child({"SOURCECRS"});
        crs = src && !src->kids.empty() ? src->kids[0].get() : nullptr;
    }
    if (crs && (crs->key == "COMPD_CS" || crs->key == "COMPOUNDCRS")) {
        const Node* h = nullptr;
        for (const auto& k : crs->kids)
            if (!k->key.empty()) {
                h = k.get();
                break;
            }
        crs = h;
    }
    if (!crs) {
        error = "WKT has no coordinate system";
        return false;
    }
    const bool projected = crs->key == "PROJCS" || crs->key == "PROJCRS" || crs->key == "PROJECTEDCRS";
    const bool geographic = crs->key == "GEOGCS" || crs->key == "GEOGCRS" || crs->key == "GEODCRS" ||
                            crs->key == "GEOGRAPHICCRS" || crs->key == "GEODETICCRS";
    if (!projected && !geographic) {
        error = "WKT '" + crs->key + "' is not a projected or geographic system";
        return false;
    }
    const Node* sph = crs->find({"SPHEROID", "ELLIPSOID"});
    if (!sph) {
        error = "WKT has no SPHEROID";
        return false;
    }
    double a_unit = 1;
    if (const Node* u = sph->child({"LENGTHUNIT", "UNIT"})) a_unit = u->num(1, 1);
    set_ellipsoid(c, sph->num(1) * a_unit, sph->num(2), sph->str(0));
    if (c.a <= 0) {
        error = "WKT SPHEROID has no semi-major axis";
        return false;
    }
    if (const Node* d = crs->find({"DATUM", "GEODETICDATUM", "TRF"})) c.datum = d->str(0);
    std::fill(c.towgs84, c.towgs84 + 7, 0.0);
    if (const Node* t = crs->find({"TOWGS84"}))
        for (size_t k = 0; k < 7; k++) c.towgs84[k] = t->num(k);
    if (bound_tf) {
        const Node* m = bound_tf->child({"METHOD"});
        const std::string method = m ? squash(m->str(0)) : "";
        const double rot_sign = method.find("coordinateframe") != std::string::npos ? -1.0 : 1.0;
        for (const auto& k : bound_tf->kids) {
            if (k->key != "PARAMETER") continue;
            const std::string n = squash(k->str(0));
            const double v = k->num(1);
            const bool unitless = !k->child({"ANGLEUNIT", "LENGTHUNIT", "SCALEUNIT", "UNIT"});
            if (n == "xaxistranslation") c.towgs84[0] = v * unit_factor(*k, 1.0);
            else if (n == "yaxistranslation") c.towgs84[1] = v * unit_factor(*k, 1.0);
            else if (n == "zaxistranslation") c.towgs84[2] = v * unit_factor(*k, 1.0);
            else if (n == "xaxisrotation") c.towgs84[3] = rot_sign * v * unit_factor(*k, kSec) / kSec;
            else if (n == "yaxisrotation") c.towgs84[4] = rot_sign * v * unit_factor(*k, kSec) / kSec;
            else if (n == "zaxisrotation") c.towgs84[5] = rot_sign * v * unit_factor(*k, kSec) / kSec;
            // PROJ writes a bare factor (0.99999978); EPSG a value in ppm with its unit.
            else if (n == "scaledifference")
                c.towgs84[6] = unitless ? (std::fabs(v - 1) < 0.01 ? (v - 1) * 1e6 : v) : v * unit_factor(*k, 1e-6) * 1e6;
        }
    }
    shift_from_datum_name(c);

    c.lat0 = c.lon0 = c.lat1 = c.lat2 = 0;
    c.k0 = 1;
    c.x0 = c.y0 = 0;
    c.to_meter = 1;
    if (geographic) {
        c.kind = Crs::Kind::Geographic;
        return true;
    }
    const Node* conv = crs->find({"CONVERSION"});
    const Node* method = conv ? conv->child({"METHOD", "PROJECTION"}) : crs->child({"PROJECTION"});
    if (!method) {
        error = "WKT has no PROJECTION";
        return false;
    }
    const std::string m = squash(method->str(0));
    if (m.find("transversemercator") != std::string::npos || m.find("gausskruger") != std::string::npos) {
        if (m.find("south") != std::string::npos) {
            error = "south-oriented Transverse Mercator is not supported";
            return false;
        }
        c.kind = Crs::Kind::TransverseMercator;
    } else if (m.find("lambertconformalconic") != std::string::npos ||
               m.find("lambertconicconformal") != std::string::npos) {
        c.kind = Crs::Kind::LambertConic;
    } else {
        error = "projection '" + method->str(0) + "' is not supported (Transverse Mercator, Lambert conic)";
        return false;
    }
    // WKT 1 lengths are in the PROJCS unit; WKT 2 parameters carry their own.
    double lin = 1;
    if (const Node* u = crs->child({"UNIT", "LENGTHUNIT"})) lin = u->num(1, 1);
    if (const Node* ax = crs->find({"AXIS"}))
        if (const Node* u = ax->child({"LENGTHUNIT", "UNIT"})) lin = u->num(1, lin);
    c.to_meter = lin;
    bool has_lat1 = false, has_lat2 = false;
    const Node* params = conv ? conv : crs;
    for (const auto& k : params->kids) {
        if (k->key != "PARAMETER") continue;
        const std::string n = squash(k->str(0));
        const double v = k->num(1);
        const double ang = conv ? unit_factor(*k, kDeg) / kDeg : 1.0;
        const double len = conv ? unit_factor(*k, 1.0) : lin;
        if (n == "latitudeoforigin" || n == "latitudeofnaturalorigin" || n == "latitudeoffalseorigin" ||
            n == "latitudeofcenter" || n == "latitudeofprojectioncentre")
            c.lat0 = v * ang;
        else if (n == "centralmeridian" || n == "longitudeofnaturalorigin" || n == "longitudeoffalseorigin" ||
                 n == "longitudeofcenter" || n == "longitudeoforigin")
            c.lon0 = v * ang;
        else if (n == "scalefactor" || n == "scalefactoratnaturalorigin")
            c.k0 = v * (conv ? unit_factor(*k, 1.0) : 1.0);
        else if (n == "falseeasting" || n == "eastingatfalseorigin")
            c.x0 = v * len;
        else if (n == "falsenorthing" || n == "northingatfalseorigin")
            c.y0 = v * len;
        else if (n == "standardparallel1" || n == "latitudeof1ststandardparallel") {
            c.lat1 = v * ang;
            has_lat1 = true;
        } else if (n == "standardparallel2" || n == "latitudeof2ndstandardparallel") {
            c.lat2 = v * ang;
            has_lat2 = true;
        }
    }
    if (!has_lat1) c.lat1 = c.lat0;
    if (!has_lat2) c.lat2 = c.lat1;
    return true;
}

// ---- math ----

struct Tm {
    double A, n, e, alpha[7], beta[7], xi0;
};

Tm tm_setup(const Crs& c) {
    Tm t{};
    const double n = c.f / (2 - c.f), n2 = n * n, n3 = n2 * n, n4 = n3 * n, n5 = n4 * n, n6 = n5 * n;
    t.n = n;
    t.e = std::sqrt(c.f * (2 - c.f));
    t.A = c.a / (1 + n) * (1 + n2 / 4 + n4 / 64 + n6 / 256);
    // Kruger series to n^6 (Karney 2011): sub-millimetre to 4000 km from the meridian.
    t.alpha[1] = n / 2 - 2 * n2 / 3 + 5 * n3 / 16 + 41 * n4 / 180 - 127 * n5 / 288 + 7891 * n6 / 37800;
    t.alpha[2] = 13 * n2 / 48 - 3 * n3 / 5 + 557 * n4 / 1440 + 281 * n5 / 630 - 1983433 * n6 / 1935360;
    t.alpha[3] = 61 * n3 / 240 - 103 * n4 / 140 + 15061 * n5 / 26880 + 167603 * n6 / 181440;
    t.alpha[4] = 49561 * n4 / 161280 - 179 * n5 / 168 + 6601661 * n6 / 7257600;
    t.alpha[5] = 34729 * n5 / 80640 - 3418889 * n6 / 1995840;
    t.alpha[6] = 212378941 * n6 / 319334400;
    t.beta[1] = n / 2 - 2 * n2 / 3 + 37 * n3 / 96 - n4 / 360 - 81 * n5 / 512 + 96199 * n6 / 604800;
    t.beta[2] = n2 / 48 + n3 / 15 - 437 * n4 / 1440 + 46 * n5 / 105 - 1118711 * n6 / 3870720;
    t.beta[3] = 17 * n3 / 480 - 37 * n4 / 840 - 209 * n5 / 4480 + 5569 * n6 / 90720;
    t.beta[4] = 4397 * n4 / 161280 - 11 * n5 / 504 - 830251 * n6 / 7257600;
    t.beta[5] = 4583 * n5 / 161280 - 108847 * n6 / 3991680;
    t.beta[6] = 20648693 * n6 / 638668800;
    return t;
}

double conformal_tan(double phi, double e) {
    const double tau = std::tan(phi);
    const double s = std::sinh(e * std::atanh(e * tau / std::sqrt(1 + tau * tau)));
    return tau * std::sqrt(1 + s * s) - s * std::sqrt(1 + tau * tau);
}

void tm_forward_raw(const Tm& t, double phi, double lam, double& xi, double& eta) {
    const double tp = conformal_tan(phi, t.e);
    const double xi_ = std::atan2(tp, std::cos(lam));
    const double eta_ = std::asinh(std::sin(lam) / std::sqrt(tp * tp + std::cos(lam) * std::cos(lam)));
    xi = xi_;
    eta = eta_;
    for (int j = 1; j <= 6; j++) {
        xi += t.alpha[j] * std::sin(2 * j * xi_) * std::cosh(2 * j * eta_);
        eta += t.alpha[j] * std::cos(2 * j * xi_) * std::sinh(2 * j * eta_);
    }
}

void tm_forward(const Crs& c, double lat, double lon, double& x, double& y) {
    const Tm t = tm_setup(c);
    double xi0, eta0, xi, eta;
    tm_forward_raw(t, c.lat0 * kDeg, 0, xi0, eta0);
    double dl = lon - c.lon0;
    while (dl > 180) dl -= 360;
    while (dl < -180) dl += 360;
    tm_forward_raw(t, lat * kDeg, dl * kDeg, xi, eta);
    x = c.y0 + c.k0 * t.A * (xi - xi0);
    y = c.x0 + c.k0 * t.A * eta;
}

void tm_inverse(const Crs& c, double x, double y, double& lat, double& lon) {
    const Tm t = tm_setup(c);
    double xi0, eta0;
    tm_forward_raw(t, c.lat0 * kDeg, 0, xi0, eta0);
    const double xi = (x - c.y0) / (c.k0 * t.A) + xi0;
    const double eta = (y - c.x0) / (c.k0 * t.A);
    double xi_ = xi, eta_ = eta;
    for (int j = 1; j <= 6; j++) {
        xi_ -= t.beta[j] * std::sin(2 * j * xi) * std::cosh(2 * j * eta);
        eta_ -= t.beta[j] * std::cos(2 * j * xi) * std::sinh(2 * j * eta);
    }
    const double tp = std::sin(xi_) / std::sqrt(std::sinh(eta_) * std::sinh(eta_) + std::cos(xi_) * std::cos(xi_));
    double tau = tp;
    const double e2 = t.e * t.e;
    for (int it = 0; it < 6; it++) {
        const double s = std::sinh(t.e * std::atanh(t.e * tau / std::sqrt(1 + tau * tau)));
        const double ti = tau * std::sqrt(1 + s * s) - s * std::sqrt(1 + tau * tau);
        const double d = (tp - ti) / std::sqrt(1 + ti * ti) * (1 + (1 - e2) * tau * tau) /
                         ((1 - e2) * std::sqrt(1 + tau * tau));
        tau += d;
        if (std::fabs(d) < 1e-14) break;
    }
    lat = std::atan(tau) / kDeg;
    lon = c.lon0 + std::atan2(std::sinh(eta_), std::cos(xi_)) / kDeg;
}

struct Lcc {
    double e, n, F, rho0;
};

double lcc_m(double phi, double e) {
    return std::cos(phi) / std::sqrt(1 - e * e * std::sin(phi) * std::sin(phi));
}
double lcc_t(double phi, double e) {
    const double es = e * std::sin(phi);
    return std::tan(kPi / 4 - phi / 2) / std::pow((1 - es) / (1 + es), e / 2);
}

Lcc lcc_setup(const Crs& c) {
    Lcc l{};
    l.e = std::sqrt(c.f * (2 - c.f));
    const double p1 = c.lat1 * kDeg, p2 = c.lat2 * kDeg;
    const double m1 = lcc_m(p1, l.e), t1 = lcc_t(p1, l.e);
    if (std::fabs(p1 - p2) < 1e-12) l.n = std::sin(p1);
    else l.n = (std::log(m1) - std::log(lcc_m(p2, l.e))) / (std::log(t1) - std::log(lcc_t(p2, l.e)));
    l.F = m1 / (l.n * std::pow(t1, l.n));
    l.rho0 = c.a * c.k0 * l.F * std::pow(lcc_t(c.lat0 * kDeg, l.e), l.n);
    return l;
}

void lcc_forward(const Crs& c, double lat, double lon, double& x, double& y) {
    const Lcc l = lcc_setup(c);
    const double rho = c.a * c.k0 * l.F * std::pow(lcc_t(lat * kDeg, l.e), l.n);
    const double th = l.n * (lon - c.lon0) * kDeg;
    y = c.x0 + rho * std::sin(th);
    x = c.y0 + l.rho0 - rho * std::cos(th);
}

void lcc_inverse(const Crs& c, double x, double y, double& lat, double& lon) {
    const Lcc l = lcc_setup(c);
    const double s = l.n < 0 ? -1.0 : 1.0;
    const double dx = y - c.x0, dy = l.rho0 - (x - c.y0);
    const double rho = s * std::sqrt(dx * dx + dy * dy);
    const double th = std::atan2(s * dx, s * dy);
    const double t = std::pow(rho / (c.a * c.k0 * l.F), 1 / l.n);
    double phi = kPi / 2 - 2 * std::atan(t);
    for (int it = 0; it < 15; it++) {
        const double es = l.e * std::sin(phi);
        const double next = kPi / 2 - 2 * std::atan(t * std::pow((1 - es) / (1 + es), l.e / 2));
        if (std::fabs(next - phi) < 1e-14) {
            phi = next;
            break;
        }
        phi = next;
    }
    lat = phi / kDeg;
    lon = c.lon0 + th / l.n / kDeg;
}

void to_ecef(double a, double f, double lat, double lon, double h, double X[3]) {
    const double e2 = f * (2 - f), p = lat * kDeg, l = lon * kDeg;
    const double N = a / std::sqrt(1 - e2 * std::sin(p) * std::sin(p));
    X[0] = (N + h) * std::cos(p) * std::cos(l);
    X[1] = (N + h) * std::cos(p) * std::sin(l);
    X[2] = (N * (1 - e2) + h) * std::sin(p);
}

void from_ecef(double a, double f, const double X[3], double& lat, double& lon) {
    const double e2 = f * (2 - f), p = std::hypot(X[0], X[1]);
    double phi = std::atan2(X[2], p * (1 - e2));
    for (int it = 0; it < 8; it++) {
        const double N = a / std::sqrt(1 - e2 * std::sin(phi) * std::sin(phi));
        const double h = p / std::cos(phi) - N;
        phi = std::atan2(X[2], p * (1 - e2 * N / (N + h)));
    }
    lat = phi / kDeg;
    lon = std::atan2(X[1], X[0]) / kDeg;
}

// Position-vector Helmert; inverse by the transpose (rotations are microradians).
void helmert(const double t[7], const double in[3], double out[3], bool inverse) {
    const double rx = t[3] * kSec, ry = t[4] * kSec, rz = t[5] * kSec, s = 1 + t[6] * 1e-6;
    const double R[3][3] = {{1, -rz, ry}, {rz, 1, -rx}, {-ry, rx, 1}};
    if (!inverse) {
        for (int r = 0; r < 3; r++)
            out[r] = t[r] + s * (R[r][0] * in[0] + R[r][1] * in[1] + R[r][2] * in[2]);
    } else {
        const double d[3] = {in[0] - t[0], in[1] - t[1], in[2] - t[2]};
        for (int r = 0; r < 3; r++) out[r] = (R[0][r] * d[0] + R[1][r] * d[1] + R[2][r] * d[2]) / s;
    }
}

// No shift given means the same datum as WGS 84, whatever the ellipsoid (PROJ's
// ballpark): going through geocentric with a zero shift would move it ~200 m.
bool no_shift(const Crs& c) {
    for (double t : c.towgs84)
        if (t != 0) return false;
    return true;
}

// The system's own datum <-> WGS 84, ellipsoidal height 0 (plan only).
void datum_to_wgs84(const Crs& c, double& lat, double& lon) {
    if (no_shift(c)) return;
    double X[3], Y[3];
    to_ecef(c.a, c.f, lat, lon, 0, X);
    helmert(c.towgs84, X, Y, false);
    from_ecef(6378137.0, 1 / 298.257223563, Y, lat, lon);
}

void wgs84_to_datum(const Crs& c, double& lat, double& lon) {
    if (no_shift(c)) return;
    double X[3], Y[3];
    to_ecef(6378137.0, 1 / 298.257223563, lat, lon, 0, X);
    helmert(c.towgs84, X, Y, true);
    from_ecef(c.a, c.f, Y, lat, lon);
}

std::string fmt(double v) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.15g", v);
    return buf;
}

std::string wkt_name(std::string s) {
    for (char& c : s)
        if (c == '"') c = '\'';
    return s;
}

}  // namespace

bool parse_crs(const std::string& def_in, Crs& out, std::string& error) {
    std::string def = def_in;
    while (!def.empty() && std::isspace((unsigned char)def.back())) def.pop_back();
    size_t k = 0;
    while (k < def.size() && std::isspace((unsigned char)def[k])) k++;
    def.erase(0, k);
    Crs c;
    bool ok;
    const std::string low = lower(def);
    if (def.empty()) {
        error = "empty definition";
        return false;
    } else if (def[0] == '+') {
        ok = parse_proj(def, c, error);
    } else if (low.rfind("epsg:", 0) == 0 || std::all_of(def.begin(), def.end(), [](char ch) { return ch >= '0' && ch <= '9'; })) {
        const int code = std::atoi(def.c_str() + (std::isdigit((unsigned char)def[0]) ? 0 : 5));
        ok = parse_epsg(code, c, error);
    } else {
        ok = parse_wkt(def, c, error);
    }
    if (!ok) return false;
    if (!(c.a > 0) || !(c.k0 > 0) || !(c.to_meter > 0) || c.f < 0 || c.f >= 1) {
        error = "ellipsoid, scale or unit is out of range";
        return false;
    }
    out = c;
    return true;
}

void crs_to_wgs84(const Crs& c, double x, double y, double& lat, double& lon) {
    if (c.kind == Crs::Kind::Geographic) {
        lat = x;
        lon = y;
    } else {
        x *= c.to_meter;
        y *= c.to_meter;
        if (c.kind == Crs::Kind::TransverseMercator) tm_inverse(c, x, y, lat, lon);
        else lcc_inverse(c, x, y, lat, lon);
    }
    datum_to_wgs84(c, lat, lon);
}

void wgs84_to_crs(const Crs& c, double lat, double lon, double& x, double& y) {
    wgs84_to_datum(c, lat, lon);
    if (c.kind == Crs::Kind::Geographic) {
        x = lat;
        y = lon;
        return;
    }
    if (c.kind == Crs::Kind::TransverseMercator) tm_forward(c, lat, lon, x, y);
    else lcc_forward(c, lat, lon, x, y);
    x /= c.to_meter;
    y /= c.to_meter;
}

std::string crs_to_wkt(const Crs& c, const std::string& name) {
    const std::string datum = c.datum.empty() ? "unknown" : c.datum;
    std::string g = "GEOGCS[\"" + wkt_name(datum) + "\",DATUM[\"" + wkt_name(datum) + "\",SPHEROID[\"" +
                    wkt_name(c.ellipsoid.empty() ? "unknown" : c.ellipsoid) + "\"," + fmt(c.a) + "," +
                    fmt(c.f > 0 ? 1 / c.f : 0) + "]";
    bool shift = false;
    for (double t : c.towgs84) shift |= t != 0;
    if (shift) {
        g += ",TOWGS84[";
        for (int k = 0; k < 7; k++) g += (k ? "," : "") + fmt(c.towgs84[k]);
        g += "]";
    }
    g += "],PRIMEM[\"Greenwich\",0],UNIT[\"degree\",0.0174532925199433]]";
    if (c.kind == Crs::Kind::Geographic) return g;
    auto par = [](const char* n, double v) { return std::string(",PARAMETER[\"") + n + "\"," + fmt(v) + "]"; };
    std::string s = "PROJCS[\"" + wkt_name(name) + "\"," + g;
    if (c.kind == Crs::Kind::TransverseMercator) {
        s += ",PROJECTION[\"Transverse_Mercator\"]" + par("latitude_of_origin", c.lat0) +
             par("central_meridian", c.lon0) + par("scale_factor", c.k0);
    } else if (std::fabs(c.lat1 - c.lat2) < 1e-12 && c.k0 != 1) {
        s += ",PROJECTION[\"Lambert_Conformal_Conic_1SP\"]" + par("latitude_of_origin", c.lat0) +
             par("central_meridian", c.lon0) + par("scale_factor", c.k0);
    } else {
        s += ",PROJECTION[\"Lambert_Conformal_Conic_2SP\"]" + par("standard_parallel_1", c.lat1) +
             par("standard_parallel_2", c.lat2) + par("latitude_of_origin", c.lat0) +
             par("central_meridian", c.lon0);
    }
    s += par("false_easting", c.x0 / c.to_meter) + par("false_northing", c.y0 / c.to_meter);
    s += c.to_meter == 1 ? ",UNIT[\"metre\",1]]" : ",UNIT[\"unit\"," + fmt(c.to_meter) + "]]";
    return s;
}

const std::vector<CrsEntry>& builtin_crs() {
    static const std::vector<CrsEntry> list = [] {
        std::vector<CrsEntry> v;
        v.push_back({"\xD0\x9C\xD0\xA1\xD0\x9A-52, \xD0\xB7\xD0\xBE\xD0\xBD\xD0\xB0 2 (MSK-52 zone 2)",
                     "+proj=tmerc +lat_0=0 +lon_0=44.55 +k=1 +x_0=2250000 +y_0=-5714743.504 +ellps=krass "
                     "+towgs84=23.57,-140.95,-79.8,0,0.35,0.79,-0.22 +units=m"});
        char name[96];
        for (int z = 4; z <= 32; z++) {
            std::snprintf(name, sizeof name,
                          "\xD0\xA1\xD0\x9A-42, \xD0\xB7\xD0\xBE\xD0\xBD\xD0\xB0 %d (Pulkovo 1942 / GK zone %d)",
                          z, z);
            v.push_back({name, "EPSG:" + std::to_string(28400 + z)});
        }
        for (int z = 1; z <= 60; z++) {
            std::snprintf(name, sizeof name, "WGS 84 / UTM zone %dN", z);
            v.push_back({name, "EPSG:" + std::to_string(32600 + z)});
        }
        for (int z = 1; z <= 60; z++) {
            std::snprintf(name, sizeof name, "WGS 84 / UTM zone %dS", z);
            v.push_back({name, "EPSG:" + std::to_string(32700 + z)});
        }
        for (int z = 28; z <= 38; z++) {
            std::snprintf(name, sizeof name, "ETRS89 / UTM zone %dN", z);
            v.push_back({name, "EPSG:" + std::to_string(25800 + z)});
        }
        for (int z = 2; z <= 5; z++) {
            std::snprintf(name, sizeof name, "DHDN / 3-degree GK zone %d", z);
            v.push_back({name, "EPSG:" + std::to_string(31464 + z)});
        }
        v.push_back({"RGF93 / Lambert-93", "EPSG:2154"});
        v.push_back({"ETRF2000-PL / CS92", "EPSG:2180"});
        return v;
    }();
    return list;
}

bool load_crs_list(const std::string& path, std::vector<CrsEntry>& out, std::string& error) {
    std::string text;
    if (!read_text(path, text, error)) return false;
    try {
        const JsonValue j = json_parse(text);
        const JsonValue* list = j.find("systems");
        if (!list || !list->is_array()) {
            error = "no \"systems\" list in " + path;
            return false;
        }
        std::vector<CrsEntry> v;
        for (const auto& e : list->arr) {
            const JsonValue* n = e.find("name");
            const JsonValue* d = e.find("def");
            if (n && d && !n->as_string().empty()) v.push_back({n->as_string(), d->as_string()});
        }
        out = std::move(v);
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

bool save_crs_list(const std::string& path, const std::vector<CrsEntry>& list, std::string& error) {
    JsonWriter w;
    w.object();
    w.key("version").value(1);
    w.key("systems").array();
    for (const auto& e : list) {
        w.object();
        w.key("name").value(e.name);
        w.key("def").value(e.def);
        w.end();
    }
    w.end();
    w.end();
    return write_text(path, w.str(), error);
}

}  // namespace geo
}  // namespace spirula
