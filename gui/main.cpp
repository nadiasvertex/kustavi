// A spike of the C++ GUI: a grid of thousands of photo thumbnails in a Magma window.
//
//   kustavi_gui_spike [--items N] [--photos DIR]                    show the window
//   kustavi_gui_spike --bench [--frames N] [--raster] [--scale S]   scroll offscreen, print timings
//   kustavi_gui_spike --screenshot FILE.png [--scroll PIXELS]       save one frame
//
// The grid asks for the images of the cells in view, a few worker threads supply them, and the
// renderer draws the result. The photos in DIR are decoded once and stand in for the many files of
// a real collection; see `thumbnail_loader`.

#include "basalt/basalt.h"
#include "magma/magma.h"

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <print>
#include <ranges>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

#include "thumbnail_loader.h"

namespace {

namespace fs = std::filesystem;

struct options {
  std::size_t items = 5'000;
  fs::path photos = KUSTAVI_PHOTO_DIR;
  bool bench = false;
  std::size_t frames = 600;
  bool raster = false;
  float scale = 2;
  basalt::size viewport{1280, 800};
  fs::path screenshot;
  float scroll = 0;
};

auto parse(int argc, char **argv) -> std::optional<options> {
  auto result = options{};
  for (int i = 1; i < argc; ++i) {
    auto const arg = std::string_view{argv[i]};
    auto const value = [&]() -> std::optional<std::string_view> {
      return i + 1 < argc ? std::optional<std::string_view>{argv[++i]} : std::nullopt;
    };
    auto const number = [&](auto &target) {
      auto const text = value();
      if (!text) {
        return false;
      }
      target = static_cast<std::remove_reference_t<decltype(target)>>(std::atof(text->data()));
      return true;
    };
    bool ok = true;
    if (arg == "--items") {
      ok = number(result.items);
    } else if (arg == "--frames") {
      ok = number(result.frames);
    } else if (arg == "--scale") {
      ok = number(result.scale);
    } else if (arg == "--scroll") {
      ok = number(result.scroll);
    } else if (arg == "--photos") {
      auto const text = value();
      ok = text.has_value();
      if (ok) {
        result.photos = *text;
      }
    } else if (arg == "--screenshot") {
      auto const text = value();
      ok = text.has_value();
      if (ok) {
        result.screenshot = *text;
      }
    } else if (arg == "--bench") {
      result.bench = true;
    } else if (arg == "--raster") {
      result.raster = true;
    } else {
      ok = false;
    }
    if (!ok) {
      std::println(stderr, "Unknown or incomplete option: {}", arg);
      return std::nullopt;
    }
  }
  return result;
}

auto photo_files(fs::path const &directory) -> std::vector<fs::path> {
  auto files = std::vector<fs::path>{};
  if (!fs::is_directory(directory)) {
    return files;
  }
  for (auto const &entry : fs::directory_iterator{directory}) {
    auto extension = entry.path().extension().string();
    std::ranges::transform(extension, extension.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (extension == ".jpg" || extension == ".jpeg" || extension == ".png") {
      files.push_back(entry.path());
    }
  }
  std::ranges::sort(files);
  return files;
}

constexpr auto cell_edge = 160.0F;
constexpr auto thumbnail_edge = 256;

// The tree, the grid and the images behind it. Everything here runs on the UI thread except the
// loader's workers.
class photo_browser {
public:
  photo_browser(options const &config, std::function<void()> on_ready)
      : tree_(basalt::make_theme(basalt::color_scheme::light)),
        loader_(photo_files(config.photos), thumbnail_edge, 4, std::move(on_ready)) {
    fonts_.add_system_fonts();
    shaper_ = std::make_unique<basalt::text::text_shaper>(fonts_);
    tree_.set_text_shaper(shaper_.get());

    grid_ = &tree_.emplace_root<basalt::grid_view>("Photos");
    grid_->set_cell_size(basalt::fixed(cell_edge), basalt::fixed(cell_edge));
    grid_->set_gap(basalt::fixed(8));
    grid_->set_cell_factory([this](std::size_t index, std::unique_ptr<basalt::widget> recycled) {
      return make_cell(index, std::move(recycled));
    });
    grid_->set_on_visible_change([this](basalt::index_range) { request_images(); });
    grid_->set_item_count(config.items);
  }

  [[nodiscard]] auto tree() -> basalt::widget_tree & { return tree_; }
  [[nodiscard]] auto grid() -> basalt::grid_view & { return *grid_; }
  [[nodiscard]] auto source_count() const -> std::size_t { return loader_.source_count(); }

  // Gives the finished images to the cells that show them.
  auto apply_ready_images() -> void {
    for (auto &[item, image] : loader_.take_ready()) {
      if (auto *const cell = grid_->cell(item)) {
        static_cast<basalt::image_tile *>(cell)->set_image(image);
      }
      images_[item] = std::move(image);
    }
    trim_images();
  }

private:
  auto make_cell(std::size_t index, std::unique_ptr<basalt::widget> recycled)
      -> std::unique_ptr<basalt::widget> {
    std::unique_ptr<basalt::image_tile> tile;
    if (recycled != nullptr) {
      tile.reset(static_cast<basalt::image_tile *>(recycled.release()));
    } else {
      tile = std::make_unique<basalt::image_tile>();
    }
    tile->set_caption(std::format("IMG_{:05}.jpg", index));
    auto const known = images_.find(index);
    tile->set_image(known != images_.end() ? known->second : nullptr);
    return tile;
  }

  // Asks for the images of the cells that have none, the ones in view first.
  auto request_images() -> void {
    auto const visible = grid_->visible_items();
    auto const realized = grid_->realized_items();
    auto wanted = std::vector<std::size_t>{};
    auto const add = [&](std::size_t from, std::size_t to) {
      for (std::size_t item = from; item < to; ++item) {
        if (!images_.contains(item)) {
          wanted.push_back(item);
        }
      }
    };
    add(visible.first, visible.last);
    add(realized.first, visible.first);
    add(visible.last, realized.last);
    loader_.request(std::move(wanted));
  }

  // Keeps the images of the cells near the view, and a margin of earlier ones for scrolling back.
  auto trim_images() -> void {
    constexpr std::size_t keep = 1'500;
    if (images_.size() <= keep) {
      return;
    }
    auto const realized = grid_->realized_items();
    std::erase_if(images_, [&](auto const &entry) { return !realized.contains(entry.first); });
  }

  basalt::text::font_manager fonts_;
  std::unique_ptr<basalt::text::text_shaper> shaper_;
  basalt::widget_tree tree_;
  basalt::grid_view *grid_ = nullptr;
  kustavi::gui::thumbnail_loader loader_;
  std::unordered_map<std::size_t, basalt::raster_image_ptr> images_;
};

// Paints what changed and renders it. False when there was nothing to draw.
auto render_frame(basalt::widget_tree &tree, magma::renderer &renderer,
                  basalt::paint_commands &commands) -> bool {
  auto const damage = tree.take_damage();
  bool const full = renderer.needs_full_redraw();
  if (damage.empty() && !full) {
    return false;
  }
  commands.clear();
  basalt::paint_tree(tree, commands);
  auto const whole = std::vector<basalt::rect>{{{0, 0}, tree.viewport()}};
  auto const drawn = renderer.render(commands, full ? std::span{whole} : std::span{damage},
                                     tree.theme().colors.background_default);
  if (!drawn) {
    std::println(stderr, "Rendering failed: {}", drawn.error().message);
  }
  return true;
}

auto set_metrics(basalt::widget_tree &tree, magma::renderer &renderer, basalt::size viewport,
                 float scale) -> void {
  auto factors = tree.scale();
  factors.display = scale;
  tree.set_scale(factors);
  tree.set_viewport(viewport);
  renderer.set_metrics(viewport, scale);
}

auto save_frame(magma::renderer const &renderer, fs::path const &path) -> bool {
  auto const frame = renderer.read_frame();
  if (frame == nullptr) {
    return false;
  }
  cv::Mat rgba(static_cast<int>(frame->height()), static_cast<int>(frame->width()), CV_8UC4,
               const_cast<std::uint8_t *>(frame->pixels().data()));
  cv::Mat bgra;
  cv::cvtColor(rgba, bgra, cv::COLOR_RGBA2BGRA);
  return cv::imwrite(path.string(), bgra);
}

auto scroll_to(photo_browser &browser, magma::renderer &renderer, basalt::paint_commands &commands,
               float offset) -> bool {
  browser.grid().set_scroll_offset(offset);
  browser.tree().update_layout();
  browser.apply_ready_images();
  browser.tree().update_layout();
  return render_frame(browser.tree(), renderer, commands);
}

auto percentile(std::vector<double> const &sorted, double fraction) -> double {
  return sorted[std::min(sorted.size() - 1, static_cast<std::size_t>(fraction * sorted.size()))];
}

// Renders `frames` frames offscreen, scrolling a little more each time, and prints how long they
// took. A frame is the work for one display refresh: layout, the factory and loader results, the
// paint commands and the renderer.
auto run_bench(options const &config, photo_browser &browser) -> int {
  auto renderer = magma::renderer::create_offscreen(config.raster
                                                        ? magma::backend_preference::raster_only
                                                        : magma::backend_preference::automatic);
  set_metrics(browser.tree(), renderer, config.viewport, config.scale);
  auto commands = basalt::paint_commands{};
  browser.tree().update_layout();
  render_frame(browser.tree(), renderer, commands);

  // A page of rows each second at 60 frames a second is a fast scroll.
  auto const step = config.scroll > 0 ? config.scroll : 24.0F;
  auto durations = std::vector<double>{};
  durations.reserve(config.frames);
  for (std::size_t frame = 0; frame < config.frames; ++frame) {
    auto const start = std::chrono::steady_clock::now();
    scroll_to(browser, renderer, commands, step * static_cast<float>(frame));
    auto const spent = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                                 start);
    durations.push_back(spent.count());
  }
  std::ranges::sort(durations);
  auto const total = std::ranges::fold_left(durations, 0.0, std::plus<>{});
  std::println("backend {}, {} frames, {}x{} at {}x, {} items, {} source photos",
               renderer.active_backend() == magma::backend::raster ? "raster" : "gpu",
               durations.size(), config.viewport.width, config.viewport.height, config.scale,
               config.items, browser.source_count());
  std::println("frame ms: mean {:.2f}  p50 {:.2f}  p95 {:.2f}  p99 {:.2f}  max {:.2f}",
               total / static_cast<double>(durations.size()), percentile(durations, 0.5),
               percentile(durations, 0.95), percentile(durations, 0.99), durations.back());
  std::println("visible items {}..{}, realized cells {}", browser.grid().visible_items().first,
               browser.grid().visible_items().last, browser.grid().realized_items().size());
  if (!config.screenshot.empty() && !save_frame(renderer, config.screenshot)) {
    std::println(stderr, "Cannot write {}", config.screenshot.string());
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}

auto run_screenshot(options const &config, photo_browser &browser) -> int {
  auto renderer = magma::renderer::create_offscreen(config.raster
                                                        ? magma::backend_preference::raster_only
                                                        : magma::backend_preference::automatic);
  set_metrics(browser.tree(), renderer, config.viewport, config.scale);
  auto commands = basalt::paint_commands{};
  browser.tree().update_layout();
  render_frame(browser.tree(), renderer, commands);
  // Give the workers a moment, then draw the frame with the images in it.
  std::this_thread::sleep_for(std::chrono::milliseconds{500});
  scroll_to(browser, renderer, commands, config.scroll);
  if (!save_frame(renderer, config.screenshot)) {
    std::println(stderr, "Cannot write {}", config.screenshot.string());
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}

auto now_ns() -> std::int64_t { return magma::platform::event_clock_now().count(); }

auto refresh_interval(magma::platform const &platform, magma::window_id window) -> std::int64_t {
  auto const rate = platform.display_refresh_rate(window).value_or(60.0F);
  return static_cast<std::int64_t>(std::llround(1e9 / rate));
}

auto run_window(options const &config) -> int {
  auto created = magma::platform::create();
  if (!created) {
    std::println(stderr, "{}", created.error().message);
    return EXIT_FAILURE;
  }
  auto platform = std::move(*created);
  auto const window = platform.create_window({
      .graphics = magma::preferred_graphics_api(config.raster
                                                    ? magma::backend_preference::raster_only
                                                    : magma::backend_preference::automatic),
      .title = "Kustavi",
      .size = config.viewport,
  });
  if (!window) {
    std::println(stderr, "{}", window.error().message);
    return EXIT_FAILURE;
  }
  auto rendering = magma::renderer::create(platform, *window,
                                           config.raster ? magma::backend_preference::raster_only
                                                         : magma::backend_preference::automatic);
  if (!rendering) {
    std::println(stderr, "{}", rendering.error().message);
    return EXIT_FAILURE;
  }
  auto renderer = std::move(*rendering);

  auto browser = photo_browser{config, [&platform] { platform.wake(); }};
  auto &tree = browser.tree();
  auto router = basalt::event_router{tree};
  auto scheduler = magma::frame_scheduler{refresh_interval(platform, *window)};
  tree.set_frame_request_handler([&scheduler] { scheduler.request_frame(); });

  auto const apply_window = [&] {
    set_metrics(tree, renderer, platform.window_size(*window).value_or(config.viewport),
                platform.display_scale(*window).value_or(1.0F));
    renderer.request_full_redraw();
    scheduler.request_frame();
  };
  apply_window();

  auto commands = basalt::paint_commands{};
  auto running = true;
  while (running) {
    if (scheduler.pending()) {
      (void)platform.request_display_tick(*window);
    }
    auto const timeout = scheduler.wait_timeout(now_ns());
    platform.wait_for_events(timeout ? std::optional{std::chrono::nanoseconds{*timeout}}
                                     : std::nullopt);
    if (auto const tick = platform.take_display_tick(*window)) {
      scheduler.display_refreshed(tick->time.count(), tick->interval.count());
    }
    if (!renderer.frame_in_flight()) {
      scheduler.frame_shown();
    }

    for (auto const &incoming : platform.poll_events()) {
      if (auto const *const event = std::get_if<basalt::window_event>(&incoming.event)) {
        if (event->action == basalt::window_action::close_requested) {
          running = false;
        } else if (event->action == basalt::window_action::resized ||
                   event->action == basalt::window_action::scale_changed ||
                   event->action == basalt::window_action::shown) {
          if (!scheduler.follows_display()) {
            scheduler.set_interval(refresh_interval(platform, *window));
          }
          apply_window();
        }
        continue;
      }
      router.dispatch(incoming.event);
      scheduler.request_input_frame();
    }

    auto const time = now_ns();
    if (running && scheduler.frame_due(time)) {
      scheduler.begin_frame(time);
      tree.advance_animations(std::chrono::nanoseconds{time});
      tree.update_layout();
      browser.apply_ready_images();
      tree.update_layout();
      router.update_hover();
      render_frame(tree, renderer, commands);
      tree.collect_garbage();
    }
  }
  return EXIT_SUCCESS;
}

} // namespace

auto main(int argc, char **argv) -> int {
  auto const config = parse(argc, argv);
  if (!config) {
    return EXIT_FAILURE;
  }
  if (photo_files(config->photos).empty()) {
    std::println(stderr, "No photos in {}", config->photos.string());
    return EXIT_FAILURE;
  }
  if (config->bench || !config->screenshot.empty()) {
    auto browser = photo_browser{*config, [] {}};
    return config->bench ? run_bench(*config, browser) : run_screenshot(*config, browser);
  }
  return run_window(*config);
}
