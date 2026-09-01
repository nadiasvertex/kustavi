#include "pass/junk.h"

#include <llama.h>
#include <mtmd-helper.h>
#include <mtmd.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace kustavi::image {

namespace {

/// The vision model is asked to answer with one lowercase word.
constexpr std::string_view k_question =
    "\n\nQuestion: Is this image an ordinary photograph, or is it a screenshot, "
    "a photo of a screen, a scanned document, a meme, or another "
    "computer-generated graphic? If it is an ordinary photo, answer \"photo\". "
    "Otherwise answer with the single best category word.\n\nAnswer:";

constexpr int k_max_answer_tokens = 24;
constexpr int k_context_tokens = 2048;

/// llama_backend_init is process-global; run it once.
void llama_backend_init_once() {
  static std::once_flag flag;
  std::call_once(flag, []() -> void {
    ggml_backend_load_all();
    llama_backend_init();
  });
}

/// Maps a raw model answer to a junk reason bucket. Empty result ⇒ keep.
auto reason_from_answer(std::string_view answer) -> std::string {
  std::string lower;
  lower.reserve(answer.size());
  for (const char ch : answer) {
    lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
  }

  const auto has = [&](std::string_view needle) -> bool {
    return lower.find(needle) != std::string::npos;
  };

  if (has("photo") || has("picture") || has("photograph")) {
    return {};
  }
  if (has("screenshot") || has("screen shot") || has("screen")) {
    return "screenshot";
  }
  if (has("meme")) {
    return "meme";
  }
  if (has("scan") || has("document") || has("receipt") || has("paper")) {
    return "scan";
  }
  if (has("diagram") || has("chart") || has("graph") || has("plot")) {
    return "diagram";
  }
  if (has("drawing") || has("illustration") || has("sketch") || has("art") ||
      has("cartoon") || has("render")) {
    return "drawing";
  }
  if (has("poster") || has("flyer") || has("advert") || has("banner")) {
    return "poster";
  }
  if (has("text")) {
    return "document";
  }
  return "other";
}

/// Probability of the argmax token from a logits vector (softmax at the peak).
auto peak_probability(const float *logits, int n) -> double {
  if (logits == nullptr || n <= 0) {
    return 0.0;
  }
  const float max = *std::max_element(logits, logits + n);
  double sum = 0.0;
  for (int i = 0; i < n; ++i) {
    sum += std::exp(static_cast<double>(logits[i] - max));
  }
  return sum > 0.0 ? 1.0 / sum : 0.0;
}

} // namespace

struct junk_classifier::impl {
  llama_model *model = nullptr;
  llama_context *lctx = nullptr;
  mtmd_context *mctx = nullptr;
  llama_sampler *sampler = nullptr;
  const llama_vocab *vocab = nullptr;

  impl() = default;
  impl(const impl &) = delete;
  auto operator=(const impl &) -> impl & = delete;

  ~impl() {
    if (sampler != nullptr) {
      llama_sampler_free(sampler);
    }
    if (mctx != nullptr) {
      mtmd_free(mctx);
    }
    if (lctx != nullptr) {
      llama_free(lctx);
    }
    if (model != nullptr) {
      llama_model_free(model);
    }
  }
};

junk_classifier::junk_classifier() : impl_(std::make_unique<impl>()) {}
junk_classifier::junk_classifier(junk_classifier &&) noexcept = default;
auto junk_classifier::operator=(junk_classifier &&) noexcept
    -> junk_classifier & = default;
junk_classifier::~junk_classifier() = default;

auto junk_classifier::load(const std::filesystem::path &text_model_gguf,
                           const std::filesystem::path &mmproj_gguf)
    -> std::expected<junk_classifier, std::string> {
  llama_backend_init_once();

  junk_classifier self;
  impl &state = *self.impl_;

  llama_model_params model_params = llama_model_default_params();
  model_params.n_gpu_layers = 999; // offload everything the backend accepts
  state.model = llama_model_load_from_file(text_model_gguf.string().c_str(),
                                           model_params);
  if (state.model == nullptr) {
    return std::unexpected("failed to load text model: " +
                           text_model_gguf.string());
  }
  state.vocab = llama_model_get_vocab(state.model);

  const int threads =
      std::max(1, static_cast<int>(std::thread::hardware_concurrency()));

  llama_context_params ctx_params = llama_context_default_params();
  ctx_params.n_ctx = k_context_tokens;
  ctx_params.n_batch = k_context_tokens;
  ctx_params.n_ubatch = k_context_tokens;
  ctx_params.n_threads = threads;
  ctx_params.n_threads_batch = threads;
  ctx_params.no_perf = true;
  state.lctx = llama_init_from_model(state.model, ctx_params);
  if (state.lctx == nullptr) {
    return std::unexpected("failed to create llama context");
  }

  mtmd_context_params mtmd_params = mtmd_context_params_default();
  mtmd_params.use_gpu = true;
  mtmd_params.print_timings = false;
  mtmd_params.n_threads = threads;
  mtmd_params.media_marker = mtmd_default_marker();
  state.mctx = mtmd_init_from_file(mmproj_gguf.string().c_str(), state.model,
                                   mtmd_params);
  if (state.mctx == nullptr) {
    return std::unexpected("failed to load multimodal projector: " +
                           mmproj_gguf.string());
  }
  if (!mtmd_support_vision(state.mctx)) {
    return std::unexpected("multimodal projector has no vision support");
  }

  state.sampler = llama_sampler_init_greedy();
  return self;
}

auto junk_classifier::classify(const std::filesystem::path &image_path)
    -> junk_result {
  impl &state = *impl_;

  // Independent per image: wipe the KV cache and sampler history.
  llama_memory_clear(llama_get_memory(state.lctx), true);
  llama_sampler_reset(state.sampler);

  const mtmd_helper_bitmap_wrapper wrapper = mtmd_helper_bitmap_init_from_file(
      state.mctx, image_path.string().c_str(), /*placeholder=*/false,
      mtmd_helper_init_opt_default());
  if (wrapper.bitmap == nullptr) {
    return {.valid = false};
  }
  const mtmd::bitmap_ptr bitmap(wrapper.bitmap);

  const std::string prompt =
      std::string(mtmd_default_marker()) + std::string(k_question);
  const mtmd_input_text text{
      .text = prompt.c_str(),
      .text_len = prompt.size(),
      .add_special = true,
      .parse_special = true,
  };

  const mtmd::input_chunks_ptr chunks(mtmd_input_chunks_init());
  const mtmd_bitmap *bitmaps[] = {bitmap.get()};
  if (mtmd_tokenize(state.mctx, chunks.get(), &text, bitmaps, 1) != 0) {
    return {.valid = false};
  }

  llama_pos n_past = 0;
  if (mtmd_helper_eval_chunks(state.mctx, state.lctx, chunks.get(), n_past,
                              /*seq_id=*/0, k_context_tokens,
                              /*logits_last=*/true, &n_past) != 0) {
    return {.valid = false};
  }

  const double confidence = peak_probability(
      llama_get_logits_ith(state.lctx, -1), llama_vocab_n_tokens(state.vocab));

  std::string answer;
  for (int i = 0; i < k_max_answer_tokens; ++i) {
    const llama_token token = llama_sampler_sample(state.sampler, state.lctx, -1);
    if (llama_vocab_is_eog(state.vocab, token)) {
      break;
    }
    std::array<char, 256> piece{};
    const int32_t n = llama_token_to_piece(state.vocab, token, piece.data(),
                                           static_cast<int32_t>(piece.size()),
                                           /*lstrip=*/0, /*special=*/false);
    if (n > 0) {
      answer.append(piece.data(), static_cast<std::size_t>(n));
    }
    llama_token next = token;
    if (llama_decode(state.lctx, llama_batch_get_one(&next, 1)) != 0) {
      break;
    }
  }

  std::string reason = reason_from_answer(answer);
  if (reason.empty()) {
    return {.is_junk = false, .confidence = confidence, .valid = true};
  }
  return {.is_junk = true,
          .reason = std::move(reason),
          .confidence = confidence,
          .valid = true};
}

} // namespace kustavi::image
