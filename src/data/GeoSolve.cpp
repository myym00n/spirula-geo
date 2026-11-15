#include "data/GeoSolve.h"

#include "data/CameraMath.h"
#include "data/ImageProbe.h"
#include "data/MarkerAlign.h"
#include "data/MarkerTriangulate.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>

namespace spirula {
namespace geo {

namespace {

std::string base_name(const std::string& path) {
    const size_t k = path.find_last_of("/\\");
    return k == std::string::npos ? path : path.substr(k + 1);
}

int quarter_turns(const ParsedDataset& ds, int cam) {
    return ds.exif_quarter_turns.empty() ? 0 : (int)ds.exif_quarter_turns[(size_t)cam] & 3;
}

// The file's pixels as displayed (EXIF quarter turns applied) -> the camera's.
bool file_scale(const ParsedDataset& ds, int cam, double& sx, double& sy) {
    int w = 0, h = 0;
    if (!probe_image_size(ds.image_filenames[(size_t)cam].c_str(), &w, &h) || w <= 0 || h <= 0)
        return false;
    if (quarter_turns(ds, cam) & 1) std::swap(w, h);
    sx = (double)ds.widths[(size_t)cam] / w;
    sy = (double)ds.heights[(size_t)cam] / h;
    return true;
}

bool cam_ray(const ParsedDataset& ds, int cam, double px, double py, double o[3], double d[3]) {
    const float* c2w = &ds.c2w[(size_t)cam * 12];
    const float* in = &ds.intrins[(size_t)cam * 4];
    if (in[0] <= 0 || in[1] <= 0) return false;
    double ray[3];
    if (!camhost::generate_ray((px - in[2]) / in[0], (py - in[3]) / in[1], ds.camera_models[(size_t)cam],
                               ds.camera_distortions[(size_t)cam], &ds.dist_coeffs[(size_t)cam * 8], ray))
        return false;
    const double gl[3] = {ray[0], -ray[1], -ray[2]};   // c2w is OpenGL-style
    for (int r = 0; r < 3; r++) {
        o[r] = c2w[r * 4 + 3];
        d[r] = c2w[r * 4 + 0] * gl[0] + c2w[r * 4 + 1] * gl[1] + c2w[r * 4 + 2] * gl[2];
    }
    const double n = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (n < 1e-12) return false;
    for (int r = 0; r < 3; r++) d[r] /= n;
    return true;
}

bool cam_project(const ParsedDataset& ds, int cam, const double p[3], double& px, double& py) {
    const float* c2w = &ds.c2w[(size_t)cam * 12];
    const float* in = &ds.intrins[(size_t)cam * 4];
    const double q[3] = {p[0] - c2w[3], p[1] - c2w[7], p[2] - c2w[11]};
    double gl[3];
    for (int c = 0; c < 3; c++) gl[c] = c2w[0 * 4 + c] * q[0] + c2w[1 * 4 + c] * q[1] + c2w[2 * 4 + c] * q[2];
    const double ray[3] = {gl[0], -gl[1], -gl[2]};
    if (ray[2] <= 1e-9) return false;
    double n[2];
    if (!camhost::project_ray(ray, ds.camera_models[(size_t)cam], ds.camera_distortions[(size_t)cam],
                              &ds.dist_coeffs[(size_t)cam * 8], n))
        return false;
    px = n[0] * in[0] + in[2];
    py = n[1] * in[1] + in[3];
    return true;
}

}  // namespace

std::vector<std::pair<std::string, int>> image_index(const ParsedDataset& ds) {
    std::vector<std::pair<std::string, int>> out;
    out.reserve(ds.image_filenames.size());
    for (size_t i = 0; i < ds.image_filenames.size(); i++)
        out.emplace_back(base_name(ds.image_filenames[i]), (int)i);
    std::sort(out.begin(), out.end());
    return out;
}

void local_to_survey(const Solution& s, const double l[3], double& x, double& y, double& h) {
    y = l[0] + s.offset[0];
    x = l[1] + s.offset[1];
    h = l[2] + s.offset[2];
}

void survey_to_local(const Solution& s, double x, double y, double h, double l[3]) {
    l[0] = y - s.offset[0];
    l[1] = x - s.offset[1];
    l[2] = h - s.offset[2];
}

bool project_to_image(const ParsedDataset& ds, int cam, const Sim3& model_from_dataset,
                      const double model[3], int file_w, int file_h, double& u, double& v) {
    double p[3];
    model_from_dataset.inverse().apply(model, p);
    double px, py;
    if (!cam_project(ds, cam, p, px, py)) return false;
    u = px * file_w / ds.widths[(size_t)cam];
    v = py * file_h / ds.heights[(size_t)cam];
    return u >= 0 && v >= 0 && u < file_w && v < file_h;
}

Solution solve(const GeoProject& p, const ParsedDataset& ds, const Sim3& model_from_dataset) {
    Solution s;
    const auto index = image_index(ds);
    auto cam_of = [&](const std::string& img) {
        auto it = std::lower_bound(index.begin(), index.end(), std::make_pair(img, -1));
        return it != index.end() && it->first == img ? it->second : -1;
    };
    std::map<int, std::array<double, 2>> scale;

    for (const auto& [name, imgs] : p.marks) {
        PointResult r;
        r.name = name;
        const GeoPoint* g = p.find(name);
        r.role = g ? g->role : Role::Off;
        std::vector<double> O, D;
        std::vector<std::array<double, 3>> obs;      // cam, px, py
        for (const auto& [img, m] : imgs) {
            const int cam = cam_of(img);
            if (cam < 0) continue;
            auto sc = scale.find(cam);
            if (sc == scale.end()) {
                double sx = 1, sy = 1;
                if (!file_scale(ds, cam, sx, sy)) continue;
                sc = scale.emplace(cam, std::array<double, 2>{sx, sy}).first;
            }
            const double px = m.u * sc->second[0], py = m.v * sc->second[1];
            double o[3], d[3];
            if (!cam_ray(ds, cam, px, py, o, d)) continue;
            O.insert(O.end(), o, o + 3);
            D.insert(D.end(), d, d + 3);
            obs.push_back({(double)cam, px, py});
        }
        r.marks = (int)obs.size();
        double X[3];
        if (r.marks >= 2 && intersect_rays(O.data(), D.data(), r.marks, X)) {
            double sum = 0;
            for (const auto& o : obs) {
                double u, v;
                double e = 1e3;
                if (cam_project(ds, (int)o[0], X, u, v)) e = std::hypot(u - o[1], v - o[2]);
                sum += e * e;
                if (e > r.reproj_max) {
                    r.reproj_max = e;
                    r.worst_image = base_name(ds.image_filenames[(size_t)o[0]]);
                }
            }
            r.reproj_rms = std::sqrt(sum / r.marks);
            model_from_dataset.apply(X, r.model);
            r.triangulated = true;
        }
        s.points.push_back(r);
    }

    std::vector<std::array<double, 3>> src, dst;
    double mean[3] = {0, 0, 0};
    for (const auto& r : s.points) {
        const GeoPoint* g = p.find(r.name);
        if (!r.triangulated || !g || g->role != Role::Control || !g->has_h) continue;
        src.push_back({r.model[0], r.model[1], r.model[2]});
        dst.push_back({g->y, g->x, g->h});
        for (int k = 0; k < 3; k++) mean[k] += dst.back()[k];
    }
    s.n_control = (int)src.size();
    if (s.n_control < 3) {
        s.error = "need 3 control points with height, each marked on 2+ photos";
        return s;
    }
    // Survey coordinates are millions of metres; the model is kept near zero
    // so float32 splat positions still hold millimetres.
    for (int k = 0; k < 3; k++) s.offset[k] = std::round(mean[k] / s.n_control / 100.0) * 100.0;
    for (auto& q : dst)
        for (int k = 0; k < 3; k++) q[k] -= s.offset[k];
    if (!estimate_sim3_umeyama(src, dst, s.model_to_local)) {
        s.error = "control points are degenerate (on one line?)";
        return s;
    }
    s.ok = true;
    double pc = 0, hc = 0, pk = 0, hk = 0;
    for (auto& r : s.points) {
        if (!r.triangulated) continue;
        double l[3];
        s.model_to_local.apply(r.model, l);
        local_to_survey(s, l, r.x, r.y, r.h);
        r.fitted = true;
        const GeoPoint* g = p.find(r.name);
        if (!g) continue;
        r.dx = r.x - g->x;
        r.dy = r.y - g->y;
        r.dh = g->has_h ? r.h - g->h : 0.0;
        const double plan2 = r.dx * r.dx + r.dy * r.dy;
        if (g->role == Role::Control && g->has_h) {
            pc += plan2;
            hc += r.dh * r.dh;
        } else if (g->role == Role::Check) {
            s.n_check++;
            pk += plan2;
            hk += r.dh * r.dh;
        }
    }
    s.rms_plan_control = std::sqrt(pc / s.n_control);
    s.rms_h_control = std::sqrt(hc / s.n_control);
    if (s.n_check) {
        s.rms_plan_check = std::sqrt(pk / s.n_check);
        s.rms_h_check = std::sqrt(hk / s.n_check);
    }
    return s;
}

}  // namespace geo
}  // namespace spirula
