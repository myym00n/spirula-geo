// SurveyScreen.cpp -- spirula-geo's Survey screen and its menu, as GuiApp
// members so the app's own model loading, log and layout are reused. The
// hooks into GuiApp are listed in docs/fork.md.

#include "app/gui/GuiApp.h"

#include "app/gui/FileDialog.h"
#include "app/gui/Layout.h"
#include "app/gui/Ui.h"
#include "app/gui/geo/SurveySession.h"
#include "i18n/catalog/Geo.h"
#include "i18n/catalog/Gui.h"

#include "imgui.h"

#include <algorithm>
#include <filesystem>

namespace gmsg = spirula::i18n::msg::geo;
namespace msg = spirula::i18n::msg::gui;

namespace gui {

namespace {

// The picker the Survey menu and screen open models with; drawn every frame
// from the menu bar, so it answers from any screen.
FileDialog& open_dialog() {
    static FileDialog d;
    return d;
}

const std::vector<std::string> kSurveyOpenable = {".ply", ".obj", ".gltf", ".glb", ".stl",
                                                   ".json", ".bin", ".txt", ".xml"};

}  // namespace

void GuiApp::survey_detach() {
    if (_survey) _survey->detach();
}

void GuiApp::draw_survey_menu() {
    FileDialog& dlg = open_dialog();
    if (dlg.draw() && !dlg.result().empty()) {
        const std::string path = dlg.result();
        _edit_after_open = true;
        request_open_splat(path);
        if (_compare.count() > 0) _screen = Screen::Survey;
    }
    if (!ui::BeginMenu(gmsg::menu_survey)) return;
    if (ui::MenuItem(gmsg::menu_open, nullptr, false, !native_work_busy()))
        dlg.open(gmsg::menu_open.get(), FileDialog::Mode::FileOrFolder, kSurveyOpenable);
    ui::help_on_hover(gmsg::menu_help);
    if (ui::MenuItem(gmsg::menu_current, nullptr, false, _compare.count() > 0)) _screen = Screen::Survey;
    ImGui::EndMenu();
}

void GuiApp::draw_survey() {
    if (!_survey) _survey = std::make_shared<SurveySession>(_compare.edit());
    SurveySession& s = *_survey;
    _compare.set_render_allowed(false);

    if (ui::Button(msg::back_home))
        _compare.confirm_discard_edits([this] {
            survey_detach();
            request_go_home();
        });
    ImGui::SameLine();
    if (ui::Button(gmsg::open_model) && !native_work_busy())
        open_dialog().open(gmsg::open_model.get(), FileDialog::Mode::FileOrFolder, kSurveyOpenable);
    if (_compare.count() > 0) {
        ImGui::SameLine();
        ui::TextDisabledRaw(std::filesystem::u8path(_compare.edit().active() ? _compare.edit().doc()->source_path()
                                                                               : std::string())
                                .filename()
                                .u8string());
    }

    const float log_h = log_height(ImGui::GetContentRegionAvail().y);
    if (_compare.count() == 0) {
        ImGui::BeginChild("##survey", ImVec2(0, body_height(log_h)));
        ui::TextDisabledWrapped(gmsg::no_model);
        ImGui::EndChild();
        draw_log_panel(log_h);
        return;
    }
    // The tools work through the editor; it opens on the first model.
    if (_compare.editing() < 0 && !_compare.edit_busy()) _compare.begin_edit(0);

    s.poll();
    s.draw_tab_bar();
    s.draw_tool_row();

    ImGui::BeginChild("##survey", ImVec2(0, body_height(log_h)));
    const float avail = ImGui::GetContentRegionAvail().x;
    const float h = ImGui::GetContentRegionAvail().y;
    const float gap = px(8.0f);
    float w = std::clamp(_edit_panel_w * ui_scale() * 1.15f, px(260.0f), std::max(px(260.0f), avail * 0.6f));
    float left = std::max(px(120.0f), avail - w - gap);
    ImGui::BeginChild("##surveyview", ImVec2(left, 0));
    if (!_survey->ready()) ui::TextDisabled(gmsg::loading_model);
    if (s.shows_photo()) s.draw_photo_page();
    else _compare.draw(0.0f);
    ImGui::EndChild();
    ImGui::SameLine();
    // The panel is on the right: the splitter moves the view's edge.
    if (splitter_v("##surveysplit", &left, avail * 0.4f, avail - px(260.0f) - gap, h)) {
        w = avail - left - gap;
        _edit_panel_w = w / ui_scale() / 1.15f;
        _layout_dirty = true;
    }
    ImGui::SameLine();
    ImGui::BeginChild("##surveyside", ImVec2(0, 0), ImGuiChildFlags_Borders);
    s.draw_side_panel(ImGui::GetContentRegionAvail().x);
    ImGui::EndChild();
    ImGui::EndChild();
    s.draw_modals();
    draw_log_panel(log_h);
}

}  // namespace gui
