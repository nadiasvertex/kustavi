// The Kustavi GUI.
//
//   kustavi_gui [--raster]
//
// The window follows magma's event loop: wait for events, feed the frame scheduler, dispatch input
// to the Basalt event router and draw when a frame is due. Workers hand their results to the UI
// thread through a `ui_queue`.

#include "app/executable_path.h"
#include "app/image_loader.h"
#include "app/wizard_shell.h"
#include "core/backend_host.h"

#include "basalt/basalt.h"
#include "magma/magma.h"

#include <cstdlib>
#include <filesystem>
#include <print>
#include <string>
#include <string_view>

namespace {

using namespace kustavi::gui;

constexpr auto initial_size = basalt::size{1280, 800};
// The longest side of the working images that the grid shows, in pixels.
constexpr auto thumbnail_edge = 256;

auto now_ns() -> std::int64_t { return magma::platform::event_clock_now().count(); }

auto refresh_interval(magma::platform const &platform, magma::window_id window) -> std::int64_t {
  auto const rate = platform.display_refresh_rate(window).value_or(60.0F);
  return static_cast<std::int64_t>(std::llround(1e9 / rate));
}

auto render_frame(basalt::widget_tree &tree, magma::renderer &renderer,
                  basalt::paint_commands &commands) -> void {
  auto const damage = tree.take_damage();
  bool const full = renderer.needs_full_redraw();
  if (damage.empty() && !full) {
    return;
  }
  commands.clear();
  basalt::paint_tree(tree, commands);
  auto const whole = std::vector<basalt::rect>{{{0, 0}, tree.viewport()}};
  auto const drawn = renderer.render(commands, full ? std::span{whole} : std::span{damage},
                                     tree.theme().colors.background_default);
  if (!drawn) {
    std::println(stderr, "Rendering failed: {}", drawn.error().message);
  }
}

auto run(int, char **argv, bool raster) -> int {
  auto created = magma::platform::create();
  if (!created) {
    std::println(stderr, "{}", created.error().message);
    return EXIT_FAILURE;
  }
  auto platform = std::move(*created);
  auto const preference =
      raster ? magma::backend_preference::raster_only : magma::backend_preference::automatic;
  auto const window = platform.create_window({
      .graphics = magma::preferred_graphics_api(preference),
      .title = "Kustavi",
      .size = initial_size,
  });
  if (!window) {
    std::println(stderr, "{}", window.error().message);
    return EXIT_FAILURE;
  }
  auto rendering = magma::renderer::create(platform, *window, preference);
  if (!rendering) {
    std::println(stderr, "{}", rendering.error().message);
    return EXIT_FAILURE;
  }
  auto renderer = std::move(*rendering);

  auto scheduler = magma::frame_scheduler{refresh_interval(platform, *window)};
  ui_queue queue{[&platform] { platform.wake(); }};
  worker_pool pool{2};

  basalt::text::font_manager fonts;
  fonts.add_system_fonts();
  basalt::text::text_shaper shaper{fonts};
  basalt::widget_tree tree{basalt::make_theme(basalt::color_scheme::light)};
  tree.set_text_shaper(&shaper);
  tree.set_frame_request_handler([&scheduler] { scheduler.request_frame(); });
  auto router = basalt::event_router{tree};

  image_loader loader{thumbnail_edge, 4, [&queue, &scheduler] {
                        queue.post([&scheduler] { scheduler.request_frame(); });
                      }};
  wizard_controller controller{queue, pool.executor()};
  model_status model{queue};

  process_connector_options connector;
  if (auto const *override_path = std::getenv("KUSTAVI_BACKEND_PATH")) {
    connector.binary_override = override_path;
  }
  connector.executable = current_executable(argv[0]);
  connector.temp_directory = std::filesystem::temp_directory_path();
  connector.parent_pid = current_process_id();
  backend_host backend{queue, pool.executor(), make_process_connector(std::move(connector))};

  auto running = true;
  auto quitting = false;
  auto const begin_quit = [&] {
    if (quitting) {
      return;
    }
    quitting = true;
    backend.quit([&running] { running = false; });
  };

  shell_services services;
  services.version = KUSTAVI_VERSION;
  services.choose_folder = [&](std::function<void(std::string)> chosen) {
    platform.show_open_folder_dialog(*window, [&queue, chosen = std::move(chosen)](auto folders) {
      // The chooser may call back from another thread.
      if (folders && !folders->empty()) {
        queue.post([chosen, folder = folders->front()] { chosen(folder); });
      }
    });
  };
  services.pick_folder = [&] {
    services.choose_folder([&controller](std::string folder) {
      controller.select_folder(std::move(folder));
    });
  };
  services.exit_app = begin_quit;
  services.cancel_download = [&model] { model.cancel_download(); };
  services.retry_download = [&model] { model.retry_download(); };

  image_loader master_loader{8192, 2,
                             [&queue, &scheduler] {
                               queue.post([&scheduler] { scheduler.request_frame(); });
                             },
                             true};
  services.wake = [&queue, &scheduler] {
    queue.post([&scheduler] { scheduler.request_frame(); });
  };
  wizard_shell shell{tree, controller, model, loader, master_loader, services,
                     default_video_player_factory()};
  shell.services().show_backend_log = [&] { shell.show_log_dialog(backend.log_text()); };

  auto const dirty = [&] {
    shell.mark_dirty();
    scheduler.request_frame();
  };
  controller.on_change([&] { shell.phase_changed(); scheduler.request_frame(); });
  model.on_change([&](model_prep_state const &state) {
    // The junk pass starts when the model is ready.
    controller.set_model_ready(std::holds_alternative<model_prep_ready>(state));
    dirty();
  });
  controller.set_cancel_model_download([&model] { model.cancel_download(); });

  auto const retry_backend = [&] {
    backend.restart();
    if (std::holds_alternative<wizard_failed>(controller.phase())) {
      controller.go_back_from_error();
    }
  };
  backend_host::handlers handlers;
  handlers.on_ready = [&](std::shared_ptr<kustavi_client> client) {
    controller.set_client(client);
    model.start(std::move(client));
  };
  handlers.on_crashed = [&] {
    controller.set_client(nullptr);
    shell.show_crash_dialog("The back end stopped unexpectedly.", retry_backend);
  };
  handlers.on_failed = [&](std::string message) {
    shell.show_crash_dialog("The back end could not be started. " + message, retry_backend);
  };
  backend.set_handlers(std::move(handlers));
  backend.start();

  auto const apply_window = [&] {
    auto factors = tree.scale();
    factors.display = platform.display_scale(*window).value_or(1.0F);
    tree.set_scale(factors);
    auto const size = platform.window_size(*window).value_or(initial_size);
    tree.set_viewport(size);
    renderer.set_metrics(size, factors.display);
    renderer.request_full_redraw();
    scheduler.request_frame();
  };
  apply_window();

  auto commands = basalt::paint_commands{};
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

    queue.drain();

    for (auto const &incoming : platform.poll_events()) {
      if (auto const *const event = std::get_if<basalt::window_event>(&incoming.event)) {
        if (event->action == basalt::window_action::close_requested) {
          begin_quit();
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
      shell.update();
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
  bool raster = false;
  for (int i = 1; i < argc; ++i) {
    if (std::string_view{argv[i]} == "--raster") {
      raster = true;
    } else {
      std::println(stderr, "Unknown option: {}", argv[i]);
      return EXIT_FAILURE;
    }
  }
  return run(argc, argv, raster);
}
