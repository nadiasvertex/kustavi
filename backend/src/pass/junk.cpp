#include "pass/junk.h"

#include <llama.h>
#include <mtmd-helper.h>
#include <mtmd.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace kustavi::image {

namespace {

constexpr int k_max_answer_tokens = 8;
// Qwen2.5-VL turns a 768px image into ~700 vision tokens; leave generous room.
constexpr int k_context_tokens = 4096;

// An image is only flagged as junk when the model is at least this confident
// that it is *not* an ordinary photograph. Borderline cases are kept.
constexpr double k_junk_threshold = 0.70;

/// Wraps a user turn in Qwen2.5-VL's ChatML template. The media marker is
/// expanded to the model's image-token sequence by `mtmd_tokenize`.
auto build_prompt(std::string_view user_text) -> std::string {
  std::string out;
  out += "<|im_start|>system\nYou are a precise image classifier. Reply with "
         "only the single word requested.<|im_end|>\n<|im_start|>user\n";
  out += mtmd_default_marker();
  out += user_text;
  out += "<|im_end|>\n<|im_start|>assistant\n";
  return out;
}

// Step 1: a calibrated yes/no. "no" only for the enumerated junk categories so
// the model does not reject unusual-but-real photos (macro, night, abstract).
constexpr std::string_view k_photo_question =
    "Is this a normal photograph taken with a camera of a real scene, person, "
    "animal, object, or place? Answer \"yes\" or \"no\". Answer \"no\" only if "
    "it is clearly a screenshot, a photo of a screen or monitor, a scanned "
    "document or receipt, a meme, or a computer-generated graphic.";

// Step 2: reached only after step 1 says "no"; picks the reason bucket.
constexpr std::string_view k_category_question =
    "This image is not an ordinary photo. Which single word best describes it: "
    "screenshot, scan, document, meme, diagram, drawing, or poster?";

/// llama_backend_init is process-global; run it once.
void llama_backend_init_once() {
  static std::once_flag flag;
  std::call_once(flag, []() -> void {
    ggml_backend_load_all();
    llama_backend_init();
  });
}

/// Token ids for each spelling of a short word that encodes to a single token
/// (e.g. "yes", " yes", "Yes"). Used to sum probability mass across variants.
auto single_token_ids(const llama_vocab *vocab,
                      std::initializer_list<const char *> words)
    -> std::vector<llama_token> {
  std::vector<llama_token> ids;
  for (const char *word : words) {
    std::array<llama_token, 8> buf{};
    const int32_t n = llama_tokenize(
        vocab, word, static_cast<int32_t>(std::strlen(word)), buf.data(),
        static_cast<int32_t>(buf.size()), /*add_special=*/false,
        /*parse_special=*/false);
    if (n == 1) {
      ids.push_back(buf[0]);
    }
  }
  std::sort(ids.begin(), ids.end());
  ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
  return ids;
}

/// Summed softmax probability of a set of token ids at the given logits.
auto class_probability(const float *logits, int n,
                       const std::vector<llama_token> &ids) -> double {
  if (logits == nullptr || n <= 0 || ids.empty()) {
    return 0.0;
  }
  const float max = *std::max_element(logits, logits + n);
  double partition = 0.0;
  for (int i = 0; i < n; ++i) {
    partition += std::exp(static_cast<double>(logits[i] - max));
  }
  if (partition <= 0.0) {
    return 0.0;
  }
  double mass = 0.0;
  for (const llama_token id : ids) {
    if (id >= 0 && id < n) {
      mass += std::exp(static_cast<double>(logits[id] - max));
    }
  }
  return mass / partition;
}

/// Maps the step-2 answer to a junk reason bucket. Never returns empty: step 2
/// runs only once the image is already judged non-photographic.
auto category_from_answer(std::string_view answer) -> std::string {
  std::string lower;
  lower.reserve(answer.size());
  for (const char ch : answer) {
    lower.push_back(
        static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
  }
  const auto has = [&](std::string_view needle) -> bool {
    return lower.find(needle) != std::string::npos;
  };

  if (has("screenshot") || has("screen shot") || has("screen capture") ||
      has("monitor") || has("display")) {
    return "screenshot";
  }
  if (has("meme")) {
    return "meme";
  }
  if (has("receipt")) {
    return "scan";
  }
  if (has("scan")) {
    return "scan";
  }
  if (has("document") || has("paper") || has("page of text") ||
      has("text document")) {
    return "document";
  }
  if (has("diagram") || has("chart") || has("graph") || has("plot") ||
      has("infographic")) {
    return "diagram";
  }
  if (has("drawing") || has("illustration") || has("sketch") ||
      has("painting") || has("cartoon") || has("clip art") || has("clipart") ||
      has("render") || has("digital art") || has("artwork") || has("anime")) {
    return "drawing";
  }
  if (has("poster") || has("flyer") || has("advertisement") || has("advert") ||
      has("banner")) {
    return "poster";
  }
  return "other";
}

} // namespace

struct junk_classifier::impl {
  llama_model *model = nullptr;
  llama_context *lctx = nullptr;
  mtmd_context *mctx = nullptr;
  llama_sampler *sampler = nullptr;
  const llama_vocab *vocab = nullptr;
  std::vector<llama_token> yes_ids;
  std::vector<llama_token> no_ids;

  impl() = default;
  impl(const impl &) = delete;
  auto operator=(const impl &) -> impl & = delete;

  /// Loads an image and evaluates `question` about it, leaving the context
  /// positioned so the next token is the model's answer. False on any
  /// tokenize/eval failure.
  auto eval_question(const std::filesystem::path &image_path,
                     std::string_view question) -> bool;

  /// Greedy-decodes up to `k_max_answer_tokens` tokens from the current
  /// position.
  auto decode_answer() -> std::string;

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
  state.yes_ids = single_token_ids(
      state.vocab, {"yes", " yes", "Yes", " Yes", "YES", " YES"});
  state.no_ids =
      single_token_ids(state.vocab, {"no", " no", "No", " No", "NO", " NO"});

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

auto junk_classifier::impl::eval_question(
    const std::filesystem::path &image_path, std::string_view question)
    -> bool {
  llama_memory_clear(llama_get_memory(lctx), true);
  llama_sampler_reset(sampler);

  const mtmd_helper_bitmap_wrapper wrapper = mtmd_helper_bitmap_init_from_file(
      mctx, image_path.string().c_str(), /*placeholder=*/false,
      mtmd_helper_init_opt_default());
  if (wrapper.bitmap == nullptr) {
    return false;
  }
  const mtmd::bitmap_ptr bitmap(wrapper.bitmap);

  const std::string prompt = build_prompt(question);
  const mtmd_input_text text{
      .text = prompt.c_str(),
      .text_len = prompt.size(),
      .add_special = true,
      .parse_special = true,
  };

  const mtmd::input_chunks_ptr chunks(mtmd_input_chunks_init());
  const mtmd_bitmap *bitmaps[] = {bitmap.get()};
  if (mtmd_tokenize(mctx, chunks.get(), &text, bitmaps, 1) != 0) {
    return false;
  }

  llama_pos n_past = 0;
  return mtmd_helper_eval_chunks(mctx, lctx, chunks.get(), n_past,
                                 /*seq_id=*/0, k_context_tokens,
                                 /*logits_last=*/true, &n_past) == 0;
}

auto junk_classifier::impl::decode_answer() -> std::string {
  std::string answer;
  for (int i = 0; i < k_max_answer_tokens; ++i) {
    const llama_token token = llama_sampler_sample(sampler, lctx, -1);
    if (llama_vocab_is_eog(vocab, token)) {
      break;
    }
    std::array<char, 256> piece{};
    const int32_t n = llama_token_to_piece(vocab, token, piece.data(),
                                           static_cast<int32_t>(piece.size()),
                                           /*lstrip=*/0, /*special=*/false);
    if (n > 0) {
      answer.append(piece.data(), static_cast<std::size_t>(n));
    }
    llama_token next = token;
    if (llama_decode(lctx, llama_batch_get_one(&next, 1)) != 0) {
      break;
    }
  }
  return answer;
}

auto junk_classifier::classify(const std::filesystem::path &image_path)
    -> junk_result {
  impl &state = *impl_;

  // Step 1: calibrated "is this a photograph?" from the yes/no logits.
  if (!state.eval_question(image_path, k_photo_question)) {
    return {.valid = false};
  }

  const int n_vocab = llama_vocab_n_tokens(state.vocab);
  const float *logits = llama_get_logits_ith(state.lctx, -1);
  const double p_yes = class_probability(logits, n_vocab, state.yes_ids);
  const double p_no = class_probability(logits, n_vocab, state.no_ids);

  double p_junk = 0.0;
  if (p_yes + p_no > 1e-6) {
    p_junk = p_no / (p_yes + p_no);
  } else {
    // Vocab has no single-token yes/no (unexpected): fall back to the decoded
    // word, treating an explicit "no" as a low-confidence junk vote.
    const std::string word = state.decode_answer();
    std::string lower;
    for (const char ch : word) {
      lower.push_back(
          static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    p_junk = lower.find("no") != std::string::npos ? 0.75 : 0.0;
  }

  if (p_junk < k_junk_threshold) {
    return {.is_junk = false, .confidence = p_junk, .valid = true};
  }

  // Step 2: it is not a photo — ask which kind so we can label the reason.
  std::string reason = "other";
  if (state.eval_question(image_path, k_category_question)) {
    reason = category_from_answer(state.decode_answer());
  }
  return {.is_junk = true,
          .reason = std::move(reason),
          .confidence = p_junk,
          .valid = true};
}

} // namespace kustavi::image
