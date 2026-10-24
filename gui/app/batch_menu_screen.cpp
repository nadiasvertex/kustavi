#include "app/batch_menu_screen.h"

#include "app/ui_kit.h"
#include "core/format.h"

#include <array>
#include <format>

namespace kustavi::gui {

namespace {

using basalt::button_variant;
using basalt::text_role;
using basalt::text_tone;

struct pass_info {
  wizard_step step;
  char const *label;
  char const *hint;
};

// The passes in their suggested order, with what each costs.
constexpr std::array passes{
    pass_info{wizard_step::quality, "Quality", "Blurry and badly exposed photos · fast"},
    pass_info{wizard_step::duplicates, "Duplicates", "Near-identical photos · fast"},
    pass_info{wizard_step::junk, "Junk", "Screenshots, scans and memes · slow, about 2 s per photo"},
    pass_info{wizard_step::video, "Video", "Short, corrupt, blurry or static clips"},
};

auto batch_row_label(batch_summary const &batch) -> std::string {
  auto label = std::format("{}  ·  {} photos", batch.title, format_int(batch.photo_count));
  if (batch.video_count > 0) {
    label += std::format(" · {} videos", format_int(batch.video_count));
  }
  auto const done = std::ranges::count_if(batch.passes, [](auto const &entry) {
    return entry.second.done;
  });
  if (done > 0) {
    label += std::format("  ·  {} done", done);
  }
  return label;
}

auto make_pass_card(pass_info const &info, batch_pass_status const &status,
                    wizard_controller &controller) -> std::unique_ptr<basalt::widget> {
  auto card = ui::row(12);
  card->set_padding(basalt::length_insets::uniform(basalt::fixed(16)));
  card->set_surface(basalt::box_surface{});
  auto text = ui::column(2);
  text->add_child(ui::text(info.label, text_role::body_strong));
  text->add_child(ui::text(info.hint, text_role::small, text_tone::subdued));
  text->add_child(ui::text(pass_summary(info.step, status), text_role::body,
                           status.done ? text_tone::interactive : text_tone::normal));
  ui::fill(*card, std::move(text));
  auto const step = info.step;
  if (status.done) {
    card->add_child(ui::make_button("Review", [&controller, step] {
      controller.review_batch_pass(step);
    }));
    card->add_child(ui::make_button(
        "Run again", [&controller, step] { controller.start_batch_pass(step); },
        button_variant::tertiary));
  } else {
    card->add_child(ui::make_button(
        status.flagged > 0 ? "Finish" : "Run",
        [&controller, step] { controller.start_batch_pass(step); }, button_variant::primary));
    card->add_child(ui::make_button(
        "Run on all batches", [&controller, step] { controller.start_pass_on_all_batches(step); },
        button_variant::tertiary));
  }
  return card;
}

} // namespace

auto pass_summary(wizard_step step, batch_pass_status const &status) -> std::string {
  auto const unit = step == wizard_step::duplicates ? "groups" : "flagged";
  if (status.done) {
    return std::format("Done · {} {}", status.flagged, unit);
  }
  if (status.flagged > 0) {
    return std::format("Partly run · {} {}", status.flagged, unit);
  }
  return "Not run";
}

auto build_batch_menu_screen(wizard_batch_menu const &menu, wizard_controller &controller)
    -> std::unique_ptr<basalt::widget> {
  auto screen = ui::column(8);
  screen->set_padding(basalt::length_insets{basalt::fixed(8), basalt::fixed(16), basalt::fixed(0),
                                            basalt::fixed(16)});
  screen->add_child(ui::text(std::format("{} batches · {} of {} photos marked for deletion",
                                         menu.batches.size(), format_int(menu.marked_count),
                                         format_int(menu.total_images)),
                             text_role::heading_small));
  screen->add_child(ui::text("Run any passes on a batch, in any order. The fast passes (quality, "
                             "duplicates) are worth running first: the slow junk pass skips "
                             "photos already marked for deletion.",
                             text_role::small, text_tone::subdued));

  auto body = ui::row(16);
  body->set_cross_alignment(basalt::alignment::stretch);

  auto list = ui::column(2);
  for (auto const &batch : menu.batches) {
    auto const selected = batch.key == menu.selected_key;
    auto &row = list->add_child(ui::make_button(
        batch_row_label(batch), [&controller, key = batch.key] { controller.select_batch(key); },
        selected ? button_variant::secondary : button_variant::tertiary));
    row.set_label_alignment(basalt::alignment::start);
  }
  auto list_scroll = std::make_unique<basalt::scroll_view>();
  list_scroll->set_content(std::move(list));
  body->add(std::move(list_scroll), basalt::box_item{.basis = basalt::fixed(360)});

  auto detail = ui::column(12);
  auto const selected = std::ranges::find(menu.batches, menu.selected_key, &batch_summary::key);
  if (selected == menu.batches.end()) {
    detail->add_child(ui::text("No photos to review"));
  } else {
    detail->add_child(ui::text(selected->title, text_role::body_strong));
    for (auto const &info : passes) {
      // A batch with no videos has nothing for the video pass to do.
      if (info.step == wizard_step::video && selected->video_count == 0) {
        continue;
      }
      auto const status = selected->passes.find(index_of(info.step));
      detail->add_child(make_pass_card(
          info, status == selected->passes.end() ? batch_pass_status{} : status->second,
          controller));
    }
  }
  auto detail_scroll = std::make_unique<basalt::scroll_view>();
  detail_scroll->set_content(std::move(detail));
  ui::fill(*body, std::move(detail_scroll));
  ui::fill(*screen, std::move(body));
  return screen;
}

} // namespace kustavi::gui
