#pragma once

#include "core/trip_edits.h"
#include "core/wizard_controller.h"
#include "testing/fake_kustavi_client.h"

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <optional>
#include <string>
#include <vector>

// A controller over a fake back end with the helpers that its tests share.
namespace kustavi::gui::testing {

using namespace kustavi::gui;



inline auto scan_image(std::string const &id) -> kustavi::ScanEvent {
  kustavi::ScanEvent event;
  auto *meta = event.mutable_image();
  meta->set_id(id);
  meta->set_name(id);
  meta->set_thumbnail_path("/cache/" + id);
  return event;
}

inline auto scan_complete(std::uint32_t images, std::vector<std::string> const &errors = {})
    -> kustavi::ScanEvent {
  kustavi::ScanEvent event;
  event.mutable_complete()->set_images(images);
  for (auto const &error : errors) {
    event.mutable_complete()->add_errors(error);
  }
  return event;
}

inline auto scan_progress(std::uint32_t seen, std::uint32_t found) -> kustavi::ScanEvent {
  kustavi::ScanEvent event;
  event.mutable_progress()->set_files_seen(seen);
  event.mutable_progress()->set_images_found(found);
  event.mutable_progress()->set_current_path("/photos/x");
  return event;
}

struct harness {
  harness() : controller(queue, inline_executor()) {
    controller.set_client(client);
    controller.on_change([this] { ++changes; });
  }

  // Runs what the workers posted, and what that posted, until the queue is quiet.
  auto settle() -> void {
    while (queue.pending()) {
      queue.drain();
    }
  }

  template <class Phase> [[nodiscard]] auto in() const -> bool {
    return std::holds_alternative<Phase>(controller.phase());
  }
  template <class Phase> [[nodiscard]] auto get() const -> Phase const & {
    return std::get<Phase>(controller.phase());
  }

  ui_queue queue;
  std::shared_ptr<fake_kustavi_client> client = std::make_shared<fake_kustavi_client>();
  wizard_controller controller;
  int changes = 0;
};

inline auto reach_confirm(harness &h, std::string const &folder = "/photos") -> void {
  h.controller.select_folder(folder);
  h.settle();
  REQUIRE(h.in<wizard_confirm_folder>());
}




inline auto trip_event(std::uint32_t id, std::int64_t start_ms, std::vector<std::string> const &ids,
                std::string const &folder) -> kustavi::TripsEvent {
  kustavi::TripsEvent event;
  auto *trip = event.mutable_trip();
  trip->set_id(id);
  trip->set_start_unix_ms(start_ms);
  trip->set_end_unix_ms(start_ms + 1000);
  for (auto const &image : ids) {
    trip->add_image_ids(image);
  }
  trip->set_folder(folder);
  trip->set_folder_slug(folder);
  return event;
}

inline auto trips_progress(std::uint32_t done, std::uint32_t total) -> kustavi::TripsEvent {
  kustavi::TripsEvent event;
  event.mutable_progress()->set_done(done);
  event.mutable_progress()->set_total(total);
  return event;
}

// Four photos and two trips: a and b in "Rome", c in "Paris", and d in no trip.
inline auto reach_trips_review(harness &h) -> void {
  h.client->scan_events = {scan_image("a.jpg"), scan_image("b.jpg"), scan_image("c.jpg"),
                           scan_image("d.jpg"), scan_complete(4)};
  h.client->trips_events = {trip_event(1, 1000, {"a.jpg", "b.jpg"}, "Rome"),
                            trip_event(2, 2000, {"c.jpg"}, "Paris")};
  reach_confirm(h);
  h.controller.continue_from_confirm();
  h.settle();
  REQUIRE(h.in<wizard_trips_review>());
}

inline auto saved_edits(harness &h) -> std::optional<trip_edits> {
  for (auto it = h.client->saved_session_states.rbegin(); it != h.client->saved_session_states.rend();
       ++it) {
    if (it->has_trip_edits()) {
      return trip_edits::decode(it->trip_edits());
    }
  }
  return std::nullopt;
}


} // namespace kustavi::gui::testing
