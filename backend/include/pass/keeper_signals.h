#pragma once

#include <expected>
#include <filesystem>
#include <memory>
#include <string>

namespace kustavi::image {

/**
 * Per-image signals the similar pass blends into its keeper "bestness" score,
 * on top of the sharpness term it already uses. All fields are neutral (no
 * effect on the score) when they cannot be measured.
 */
struct keeper_metrics {
  int face_count = 0;              //! faces detected (any size)
  double largest_face_focus = 0.0; //! Laplacian variance of the largest face
                                   //! ROI, normalized v/(v+100); 0 when no face
  double eyes_open_ratio = 1.0;    //! mean over usable faces of an eyes-open
                                   //! proxy in [0,1]; 1.0 == open / unknown
  double redeye_ratio = 0.0;  //! fraction of sampled eyes with red dominance
  double color_balance = 1.0; //! gray-world neutrality, 0 (strong cast) ..
                              //! 1 (neutral white balance)
  bool valid = false;         //! false when the image could not be decoded
};

/**
 * @brief Detects faces (YuNet ONNX) and, per face, derives an eyes-open proxy
 * and a red-eye score from the eye landmarks; also measures overall
 * white-balance neutrality (gray-world).
 *
 * OpenCV 5 dropped the Haar `CascadeClassifier`, so eyes-open is a
 * landmark-anchored contrast/darkness heuristic rather than a cascade hit: it
 * only pulls the ratio down when an eye ROI is clearly flat and skin-toned
 * (a likely blink), and is meant as a *relative* tie-breaker between
 * near-duplicate frames of the same subject.
 *
 * One decode per `analyze` call feeds every metric. Not thread-safe: a single
 * instance drives one `cv::FaceDetectorYN`; call `analyze` serially.
 */
class keeper_analyzer {
public:
  static auto load(const std::filesystem::path &yunet_onnx)
      -> std::expected<keeper_analyzer, std::string>;

  keeper_analyzer(keeper_analyzer &&) noexcept;
  auto operator=(keeper_analyzer &&) noexcept -> keeper_analyzer &;
  keeper_analyzer(const keeper_analyzer &) = delete;
  auto operator=(const keeper_analyzer &) -> keeper_analyzer & = delete;
  ~keeper_analyzer();

  [[nodiscard]] auto analyze(const std::filesystem::path &image_path)
      -> keeper_metrics;

private:
  keeper_analyzer();

  struct impl;
  std::unique_ptr<impl> impl_;
};

/**
 * Gray-world white-balance neutrality of the image at `image_path`, in [0, 1]
 * (1 == neutral). Decodes the file itself; returns 1.0 when it cannot be read
 * or judged. Exposed for tests; `keeper_analyzer::analyze` computes the same
 * value from its single decode.
 */
auto color_balance_score(const std::filesystem::path &image_path) -> double;

} // namespace kustavi::image
