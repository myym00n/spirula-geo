// SurveyPages.cpp -- the full-width pages of the Survey screen: the control
// point catalog with its imports, the pickets of a DXF sketch, and the plan,
// a top view drawn here with the same tools as the 3D view.

#include "app/gui/Layout.h"
#include "app/gui/Ui.h"
#include "app/gui/edit/EditSession.h"
#include "app/gui/geo/GeoState.h"
#include "app/gui/geo/SurveySession.h"
#include "i18n/catalog/Geo.h"

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

std::string base_name(const std::string& p) {
    const size_t k = p.find_last_of("/\\");
    return k == std::string::npos ? p : p.substr(k + 1);
}

const geo::PointResult* result_of(const geo::Solution& s, const std::string& name) {
    for (const auto& r : s.points)
        if (r.name == name) return &r;
    return nullptr;
}

std::string lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

void outlined(ImDrawList* dl, ImVec2 at, ImU32 col, const std::string& s) {
    dl->AddText(ImVec2(at.x + 1, at.y + 1), IM_COL32_BLACK, s.c_str());
    dl->AddText(at, col, s.c_str());
}

ImU32 role_colour(geo::Role r) {
    switch (r) {
        case geo::Role::Control: return IM_COL32(90, 230, 115, 255);
        case geo::Role::Check: return IM_COL32(85, 175, 255, 255);
        case geo::Role::Measure: return IM_COL32(255, 200, 80, 255);
        default: return IM_COL32(170, 170, 170, 255);
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// Cameras and photos: found, loading, or to be pointed at
// ---------------------------------------------------------------------------

bool SurveySession::draw_dataset_state() {
    GeoState& g = *_geo;
    if (g.ds && !g.loading) return true;
    if (g.loading) {
        ui::TextDisabled(gmsg::loading_dataset);
        return false;
    }
    if (g.md.directory.empty()) ui::TextColoredWrapped(ImVec4(1, 0.7f, 0.3f, 1), gmsg::no_dataset);
    else ui::TextColoredWrapped(ImVec4(1, 0.45f, 0.4f, 1), gmsg::dataset_failed, {g.load_error});
    if (ui::Button(gmsg::dataset_pick)) {
        g.ask = GeoState::Ask::Dataset;
        g.dlg.open(gmsg::dataset_pick.get(), FileDialog::Mode::Folder, {});
    }
    ui::help_on_hover(gmsg::dataset_pick_help);
    return false;
}

void SurveySession::set_dataset_dir(const std::string& dir) {
    if (!_e._doc) return;
    // Kept beside the model; the session then opens again on it.
    const std::string side_path = _e._doc->source_path() + ".geo.json";
    geo::GeoProject side;
    std::string err;
    if (std::filesystem::exists(std::filesystem::u8path(side_path))) geo::load_geo(side_path, side, err);
    side.dataset_dir = dir;
    geo::save_geo(side_path, side, err);
    _reopen = true;   // poll() does it: the caller still holds the state
}

// ---------------------------------------------------------------------------
// Pickets of a DXF sketch
// ---------------------------------------------------------------------------

void SurveySession::open_pickets(const std::string& path) {
    GeoState& g = *_geo;
    std::string err;
    std::vector<geo::Picket> p;
    if (!geo::read_dxf_pickets(path, p, err)) {
        g.status = spirula::i18n::format(gmsg::import_failed, {base_name(path), err});
        g.status_err = true;
        return;
    }
    g.pickets = std::move(p);
    // Named ones are the likely control points; the rest wait to be ticked.
    g.picket_on.assign(g.pickets.size(), 0);
    for (size_t i = 0; i < g.pickets.size(); i++) g.picket_on[i] = !g.pickets[i].name.empty();
    g.pickets_filter.clear();
    g.pickets_open = true;
}

void SurveySession::draw_pickets_modal() {
    GeoState& g = *_geo;
    if (g.pickets_open) {
        g.pickets_open = false;
        ui::OpenPopup(gmsg::pickets_title);
    }
    ImGui::SetNextWindowSize(ImVec2(px(860), px(620)), ImGuiCond_Appearing);
    if (!ui::BeginPopupModal(gmsg::pickets_title)) return;
    ui::Checkbox(gmsg::pickets_swap, &g.pickets_swap);
    ImGui::SameLine();
    ui::Checkbox(gmsg::pickets_h_only, &g.pickets_h_only);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(px(200));
    ui::InputTextWithHintRaw("##pkfilter", gmsg::crs_filter, &g.pickets_filter);

    const std::string f = lower(g.pickets_filter);
    std::vector<int> shown;
    for (int i = 0; i < (int)g.pickets.size(); i++) {
        const auto& p = g.pickets[(size_t)i];
        if (g.pickets_h_only && !p.has_h) continue;
        if (!f.empty() && lower(p.name + " " + p.code + " " + p.layer).find(f) == std::string::npos) continue;
        shown.push_back(i);
    }
    ImGui::SameLine();
    if (ui::Button(gmsg::pickets_all))
        for (int i : shown) g.picket_on[(size_t)i] = 1;
    ImGui::SameLine();
    if (ui::Button(gmsg::pickets_none))
        for (int i : shown) g.picket_on[(size_t)i] = 0;

    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersOuter |
                                  ImGuiTableFlags_SizingFixedFit;
    if (ImGui::BeginTable("##pickets", 7, flags, ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 1.4f))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ui::TableSetupColumnRaw("##on", ImGuiTableColumnFlags_WidthFixed, px(28));
        ui::TableSetupColumn(gmsg::col_name, ImGuiTableColumnFlags_WidthFixed, px(110));
        ui::TableSetupColumn(gmsg::col_x, ImGuiTableColumnFlags_WidthFixed, px(120));
        ui::TableSetupColumn(gmsg::col_y, ImGuiTableColumnFlags_WidthFixed, px(120));
        ui::TableSetupColumn(gmsg::col_h, ImGuiTableColumnFlags_WidthFixed, px(80));
        ui::TableSetupColumn(gmsg::col_code, ImGuiTableColumnFlags_WidthFixed, px(110));
        ui::TableSetupColumn(gmsg::col_layer, ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        ImGuiListClipper clip;
        clip.Begin((int)shown.size());
        while (clip.Step()) {
            for (int r = clip.DisplayStart; r < clip.DisplayEnd; r++) {
                const int i = shown[(size_t)r];
                const auto& p = g.pickets[(size_t)i];
                ImGui::PushID(i);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                bool on = g.picket_on[(size_t)i] != 0;
                if (ui::CheckboxRaw("##on", &on)) g.picket_on[(size_t)i] = on;
                ImGui::TableNextColumn();
                ui::TextRaw(p.name);
                ImGui::TableNextColumn();
                ui::TextRaw(fixed(g.pickets_swap ? p.y : p.x, 3));
                ImGui::TableNextColumn();
                ui::TextRaw(fixed(g.pickets_swap ? p.x : p.y, 3));
                ImGui::TableNextColumn();
                if (p.has_h) ui::TextRaw(fixed(p.h, 3));
                ImGui::TableNextColumn();
                ui::TextRaw(p.code);
                ImGui::TableNextColumn();
                ui::TextRaw(p.layer);
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }
    long long ticked = 0;
    for (uint8_t v : g.picket_on) ticked += v != 0;
    ImGui::SetNextItemWidth(px(140));
    ui::ComboRaw("##pkrole", &g.pickets_role, {&gmsg::role_control, &gmsg::role_check});
    ImGui::SameLine();
    ui::Text(gmsg::pickets_count, {ticked, (long long)g.pickets.size()});
    ImGui::SameLine();
    ImGui::BeginDisabled(ticked == 0);
    if (ui::Button(gmsg::pickets_add)) {
        int n = 0, auto_no = 1;
        for (size_t i = 0; i < g.pickets.size(); i++) {
            if (!g.picket_on[i]) continue;
            const auto& p = g.pickets[i];
            geo::GeoPoint q;
            q.name = p.name;
            while (q.name.empty() || (p.name.empty() && g.gp.find(q.name))) q.name = "pk" + std::to_string(auto_no++);
            q.x = g.pickets_swap ? p.y : p.x;
            q.y = g.pickets_swap ? p.x : p.y;
            q.h = p.h;
            q.has_h = p.has_h;
            q.code = p.code;
            q.role = g.pickets_role == 1 ? geo::Role::Check : geo::Role::Control;
            if (geo::GeoPoint* old = g.gp.find(q.name)) *old = q;
            else g.gp.points.push_back(q);
            n++;
        }
        g.status = spirula::i18n::format(gmsg::imported, {(long long)n, 0LL});
        g.status_err = false;
        g.stale = true;
        g.plan_count = -1;
        geo_save();
        g.pickets.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ui::Button(gmsg::cancel)) {
        g.pickets.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

// ---------------------------------------------------------------------------
// 1. Control points
// ---------------------------------------------------------------------------

void SurveySession::draw_control_page() {
    GeoState& g = *_geo;
    // ---- imports ----
    if (ui::Button(gmsg::import_catalog)) {
        if (g.latlon && !g.crs_ok) {
            g.status = gmsg::latlon_need.get();
            g.status_err = true;
        } else {
            g.ask = GeoState::Ask::Catalog;
            g.dlg.open(gmsg::import_catalog.get(), FileDialog::Mode::File, {".txt", ".csv", ".dxf", ".xyz", ".dat"});
        }
    }
    ui::help_on_hover(gmsg::import_catalog_help);
    ImGui::SameLine();
    if (ui::Button(gmsg::pickets_btn)) {
        g.ask = GeoState::Ask::Pickets;
        g.dlg.open(gmsg::pickets_btn.get(), FileDialog::Mode::File, {".dxf"});
    }
    ui::help_on_hover(gmsg::pickets_help);
    ImGui::SameLine();
    if (ui::Button(gmsg::paste)) {
        const char* clip = ImGui::GetClipboardText();
        std::string err;
        const int n = clip ? geo::import_catalog_text(clip, g.gp, err, g.latlon && g.crs_ok ? &g.crs : nullptr) : 0;
        g.status = n > 0 ? spirula::i18n::format(gmsg::imported, {(long long)n, 0LL})
                         : spirula::i18n::format(gmsg::import_failed, {gmsg::paste.get(), err});
        g.status_err = n <= 0;
        if (n > 0) {
            g.stale = true;
            geo_save();
        }
    }
    ui::help_on_hover(gmsg::import_catalog_help);
    ImGui::SameLine();
    if (ui::Button(gmsg::add)) {
        geo::GeoPoint q;
        int n = (int)g.gp.points.size() + 1;
        do q.name = "P" + std::to_string(n++);
        while (g.gp.find(q.name));
        g.gp.points.push_back(q);
        g.sel = (int)g.gp.points.size() - 1;
        geo_save();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(g.sel < 0 || g.sel >= (int)g.gp.points.size());
    if (ui::Button(gmsg::delete_point)) {
        g.gp.remove_point(g.gp.points[(size_t)g.sel].name);
        g.sel = -1;
        g.stale = true;
        geo_save();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ui::Button(gmsg::import_mooncloud)) {
        g.ask = GeoState::Ask::Mooncloud;
        g.dlg.open(gmsg::import_mooncloud.get(), FileDialog::Mode::File, {".json"});
    }
    ui::help_on_hover(gmsg::import_mooncloud_help);
    ImGui::SameLine();
    ui::Checkbox(gmsg::latlon, &g.latlon);
    ui::help_on_hover(gmsg::latlon_help);

    // ---- the table; the system and the fit beside it ----
    const float side_w = px(380);
    const float table_w = std::max(px(300), ImGui::GetContentRegionAvail().x - side_w - ImGui::GetStyle().ItemSpacing.x);
    ImGui::BeginChild("##ctltable", ImVec2(table_w, 0));
    if (g.gp.points.empty()) ui::TextDisabledWrapped(gmsg::no_points);
    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_BordersOuter |
                                  ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_Resizable;
    if (!g.gp.points.empty() && ImGui::BeginTable("##geopoints", 10, flags, ImVec2(0, 0))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ui::TableSetupColumnRaw("##sel", ImGuiTableColumnFlags_WidthFixed, px(30));
        ui::TableSetupColumn(gmsg::col_name, ImGuiTableColumnFlags_WidthFixed, px(110));
        ui::TableSetupColumn(gmsg::col_x, ImGuiTableColumnFlags_WidthFixed, px(125));
        ui::TableSetupColumn(gmsg::col_y, ImGuiTableColumnFlags_WidthFixed, px(125));
        ui::TableSetupColumn(gmsg::col_h, ImGuiTableColumnFlags_WidthFixed, px(90));
        ui::TableSetupColumn(gmsg::col_role, ImGuiTableColumnFlags_WidthFixed, px(120));
        ui::TableSetupColumn(gmsg::col_marks, ImGuiTableColumnFlags_WidthFixed, px(50));
        ui::TableSetupColumn(gmsg::col_dplan, ImGuiTableColumnFlags_WidthFixed, px(70));
        ui::TableSetupColumn(gmsg::col_dh, ImGuiTableColumnFlags_WidthFixed, px(70));
        ui::TableSetupColumn(gmsg::col_code, ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (int i = 0; i < (int)g.gp.points.size(); i++) {
            auto& p = g.gp.points[(size_t)i];
            const auto* r = result_of(g.sol, p.name);
            const bool measured = p.role == geo::Role::Measure;
            bool edited = false;
            ImGui::PushID(i);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (ui::SelectableRaw(std::to_string(i + 1).c_str(), g.sel == i)) g.sel = i;
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-1);
            std::string name = p.name;
            if (ui::InputTextRaw("##name", &name) && !name.empty() && !g.gp.find(name)) {
                // The marks are keyed by name: they follow the rename.
                if (auto m = g.gp.marks.find(p.name); m != g.gp.marks.end()) {
                    g.gp.marks[name] = std::move(m->second);
                    g.gp.marks.erase(p.name);
                }
                if (g.point == p.name) g.point = name;
                p.name = name;
                edited = true;
            }
            if (measured) {
                // Found from its marks; shown, not typed.
                double xyh[3] = {0, 0, 0};
                bool have = false;
                if (r && r->fitted) {
                    xyh[0] = r->x, xyh[1] = r->y, xyh[2] = r->h;
                    have = true;
                } else if (r && r->triangulated) {
                    const auto s = geo_survey(r->model);
                    std::copy(s.begin(), s.end(), xyh);
                    have = true;
                }
                for (int k = 0; k < 3; k++) {
                    ImGui::TableNextColumn();
                    if (have) ui::TextDisabledRaw(fixed(xyh[k], 3));
                }
            } else {
                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-1);
                edited |= ui::InputDoubleRaw("##x", &p.x, 0.0, 0.0, "%.3f");
                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-1);
                edited |= ui::InputDoubleRaw("##y", &p.y, 0.0, 0.0, "%.3f");
                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-1);
                if (ui::InputDoubleRaw("##h", &p.h, 0.0, 0.0, "%.3f")) {
                    p.has_h = true;
                    edited = true;
                }
            }
            ImGui::TableNextColumn();
            int role = (int)p.role;
            ImGui::SetNextItemWidth(-1);
            if (ui::ComboRaw("##role", &role,
                             {&gmsg::role_control, &gmsg::role_check, &gmsg::role_off, &gmsg::role_measure})) {
                p.role = (geo::Role)role;
                edited = true;
            }
            ui::help_on_hover(gmsg::role_help);
            ImGui::TableNextColumn();
            ui::TextRaw(std::to_string(g.gp.mark_count(p.name)));
            ImGui::TableNextColumn();
            if (r && r->fitted && !measured) {
                const double plan = std::hypot(r->dx, r->dy);
                if (plan > 0.05 && p.role != geo::Role::Off) ui::TextColoredRaw(ImVec4(1, 0.55f, 0.4f, 1), fixed(plan, 3));
                else ui::TextRaw(fixed(plan, 3));
                ImGui::TableNextColumn();
                if (p.has_h) ui::TextRaw(fixed(r->dh, 3));
            } else {
                ImGui::TableNextColumn();
            }
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-1);
            edited |= ui::InputTextRaw("##code", &p.code);
            if (edited) {
                g.stale = true;
                g.plan_count = -1;
                geo_save();
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();
    ImGui::SameLine();

    ImGui::BeginChild("##ctlside", ImVec2(0, 0));
    const float full = ImGui::GetContentRegionAvail().x;
    draw_dataset_state();
    draw_crs(full);
    ui::SeparatorText(gmsg::sec_place);
    ImGui::BeginDisabled(!g.ds);
    if (ui::Button(gmsg::place, ImVec2(full, 0))) geo_place();
    ImGui::EndDisabled();
    ui::help_on_hover(gmsg::place_help);
    if (g.sol.ok) {
        ui::TextWrapped(gmsg::placed_control, {fixed(g.sol.rms_plan_control, 3), fixed(g.sol.rms_h_control, 3),
                                               (long long)g.sol.n_control});
        if (g.sol.n_check)
            ui::TextWrapped(gmsg::placed_check, {fixed(g.sol.rms_plan_check, 3), fixed(g.sol.rms_h_check, 3),
                                                 (long long)g.sol.n_check});
    } else {
        ui::TextDisabledWrapped(gmsg::local_hint);
    }
    if (g.gp.placed)
        ui::TextDisabledWrapped(gmsg::offset_note, {fixed(g.gp.offset[0], 0), fixed(g.gp.offset[1], 0),
                                                    fixed(g.gp.offset[2], 0)});
    if (g.sel >= 0 && g.sel < (int)g.gp.points.size()) {
        const auto& p = g.gp.points[(size_t)g.sel];
        if (g.crs_ok && p.role != geo::Role::Measure) {
            double lat, lon;
            geo::crs_to_wgs84(g.crs, p.x, p.y, lat, lon);
            ui::TextDisabled(gmsg::point_latlon, {fixed(lat, 7), fixed(lon, 7)});
        }
        if (const auto* r = result_of(g.sol, p.name); r && r->triangulated && !r->worst_image.empty())
            ui::TextDisabledWrapped(gmsg::worst_mark, {fixed(r->reproj_max, 1), r->worst_image});
        if (ui::Button(gmsg::mark_photos, ImVec2(full, 0))) {
            _tab = Tab::Photos;
            _tab_forced = true;
            g.point = p.name;
            g.cam = -1;
        }
    }
    ImGui::BeginDisabled(!g.sol.ok);
    if (ui::Button(gmsg::export_points, ImVec2(full, 0))) {
        g.ask = GeoState::Ask::Export;
        g.dlg.open(gmsg::export_points.get(), FileDialog::Mode::Save, {".csv"}, g.md.directory, false, "points.csv");
    }
    ImGui::EndDisabled();
    draw_status(full);
    ImGui::EndChild();
}

// ---------------------------------------------------------------------------
// 3. The plan
// ---------------------------------------------------------------------------

// A sample of the model seen from above, rebuilt when the model or its
// placement changes.
void SurveySession::plan_refresh() {
    GeoState& g = *_geo;
    EditDoc& doc = *_e._doc;
    const Sim3 to_saved = doc.view_frame().inverse() * doc.placement();
    const double zero[3] = {0, 0, 0};
    const double* off = g.gp.placed ? g.gp.offset : zero;
    const int was = doc.layer();
    doc.set_layer(0);
    const int64_t n = doc.count();
    const bool same = g.plan_doc == &doc && g.plan_count == n && (g.plan_for.inverse() * to_saved).is_identity(1e-12) &&
                      std::equal(off, off + 3, g.plan_off);
    if (same) {
        doc.set_layer(was);
        return;
    }
    g.plan_doc = &doc;
    g.plan_count = n;
    g.plan_for = to_saved;
    std::copy(off, off + 3, g.plan_off);
    g.plan_xyh.clear();
    g.plan_rgb.clear();
    std::vector<float> col;
    const bool coloured = doc.colours(col) && (int64_t)col.size() == n * 3;
    const float* p = doc.positions_of(0);
    const std::vector<uint8_t>& alive = doc.alive_of(0);
    doc.set_layer(was);
    const int64_t step = std::max<int64_t>(1, n / 60000);
    for (int64_t i = 0; i < n; i += step) {
        if (!alive.empty() && !alive[(size_t)i]) continue;
        const double a[3] = {p[i * 3], p[i * 3 + 1], p[i * 3 + 2]};
        double s[3];
        to_saved.apply(a, s);
        // Kept relative to the offset: float holds millimetres there.
        g.plan_xyh.insert(g.plan_xyh.end(), {(float)s[1], (float)s[0], (float)s[2]});
        ImU32 c = IM_COL32(150, 150, 150, 255);
        if (coloured) {
            auto ch = [&](int k) { return (int)std::lround(std::clamp(col[(size_t)(i * 3 + k)], 0.0f, 1.0f) * 255.0f); };
            c = IM_COL32(ch(0), ch(1), ch(2), 255);
        }
        g.plan_rgb.push_back(c);
    }
}

void SurveySession::draw_plan_page() {
    GeoState& g = *_geo;
    plan_refresh();
    const double zero[3] = {0, 0, 0};
    const double* off = g.gp.placed ? g.gp.offset : zero;
    const float side_w = px(360);
    const float w = std::max(px(200), ImGui::GetContentRegionAvail().x - side_w - ImGui::GetStyle().ItemSpacing.x);
    ImGui::BeginChild("##plancanvas", ImVec2(w, 0), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    const ImVec2 c0 = ImGui::GetCursorScreenPos();
    const ImVec2 size = ImGui::GetContentRegionAvail();
    ui::InvisibleButtonRaw("##plan", ImVec2(std::max(size.x, 1.0f), std::max(size.y, 1.0f)),
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const bool hovered = ImGui::IsItemHovered();
    const ImGuiIO& io = ImGui::GetIO();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Everything worth seeing, for the first view and "Show all".
    if (g.plan_scale <= 0) {
        double lo[2] = {1e300, 1e300}, hi[2] = {-1e300, -1e300};
        auto grow = [&](double x, double y) {
            lo[0] = std::min(lo[0], x), hi[0] = std::max(hi[0], x);
            lo[1] = std::min(lo[1], y), hi[1] = std::max(hi[1], y);
        };
        for (size_t i = 0; i + 2 < g.plan_xyh.size(); i += 3) grow(g.plan_xyh[i] + off[1], g.plan_xyh[i + 1] + off[0]);
        for (const auto& f : g.features)
            for (const auto& v : f.v) grow(v[0], v[1]);
        if (lo[0] > hi[0]) lo[0] = lo[1] = -10, hi[0] = hi[1] = 10;
        g.plan_centre[0] = (lo[0] + hi[0]) / 2;
        g.plan_centre[1] = (lo[1] + hi[1]) / 2;
        const double span = std::max({hi[0] - lo[0], hi[1] - lo[1], 1.0});
        g.plan_scale = (float)(std::min(size.x, size.y) * 0.9 / span);
    }
    auto screen = [&](double x, double y) {
        return ImVec2(c0.x + size.x / 2 + (float)((y - g.plan_centre[1]) * g.plan_scale),
                      c0.y + size.y / 2 - (float)((x - g.plan_centre[0]) * g.plan_scale));
    };
    const double mx = g.plan_centre[0] - (io.MousePos.y - c0.y - size.y / 2) / g.plan_scale;
    const double my = g.plan_centre[1] + (io.MousePos.x - c0.x - size.x / 2) / g.plan_scale;
    if (hovered && io.MouseWheel != 0) {
        const float k = std::pow(1.25f, io.MouseWheel);
        g.plan_scale = std::clamp(g.plan_scale * k, 0.01f, 20000.0f);
        // The point under the pointer stays under it.
        g.plan_centre[0] = mx + (io.MousePos.y - c0.y - size.y / 2) / g.plan_scale;
        g.plan_centre[1] = my - (io.MousePos.x - c0.x - size.x / 2) / g.plan_scale;
    }
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Right, 0.0f)) {
        g.plan_centre[0] += io.MouseDelta.y / g.plan_scale;
        g.plan_centre[1] -= io.MouseDelta.x / g.plan_scale;
    }

    dl->PushClipRect(c0, ImVec2(c0.x + size.x, c0.y + size.y), true);
    dl->AddRectFilled(c0, ImVec2(c0.x + size.x, c0.y + size.y), IM_COL32(24, 26, 30, 255));
    // The cloud; the nearest sample under the pointer lends its height.
    float near_d = px(8);
    double near_h = 0;
    bool near_ok = false;
    if (g.plan_cloud) {
        const float r = std::clamp(g.plan_scale * 0.03f, 1.0f, 3.0f);
        for (size_t i = 0, k = 0; i + 2 < g.plan_xyh.size(); i += 3, k++) {
            const ImVec2 p = screen(g.plan_xyh[i] + off[1], g.plan_xyh[i + 1] + off[0]);
            if (p.x < c0.x || p.y < c0.y || p.x > c0.x + size.x || p.y > c0.y + size.y) continue;
            dl->AddRectFilled(ImVec2(p.x - r, p.y - r), ImVec2(p.x + r, p.y + r), g.plan_rgb[k]);
            if (hovered) {
                const float d = std::hypot(p.x - io.MousePos.x, p.y - io.MousePos.y);
                if (d < near_d) {
                    near_d = d;
                    near_h = g.plan_xyh[i + 2] + off[2];
                    near_ok = true;
                }
            }
        }
    }

    // Snap targets: drawn vertices, then control and measured points.
    float snap_d = px(10);
    std::array<double, 3> snap{};
    int snap_feat = -1, snap_vtx = -1;
    bool snapped = false;
    auto try_snap = [&](const std::array<double, 3>& v, int f, int k) {
        const ImVec2 p = screen(v[0], v[1]);
        const float d = std::hypot(p.x - io.MousePos.x, p.y - io.MousePos.y);
        if (hovered && d < snap_d) {
            snap_d = d;
            snap = v;
            snap_feat = f;
            snap_vtx = k;
            snapped = true;
        }
    };
    for (int i = 0; i < (int)g.features.size(); i++) {
        const auto& f = g.features[(size_t)i];
        const bool sel = i == g.feat_sel;
        const ImU32 col = sel ? IM_COL32(255, 230, 60, 255) : IM_COL32(235, 235, 235, 255);
        std::vector<ImVec2> s;
        for (int k = 0; k < (int)f.v.size(); k++) {
            s.push_back(screen(f.v[(size_t)k][0], f.v[(size_t)k][1]));
            if (!(i == g.move_feat && k == g.move_vtx)) try_snap(f.v[(size_t)k], i, k);
        }
        if (f.kind != geo::FeatureKind::Point && s.size() >= 2)
            dl->AddPolyline(s.data(), (int)s.size(), col, f.kind == geo::FeatureKind::Polygon ? ImDrawFlags_Closed : 0,
                            px(sel ? 2.5f : 1.5f));
        for (const auto& p : s) dl->AddCircleFilled(p, px(3), col);
        if (!s.empty() && !f.name.empty()) outlined(dl, ImVec2(s[0].x + px(6), s[0].y - px(16)), col, f.name);
    }
    for (const auto& p : g.gp.points) {
        std::array<double, 3> v{p.x, p.y, p.h};
        if (p.role == geo::Role::Measure) {
            const auto* r = result_of(g.sol, p.name);
            if (!r || !r->triangulated) continue;
            if (r->fitted) v = {r->x, r->y, r->h};
            else v = geo_survey(r->model);
        }
        const ImVec2 at = screen(v[0], v[1]);
        const float t = px(6);
        const ImU32 col = role_colour(p.role);
        dl->AddTriangleFilled(ImVec2(at.x, at.y - t), ImVec2(at.x + t, at.y + t * 0.8f), ImVec2(at.x - t, at.y + t * 0.8f), col);
        outlined(dl, ImVec2(at.x + px(8), at.y - px(8)), col, p.name);
        if (_tool != Tool::Move) try_snap(v, -1, -1);
    }
    if (_tool != Tool::Move)
        for (const auto& v : g.pts) try_snap(v, -1, -1);

    // The pointer as a survey point.
    const bool picking = picks();
    std::array<double, 3> at{mx, my, near_ok ? near_h : 0.0};
    if (snapped) at = snap;
    if (hovered) {
        g.hover_ok = true;
        g.snapped = snapped;
        std::copy(at.begin(), at.end(), g.hover);
    }
    if (picking) {
        auto live = g.pts;
        int kind = _tool == Tool::Measure ? g.measure_kind : 1;
        if (_tool == Tool::Contour && g.code >= 0 && g.code < (int)g.classes.size())
            kind = std::max(1, (int)g.classes[(size_t)g.code].kind);
        else if (_tool == Tool::Contour && g.draw_kind == 2) kind = 2;
        if (hovered && !live.empty() && kind != 0 && !(_tool == Tool::Measure && g.measure_done)) live.push_back(at);
        std::vector<ImVec2> s;
        for (const auto& v : live) s.push_back(screen(v[0], v[1]));
        const ImU32 col = IM_COL32(255, 140, 40, 255);
        if (s.size() >= 2 && _tool != Tool::Point && _tool != Tool::Move)
            dl->AddPolyline(s.data(), (int)s.size(), col, kind == 2 ? ImDrawFlags_Closed : 0, px(2));
        if (_tool != Tool::Move)
            for (const auto& p : s) dl->AddCircleFilled(p, px(3.5f), col);
    }
    if (g.move_feat >= 0 && g.move_feat < (int)g.features.size()) {
        const auto& fv = g.features[(size_t)g.move_feat].v;
        if (g.move_vtx >= 0 && g.move_vtx < (int)fv.size()) {
            const auto& v = fv[(size_t)g.move_vtx];
            dl->AddCircle(screen(v[0], v[1]), px(9), g.move_dirty ? IM_COL32(255, 150, 40, 255) : IM_COL32(255, 230, 60, 255),
                          0, px(2.5f));
            if (g.move_dirty)
                dl->AddLine(screen(g.move_orig[0], g.move_orig[1]), screen(v[0], v[1]), IM_COL32(255, 150, 40, 160), px(1.5f));
        }
    }
    if (hovered) {
        const ImVec2 p = screen(at[0], at[1]);
        const ImU32 col = snapped ? IM_COL32(255, 230, 60, 255) : IM_COL32(255, 255, 255, 255);
        dl->AddCircle(p, px(5), col, 0, px(1.5f));
        char line[160];
        if (snapped || near_ok) std::snprintf(line, sizeof line, "X %.3f  Y %.3f  H %.3f", at[0], at[1], at[2]);
        else std::snprintf(line, sizeof line, "X %.3f  Y %.3f", at[0], at[1]);
        outlined(dl, ImVec2(p.x + px(12), p.y + px(10)), col, line);
    }
    // A scale bar: a round number of metres about a sixth of the width.
    {
        const double want = size.x / 6.0 / g.plan_scale;
        const double mag = std::pow(10.0, std::floor(std::log10(std::max(want, 1e-6))));
        const double len = (want / mag >= 5 ? 5 : want / mag >= 2 ? 2 : 1) * mag;
        const ImVec2 a(c0.x + px(14), c0.y + size.y - px(18));
        const ImVec2 b(a.x + (float)(len * g.plan_scale), a.y);
        dl->AddLine(a, b, IM_COL32_WHITE, px(2));
        char t[48];
        std::snprintf(t, sizeof t, len >= 1 ? "%.0f m" : "%.2f m", len);
        outlined(dl, ImVec2(a.x, a.y - px(18)), IM_COL32_WHITE, t);
    }
    dl->PopClipRect();

    // Clicks: the same tools as in the 3D view.
    const bool click = ImGui::IsItemDeactivated() && ImGui::IsMouseReleased(ImGuiMouseButton_Left) &&
                       io.MouseDragMaxDistanceSqr[ImGuiMouseButton_Left] < px(4) * px(4);
    if (click && hovered) {
        if (_tool == Tool::Move && snapped && snap_feat >= 0) {
            move_take(snap_feat, snap_vtx);
        } else if (picking) {
            tool_click(at);
        } else {
            g.readout_ok = true;
            std::copy(at.begin(), at.end(), g.readout);
            // A click on a feature's vertex selects the feature.
            if (snapped && snap_feat >= 0) g.feat_sel = snap_feat;
        }
    }
    if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && _tool == Tool::Contour) geo_finish();
    ImGui::EndChild();
    ImGui::SameLine();

    ImGui::BeginChild("##planside", ImVec2(0, 0), ImGuiChildFlags_Borders);
    const float full = ImGui::GetContentRegionAvail().x;
    geo_keys();
    ui::TextDisabledWrapped(gmsg::plan_help);
    ui::Checkbox(gmsg::plan_cloud, &g.plan_cloud);
    ImGui::SameLine();
    if (ui::Button(gmsg::plan_fit)) g.plan_scale = 0;
    if (!g.gp.placed) ui::TextColoredWrapped(ImVec4(1, 0.7f, 0.3f, 1), gmsg::not_placed);
    ImGui::Separator();
    switch (_tool) {
        case Tool::Measure: draw_measure_panel(full); break;
        case Tool::Move: draw_move_panel(full); break;
        case Tool::Point:
            ui::TextDisabledWrapped(gmsg::point_help);
            draw_code_picker(full);
            break;
        case Tool::Contour:
            ui::TextDisabledWrapped(gmsg::draw_help);
            draw_code_picker(full);
            break;
        default:
            if (g.readout_ok) {
                char line[160];
                std::snprintf(line, sizeof line, "X %.3f   Y %.3f   H %.3f", g.readout[0], g.readout[1], g.readout[2]);
                ui::TextRaw(line);
            }
            break;
    }
    ImGui::Separator();
    draw_feature_list(full);
    draw_status(full);
    ImGui::EndChild();
}

}  // namespace gui
