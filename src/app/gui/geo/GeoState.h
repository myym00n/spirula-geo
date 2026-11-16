// GeoState.h -- what the Reference, Measure, Draw and Export tabs keep between
// frames. Private to EditGeo.cpp and EditMeasure.cpp.
#pragma once

#include "app/gui/FileDialog.h"
#include "app/gui/Picture.h"
#include "app/gui/PreviewTexture.h"
#include "data/DatasetParser.h"
#include "data/GeoCrs.h"
#include "data/GeoProject.h"
#include "data/GeoSolve.h"
#include "data/GeoVectors.h"
#include "data/MarkerScan.h"

#include "imgui.h"

#include <array>
#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace gui {

struct GeoState {
    spirula::MarkerDataset md;
    std::unique_ptr<ParsedDataset> own;
    const ParsedDataset* ds = nullptr;          // set by the loader, read once it is done
    std::vector<std::array<int, 2>> dims;       // per camera, file as displayed
    std::thread loader;
    std::atomic<bool> loading{false};
    std::string load_error;

    spirula::geo::GeoProject gp;
    std::string path;                           // geo.json, or the model's sidecar
    spirula::geo::Solution sol;
    bool stale = true;
    spirula::Sim3 solved_for;
    std::string status;
    bool status_err = false;
    int sel = -1;

    std::vector<spirula::geo::CrsEntry> user;
    bool user_read = false;
    spirula::geo::Crs crs;
    bool crs_ok = false;
    std::string filter, add_name, add_def, add_error;
    bool add_open = false, latlon = false;

    FileDialog dlg;
    enum class Ask { None, Catalog, Mooncloud, Export, Prj, Classifier, Model, Drawings } ask = Ask::None;

    std::string point;
    int cam = -1;
    bool only_visible = true;
    Picture pic;
    bool pic_ok = false;
    int pic_want = -1, tex_cam = -1;
    std::thread pic_worker;
    std::atomic<bool> pic_busy{false};
    PreviewTexture tex;
    float zoom = 1.0f;
    ImVec2 pan{0, 0};
    bool recentre = true;
    bool mark_drag = false;

    // ---- drawn on the model, x north / y east / h (GeoVectors.h) ----
    std::string vec_path;
    std::vector<spirula::geo::Feature> features;
    bool vec_survey = false;
    std::vector<std::vector<spirula::geo::Feature>> undo;
    std::vector<spirula::geo::ClassCode> classes;
    std::string class_path;
    int code = -1;                              // into classes
    std::string free_code = "1", next_name = "1", code_filter;
    int draw_kind = 0;                          // FeatureKind when no classifier
    int feat_sel = -1;

    // The figure under way: a measure in the Measure tab, a feature in Draw.
    int measure_kind = 1;                       // FeatureKind
    bool measure_done = false;                  // the next click starts a new one
    std::vector<std::array<double, 3>> pts;
    bool hover_ok = false, snapped = false;
    double hover[3] = {0, 0, 0};
    float last_mouse[2] = {-1, -1};
    // The viewport's own depth pick, one in flight at a time.
    bool pick_inflight = false;
    float pick_xy[2] = {0, 0};
    enum class Click { None, Act, Readout } click = Click::None;
    float press_xy[2] = {0, 0};
    bool readout_ok = false;
    double readout[3] = {0, 0, 0};

    // A vertex being moved: shown where it goes, kept on Done.
    int move_feat = -1, move_vtx = -1;
    std::array<double, 3> move_orig{};
    bool move_dirty = false, move_ask = false;
    int move_next_feat = -1, move_next_vtx = -1;

    int model_fmt = 0, vec_fmt = 0;
    bool with_control = true;
    spirula::geo::DxfOptions dxf;
    std::thread export_worker;
    std::atomic<bool> exporting{false};
    std::string export_result;
    bool export_err = false;

    void join() {
        if (loader.joinable()) loader.join();
        if (pic_worker.joinable()) pic_worker.join();
        if (export_worker.joinable()) export_worker.join();
    }
};

}  // namespace gui
