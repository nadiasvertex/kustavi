#include "app/wizard_shell.h"

#include "app/ui_kit.h"

#include <algorithm>

namespace kustavi::gui {

wizard_shell::wizard_shell(basalt::widget_tree &tree, wizard_controller &controller,
                           model_status &model, image_loader &loader, shell_services services)
    : tree_(tree), controller_(controller), model_(model), loader_(loader),
      services_(std::move(services)) {
  host_ = &tree_.emplace_root<basalt::overlay_host>();
  auto frame = ui::column(0);
  frame->set_cross_alignment(basalt::alignment::stretch);
  frame_ = &host_->set_content(std::move(frame));
}

wizard_shell::~wizard_shell() {
  // The grid refers to a widget in the tree, and its cells refer to the grid.
  grid_.reset();
}

auto wizard_shell::mark_dirty() -> void { dirty_ = true; }

auto wizard_shell::update() -> void {
  if (dirty_) {
    rebuild();
  }
  if (grid_) {
    grid_->apply_ready_images();
  }
  if (busy_ != nullptr) {
    busy_->tick(0.02F);
  }
}

auto wizard_shell::rebuild() -> void {
  dirty_ = false;
  // The grid goes before the widgets that show it.
  grid_.reset();
  busy_ = nullptr;
  frame_->clear_children();
  auto const &phase = controller_.phase();
  frame_->add_child(build_step_indicator(stage_index_of(phase)));
  ui::fill(*frame_, build_body());
  if (auto bar = build_action_bar(phase, controller_, services_)) {
    frame_->add_child(std::move(bar));
  }
}

auto wizard_shell::build_body() -> std::unique_ptr<basalt::widget> {
  auto const &phase = controller_.phase();
  struct visitor {
    wizard_shell &shell;

    auto operator()(wizard_start const &) const -> std::unique_ptr<basalt::widget> {
      return build_start_screen(shell.model_.state(), shell.services_);
    }
    auto operator()(wizard_scanning const &scanning) const -> std::unique_ptr<basalt::widget> {
      return build_scanning_screen(scanning, shell.busy_);
    }
    auto operator()(wizard_session_restore const &restore) const
        -> std::unique_ptr<basalt::widget> {
      return build_session_restore_screen(restore, shell.controller_);
    }
    auto operator()(wizard_confirm_folder const &confirm) const -> std::unique_ptr<basalt::widget> {
      std::vector<image_info> images;
      images.reserve(shell.controller_.image_count());
      for (auto const &id : shell.controller_.image_ids()) {
        images.push_back(*shell.controller_.image(id));
      }
      shell.grid_ = std::make_unique<image_grid>(std::move(images), shell.loader_);
      return build_confirm_screen(confirm, *shell.grid_);
    }
    auto operator()(wizard_no_images const &none) const -> std::unique_ptr<basalt::widget> {
      return build_no_images_screen(none);
    }
    auto operator()(wizard_failed const &failed) const -> std::unique_ptr<basalt::widget> {
      return build_failed_screen(failed, shell.services_);
    }
  };
  return std::visit(visitor{*this}, phase);
}

auto wizard_shell::show_crash_dialog(std::string message, std::function<void()> on_retry)
    -> void {
  auto crash = std::make_unique<basalt::dialog>(
      "Processing error", std::move(message),
      std::vector<basalt::dialog_button>{{"Exit app", basalt::button_variant::secondary},
                                         {"Retry", basalt::button_variant::primary}});
  auto *const raw = crash.get();
  auto const id = host_->show_dialog(std::move(crash));
  raw->set_on_choice([this, id, on_retry = std::move(on_retry)](std::size_t choice) {
    host_->dismiss(id);
    if (choice == 1) {
      on_retry();
    } else {
      services_.exit_app();
    }
  });
}

auto wizard_shell::show_log_dialog(std::string const &log) -> void {
  // The dialog wraps a message, so a long log shows its last lines.
  constexpr std::size_t shown_lines = 30;
  std::vector<std::string_view> lines;
  for (std::size_t start = 0; start < log.size();) {
    auto const end = std::min(log.find('\n', start), log.size());
    lines.emplace_back(log.data() + start, end - start);
    start = end + 1;
  }
  std::string text;
  for (auto line = lines.size() > shown_lines ? lines.end() - shown_lines : lines.begin();
       line != lines.end(); ++line) {
    text.append(*line).push_back('\n');
  }
  if (text.empty()) {
    text = "The back end has not written anything yet.";
  }
  auto dialog = std::make_unique<basalt::dialog>(
      "Back-end log", std::move(text),
      std::vector<basalt::dialog_button>{{"Close", basalt::button_variant::primary}}, 0);
  auto *const raw = dialog.get();
  auto const id = host_->show_dialog(std::move(dialog));
  raw->set_on_choice([this, id](std::size_t) { host_->dismiss(id); });
}

} // namespace kustavi::gui
