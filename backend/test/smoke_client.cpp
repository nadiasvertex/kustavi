#include <grpcpp/grpcpp.h>

#include <proto/service.grpc.pb.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <print>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
namespace k = kustavi;

namespace {

constexpr int k_fail = 1;

[[noreturn]] void fail(const std::string &message) {
  std::println(stderr, "FAIL: {}", message);
  std::exit(k_fail);
}

struct options {
  std::string target;
  std::string token;
  std::string folder;
  std::string destination;
  bool no_auth = false;
  bool shutdown = false;
  bool cancel_check = false;
  bool concurrency_check = false;
};

auto parse_args(int argc, char **argv) -> options {
  std::vector<std::string> args;
  if (argc > 1) {
    args.reserve(static_cast<std::size_t>(argc - 1));
  }
  for (int i = 1; i < argc; ++i) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic) argv
    args.emplace_back(argv[i]);
  }

  options opts;
  opts.target = "127.0.0.1:1";
  for (std::size_t i = 0; i < args.size(); ++i) {
    const auto &arg = args[i];
    if (arg == "--target" && i + 1 < args.size()) {
      opts.target = args[++i];
    } else if (arg == "--token" && i + 1 < args.size()) {
      opts.token = args[++i];
    } else if (arg == "--folder" && i + 1 < args.size()) {
      opts.folder = args[++i];
    } else if (arg == "--destination" && i + 1 < args.size()) {
      opts.destination = args[++i];
    } else if (arg == "--no-auth") {
      opts.no_auth = true;
    } else if (arg == "--shutdown") {
      opts.shutdown = true;
    } else if (arg == "--cancel-check") {
      opts.cancel_check = true;
    } else if (arg == "--concurrency-check") {
      opts.concurrency_check = true;
    } else {
      fail("unknown argument: " + arg);
    }
  }
  return opts;
}

auto make_channel(const options &opts) -> std::shared_ptr<grpc::Channel> {
  return grpc::CreateChannel(opts.target, grpc::InsecureChannelCredentials());
}

void add_auth_metadata(grpc::ClientContext &context, const options &opts) {
  if (!opts.no_auth) {
    context.AddMetadata("x-kustavi-auth-token", opts.token);
  }
}

void check_get_info(const options &opts, const bool expect_auth_failure) {
  auto stub = k::Kustavi::NewStub(make_channel(opts));
  k::GetInfoRequest request;
  k::GetInfoResponse response;
  grpc::ClientContext context;
  if (!expect_auth_failure) {
    add_auth_metadata(context, opts);
  }
  const auto status = stub->GetInfo(&context, request, &response);
  if (expect_auth_failure) {
    if (status.error_code() != grpc::StatusCode::UNAUTHENTICATED) {
      fail("GetInfo without token: expected UNAUTHENTICATED, got " +
           status.error_message());
    }
    std::println("ok: GetInfo without token -> UNAUTHENTICATED");
    return;
  }
  if (!status.ok()) {
    fail("GetInfo: " + status.error_message());
  }
  std::println("ok: GetInfo version={} formats={} model={}", response.version(),
               response.supported_formats_size(), response.model_name());
}

std::vector<std::string> g_image_ids;

void run_scan(const options &opts) {
  auto stub = k::Kustavi::NewStub(make_channel(opts));
  k::ScanFolderRequest request;
  request.set_folder(opts.folder);
  request.set_recursive(true);
  grpc::ClientContext context;
  add_auth_metadata(context, opts);
  auto reader = stub->ScanFolder(&context, request);
  k::ScanEvent event;
  std::size_t images = 0;
  std::size_t progress_events = 0;
  uint32_t complete_images = 0;
  while (reader->Read(&event)) {
    if (event.has_image()) {
      images++;
      g_image_ids.push_back(event.image().id());
    } else if (event.has_progress()) {
      progress_events++;
    } else if (event.has_complete()) {
      complete_images = event.complete().images();
    }
  }
  const auto status = reader->Finish();
  if (!status.ok()) {
    fail("ScanFolder: " + status.error_message());
  }
  if (images == 0 || images != complete_images) {
    fail("ScanFolder: streamed " + std::to_string(images) +
         " images but complete reported " + std::to_string(complete_images));
  }
  std::println("ok: ScanFolder images={} progress_events={}", images,
               progress_events);
}

void run_quality(const options &opts) {
  auto stub = k::Kustavi::NewStub(make_channel(opts));
  k::RunQualityPassRequest request;
  grpc::ClientContext context;
  add_auth_metadata(context, opts);
  auto reader = stub->RunQualityPass(&context, request);
  k::QualityEvent event;
  std::size_t flags = 0;
  uint32_t total = 0;
  while (reader->Read(&event)) {
    if (event.has_flag()) {
      flags++;
    } else if (event.has_complete()) {
      total = event.complete().total();
    }
  }
  const auto status = reader->Finish();
  if (!status.ok()) {
    fail("RunQualityPass: " + status.error_message());
  }
  std::println("ok: RunQualityPass flagged={} total={}", flags, total);
}

void run_similar(const options &opts) {
  auto stub = k::Kustavi::NewStub(make_channel(opts));
  k::RunSimilarPassRequest request;
  grpc::ClientContext context;
  add_auth_metadata(context, opts);
  auto reader = stub->RunSimilarPass(&context, request);
  k::SimilarEvent event;
  std::size_t groups = 0;
  uint32_t complete_groups = 0;
  while (reader->Read(&event)) {
    if (event.has_group()) {
      groups++;
      if (event.group().image_ids_size() < 2) {
        fail("similar group with fewer than 2 members");
      }
    } else if (event.has_complete()) {
      complete_groups = event.complete().groups();
    }
  }
  const auto status = reader->Finish();
  if (!status.ok()) {
    fail("RunSimilarPass: " + status.error_message());
  }
  if (groups != complete_groups) {
    fail("similar group count mismatch: streamed " + std::to_string(groups) +
         " vs complete " + std::to_string(complete_groups));
  }
  std::println("ok: RunSimilarPass groups={}", groups);
}

void run_trips(const options &opts) {
  auto stub = k::Kustavi::NewStub(make_channel(opts));
  k::RunTripsPassRequest request;
  grpc::ClientContext context;
  add_auth_metadata(context, opts);
  auto reader = stub->RunTripsPass(&context, request);
  k::TripsEvent event;
  std::size_t trips = 0;
  uint32_t complete_trips = 0;
  while (reader->Read(&event)) {
    if (event.has_trip()) {
      trips++;
    } else if (event.has_complete()) {
      complete_trips = event.complete().trips();
    }
  }
  const auto status = reader->Finish();
  if (!status.ok()) {
    fail("RunTripsPass: " + status.error_message());
  }
  if (trips != complete_trips) {
    fail("trips count mismatch: streamed " + std::to_string(trips) +
         " vs complete " + std::to_string(complete_trips));
  }
  std::println("ok: RunTripsPass trips={}", trips);
}

void run_commit(const options &opts) {
  auto stub = k::Kustavi::NewStub(make_channel(opts));
  k::CommitRequest request;
  request.set_destination(opts.destination);
  for (const auto &id : g_image_ids) {
    request.add_keep_ids(id);
  }
  grpc::ClientContext context;
  add_auth_metadata(context, opts);
  auto reader = stub->Commit(&context, request);
  k::CommitEvent event;
  uint32_t copied = 0;
  uint32_t skipped = 0;
  while (reader->Read(&event)) {
    if (event.has_complete()) {
      copied = event.complete().copied();
      skipped = event.complete().skipped();
    }
  }
  const auto status = reader->Finish();
  if (!status.ok()) {
    fail("Commit: " + status.error_message());
  }
  if (copied != static_cast<uint32_t>(g_image_ids.size()) || skipped != 0) {
    fail("Commit copied " + std::to_string(copied) + " skipped " +
         std::to_string(skipped) + " of " + std::to_string(g_image_ids.size()));
  }

  // Idempotency: re-committing the same set (identical sizes) still copies.
  grpc::ClientContext second_context;
  add_auth_metadata(second_context, opts);
  auto second_reader = stub->Commit(&second_context, request);
  k::CommitEvent second_event;
  uint32_t second_copied = 0;
  while (second_reader->Read(&second_event)) {
    if (second_event.has_complete()) {
      second_copied = second_event.complete().copied();
    }
  }
  const auto second_status = second_reader->Finish();
  if (!second_status.ok()) {
    fail("Commit re-run: " + second_status.error_message());
  }
  if (second_copied != copied) {
    fail("Commit re-run copied " + std::to_string(second_copied) + " vs " +
         std::to_string(copied));
  }
  std::println("ok: Commit copied={} (re-run consistent)", copied);
}

void run_concurrency_check(const options &opts) {
  auto stub = k::Kustavi::NewStub(make_channel(opts));
  k::RunQualityPassRequest q_request;
  grpc::ClientContext q_context;
  add_auth_metadata(q_context, opts);
  auto q_reader = stub->RunQualityPass(&q_context, q_request);
  k::QualityEvent q_event;
  if (!q_reader->Read(&q_event)) {
    fail("concurrency check: quality stream ended immediately");
  }

  // The quality pass is now running; a second pass must be rejected.
  k::RunSimilarPassRequest s_request;
  grpc::ClientContext s_context;
  add_auth_metadata(s_context, opts);
  auto s_reader = stub->RunSimilarPass(&s_context, s_request);
  k::SimilarEvent s_event;
  const bool more = s_reader->Read(&s_event);
  const auto s_status = s_reader->Finish();
  if (more || s_status.error_code() != grpc::StatusCode::FAILED_PRECONDITION) {
    fail("concurrency check: second pass was not rejected (status " +
         std::to_string(static_cast<int>(s_status.error_code())) + ")");
  }

  // Drain the quality stream to completion.
  while (q_reader->Read(&q_event)) {
  }
  if (!q_reader->Finish().ok()) {
    fail("concurrency check: quality pass did not complete cleanly");
  }
  std::println("ok: concurrent pass rejected with FAILED_PRECONDITION");
}

void run_cancel_check(const options &opts) {
  auto stub = k::Kustavi::NewStub(make_channel(opts));
  k::ScanFolderRequest request;
  request.set_folder(opts.folder);
  request.set_recursive(true);
  grpc::ClientContext context;
  add_auth_metadata(context, opts);
  auto reader = stub->ScanFolder(&context, request);
  k::ScanEvent event;
  // Read one event (the pass is now in flight), then cancel the stream.
  if (!reader->Read(&event)) {
    fail("cancel check: scan stream ended before we could cancel");
  }
  context.TryCancel();
  const auto start = std::chrono::steady_clock::now();
  while (reader->Read(&event)) {
  }
  const auto elapsed = std::chrono::steady_clock::now() - start;
  const auto status = reader->Finish();
  if (std::chrono::duration_cast<std::chrono::seconds>(elapsed).count() > 10) {
    fail("cancel check: server took too long to wind down the pass");
  }
  std::println(
      "ok: cancelled scan wound down in {} ms (finish={})",
      static_cast<long long>(
          std::chrono::duration_cast<std::chrono::milliseconds>(elapsed)
              .count()),
      static_cast<int>(status.error_code()));

  // The pass lock must be released: a fresh scan works. Retry briefly while
  // the cancelled pass finishes winding down on the server.
  for (int attempt = 0; attempt < 200; ++attempt) {
    k::ScanFolderRequest retry_request;
    retry_request.set_folder(opts.folder);
    retry_request.set_recursive(true);
    grpc::ClientContext retry_context;
    add_auth_metadata(retry_context, opts);
    auto retry_reader = stub->ScanFolder(&retry_context, retry_request);
    k::ScanEvent retry_event;
    std::size_t images = 0;
    while (retry_reader->Read(&retry_event)) {
      if (retry_event.has_image()) {
        images++;
      }
    }
    const auto retry_status = retry_reader->Finish();
    if (retry_status.error_code() == grpc::StatusCode::FAILED_PRECONDITION) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      continue;
    }
    if (!retry_status.ok()) {
      fail("cancel check: post-cancel scan failed: " +
           retry_status.error_message());
    }
    if (images == 0) {
      fail("cancel check: post-cancel scan returned no images");
    }
    std::println("ok: post-cancel scan works ({} images, attempt {})", images,
                 attempt + 1);
    return;
  }
  fail("cancel check: pass lock never released");
}

void run_shutdown(const options &opts) {
  auto stub = k::Kustavi::NewStub(make_channel(opts));
  k::ShutdownRequest request;
  k::ShutdownResponse response;
  grpc::ClientContext context;
  add_auth_metadata(context, opts);
  const auto status = stub->Shutdown(&context, request, &response);
  if (!status.ok()) {
    fail("Shutdown: " + status.error_message());
  }
  // The server must stop accepting connections shortly after Shutdown.
  for (int i = 0; i < 50; ++i) {
    grpc::ClientContext probe_context;
    k::GetInfoRequest probe_request;
    k::GetInfoResponse probe_response;
    const auto probe =
        stub->GetInfo(&probe_context, probe_request, &probe_response);
    if (probe.error_code() != grpc::StatusCode::OK) {
      std::println("ok: Shutdown (server stopped)");
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  fail("Shutdown: server still accepting requests");
}

} // namespace

auto main(int argc, char **argv) -> int {
  try {
    const auto opts = parse_args(argc, argv);
    if (!opts.token.empty()) {
      check_get_info(opts, true);
    }
    check_get_info(opts, false);
    if (opts.folder.empty()) {
      std::println("ok: no --folder given; skipping pass checks");
      return 0;
    }
    run_scan(opts);
    run_quality(opts);
    run_similar(opts);
    run_trips(opts);
    if (!opts.destination.empty()) {
      std::error_code ec;
      fs::remove_all(opts.destination, ec);
      run_commit(opts);
    }
    if (opts.concurrency_check) {
      run_concurrency_check(opts);
    }
    if (opts.cancel_check) {
      run_cancel_check(opts);
    }
    if (opts.shutdown) {
      run_shutdown(opts);
    }
    std::println("ALL OK");
    return 0;
  } catch (const std::exception &e) {
    // Plain fprintf: a catch handler must not itself throw.
    // NOLINTNEXTLINE(modernize-use-std-print)
    std::fprintf(stderr, "FAIL: unhandled exception: %s\n", e.what());
    std::exit(k_fail);
  } catch (...) {
    // NOLINTNEXTLINE(modernize-use-std-print)
    std::fprintf(stderr, "FAIL: unhandled unknown exception\n");
    std::exit(k_fail);
  }
}
