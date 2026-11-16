// EditGeo.cpp -- the Reference tab: control points, their marks on the photos,
// and the model placed in survey coordinates. Frames: docs/geodesy.md.

#include "app/AppPaths.h"
#include "app/gui/FileDialog.h"
#include "app/gui/Layout.h"
#include "app/gui/Picture.h"
#include "app/gui/PreviewTexture.h"
#include "app/gui/Ui.h"
#include "app/gui/ViewportPanel.h"
#include "app/gui/edit/EditSession.h"
#include "app/gui/geo/GeoState.h"
#include "app/gui/geo/SurveySession.h"
#include "core/ImageOrient.h"
#include "data/DatasetParser.h"
#include "data/GeoCrs.h"
#include "data/GeoProject.h"
#include "data/GeoSolve.h"
#include "data/ImageProbe.h"
#include "i18n/catalog/Geo.h"

#include "imgui.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>

namespace gmsg = spirula::i18n::msg::geo;
namespace geo = spirula::geo;
using spirula::Sim3;

namespace gui {

namespace {

std::string base_name(const std::string& p) {
    const size_t k = p.find_last_of("/\\");
    return k == std::string::npos ? p : p.substr(k + 1);
}

int turns_of(const ParsedDataset& ds, int cam) {
    return ds.exif_quarter_turns.empty() ? 0 : ds.exif_quarter_turns[(size_t)cam] & 3;
}

std::string fixed(double v, int digits) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.*f", digits, v);
    return buf;
}

const geo::PointResult* result_of(const geo::Solution& s, const std::string& name) {
    for (const auto& r : s.points)
        if (r.name == name) return &r;
    return nullptr;
}

std::string crs_list_path() {
    return (std::filesystem::u8path(app::config_dir()) / "crs.json").u8string();
}

ImU32 role_colour(geo::Role r, int alpha = 255) {
    switch (r) {
        case geo::Role::Control: return IM_COL32(90, 230, 115, alpha);
        case geo::Role::Check: return IM_COL32(85, 175, 255, alpha);
        default: return IM_COL32(170, 170, 170, alpha);
    }
}

void outlined_text(ImDrawList* dl, ImVec2 at, ImU32 colour, const std::string& s) {
    dl->AddText(ImVec2(at.x + 1, at.y + 1), IM_COL32_BLACK, s.c_str());
    dl->AddText(at, colour, s.c_str());
}

}  // namespace

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

void SurveySession::geo_close() {
    if (!_geo) return;
    _geo->join();
    _geo->tex.reset();
    _geo.reset();
}

void SurveySession::geo_open() {
    if (_geo || !_e._doc) return;
    _geo = std::make_shared<GeoState>();
    GeoState& g = *_geo;
    g.md = spirula::resolve_marker_dataset(_e._doc->source_path());
    if (_e._doc->dataset()) g.md.file_from_dataset = {};
    // Without a dataset the model carries its offset and system in a sidecar,
    // written when a placed model is saved.
    g.path = g.md.directory.empty() ? _e._doc->source_path() + ".geo.json" : geo::geo_path(g.md.directory);
    g.vec_path = geo::vectors_path(g.md.directory, _e._doc->source_path());
    std::string err;
    if (std::filesystem::exists(std::filesystem::u8path(g.path)) && !geo::load_geo(g.path, g.gp, err)) {
        g.status = spirula::i18n::format(gmsg::import_failed, {g.path, err});
        g.status_err = true;
    }
    if (!g.gp.crs_def.empty()) g.crs_ok = geo::parse_crs(g.gp.crs_def, g.crs, err);
    if (std::filesystem::exists(std::filesystem::u8path(g.vec_path)) &&
        !geo::load_vectors(g.vec_path, g.features, g.vec_survey, err)) {
        g.status = spirula::i18n::format(gmsg::import_failed, {g.vec_path, err});
        g.status_err = true;
    }
    geo_load_classifier_setting();
    if (g.md.directory.empty()) return;
    // A model saved after placement records where the dataset sits in it: the
    // run's own scene_transform.json still describes the file before the move.
    geo::GeoProject side;
    const std::string side_path = _e._doc->source_path() + ".geo.json";
    if (!_e._doc->dataset() && std::filesystem::exists(std::filesystem::u8path(side_path)) &&
        geo::load_geo(side_path, side, err) && side.model_frame)
        g.md.file_from_dataset = side.file_from_dataset;

    GeoState* gs = &g;
    const ParsedDataset* given = _e._doc->dataset();
    g.loading = true;
    g.loader = std::thread([gs, given]() {
        try {
            const ParsedDataset* ds = given;
            if (!ds) {
                auto cfg = gs->md.config;
                cfg.require_image_files = false;
                gs->own = std::make_unique<ParsedDataset>(parse_dataset(gs->md.directory, cfg, ""));
                ds = gs->own.get();
            }
            gs->dims.assign(ds->image_filenames.size(), {0, 0});
            for (size_t i = 0; i < ds->image_filenames.size(); i++) {
                int w = 0, h = 0;
                if (!probe_image_size(ds->image_filenames[i].c_str(), &w, &h)) continue;
                if (turns_of(*ds, (int)i) & 1) std::swap(w, h);
                gs->dims[i] = {w, h};
            }
            gs->ds = ds;
        } catch (const std::exception& e) {
            gs->load_error = e.what();
        }
        gs->loading = false;
    });
}

void SurveySession::geo_save() {
    if (!_geo || _geo->path.empty()) return;
    std::string err;
    if (!geo::save_geo(_geo->path, _geo->gp, err)) {
        _geo->status = spirula::i18n::format(gmsg::save_failed, {_geo->path, err});
        _geo->status_err = true;
    }
}

spirula::Sim3 SurveySession::geo_model_from_dataset() const {
    return _e._doc->file_placement() * _geo->md.file_from_dataset;
}

void SurveySession::geo_solve() {
    GeoState& g = *_geo;
    if (!g.ds || g.loading) return;
    const Sim3 m = geo_model_from_dataset();
    if (!g.stale && (g.solved_for.inverse() * m).is_identity(1e-12)) return;
    g.sol = geo::solve(g.gp, *g.ds, m);
    g.solved_for = m;
    g.stale = false;
}

// Where a point is in the model frame: triangulated from its marks, else from
// the catalog once the fit (or an earlier placement) says where the survey is.
bool SurveySession::geo_model_point(const std::string& name, double out[3]) const {
    const GeoState& g = *_geo;
    if (const auto* r = result_of(g.sol, name); r && r->triangulated) {
        std::copy(r->model, r->model + 3, out);
        return true;
    }
    const geo::GeoPoint* p = g.gp.find(name);
    if (!p) return false;
    if (g.sol.ok) {
        double l[3];
        geo::survey_to_local(g.sol, p->x, p->y, p->h, l);
        g.sol.model_to_local.inverse().apply(l, out);
        return true;
    }
    if (g.gp.placed) {
        out[0] = p->y - g.gp.offset[0];
        out[1] = p->x - g.gp.offset[1];
        out[2] = p->h - g.gp.offset[2];
        return true;
    }
    return false;
}

void SurveySession::geo_place() {
    GeoState& g = *_geo;
    g.stale = true;
    geo_solve();
    if (!g.sol.ok) {
        g.status = spirula::i18n::format(gmsg::place_failed, {g.sol.error});
        g.status_err = true;
        return;
    }
    // The axes that get saved are the file's own, as in the Transform tab.
    if (_e._panel && _e._panel->has_levelling() && _e._panel->level_cameras()) {
        _e._levelling_was = true;
        _e._levelling_touched = true;
        _e._panel->set_level_cameras(false);
    }
    _e.place_saved(g.sol.model_to_local, gmsg::op_place, /*carry=*/true);
    // A drawing made before the first placement is in the model's own
    // coordinates; it goes where the model went.
    if (!g.vec_survey && !g.features.empty()) {
        for (auto& f : g.features) {
            for (auto& p : f.v) {
                double saved[3], moved[3];
                geo_saved(p, saved);
                g.sol.model_to_local.apply(saved, moved);
                p = {moved[1] + g.sol.offset[1], moved[0] + g.sol.offset[0], moved[2] + g.sol.offset[2]};
            }
        }
    }
    g.gp.placed = true;
    std::copy(g.sol.offset, g.sol.offset + 3, g.gp.offset);
    g.vec_survey = true;
    geo_save();
    if (!g.features.empty()) geo_save_vectors();
    g.stale = true;
    geo_solve();
    g.status = spirula::i18n::format(gmsg::placed_control,
                                     {fixed(g.sol.rms_plan_control, 3), fixed(g.sol.rms_h_control, 3),
                                      (long long)g.sol.n_control});
    g.status_err = false;
    _e.note(g.status);
}

// ---------------------------------------------------------------------------
// Coordinate system
// ---------------------------------------------------------------------------

void SurveySession::geo_pick_crs(const std::string& name, const std::string& def) {
    GeoState& g = *_geo;
    g.gp.crs_name = name;
    g.gp.crs_def = def;
    std::string err;
    g.crs_ok = !def.empty() && geo::parse_crs(def, g.crs, err);
    if (!def.empty() && !g.crs_ok) {
        g.status = spirula::i18n::format(gmsg::crs_bad, {err});
        g.status_err = true;
    }
    geo_save();
}

void SurveySession::draw_crs(float full) {
    GeoState& g = *_geo;
    if (!g.user_read) {
        g.user_read = true;
        std::string err;
        const std::string p = crs_list_path();
        if (std::filesystem::exists(std::filesystem::u8path(p))) geo::load_crs_list(p, g.user, err);
    }
    ui::Text(gmsg::crs_name);
    ImGui::SetNextItemWidth(full);
    const std::string preview = g.gp.crs_name.empty() ? std::string(gmsg::crs_none.get()) : g.gp.crs_name;
    if (ui::BeginComboRaw("##geocrs", preview.c_str(), ImGuiComboFlags_HeightLarge)) {
        if (ImGui::IsWindowAppearing()) {
            g.filter.clear();
            ImGui::SetKeyboardFocusHere();
        }
        ImGui::SetNextItemWidth(-1);
        ui::InputTextWithHintRaw("##geocrsfilter", gmsg::crs_filter, &g.filter);
        std::string f = g.filter;
        for (char& c : f) c = (char)std::tolower((unsigned char)c);
        auto shown = [&](const std::string& s) {
            if (f.empty()) return true;
            std::string l = s;
            for (char& c : l) c = (char)std::tolower((unsigned char)c);
            return l.find(f) != std::string::npos;
        };
        if (ui::Selectable(gmsg::crs_none, g.gp.crs_def.empty())) geo_pick_crs("", "");
        int remove = -1;
        for (int i = 0; i < (int)g.user.size(); i++) {
            const auto& e = g.user[(size_t)i];
            if (!shown(e.name)) continue;
            ImGui::PushID(i);
            if (ui::SelectableRaw(e.name, e.def == g.gp.crs_def)) geo_pick_crs(e.name, e.def);
            if (ImGui::BeginPopupContextItem("##usercrs")) {
                if (ui::MenuItem(gmsg::crs_remove)) remove = i;
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }
        if (!g.user.empty()) ImGui::Separator();
        for (const auto& e : geo::builtin_crs()) {
            if (!shown(e.name)) continue;
            if (ui::SelectableRaw(e.name, e.def == g.gp.crs_def)) geo_pick_crs(e.name, e.def);
        }
        ImGui::EndCombo();
        if (remove >= 0) {
            g.user.erase(g.user.begin() + remove);
            std::string err;
            if (!geo::save_crs_list(crs_list_path(), g.user, err)) {
                g.status = spirula::i18n::format(gmsg::save_failed, {crs_list_path(), err});
                g.status_err = true;
            }
        }
    }
    ui::help_on_hover(gmsg::crs_help);
    if (g.crs_ok && !g.gp.points.empty()) {
        const auto& p = g.gp.points.front();
        double lat, lon;
        geo::crs_to_wgs84(g.crs, p.x, p.y, lat, lon);
        ui::TextDisabled(gmsg::crs_check, {fixed(lat, 6), fixed(lon, 6)});
    }
    if (ui::Button(gmsg::crs_add, ImVec2(full, 0))) g.add_open = !g.add_open;
    if (!g.add_open) return;
    ImGui::Indent();
    const float w = full - ImGui::GetStyle().IndentSpacing;
    ui::Text(gmsg::crs_new_name);
    ImGui::SetNextItemWidth(w);
    ui::InputTextRaw("##geocrsname", &g.add_name);
    ui::Text(gmsg::crs_def);
    ui::help_on_hover(gmsg::crs_def_help);
    ui::InputTextMultilineRaw("##geocrsdef", &g.add_def, ImVec2(w, ImGui::GetTextLineHeight() * 5));
    if (g.add_def.empty()) ui::TextDisabled(gmsg::crs_def_hint);
    const float third = (w - 2 * ImGui::GetStyle().ItemSpacing.x) / 3;
    if (ui::Button(gmsg::crs_from_prj, ImVec2(third, 0))) {
        g.ask = GeoState::Ask::Prj;
        g.dlg.open(gmsg::crs_from_prj.get(), FileDialog::Mode::File, {".prj", ".wkt", ".txt"});
    }
    ImGui::SameLine();
    if (ui::Button(gmsg::add, ImVec2(third, 0))) {
        geo::Crs c;
        std::string err;
        if (geo::parse_crs(g.add_def, c, err)) {
            const std::string name = g.add_name.empty() ? g.add_def.substr(0, 40) : g.add_name;
            g.user.push_back({name, g.add_def});
            if (!geo::save_crs_list(crs_list_path(), g.user, err)) {
                g.status = spirula::i18n::format(gmsg::save_failed, {crs_list_path(), err});
                g.status_err = true;
            }
            geo_pick_crs(name, g.add_def);
            g.add_open = false;
            g.add_name.clear();
            g.add_def.clear();
            g.add_error.clear();
        } else {
            g.add_error = spirula::i18n::format(gmsg::crs_bad, {err});
        }
    }
    ImGui::SameLine();
    if (ui::Button(gmsg::cancel, ImVec2(third, 0))) {
        g.add_open = false;
        g.add_error.clear();
    }
    if (!g.add_error.empty()) ui::TextColoredWrappedRaw(ImVec4(1, 0.45f, 0.4f, 1), g.add_error);
    ImGui::Unindent();
}

// ---------------------------------------------------------------------------
// The tab
// ---------------------------------------------------------------------------

void SurveySession::geo_file_chosen(const std::string& path) {
    GeoState& g = *_geo;
    std::string err;
    switch (g.ask) {
        case GeoState::Ask::Catalog:
        case GeoState::Ask::Mooncloud: {
            const size_t marks_before = [&] {
                size_t n = 0;
                for (const auto& [name, m] : g.gp.marks) n += m.size();
                return n;
            }();
            const int n = g.ask == GeoState::Ask::Catalog
                              ? geo::import_catalog(path, g.gp, err, g.latlon ? &g.crs : nullptr)
                              : geo::import_mooncloud(path, g.gp, err);
            if (n < 0 || (n == 0 && !err.empty())) {
                g.status = spirula::i18n::format(gmsg::import_failed, {base_name(path), err});
                g.status_err = true;
                break;
            }
            size_t marks = 0;
            for (const auto& [name, m] : g.gp.marks) marks += m.size();
            g.status = spirula::i18n::format(gmsg::imported, {(long long)n, (long long)(marks - marks_before)});
            g.status_err = false;
            g.stale = true;
            geo_save();
            break;
        }
        case GeoState::Ask::Export: {
            std::vector<geo::GeoPoint> pts;
            for (const auto& r : g.sol.points) {
                if (!r.fitted) continue;
                geo::GeoPoint p;
                p.name = r.name;
                p.x = r.x;
                p.y = r.y;
                p.h = r.h;
                if (const auto* c = g.gp.find(r.name)) p.code = c->code;
                pts.push_back(p);
            }
            if (!geo::export_points_csv(path, pts, err)) {
                g.status = spirula::i18n::format(gmsg::save_failed, {path, err});
                g.status_err = true;
                break;
            }
            if (g.crs_ok) {
                auto prj = std::filesystem::u8path(path);
                prj.replace_extension(".prj");
                geo::write_text(prj.u8string(), geo::crs_to_wkt(g.crs, g.gp.crs_name), err);
            }
            g.status = spirula::i18n::format(gmsg::exported, {base_name(path)});
            g.status_err = false;
            break;
        }
        case GeoState::Ask::Prj: {
            std::string text;
            if (!geo::read_text(path, text, err)) {
                g.add_error = spirula::i18n::format(gmsg::import_failed, {base_name(path), err});
                break;
            }
            g.add_def = text;
            if (g.add_name.empty()) g.add_name = std::filesystem::u8path(path).stem().u8string();
            break;
        }
        case GeoState::Ask::Classifier: geo_load_classifier(path); break;
        case GeoState::Ask::Model: geo_export_model(path); break;
        case GeoState::Ask::Drawings: geo_export_drawings(path); break;
        case GeoState::Ask::None: break;
    }
    g.ask = GeoState::Ask::None;
}

void SurveySession::draw_control_panel(float full) {
    geo_open();
    GeoState& g = *_geo;

    ui::TextDisabledWrapped(gmsg::tab_geo_help);
    if (g.md.directory.empty()) {
        ui::TextColoredWrapped(ImVec4(1, 0.7f, 0.3f, 1), gmsg::no_dataset);
        return;
    }
    if (g.loading) {
        ui::TextDisabled(gmsg::loading_dataset);
        return;
    }
    if (!g.ds) {
        ui::TextColoredWrapped(ImVec4(1, 0.45f, 0.4f, 1), gmsg::dataset_failed, {g.load_error});
        return;
    }
    const float half = (full - ImGui::GetStyle().ItemSpacing.x) * 0.5f;

    // ---- catalog ----
    ui::SeparatorText(gmsg::sec_catalog);
    draw_crs(full);
    ImGui::Spacing();
    if (ui::Button(gmsg::import_catalog, ImVec2(half, 0))) {
        if (g.latlon && !g.crs_ok) {
            g.status = gmsg::latlon_need.get();
            g.status_err = true;
        } else {
            g.ask = GeoState::Ask::Catalog;
            g.dlg.open(gmsg::import_catalog.get(), FileDialog::Mode::File, {".txt", ".csv", ".xyz", ".dat"});
        }
    }
    ui::help_on_hover(gmsg::import_catalog_help);
    ImGui::SameLine();
    if (ui::Button(gmsg::import_mooncloud, ImVec2(half, 0))) {
        g.ask = GeoState::Ask::Mooncloud;
        g.dlg.open(gmsg::import_mooncloud.get(), FileDialog::Mode::File, {".json"});
    }
    ui::help_on_hover(gmsg::import_mooncloud_help);
    ui::Checkbox(gmsg::latlon, &g.latlon);
    ui::help_on_hover(gmsg::latlon_help);

    // ---- points ----
    if (g.gp.points.empty()) {
        ui::TextDisabledWrapped(gmsg::no_points);
    } else {
        const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                                      ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit;
        const float rows = (float)std::min<size_t>(g.gp.points.size(), 10) + 1.4f;
        if (ImGui::BeginTable("##geopoints", 5, flags, ImVec2(full, ImGui::GetFrameHeightWithSpacing() * rows))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ui::TableSetupColumn(gmsg::col_name, ImGuiTableColumnFlags_WidthStretch);
            ui::TableSetupColumn(gmsg::col_role, ImGuiTableColumnFlags_WidthFixed, px(92));
            ui::TableSetupColumn(gmsg::col_marks, ImGuiTableColumnFlags_WidthFixed, px(36));
            ui::TableSetupColumn(gmsg::col_dplan, ImGuiTableColumnFlags_WidthFixed, px(52));
            ui::TableSetupColumn(gmsg::col_dh, ImGuiTableColumnFlags_WidthFixed, px(52));
            ImGui::TableHeadersRow();
            for (int i = 0; i < (int)g.gp.points.size(); i++) {
                auto& p = g.gp.points[(size_t)i];
                const auto* r = result_of(g.sol, p.name);
                ImGui::PushID(i);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                if (ui::SelectableRaw(p.name.c_str(), g.sel == i,
                                      ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap |
                                          ImGuiSelectableFlags_AllowDoubleClick)) {
                    g.sel = i;
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                        _tab = Tab::Photos;
                _tab_forced = true;
                        g.point = p.name;
                        g.cam = -1;
                    }
                }
                ImGui::TableNextColumn();
                int role = (int)p.role;
                ImGui::SetNextItemWidth(-1);
                if (ui::ComboRaw("##role", &role, {&gmsg::role_control, &gmsg::role_check, &gmsg::role_off})) {
                    p.role = (geo::Role)role;
                    g.stale = true;
                    geo_save();
                }
                ui::help_on_hover(gmsg::role_help);
                ImGui::TableNextColumn();
                ui::TextRaw(std::to_string(g.gp.mark_count(p.name)));
                ImGui::TableNextColumn();
                if (r && r->fitted) {
                    const double plan = std::hypot(r->dx, r->dy);
                    const bool bad = plan > 0.05 && p.role != geo::Role::Off;
                    const std::string s = fixed(plan, 3);
                    if (bad) ui::TextColoredRaw(ImVec4(1, 0.55f, 0.4f, 1), s);
                    else ui::TextRaw(s);
                    ImGui::TableNextColumn();
                    if (p.has_h) ui::TextRaw(fixed(r->dh, 3));
                } else {
                    ImGui::TableNextColumn();
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }

        if (g.sel >= (int)g.gp.points.size()) g.sel = -1;
        if (g.sel >= 0) {
            const auto p = g.gp.points[(size_t)g.sel];
            char line[160];
            if (p.has_h) std::snprintf(line, sizeof line, "X %.3f  Y %.3f  H %.3f", p.x, p.y, p.h);
            else std::snprintf(line, sizeof line, "X %.3f  Y %.3f", p.x, p.y);
            ui::TextRaw(line);
            if (g.crs_ok) {
                double lat, lon;
                geo::crs_to_wgs84(g.crs, p.x, p.y, lat, lon);
                ui::TextDisabled(gmsg::point_latlon, {fixed(lat, 7), fixed(lon, 7)});
            }
            if (const auto* r = result_of(g.sol, p.name); r && r->triangulated && !r->worst_image.empty())
                ui::TextDisabled(gmsg::worst_mark, {fixed(r->reproj_max, 1), r->worst_image});
            if (ui::Button(gmsg::mark_photos, ImVec2(half, 0))) {
                _tab = Tab::Photos;
                _tab_forced = true;
                g.point = p.name;
                g.cam = -1;
            }
            ImGui::SameLine();
            if (ui::Button(gmsg::delete_point, ImVec2(half, 0))) {
                g.gp.remove_point(p.name);
                if (g.point == p.name) g.point.clear();
                g.sel = -1;
                g.stale = true;
                geo_save();
            }
        }
    }

    // ---- placement ----
    ui::SeparatorText(gmsg::sec_place);
    if (ui::Button(gmsg::place, ImVec2(full, 0))) geo_place();
    ui::help_on_hover(gmsg::place_help);
    if (g.sol.ok) {
        ui::Text(gmsg::placed_control, {fixed(g.sol.rms_plan_control, 3), fixed(g.sol.rms_h_control, 3),
                                        (long long)g.sol.n_control});
        if (g.sol.n_check)
            ui::Text(gmsg::placed_check, {fixed(g.sol.rms_plan_check, 3), fixed(g.sol.rms_h_check, 3),
                                          (long long)g.sol.n_check});
    }
    if (g.gp.placed)
        ui::TextDisabledWrapped(gmsg::offset_note, {fixed(g.gp.offset[0], 0), fixed(g.gp.offset[1], 0),
                                                    fixed(g.gp.offset[2], 0)});
    ImGui::BeginDisabled(!g.sol.ok);
    if (ui::Button(gmsg::export_points, ImVec2(full, 0))) {
        g.ask = GeoState::Ask::Export;
        g.dlg.open(gmsg::export_points.get(), FileDialog::Mode::Save, {".csv"}, g.md.directory, false,
                   "points.csv");
    }
    ImGui::EndDisabled();
    if (!g.status.empty()) {
        if (g.status_err) ui::TextColoredWrappedRaw(ImVec4(1, 0.45f, 0.4f, 1), g.status);
        else ui::TextWrappedRaw(g.status);
    }
}

// ---------------------------------------------------------------------------
// Marking on the photos
// ---------------------------------------------------------------------------

// The Photos tab's side: which point is being marked, and how far along.
void SurveySession::draw_marks_panel(float full) {
    geo_open();
    if (!_geo) return;
    GeoState& g = *_geo;
    ui::TextDisabledWrapped(gmsg::marks_help);
    if (g.gp.points.empty()) {
        ui::TextDisabledWrapped(gmsg::no_points);
        return;
    }
    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit;
    if (ImGui::BeginTable("##geomarkpoints", 3, flags, ImVec2(full, ImGui::GetContentRegionAvail().y - px(4)))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ui::TableSetupColumn(gmsg::col_name, ImGuiTableColumnFlags_WidthStretch);
        ui::TableSetupColumn(gmsg::col_marks, ImGuiTableColumnFlags_WidthFixed, px(40));
        ui::TableSetupColumn(gmsg::col_dplan, ImGuiTableColumnFlags_WidthFixed, px(60));
        ImGui::TableHeadersRow();
        for (int i = 0; i < (int)g.gp.points.size(); i++) {
            const auto& p = g.gp.points[(size_t)i];
            ImGui::PushID(i);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (ui::SelectableRaw(p.name.c_str(), g.point == p.name, ImGuiSelectableFlags_SpanAllColumns)) {
                g.point = p.name;
                g.cam = -1;
            }
            ImGui::TableNextColumn();
            ui::TextRaw(std::to_string(g.gp.mark_count(p.name)));
            ImGui::TableNextColumn();
            if (const auto* r = result_of(g.sol, p.name); r && r->fitted) ui::TextRaw(fixed(std::hypot(r->dx, r->dy), 3));
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

void SurveySession::draw_photo_page() {
    geo_open();
    if (!_geo) return;
    GeoState& g = *_geo;
    if (g.md.directory.empty()) {
        ui::TextColoredWrapped(ImVec4(1, 0.7f, 0.3f, 1), gmsg::no_dataset);
        return;
    }
    if (g.loading || !g.ds) {
        if (g.loading) ui::TextDisabled(gmsg::loading_dataset);
        else ui::TextColoredWrapped(ImVec4(1, 0.45f, 0.4f, 1), gmsg::dataset_failed, {g.load_error});
        return;
    }
    const ParsedDataset& ds = *g.ds;
    if (!g.gp.find(g.point)) {
        if (g.gp.points.empty()) {
            ui::TextDisabledWrapped(gmsg::no_points);
            return;
        }
        g.point = g.gp.points.front().name;
        g.cam = -1;
    }

    // A picture that finished loading goes up; the next one is asked for.
    if (!g.pic_busy && g.pic_worker.joinable()) {
        g.pic_worker.join();
        if (g.pic_ok) g.tex.upload(g.pic);
        g.tex_cam = g.pic_ok ? g.pic_want : -1;
        g.pic = Picture{};
        g.recentre = true;
    }
    if (g.cam >= 0 && g.cam != g.pic_want && !g.pic_busy) {
        g.pic_want = g.cam;
        g.pic_busy = true;
        GeoState* gs = &g;
        const std::string file = ds.image_filenames[(size_t)g.cam];
        const int turns = turns_of(ds, g.cam);
        g.pic_worker = std::thread([gs, file, turns]() {
            Picture p;
            bool ok = load_picture(file, "", 8192, p);
            if (ok && turns) {
                std::vector<uint8_t> out(p.rgb.size());
                spirula::orient_pixels(p.rgb.data(), p.w, p.h, 3, turns, false, out.data());
                p.rgb.swap(out);
                spirula::oriented_size(turns, p.w, p.h);
            }
            gs->pic = std::move(p);
            gs->pic_ok = ok;
            gs->pic_busy = false;
        });
    }

    // The photos: marked first, then the ones the point should be on.
    double model[3];
    const bool predicted = geo_model_point(g.point, model);
    const Sim3 mfd = geo_model_from_dataset();
    const auto marks_it = g.gp.marks.find(g.point);
    struct Row {
        int cam;
        int kind;   // 0 marked, 1 predicted, 2 other
        double u, v;
    };
    std::vector<Row> rows;
    for (int c = 0; c < (int)ds.image_filenames.size(); c++) {
        const std::string name = base_name(ds.image_filenames[(size_t)c]);
        Row r{c, 2, 0, 0};
        if (marks_it != g.gp.marks.end() && marks_it->second.count(name)) r.kind = 0;
        else if (predicted && g.dims[(size_t)c][0] > 0 &&
                 geo::project_to_image(ds, c, mfd, model, g.dims[(size_t)c][0], g.dims[(size_t)c][1], r.u, r.v))
            r.kind = 1;
        if (r.kind == 2 && g.only_visible && predicted) continue;
        rows.push_back(r);
    }
    std::stable_sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.kind < b.kind; });
    if (g.cam < 0 && !rows.empty()) g.cam = rows.front().cam;

    ui::Text(gmsg::win_marks, {g.point});
    int at = -1;
    for (int i = 0; i < (int)rows.size(); i++)
        if (rows[(size_t)i].cam == g.cam) at = i;

    ImGui::BeginChild("##geophotos", ImVec2(px(250), -ImGui::GetFrameHeightWithSpacing() * 2.2f),
                      ImGuiChildFlags_Borders);
    for (int i = 0; i < (int)rows.size(); i++) {
        const Row& r = rows[(size_t)i];
        ImGui::PushID(r.cam);
        const std::string name = base_name(ds.image_filenames[(size_t)r.cam]);
        if (ui::SelectableRaw(name, r.cam == g.cam)) g.cam = r.cam;
        if (r.kind < 2) {
            ImGui::SameLine();
            if (r.kind == 0) ui::TextColored(ImVec4(0.45f, 0.9f, 0.5f, 1), gmsg::photo_marked);
            else ui::TextDisabled(gmsg::photo_predicted);
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::SameLine();

    const ImVec2 avail(ImGui::GetContentRegionAvail().x, -ImGui::GetFrameHeightWithSpacing() * 2.2f);
    ImGui::BeginChild("##geocanvas", avail, ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    const ImVec2 c0 = ImGui::GetCursorScreenPos();
    const ImVec2 size = ImGui::GetContentRegionAvail();
    ui::InvisibleButtonRaw("##geoimg", ImVec2(std::max(size.x, 1.0f), std::max(size.y, 1.0f)),
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (g.cam >= 0 && g.tex_cam == g.cam && g.tex.ready() && g.dims[(size_t)g.cam][0] > 0) {
        const float fw = (float)g.dims[(size_t)g.cam][0], fh = (float)g.dims[(size_t)g.cam][1];
        const float fit = std::min(size.x / fw, size.y / fh);
        const std::string img = base_name(ds.image_filenames[(size_t)g.cam]);
        const geo::GeoMark* mark = nullptr;
        if (marks_it != g.gp.marks.end())
            if (auto m = marks_it->second.find(img); m != marks_it->second.end()) mark = &m->second;
        const Row* row = at >= 0 ? &rows[(size_t)at] : nullptr;
        if (g.recentre) {
            g.recentre = false;
            double cu = fw / 2, cv = fh / 2;
            g.zoom = 1;
            if (mark || (row && row->kind == 1)) {
                cu = mark ? mark->u : row->u;
                cv = mark ? mark->v : row->v;
                g.zoom = std::max(1.0f, std::min(fw, fh) / 500.0f);
            }
            const float s = fit * g.zoom;
            g.pan = ImVec2(size.x / 2 - (float)cu * s, size.y / 2 - (float)cv * s);
        }
        const ImGuiIO& io = ImGui::GetIO();
        float s = fit * g.zoom;
        if (ImGui::IsItemHovered() && io.MouseWheel != 0) {
            const ImVec2 m(io.MousePos.x - c0.x, io.MousePos.y - c0.y);
            const ImVec2 uv((m.x - g.pan.x) / s, (m.y - g.pan.y) / s);
            g.zoom = std::clamp(g.zoom * std::pow(1.25f, io.MouseWheel), 0.5f, 64.0f);
            s = fit * g.zoom;
            g.pan = ImVec2(m.x - uv.x * s, m.y - uv.y * s);
        }
        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Right, 0.0f)) {
            g.pan.x += io.MouseDelta.x;
            g.pan.y += io.MouseDelta.y;
        }
        auto screen = [&](double u, double v) {
            return ImVec2(c0.x + g.pan.x + (float)u * s, c0.y + g.pan.y + (float)v * s);
        };
        dl->PushClipRect(c0, ImVec2(c0.x + size.x, c0.y + size.y), true);
        dl->AddImage(g.tex.id(), screen(0, 0), screen(fw, fh));
        if (row && row->kind == 1) {
            const ImVec2 p = screen(row->u, row->v);
            dl->AddCircle(p, px(14), IM_COL32(90, 220, 255, 230), 0, px(1.5f));
        }
        if (const auto* r = result_of(g.sol, g.point); r && r->triangulated) {
            double u, v;
            if (geo::project_to_image(ds, g.cam, mfd, r->model, (int)fw, (int)fh, u, v))
                dl->AddCircleFilled(screen(u, v), px(3), IM_COL32(255, 80, 230, 255));
        }
        if (mark) {
            const ImVec2 p = screen(mark->u, mark->v);
            const float a = px(12), b = px(3);
            for (const ImU32 col : {IM_COL32(0, 0, 0, 220), IM_COL32(255, 230, 60, 255)}) {
                const float w = col == IM_COL32(0, 0, 0, 220) ? px(3) : px(1.5f);
                dl->AddLine(ImVec2(p.x - a, p.y), ImVec2(p.x - b, p.y), col, w);
                dl->AddLine(ImVec2(p.x + b, p.y), ImVec2(p.x + a, p.y), col, w);
                dl->AddLine(ImVec2(p.x, p.y - a), ImVec2(p.x, p.y - b), col, w);
                dl->AddLine(ImVec2(p.x, p.y + b), ImVec2(p.x, p.y + a), col, w);
            }
            outlined_text(dl, ImVec2(p.x + px(10), p.y - px(22)), IM_COL32(255, 230, 60, 255), g.point);
        }
        dl->PopClipRect();

        // A press near the mark drags it; anywhere else a click puts it there.
        const double mu = (io.MousePos.x - c0.x - g.pan.x) / s, mv = (io.MousePos.y - c0.y - g.pan.y) / s;
        if (ImGui::IsItemActivated() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            const ImVec2 p = mark ? screen(mark->u, mark->v) : ImVec2(-1e9f, -1e9f);
            g.mark_drag = mark && std::hypot(io.MousePos.x - p.x, io.MousePos.y - p.y) < px(12);
        }
        if (g.mark_drag && ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left) && mark &&
            mu >= 0 && mv >= 0 && mu < fw && mv < fh) {
            g.gp.marks[g.point][img] = {mu, mv};
            g.stale = true;
        }
        if (ImGui::IsItemDeactivated() && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            const bool click = io.MouseDragMaxDistanceSqr[ImGuiMouseButton_Left] < px(4) * px(4);
            if ((click || g.mark_drag) && mu >= 0 && mv >= 0 && mu < fw && mv < fh) {
                g.gp.marks[g.point][img] = {mu, mv};
                g.stale = true;
                geo_save();
            }
            g.mark_drag = false;
        }
        // The loupe: four times the screen, beside the pointer.
        if (ImGui::IsItemHovered() && mu >= 0 && mv >= 0 && mu < fw && mv < fh) {
            const float r = px(70), half = r / (4 * s);
            const ImVec2 at_(io.MousePos.x + px(24), io.MousePos.y + px(24));
            const ImVec2 a(at_.x, at_.y), b(at_.x + 2 * r, at_.y + 2 * r);
            const ImVec2 uv0((float)(mu - half) / fw, (float)(mv - half) / fh);
            const ImVec2 uv1((float)(mu + half) / fw, (float)(mv + half) / fh);
            ImDrawList* fg = ImGui::GetForegroundDrawList();
            fg->AddRectFilled(ImVec2(a.x - 2, a.y - 2), ImVec2(b.x + 2, b.y + 2), IM_COL32(0, 0, 0, 255));
            fg->AddImage(g.tex.id(), a, b, uv0, uv1);
            const ImVec2 c(a.x + r, a.y + r);
            fg->AddLine(ImVec2(c.x - px(10), c.y), ImVec2(c.x + px(10), c.y), IM_COL32(255, 60, 60, 255), px(1));
            fg->AddLine(ImVec2(c.x, c.y - px(10)), ImVec2(c.x, c.y + px(10)), IM_COL32(255, 60, 60, 255), px(1));
        }
        if (mark && ImGui::IsWindowHovered() && ImGui::IsKeyPressed(ImGuiKey_Delete)) {
            g.gp.marks[g.point].erase(img);
            if (g.gp.marks[g.point].empty()) g.gp.marks.erase(g.point);
            g.stale = true;
            geo_save();
        }
    } else if (g.cam >= 0 && !g.pic_busy && g.tex_cam != g.cam && g.pic_want == g.cam) {
        ui::TextColored(ImVec4(1, 0.45f, 0.4f, 1), gmsg::photo_failed);
    }
    ImGui::EndChild();

    const float w = px(120);
    ImGui::BeginDisabled(at <= 0);
    if (ui::Button(gmsg::prev_photo, ImVec2(w, 0)) || (at > 0 && ImGui::IsKeyPressed(ImGuiKey_PageUp)))
        g.cam = rows[(size_t)at - 1].cam;
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(at < 0 || at + 1 >= (int)rows.size());
    if (ui::Button(gmsg::next_photo, ImVec2(w, 0)) ||
        (at >= 0 && at + 1 < (int)rows.size() && ImGui::IsKeyPressed(ImGuiKey_PageDown)))
        g.cam = rows[(size_t)at + 1].cam;
    ImGui::EndDisabled();
    ImGui::SameLine();
    ui::Checkbox(gmsg::only_visible, &g.only_visible);
    ImGui::SameLine();
    ui::TextDisabled(gmsg::marks_help);
}

// ---------------------------------------------------------------------------
// In the viewport
// ---------------------------------------------------------------------------

void SurveySession::draw_control_overlay(const ViewportOverlay& v, const ViewProjection& camera) {
    if (!_geo || !_geo->ds || !v.dl) return;
    const GeoState& g = *_geo;
    const Sim3 to_shared = _e.saved_to_shared();
    v.dl->PushClipRect(ImVec2(v.x, v.y), ImVec2(v.x + v.w, v.y + v.h), true);
    for (int i = 0; i < (int)g.gp.points.size(); i++) {
        const auto& p = g.gp.points[(size_t)i];
        double m[3], w[3];
        if (!geo_model_point(p.name, m)) continue;
        to_shared.apply(m, w);
        const float q[3] = {(float)w[0], (float)w[1], (float)w[2]};
        float x, y, depth;
        if (!camera.project(q, x, y, depth) || !(depth > 0) || !std::isfinite(x) || !std::isfinite(y)) continue;
        const ImVec2 at(v.x + x, v.y + y);
        const bool sel = i == g.sel;
        const ImU32 col = role_colour(p.role);
        const float r = px(sel ? 7 : 5);
        const ImVec2 d[4] = {{at.x, at.y - r}, {at.x + r, at.y}, {at.x, at.y + r}, {at.x - r, at.y}};
        v.dl->AddQuadFilled(d[0], d[1], d[2], d[3], col);
        v.dl->AddQuad(d[0], d[1], d[2], d[3], IM_COL32(0, 0, 0, 230), px(1));
        outlined_text(v.dl, ImVec2(at.x + px(8), at.y - ImGui::GetFontSize()), col, p.name);
    }
    v.dl->PopClipRect();
}

}  // namespace gui
