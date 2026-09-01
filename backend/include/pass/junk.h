#pragma once

#include <expected>
#include <filesystem>
#include <memory>
#include <string>

namespace kustavi::image {

/** One image's junk classification from the vision model. */
struct junk_result {
  bool is_junk = false;
  std::string reason;     //! e.g. "screenshot", "meme", "scan"; empty if kept.
  double confidence = 0.0; //! 0..1, probability of the model's first answer token.
  bool valid = false;      //! false when the image could not be analyzed.
};

/**
 * @brief Loads a Moondream 2 GGUF + multimodal projector and classifies images
 * as ordinary photographs or junk (screenshots, scans, memes, graphics).
 *
 * Uses the optimal ggml backend for the platform (Metal on macOS). Greedy
 * decoding (temperature 0) for reproducibility. Not thread-safe: one instance
 * drives a single llama context; call `classify` serially.
 */
class junk_classifier {
public:
  static auto load(const std::filesystem::path &text_model_gguf,
                   const std::filesystem::path &mmproj_gguf)
      -> std::expected<junk_classifier, std::string>;

  junk_classifier(junk_classifier &&) noexcept;
  auto operator=(junk_classifier &&) noexcept -> junk_classifier &;
  junk_classifier(const junk_classifier &) = delete;
  auto operator=(const junk_classifier &) -> junk_classifier & = delete;
  ~junk_classifier();

  [[nodiscard]] auto classify(const std::filesystem::path &image_path)
      -> junk_result;

private:
  junk_classifier();

  struct impl;
  std::unique_ptr<impl> impl_;
};

} // namespace kustavi::image
