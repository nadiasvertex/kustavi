#include "app/screens.h"

#include "app/commit_screens.h"
#include "app/ui_kit.h"
#include "core/format.h"

#include <format>

namespace kustavi::gui {

namespace {

using basalt::button_variant;
using basalt::text_role;
using basalt::text_tone;

constexpr auto content_padding = 16.0F;

} // namespace

auto build_step_indicator(int stage_index) -> std::unique_ptr<basalt::widget> {
  auto bar = ui::row(8);
  bar->set_padding(basalt::length_insets::symmetric(basalt::fixed(content_padding),
                                                    basalt::fixed(10)));
  constexpr std::array stages{wizard_stage::select, wizard_stage::organize, wizard_stage::review,
                              wizard_stage::copy};
  for (auto const stage : stages) {
    auto const index = static_cast<int>(stage);
    if (index > 0) {
      bar->add_child(ui::text("›", text_role::small, text_tone::subdued));
    }
    auto const completed = index < stage_index;
    auto const current = index == stage_index;
    auto const tone = completed ? text_tone::success
                      : current ? text_tone::interactive
                                : text_tone::subdued;
    bar->add_child(ui::text(std::string{label_of(stage)},
                            current ? text_role::small_strong : text_role::small, tone));
  }
  return bar;
}

auto model_card_caption(model_prep_downloading const &state) -> std::string {
  auto const fraction = state.fraction();
  if (!fraction) {
    return "Preparing vision model…";
  }
  return std::format("Preparing vision model — {}% ({} / {})", static_cast<int>(*fraction * 100),
                     format_gb(state.done_bytes), format_gb(state.total_bytes));
}

auto build_start_screen(model_prep_state const &model, shell_services const &services)
    -> std::unique_ptr<basalt::widget> {
  auto screen = ui::centered(12);
  screen->add_child(ui::text("Kustavi", text_role::heading_large));
  screen->add_child(ui::text("Select a folder of photos to keep the best of.", text_role::body,
                             text_tone::subdued));
  screen->add_child(
      ui::make_button("Select folder…", services.pick_folder, button_variant::primary));

  // The card appears only while a download is in progress. Unknown (no event yet) and ready show
  // nothing.
  if (auto const *downloading = std::get_if<model_prep_downloading>(&model)) {
    auto card = ui::column(8);
    card->set_padding(basalt::length_insets::symmetric(basalt::fixed(16), basalt::fixed(12)));
    card->set_surface(basalt::box_surface{});
    card->set_cross_alignment(basalt::alignment::stretch);
    card->add_child(ui::text(model_card_caption(*downloading)));
    auto fraction = downloading->fraction();
    card->add_child(std::make_unique<basalt::progress_bar>(
        fraction ? std::optional<float>{static_cast<float>(*fraction)} : std::nullopt));
    screen->add_child(std::move(card));
  }
  screen->add_child(
      ui::text(std::format("Kustavi v{}", services.version), text_role::small, text_tone::subdued));
  return screen;
}

auto build_scanning_screen(wizard_scanning const &phase, basalt::progress_bar *&busy)
    -> std::unique_ptr<basalt::widget> {
  auto screen = ui::centered(12);
  screen->add_child(ui::text(std::format("Scanning {}", phase.folder), text_role::heading_small));
  busy = &screen->add_child(std::make_unique<basalt::progress_bar>());
  screen->add_child(ui::text(std::format("Found {} {}", format_int(phase.images_found),
                                         phase.images_found == 1 ? "image" : "images")));
  if (!phase.current_path.empty()) {
    screen->add_child(ui::text(phase.current_path, text_role::source, text_tone::subdued));
  }
  return screen;
}

auto build_progress_screen(std::string title, std::uint32_t done, std::uint32_t total,
                           basalt::progress_bar *&busy, std::string caption)
    -> std::unique_ptr<basalt::widget> {
  auto screen = ui::centered(12);
  screen->add_child(ui::text(std::move(title), text_role::heading_small));
  if (total > 0) {
    screen->add_child(std::make_unique<basalt::progress_bar>(
        static_cast<float>(done) / static_cast<float>(total)));
    screen->add_child(ui::text(std::format("{} of {}", format_int(done), format_int(total))));
    busy = nullptr;
  } else {
    // Without a total the bar moves along its track, and the shell ticks it every frame.
    busy = &screen->add_child(std::make_unique<basalt::progress_bar>());
  }
  if (!caption.empty()) {
    screen->add_child(ui::text(std::move(caption), text_role::small, text_tone::subdued));
  }
  return screen;
}

auto build_junk_prep_screen(model_prep_state const &model, shell_services const &services)
    -> std::unique_ptr<basalt::widget> {
  if (std::holds_alternative<model_prep_ready>(model)) {
    // The pass starts at once, so there is nothing to show.
    return std::make_unique<basalt::box>();
  }
  auto screen = ui::centered(12);
  if (auto const *failed = std::get_if<model_prep_failed>(&model)) {
    screen->add_child(ui::text("The vision model download failed", text_role::heading_small,
                               text_tone::danger));
    screen->add_child(ui::text(failed->message));
    auto buttons = ui::row(12);
    buttons->add_child(ui::make_button("Retry download", services.retry_download));
    buttons->add_child(ui::make_button("Exit app", services.exit_app, button_variant::tertiary));
    screen->add_child(std::move(buttons));
    return screen;
  }
  model_prep_downloading downloading;
  if (auto const *current = std::get_if<model_prep_downloading>(&model)) {
    downloading = *current;
  }
  auto const fraction = downloading.fraction();
  screen->add_child(ui::text("Downloading vision model", text_role::heading_small));
  screen->add_child(std::make_unique<basalt::progress_bar>(
      fraction ? std::optional<float>{static_cast<float>(*fraction)} : std::nullopt));
  screen->add_child(ui::text(
      fraction ? std::format("{} / {} · {:.1f} MB/s", format_gb(downloading.done_bytes),
                             format_gb(downloading.total_bytes),
                             downloading.speed_bytes_per_second / 1e6)
               : format_bytes(downloading.done_bytes)));
  screen->add_child(ui::text("The Qwen2.5-VL vision model powers the junk pass and is required; "
                             "the download runs in the background.",
                             text_role::small, text_tone::subdued));
  return screen;
}

auto build_session_restore_screen(wizard_session_restore const &phase,
                                  wizard_controller &controller)
    -> std::unique_ptr<basalt::widget> {
  auto const saved = phase.saved_step_index;
  auto const step_label =
      saved >= 0 && saved <= index_of(wizard_step::copy)
          ? label_of(static_cast<wizard_step>(saved))
          : label_of(wizard_step::select);
  auto screen = ui::centered(12);
  screen->add_child(ui::text("Resume this session?", text_role::heading_small));
  screen->add_child(
      ui::text(std::format("Found saved progress in {} — {} images, stopped at the \"{}\" step.",
                           phase.folder, phase.image_count, step_label)));
  auto buttons = ui::row(12);
  buttons->add_child(ui::make_button("Start fresh", [&controller] {
    controller.start_fresh_from_restore();
  }));
  buttons->add_child(ui::make_button(
      "Resume", [&controller] { controller.resume_session(); }, button_variant::primary));
  screen->add_child(std::move(buttons));
  return screen;
}

auto build_confirm_screen(wizard_confirm_folder const &phase, image_grid &grid)
    -> std::unique_ptr<basalt::widget> {
  auto screen = ui::column(8);
  screen->set_padding(basalt::length_insets{basalt::fixed(8), basalt::fixed(content_padding),
                                            basalt::fixed(0), basalt::fixed(content_padding)});
  screen->add_child(ui::text(std::format("{} images in {}", format_int(phase.image_count),
                                         phase.folder),
                             text_role::heading_small));
  if (!phase.scan_errors.empty()) {
    auto const count = phase.scan_errors.size();
    screen->add_child(ui::text(std::format("{} {} could not be read", count,
                                           count == 1 ? "file" : "files"),
                               text_role::small, text_tone::danger));
  }
  ui::fill(*screen, grid.make_widget());
  return screen;
}

auto build_no_images_screen(wizard_no_images const &phase) -> std::unique_ptr<basalt::widget> {
  auto screen = ui::centered(8);
  screen->add_child(ui::text(std::format("No images found in {}", phase.folder),
                             text_role::heading_small));
  screen->add_child(ui::text("Choose another folder to continue."));
  return screen;
}

auto build_failed_screen(wizard_failed const &phase)
    -> std::unique_ptr<basalt::widget> {
  auto screen = ui::centered(8);
  screen->add_child(ui::text("Processing error", text_role::heading_small, text_tone::danger));
  screen->add_child(ui::text(message_of(phase.error)));
  return screen;
}

auto build_action_bar(wizard_phase const &phase, wizard_controller &controller,
                      shell_services const &services, basalt::button **copy)
    -> std::unique_ptr<basalt::widget> {
  auto bar = ui::row(8);
  bar->set_main_alignment(basalt::alignment::end);
  bar->set_padding(basalt::length_insets::symmetric(basalt::fixed(content_padding),
                                                    basalt::fixed(12)));
  struct builder {
    basalt::box &bar;
    wizard_controller &controller;
    shell_services const &services;
    bool any = false;

    auto add(std::string label, std::function<void()> action,
             button_variant variant = button_variant::secondary, bool enabled = true)
        -> basalt::button & {
      auto &button = bar.add_child(ui::make_button(std::move(label), std::move(action), variant));
      button.set_enabled(enabled);
      any = true;
      return button;
    }
  } add{*bar, controller, services};

  std::visit(
      [&](auto const &current) {
        using phase_type = std::decay_t<decltype(current)>;
        if constexpr (std::is_same_v<phase_type, wizard_scanning>) {
          add.add("Cancel", [&controller] { controller.cancel_scan(); });
        } else if constexpr (std::is_same_v<phase_type, wizard_no_images>) {
          add.add("Exit app", services.exit_app, button_variant::tertiary);
          add.add("Choose another folder", [&controller] { controller.reset_to_start(); },
                  button_variant::primary);
        } else if constexpr (std::is_same_v<phase_type, wizard_confirm_folder>) {
          add.add("Back", [&controller] { controller.back_from_confirm(); });
          add.add("Continue", [&controller] { controller.continue_from_confirm(); },
                  button_variant::primary);
        } else if constexpr (std::is_same_v<phase_type, wizard_trips_running>) {
          add.add("Cancel", [&controller] { controller.cancel_trips(); });
        } else if constexpr (std::is_same_v<phase_type, wizard_trips_review>) {
          add.add("Back", [&controller] { controller.back_from_trips(); });
          add.add("Continue", [&controller] { controller.continue_from_trips(); },
                  button_variant::primary);
        } else if constexpr (std::is_same_v<phase_type, wizard_quality_running> ||
                             std::is_same_v<phase_type, wizard_junk_running> ||
                             std::is_same_v<phase_type, wizard_similar_running> ||
                             std::is_same_v<phase_type, wizard_video_running>) {
          add.add("Cancel", [&controller] { controller.cancel_pass(); });
        } else if constexpr (std::is_same_v<phase_type, wizard_junk_prep>) {
          add.add("Cancel", [&controller] { controller.cancel_junk_prep(); });
        } else if constexpr (std::is_same_v<phase_type, wizard_quality_review> ||
                             std::is_same_v<phase_type, wizard_junk_review> ||
                             std::is_same_v<phase_type, wizard_video_review>) {
          constexpr auto step = std::is_same_v<phase_type, wizard_quality_review>
                                    ? deletion_step::quality
                                : std::is_same_v<phase_type, wizard_junk_review>
                                    ? deletion_step::junk
                                    : deletion_step::video;
          if (current.flagged_count > 0) {
            add.add("Keep all", [&controller] { controller.keep_all_flagged(step); });
            add.add("Mark all", [&controller] { controller.mark_all_flagged(step); });
          }
          add.add("Done", [&controller] { controller.close_batch_review(); },
                  button_variant::primary);
        } else if constexpr (std::is_same_v<phase_type, wizard_similar_review>) {
          add.add("Done", [&controller] { controller.close_batch_review(); },
                  button_variant::primary);
        } else if constexpr (std::is_same_v<phase_type, wizard_batch_menu>) {
          add.add("Edit trips", [&controller] { controller.reopen_organize(); });
          add.add("Continue", [&controller] { controller.continue_from_batches(); },
                  button_variant::primary);
        } else if constexpr (std::is_same_v<phase_type, wizard_deletion_review>) {
          add.add("Back", [&controller] { controller.back_from_deletion_review(); });
          add.add("Continue", [&controller] { controller.continue_from_deletion_review(); },
                  button_variant::primary);
        } else if constexpr (std::is_same_v<phase_type, wizard_commit_summary>) {
          add.add("Back", [&controller] { controller.back_from_commit_summary(); });
          auto &button = add.add("Copy", [&controller] { controller.start_commit(); },
                                 button_variant::primary, commit_summary_screen::can_copy(current));
          if (copy != nullptr) {
            *copy = &button;
          }
        } else if constexpr (std::is_same_v<phase_type, wizard_committing>) {
          add.add("Cancel", [&controller] { controller.cancel_commit(); });
        } else if constexpr (std::is_same_v<phase_type, wizard_done>) {
          add.add("Start over", [&controller] { controller.reset_to_start(); });
          add.add("Done", services.exit_app, button_variant::primary);
        } else if constexpr (std::is_same_v<phase_type, wizard_failed>) {
          add.add("Exit app", services.exit_app, button_variant::tertiary);
          add.add("Back", [&controller] { controller.go_back_from_error(); });
        }
      },
      phase);
  if (!add.any) {
    return nullptr;
  }
  return bar;
}

} // namespace kustavi::gui
