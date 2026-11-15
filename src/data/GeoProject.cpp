#include "data/GeoProject.h"

#include "core/AtomicFile.h"
#include "data/GeoCrs.h"
#include "data/Json.h"
#include "data/JsonWrite.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace spirula {
namespace geo {

namespace fs = std::filesystem;

GeoPoint* GeoProject::find(const std::string& name) {
    for (auto& p : points)
        if (p.name == name) return &p;
    return nullptr;
}

const GeoPoint* GeoProject::find(const std::string& name) const {
    for (const auto& p : points)
        if (p.name == name) return &p;
    return nullptr;
}

int GeoProject::mark_count(const std::string& name) const {
    auto it = marks.find(name);
    return it == marks.end() ? 0 : (int)it->second.size();
}

void GeoProject::remove_point(const std::string& name) {
    points.erase(std::remove_if(points.begin(), points.end(),
                                [&](const GeoPoint& p) { return p.name == name; }),
                 points.end());
    marks.erase(name);
}

const char* role_key(Role r) {
    switch (r) {
        case Role::Check: return "check";
        case Role::Off: return "off";
        default: return "control";
    }
}

Role role_from_key(const std::string& k) {
    // mooncloud writes the Russian words
    if (k == "check" || k == "\xD0\xBA\xD0\xBE\xD0\xBD\xD1\x82\xD1\x80\xD0\xBE\xD0\xBB\xD1\x8C\xD0\xBD\xD0\xB0\xD1\x8F")
        return Role::Check;
    if (k == "off" || k == "\xD0\xB2\xD1\x8B\xD0\xBA\xD0\xBB") return Role::Off;
    return Role::Control;
}

bool read_text(const std::string& path, std::string& out, std::string& error) {
    std::ifstream f(fs::u8path(path), std::ios::binary);
    if (!f) {
        error = "cannot open " + path;
        return false;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    if (out.size() >= 3 && (unsigned char)out[0] == 0xEF && (unsigned char)out[1] == 0xBB &&
        (unsigned char)out[2] == 0xBF)
        out.erase(0, 3);
    return true;
}

bool write_text(const std::string& path, const std::string& text, std::string& error) {
    const fs::path dst = fs::u8path(path);
    fs::path tmp = dst;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) {
            error = "cannot write " + path;
            return false;
        }
        f.write(text.data(), (std::streamsize)text.size());
        f.close();
        if (!f) {
            error = "cannot write " + path;
            return false;
        }
    }
    try {
        replace_file(tmp, dst);
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
    return true;
}

std::string geo_path(const std::string& dataset_dir) {
    return (fs::path(dataset_dir) / "geo.json").string();
}

namespace {

std::string file_name(const std::string& path) {
    const size_t k = path.find_last_of("/\\");
    return k == std::string::npos ? path : path.substr(k + 1);
}

bool parse_number(std::string s, double& v) {
    std::replace(s.begin(), s.end(), ',', '.');
    if (s.empty()) return false;
    char* end = nullptr;
    v = std::strtod(s.c_str(), &end);
    return end && *end == '\0';
}

std::vector<std::string> split_fields(const std::string& line) {
    std::vector<std::string> out;
    const bool strong = line.find(';') != std::string::npos || line.find('\t') != std::string::npos;
    std::string cur;
    auto flush = [&]() {
        if (!cur.empty() || strong) out.push_back(cur);
        cur.clear();
    };
    for (char ch : line) {
        const bool sep = strong ? (ch == ';' || ch == '\t') : (ch == ' ' || ch == ',');
        if (sep) flush();
        else if (!(strong && ch == ' ' && cur.empty())) cur += ch;
    }
    flush();
    for (auto& f : out)
        while (!f.empty() && (f.back() == ' ' || f.back() == '\r')) f.pop_back();
    out.erase(std::remove(out.begin(), out.end(), std::string()), out.end());
    return out;
}

}  // namespace

bool load_geo(const std::string& path, GeoProject& out, std::string& error) {
    std::string text;
    if (!read_text(path, text, error)) return false;
    try {
        const JsonValue j = json_parse(text);
        GeoProject p;
        if (const JsonValue* c = j.find("crs")) p.crs_name = c->as_string();
        if (const JsonValue* c = j.find("crs_def")) p.crs_def = c->as_string();
        if (const JsonValue* o = j.find("offset"); o && o->is_array() && o->arr.size() == 3) {
            p.placed = true;
            for (int k = 0; k < 3; k++) p.offset[k] = o->arr[(size_t)k].as_double();
        }
        if (const JsonValue* m = j.find("file_from_dataset"); m && m->is_object()) {
            const JsonValue* R = m->find("R");
            const JsonValue* t = m->find("t");
            if (R && t && R->is_array() && t->is_array() && R->arr.size() == 9 && t->arr.size() == 3) {
                p.model_frame = true;
                p.file_from_dataset.s = m->get_double("s", 1.0);
                for (int k = 0; k < 9; k++) p.file_from_dataset.R[k] = R->arr[(size_t)k].as_double();
                for (int k = 0; k < 3; k++) p.file_from_dataset.t[k] = t->arr[(size_t)k].as_double();
            }
        }
        if (const JsonValue* pts = j.find("points"); pts && pts->is_array()) {
            for (const auto& v : pts->arr) {
                GeoPoint g;
                if (const JsonValue* n = v.find("name")) g.name = n->as_string();
                g.x = v.get_double("x", 0.0);
                g.y = v.get_double("y", 0.0);
                const JsonValue* h = v.find("h");
                g.has_h = h && !h->is_null();
                g.h = g.has_h ? h->as_double() : 0.0;
                if (const JsonValue* r = v.find("role")) g.role = role_from_key(r->as_string());
                if (const JsonValue* c = v.find("code")) g.code = c->as_string();
                if (!g.name.empty()) p.points.push_back(g);
            }
        }
        if (const JsonValue* m = j.find("marks"); m && m->is_object()) {
            for (const auto& [name, imgs] : m->obj) {
                for (const auto& [img, uv] : imgs.obj) {
                    if (!uv.is_array() || uv.arr.size() < 2) continue;
                    p.marks[name][img] = {uv.arr[0].as_double(), uv.arr[1].as_double()};
                }
            }
        }
        out = std::move(p);
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

bool save_geo(const std::string& path, const GeoProject& p, std::string& error) {
    JsonWriter w;
    w.object();
    w.key("version").value(1);
    w.key("crs").value(p.crs_name);
    w.key("crs_def").value(p.crs_def);
    if (p.placed) {
        w.key("offset").array();
        for (int k = 0; k < 3; k++) w.raw(json_number_exact(p.offset[k]));
        w.end();
    }
    if (p.model_frame) {
        const Sim3& m = p.file_from_dataset;
        w.key("file_from_dataset").object();
        w.key("s").raw(json_number_exact(m.s));
        w.key("R").array();
        for (double v : m.R) w.raw(json_number_exact(v));
        w.end();
        w.key("t").array();
        for (double v : m.t) w.raw(json_number_exact(v));
        w.end();
        w.end();
    }
    w.key("points").array();
    for (const auto& g : p.points) {
        w.object();
        w.key("name").value(g.name);
        w.key("x").raw(json_number_exact(g.x));
        w.key("y").raw(json_number_exact(g.y));
        if (g.has_h) w.key("h").raw(json_number_exact(g.h));
        else w.key("h").raw("null");
        w.key("role").value(role_key(g.role));
        if (!g.code.empty()) w.key("code").value(g.code);
        w.end();
    }
    w.end();
    w.key("marks").object();
    for (const auto& [name, imgs] : p.marks) {
        if (imgs.empty()) continue;
        w.key(name.c_str()).object();
        for (const auto& [img, m] : imgs) {
            w.key(img.c_str()).array();
            w.raw(json_number_exact(m.u)).raw(json_number_exact(m.v));
            w.end();
        }
        w.end();
    }
    w.end();
    w.end();
    return write_text(path, w.str(), error);
}

int import_catalog(const std::string& path, GeoProject& p, std::string& error, const Crs* latlon_into) {
    std::string text;
    if (!read_text(path, text, error)) return -1;
    std::istringstream in(text);
    std::string line;
    int n = 0;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        const auto f = split_fields(line);
        if (f.size() < 3) continue;
        GeoPoint g;
        g.name = f[0];
        double x, y, h = 0;
        if (!parse_number(f[1], x) || !parse_number(f[2], y)) continue;   // header row
        if (latlon_into) wgs84_to_crs(*latlon_into, x, y, x, y);
        g.x = x;
        g.y = y;
        g.has_h = f.size() >= 4 && parse_number(f[3], h);
        g.h = g.has_h ? h : 0.0;
        if (f.size() >= 5) g.code = f[4];
        if (GeoPoint* old = p.find(g.name)) {
            g.role = old->role;
            *old = g;
        } else {
            p.points.push_back(g);
        }
        n++;
    }
    if (!n) error = "no rows of the form: name x y h";
    return n;
}

int import_mooncloud(const std::string& path, GeoProject& p, std::string& error) {
    std::string text;
    if (!read_text(path, text, error)) return -1;
    try {
        const JsonValue j = json_parse(text);
        int n = 0;
        if (const JsonValue* g = j.find("gcps"); g && g->is_array()) {
            for (const auto& v : g->arr) {
                GeoPoint pt;
                if (const JsonValue* nm = v.find("name")) pt.name = nm->as_string();
                if (pt.name.empty()) continue;
                pt.x = v.get_double("x", 0.0);
                pt.y = v.get_double("y", 0.0);
                const JsonValue* h = v.find("h");
                pt.has_h = h && !h->is_null();
                pt.h = pt.has_h ? h->as_double() : 0.0;
                if (const JsonValue* r = v.find("role")) pt.role = role_from_key(r->as_string());
                if (GeoPoint* old = p.find(pt.name)) *old = pt;
                else p.points.push_back(pt);
                n++;
            }
        }
        if (const JsonValue* o = j.find("observations"); o && o->is_object()) {
            for (const auto& [name, imgs] : o->obj) {
                for (const auto& [img, uv] : imgs.obj) {
                    if (!uv.is_array() || uv.arr.size() < 2) continue;
                    p.marks[name][file_name(img)] = {uv.arr[0].as_double(), uv.arr[1].as_double()};
                }
            }
        }
        if (!n && p.marks.empty()) error = "no gcps or observations in " + path;
        return n;
    } catch (const std::exception& e) {
        error = e.what();
        return -1;
    }
}

bool export_points_csv(const std::string& path, const std::vector<GeoPoint>& pts,
                       std::string& error) {
    std::ofstream f(fs::u8path(path), std::ios::binary | std::ios::trunc);
    if (!f) {
        error = "cannot write " + path;
        return false;
    }
    char buf[256];
    for (const auto& g : pts) {
        if (g.has_h)
            std::snprintf(buf, sizeof buf, ";%.3f;%.3f;%.3f;", g.x, g.y, g.h);
        else
            std::snprintf(buf, sizeof buf, ";%.3f;%.3f;;", g.x, g.y);
        f << g.name << buf << g.code << "\r\n";
    }
    f.close();
    if (!f) {
        error = "cannot write " + path;
        return false;
    }
    return true;
}

}  // namespace geo
}  // namespace spirula
