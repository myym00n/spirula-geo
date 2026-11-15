// EditMeasure.cpp -- the Measure, Draw and Export tabs: picking points on the
// model, figures in survey coordinates, features with the user's codes, and
// the model and the drawing written out in full coordinates.

#include "app/AppPaths.h"
#include "app/gui/Layout.h"
#include "app/gui/Ui.h"
#include "app/gui/edit/EditSession.h"
#include "app/gui/edit/GeoState.h"
#include "data/JsonWrite.h"
#include "data/Json.h"
#include "i18n/catalog/Geo.h"
#include "mesh/MeshExport.h"

#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>

namespace gmsg = spirula::i18n::msg::geo;
namespace geo = spirula::geo;
using spirula::Sim3;

namespace gui {

namespace {

constexpr int kMeasure = 3, kDraw = 4, kExport = 5;

std::string fixed(double v, int digits) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.*f", digits, v);
    return buf;
}

std::string settings_path() {
    return (std::filesystem::u8path(app::config_dir()) / "geo_settings.json").u8string();
}

std::string base_name(const std::string& p) {
    const size_t k = p.find_last_of("/\\");
    return k == std::string::npos ? p : p.substr(k + 1);
}

// The few ACI colours a classifier names most; the rest fall back to white.
ImU32 aci_colour(int aci) {
    static const ImU32 pal[] = {IM_COL32(255, 255, 255, 255), IM_COL32(255, 70, 70, 255),
                                IM_COL32(255, 235, 60, 255),  IM_COL32(80, 230, 90, 255),
                                IM_COL32(70, 230, 240, 255),  IM_COL32(80, 120, 255, 255),
                                IM_COL32(240, 80, 240, 255),  IM_COL32(255, 255, 255, 255),
                                IM_COL32(150, 150, 150, 255), IM_COL32(200, 200, 200, 255)};
    return aci >= 0 && aci < 10 ? pal[aci] : pal[7];
}

// "12" -> "13", "П-7" -> "П-8"; anything not ending in a digit stays.
std::string next_number(const std::string& s) {
    size_t k = s.size();
    while (k > 0 && std::isdigit((unsigned char)s[k - 1])) k--;
    if (k == s.size()) return s;
    const std::string digits = s.substr(k);
    std::string n = std::to_string(std::stoll(digits) + 1);
    if (n.size() < digits.size()) n = std::string(digits.size() - n.size(), '0') + n;
    return s.substr(0, k) + n;
}

void outlined(ImDrawList* dl, ImVec2 at, ImU32 col, const std::string& s) {
    dl->AddText(ImVec2(at.x + 1, at.y + 1), IM_COL32_BLACK, s.c_str());
    dl->AddText(at, col, s.c_str());
}

}  // namespace

// ---------------------------------------------------------------------------
// Frames and picking
// ---------------------------------------------------------------------------

bool EditSession::geo_owns_left() const { return _geo && (_tab == kMeasure || _tab == kDraw); }

std::array<double, 3> EditSession::geo_survey(const double saved[3]) const {
    const double zero[3] = {0, 0, 0};
    const double* off = _geo->gp.placed ? _geo->gp.offset : zero;
    return {saved[1] + off[1], saved[0] + off[0], saved[2] + off[2]};
}

void EditSession::geo_saved(const std::array<double, 3>& xyh, double saved[3]) const {
    const double zero[3] = {0, 0, 0};
    const double* off = _geo->gp.placed ? _geo->gp.offset : zero;
    saved[0] = xyh[1] - off[0];
    saved[1] = xyh[0] - off[1];
    saved[2] = xyh[2] - off[2];
}

bool EditSession::geo_pick(float x, float y, double xyh[3]) {
    ViewProjection vp;
    if (!_doc || !view(vp)) return false;
    const int was = _doc->layer();
    _doc->set_layer(0);
    const int64_t hit = pick_element(*_doc, vp, x, y, 16.0f);
    _doc->set_layer(was);
    if (hit < 0) return false;
    const float* p = _doc->positions_of(0) + hit * 3;
    const double pm[3] = {p[0], p[1], p[2]};
    double saved[3];
    (_doc->view_frame().inverse() * _doc->placement()).apply(pm, saved);
    const auto s = geo_survey(saved);
    std::copy(s.begin(), s.end(), xyh);
    return true;
}

bool EditSession::geo_input(const ViewportInput& in) {
    GeoState& g = *_geo;
    const bool moved = std::fabs(in.x - g.last_mouse[0]) > 1.0f || std::fabs(in.y - g.last_mouse[1]) > 1.0f;
    g.last_mouse[0] = in.x;
    g.last_mouse[1] = in.y;
    if (!in.hovered) {
        g.hover_ok = false;
        return false;
    }
    if (moved) g.pick_stale = true;
    if ((g.pick_stale && !moved) || in.clicked) {
        g.pick_stale = false;
        g.hover_ok = geo_pick(in.x, in.y, g.hover);
        g.snapped = false;
        // A vertex already drawn wins over the surface under it.
        XformFrame f;
        if (xform_frame(f)) {
            const Sim3 to_shared = saved_to_shared();
            float best = px(10);
            auto test = [&](const std::array<double, 3>& a) {
                double sv[3], w[3];
                geo_saved(a, sv);
                to_shared.apply(sv, w);
                const float q[3] = {(float)w[0], (float)w[1], (float)w[2]};
                float sx, sy, depth;
                if (!f.cam.project(q, sx, sy, depth) || !(depth > 0)) return;
                const float d = std::hypot(sx - in.x, sy - in.y);
                if (d < best) {
                    best = d;
                    std::copy(a.begin(), a.end(), g.hover);
                    g.hover_ok = g.snapped = true;
                }
            };
            for (const auto& ft : g.features)
                for (const auto& a : ft.v) test(a);
            for (const auto& a : g.pts) test(a);
        }
    }
    if (in.clicked && g.hover_ok) {
        const std::array<double, 3> a{g.hover[0], g.hover[1], g.hover[2]};
        if (_tab == kMeasure) {
            if (g.measure_kind == (int)geo::FeatureKind::Point || g.measure_done) g.pts.clear();
            g.measure_done = false;
            g.pts.push_back(a);
        } else {
            const geo::ClassCode* c = g.code >= 0 && g.code < (int)g.classes.size() ? &g.classes[(size_t)g.code] : nullptr;
            const auto kind = c ? c->kind : (geo::FeatureKind)g.draw_kind;
            if (kind == geo::FeatureKind::Point) geo_commit(kind, {a});
            else if (g.pts.empty() || g.pts.back() != a) g.pts.push_back(a);
        }
    }
    if (in.double_clicked && _tab == kDraw) geo_finish();
    return in.down || in.clicked;
}

void EditSession::geo_keys() {
    GeoState& g = *_geo;
    if (ImGui::GetIO().WantTextInput) return;
    if (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)) geo_finish();
    if (ImGui::IsKeyPressed(ImGuiKey_Backspace) && !g.pts.empty()) g.pts.pop_back();
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) g.pts.clear();
    if (_tab == kDraw && ImGui::IsKeyPressed(ImGuiKey_Delete) && g.feat_sel >= 0 &&
        g.feat_sel < (int)g.features.size()) {
        g.undo.push_back(g.features);
        g.features.erase(g.features.begin() + g.feat_sel);
        g.feat_sel = -1;
        geo_save_vectors();
    }
}

void EditSession::geo_finish() {
    GeoState& g = *_geo;
    if (_tab == kMeasure) g.measure_done = true;
    if (_tab != kDraw) return;
    const geo::ClassCode* c = g.code >= 0 && g.code < (int)g.classes.size() ? &g.classes[(size_t)g.code] : nullptr;
    const auto kind = c ? c->kind : (geo::FeatureKind)g.draw_kind;
    const size_t need = kind == geo::FeatureKind::Polygon ? 3 : 2;
    if (g.pts.size() >= need) geo_commit(kind, g.pts);
    g.pts.clear();
}

void EditSession::geo_commit(geo::FeatureKind kind, const std::vector<std::array<double, 3>>& pts) {
    GeoState& g = *_geo;
    geo::Feature f;
    f.kind = kind;
    const geo::ClassCode* c = g.code >= 0 && g.code < (int)g.classes.size() ? &g.classes[(size_t)g.code] : nullptr;
    f.code = c ? c->code : g.free_code;
    f.name = g.next_name;
    f.v = pts;
    g.undo.push_back(g.features);
    if (g.undo.size() > 50) g.undo.erase(g.undo.begin());
    g.features.push_back(std::move(f));
    g.feat_sel = (int)g.features.size() - 1;
    g.next_name = next_number(g.next_name);
    geo_save_vectors();
}

void EditSession::geo_save_vectors() {
    GeoState& g = *_geo;
    std::string err;
    if (!g.vec_path.empty() && !geo::save_vectors(g.vec_path, g.features, g.vec_survey || g.gp.placed, err)) {
        g.status = spirula::i18n::format(gmsg::save_failed, {g.vec_path, err});
        g.status_err = true;
    }
}

// ---------------------------------------------------------------------------
// Classifier, kept across sessions as a path
// ---------------------------------------------------------------------------

void EditSession::geo_load_classifier(const std::string& path) {
    GeoState& g = *_geo;
    std::string err;
    std::vector<geo::ClassCode> c;
    if (!geo::load_classifier(path, c, err)) {
        g.status = spirula::i18n::format(gmsg::import_failed, {base_name(path), err});
        g.status_err = true;
        return;
    }
    g.classes = std::move(c);
    g.class_path = path;
    g.code = g.classes.empty() ? -1 : 0;
    g.status = spirula::i18n::format(gmsg::cls_loaded, {(long long)g.classes.size()});
    g.status_err = false;
    JsonWriter w;
    w.object();
    w.key("classifier").value(path);
    w.end();
    geo::write_text(settings_path(), w.str(), err);
}

void EditSession::geo_load_classifier_setting() {
    GeoState& g = *_geo;
    std::string text, err;
    if (!std::filesystem::exists(std::filesystem::u8path(settings_path())) ||
        !geo::read_text(settings_path(), text, err))
        return;
    try {
        const JsonValue j = json_parse(text);
        const JsonValue* c = j.find("classifier");
        if (!c || c->as_string().empty()) return;
        std::vector<geo::ClassCode> v;
        if (geo::load_classifier(c->as_string(), v, err)) {
            g.classes = std::move(v);
            g.class_path = c->as_string();
            g.code = g.classes.empty() ? -1 : 0;
        }
    } catch (const std::exception&) {
    }
}

void EditSession::geo_after_save(const std::string& saved) {
    if (!_geo || !_geo->gp.placed || saved.empty() || _doc->dataset()) return;
    geo::GeoProject side = _geo->gp;
    side.model_frame = !_geo->md.directory.empty();
    side.file_from_dataset = geo_model_from_dataset();
    std::string err;
    geo::save_geo(saved + ".geo.json", side, err);
}

// ---------------------------------------------------------------------------
// The Measure tab
// ---------------------------------------------------------------------------

void EditSession::draw_measure_tab(float full) {
    geo_open();
    GeoState& g = *_geo;
    geo_keys();
    ui::TextDisabledWrapped(gmsg::measure_help);
    if (!g.gp.placed) ui::TextColoredWrapped(ImVec4(1, 0.7f, 0.3f, 1), gmsg::not_placed);
    if (g.hover_ok) {
        char line[160];
        std::snprintf(line, sizeof line, "X %.3f   Y %.3f   H %.3f", g.hover[0], g.hover[1], g.hover[2]);
        ui::TextRaw(line);
    } else {
        ui::TextDisabledRaw("X -   Y -   H -");
    }
    ImGui::Spacing();
    const int before = g.measure_kind;
    if (ui::RadioButton(gmsg::m_point, g.measure_kind == 0)) g.measure_kind = 0;
    ImGui::SameLine();
    if (ui::RadioButton(gmsg::m_line, g.measure_kind == 1)) g.measure_kind = 1;
    ImGui::SameLine();
    if (ui::RadioButton(gmsg::m_area, g.measure_kind == 2)) g.measure_kind = 2;
    if (before != g.measure_kind) g.pts.clear();

    const std::vector<std::array<double, 3>>& s = g.pts;
    ui::Text(gmsg::r_points, {(long long)s.size()});
    if (g.measure_kind == 0 && !s.empty()) {
        char line[160];
        std::snprintf(line, sizeof line, "X %.3f\nY %.3f\nH %.3f", s.back()[0], s.back()[1], s.back()[2]);
        ui::TextRaw(line);
    } else if (g.measure_kind == 1 && s.size() >= 2) {
        const double plan = geo::plan_length(s, false);
        const double dh = s.back()[2] - s.front()[2];
        ui::Text(gmsg::r_plan, {fixed(plan, 3)});
        ui::Text(gmsg::r_slope, {fixed(geo::slope_length(s, false), 3)});
        ui::Text(gmsg::r_dh, {fixed(dh, 3), fixed(plan > 1e-9 ? dh / plan * 100 : 0, 2)});
    } else if (g.measure_kind == 2 && s.size() >= 3) {
        ui::Text(gmsg::r_area, {fixed(geo::plan_area(s), 3)});
        ui::Text(gmsg::r_perim, {fixed(geo::plan_length(s, true), 3)});
    }
    const float half = (full - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
    ImGui::BeginDisabled(g.pts.empty());
    if (ui::Button(gmsg::keep_drawing, ImVec2(half, 0))) {
        geo_commit((geo::FeatureKind)g.measure_kind, g.pts);
        g.pts.clear();
    }
    ImGui::SameLine();
    if (ui::Button(gmsg::clear, ImVec2(half, 0))) g.pts.clear();
    ImGui::EndDisabled();
    if (!g.status.empty()) {
        if (g.status_err) ui::TextColoredWrappedRaw(ImVec4(1, 0.45f, 0.4f, 1), g.status);
        else ui::TextWrappedRaw(g.status);
    }
}

// ---------------------------------------------------------------------------
// The Draw tab
// ---------------------------------------------------------------------------

void EditSession::draw_draw_tab(float full) {
    geo_open();
    GeoState& g = *_geo;
    if (g.dlg.draw() && !g.dlg.result().empty()) geo_file_chosen(g.dlg.result());
    geo_keys();
    ui::TextDisabledWrapped(gmsg::draw_help);
    if (!g.gp.placed) ui::TextColoredWrapped(ImVec4(1, 0.7f, 0.3f, 1), gmsg::not_placed);

    if (ui::Button(gmsg::cls_load, ImVec2(full, 0))) {
        g.ask = GeoState::Ask::Classifier;
        g.dlg.open(gmsg::cls_load.get(), FileDialog::Mode::File, {".csv", ".txt", ".dxf"});
    }
    ui::help_on_hover(gmsg::cls_help);
    if (g.classes.empty()) {
        ui::TextDisabledWrapped(gmsg::cls_none);
        ui::Text(gmsg::code);
        ImGui::SetNextItemWidth(full);
        ui::InputTextRaw("##geofreecode", &g.free_code);
        if (ui::RadioButton(gmsg::kind_point, g.draw_kind == 0)) g.draw_kind = 0;
        ImGui::SameLine();
        if (ui::RadioButton(gmsg::kind_line, g.draw_kind == 1)) g.draw_kind = 1;
        ImGui::SameLine();
        if (ui::RadioButton(gmsg::kind_polygon, g.draw_kind == 2)) g.draw_kind = 2;
    } else {
        ui::TextDisabledRaw(base_name(g.class_path));
        ui::Text(gmsg::code);
        ImGui::SetNextItemWidth(full);
        const auto& cur = g.classes[(size_t)std::clamp(g.code, 0, (int)g.classes.size() - 1)];
        const std::string preview = cur.code + "  " + cur.name;
        if (ui::BeginComboRaw("##geocode", preview.c_str(), ImGuiComboFlags_HeightLarge)) {
            if (ImGui::IsWindowAppearing()) {
                g.code_filter.clear();
                ImGui::SetKeyboardFocusHere();
            }
            ImGui::SetNextItemWidth(-1);
            ui::InputTextWithHintRaw("##geocodefilter", gmsg::crs_filter, &g.code_filter);
            std::string f = g.code_filter;
            for (char& ch : f) ch = (char)std::tolower((unsigned char)ch);
            for (int i = 0; i < (int)g.classes.size(); i++) {
                const auto& c = g.classes[(size_t)i];
                std::string label = c.code + "  " + c.name;
                std::string l = label;
                for (char& ch : l) ch = (char)std::tolower((unsigned char)ch);
                if (!f.empty() && l.find(f) == std::string::npos) continue;
                ImGui::PushID(i);
                if (ui::SelectableRaw(label, i == g.code)) {
                    g.code = i;
                    g.pts.clear();
                }
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
    }
    ui::Text(gmsg::next_name);
    ImGui::SetNextItemWidth(full);
    ui::InputTextRaw("##geonextname", &g.next_name);
    ui::help_on_hover(gmsg::next_name_help);

    ui::Text(gmsg::drawn, {(long long)g.features.size()});
    if (!g.features.empty()) {
        const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit;
        const float rows = (float)std::min<size_t>(g.features.size(), 10) + 1.4f;
        if (ImGui::BeginTable("##geofeatures", 3, flags, ImVec2(full, ImGui::GetFrameHeightWithSpacing() * rows))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ui::TableSetupColumn(gmsg::next_name, ImGuiTableColumnFlags_WidthStretch);
            ui::TableSetupColumn(gmsg::col_code, ImGuiTableColumnFlags_WidthFixed, px(70));
            ui::TableSetupColumn(gmsg::col_size, ImGuiTableColumnFlags_WidthFixed, px(80));
            ImGui::TableHeadersRow();
            for (int i = 0; i < (int)g.features.size(); i++) {
                const auto& f = g.features[(size_t)i];
                ImGui::PushID(i);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                if (ui::SelectableRaw(f.name.empty() ? "-" : f.name.c_str(), g.feat_sel == i,
                                      ImGuiSelectableFlags_SpanAllColumns))
                    g.feat_sel = i;
                ImGui::TableNextColumn();
                ui::TextRaw(f.code);
                ImGui::TableNextColumn();
                const auto& s = f.v;
                if (f.kind == geo::FeatureKind::Line) ui::TextRaw(fixed(geo::plan_length(s, false), 2) + " m");
                else if (f.kind == geo::FeatureKind::Polygon) ui::TextRaw(fixed(geo::plan_area(s), 2) + " m2");
                else ui::TextRaw(fixed(s[0][2], 2));
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }
    const float half = (full - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
    ImGui::BeginDisabled(g.feat_sel < 0 || g.feat_sel >= (int)g.features.size());
    if (ui::Button(gmsg::delete_point, ImVec2(half, 0))) {
        g.undo.push_back(g.features);
        g.features.erase(g.features.begin() + g.feat_sel);
        g.feat_sel = -1;
        geo_save_vectors();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(g.undo.empty());
    if (ui::Button(gmsg::undo, ImVec2(half, 0))) {
        g.features = std::move(g.undo.back());
        g.undo.pop_back();
        g.feat_sel = -1;
        geo_save_vectors();
    }
    ImGui::EndDisabled();
    if (!g.status.empty()) {
        if (g.status_err) ui::TextColoredWrappedRaw(ImVec4(1, 0.45f, 0.4f, 1), g.status);
        else ui::TextWrappedRaw(g.status);
    }
}

// ---------------------------------------------------------------------------
// The Export tab
// ---------------------------------------------------------------------------

void EditSession::geo_export_model(const std::string& path) {
    GeoState& g = *_geo;
    if (g.exporting) return;
    const bool mesh = _doc->kind() == EditDoc::Kind::Mesh && _doc->mesh();
    const bool obj = mesh && g.model_fmt == 3;
    double offset[3] = {0, 0, 0};
    if (g.gp.placed) std::copy(g.gp.offset, g.gp.offset + 3, offset);
    const std::string prj = g.crs_ok ? geo::crs_to_wkt(g.crs, g.gp.crs_name) : std::string();
    const int fmt = g.model_fmt;

    // Copied on this thread: the document is the UI's.
    auto xyz = std::make_shared<std::vector<float>>();
    auto rgb = std::make_shared<std::vector<uint8_t>>();
    auto keep = std::make_shared<std::vector<uint8_t>>();
    auto out_mesh = std::make_shared<meshing::MeshData>();
    if (obj) {
        const meshing::MeshData& m = *_doc->mesh();
        const std::vector<uint8_t>& alive = _doc->alive_of(0);
        std::vector<int> remap(m.V.size(), -1);
        const Sim3 moved = _doc->file_placement();
        for (const auto& f : m.F) {
            if (!(alive[(size_t)f[0]] && alive[(size_t)f[1]] && alive[(size_t)f[2]])) continue;
            std::array<int, 3> t{};
            for (int k = 0; k < 3; k++) {
                int& r = remap[(size_t)f[k]];
                if (r < 0) {
                    r = (int)out_mesh->V.size();
                    const auto& v = m.V[(size_t)f[k]];
                    const double p[3] = {v[0], v[1], v[2]};
                    double q[3];
                    moved.apply(p, q);
                    out_mesh->V.push_back({(float)q[0], (float)q[1], (float)q[2]});
                    if (m.UV.size() == m.V.size()) out_mesh->UV.push_back(m.UV[(size_t)f[k]]);
                    if (m.N.size() == m.V.size()) {
                        const auto& n = m.N[(size_t)f[k]];
                        const double a[3] = {n[0], n[1], n[2]};
                        double b[3];
                        moved.rotate(a, b);
                        out_mesh->N.push_back({(float)b[0], (float)b[1], (float)b[2]});
                    }
                }
                t[k] = r;
            }
            out_mesh->F.push_back(t);
        }
        out_mesh->tex_width = m.tex_width;
        out_mesh->tex_height = m.tex_height;
        out_mesh->texture = m.texture;
    } else {
        const int64_t n = [&] {
            const int was = _doc->layer();
            _doc->set_layer(0);
            const int64_t c = _doc->count();
            std::vector<float> col;
            if (_doc->colours(col) && (int64_t)col.size() == c * 3) {
                rgb->resize(col.size());
                for (size_t i = 0; i < col.size(); i++)
                    (*rgb)[i] = (uint8_t)std::lround(std::clamp(col[i], 0.0f, 1.0f) * 255.0f);
            }
            _doc->set_layer(was);
            return c;
        }();
        const Sim3 to_saved = _doc->view_frame().inverse() * _doc->placement();
        const float* p = _doc->positions_of(0);
        xyz->resize((size_t)n * 3);
        for (int64_t i = 0; i < n; i++) {
            const double a[3] = {p[i * 3], p[i * 3 + 1], p[i * 3 + 2]};
            double b[3];
            to_saved.apply(a, b);
            for (int k = 0; k < 3; k++) (*xyz)[(size_t)(i * 3 + k)] = (float)b[k];
        }
        *keep = _doc->alive_of(0);
    }

    g.exporting = true;
    g.export_result.clear();
    GeoState* gs = &g;
    g.export_worker = std::thread([gs, path, xyz, rgb, keep, out_mesh, obj, fmt, prj, o0 = offset[0], o1 = offset[1],
                                   o2 = offset[2]]() {
        const double off[3] = {o0, o1, o2};
        std::string err;
        bool ok = true;
        try {
            if (obj) {
                meshing::MeshColorMode mode = !out_mesh->UV.empty() && !out_mesh->texture.empty()
                                                  ? meshing::MeshColorMode::Texture
                                                  : meshing::MeshColorMode::None;
                const auto spec = meshing::parse_one_mesh_format("obj");
                const std::string base = meshing::mesh_output_strip_ext(path);
                meshing::write_mesh(*out_mesh, mode, spec, base, false);
                ok = geo::obj_add_offset(base + ".obj", off, err);
            } else {
                geo::PointsOut po;
                po.xyz = xyz->data();
                po.n = (int64_t)xyz->size() / 3;
                po.rgb = rgb->empty() ? nullptr : rgb->data();
                po.keep = keep->empty() ? nullptr : keep->data();
                std::copy(off, off + 3, po.offset);
                ok = fmt == 0 ? geo::write_las(path, po, err)
                     : fmt == 1 ? geo::write_ply_double(path, po, err)
                                : geo::write_xyz(path, po, err);
            }
            if (ok && !prj.empty()) {
                auto p = std::filesystem::u8path(path);
                p.replace_extension(".prj");
                geo::write_text(p.u8string(), prj, err);
            }
        } catch (const std::exception& e) {
            ok = false;
            err = e.what();
        }
        gs->export_err = !ok;
        gs->export_result = ok ? spirula::i18n::format(gmsg::exported, {base_name(path)})
                               : spirula::i18n::format(gmsg::save_failed, {path, err});
        gs->exporting = false;
    });
}

void EditSession::geo_export_drawings(const std::string& path) {
    GeoState& g = *_geo;
    std::vector<geo::OutFeature> out;
    for (const auto& f : g.features) {
        geo::OutFeature o;
        o.kind = f.kind;
        o.code = f.code;
        o.name = f.name;
        const geo::ClassCode* c = geo::find_code(g.classes, f.code);
        o.layer = c ? c->layer : f.code;
        o.aci = c ? c->aci : 7;
        o.xyh = f.v;
        out.push_back(std::move(o));
    }
    if (g.with_control) {
        for (const auto& p : g.gp.points) {
            if (p.role == geo::Role::Off) continue;
            geo::OutFeature o;
            o.kind = geo::FeatureKind::Point;
            o.code = p.code.empty() ? std::string("GCP") : p.code;
            o.name = p.name;
            o.layer = "GCP";
            o.aci = 1;
            o.xyh.push_back({p.x, p.y, p.h});
            out.push_back(std::move(o));
        }
    }
    std::string err;
    if (out.empty()) {
        g.status = gmsg::nothing.get();
        g.status_err = true;
        return;
    }
    bool ok = false;
    switch (g.vec_fmt) {
        case 0: ok = geo::write_dxf(path, out, g.dxf, err); break;
        case 1: ok = geo::write_vectors_csv(path, out, err); break;
        case 2: ok = geo::write_geojson(path, out, g.crs_ok ? &g.crs : nullptr, err); break;
        default:
            if (!g.crs_ok) {
                g.status = gmsg::kml_need.get();
                g.status_err = true;
                return;
            }
            ok = geo::write_kml(path, out, g.crs, err);
    }
    if (ok && g.crs_ok && g.vec_fmt <= 1) {
        auto p = std::filesystem::u8path(path);
        p.replace_extension(".prj");
        geo::write_text(p.u8string(), geo::crs_to_wkt(g.crs, g.gp.crs_name), err);
    }
    g.status = ok ? spirula::i18n::format(gmsg::exported, {base_name(path)})
                  : spirula::i18n::format(gmsg::save_failed, {path, err});
    g.status_err = !ok;
}

void EditSession::draw_export_tab(float full) {
    geo_open();
    GeoState& g = *_geo;
    if (!g.exporting && g.export_worker.joinable()) {
        g.export_worker.join();
        g.status = g.export_result;
        g.status_err = g.export_err;
    }
    if (g.dlg.draw() && !g.dlg.result().empty()) geo_file_chosen(g.dlg.result());
    if (!g.gp.placed) ui::TextColoredWrapped(ImVec4(1, 0.7f, 0.3f, 1), gmsg::not_placed);

    ui::SeparatorText(gmsg::sec_model);
    const bool mesh = _doc->kind() == EditDoc::Kind::Mesh && _doc->mesh();
    if (!mesh && g.model_fmt == 3) g.model_fmt = 0;
    ImGui::SetNextItemWidth(full);
    if (mesh) ui::ComboRaw("##geomodelfmt", &g.model_fmt, {&gmsg::fmt_las, &gmsg::fmt_ply, &gmsg::fmt_xyz, &gmsg::fmt_obj});
    else ui::ComboRaw("##geomodelfmt", &g.model_fmt, {&gmsg::fmt_las, &gmsg::fmt_ply, &gmsg::fmt_xyz});
    ui::help_on_hover(gmsg::model_help);
    ImGui::BeginDisabled(g.exporting.load());
    if (ui::Button(gmsg::export_model, ImVec2(full, 0))) {
        static const char* ext[] = {".las", ".ply", ".txt", ".obj"};
        g.ask = GeoState::Ask::Model;
        const std::string stem = std::filesystem::u8path(_doc->source_path()).stem().u8string();
        g.dlg.open(gmsg::export_model.get(), FileDialog::Mode::Save, {ext[g.model_fmt]}, "", false,
                   stem + "_full" + ext[g.model_fmt]);
    }
    ImGui::EndDisabled();
    if (g.exporting) ui::TextDisabled(gmsg::exporting);

    ui::SeparatorText(gmsg::sec_drawings);
    ImGui::SetNextItemWidth(full);
    ui::ComboRaw("##geovecfmt", &g.vec_fmt, {&gmsg::fmt_dxf, &gmsg::fmt_csv, &gmsg::fmt_geojson, &gmsg::fmt_kml});
    if (g.vec_fmt == 0) {
        ui::Checkbox(gmsg::names_lbl, &g.dxf.name_labels);
        ui::Checkbox(gmsg::heights_lbl, &g.dxf.height_labels);
        float th = (float)g.dxf.text_height;
        ImGui::SetNextItemWidth(px(90));
        if (ui::InputFloatRaw("##geotexth", &th, "%.2f")) g.dxf.text_height = std::max(0.01f, th);
        ImGui::SameLine();
        ui::Text(gmsg::text_h);
    }
    ui::Checkbox(gmsg::with_control, &g.with_control);
    if (ui::Button(gmsg::export_drawings, ImVec2(full, 0))) {
        static const char* ext[] = {".dxf", ".csv", ".geojson", ".kml"};
        if (g.vec_fmt == 3 && !g.crs_ok) {
            g.status = gmsg::kml_need.get();
            g.status_err = true;
        } else {
            g.ask = GeoState::Ask::Drawings;
            const std::string stem = std::filesystem::u8path(_doc->source_path()).stem().u8string();
            g.dlg.open(gmsg::export_drawings.get(), FileDialog::Mode::Save, {ext[g.vec_fmt]}, "", false,
                       stem + "_plan" + ext[g.vec_fmt]);
        }
    }
    if (!g.status.empty()) {
        if (g.status_err) ui::TextColoredWrappedRaw(ImVec4(1, 0.45f, 0.4f, 1), g.status);
        else ui::TextWrappedRaw(g.status);
    }
}

// ---------------------------------------------------------------------------
// In the viewport
// ---------------------------------------------------------------------------

void EditSession::draw_vector_overlay(const ViewportOverlay& v, const ViewProjection& camera) {
    if (!_geo || !v.dl) return;
    GeoState& g = *_geo;
    const Sim3 to_shared = saved_to_shared();
    auto project = [&](const std::array<double, 3>& a, ImVec2& out) {
        double sv[3], w[3];
        geo_saved(a, sv);
        to_shared.apply(sv, w);
        const float q[3] = {(float)w[0], (float)w[1], (float)w[2]};
        float x, y, depth;
        if (!camera.project(q, x, y, depth) || !(depth > 0) || !std::isfinite(x) || !std::isfinite(y)) return false;
        out = ImVec2(v.x + x, v.y + y);
        return true;
    };
    v.dl->PushClipRect(ImVec2(v.x, v.y), ImVec2(v.x + v.w, v.y + v.h), true);
    auto draw = [&](const std::vector<std::array<double, 3>>& pts, geo::FeatureKind kind, ImU32 col, float w,
                    bool close) {
        std::vector<ImVec2> s;
        for (const auto& a : pts) {
            ImVec2 p;
            if (project(a, p)) s.push_back(p);
        }
        if (kind != geo::FeatureKind::Point && s.size() >= 2) {
            v.dl->AddPolyline(s.data(), (int)s.size(), IM_COL32(0, 0, 0, 200), close ? ImDrawFlags_Closed : 0, w + px(2));
            v.dl->AddPolyline(s.data(), (int)s.size(), col, close ? ImDrawFlags_Closed : 0, w);
        }
        for (const auto& p : s) {
            v.dl->AddCircleFilled(p, px(3.5f), col);
            v.dl->AddCircle(p, px(4), IM_COL32(0, 0, 0, 220), 0, px(1));
        }
        return s;
    };
    for (int i = 0; i < (int)g.features.size(); i++) {
        const auto& f = g.features[(size_t)i];
        const geo::ClassCode* c = geo::find_code(g.classes, f.code);
        const bool sel = i == g.feat_sel && _tab == kDraw;
        const ImU32 col = sel ? IM_COL32(255, 230, 60, 255) : aci_colour(c ? c->aci : 7);
        const auto s = draw(f.v, f.kind, col, px(sel ? 3.0f : 2.0f), f.kind == geo::FeatureKind::Polygon);
        if (!s.empty() && !f.name.empty()) outlined(v.dl, ImVec2(s[0].x + px(6), s[0].y - px(18)), col, f.name);
    }
    if (_tab == kMeasure || _tab == kDraw) {
        const ImU32 live = IM_COL32(255, 140, 40, 255);
        auto pts = g.pts;
        int kind = _tab == kMeasure ? g.measure_kind : g.draw_kind;
        if (_tab == kDraw && g.code >= 0 && g.code < (int)g.classes.size()) kind = (int)g.classes[(size_t)g.code].kind;
        if (g.hover_ok && !pts.empty() && kind != 0) pts.push_back({g.hover[0], g.hover[1], g.hover[2]});
        draw(pts, (geo::FeatureKind)kind, live, px(2.5f), kind == 2);
        if (g.hover_ok) {
            ImVec2 p;
            if (project({g.hover[0], g.hover[1], g.hover[2]}, p)) {
                const ImU32 col = g.snapped ? IM_COL32(255, 230, 60, 255) : IM_COL32(255, 255, 255, 255);
                const float a = px(10);
                v.dl->AddLine(ImVec2(p.x - a, p.y), ImVec2(p.x + a, p.y), col, px(1.5f));
                v.dl->AddLine(ImVec2(p.x, p.y - a), ImVec2(p.x, p.y + a), col, px(1.5f));
                char line[160];
                std::snprintf(line, sizeof line, "X %.3f  Y %.3f  H %.3f", g.hover[0], g.hover[1], g.hover[2]);
                outlined(v.dl, ImVec2(p.x + px(14), p.y + px(10)), col, line);
            }
        }
    }
    v.dl->PopClipRect();
}

}  // namespace gui
