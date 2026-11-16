// SurveySession.h -- the Survey screen's tools over an open editor session:
// control points and photo marks, measuring, drawing, export. It reaches the
// editor through EditSession's overlay hook and friendship only, so the
// editor itself stays upstream's (docs/fork.md). Frames: docs/geodesy.md.
#pragma once

#include "app/gui/ViewportInput.h"
#include "core/Similarity.h"
#include "data/GeoVectors.h"

#include <array>
#include <memory>
#include <string>
#include <vector>

struct ImVec2;

namespace gui {

class EditSession;
struct GeoState;
struct ViewProjection;

class SurveySession : public ViewportInteractor {
public:
    // The pages of the screen, mooncloud's order.
    enum class Tab { Control, Photos, Plan, Model, Basemap, Report };
    // The 3D tab's tools.
    enum class Tool { View, Measure, Point, Trunk, Move, Contour, Surface, Select, Clean };

    explicit SurveySession(EditSession& edit) : _e(edit) {}
    ~SurveySession() override;

    // Install over the editor (when it has a model) and take it back off.
    void attach();
    void detach();
    bool ready() const;

    Tab tab() const { return _tab; }
    void set_tab(Tab t);
    Tool tool() const { return _tool; }
    void set_tool(Tool t);
    // The 3D view is the 3D model tab's; the other tabs are pages of their own.
    bool wants_3d() const { return _tab == Tab::Model; }

    void begin_frame();
    void poll();
    void draw_tab_bar();
    void draw_tool_row();
    void draw_side_panel(float full);   // beside the 3D view
    void draw_page();                   // every other tab, full width
    void draw_modals();

    // ViewportInteractor, through EditSession's overlay hook.
    bool owns_left_button() const override;
    bool on_viewport_input(const ViewportInput& in) override;
    void draw_viewport_overlay(const ViewportOverlay& v) override;

private:
    // ---- control points (SurveyControl.cpp) ----
    void geo_open();
    void geo_close();
    void geo_save();
    void geo_solve();
    void geo_place();
    void geo_pick_crs(const std::string& name, const std::string& def);
    void geo_file_chosen(const std::string& path);
    spirula::Sim3 geo_model_from_dataset() const;
    bool geo_model_point(const std::string& name, double out[3]) const;
    void draw_crs(float full);
    void draw_photo_page();

    // ---- pages (SurveyPages.cpp) ----
    bool draw_dataset_state();
    void set_dataset_dir(const std::string& dir);
    void open_pickets(const std::string& path);
    void draw_pickets_modal();
    void draw_control_page();
    void plan_refresh();
    void draw_plan_page();
    void draw_control_overlay(const ViewportOverlay& v, const ViewProjection& camera);

    // ---- measuring, drawing, export (SurveyTools.cpp) ----
    bool picks() const;
    void ask_pick(const ViewportInput& in);
    void take_pick();
    void tool_click(const std::array<double, 3>& xyh);
    void move_take(int feat, int vtx);
    std::array<double, 3> geo_survey(const double saved[3]) const;
    void geo_saved(const std::array<double, 3>& xyh, double saved[3]) const;
    bool vertex_at(float x, float y, int& feat, int& vtx, double xyh[3]);
    void geo_keys();
    void geo_finish();
    void geo_commit(spirula::geo::FeatureKind kind, const std::vector<std::array<double, 3>>& pts);
    void geo_save_vectors();
    void geo_load_classifier(const std::string& path);
    void geo_load_classifier_setting();
    void move_confirm();
    void move_cancel();
    void geo_export_model(const std::string& path);
    void geo_export_drawings(const std::string& path);
    void draw_measure_panel(float full);
    void draw_code_picker(float full);
    void draw_feature_list(float full);
    void draw_local_system(float full);
    void draw_move_panel(float full);
    void draw_report_panel(float full);
    void draw_status(float full);
    void draw_model_panel(float full);
    void draw_vector_overlay(const ViewportOverlay& v, const ViewProjection& camera);

    EditSession& _e;
    std::shared_ptr<GeoState> _geo;
    const void* _doc_seen = nullptr;    // the document _geo was opened for
    Tab _tab = Tab::Control;
    Tool _tool = Tool::View;
    bool _attached = false;
    bool _tab_forced = false;            // set by code: select that tab once
    bool _was_saving = false;
    bool _reopen = false;
    int _frame = -10;                   // last frame the screen drew; inert when stale
};

}  // namespace gui
