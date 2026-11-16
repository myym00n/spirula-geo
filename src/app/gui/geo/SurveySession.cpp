// SurveySession.cpp -- the Survey screen's frame: which page is open, the
// tool row, the side panel, and hooking the session over the editor.

#include "app/gui/geo/SurveySession.h"

#include "app/gui/Layout.h"
#include "app/gui/Ui.h"
#include "app/gui/edit/EditSession.h"
#include "app/gui/geo/GeoState.h"
#include "i18n/catalog/Geo.h"

#include "imgui.h"

namespace gmsg = spirula::i18n::msg::geo;
namespace geo = spirula::geo;

namespace gui {

SurveySession::~SurveySession() { detach(); }

void SurveySession::attach() {
    if (!_e.active()) return;
    if (_doc_seen != _e._doc.get()) {
        geo_close();
        _doc_seen = _e._doc.get();
    }
    _e.set_overlay_tool(this);
    if (!_attached) {
        _e._tool.set_id(ToolId::Navigate);
        _attached = true;
    }
    geo_open();
}

void SurveySession::detach() {
    if (_attached) _e.set_overlay_tool(nullptr);
    _attached = false;
    geo_close();
    _doc_seen = nullptr;
}

bool SurveySession::ready() const { return _attached && _geo && _e.active(); }

void SurveySession::set_tab(Tab t) {
    if (_geo) _geo->pts.clear();
    _tab = t;
}

void SurveySession::set_tool(Tool t) {
    if (!_geo) return;
    GeoState& g = *_geo;
    if (_tool == Tool::Move && t != Tool::Move) move_cancel();
    g.pts.clear();
    g.measure_done = false;
    g.click = GeoState::Click::None;
    _tool = t;
    // Selecting and cleaning are Spirula's own tools, shown in its panel.
    if (t == Tool::Select) _e._tool.set_id(ToolId::Box);
    else if (t == Tool::Clean) _e._tool.set_id(ToolId::Brush);
    else _e._tool.set_id(ToolId::Navigate);
}

void SurveySession::poll() {
    if (!_e.active()) {
        if (_attached) detach();
        return;
    }
    if (!_attached || _doc_seen != _e._doc.get()) attach();
    if (!_geo) return;
    GeoState& g = *_geo;
    if (!g.loading && g.loader.joinable()) {
        g.loader.join();
        g.stale = true;
    }
    geo_solve();
    if (!g.exporting && g.export_worker.joinable()) {
        g.export_worker.join();
        g.status = g.export_result;
        g.status_err = g.export_err;
    }
    // A placed model saved without its dataset keeps its offset beside it.
    const bool saving = _e._save_busy.load() || _e._save_worker.joinable();
    if (_was_saving && !saving && g.gp.placed && !_e._save_path.empty() && !_e._doc->dataset()) {
        geo::GeoProject side = g.gp;
        side.model_frame = !g.md.directory.empty();
        side.file_from_dataset = geo_model_from_dataset();
        std::string err;
        geo::save_geo(_e._save_path + ".geo.json", side, err);
    }
    _was_saving = saving;
}

void SurveySession::draw_tab_bar() {
    struct Item {
        Tab tab;
        const spirula::i18n::Msg* label;
    };
    static const Item items[] = {
        {Tab::Control, &gmsg::t_control}, {Tab::Photos, &gmsg::t_photos}, {Tab::Plan, &gmsg::t_plan},
        {Tab::Model, &gmsg::t_model},     {Tab::Basemap, &gmsg::t_basemap}, {Tab::Report, &gmsg::t_report},
    };
    if (!ImGui::BeginTabBar("##surveytabs")) return;
    for (const Item& it : items) {
        // A tab chosen from code (a double click in the table) is forced once.
        const bool force = _tab_forced && _tab == it.tab;
        if (ui::BeginTabItem(*it.label, force ? ImGuiTabItemFlags_SetSelected : 0)) {
            if (!_tab_forced && _tab != it.tab) set_tab(it.tab);
            ImGui::EndTabItem();
        }
    }
    _tab_forced = false;
    ImGui::EndTabBar();
}

void SurveySession::draw_tool_row() {
    if (_tab != Tab::Model && _tab != Tab::Plan) return;
    struct Item {
        Tool tool;
        const spirula::i18n::Msg* label;
        bool ready;
    };
    static const Item items[] = {
        {Tool::View, &gmsg::tool_view, true},       {Tool::Measure, &gmsg::tool_measure, true},
        {Tool::Point, &gmsg::tool_point, true},     {Tool::Trunk, &gmsg::tool_trunk, false},
        {Tool::Move, &gmsg::tool_move, true},       {Tool::Contour, &gmsg::tool_contour, true},
        {Tool::Surface, &gmsg::tool_surface, false}, {Tool::Select, &gmsg::tool_select, true},
        {Tool::Clean, &gmsg::tool_clean, true},
    };
    bool first = true;
    for (const Item& it : items) {
        if (!first) ImGui::SameLine();
        first = false;
        const bool on = _tool == it.tool;
        if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        ImGui::BeginDisabled(!it.ready || !_geo);
        if (ui::Button(*it.label)) set_tool(it.tool);
        ImGui::EndDisabled();
        if (!it.ready) ui::help_on_hover_disabled(gmsg::soon);
        if (on) ImGui::PopStyleColor();
    }
}

void SurveySession::draw_model_panel(float full) {
    GeoState& g = *_geo;
    switch (_tool) {
        case Tool::View:
            ui::TextDisabledWrapped(gmsg::view_help);
            if (g.readout_ok) {
                char line[160];
                std::snprintf(line, sizeof line, "X %.3f\nY %.3f\nH %.3f", g.readout[0], g.readout[1], g.readout[2]);
                ui::TextRaw(line);
            }
            break;
        case Tool::Measure: draw_measure_panel(full); break;
        case Tool::Point:
            ui::TextDisabledWrapped(gmsg::point_help);
            draw_code_picker(full);
            break;
        case Tool::Contour:
            ui::TextDisabledWrapped(gmsg::draw_help);
            draw_code_picker(full);
            break;
        case Tool::Move: draw_move_panel(full); break;
        case Tool::Select:
        case Tool::Clean:
            ui::TextDisabledWrapped(_tool == Tool::Select ? gmsg::select_help : gmsg::clean_help);
            ImGui::Separator();
            // Spirula's editor panel as it is: tools, actions, saving.
            ImGui::BeginChild("##spirulaedit", ImVec2(0, 0));
            _e.draw_panel();
            ImGui::EndChild();
            break;
        default: ui::TextDisabledWrapped(gmsg::soon); break;
    }
}

void SurveySession::draw_side_panel(float full) {
    _frame = ImGui::GetFrameCount();
    if (!ready()) return;
    if (_tool != Tool::Select && _tool != Tool::Clean) geo_keys();
    switch (_tab) {
        case Tab::Control: draw_control_panel(full); break;
        case Tab::Photos: draw_marks_panel(full); break;
        case Tab::Plan:
            if (_tool == Tool::Select || _tool == Tool::Clean || _tool == Tool::Move || _tool == Tool::Measure)
                draw_model_panel(full);
            else draw_plan_panel(full);
            break;
        case Tab::Model: draw_model_panel(full); break;
        case Tab::Basemap: ui::TextDisabledWrapped(gmsg::soon); break;
        case Tab::Report: draw_report_panel(full); break;
    }
    if (_tool != Tool::Select && _tool != Tool::Clean) {
        ImGui::Spacing();
        if (ui::Button(gmsg::save_model, ImVec2(full, 0))) {
            // Spirula's save asks before it overwrites; its panel draws that.
            set_tool(Tool::Select);
            _tab = Tab::Model;
            _tab_forced = true;
            _e._ask_overwrite = true;
        }
        ui::help_on_hover(gmsg::save_model_help);
        draw_status(full);
    }
}

void SurveySession::draw_viewport_overlay(const ViewportOverlay& v) {
    if (!_attached || !_geo || !v.dl || ImGui::GetFrameCount() - _frame > 1) return;
    XformFrame f;
    if (!_e.xform_frame(f)) return;
    v.dl->PushClipRect(ImVec2(v.x, v.y), ImVec2(v.x + v.w, v.y + v.h), true);
    draw_control_overlay(v, f.cam);
    draw_vector_overlay(v, f.cam);
    v.dl->PopClipRect();
}

}  // namespace gui
