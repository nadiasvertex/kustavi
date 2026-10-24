// Draws one screen of the wizard to a PNG file, for a look at the layout without a back end.
//
//   kustavi_gui_preview --screen NAME --out FILE.png [--photos DIR] [--raster] [--scale S]
//
// NAME is one of start, downloading, scanning, restore, confirm, none, failed, trips,
// trips-running, menu, quality, similar, junk-prep, junk-running, review, summary, copying, done and detail. A scripted fake back end puts the controller in the state of that screen.

#include "app/executable_path.h"
#include "app/fonts.h"
#include "app/wizard_shell.h"
#include "testing/fake_kustavi_client.h"

#include "magma/magma.h"

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <print>
#include <string>
#include <string_view>
#include <thread>

namespace {

using namespace kustavi::gui;
namespace fs = std::filesystem;

struct options {
  std::string screen = "start";
  fs::path out;
  fs::path photos = KUSTAVI_PHOTO_DIR;
  bool raster = false;
  float scale = 2;
};

auto parse(int argc, char **argv) -> std::optional<options> {
  options result;
  for (int i = 1; i < argc; ++i) {
    std::string_view const arg = argv[i];
    auto const value = [&]() -> char const * { return i + 1 < argc ? argv[++i] : nullptr; };
    if (arg == "--raster") {
      result.raster = true;
    } else if (auto const *text = (arg == "--screen" || arg == "--out" || arg == "--photos" ||
                                   arg == "--scale")
                                      ? value()
                                      : nullptr) {
      if (arg == "--screen") {
        result.screen = text;
      } else if (arg == "--out") {
        result.out = text;
      } else if (arg == "--photos") {
        result.photos = text;
      } else {
        result.scale = static_cast<float>(std::atof(text));
      }
    } else {
      std::println(stderr, "Unknown or incomplete option: {}", arg);
      return std::nullopt;
    }
  }
  if (result.out.empty()) {
    std::println(stderr, "--out is required");
    return std::nullopt;
  }
  return result;
}

auto photo_files(fs::path const &directory) -> std::vector<fs::path> {
  std::vector<fs::path> files;
  if (fs::is_directory(directory)) {
    for (auto const &entry : fs::directory_iterator{directory}) {
      auto extension = entry.path().extension().string();
      std::ranges::transform(extension, extension.begin(),
                             [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      if (extension == ".jpg" || extension == ".jpeg" || extension == ".png") {
        files.push_back(entry.path());
      }
    }
  }
  std::ranges::sort(files);
  return files;
}

} // namespace

auto main(int argc, char **argv) -> int {
  auto const config = parse(argc, argv);
  if (!config) {
    return EXIT_FAILURE;
  }
  auto const preference = config->raster ? magma::backend_preference::raster_only
                                         : magma::backend_preference::automatic;
  auto renderer = magma::renderer::create_offscreen(preference);
  constexpr basalt::size viewport{1100, 720};

  ui_queue queue;
  auto client = std::make_shared<testing::fake_kustavi_client>();
  basalt::text::font_manager fonts;
  add_application_fonts(fonts, current_executable(argv[0]));
  basalt::text::text_shaper shaper{fonts};
  basalt::widget_tree tree{basalt::make_theme(basalt::color_scheme::light)};
  tree.set_text_shaper(&shaper);
  auto factors = tree.scale();
  factors.display = config->scale;
  tree.set_scale(factors);
  tree.set_viewport(viewport);
  renderer.set_metrics(viewport, config->scale);

  image_loader loader{256, 4, [] {}};
  wizard_controller controller{queue, inline_executor()};
  controller.set_client(client);
  model_status model{queue};
  shell_services services;
  services.version = "0.0.0";
  image_loader master_loader{4096, 2, [] {}, true};
  wizard_shell shell{tree, controller, model, loader, master_loader, services};
  controller.on_change([&shell] { shell.phase_changed(); });
  model.on_change([&shell](model_prep_state const &) { shell.mark_dirty(); });

  auto const settle = [&] {
    while (queue.pending()) {
      queue.drain();
    }
  };
  auto const photos = photo_files(config->photos);
  auto const scan_events = [&](std::size_t count) {
    for (std::size_t i = 0; i < count && !photos.empty(); ++i) {
      kustavi::ScanEvent event;
      auto *meta = event.mutable_image();
      meta->set_id(std::format("2024/IMG_{:04}.jpg", i));
      meta->set_name(std::format("IMG_{:04}.jpg", i));
      meta->set_thumbnail_path(photos[i % photos.size()].string());
      client->scan_events.push_back(event);
    }
    kustavi::ScanEvent complete;
    complete.mutable_complete()->set_images(static_cast<std::uint32_t>(count));
    client->scan_events.push_back(complete);
  };

  // Sixty photos in three trips (the second has two legs) and ten in none.
  auto const trips_events = [&] {
    constexpr std::int64_t day = 86'400'000;
    constexpr std::int64_t start = 1'775'000'000'000;
    for (std::size_t i = 0; i < 70 && !photos.empty(); ++i) {
      kustavi::ScanEvent event;
      auto *meta = event.mutable_image();
      meta->set_id(std::format("IMG_{:04}.jpg", i));
      meta->set_name(std::format("IMG_{:04}.jpg", i));
      meta->set_thumbnail_path(photos[i % photos.size()].string());
      meta->set_taken_unix_ms(start + static_cast<std::int64_t>(i) * day / 4);
      client->scan_events.push_back(event);
    }
    kustavi::ScanEvent complete;
    complete.mutable_complete()->set_images(70);
    client->scan_events.push_back(complete);
    auto const make_trip = [&](std::uint32_t id, std::size_t from, std::size_t to, char const *folder,
                               char const *place) {
      kustavi::TripsEvent event;
      auto *trip = event.mutable_trip();
      trip->set_id(id);
      trip->set_start_unix_ms(start + static_cast<std::int64_t>(from) * day / 4);
      trip->set_end_unix_ms(start + static_cast<std::int64_t>(to) * day / 4);
      trip->set_folder(folder);
      trip->set_folder_slug(folder);
      trip->set_place_name(place);
      for (std::size_t i = from; i < to; ++i) {
        trip->add_image_ids(std::format("IMG_{:04}.jpg", i));
      }
      return event;
    };
    auto rome = make_trip(1, 0, 20, "Rome, Italy · April 2026", "Italy");
    auto alps = make_trip(2, 20, 60, "Alps · April 2026", "Switzerland");
    for (auto [leg_from, leg_to, name] : {std::tuple{20, 38, "Zermatt, Switzerland"},
                                          std::tuple{38, 60, "Interlaken, Switzerland"}}) {
      auto *leg = alps.mutable_trip()->add_legs();
      leg->set_place_name(name);
      leg->set_slug(name);
      for (int i = leg_from; i < leg_to; ++i) {
        leg->add_image_ids(std::format("IMG_{:04}.jpg", i));
      }
    }
    client->trips_events = {rome, alps};
  };

  auto const &screen = config->screen;
  if (screen == "downloading") {
    kustavi::ModelEvent progress;
    progress.mutable_progress()->set_done_bytes(1'200'000'000);
    progress.mutable_progress()->set_total_bytes(2'900'000'000);
    client->model_events = {progress};
    model.start(client);
  } else if (screen == "scanning") {
    client->hold_streams = true;
    controller.select_folder("/Users/me/Pictures/Italy 2024");
  } else if (screen == "restore") {
    client->inspect_response.set_has_session(true);
    client->inspect_response.set_image_count(1432);
    client->inspect_response.set_resume_step(2);
    controller.select_folder("/Users/me/Pictures/Italy 2024");
  } else if (screen == "confirm") {
    scan_events(300);
    controller.select_folder("/Users/me/Pictures/Italy 2024");
  } else if (screen == "none") {
    scan_events(0);
    controller.select_folder("/Users/me/Pictures/Empty");
  } else if (screen == "failed") {
    client->stream_error = backend_error{status_code::internal, "The disk is full."};
    controller.select_folder("/Users/me/Pictures/Italy 2024");
  } else if (screen == "trips") {
    trips_events();
    controller.select_folder("/Users/me/Pictures/Alps");
    settle();
    controller.continue_from_confirm();
  } else if (screen == "menu" || screen == "quality" || screen == "similar" ||
             screen == "junk-prep" || screen == "junk-running") {
    trips_events();
    controller.select_folder("/Users/me/Pictures/Alps");
    settle();
    controller.continue_from_confirm();
    settle();
    controller.continue_from_trips();
    settle();
    controller.select_batch("Alps · April 2026");
    if (screen == "quality") {
      for (int i = 20; i < 34; ++i) {
        kustavi::QualityEvent event;
        event.mutable_flag()->set_image_id(std::format("IMG_{:04}.jpg", i));
        event.mutable_flag()->add_reasons(i % 3 == 0 ? kustavi::UNDER_EXPOSED : kustavi::BLURRY);
        client->quality_events.push_back(event);
      }
      controller.start_batch_pass(wizard_step::quality);
      settle();
      controller.keep_image("IMG_0021.jpg");
      controller.keep_image("IMG_0024.jpg");
    } else if (screen == "similar") {
      for (std::uint32_t group = 0; group < 3; ++group) {
        kustavi::SimilarEvent event;
        event.mutable_group()->set_id(group);
        for (std::uint32_t i = 0; i < 3; ++i) {
          event.mutable_group()->add_image_ids(std::format("IMG_{:04}.jpg", 20 + group * 3 + i));
        }
        event.mutable_group()->set_recommended_keep_id(
            std::format("IMG_{:04}.jpg", 20 + group * 3));
        client->similar_events.push_back(event);
      }
      controller.start_batch_pass(wizard_step::duplicates);
      settle();
      controller.set_group_keeper(1, "IMG_0024.jpg");
    } else if (screen == "junk-prep") {
      controller.start_batch_pass(wizard_step::junk);
      kustavi::ModelEvent progress;
      progress.mutable_progress()->set_done_bytes(1'200'000'000);
      progress.mutable_progress()->set_total_bytes(2'900'000'000);
      progress.mutable_progress()->set_speed_bps(18e6);
      client->model_events = {progress};
      model.start(client);
    } else if (screen == "junk-running") {
      controller.set_model_ready(true);
      client->hold_streams = true;
      controller.start_batch_pass(wizard_step::junk);
    }
  } else if (screen == "review" || screen == "summary" || screen == "copying" ||
             screen == "done") {
    trips_events();
    controller.select_folder("/Users/me/Pictures/Alps");
    settle();
    controller.continue_from_confirm();
    settle();
    controller.continue_from_trips();
    for (int i = 20; i < 29; ++i) {
      kustavi::QualityEvent event;
      event.mutable_flag()->set_image_id(std::format("IMG_{:04}.jpg", i));
      event.mutable_flag()->add_reasons(kustavi::BLURRY);
      client->quality_events.push_back(event);
    }
    controller.select_batch("Alps · April 2026");
    controller.start_batch_pass(wizard_step::quality);
    settle();
    controller.mark_image("IMG_0040.jpg");
    controller.close_batch_review();
    controller.continue_from_batches();
    if (screen != "review") {
      client->estimate_response.set_fits(true);
      client->estimate_response.set_new_bytes(2'400'000'000);
      client->estimate_response.set_total_bytes(2'400'000'000);
      client->estimate_response.set_free_bytes_known(true);
      client->estimate_response.set_free_bytes(180'000'000'000);
      controller.continue_from_deletion_review();
      settle();
    }
    if (screen == "copying" || screen == "done") {
      kustavi::CommitEvent progress;
      progress.mutable_progress()->set_done(31);
      progress.mutable_progress()->set_total(61);
      progress.mutable_progress()->set_current_name("2026/04/IMG_0031.jpg");
      kustavi::CommitEvent complete;
      complete.mutable_complete()->set_copied(61);
      complete.mutable_complete()->set_already_present(4);
      complete.mutable_complete()->add_errors("IMG_0050.jpg: permission denied");
      client->commit_events = screen == "copying" ? std::vector{progress}
                                                  : std::vector{progress, complete};
      client->hold_streams = screen == "copying";
      controller.start_commit();
      if (screen == "copying") {
        client->release_streams();
        queue.drain();
        client->hold_streams = true;
      }
    }
  } else if (screen == "detail") {
    trips_events();
    controller.select_folder("/Users/me/Pictures/Alps");
    settle();
    controller.continue_from_confirm();
    settle();
    shell.update();
    shell.open_detail(*controller.image("IMG_0001.jpg"));
  } else if (screen == "trips-running") {
    trips_events();
    controller.select_folder("/Users/me/Pictures/Alps");
    settle();
    client->hold_streams = true;
    controller.continue_from_confirm();
  } else if (screen != "start") {
    std::println(stderr, "Unknown screen: {}", screen);
    return EXIT_FAILURE;
  }
  settle();
  shell.update();

  // Give the loader a moment to decode the images in view, then draw the frame that has them.
  auto commands = basalt::paint_commands{};
  auto const draw = [&] {
    shell.update();
    tree.update_layout();
    shell.update();
    tree.update_layout();
    (void)tree.take_damage();
    commands.clear();
    basalt::paint_tree(tree, commands);
    auto const whole = std::vector<basalt::rect>{{{0, 0}, viewport}};
    auto const drawn = renderer.render(commands, whole, tree.theme().colors.background_default);
    if (!drawn) {
      std::println(stderr, "Rendering failed: {}", drawn.error().message);
    }
  };
  draw();
  std::this_thread::sleep_for(std::chrono::milliseconds{800});
  draw();

  auto const frame = renderer.read_frame();
  if (frame == nullptr) {
    std::println(stderr, "No frame");
    return EXIT_FAILURE;
  }
  cv::Mat rgba(static_cast<int>(frame->height()), static_cast<int>(frame->width()), CV_8UC4,
               const_cast<std::uint8_t *>(frame->pixels().data()));
  cv::Mat bgra;
  cv::cvtColor(rgba, bgra, cv::COLOR_RGBA2BGRA);
  if (!cv::imwrite(config->out.string(), bgra)) {
    std::println(stderr, "Cannot write {}", config->out.string());
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
