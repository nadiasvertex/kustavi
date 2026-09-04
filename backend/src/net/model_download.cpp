#include "net/model_download.h"

#include <curl/curl.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace kustavi::net {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// SHA-256 (FIPS 180-4). Self-contained: the build vendors no llama-common.
// ---------------------------------------------------------------------------

namespace {

// A byte-oriented hash primitive: raw indexing and pointer walks are the
// natural expression and stay local to this class.
// NOLINTBEGIN(cppcoreguidelines-pro-bounds-constant-array-index,cppcoreguidelines-pro-bounds-pointer-arithmetic)
class sha256 {
public:
  void update(const unsigned char *data, std::size_t len) {
    for (std::size_t i = 0; i < len; ++i) {
      buffer_[buffer_len_++] = data[i];
      if (buffer_len_ == 64) {
        transform(buffer_.data());
        bit_len_ += 512;
        buffer_len_ = 0;
      }
    }
  }

  auto hex() -> std::string {
    std::array<unsigned char, 32> digest{};
    finalize(digest.data());
    static constexpr std::string_view lut = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (const unsigned char byte : digest) {
      out.push_back(lut[byte >> 4]);
      out.push_back(lut[byte & 0x0f]);
    }
    return out;
  }

private:
  static auto rotr(std::uint32_t x, std::uint32_t n) -> std::uint32_t {
    return (x >> n) | (x << (32 - n));
  }

  void transform(const unsigned char *chunk) {
    static constexpr std::array<std::uint32_t, 64> k = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
        0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
        0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
        0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
        0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
        0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
        0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
        0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
        0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

    std::array<std::uint32_t, 64> w{};
    for (std::size_t i = 0; i < 16; ++i) {
      w[i] = (static_cast<std::uint32_t>(chunk[i * 4]) << 24) |
             (static_cast<std::uint32_t>(chunk[i * 4 + 1]) << 16) |
             (static_cast<std::uint32_t>(chunk[i * 4 + 2]) << 8) |
             (static_cast<std::uint32_t>(chunk[i * 4 + 3]));
    }
    for (std::size_t i = 16; i < 64; ++i) {
      const std::uint32_t s0 =
          rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
      const std::uint32_t s1 =
          rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    std::uint32_t a = state_[0];
    std::uint32_t b = state_[1];
    std::uint32_t c = state_[2];
    std::uint32_t d = state_[3];
    std::uint32_t e = state_[4];
    std::uint32_t f = state_[5];
    std::uint32_t g = state_[6];
    std::uint32_t h = state_[7];

    for (std::size_t i = 0; i < 64; ++i) {
      const std::uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
      const std::uint32_t ch = (e & f) ^ (~e & g);
      const std::uint32_t temp1 = h + s1 + ch + k[i] + w[i];
      const std::uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
      const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
      const std::uint32_t temp2 = s0 + maj;
      h = g;
      g = f;
      f = e;
      e = d + temp1;
      d = c;
      c = b;
      b = a;
      a = temp1 + temp2;
    }

    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
    state_[5] += f;
    state_[6] += g;
    state_[7] += h;
  }

  void finalize(unsigned char *out) {
    const std::uint64_t total_bits =
        bit_len_ + (std::uint64_t{buffer_len_} * 8);
    std::size_t i = buffer_len_;

    buffer_[i++] = 0x80;
    if (i > 56) {
      while (i < 64) {
        buffer_[i++] = 0x00;
      }
      transform(buffer_.data());
      i = 0;
    }
    while (i < 56) {
      buffer_[i++] = 0x00;
    }
    for (int shift = 56; shift >= 0; shift -= 8) {
      buffer_[i++] = static_cast<unsigned char>((total_bits >> shift) & 0xff);
    }
    transform(buffer_.data());

    for (std::size_t j = 0; j < 8; ++j) {
      out[j * 4] = static_cast<unsigned char>((state_[j] >> 24) & 0xff);
      out[j * 4 + 1] = static_cast<unsigned char>((state_[j] >> 16) & 0xff);
      out[j * 4 + 2] = static_cast<unsigned char>((state_[j] >> 8) & 0xff);
      out[j * 4 + 3] = static_cast<unsigned char>(state_[j] & 0xff);
    }
  }

  std::array<std::uint32_t, 8> state_ = {0x6a09e667, 0xbb67ae85, 0x3c6ef372,
                                         0xa54ff53a, 0x510e527f, 0x9b05688c,
                                         0x1f83d9ab, 0x5be0cd19};
  std::array<unsigned char, 64> buffer_{};
  std::size_t buffer_len_ = 0;
  std::uint64_t bit_len_ = 0;
};
// NOLINTEND(cppcoreguidelines-pro-bounds-constant-array-index,cppcoreguidelines-pro-bounds-pointer-arithmetic)

/** Streams a file through SHA-256; empty string on read failure. */
auto file_sha256(const fs::path &path) -> std::string {
  std::FILE *file = std::fopen(path.string().c_str(), "rb");
  if (file == nullptr) {
    return {};
  }
  sha256 hasher;
  std::array<unsigned char, 1 << 16> chunk{};
  while (true) {
    const std::size_t n = std::fread(chunk.data(), 1, chunk.size(), file);
    if (n > 0) {
      hasher.update(chunk.data(), n);
    }
    if (n < chunk.size()) {
      break;
    }
  }
  const bool ok = std::ferror(file) == 0;
  std::fclose(file);
  if (!ok) {
    return {};
  }
  return hasher.hex();
}

void curl_global_init_once() {
  static std::once_flag flag;
  std::call_once(flag, []() -> void { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

/** Per-transfer state shared with the libcurl callbacks. */
struct transfer_state {
  std::FILE *file = nullptr;
  std::uint64_t resume_from = 0;
  std::uint64_t expected_total = 0;
  std::stop_token stop_token;
  const download_progress_cb *progress = nullptr;

  std::chrono::steady_clock::time_point window_start{};
  std::uint64_t window_start_bytes = 0;
  double speed_bps = 0.0;
  std::chrono::steady_clock::time_point last_report{};
};

auto write_cb(char *ptr, std::size_t size, std::size_t nmemb, void *userdata)
    -> std::size_t {
  auto *state = static_cast<transfer_state *>(userdata);
  return std::fwrite(ptr, size, nmemb, state->file) * size;
}

auto xfer_cb(void *clientp, curl_off_t /*dltotal*/, curl_off_t dlnow,
             curl_off_t /*ultotal*/, curl_off_t /*ulnow*/) -> int {
  auto *state = static_cast<transfer_state *>(clientp);
  if (state->stop_token.stop_requested()) {
    return 1; // aborts curl_easy_perform with CURLE_ABORTED_BY_CALLBACK
  }

  const auto now = std::chrono::steady_clock::now();
  const std::uint64_t done =
      state->resume_from + static_cast<std::uint64_t>(dlnow);

  if (state->window_start.time_since_epoch().count() == 0) {
    state->window_start = now;
    state->window_start_bytes = done;
  }
  const auto window =
      std::chrono::duration<double>(now - state->window_start).count();
  if (window >= 0.5) {
    state->speed_bps =
        static_cast<double>(done - state->window_start_bytes) / window;
    state->window_start = now;
    state->window_start_bytes = done;
  }

  if (state->progress != nullptr &&
      std::chrono::duration<double>(now - state->last_report).count() >= 0.25) {
    state->last_report = now;
    (*state->progress)(download_progress{.done_bytes = done,
                                         .total_bytes = state->expected_total,
                                         .speed_bps = state->speed_bps});
  }
  return 0;
}

} // namespace

auto asset_ready(const remote_asset &asset) -> bool {
  // Size-only: the SHA-256 is verified once, right after the download
  // completes (see download_asset). Re-hashing multi-GB weights on every
  // readiness probe would add tens of seconds to each EnsureModel /
  // RunJunkPass precondition check.
  std::error_code ec;
  const auto size = fs::file_size(asset.dest, ec);
  return !ec && (asset.size_bytes == 0 || size == asset.size_bytes);
}

auto download_asset(const remote_asset &asset, std::stop_token stop_token,
                    const download_progress_cb &progress)
    -> std::expected<void, std::string> {
  if (asset_ready(asset)) {
    return {};
  }
  curl_global_init_once();

  const fs::path part = fs::path(asset.dest) += ".part";
  std::error_code ec;
  fs::create_directories(asset.dest.parent_path(), ec);

  std::uint64_t resume_from = 0;
  if (const auto part_size = fs::file_size(part, ec); !ec) {
    if (part_size < asset.size_bytes) {
      resume_from = part_size;
    } else {
      fs::remove(part, ec); // stale/oversized — start over
    }
  }

  std::FILE *file =
      std::fopen(part.string().c_str(), resume_from > 0 ? "ab" : "wb");
  if (file == nullptr) {
    return std::unexpected("cannot open " + part.string() + " for writing");
  }

  transfer_state state;
  state.file = file;
  state.resume_from = resume_from;
  state.expected_total = asset.size_bytes;
  state.stop_token = std::move(stop_token);
  state.progress = &progress;

  CURL *curl = curl_easy_init();
  if (curl == nullptr) {
    std::fclose(file);
    return std::unexpected("curl_easy_init failed");
  }

  std::array<char, CURL_ERROR_SIZE> errbuf{};
  curl_easy_setopt(curl, CURLOPT_URL, asset.url.c_str());
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "kustavi/1.0");
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
  // Abort a wedged transfer: < 1 KB/s sustained for 60 s.
  curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);
  curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 60L);
  curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf.data());
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &state);
  curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, xfer_cb);
  curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &state);
  if (resume_from > 0) {
    curl_easy_setopt(curl, CURLOPT_RESUME_FROM_LARGE,
                     static_cast<curl_off_t>(resume_from));
  }

  const CURLcode code = curl_easy_perform(curl);
  curl_easy_cleanup(curl);
  std::fclose(file);

  if (code == CURLE_ABORTED_BY_CALLBACK) {
    return std::unexpected("download cancelled");
  }
  if (code != CURLE_OK) {
    const std::string detail = errbuf[0] != '\0'
                                   ? std::string(errbuf.data())
                                   : std::string(curl_easy_strerror(code));
    return std::unexpected("download failed: " + detail);
  }

  const auto final_size = fs::file_size(part, ec);
  if (ec || (asset.size_bytes != 0 && final_size != asset.size_bytes)) {
    fs::remove(part, ec);
    return std::unexpected("downloaded size mismatch for " +
                           asset.dest.filename().string());
  }
  if (!asset.sha256_hex.empty() && file_sha256(part) != asset.sha256_hex) {
    fs::remove(part, ec);
    return std::unexpected("checksum mismatch for " +
                           asset.dest.filename().string());
  }

  fs::rename(part, asset.dest, ec);
  if (ec) {
    return std::unexpected("cannot install " + asset.dest.string() + ": " +
                           ec.message());
  }
  return {};
}

} // namespace kustavi::net
