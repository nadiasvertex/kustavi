#include "core/model_prep.h"

#include "proto/service.pb.h"

namespace kustavi::gui {

auto model_prep_from(ModelEvent const &event) -> std::optional<model_prep_state> {
  switch (event.event_case()) {
  case ModelEvent::kReady:
    return model_prep_ready{event.ready().model_name(), event.ready().size_bytes()};
  case ModelEvent::kProgress:
    return model_prep_downloading{event.progress().done_bytes(), event.progress().total_bytes(),
                                  event.progress().speed_bps()};
  case ModelEvent::EVENT_NOT_SET:
    break;
  }
  return std::nullopt;
}

} // namespace kustavi::gui
