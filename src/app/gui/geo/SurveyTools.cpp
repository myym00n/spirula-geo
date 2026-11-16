// SurveyTools.cpp -- measuring, drawing and moving points on the model, and
// writing the model and the drawing out in full survey coordinates. Points
// come from the viewport's own depth pick, so the cursor slides over what is
// drawn rather than jumping between splat centres.

#include "app/AppPaths.h"
#include "app/gui/Layout.h"
#include "app/gui/Ui.h"
#include "app/gui/ViewportPanel.h"
#include "app/gui/edit/EditSession.h"
#include "app/gui/geo/GeoState.h"
#include "app/gui/geo/SurveySession.h"
#include "data/Json.h"
#include "data/JsonWrite.h"
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

// "12" -> "13", "P-7" -> "P-8"; anything not ending in a digit stays.
std::string next_number(const std::string& s) {
    size_t k = s.size();
    while (k > 0 && std::isdigit((unsigned char)s[k - 1])) k--;
    if (k == s.size() || s.size() - k > 15) return s;
    const std::string digits = s.substr(k);
    std::string n = std::to_string(std::stoll(digits) + 1);
    if (n.size() < digits.size()) n = std::string(digits.size() - n.size(), '0') + n;
    return s.substr(0, k) + n;
}

void outlined(ImDrawList* dl, ImVec2 at, ImU32 col, const std::string& s) {
    dl->AddText(ImVec2(at.x + 1, at.y + 1), IM_COL32_BLACK, s.c_str());
    dl->AddText(at, col, s.c_str());
}

void status_line(GeoState& g) {
    if (g.status.empty()) return;
    if (g.status_err) ui::TextColoredWrappedRaw(ImVec4(1, 0.45f, 0.4f, 1), g.status);
    else ui::TextWrappedRaw(g.status);
}

}  // namespace

// ---------------------------------------------------------------------------
// Frames
// ---------------------------------------------------------------------------

std::array<double, 3> SurveySession::geo_survey(const double saved[3]) const {
    const double zero[3] = {0, 0, 0};
    const double* off = _geo->gp.placed ? _geo->gp.offset : zero;
    return {saved[1] + off[1], saved[0] + off[0], saved[2] + off[2]};
}

void SurveySession::geo_saved(const std::array<double, 3>& xyh, double saved[3]) const {
    const double zero[3] = {0, 0, 0};
    const double* off = _geo->gp.placed ? _geo->gp.offset : zero;
    saved[0] = xyh[1] - off[0];
    saved[1] = xyh[0] - off[1];
    saved[2] = xyh[2] - off[2];
}

// ---------------------------------------------------------------------------
// The pointer
// ---------------------------------------------------------------------------

bool SurveySession::picks() const {
    return _tool == Tool::Measure || _tool == Tool::Point || _tool == Tool::Contour || _tool == Tool::Move;
}

void SurveySession::ask_pick(const ViewportInput& in) {
    GeoState& g = *_geo;
    if (g.pick_inflight || !_e._panel) return;
    g.pick_inflight = true;
    g.pick_xy[0] = in.x;
    g.pick_xy[1] = in.y;
    _e._panel->request_pick(in.x / (float)std::max(in.W, 1), in.y / (float)std::max(in.H, 1));
}

void SurveySession::take_pick() {
    GeoState& g = *_geo;
    float p[3];
    bool hit = false;
    if (!_e._panel || !_e._panel->take_pick(p, hit)) return;
    g.pick_inflight = false;
    double xyh[3] = {0, 0, 0};
    if (hit) {
        const double shared[3] = {p[0], p[1], p[2]};
        double saved[3];
        _e.saved_to_shared().inverse().apply(shared, saved);
        const auto s = geo_survey(saved);
        std::copy(s.begin(), s.end(), xyh);
    }
    // An existing vertex near the pointer wins over the surface under it.
    int f = -1, v = -1;
    double snap[3];
    g.snapped = picks() && _tool != Tool::Move && vertex_at(g.pick_xy[0], g.pick_xy[1], f, v, snap);
    if (g.snapped) std::copy(snap, snap + 3, xyh);
    const bool ok = hit || g.snapped;

    if (g.click == GeoState::Click::Readout) {
        g.click = GeoState::Click::None;
        g.readout_ok = ok;
        std::copy(xyh, xyh + 3, g.readout);
        return;
    }
    g.hover_ok = ok;
    std::copy(xyh, xyh + 3, g.hover);
    if (g.click != GeoState::Click::Act) return;
    g.click = GeoState::Click::None;
    if (!ok) return;
    tool_click({xyh[0], xyh[1], xyh[2]});
}

// What a click at a survey point does with the tool in hand; the 3D view and
// the plan both end here.
void SurveySession::tool_click(const std::array<double, 3>& a) {
    GeoState& g = *_geo;
    if (_tool == Tool::Measure) {
        if (g.measure_kind == (int)geo::FeatureKind::Point || g.measure_done) g.pts.clear();
        g.measure_done = false;
        g.pts.push_back(a);
    } else if (_tool == Tool::Point) {
        geo_commit(geo::FeatureKind::Point, {a});
    } else if (_tool == Tool::Contour) {
        if (g.pts.empty() || g.pts.back() != a) g.pts.push_back(a);
    } else if (_tool == Tool::Move && g.move_feat >= 0 && g.move_feat < (int)g.features.size()) {
        auto& fv = g.features[(size_t)g.move_feat].v;
        if (g.move_vtx >= 0 && g.move_vtx < (int)fv.size()) {
            fv[(size_t)g.move_vtx] = a;
            g.move_dirty = true;
        }
    }
}

// Move: take this vertex in hand, asking first if another is unconfirmed.
void SurveySession::move_take(int f, int v) {
    GeoState& g = *_geo;
    if (f == g.move_feat && v == g.move_vtx) return;
    if (g.move_dirty) {
        g.move_ask = true;
        g.move_next_feat = f;
        g.move_next_vtx = v;
        return;
    }
    g.move_feat = f;
    g.move_vtx = v;
    g.move_orig = g.features[(size_t)f].v[(size_t)v];
    g.feat_sel = f;
}

// The drawn vertex nearest the pointer within ten pixels.
bool SurveySession::vertex_at(float x, float y, int& feat, int& vtx, double xyh[3]) {
    GeoState& g = *_geo;
    XformFrame f;
    if (!_e.xform_frame(f)) return false;
    const Sim3 to_shared = _e.saved_to_shared();
    float best = px(10);
    feat = vtx = -1;
    for (int i = 0; i < (int)g.features.size(); i++) {
        const auto& v = g.features[(size_t)i].v;
        for (int k = 0; k < (int)v.size(); k++) {
            if (i == g.move_feat && k == g.move_vtx) continue;
            double sv[3], w[3];
            geo_saved(v[(size_t)k], sv);
            to_shared.apply(sv, w);
            const float q[3] = {(float)w[0], (float)w[1], (float)w[2]};
            float sx, sy, depth;
            if (!f.cam.project(q, sx, sy, depth) || !(depth > 0)) continue;
            const float d = std::hypot(sx - x, sy - y);
            if (d < best) {
                best = d;
                feat = i;
                vtx = k;
                std::copy(v[(size_t)k].begin(), v[(size_t)k].end(), xyh);
            }
        }
    }
    if (_tool != Tool::Move)
        for (const auto& a : g.pts) {
            double sv[3], w[3];
            geo_saved(a, sv);
            to_shared.apply(sv, w);
            const float q[3] = {(float)w[0], (float)w[1], (float)w[2]};
            float sx, sy, depth;
            if (!f.cam.project(q, sx, sy, depth) || !(depth > 0)) continue;
            const float d = std::hypot(sx - x, sy - y);
            if (d < best) {
                best = d;
                feat = vtx = -1;
                std::copy(a.begin(), a.end(), xyh);
            }
        }
    return best < px(10);
}

bool SurveySession::owns_left_button() const {
    return _attached && _geo && ImGui::GetFrameCount() - _frame <= 1 && picks();
}

bool SurveySession::on_viewport_input(const ViewportInput& in) {
    if (!_attached || !_geo || ImGui::GetFrameCount() - _frame > 1) return false;
    GeoState& g = *_geo;
    take_pick();
    const bool moved = std::fabs(in.x - g.last_mouse[0]) > 0.5f || std::fabs(in.y - g.last_mouse[1]) > 0.5f;
    g.last_mouse[0] = in.x;
    g.last_mouse[1] = in.y;

    if (!picks()) {
        // Looking around: a click that did not drag reads the point under it.
        if (in.clicked) {
            g.press_xy[0] = in.x;
            g.press_xy[1] = in.y;
        }
        if (in.released && in.hovered && std::hypot(in.x - g.press_xy[0], in.y - g.press_xy[1]) < 3.0f &&
            !g.pick_inflight) {
            ask_pick(in);
            g.click = GeoState::Click::Readout;
        }
        return false;
    }
    if (!in.hovered) {
        g.hover_ok = false;
        return false;
    }
    if (in.clicked && _tool == Tool::Move) {
        int f, v;
        double xyh[3];
        if (vertex_at(in.x, in.y, f, v, xyh) && f >= 0) {
            move_take(f, v);
            return true;
        }
    }
    if (in.clicked) {
        g.pick_inflight = false;
        ask_pick(in);
        g.click = GeoState::Click::Act;
    } else if (moved || !g.hover_ok) {
        ask_pick(in);
    }
    if (in.double_clicked && _tool == Tool::Contour) geo_finish();
    return in.down || in.clicked;
}

void SurveySession::geo_keys() {
    GeoState& g = *_geo;
    if (ImGui::GetIO().WantTextInput) return;
    if (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)) {
        if (_tool == Tool::Move) move_confirm();
        else geo_finish();
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Backspace) && !g.pts.empty()) g.pts.pop_back();
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        if (_tool == Tool::Move) move_cancel();
        g.pts.clear();
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Delete) && (_tab == Tab::Plan || _tool == Tool::Move) && g.feat_sel >= 0 &&
        g.feat_sel < (int)g.features.size()) {
        move_cancel();
        g.undo.push_back(g.features);
        g.features.erase(g.features.begin() + g.feat_sel);
        g.feat_sel = -1;
        geo_save_vectors();
    }
    if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z, false) && !g.undo.empty()) {
        move_cancel();
        g.features = std::move(g.undo.back());
        g.undo.pop_back();
        g.feat_sel = -1;
        geo_save_vectors();
    }
}

void SurveySession::geo_finish() {
    GeoState& g = *_geo;
    if (_tool == Tool::Measure) g.measure_done = true;
    if (_tool != Tool::Contour) return;
    const geo::ClassCode* c = g.code >= 0 && g.code < (int)g.classes.size() ? &g.classes[(size_t)g.code] : nullptr;
    auto kind = c ? c->kind : (geo::FeatureKind)g.draw_kind;
    if (kind == geo::FeatureKind::Point) kind = geo::FeatureKind::Line;
    const size_t need = kind == geo::FeatureKind::Polygon ? 3 : 2;
    if (g.pts.size() >= need) geo_commit(kind, g.pts);
    g.pts.clear();
}

void SurveySession::geo_commit(geo::FeatureKind kind, const std::vector<std::array<double, 3>>& pts) {
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

void SurveySession::geo_save_vectors() {
    GeoState& g = *_geo;
    std::string err;
    if (!g.vec_path.empty() && !geo::save_vectors(g.vec_path, g.features, g.vec_survey || g.gp.placed, err)) {
        g.status = spirula::i18n::format(gmsg::save_failed, {g.vec_path, err});
        g.status_err = true;
    }
}

void SurveySession::move_confirm() {
    GeoState& g = *_geo;
    if (g.move_dirty) {
        // The undo step holds the vertex where it was before the move.
        auto before = g.features;
        if (g.move_feat >= 0 && g.move_feat < (int)before.size() && g.move_vtx >= 0 &&
            g.move_vtx < (int)before[(size_t)g.move_feat].v.size())
            before[(size_t)g.move_feat].v[(size_t)g.move_vtx] = g.move_orig;
        g.undo.push_back(std::move(before));
        geo_save_vectors();
    }
    g.move_dirty = false;
    g.move_feat = g.move_vtx = -1;
}

void SurveySession::move_cancel() {
    GeoState& g = *_geo;
    if (g.move_dirty && g.move_feat >= 0 && g.move_feat < (int)g.features.size()) {
        auto& v = g.features[(size_t)g.move_feat].v;
        if (g.move_vtx >= 0 && g.move_vtx < (int)v.size()) v[(size_t)g.move_vtx] = g.move_orig;
    }
    g.move_dirty = false;
    g.move_feat = g.move_vtx = -1;
}

void SurveySession::draw_modals() {
    if (!_geo) return;
    GeoState& g = *_geo;
    if (g.dlg.draw() && !g.dlg.result().empty()) geo_file_chosen(g.dlg.result());
    draw_pickets_modal();
    if (g.move_ask) {
        g.move_ask = false;
        ui::OpenPopup(gmsg::move_title);
    }
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 26.0f, 0.0f), ImGuiCond_Appearing);
    if (ui::BeginPopupModal(gmsg::move_title)) {
        const std::string name = g.move_feat >= 0 && g.move_feat < (int)g.features.size()
                                     ? g.features[(size_t)g.move_feat].name
                                     : std::string();
        ui::TextWrapped(gmsg::move_body, {name});
        auto go_next = [&] {
            const int f = g.move_next_feat, v = g.move_next_vtx;
            if (f >= 0 && f < (int)g.features.size() && v >= 0 && v < (int)g.features[(size_t)f].v.size()) {
                g.move_feat = f;
                g.move_vtx = v;
                g.move_orig = g.features[(size_t)f].v[(size_t)v];
                g.feat_sel = f;
            }
        };
        if (ui::Button(gmsg::move_keep)) {
            move_confirm();
            go_next();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ui::Button(gmsg::move_drop)) {
            move_cancel();
            go_next();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ui::Button(gmsg::move_stay)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

// ---------------------------------------------------------------------------
// Classifier, kept across sessions as a path
// ---------------------------------------------------------------------------

void SurveySession::geo_load_classifier(const std::string& path) {
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
    w.key("version").value(1);
    w.key("classifier").value(path);
    w.end();
    geo::write_text(settings_path(), w.str(), err);
}

void SurveySession::geo_load_classifier_setting() {
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

// ---------------------------------------------------------------------------
// Panels
// ---------------------------------------------------------------------------

void SurveySession::draw_measure_panel(float full) {
    GeoState& g = *_geo;
    ui::TextDisabledWrapped(gmsg::measure_help);
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
}

void SurveySession::draw_code_picker(float full) {
    GeoState& g = *_geo;
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
                const std::string label = c.code + "  " + c.name;
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
}

void SurveySession::draw_feature_list(float full) {
    GeoState& g = *_geo;
    ui::Text(gmsg::drawn, {(long long)g.features.size()});
    if (!g.features.empty()) {
        const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit;
        const float rows = (float)std::min<size_t>(g.features.size(), 14) + 1.4f;
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
                if (f.kind == geo::FeatureKind::Line) ui::TextRaw(fixed(geo::plan_length(f.v, false), 2) + " m");
                else if (f.kind == geo::FeatureKind::Polygon) ui::TextRaw(fixed(geo::plan_area(f.v), 2) + " m2");
                else if (!f.v.empty()) ui::TextRaw(fixed(f.v[0][2], 2));
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }
    const float half = (full - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
    ImGui::BeginDisabled(g.feat_sel < 0 || g.feat_sel >= (int)g.features.size());
    if (ui::Button(gmsg::delete_point, ImVec2(half, 0))) {
        move_cancel();
        g.undo.push_back(g.features);
        g.features.erase(g.features.begin() + g.feat_sel);
        g.feat_sel = -1;
        geo_save_vectors();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(g.undo.empty());
    if (ui::Button(gmsg::undo, ImVec2(half, 0))) {
        move_cancel();
        g.features = std::move(g.undo.back());
        g.undo.pop_back();
        g.feat_sel = -1;
        geo_save_vectors();
    }
    ImGui::EndDisabled();
}

void SurveySession::draw_move_panel(float full) {
    GeoState& g = *_geo;
    ui::TextDisabledWrapped(gmsg::move_help);
    if (g.move_feat < 0 || g.move_feat >= (int)g.features.size()) return;
    const auto& f = g.features[(size_t)g.move_feat];
    if (g.move_vtx < 0 || g.move_vtx >= (int)f.v.size()) return;
    ui::Text(gmsg::moving, {f.name, (long long)(g.move_vtx + 1)});
    const auto& p = f.v[(size_t)g.move_vtx];
    char line[160];
    std::snprintf(line, sizeof line, "X %.3f   Y %.3f   H %.3f", p[0], p[1], p[2]);
    if (g.move_dirty) ui::TextColoredRaw(ImVec4(1, 0.75f, 0.3f, 1), line);
    else ui::TextRaw(line);
    const float half = (full - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
    if (ui::Button(gmsg::done, ImVec2(half, 0))) move_confirm();
    ImGui::SameLine();
    if (ui::Button(gmsg::cancel, ImVec2(half, 0))) move_cancel();
}

// No control points: the point last read gets the coordinates typed here and
// everything drawn follows. Only the offset changes; the model does not move.
void SurveySession::draw_local_system(float full) {
    GeoState& g = *_geo;
    if (g.sol.ok || (g.gp.placed && !g.gp.local)) return;
    ui::SeparatorText(gmsg::local_title);
    ui::TextDisabledWrapped(gmsg::local_help);
    ImGui::BeginDisabled(!g.readout_ok);
    static const char* ids[3] = {"X##geolocal", "Y##geolocal", "H##geolocal"};
    for (int k = 0; k < 3; k++) {
        ImGui::SetNextItemWidth(full * 0.7f);
        ui::InputDoubleRaw(ids[k], &g.local_xyh[k], 0.0, 0.0, "%.3f");
    }
    if (ui::Button(gmsg::local_apply, ImVec2(full, 0))) {
        double saved[3];
        geo_saved({g.readout[0], g.readout[1], g.readout[2]}, saved);
        const double now[3] = {g.local_xyh[1] - saved[0], g.local_xyh[0] - saved[1], g.local_xyh[2] - saved[2]};
        double d[3];
        for (int k = 0; k < 3; k++) d[k] = now[k] - (g.gp.placed ? g.gp.offset[k] : 0.0);
        auto shift = [&](std::array<double, 3>& v) {
            v[0] += d[1];
            v[1] += d[0];
            v[2] += d[2];
        };
        for (auto& f : g.features)
            for (auto& v : f.v) shift(v);
        for (auto& v : g.pts) shift(v);
        std::copy(now, now + 3, g.gp.offset);
        std::copy(g.local_xyh, g.local_xyh + 3, g.readout);
        g.gp.placed = g.gp.local = true;
        g.vec_survey = true;
        g.plan_count = -1;
        geo_save();
        geo_save_vectors();
        g.status = gmsg::local_done.get();
        g.status_err = false;
    }
    ImGui::EndDisabled();
}

// ---------------------------------------------------------------------------
// Export
// ---------------------------------------------------------------------------

void SurveySession::geo_export_model(const std::string& path) {
    GeoState& g = *_geo;
    if (g.exporting || !_e._doc) return;
    EditDoc& doc = *_e._doc;
    const bool obj = doc.kind() == EditDoc::Kind::Mesh && doc.mesh() && g.model_fmt == 3;
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
        const meshing::MeshData& m = *doc.mesh();
        const std::vector<uint8_t>& alive = doc.alive_of(0);
        std::vector<int> remap(m.V.size(), -1);
        const Sim3 moved = doc.file_placement();
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
        const int was = doc.layer();
        doc.set_layer(0);
        const int64_t n = doc.count();
        std::vector<float> col;
        if (doc.colours(col) && (int64_t)col.size() == n * 3) {
            rgb->resize(col.size());
            for (size_t i = 0; i < col.size(); i++)
                (*rgb)[i] = (uint8_t)std::lround(std::clamp(col[i], 0.0f, 1.0f) * 255.0f);
        }
        doc.set_layer(was);
        const Sim3 to_saved = doc.view_frame().inverse() * doc.placement();
        const float* p = doc.positions_of(0);
        xyz->resize((size_t)n * 3);
        for (int64_t i = 0; i < n; i++) {
            const double a[3] = {p[i * 3], p[i * 3 + 1], p[i * 3 + 2]};
            double b[3];
            to_saved.apply(a, b);
            for (int k = 0; k < 3; k++) (*xyz)[(size_t)(i * 3 + k)] = (float)b[k];
        }
        *keep = doc.alive_of(0);
    }

    g.exporting = true;
    g.export_result.clear();
    GeoState* gs = &g;
    g.export_worker = std::thread([gs, path, xyz, rgb, keep, out_mesh, obj, fmt, prj, o0 = offset[0],
                                   o1 = offset[1], o2 = offset[2]]() {
        const double off[3] = {o0, o1, o2};
        std::string err;
        bool ok = true;
        try {
            if (obj) {
                const meshing::MeshColorMode mode = !out_mesh->UV.empty() && !out_mesh->texture.empty()
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
                ok = fmt == 0   ? geo::write_las(path, po, err)
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

void SurveySession::geo_export_drawings(const std::string& path) {
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

void SurveySession::draw_report_panel(float full) {
    GeoState& g = *_geo;
    if (!g.gp.placed) ui::TextColoredWrapped(ImVec4(1, 0.7f, 0.3f, 1), gmsg::not_placed);
    ui::TextDisabledWrapped(gmsg::report_soon);

    ui::SeparatorText(gmsg::sec_model);
    const bool mesh = _e._doc->kind() == EditDoc::Kind::Mesh && _e._doc->mesh();
    if (!mesh && g.model_fmt == 3) g.model_fmt = 0;
    ImGui::SetNextItemWidth(full);
    if (mesh) ui::ComboRaw("##geomodelfmt", &g.model_fmt, {&gmsg::fmt_las, &gmsg::fmt_ply, &gmsg::fmt_xyz, &gmsg::fmt_obj});
    else ui::ComboRaw("##geomodelfmt", &g.model_fmt, {&gmsg::fmt_las, &gmsg::fmt_ply, &gmsg::fmt_xyz});
    ui::help_on_hover(gmsg::model_help);
    ImGui::BeginDisabled(g.exporting.load());
    if (ui::Button(gmsg::export_model, ImVec2(full, 0))) {
        static const char* ext[] = {".las", ".ply", ".txt", ".obj"};
        g.ask = GeoState::Ask::Model;
        const std::string stem = std::filesystem::u8path(_e._doc->source_path()).stem().u8string();
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
            const std::string stem = std::filesystem::u8path(_e._doc->source_path()).stem().u8string();
            g.dlg.open(gmsg::export_drawings.get(), FileDialog::Mode::Save, {ext[g.vec_fmt]}, "", false,
                       stem + "_plan" + ext[g.vec_fmt]);
        }
    }
}

void SurveySession::draw_status(float) {
    if (_geo) status_line(*_geo);
}

// ---------------------------------------------------------------------------
// In the viewport
// ---------------------------------------------------------------------------

void SurveySession::draw_vector_overlay(const ViewportOverlay& v, const ViewProjection& camera) {
    GeoState& g = *_geo;
    const Sim3 to_shared = _e.saved_to_shared();
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
        const bool sel = i == g.feat_sel;
        const ImU32 col = sel ? IM_COL32(255, 230, 60, 255) : aci_colour(c ? c->aci : 7);
        const auto s = draw(f.v, f.kind, col, px(sel ? 3.0f : 2.0f), f.kind == geo::FeatureKind::Polygon);
        if (!s.empty() && !f.name.empty()) outlined(v.dl, ImVec2(s[0].x + px(6), s[0].y - px(18)), col, f.name);
    }
    if (g.move_feat >= 0 && g.move_feat < (int)g.features.size()) {
        const auto& fv = g.features[(size_t)g.move_feat].v;
        ImVec2 p;
        if (g.move_vtx >= 0 && g.move_vtx < (int)fv.size() && project(fv[(size_t)g.move_vtx], p)) {
            const ImU32 col = g.move_dirty ? IM_COL32(255, 150, 40, 255) : IM_COL32(255, 230, 60, 255);
            v.dl->AddCircle(p, px(9), col, 0, px(2.5f));
            if (g.move_dirty) {
                ImVec2 o;
                if (project(g.move_orig, o)) {
                    v.dl->AddLine(o, p, IM_COL32(255, 150, 40, 160), px(1.5f));
                    v.dl->AddCircle(o, px(5), IM_COL32(255, 150, 40, 160), 0, px(1.5f));
                }
            }
        }
    }
    if (picks()) {
        const ImU32 live = IM_COL32(255, 140, 40, 255);
        auto pts = g.pts;
        int kind = _tool == Tool::Measure ? g.measure_kind : g.draw_kind;
        if (_tool == Tool::Contour && g.code >= 0 && g.code < (int)g.classes.size())
            kind = (int)g.classes[(size_t)g.code].kind;
        if (_tool == Tool::Contour && kind == 0) kind = 1;
        if (_tool == Tool::Point || _tool == Tool::Move) kind = 0;
        if (g.hover_ok && !pts.empty() && kind != 0 && !(_tool == Tool::Measure && g.measure_done))
            pts.push_back({g.hover[0], g.hover[1], g.hover[2]});
        if (_tool != Tool::Move) draw(pts, (geo::FeatureKind)kind, live, px(2.5f), kind == 2);
        if (g.hover_ok) {
            ImVec2 p;
            if (project({g.hover[0], g.hover[1], g.hover[2]}, p)) {
                const ImU32 col = g.snapped ? IM_COL32(255, 230, 60, 255) : IM_COL32(255, 255, 255, 255);
                const float a = px(10);
                v.dl->AddCircleFilled(p, px(3), col);
                v.dl->AddLine(ImVec2(p.x - a, p.y), ImVec2(p.x - px(4), p.y), col, px(1.5f));
                v.dl->AddLine(ImVec2(p.x + px(4), p.y), ImVec2(p.x + a, p.y), col, px(1.5f));
                v.dl->AddLine(ImVec2(p.x, p.y - a), ImVec2(p.x, p.y - px(4)), col, px(1.5f));
                v.dl->AddLine(ImVec2(p.x, p.y + px(4)), ImVec2(p.x, p.y + a), col, px(1.5f));
                char line[160];
                std::snprintf(line, sizeof line, "X %.3f  Y %.3f  H %.3f", g.hover[0], g.hover[1], g.hover[2]);
                outlined(v.dl, ImVec2(p.x + px(14), p.y + px(10)), col, line);
            }
        }
    }
    if (_tool == Tool::View && g.readout_ok) {
        ImVec2 p;
        if (project({g.readout[0], g.readout[1], g.readout[2]}, p)) {
            v.dl->AddCircle(p, px(6), IM_COL32(90, 220, 255, 255), 0, px(2));
            char line[160];
            std::snprintf(line, sizeof line, "X %.3f  Y %.3f  H %.3f", g.readout[0], g.readout[1], g.readout[2]);
            outlined(v.dl, ImVec2(p.x + px(10), p.y + px(8)), IM_COL32(90, 220, 255, 255), line);
        }
    }
}

}  // namespace gui
