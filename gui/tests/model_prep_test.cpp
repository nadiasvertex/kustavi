#include <catch2/catch_test_macros.hpp>

#include "core/model_prep.h"
#include "proto/service.pb.h"

using namespace kustavi::gui;

TEST_CASE("a ready event gives a ready state", "[model_prep]") {
  kustavi::ModelEvent event;
  event.mutable_ready()->set_model_name("Qwen2.5-VL-3B");
  event.mutable_ready()->set_size_bytes(2'900'000'000);
  auto const state = model_prep_from(event);
  REQUIRE(state.has_value());
  auto const *ready = std::get_if<model_prep_ready>(&*state);
  REQUIRE(ready != nullptr);
  CHECK(ready->model_name == "Qwen2.5-VL-3B");
  CHECK(ready->size_bytes == 2'900'000'000);
}

TEST_CASE("a progress event gives a downloading state with a fraction", "[model_prep]") {
  kustavi::ModelEvent event;
  event.mutable_progress()->set_done_bytes(600'000'000);
  event.mutable_progress()->set_total_bytes(1'200'000'000);
  event.mutable_progress()->set_speed_bps(5e6);
  auto const state = model_prep_from(event);
  auto const *downloading = std::get_if<model_prep_downloading>(&*state);
  REQUIRE(downloading != nullptr);
  CHECK(downloading->fraction() == 0.5);
  CHECK(downloading->speed_bytes_per_second == 5e6);
}

TEST_CASE("the fraction is unknown until the total is", "[model_prep]") {
  CHECK_FALSE(model_prep_downloading{100, 0, 0}.fraction().has_value());
}

TEST_CASE("an event with neither case gives no state", "[model_prep]") {
  CHECK_FALSE(model_prep_from(kustavi::ModelEvent{}).has_value());
}
