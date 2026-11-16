#include "data/GeoVectors.h"

#include "data/GeoCrs.h"
#include "data/GeoProject.h"
#include "data/Json.h"
#include "data/JsonWrite.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <regex>
#include <cwctype>
#include <sstream>

namespace spirula {
namespace geo {

namespace fs = std::filesystem;

namespace {

std::string lower_ascii(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

// Code points of a UTF-8 string; a byte that is not valid UTF-8 is taken as
// cp1251, which is what an old Russian DXF holds.
std::vector<uint32_t> code_points(const std::string& s) {
    static const uint16_t cp1251_hi[64] = {
        0x0402, 0x0403, 0x201A, 0x0453, 0x201E, 0x2026, 0x2020, 0x2021, 0x20AC, 0x2030, 0x0409,
        0x2039, 0x040A, 0x040C, 0x040B, 0x040F, 0x0452, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
        0x2013, 0x2014, 0x0020, 0x2122, 0x0459, 0x203A, 0x045A, 0x045C, 0x045B, 0x045F, 0x00A0,
        0x040E, 0x045E, 0x0408, 0x00A4, 0x0490, 0x00A6, 0x00A7, 0x0401, 0x00A9, 0x0404, 0x00AB,
        0x00AC, 0x00AD, 0x00AE, 0x0407, 0x00B0, 0x00B1, 0x0406, 0x0456, 0x0491, 0x00B5, 0x00B6,
        0x00B7, 0x0451, 0x2116, 0x0454, 0x00BB, 0x0458, 0x0405, 0x0455, 0x0457};
    std::vector<uint32_t> out;
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = (unsigned char)s[i];
        int n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        bool ok = n > 0 && i + n <= s.size();
        uint32_t cp = n == 1 ? c : n == 2 ? (c & 0x1F) : n == 3 ? (c & 0x0F) : (c & 0x07);
        for (int k = 1; ok && k < n; k++) {
            const unsigned char d = (unsigned char)s[i + k];
            if ((d >> 6) != 2) ok = false;
            cp = (cp << 6) | (d & 0x3F);
        }
        if (ok) {
            out.push_back(cp);
            i += n;
        } else {
            out.push_back(c >= 0xC0 ? 0x0410 + (c - 0xC0) : c >= 0x80 ? cp1251_hi[c - 0x80] : c);
            i++;
        }
    }
    return out;
}

std::string utf8_of(const std::vector<uint32_t>& cps) {
    std::string o;
    for (uint32_t c : cps) {
        if (c < 0x80) {
            o += (char)c;
        } else if (c < 0x800) {
            o += (char)(0xC0 | (c >> 6));
            o += (char)(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            o += (char)(0xE0 | (c >> 12));
            o += (char)(0x80 | ((c >> 6) & 0x3F));
            o += (char)(0x80 | (c & 0x3F));
        } else {
            o += (char)(0xF0 | (c >> 18));
            o += (char)(0x80 | ((c >> 12) & 0x3F));
            o += (char)(0x80 | ((c >> 6) & 0x3F));
            o += (char)(0x80 | (c & 0x3F));
        }
    }
    return o;
}

// DXF R12 text: cp1251 for Cyrillic, \U+XXXX for anything else past ASCII.
std::string dxf_text(const std::string& s) {
    std::string o;
    for (uint32_t c : code_points(s)) {
        if (c < 0x80) o += (char)c;
        else if (c >= 0x0410 && c <= 0x044F) o += (char)(0xC0 + (c - 0x0410));
        else if (c == 0x0401) o += (char)0xA8;
        else if (c == 0x0451) o += (char)0xB8;
        else if (c == 0x2116) o += (char)0xB9;
        else {
            char buf[16];
            std::snprintf(buf, sizeof buf, "\\U+%04X", (unsigned)(c & 0xFFFF));
            o += buf;
        }
    }
    return o;
}

std::string dxf_layer(const std::string& s) {
    std::string o = s.empty() ? std::string("0") : s;
    for (char& c : o)
        if (std::strchr("<>/\\\":;?*|=`", c)) c = '_';
    return dxf_text(o);
}

std::string xml_escape(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '&') o += "&amp;";
        else if (c == '<') o += "&lt;";
        else if (c == '>') o += "&gt;";
        else if (c == '"') o += "&quot;";
        else o += c;
    }
    return o;
}

int aci_from_rgb(int r, int g, int b) {
    static const int pal[9][4] = {{1, 255, 0, 0},     {2, 255, 255, 0},   {3, 0, 255, 0},
                                  {4, 0, 255, 255},   {5, 0, 0, 255},     {6, 255, 0, 255},
                                  {7, 255, 255, 255}, {8, 128, 128, 128}, {9, 192, 192, 192}};
    int best = 7;
    long bd = 1L << 30;
    for (const auto& p : pal) {
        const long d = (long)(r - p[1]) * (r - p[1]) + (long)(g - p[2]) * (g - p[2]) + (long)(b - p[3]) * (b - p[3]);
        if (d < bd) {
            bd = d;
            best = p[0];
        }
    }
    return best;
}

std::vector<std::string> split_row(const std::string& line) {
    char sep = ';';
    if (line.find(';') == std::string::npos) sep = line.find('\t') != std::string::npos ? '\t' : ',';
    std::vector<std::string> out;
    std::string cur;
    for (char c : line) {
        if (c == sep) {
            out.push_back(cur);
            cur.clear();
        } else if (c != '\r') {
            cur += c;
        }
    }
    out.push_back(cur);
    for (auto& f : out) {
        while (!f.empty() && (f.back() == ' ' || f.back() == '"')) f.pop_back();
        size_t k = 0;
        while (k < f.size() && (f[k] == ' ' || f[k] == '"')) k++;
        f.erase(0, k);
    }
    return out;
}

bool load_dxf_layers(const std::string& text, std::vector<ClassCode>& out) {
    std::istringstream in(text);
    std::string code, value;
    bool in_layer_table = false, in_layer = false;
    ClassCode cur;
    auto flush = [&] {
        if (in_layer && !cur.code.empty() && cur.code != "0") out.push_back(cur);
        in_layer = false;
        cur = ClassCode{};
    };
    while (std::getline(in, code) && std::getline(in, value)) {
        while (!code.empty() && (code.back() == '\r' || code.back() == ' ')) code.pop_back();
        while (!value.empty() && value.back() == '\r') value.pop_back();
        const int g = std::atoi(code.c_str());
        if (g == 0) {
            flush();
            if (value == "TABLE") continue;
            if (value == "ENDTAB") in_layer_table = false;
            if (value == "LAYER" && in_layer_table) {
                in_layer = true;
                cur.kind = FeatureKind::Line;
            }
        } else if (g == 2 && !in_layer && value == "LAYER") {
            in_layer_table = true;
        } else if (in_layer && g == 2) {
            cur.code = cur.layer = cur.name = utf8_of(code_points(value));
        } else if (in_layer && g == 62) {
            cur.aci = std::abs(std::atoi(value.c_str()));
        }
    }
    flush();
    return !out.empty();
}

void ring(const std::vector<std::array<double, 3>>& v, bool closed,
          const std::function<void(const std::array<double, 3>&, const std::array<double, 3>&)>& f) {
    for (size_t i = 1; i < v.size(); i++) f(v[i - 1], v[i]);
    if (closed && v.size() > 2) f(v.back(), v.front());
}

bool open_out(const std::string& path, std::ofstream& f, std::string& error) {
    f.open(fs::u8path(path), std::ios::binary | std::ios::trunc);
    if (!f) error = "cannot write " + path;
    return (bool)f;
}

std::string num(double v, int digits = 4) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.*f", digits, v);
    return buf;
}

}  // namespace

// ---- storage ----

std::string vectors_path(const std::string& dataset_dir, const std::string& model_path) {
    if (!dataset_dir.empty()) return (fs::u8path(dataset_dir) / "vectors.json").u8string();
    return model_path + ".vectors.json";
}

const char* kind_key(FeatureKind k) {
    switch (k) {
        case FeatureKind::Line: return "line";
        case FeatureKind::Polygon: return "polygon";
        default: return "point";
    }
}

FeatureKind kind_from_key(const std::string& key, FeatureKind def) {
    const std::string k = utf8_of(code_points(lower_ascii(key)));
    auto has = [&](const char* w) { return k.find(w) != std::string::npos; };
    // Russian words compared lower case: точка, линия, полигон, контур, площадь
    if (has("point") || has("\xD1\x82\xD0\xBE\xD1\x87") || has("\xD0\xA2\xD0\xBE\xD1\x87")) return FeatureKind::Point;
    if (has("polygon") || has("area") || has("\xD0\xBF\xD0\xBE\xD0\xBB\xD0\xB8\xD0\xB3") ||
        has("\xD0\x9F\xD0\xBE\xD0\xBB\xD0\xB8\xD0\xB3") || has("\xD0\xBA\xD0\xBE\xD0\xBD\xD1\x82") ||
        has("\xD0\x9A\xD0\xBE\xD0\xBD\xD1\x82") || has("\xD0\xBF\xD0\xBB\xD0\xBE\xD1\x89") ||
        has("\xD0\x9F\xD0\xBB\xD0\xBE\xD1\x89"))
        return FeatureKind::Polygon;
    if (has("line") || has("\xD0\xBB\xD0\xB8\xD0\xBD") || has("\xD0\x9B\xD0\xB8\xD0\xBD")) return FeatureKind::Line;
    return def;
}

bool load_vectors(const std::string& path, std::vector<Feature>& out, bool& survey, std::string& error) {
    std::string text;
    if (!read_text(path, text, error)) return false;
    try {
        const JsonValue j = json_parse(text);
        std::vector<Feature> v;
        survey = j.find("survey") && j.find("survey")->as_bool();
        if (const JsonValue* fs_ = j.find("features"); fs_ && fs_->is_array()) {
            for (const auto& e : fs_->arr) {
                Feature f;
                if (const JsonValue* k = e.find("kind")) f.kind = kind_from_key(k->as_string(), FeatureKind::Point);
                if (const JsonValue* c = e.find("code")) f.code = c->as_string();
                if (const JsonValue* n = e.find("name")) f.name = n->as_string();
                if (const JsonValue* p = e.find("v"); p && p->is_array())
                    for (const auto& q : p->arr)
                        if (q.is_array() && q.arr.size() >= 3)
                            f.v.push_back({q.arr[0].as_double(), q.arr[1].as_double(), q.arr[2].as_double()});
                if (!f.v.empty()) v.push_back(std::move(f));
            }
        }
        out = std::move(v);
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

bool save_vectors(const std::string& path, const std::vector<Feature>& features, bool survey,
                  std::string& error) {
    JsonWriter w;
    w.object();
    w.key("version").value(1);
    w.key("survey").value(survey);
    w.key("features").array();
    for (const auto& f : features) {
        w.object();
        w.key("kind").value(kind_key(f.kind));
        w.key("code").value(f.code);
        if (!f.name.empty()) w.key("name").value(f.name);
        w.key("v").array();
        for (const auto& p : f.v) {
            w.array();
            for (double c : p) w.raw(json_number_exact(c));
            w.end();
        }
        w.end();
        w.end();
    }
    w.end();
    w.end();
    return write_text(path, w.str(), error);
}

// ---- classifier ----

bool load_classifier(const std::string& path, std::vector<ClassCode>& out, std::string& error) {
    std::string text;
    if (!read_text(path, text, error)) return false;
    std::vector<ClassCode> v;
    if (lower_ascii(fs::u8path(path).extension().u8string()) == ".dxf") {
        if (!load_dxf_layers(text, v)) {
            error = "no layers in " + path;
            return false;
        }
        out = std::move(v);
        return true;
    }
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        const auto f = split_row(utf8_of(code_points(line)));
        if (f.empty() || f[0].empty()) continue;
        const std::string head = lower_ascii(f[0]);
        if (head == "code" || f[0] == "\xD0\xBA\xD0\xBE\xD0\xB4" || f[0] == "\xD0\x9A\xD0\xBE\xD0\xB4") continue;
        ClassCode c;
        c.code = f[0];
        c.name = f.size() > 1 && !f[1].empty() ? f[1] : f[0];
        c.kind = f.size() > 2 ? kind_from_key(f[2], FeatureKind::Point) : FeatureKind::Point;
        c.layer = f.size() > 3 && !f[3].empty() ? f[3] : c.code;
        if (f.size() > 4 && !f[4].empty()) {
            const std::string& col = f[4];
            if (col[0] == '#' && col.size() == 7) {
                const long rgb = std::strtol(col.c_str() + 1, nullptr, 16);
                c.aci = aci_from_rgb((int)(rgb >> 16) & 255, (int)(rgb >> 8) & 255, (int)rgb & 255);
            } else {
                c.aci = std::clamp(std::atoi(col.c_str()), 1, 255);
            }
        }
        v.push_back(c);
    }
    if (v.empty()) {
        error = "no rows of the form: code;name;kind;layer;colour";
        return false;
    }
    out = std::move(v);
    return true;
}

const ClassCode* find_code(const std::vector<ClassCode>& c, const std::string& code) {
    for (const auto& e : c)
        if (e.code == code) return &e;
    return nullptr;
}

// ---- vector writers ----

bool write_dxf(const std::string& path, const std::vector<OutFeature>& features, const DxfOptions& o,
               std::string& error) {
    std::ofstream f;
    if (!open_out(path, f, error)) return false;
    auto g = [&](int code, const std::string& v) { f << code << "\r\n" << v << "\r\n"; };
    std::vector<std::pair<std::string, int>> layers{{"0", 7}};
    for (const auto& e : features) {
        const std::string l = dxf_layer(e.layer.empty() ? e.code : e.layer);
        if (std::none_of(layers.begin(), layers.end(), [&](const auto& p) { return p.first == l; }))
            layers.push_back({l, e.aci});
    }
    g(0, "SECTION");
    g(2, "HEADER");
    g(9, "$ACADVER");
    g(1, "AC1009");
    g(9, "$DWGCODEPAGE");
    g(3, "ANSI_1251");
    g(0, "ENDSEC");
    g(0, "SECTION");
    g(2, "TABLES");
    g(0, "TABLE");
    g(2, "LTYPE");
    g(70, "1");
    g(0, "LTYPE");
    g(2, "CONTINUOUS");
    g(70, "0");
    g(3, "Solid line");
    g(72, "65");
    g(73, "0");
    g(40, "0.0");
    g(0, "ENDTAB");
    g(0, "TABLE");
    g(2, "LAYER");
    g(70, std::to_string(layers.size()));
    for (const auto& [name, aci] : layers) {
        g(0, "LAYER");
        g(2, name);
        g(70, "0");
        g(62, std::to_string(aci));
        g(6, "CONTINUOUS");
    }
    g(0, "ENDTAB");
    g(0, "ENDSEC");
    g(0, "SECTION");
    g(2, "ENTITIES");
    auto xyz = [&](const std::array<double, 3>& p) {
        g(10, num(p[1]));   // east
        g(20, num(p[0]));   // north
        g(30, num(p[2]));
    };
    auto text = [&](const std::string& layer, double x, double y, double z, const std::string& s) {
        g(0, "TEXT");
        g(8, layer);
        g(10, num(x));
        g(20, num(y));
        g(30, num(z));
        g(40, num(o.text_height, 3));
        g(1, dxf_text(s));
    };
    for (const auto& e : features) {
        if (e.xyh.empty()) continue;
        const std::string layer = dxf_layer(e.layer.empty() ? e.code : e.layer);
        if (e.kind == FeatureKind::Point) {
            for (const auto& p : e.xyh) {
                g(0, "POINT");
                g(8, layer);
                xyz(p);
                const double dx = o.text_height * 0.4;
                if (o.name_labels && !e.name.empty())
                    text(layer, p[1] + dx, p[0] + dx, p[2], e.name);
                if (o.height_labels)
                    text(layer, p[1] + dx, p[0] - dx - o.text_height, p[2], num(p[2], 2));
            }
            continue;
        }
        g(0, "POLYLINE");
        g(8, layer);
        g(66, "1");
        g(10, "0.0");
        g(20, "0.0");
        g(30, "0.0");
        g(70, std::to_string(8 | (e.kind == FeatureKind::Polygon ? 1 : 0)));
        for (const auto& p : e.xyh) {
            g(0, "VERTEX");
            g(8, layer);
            xyz(p);
            g(70, "32");
        }
        g(0, "SEQEND");
        g(8, layer);
        if (o.name_labels && !e.name.empty())
            text(layer, e.xyh[0][1], e.xyh[0][0], e.xyh[0][2], e.name);
    }
    g(0, "ENDSEC");
    g(0, "EOF");
    f.close();
    if (!f) {
        error = "cannot write " + path;
        return false;
    }
    return true;
}

bool write_vectors_csv(const std::string& path, const std::vector<OutFeature>& features, std::string& error) {
    std::ofstream f;
    if (!open_out(path, f, error)) return false;
    for (const auto& e : features) {
        for (size_t i = 0; i < e.xyh.size(); i++) {
            std::string name = e.name.empty() ? e.code : e.name;
            if (e.kind != FeatureKind::Point || e.xyh.size() > 1) name += "." + std::to_string(i + 1);
            f << name << ';' << num(e.xyh[i][0], 3) << ';' << num(e.xyh[i][1], 3) << ';' << num(e.xyh[i][2], 3)
              << ';' << e.code << "\r\n";
        }
    }
    return (bool)f;
}

bool write_geojson(const std::string& path, const std::vector<OutFeature>& features, const Crs* crs,
                   std::string& error) {
    JsonWriter w;
    w.object();
    w.key("type").value("FeatureCollection");
    w.key("features").array();
    for (const auto& e : features) {
        if (e.xyh.empty()) continue;
        w.object();
        w.key("type").value("Feature");
        w.key("properties").object();
        w.key("code").value(e.code);
        w.key("name").value(e.name);
        w.key("layer").value(e.layer);
        w.end();
        w.key("geometry").object();
        auto coord = [&](const std::array<double, 3>& p) {
            w.array();
            if (crs) {
                double lat, lon;
                crs_to_wgs84(*crs, p[0], p[1], lat, lon);
                w.raw(num(lon, 9)).raw(num(lat, 9));
            } else {
                w.raw(num(p[1], 4)).raw(num(p[0], 4));
            }
            w.raw(num(p[2], 3));
            w.end();
        };
        if (e.kind == FeatureKind::Point && e.xyh.size() == 1) {
            w.key("type").value("Point");
            w.key("coordinates");
            coord(e.xyh[0]);
        } else if (e.kind == FeatureKind::Polygon) {
            w.key("type").value("Polygon");
            w.key("coordinates").array().array();
            for (const auto& p : e.xyh) coord(p);
            coord(e.xyh[0]);
            w.end().end();
        } else {
            w.key("type").value(e.kind == FeatureKind::Point ? "MultiPoint" : "LineString");
            w.key("coordinates").array();
            for (const auto& p : e.xyh) coord(p);
            w.end();
        }
        w.end();
        w.end();
    }
    w.end();
    w.end();
    return write_text(path, w.str(), error);
}

bool write_kml(const std::string& path, const std::vector<OutFeature>& features, const Crs& crs,
               std::string& error) {
    std::ostringstream k;
    k << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<kml xmlns=\"http://www.opengis.net/kml/2.2\">\n<Document>\n";
    auto coords = [&](const std::vector<std::array<double, 3>>& v, bool close) {
        for (size_t i = 0; i < v.size() + (close ? 1 : 0); i++) {
            const auto& p = v[i % v.size()];
            double lat, lon;
            crs_to_wgs84(crs, p[0], p[1], lat, lon);
            k << num(lon, 9) << ',' << num(lat, 9) << ',' << num(p[2], 3) << ' ';
        }
    };
    for (const auto& e : features) {
        if (e.xyh.empty()) continue;
        k << "<Placemark><name>" << xml_escape(e.name.empty() ? e.code : e.name) << "</name>";
        k << "<description>" << xml_escape(e.code) << "</description>";
        if (e.kind == FeatureKind::Point) {
            k << "<MultiGeometry>";
            for (const auto& p : e.xyh) {
                k << "<Point><coordinates>";
                coords({p}, false);
                k << "</coordinates></Point>";
            }
            k << "</MultiGeometry>";
        } else if (e.kind == FeatureKind::Polygon) {
            k << "<Polygon><outerBoundaryIs><LinearRing><coordinates>";
            coords(e.xyh, true);
            k << "</coordinates></LinearRing></outerBoundaryIs></Polygon>";
        } else {
            k << "<LineString><coordinates>";
            coords(e.xyh, false);
            k << "</coordinates></LineString>";
        }
        k << "</Placemark>\n";
    }
    k << "</Document>\n</kml>\n";
    return write_text(path, k.str(), error);
}

// ---- model writers ----

bool write_las(const std::string& path, const PointsOut& p, std::string& error) {
    std::ofstream f;
    if (!open_out(path, f, error)) return false;
    const double scale = 0.001;
    double lo[3] = {1e300, 1e300, 1e300}, hi[3] = {-1e300, -1e300, -1e300};
    uint32_t n = 0;
    for (int64_t i = 0; i < p.n; i++) {
        if (p.keep && !p.keep[i]) continue;
        for (int k = 0; k < 3; k++) {
            const double c = (double)p.xyz[i * 3 + k] + p.offset[k];
            lo[k] = std::min(lo[k], c);
            hi[k] = std::max(hi[k], c);
        }
        n++;
    }
    if (!n) {
        std::fill(lo, lo + 3, 0.0);
        std::fill(hi, hi + 3, 0.0);
    }
    std::vector<char> h(227, 0);
    size_t at = 0;
    auto put = [&](const void* v, size_t sz) {
        std::memcpy(h.data() + at, v, sz);
        at += sz;
    };
    auto u8 = [&](uint8_t v) { put(&v, 1); };
    auto u16 = [&](uint16_t v) { put(&v, 2); };
    auto u32 = [&](uint32_t v) { put(&v, 4); };
    auto f64 = [&](double v) { put(&v, 8); };
    put("LASF", 4);
    u16(0);
    u16(0);
    at += 16;   // GUID
    u8(1);
    u8(2);
    char sys[32] = "spirula-geo", soft[32] = "spirula-geo";
    put(sys, 32);
    put(soft, 32);
    const std::time_t now = std::time(nullptr);
    const std::tm* t = std::gmtime(&now);
    u16((uint16_t)(t ? t->tm_yday + 1 : 1));
    u16((uint16_t)(t ? t->tm_year + 1900 : 2026));
    u16(227);
    u32(227);
    u32(0);
    u8(2);
    u16(26);
    u32(n);
    u32(n);
    for (int k = 0; k < 4; k++) u32(0);
    for (int k = 0; k < 3; k++) f64(scale);
    for (int k = 0; k < 3; k++) f64(p.offset[k]);
    for (int k = 0; k < 3; k++) {
        f64(hi[k]);
        f64(lo[k]);
    }
    f.write(h.data(), (std::streamsize)h.size());
    std::vector<char> buf;
    buf.reserve(26 * 65536);
    for (int64_t i = 0; i < p.n; i++) {
        if (p.keep && !p.keep[i]) continue;
        char r[26] = {};
        for (int k = 0; k < 3; k++) {
            const int32_t q = (int32_t)std::llround((double)p.xyz[i * 3 + k] / scale);
            std::memcpy(r + 4 * k, &q, 4);
        }
        r[14] = 9;   // return 1 of 1
        if (p.rgb) {
            for (int k = 0; k < 3; k++) {
                const uint16_t c = (uint16_t)(p.rgb[i * 3 + k] * 257);
                std::memcpy(r + 20 + 2 * k, &c, 2);
            }
        }
        buf.insert(buf.end(), r, r + 26);
        if (buf.size() >= 26 * 65536) {
            f.write(buf.data(), (std::streamsize)buf.size());
            buf.clear();
        }
    }
    f.write(buf.data(), (std::streamsize)buf.size());
    f.close();
    if (!f) {
        error = "cannot write " + path;
        return false;
    }
    return true;
}

bool write_ply_double(const std::string& path, const PointsOut& p, std::string& error) {
    std::ofstream f;
    if (!open_out(path, f, error)) return false;
    int64_t n = 0;
    for (int64_t i = 0; i < p.n; i++) n += !p.keep || p.keep[i];
    f << "ply\nformat binary_little_endian 1.0\ncomment spirula-geo, full survey coordinates\n"
      << "element vertex " << n << "\nproperty double x\nproperty double y\nproperty double z\n";
    if (p.rgb) f << "property uchar red\nproperty uchar green\nproperty uchar blue\n";
    f << "end_header\n";
    std::vector<char> buf;
    for (int64_t i = 0; i < p.n; i++) {
        if (p.keep && !p.keep[i]) continue;
        for (int k = 0; k < 3; k++) {
            const double c = (double)p.xyz[i * 3 + k] + p.offset[k];
            const char* b = (const char*)&c;
            buf.insert(buf.end(), b, b + 8);
        }
        if (p.rgb) buf.insert(buf.end(), (const char*)p.rgb + i * 3, (const char*)p.rgb + i * 3 + 3);
        if (buf.size() > (1 << 22)) {
            f.write(buf.data(), (std::streamsize)buf.size());
            buf.clear();
        }
    }
    f.write(buf.data(), (std::streamsize)buf.size());
    f.close();
    if (!f) {
        error = "cannot write " + path;
        return false;
    }
    return true;
}

bool write_xyz(const std::string& path, const PointsOut& p, std::string& error) {
    std::ofstream f;
    if (!open_out(path, f, error)) return false;
    std::string out;
    char line[160];
    for (int64_t i = 0; i < p.n; i++) {
        if (p.keep && !p.keep[i]) continue;
        const double x = (double)p.xyz[i * 3] + p.offset[0], y = (double)p.xyz[i * 3 + 1] + p.offset[1],
                     z = (double)p.xyz[i * 3 + 2] + p.offset[2];
        int len;
        if (p.rgb)
            len = std::snprintf(line, sizeof line, "%.3f %.3f %.3f %d %d %d\n", x, y, z, p.rgb[i * 3],
                                p.rgb[i * 3 + 1], p.rgb[i * 3 + 2]);
        else
            len = std::snprintf(line, sizeof line, "%.3f %.3f %.3f\n", x, y, z);
        out.append(line, (size_t)len);
        if (out.size() > (1 << 22)) {
            f << out;
            out.clear();
        }
    }
    f << out;
    f.close();
    if (!f) {
        error = "cannot write " + path;
        return false;
    }
    return true;
}

bool obj_add_offset(const std::string& path, const double offset[3], std::string& error) {
    std::ifstream in(fs::u8path(path), std::ios::binary);
    if (!in) {
        error = "cannot open " + path;
        return false;
    }
    const fs::path tmp = fs::u8path(path + ".tmp");
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) {
        error = "cannot write " + path;
        return false;
    }
    std::string line;
    char buf[200];
    while (std::getline(in, line)) {
        if (line.size() > 2 && line[0] == 'v' && line[1] == ' ') {
            double x, y, z;
            if (std::sscanf(line.c_str() + 2, "%lf %lf %lf", &x, &y, &z) == 3) {
                std::snprintf(buf, sizeof buf, "v %.4f %.4f %.4f", x + offset[0], y + offset[1], z + offset[2]);
                out << buf << '\n';
                continue;
            }
        }
        out << line << '\n';
    }
    in.close();
    out.close();
    if (!out) {
        error = "cannot write " + path;
        return false;
    }
    std::error_code ec;
    fs::rename(tmp, fs::u8path(path), ec);
    if (ec) {
        fs::remove(fs::u8path(path), ec);
        fs::rename(tmp, fs::u8path(path), ec);
    }
    if (ec) {
        error = ec.message();
        return false;
    }
    return true;
}

// ---- measures ----

double plan_length(const std::vector<std::array<double, 3>>& v, bool closed) {
    double s = 0;
    ring(v, closed, [&](const auto& a, const auto& b) { s += std::hypot(b[0] - a[0], b[1] - a[1]); });
    return s;
}

double slope_length(const std::vector<std::array<double, 3>>& v, bool closed) {
    double s = 0;
    ring(v, closed, [&](const auto& a, const auto& b) {
        s += std::sqrt((b[0] - a[0]) * (b[0] - a[0]) + (b[1] - a[1]) * (b[1] - a[1]) + (b[2] - a[2]) * (b[2] - a[2]));
    });
    return s;
}

double plan_area(const std::vector<std::array<double, 3>>& v) {
    if (v.size() < 3) return 0;
    double s = 0;
    const double x0 = v[0][0], y0 = v[0][1];
    for (size_t i = 0; i < v.size(); i++) {
        const auto& a = v[i];
        const auto& b = v[(i + 1) % v.size()];
        s += (a[0] - x0) * (b[1] - y0) - (b[0] - x0) * (a[1] - y0);
    }
    return std::fabs(s) * 0.5;
}

// ---- pickets of a DXF sketch ----

std::string utf8_guess(const std::string& s) { return utf8_of(code_points(s)); }

namespace {

// Also undoes DXF's \U+XXXX escapes.
std::wstring wide_of(const std::string& s) {
    std::wstring w;
    for (uint32_t c : code_points(s)) w += (wchar_t)(c < 0xFFFF ? c : L'?');
    std::wstring o;
    for (size_t i = 0; i < w.size(); i++) {
        if (w[i] == L'\\' && i + 6 < w.size() && w[i + 1] == L'U' && w[i + 2] == L'+') {
            const unsigned long v = std::wcstoul(w.substr(i + 3, 4).c_str(), nullptr, 16);
            if (v) {
                o += (wchar_t)v;
                i += 6;
                continue;
            }
        }
        o += w[i];
    }
    return o;
}

std::string narrow_of(const std::wstring& w) {
    std::vector<uint32_t> c(w.begin(), w.end());
    return utf8_of(c);
}

std::wstring trimmed(std::wstring s) {
    while (!s.empty() && iswspace(s.back())) s.pop_back();
    size_t k = 0;
    while (k < s.size() && iswspace(s[k])) k++;
    return s.substr(k);
}

// The labels beside a point -> name, height, code: "pt5(Stairs)" is a name
// and a code, "(pole)" a code, "153,64" a height; coordinates are skipped.
void classify_labels(const std::vector<std::wstring>& texts, std::string& name, std::string& code, double& h,
                     bool& has_h) {
    static const std::wregex height(L"^[+-]?\\d{1,4}[.,]\\d{1,3}$");
    static const std::wregex number(L"^[A-Za-z\\u0400-\\u04FF]{0,4}[-_.]?\\d{1,6}[A-Za-z\\u0400-\\u04FF]?$");
    static const std::wregex coords(L"^[+-]?\\d{4,}(?:[.,]\\d+)?[\\s;,]+[+-]?\\d{4,}(?:[.,]\\d+)?$");
    static const std::wregex name_code(L"^\\s*([^()\\s][^()]*?)\\s*\\(([^()]+)\\)\\s*$");
    static const std::wregex any(L"[0-9A-Za-z\\u0400-\\u04FF]");
    static const std::wregex letter(L"[A-Za-z\\u0400-\\u04FF]");
    std::wstring wname, wcode;
    for (const auto& t : texts) {
        const std::wstring s = trimmed(t);
        if (s.empty() || !std::regex_search(s, any) || std::regex_match(s, coords)) continue;
        std::wsmatch m;
        if (s.front() == L'(' && s.back() == L')') {
            if (wcode.empty()) wcode = trimmed(s.substr(1, s.size() - 2));
        } else if (std::regex_match(s, m, name_code) && !std::regex_match(m[1].str(), height)) {
            if (wname.empty()) wname = trimmed(m[1].str());
            if (wcode.empty()) wcode = trimmed(m[2].str());
        } else if (std::regex_match(s, height)) {
            if (!has_h) {
                std::wstring v = s;
                std::replace(v.begin(), v.end(), L',', L'.');
                h = std::wcstod(v.c_str(), nullptr);
                has_h = true;
            }
        } else if (std::regex_match(s, number) && wname.empty()) {
            wname = s;
        } else if (std::regex_search(s, letter)) {
            if (wcode.empty()) wcode = s;
        } else if (wname.empty()) {
            wname = s;
        }
    }
    name = narrow_of(wname);
    code = narrow_of(wcode);
}

// MTEXT without its formatting codes.
std::wstring mtext_plain(const std::wstring& s) {
    std::wstring o;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == L'\\' && i + 1 < s.size()) {
            const wchar_t c = s[i + 1];
            if (c == L'P' || c == L'~') {
                o += L' ';
                i++;
            } else if (c == L'\\' || c == L'{' || c == L'}') {
                o += c;
                i++;
            } else {
                const size_t semi = s.find(L';', i);
                if (semi == std::wstring::npos || semi - i > 40) i++;
                else i = semi;
            }
        } else if (s[i] != L'{' && s[i] != L'}') {
            o += s[i];
        }
    }
    return o;
}

}  // namespace

bool read_dxf_pickets(const std::string& path, std::vector<Picket>& out, std::string& error) {
    std::string text;
    if (!read_text(path, text, error)) return false;
    struct Ent {
        std::string type, layer;
        std::wstring text;
        double p[3] = {0, 0, 0}, a[2] = {0, 0}, height = 0;
        bool has_a = false, aligned = false, attribs = false;
    };
    std::vector<Ent> ents;
    {
        std::istringstream in(text);
        std::string code, value;
        bool in_entities = false, section_name = false;
        Ent cur;
        bool open = false;
        auto flush = [&] {
            if (open) ents.push_back(cur);
            open = false;
            cur = Ent{};
        };
        while (std::getline(in, code) && std::getline(in, value)) {
            while (!value.empty() && (value.back() == '\r' || value.back() == ' ')) value.pop_back();
            const int g = std::atoi(code.c_str());
            if (g == 0) {
                flush();
                section_name = value == "SECTION";
                if (value == "ENDSEC") in_entities = false;
                if (in_entities) {
                    cur.type = value;
                    open = true;
                }
                continue;
            }
            if (section_name && g == 2) {
                in_entities = value == "ENTITIES";
                section_name = false;
                continue;
            }
            if (!open) continue;
            const double v = std::atof(value.c_str());
            switch (g) {
                case 8: cur.layer = narrow_of(wide_of(value)); break;
                case 1: cur.text += wide_of(value); break;
                case 3:
                    if (cur.type == "MTEXT") cur.text += wide_of(value);
                    break;
                case 10: cur.p[0] = v; break;
                case 20: cur.p[1] = v; break;
                case 30: cur.p[2] = v; break;
                case 11:
                    cur.a[0] = v;
                    cur.has_a = true;
                    break;
                case 21: cur.a[1] = v; break;
                case 40: cur.height = v; break;
                case 66: cur.attribs = v != 0; break;
                case 72:
                case 73:
                    if (v != 0) cur.aligned = true;
                    break;
                default: break;
            }
        }
        flush();
    }

    struct Pt {
        double e, n, z;
        std::string layer;
        std::vector<std::pair<double, std::wstring>> labels;
    };
    std::vector<Pt> pts;
    std::vector<Picket> found;
    std::vector<std::array<double, 2>> label_at;
    std::vector<std::wstring> label_text;
    std::vector<double> heights;
    for (size_t i = 0; i < ents.size(); i++) {
        const Ent& e = ents[i];
        if (e.type == "POINT") {
            pts.push_back({e.p[0], e.p[1], e.p[2], e.layer, {}});
        } else if (e.type == "INSERT" && e.attribs) {
            std::vector<std::wstring> attrs;
            for (size_t k = i + 1; k < ents.size() && ents[k].type == "ATTRIB"; k++) attrs.push_back(ents[k].text);
            if (attrs.empty()) continue;
            Picket p;
            classify_labels(attrs, p.name, p.code, p.h, p.has_h);
            if (!p.has_h && std::fabs(e.p[2]) > 1e-6) {
                p.h = e.p[2];
                p.has_h = true;
            }
            p.x = e.p[1];
            p.y = e.p[0];
            p.layer = e.layer;
            found.push_back(p);
        } else if (e.type == "TEXT" || e.type == "MTEXT") {
            const std::wstring t = trimmed(e.type == "MTEXT" ? mtext_plain(e.text) : e.text);
            if (t.empty()) continue;
            const bool use_a = e.type == "TEXT" && e.aligned && e.has_a;
            label_at.push_back({use_a ? e.a[0] : e.p[0], use_a ? e.a[1] : e.p[1]});
            label_text.push_back(t);
            if (e.height > 0) heights.push_back(e.height);
        }
    }
    if (!pts.empty()) {
        double th = 1.0;
        if (!heights.empty()) {
            std::nth_element(heights.begin(), heights.begin() + heights.size() / 2, heights.end());
            th = heights[heights.size() / 2];
        }
        const double reach = std::max(4 * th, 1.0);
        for (size_t t = 0; t < label_at.size(); t++) {
            double best = reach;
            int at = -1;
            for (size_t k = 0; k < pts.size(); k++) {
                const double d = std::hypot(pts[k].e - label_at[t][0], pts[k].n - label_at[t][1]);
                if (d <= best) {
                    best = d;
                    at = (int)k;
                }
            }
            if (at >= 0) pts[(size_t)at].labels.push_back({best, label_text[t]});
        }
        for (auto& q : pts) {
            std::sort(q.labels.begin(), q.labels.end());
            std::vector<std::wstring> texts;
            for (const auto& l : q.labels) texts.push_back(l.second);
            Picket p;
            classify_labels(texts, p.name, p.code, p.h, p.has_h);
            if (std::fabs(q.z) > 1e-6) {
                p.h = q.z;
                p.has_h = true;
            }
            p.x = q.n;
            p.y = q.e;
            p.layer = q.layer;
            found.push_back(p);
        }
    }
    if (found.empty()) {
        error = "no POINT entities or blocks with attributes in " + path;
        return false;
    }
    out = std::move(found);
    return true;
}

}  // namespace geo
}  // namespace spirula
