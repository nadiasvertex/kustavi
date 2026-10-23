#include <catch2/catch_test_macros.hpp>

#include "core/phases.h"

using namespace kustavi::gui;

TEST_CASE("every phase of the select stage reports the first step", "[phases]") {
  CHECK(step_index_of(wizard_start{}) == 0);
  CHECK(step_index_of(wizard_scanning{}) == 0);
  CHECK(step_index_of(wizard_session_restore{}) == 0);
  CHECK(step_index_of(wizard_confirm_folder{}) == 0);
  CHECK(step_index_of(wizard_no_images{}) == 0);
}

TEST_CASE("a failure reports the step where it happened", "[phases]") {
  CHECK(step_index_of(wizard_failed{backend_crashed{"x"}, 3}) == 3);
  CHECK(stage_index_of(wizard_failed{backend_crashed{"x"}, 3}) ==
        static_cast<int>(wizard_stage::review));
}

TEST_CASE("steps map to the indicator stages", "[phases]") {
  CHECK(stage_index_of_step(0) == static_cast<int>(wizard_stage::select));
  for (int step = 1; step <= 4; ++step) {
    CHECK(stage_index_of_step(step) == static_cast<int>(wizard_stage::review));
  }
  CHECK(stage_index_of_step(5) == static_cast<int>(wizard_stage::organize));
  CHECK(stage_index_of_step(6) == static_cast<int>(wizard_stage::copy));
  CHECK(stage_index_of_step(7) == wizard_stage_count);
  CHECK(stage_index_of_step(-1) == static_cast<int>(wizard_stage::select));
}

TEST_CASE("stage labels", "[phases]") {
  CHECK(label_of(wizard_stage::organize) == "Organize");
  CHECK(label_of(wizard_stage::copy) == "Copy");
}
