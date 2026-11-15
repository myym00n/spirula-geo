// GeoSolve.h -- control points from photo marks to a survey frame: triangulate
// each marked point, fit one similarity on the control points, report the
// residuals. Frames: docs/geodesy.md.
#pragma once

#include "core/Similarity.h"
#include "data/DatasetParser.h"
#include "data/GeoProject.h"

#include <string>
#include <vector>

namespace spirula {
namespace geo {

struct PointResult {
    std::string name;
    Role role = Role::Off;
    int marks = 0;
    bool triangulated = false;
    double model[3] = {0, 0, 0};     // the frame `model_from_dataset` maps into
    double reproj_rms = 0.0, reproj_max = 0.0;   // px
    std::string worst_image;
    bool fitted = false;             // survey coordinates below are valid
    double x = 0, y = 0, h = 0;      // computed, survey frame
    double dx = 0, dy = 0, dh = 0;   // computed minus catalog
};

struct Solution {
    bool ok = false;
    std::string error;
    // model -> local survey frame: (east, north, up) minus `offset`.
    Sim3 model_to_local;
    double offset[3] = {0, 0, 0};    // east, north, up
    int n_control = 0, n_check = 0;
    double rms_plan_control = 0, rms_h_control = 0;
    double rms_plan_check = 0, rms_h_check = 0;
    std::vector<PointResult> points;
};

// Map from image file name (no directory) to camera index in `ds`.
std::vector<std::pair<std::string, int>> image_index(const ParsedDataset& ds);

// `model_from_dataset` takes the dataset's camera frame to the frame the fit
// is made in -- the editor's saved coordinates.
Solution solve(const GeoProject& p, const ParsedDataset& ds, const Sim3& model_from_dataset);

// Local (east, north, up) -> survey (x north, y east, h).
void local_to_survey(const Solution& s, const double local[3], double& x, double& y, double& h);
void survey_to_local(const Solution& s, double x, double y, double h, double local[3]);

// Where a model point lands on camera `cam`, in the image file's pixels.
bool project_to_image(const ParsedDataset& ds, int cam, const Sim3& model_from_dataset,
                      const double model[3], int file_w, int file_h, double& u, double& v);

}  // namespace geo
}  // namespace spirula
