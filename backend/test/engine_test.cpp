// The engine against a copy of test/photos: every call that the GUI makes, and
// the rules about one pass at a time and cancelling. Each test scans its own
// copy of the photos into a fresh session. The junk pass downloads the vision
// model, so its test is hidden unless it is asked for by tag.

#include "engine.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace kustavi;

namespace {

template <class Event> struct stream_result {
  status result;
  std::vector<Event> events;
};

// Runs a streaming call to its end on this thread. `on_event` sees each event
// with the request's context, which it can cancel.
template <class Event, class Call>
auto run_stream(
    Call &&call,
    std::function<void(const Event &, request_context &)> on_event = {})
    -> stream_result<Event> {
  request_context context;
  stream_result<Event> out;
  event_writer<Event> writer{[&](const Event &event) -> bool {
    out.events.push_back(event);
    if (on_event) {
      on_event(event, context);
    }
    return true;
  }};
  out.result = call(&context, &writer);
  return out;
}

auto unique_directory() -> fs::path {
  std::mt19937_64 random{std::random_device{}()};
  auto const path = fs::temp_directory_path() /
                    ("kustavi-engine-test-" + std::to_string(random()));
  fs::create_directories(path);
  return path;
}

// A scratch copy of the test photos, scanned by an engine.
struct scanned_session {
  fs::path root = unique_directory();
  fs::path photos = root / "photos";
  engine session;
  std::vector<std::string> ids;

  scanned_session() {
    fs::copy(KUSTAVI_TEST_PHOTOS, photos, fs::copy_options::recursive);
    ScanFolderRequest request;
    request.set_folder(photos.string());
    request.set_recursive(true);
    auto const scan = run_stream<ScanEvent>([&](auto *context, auto *writer) {
      return session.scan_folder(context, &request, writer);
    });
    REQUIRE(scan.result.ok());
    for (auto const &event : scan.events) {
      if (event.has_image()) {
        ids.push_back(event.image().id());
      }
    }
    REQUIRE_FALSE(ids.empty());
  }

  ~scanned_session() {
    std::error_code error;
    fs::remove_all(root, error);
  }

  scanned_session(const scanned_session &) = delete;
  auto operator=(const scanned_session &) -> scanned_session & = delete;

  auto quality(std::vector<std::string> scope = {}, std::string batch = {})
      -> stream_result<QualityEvent> {
    RunQualityPassRequest request;
    request.set_blur_threshold(100.0);
    request.set_underexposed_threshold(0.3);
    request.set_overexposed_threshold(0.3);
    request.set_batch_key(std::move(batch));
    for (auto const &id : scope) {
      request.add_scope_image_ids(id);
    }
    return run_stream<QualityEvent>([&](auto *context, auto *writer) {
      return session.run_quality_pass(context, &request, writer);
    });
  }

  auto similar(RunSimilarPassRequest request = {})
      -> stream_result<SimilarEvent> {
    return run_stream<SimilarEvent>([&](auto *context, auto *writer) {
      return session.run_similar_pass(context, &request, writer);
    });
  }

  auto commit(CommitRequest const &request) -> stream_result<CommitEvent> {
    return run_stream<CommitEvent>([&](auto *context, auto *writer) {
      return session.commit(context, &request, writer);
    });
  }

  auto results() -> GetSessionResultsResponse {
    GetSessionResultsRequest request;
    GetSessionResultsResponse response;
    REQUIRE(session.get_session_results(&request, &response).ok());
    return response;
  }
};

auto count_files(fs::path const &directory) -> std::size_t {
  std::size_t count = 0;
  std::error_code error;
  for (auto it = fs::recursive_directory_iterator(directory, error);
       !error && it != fs::recursive_directory_iterator(); ++it) {
    if (it->is_regular_file()) {
      ++count;
    }
  }
  return count;
}

} // namespace

TEST_CASE("get_info reports the version, the formats and the model",
          "[engine]") {
  engine session;
  GetInfoRequest request;
  GetInfoResponse response;
  REQUIRE(session.get_info(&request, &response).ok());
  CHECK_FALSE(response.version().empty());
  CHECK(response.supported_formats_size() > 0);
  CHECK(response.model_name() == k_model_name);
}

TEST_CASE("a call that needs a session fails before one is open", "[engine]") {
  engine session;
  GetSessionResultsRequest request;
  GetSessionResultsResponse response;
  CHECK(session.get_session_results(&request, &response).error_code() ==
        status_code::failed_precondition);
}

TEST_CASE("scan streams every image and reports the count", "[engine][scan]") {
  scanned_session fixture;
  ScanFolderRequest request;
  request.set_folder(fixture.photos.string());
  request.set_recursive(true);
  auto const scan = run_stream<ScanEvent>([&](auto *context, auto *writer) {
    return fixture.session.scan_folder(context, &request, writer);
  });
  REQUIRE(scan.result.ok());
  std::size_t images = 0;
  std::size_t complete = 0;
  for (auto const &event : scan.events) {
    images += event.has_image() ? 1 : 0;
    if (event.has_complete()) {
      complete = event.complete().images();
    }
  }
  CHECK(images > 0);
  CHECK(images == complete);
  CHECK(images == fixture.ids.size());
}

TEST_CASE("the quality pass flags images and counts the total",
          "[engine][quality]") {
  scanned_session fixture;
  auto const run = fixture.quality();
  REQUIRE(run.result.ok());
  std::size_t flags = 0;
  std::uint32_t total = 0;
  for (auto const &event : run.events) {
    flags += event.has_flag() ? 1 : 0;
    if (event.has_complete()) {
      total = event.complete().total();
    }
  }
  CHECK(total == fixture.ids.size());
  CHECK(flags <= total);
}

TEST_CASE("the similar pass groups two or more images and honors skip ids",
          "[engine][similar]") {
  scanned_session fixture;
  auto const groups_of = [](stream_result<SimilarEvent> const &run) {
    std::vector<SimilarGroup> groups;
    std::uint32_t reported = 0;
    for (auto const &event : run.events) {
      if (event.has_group()) {
        groups.push_back(event.group());
      } else if (event.has_complete()) {
        reported = event.complete().groups();
      }
    }
    CHECK(groups.size() == reported);
    return groups;
  };

  auto const first = fixture.similar();
  REQUIRE(first.result.ok());
  auto const groups = groups_of(first);
  for (auto const &group : groups) {
    CHECK(group.image_ids_size() >= 2);
  }
  REQUIRE_FALSE(groups.empty());

  auto const skipped = groups.front().image_ids(0);
  RunSimilarPassRequest request;
  request.add_skip_image_ids(skipped);
  auto const second = fixture.similar(request);
  REQUIRE(second.result.ok());
  for (auto const &group : groups_of(second)) {
    CHECK(group.image_ids_size() >= 2);
    for (auto const &id : group.image_ids()) {
      CHECK(id != skipped);
    }
  }
}

TEST_CASE("the repair and trips passes complete", "[engine][trips]") {
  scanned_session fixture;
  auto const repair = run_stream<RepairEvent>([&](auto *context, auto *writer) {
    RunRepairPassRequest request;
    return fixture.session.run_repair_pass(context, &request, writer);
  });
  REQUIRE(repair.result.ok());
  CHECK(std::ranges::any_of(repair.events,
                            [](auto const &e) { return e.has_complete(); }));

  auto const trips = run_stream<TripsEvent>([&](auto *context, auto *writer) {
    RunTripsPassRequest request;
    return fixture.session.run_trips_pass(context, &request, writer);
  });
  REQUIRE(trips.result.ok());
  std::size_t streamed = 0;
  std::uint32_t reported = 0;
  for (auto const &event : trips.events) {
    streamed += event.has_trip() ? 1 : 0;
    if (event.has_complete()) {
      reported = event.complete().trips();
    }
  }
  CHECK(streamed == reported);
}

TEST_CASE("commit sizes, copies, repeats, merges and routes into folders",
          "[engine][commit]") {
  scanned_session fixture;
  auto const destination = fixture.root / "committed";
  CommitRequest request;
  request.set_destination(destination.string());
  for (auto const &id : fixture.ids) {
    request.add_keep_ids(id);
  }

  SECTION("the estimate for a fresh destination counts every byte as new") {
    EstimateCommitResponse estimate;
    REQUIRE(fixture.session.estimate_commit(&request, &estimate).ok());
    CHECK(estimate.new_bytes() > 0);
    CHECK(estimate.new_bytes() == estimate.total_bytes());
    CHECK(estimate.fits());
  }

  SECTION(
      "a commit copies every kept image, and a second one copies them again") {
    auto const copied_of = [](stream_result<CommitEvent> const &run) {
      std::uint32_t copied = 0;
      std::uint32_t skipped = 0;
      for (auto const &event : run.events) {
        if (event.has_complete()) {
          copied = event.complete().copied();
          skipped = event.complete().skipped();
        }
      }
      CHECK(skipped == 0);
      return copied;
    };
    auto const first = fixture.commit(request);
    REQUIRE(first.result.ok());
    CHECK(copied_of(first) == fixture.ids.size());
    auto const second = fixture.commit(request);
    REQUIRE(second.result.ok());
    CHECK(copied_of(second) == fixture.ids.size());

    SECTION(
        "merging into the library that was written finds every item in place") {
      auto merge = request;
      merge.set_merge_existing(true);
      auto const run = fixture.commit(merge);
      REQUIRE(run.result.ok());
      std::uint32_t present = 0;
      for (auto const &event : run.events) {
        if (event.has_complete()) {
          present =
              event.complete().already_present() + event.complete().copied();
        }
      }
      CHECK(present == fixture.ids.size());
    }
  }

  SECTION("folder_for_id puts every file under its folder") {
    CommitRequest foldered;
    foldered.set_destination((destination / "foldered").string());
    for (auto const &id : fixture.ids) {
      foldered.add_keep_ids(id);
      (*foldered.mutable_folder_for_id())[id] = "trip-a";
    }
    REQUIRE(fixture.commit(foldered).result.ok());
    CHECK(count_files(destination / "foldered" / "trip-a") ==
          fixture.ids.size());
  }
}

TEST_CASE("a second pass is rejected while one runs", "[engine][concurrency]") {
  scanned_session fixture;
  std::mutex mutex;
  std::condition_variable running_changed;
  bool running = false;
  bool release = false;

  // The quality pass blocks inside its first event until the second pass has
  // been tried.
  std::thread first{[&] {
    RunQualityPassRequest request;
    request.set_blur_threshold(100.0);
    request.set_underexposed_threshold(0.3);
    request.set_overexposed_threshold(0.3);
    request_context context;
    event_writer<QualityEvent> writer{[&](const QualityEvent &) -> bool {
      std::unique_lock lock{mutex};
      running = true;
      running_changed.notify_all();
      running_changed.wait(lock, [&] { return release; });
      return true;
    }};
    CHECK(fixture.session.run_quality_pass(&context, &request, &writer).ok());
  }};
  {
    std::unique_lock lock{mutex};
    running_changed.wait(lock, [&] { return running; });
  }
  auto const rejected = fixture.similar();
  {
    std::scoped_lock lock{mutex};
    release = true;
  }
  running_changed.notify_all();
  first.join();

  CHECK(rejected.result.error_code() == status_code::failed_precondition);
  CHECK(rejected.events.empty());
}

TEST_CASE("cancelling a scan ends it and frees the engine for the next pass",
          "[engine][cancel]") {
  scanned_session fixture;
  ScanFolderRequest request;
  request.set_folder(fixture.photos.string());
  request.set_recursive(true);
  auto const start = std::chrono::steady_clock::now();
  auto const cancelled = run_stream<ScanEvent>(
      [&](auto *context, auto *writer) {
        return fixture.session.scan_folder(context, &request, writer);
      },
      [](ScanEvent const &, request_context &context) { context.cancel(); });
  CHECK(cancelled.result.error_code() == status_code::cancelled);
  CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds{10});

  auto const again = run_stream<ScanEvent>([&](auto *context, auto *writer) {
    return fixture.session.scan_folder(context, &request, writer);
  });
  REQUIRE(again.result.ok());
  CHECK(std::ranges::any_of(again.events,
                            [](auto const &e) { return e.has_image(); }));
}

TEST_CASE("a saved session resumes with its decisions and step",
          "[engine][resume]") {
  scanned_session fixture;
  REQUIRE(fixture.quality().result.ok());
  REQUIRE(fixture.similar().result.ok());
  REQUIRE(fixture.ids.size() >= 2);

  SaveSessionStateRequest save;
  save.set_step(4);
  save.set_trip_edits(R"({"v":1,"membership":{"a":1}})");
  save.set_replace_decisions(true);
  auto *keep = save.add_decisions();
  keep->set_image_id(fixture.ids[0]);
  keep->set_decision(Decision::KEEP);
  auto *drop = save.add_decisions();
  drop->set_image_id(fixture.ids[1]);
  drop->set_decision(Decision::DELETE);
  SaveSessionStateResponse saved;
  REQUIRE(fixture.session.save_session_state(&save, &saved).ok());

  InspectSessionRequest inspect;
  inspect.set_folder(fixture.photos.string());
  InspectSessionResponse inspected;
  REQUIRE(fixture.session.inspect_session(&inspect, &inspected).ok());
  CHECK(inspected.has_session());
  CHECK(inspected.resume_step() == 4);
  CHECK(inspected.image_count() == fixture.ids.size());

  ScanFolderRequest resume;
  resume.set_folder(fixture.photos.string());
  resume.set_recursive(true);
  resume.set_resume(true);
  auto const rescan = run_stream<ScanEvent>([&](auto *context, auto *writer) {
    return fixture.session.scan_folder(context, &resume, writer);
  });
  REQUIRE(rescan.result.ok());
  std::vector<std::string> replayed;
  for (auto const &event : rescan.events) {
    if (event.has_image()) {
      replayed.push_back(event.image().id());
    } else if (event.has_complete()) {
      CHECK(event.complete().resumed());
      CHECK(event.complete().resume_step() == 4);
    }
  }
  CHECK(replayed == fixture.ids);

  auto const results = fixture.results();
  CHECK(results.decisions_size() == 2);
  CHECK(results.quality_done());
  CHECK(results.similar_done());
  CHECK_FALSE(results.junk_done());
  CHECK_FALSE(results.video_done());
  CHECK(results.trip_edits() == R"({"v":1,"membership":{"a":1}})");
}

TEST_CASE("a pass scoped to a batch looks at that batch and records it",
          "[engine][batch]") {
  scanned_session fixture;
  REQUIRE(fixture.ids.size() >= 2);
  auto const half = fixture.ids.size() / 2;
  std::vector<std::string> const scope{fixture.ids.begin(),
                                       fixture.ids.begin() +
                                           static_cast<std::ptrdiff_t>(half)};
  auto const in_scope = [&](std::string const &id) {
    return std::ranges::find(scope, id) != scope.end();
  };

  auto const quality = fixture.quality(scope, "test-batch");
  REQUIRE(quality.result.ok());
  std::uint32_t analyzed = 0;
  for (auto const &event : quality.events) {
    if (event.has_complete()) {
      analyzed = event.complete().total();
    }
  }
  CHECK(analyzed > 0);
  CHECK(analyzed <= half);

  RunSimilarPassRequest similar;
  similar.set_batch_key("test-batch");
  for (auto const &id : scope) {
    similar.add_scope_image_ids(id);
  }
  auto const grouped = fixture.similar(similar);
  REQUIRE(grouped.result.ok());
  for (auto const &event : grouped.events) {
    if (event.has_group()) {
      for (auto const &id : event.group().image_ids()) {
        CHECK(in_scope(id));
      }
    }
  }

  auto const results = fixture.results();
  auto const has = [&](std::string const &entry) {
    return std::ranges::find(results.completed_batch_passes(), entry) !=
           results.completed_batch_passes().end();
  };
  CHECK(has("1:test-batch"));
  CHECK(has("2:test-batch"));
}

TEST_CASE("previewing thresholds counts the stored metrics",
          "[engine][quality]") {
  scanned_session fixture;
  REQUIRE(fixture.quality().result.ok());

  PreviewQualityThresholdsRequest request;
  auto *thresholds = request.mutable_thresholds();
  thresholds->set_blur_threshold(100.0);
  thresholds->set_underexposed_threshold(0.3);
  thresholds->set_overexposed_threshold(0.3);
  PreviewQualityThresholdsResponse strict;
  REQUIRE(fixture.session.preview_quality_thresholds(&request, &strict).ok());
  CHECK(strict.total() > 0);
  CHECK(strict.flagged() <= strict.total());

  thresholds->set_blur_threshold(1.0);
  thresholds->set_underexposed_threshold(0.99);
  thresholds->set_overexposed_threshold(0.99);
  PreviewQualityThresholdsResponse loose;
  REQUIRE(fixture.session.preview_quality_thresholds(&request, &loose).ok());
  CHECK(loose.flagged() <= strict.flagged());
}

// Downloads the vision model, about 3.3 GB on the first run, so `ctest -L junk`
// asks for it.
TEST_CASE("the junk pass classifies the scanned images", "[.][junk]") {
  scanned_session fixture;
  EnsureModelRequest ensure;
  auto const model = run_stream<ModelEvent>([&](auto *context, auto *writer) {
    return fixture.session.ensure_model(context, &ensure, writer);
  });
  REQUIRE(model.result.ok());
  CHECK(std::ranges::any_of(model.events,
                            [](auto const &e) { return e.has_ready(); }));

  RunJunkPassRequest request;
  auto const junk = run_stream<JunkEvent>([&](auto *context, auto *writer) {
    return fixture.session.run_junk_pass(context, &request, writer);
  });
  REQUIRE(junk.result.ok());
  std::size_t flags = 0;
  std::uint32_t reported = 0;
  bool complete = false;
  for (auto const &event : junk.events) {
    if (event.has_flag()) {
      ++flags;
      CHECK_FALSE(event.flag().image_id().empty());
    } else if (event.has_complete()) {
      complete = true;
      reported = event.complete().flagged();
    }
  }
  CHECK(complete);
  CHECK(flags == reported);
}
