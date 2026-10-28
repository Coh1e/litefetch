#include "ftp_fixture.h"
#include "http_fixture.h"
#include "task_identity.h"
#include "task_service.h"
#include "task_store.h"
#include "test_support.h"

#include <chrono>
#include <filesystem>
#include <future>
#include <stdexcept>
#include <thread>

using namespace litefetch;
using namespace litefetch::test;

namespace {
void require(bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}

std::string stateName(const TaskSnapshot& task)
{
    return std::string(taskStateName(task.state));
}

template <typename Predicate>
bool waitFor(Predicate predicate, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return predicate();
}

void checkNoStaging(const std::filesystem::path& root, const std::string& destinationName)
{
    for (const auto& entry : std::filesystem::directory_iterator(root)) {
        const auto name = entry.path().filename().string();
        require(name.rfind(destinationName + ".part", 0) != 0,
            "transfer left a staging file: " + name);
        require(name.rfind(destinationName + ".lfpart", 0) != 0,
            "finished transfer left partial data: " + name);
    }
}

void completeRoundTrip(const std::filesystem::path& root)
{
    const auto body = payload();
    HttpFixture server(HttpFixture::Options{body});
    const auto destination = root / "complete.bin";
    const auto database = root / "complete.db";
    std::string id;
    {
        TaskRepository repository(database);
        TaskService service(repository);
        const auto added = service.execute(AddTaskCommand{server.url(), destination.string()});
        require(added.ok, "add failed: " + added.error);
        require(added.task.state == TaskState::Queued, "new task is not queued");
        id = added.task.id;
        checkEqual(id, taskIdentity(server.url()), "identity is derived from the source");

        const auto started = service.execute(StartTaskCommand{id});
        require(started.ok, "start failed: " + started.error);
        require(started.task.state == TaskState::Completed,
            "completed transfer did not produce completed state: " + stateName(started.task)
                + " " + started.task.error);
        checkEqual(started.task.received, static_cast<long long>(body.size()), "final received bytes");
        checkEqual(started.task.total, static_cast<long long>(body.size()), "server total bytes");
        check(readFile(destination) == body, "downloaded file bytes differ from the controlled server body");
        checkNoStaging(root, "complete.bin");
    }

    // The command result and the durable snapshot must agree after reopening.
    {
        TaskRepository repository(database);
        TaskService service(repository);
        const auto loaded = service.snapshot(id);
        check(loaded.has_value(), "task did not survive database reopen");
        require(loaded->state == TaskState::Completed, "persisted state is not completed");
        checkEqual(loaded->received, static_cast<long long>(body.size()), "persisted byte count");
        checkEqual(static_cast<long long>(service.snapshots().size()), 1, "unexpected row count");

        const auto duplicate = service.execute(AddTaskCommand{server.url(), destination.string()});
        require(duplicate.ok, "duplicate add failed: " + duplicate.error);
        checkEqual(duplicate.task.id, id, "same source keeps the same task identity");
        checkEqual(static_cast<long long>(service.snapshots().size()), 1, "duplicate add created another row");
    }
}

void httpFailure(const std::filesystem::path& root)
{
    HttpFixture server(HttpFixture::Options{payload(1024)});
    const auto destination = root / "missing.bin";
    const auto database = root / "missing.db";
    TaskRepository repository(database);
    TaskService service(repository);
    const auto added = service.execute(AddTaskCommand{server.url("/missing"), destination.string()});
    require(added.ok, "add failed: " + added.error);
    const auto started = service.execute(StartTaskCommand{added.task.id});
    require(started.ok, "start command was not committed: " + started.error);
    require(started.task.state == TaskState::Failed, "HTTP 404 did not produce failed state");
    require(started.task.error.find("404") != std::string::npos,
        "HTTP 404 reason is missing: " + started.task.error);
    check(!std::filesystem::exists(destination), "failed transfer published a destination file");
    checkNoStaging(root, "missing.bin");
    const auto persisted = service.snapshot(added.task.id);
    require(persisted.has_value() && persisted->state == TaskState::Failed, "failed state is not durable");
}

void siteAuthentication(const std::filesystem::path& root)
{
    const auto body = payload(64 * 1024);
    HttpFixture::Options options{body};
    options.requiredAuthorization = "Basic dXNlcjpwYXNz"; // user:pass
    HttpFixture server(options);
    const auto destination = root / "auth.bin";
    const auto database = root / "auth.db";
    TaskRepository repository(database);
    TaskService service(repository);
    const auto added = service.execute(AddTaskCommand{server.url(), destination.string()});
    require(added.ok, "add failed: " + added.error);

    // Without credentials the transfer fails and publishes nothing.
    const auto anonymous = service.execute(StartTaskCommand{added.task.id});
    require(anonymous.ok, "anonymous start was not committed: " + anonymous.error);
    require(anonymous.task.state == TaskState::Failed, "a 401 did not fail the task");
    require(anonymous.task.error.find("401") != std::string::npos,
        "the 401 reason is missing: " + anonymous.task.error);
    check(!std::filesystem::exists(destination), "a 401 published a destination file");

    // The same task succeeds once credentials are supplied for this run.
    StartTaskCommand withCredentials{added.task.id};
    withCredentials.username = "user";
    withCredentials.password = "pass";
    const auto authorized = service.execute(withCredentials);
    require(authorized.ok, "authorized start was not committed: " + authorized.error);
    require(authorized.task.state == TaskState::Completed,
        "authorized transfer did not complete: " + authorized.task.error);
    check(readFile(destination) == body, "authorized download bytes differ from the source");
    checkEqual(server.lastAuthorization(), "Basic dXNlcjpwYXNz", "the fixture saw the basic credentials");

    // Credentials are not persisted with the task.
    require(authorized.task.source.find("pass") == std::string::npos, "password leaked into the row");
    require(authorized.task.error.empty(), "a completed task keeps an error");

    // A redirect to another host must not carry the Authorization header.
    HttpFixture other(HttpFixture::Options{body});
    HttpFixture::Options redirecting{body};
    // A different host name (not just another port) is what makes curl strip
    // the Authorization header on redirect.
    redirecting.redirectTo = "http://localhost:" + std::to_string(other.port()) + "/payload.bin";
    HttpFixture redirector(redirecting);
    const auto redirectedDestination = root / "redirected.bin";
    const auto redirectAdd = service.execute(AddTaskCommand{redirector.url(),
        redirectedDestination.string()});
    require(redirectAdd.ok, "redirect task add failed: " + redirectAdd.error);
    StartTaskCommand redirectStart{redirectAdd.task.id};
    redirectStart.username = "user";
    redirectStart.password = "pass";
    const auto redirected = service.execute(redirectStart);
    require(redirected.ok, "redirect start was not committed: " + redirected.error);
    require(redirected.task.state == TaskState::Completed,
        "redirected transfer did not complete: " + redirected.task.error);
    require(other.lastAuthorization().empty(),
        "credentials were forwarded to a different host after a redirect");

    // An https URL against a plain-HTTP port must fail: verification is not
    // turned off to make such a request succeed.
    const auto secure = root / "secure.bin";
    auto secureUrl = server.url();
    secureUrl.replace(0, 4, "https");
    const auto secureAdd = service.execute(AddTaskCommand{secureUrl, secure.string()});
    require(secureAdd.ok, "https task add failed: " + secureAdd.error);
    const auto secureStart = service.execute(StartTaskCommand{secureAdd.task.id});
    require(secureStart.ok, "https start was not committed: " + secureStart.error);
    require(secureStart.task.state == TaskState::Failed, "a broken TLS peer did not fail the task");
    check(!std::filesystem::exists(secure), "a failed TLS transfer published a file");
}

void segmentedTransfer(const std::filesystem::path& root)
{
    const auto body = payload(6 * 1024 * 1024);
    HttpFixture server(HttpFixture::Options{body});
    const auto destination = root / "segmented.bin";
    const auto database = root / "segmented.db";
    TaskRepository repository(database);
    TaskService service(repository);
    const auto added = service.execute(AddTaskCommand{server.url(), destination.string()});
    require(added.ok, "add failed: " + added.error);
    const auto started = service.execute(StartTaskCommand{added.task.id});
    require(started.ok, "segmented start was not committed: " + started.error);
    require(started.task.state == TaskState::Completed,
        "segmented transfer did not complete: " + started.task.error);
    check(readFile(destination) == body, "segmented download is not byte-identical");
    require(server.requestCount() >= 5,
        "one probe plus four ranges were not requested (got "
            + std::to_string(server.requestCount()) + ")");
    checkNoStaging(root, "segmented.bin");
}

void noRangeFallback(const std::filesystem::path& root)
{
    const auto body = payload(6 * 1024 * 1024);
    HttpFixture::Options options{body};
    options.supportsRange = false;
    HttpFixture server(options);
    const auto destination = root / "no-range-large.bin";
    const auto database = root / "no-range-large.db";
    TaskRepository repository(database);
    TaskService service(repository);
    const auto added = service.execute(AddTaskCommand{server.url(), destination.string()});
    require(added.ok, "add failed: " + added.error);
    const auto started = service.execute(StartTaskCommand{added.task.id});
    require(started.ok, "fallback start was not committed: " + started.error);
    require(started.task.state == TaskState::Completed,
        "a server without range support did not complete: " + started.task.error);
    check(readFile(destination) == body, "fallback download is not byte-identical");
    checkNoStaging(root, "no-range-large.bin");
}

void cancelSegmentedKeepsPrefix(const std::filesystem::path& root)
{
    const auto body = payload(6 * 1024 * 1024);
    HttpFixture server(HttpFixture::Options{body, 200, 16 * 1024, std::chrono::milliseconds(10)});
    const auto destination = root / "segment-cancel.bin";
    const auto database = root / "segment-cancel.db";
    TaskRepository repository(database);
    TaskService service(repository);
    const auto added = service.execute(AddTaskCommand{server.url(), destination.string()});
    require(added.ok, "add failed: " + added.error);

    std::promise<CommandResult> finished;
    auto result = finished.get_future();
    std::jthread worker([&] { finished.set_value(service.execute(StartTaskCommand{added.task.id})); });
    // Wait for the first range to start writing, then cancel while the other
    // ranges are still in flight.
    auto segmentPath = std::filesystem::path(partialPathFor(destination));
    segmentPath += ".seg0";
    require(waitFor([&] {
        std::error_code error;
        return std::filesystem::file_size(segmentPath, error) > 0 && !error;
    }, std::chrono::seconds(20)), "no range started writing");

    const auto paused = service.execute(PauseTaskCommand{added.task.id});
    require(paused.ok, "pause failed: " + paused.error);
    const auto firstRun = result.get();
    worker.join();
    require(firstRun.task.state == TaskState::Paused, "segmented pause did not commit");
    check(!std::filesystem::exists(destination), "paused segmented transfer published a file");
    const auto partial = std::filesystem::path(partialPathFor(destination));
    require(std::filesystem::exists(partial), "paused segmented transfer kept no prefix");
    const auto prefix = std::filesystem::file_size(partial);
    require(prefix > 0, "paused segmented transfer kept no bytes");

    // The next single-stream start resumes from the merged prefix.
    const auto resumed = service.execute(StartTaskCommand{added.task.id});
    require(resumed.ok, "resume failed: " + resumed.error);
    require(resumed.task.state == TaskState::Completed,
        "resume after a paused segmented run did not complete: " + resumed.task.error);
    check(readFile(destination) == body, "resumed file after segmentation is not byte-identical");
    checkNoStaging(root, "segment-cancel.bin");
}

void ftpTransfers(const std::filesystem::path& root)
{
    const auto body = payload(256 * 1024);
    FtpFixture server(FtpFixture::Options{body});
    const auto destination = root / "ftp.bin";
    const auto database = root / "ftp.db";
    TaskRepository repository(database);
    TaskService service(repository);
    const auto added = service.execute(AddTaskCommand{server.url(), destination.string()});
    require(added.ok, "ftp add failed: " + added.error);

    StartTaskCommand start{added.task.id};
    start.username = "user";
    start.password = "pass";
    const auto completed = service.execute(start);
    require(completed.ok, "ftp start was not committed: " + completed.error);
    require(completed.task.state == TaskState::Completed,
        "ftp download did not complete: " + completed.task.error);
    check(readFile(destination) == body, "ftp download is not byte-identical");
    checkEqual(static_cast<long long>(completed.task.total), static_cast<long long>(body.size()),
        "ftp total comes from SIZE");

    // A wrong password is classified, not merged into a generic network error.
    const auto wrongDestination = root / "ftp-wrong.bin";
    const auto wrongAdd = service.execute(AddTaskCommand{server.url("/other.bin"),
        wrongDestination.string()});
    require(wrongAdd.ok, "ftp add failed: " + wrongAdd.error);
    StartTaskCommand wrongStart{wrongAdd.task.id};
    wrongStart.username = "user";
    wrongStart.password = "nope";
    const auto denied = service.execute(wrongStart);
    require(denied.ok, "ftp start was not committed: " + denied.error);
    require(denied.task.state == TaskState::Failed, "a wrong password did not fail the task");
    require(denied.task.error.find("530") != std::string::npos,
        "530 is missing from the reason: " + denied.task.error);
    check(!std::filesystem::exists(wrongDestination), "failed ftp login published a file");

    // A missing file reports 550 and publishes nothing.
    FtpFixture::Options missingOptions{body};
    missingOptions.fileExists = false;
    FtpFixture missing(missingOptions);
    const auto missingDestination = root / "ftp-missing.bin";
    const auto missingAdd = service.execute(AddTaskCommand{missing.url(), missingDestination.string()});
    require(missingAdd.ok, "ftp add failed: " + missingAdd.error);
    StartTaskCommand missingStart{missingAdd.task.id};
    missingStart.username = "user";
    missingStart.password = "pass";
    const auto notFound = service.execute(missingStart);
    require(notFound.ok, "ftp start was not committed: " + notFound.error);
    require(notFound.task.state == TaskState::Failed, "a missing file did not fail the task");
    require(notFound.task.error.find("550") != std::string::npos,
        "550 is missing from the reason: " + notFound.task.error);
    check(!std::filesystem::exists(missingDestination), "failed ftp transfer published a file");
}

void ftpResumeAndRestart(const std::filesystem::path& root)
{
    const auto body = payload(512 * 1024);
    FtpFixture::Options options{body, "user", "pass", true, true, std::chrono::milliseconds(10), 8 * 1024};
    FtpFixture server(options);
    const auto destination = root / "ftp-resume.bin";
    const auto database = root / "ftp-resume.db";
    TaskRepository repository(database);
    TaskService service(repository);
    const auto added = service.execute(AddTaskCommand{server.url(), destination.string()});
    require(added.ok, "ftp add failed: " + added.error);
    StartTaskCommand start{added.task.id};
    start.username = "user";
    start.password = "pass";

    std::promise<CommandResult> finished;
    auto first = finished.get_future();
    std::jthread worker([&] { finished.set_value(service.execute(start)); });
    require(waitFor([&] {
        const auto snapshot = service.snapshot(added.task.id);
        return snapshot && snapshot->state == TaskState::Running && snapshot->received > 0;
    }, std::chrono::seconds(20)), "ftp download reported no progress");
    require(service.execute(PauseTaskCommand{added.task.id}).ok, "ftp pause failed");
    const auto paused = first.get();
    worker.join();
    require(paused.task.state == TaskState::Paused, "ftp pause did not commit");
    const auto partial = std::filesystem::path(partialPathFor(destination));
    const auto prefix = std::filesystem::file_size(partial);
    require(prefix > 0 && prefix < body.size(), "ftp pause kept no usable partial");
    require(!server.resumeUsed(), "no REST before the first resume; commands: " + server.commandLog());

    const auto resumed = service.execute(start);
    require(resumed.ok, "ftp resume failed: " + resumed.error);
    require(resumed.task.state == TaskState::Completed,
        "ftp resume did not complete: " + resumed.task.error);
    check(server.resumeUsed(), "the resume did not issue REST");
    check(readFile(destination) == body, "resumed ftp file is not byte-identical");
    checkNoStaging(root, "ftp-resume.bin");

    // A server without REST must refuse to append instead of starting over.
    FtpFixture::Options noRest{body, "user", "pass", false};
    noRest.chunkDelay = std::chrono::milliseconds(10);
    noRest.chunk = 8 * 1024;
    FtpFixture refusing(noRest);
    const auto refusedDestination = root / "ftp-no-rest.bin";
    const auto refusedAdd = service.execute(AddTaskCommand{refusing.url(),
        refusedDestination.string()});
    require(refusedAdd.ok, "ftp add failed: " + refusedAdd.error);
    StartTaskCommand refusedStart{refusedAdd.task.id};
    refusedStart.username = "user";
    refusedStart.password = "pass";
    std::promise<CommandResult> refusedFinished;
    auto refusedFirst = refusedFinished.get_future();
    std::jthread refusedWorker([&] { refusedFinished.set_value(service.execute(refusedStart)); });
    require(waitFor([&] {
        const auto snapshot = service.snapshot(refusedAdd.task.id);
        return snapshot && snapshot->state == TaskState::Running && snapshot->received > 0;
    }, std::chrono::seconds(20)), "ftp download reported no progress before pause");
    require(service.execute(PauseTaskCommand{refusedAdd.task.id}).ok, "ftp pause failed");
    refusedFirst.get();
    refusedWorker.join();
    const auto refusedPartial = std::filesystem::path(partialPathFor(refusedDestination));
    const auto refusedBytes = std::filesystem::file_size(refusedPartial);
    const auto refusedResume = service.execute(refusedStart);
    require(refusedResume.ok, "refused ftp resume was not committed: " + refusedResume.error);
    require(refusedResume.task.state == TaskState::Paused, "refused ftp resume did not stay paused");
    checkEqual(std::filesystem::file_size(refusedPartial), static_cast<long long>(refusedBytes),
        "refused ftp resume wrote into the partial file");
    check(!std::filesystem::exists(refusedDestination), "refused ftp resume published a file");
}

void torrentSelectionPersists(const std::filesystem::path& root)
{
    const auto database = root / "torrent.db";
    const auto savePath = root / "torrent-data";
    std::filesystem::create_directories(savePath);
    const std::string magnet =
        "magnet:?xt=urn:btih:0123456789012345678901234567890123456789&dn=fixture";
    std::string id;
    {
        TaskRepository repository(database);
        TaskService service(repository);
        AddTaskCommand add;
        add.source = magnet;
        add.destination = savePath.string();
        add.files = {2, 0};
        add.identity = "0123456789abcdef0123456789abcdef01234567";
        const auto added = service.execute(add);
        require(added.ok, "torrent add failed: " + added.error);
        id = added.task.id;
        require(added.task.files == std::vector<int>({2, 0}), "selection is stored in order");
        checkEqual(added.task.id, taskIdentity(add.identity),
            "a torrent task takes its identity from the info hash");

        // The same torrent through a different source string locates this task.
        AddTaskCommand variant;
        variant.source = "magnet:?xt=urn:btih:0123456789abcdef0123456789abcdef01234567&dn=other";
        variant.destination = savePath.string();
        variant.identity = add.identity;
        const auto located = service.execute(variant);
        require(located.ok && located.existing, "the same info hash did not locate the task");

        // Options can be edited before the transfer starts, and the edit is
        // rejected once it is running or completed.
        UpdateTorrentOptionsCommand update;
        update.taskId = added.task.id;
        update.files = {0, 1};
        update.filePriorities = {2, 0};
        update.trackers = {"udp://tracker.example:80"};
        update.seedMode = 1;
        update.seedRatioMilli = 1500;
        update.seedMinutes = 45;
        const auto edited = service.execute(update);
        require(edited.ok, "torrent option update failed: " + edited.error);
        require(edited.task.files == std::vector<int>({0, 1}), "files were not replaced");
        require(edited.task.filePriorities == std::vector<int>({2, 0}), "priorities were not replaced");
        require(edited.task.trackers.size() == 1, "tracker was not stored");
        require(edited.task.seedMode == 1 && edited.task.seedRatioMilli == 1500
                && edited.task.seedMinutes == 45,
            "seed policy was not stored");

        // Without a BitTorrent engine the start is refused with a clear reason
        // instead of pretending to download.
        const auto started = service.execute(StartTaskCommand{id});
        require(started.ok, "torrent start was not committed: " + started.error);
        require(started.task.state == TaskState::Failed, "a start without the engine did not fail");
        require(started.task.error.find("BitTorrent") != std::string::npos,
            "the missing engine is not named: " + started.task.error);
    }
    {
        TaskRepository reopened(database);
        const auto loaded = reopened.find(id);
        require(loaded.has_value(), "torrent task did not survive reopen");
        require(loaded->files == std::vector<int>({0, 1}), "selection did not survive reopen");
        require(loaded->filePriorities == std::vector<int>({2, 0}),
            "priorities did not survive reopen");
        require(loaded->trackers.size() == 1, "trackers did not survive reopen");
        require(loaded->seedMode == 1 && loaded->seedRatioMilli == 1500
                && loaded->seedMinutes == 45,
            "seed policy did not survive reopen");
        require(loaded->destination == savePath.string(), "save path did not survive reopen");
    }
}

void torrentPauseUsesSharedStop(const std::filesystem::path& root)
{
    const auto database = root / "torrent-pause.db";
    const auto savePath = root / "torrent-pause-data";
    std::filesystem::create_directories(savePath);
    TaskRepository repository(database);

    // A stub engine that blocks like a real transfer until it is cancelled, so
    // the shared pause path can be observed without a swarm.
    std::atomic_bool entered{false};
    auto stub = [&](const BtDownloadRequest&, const std::atomic_bool& cancelled,
                    const std::function<void(const BtProgressInfo&)>&) {
        entered.store(true);
        for (int step = 0; step < 500 && !cancelled.load(); ++step)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        BtDownloadResult result;
        result.cancelled = cancelled.load();
        result.ok = !result.cancelled;
        result.received = 1024;
        return result;
    };
    TaskService service(repository, transferHttp, {}, stub);

    AddTaskCommand add;
    add.source = "magnet:?xt=urn:btih:fedcba9876543210fedcba9876543210fedcba98";
    add.destination = savePath.string();
    const auto added = service.execute(add);
    require(added.ok, "torrent add failed: " + added.error);

    std::promise<CommandResult> first;
    auto firstResult = first.get_future();
    std::jthread worker([&] { first.set_value(service.execute(StartTaskCommand{added.task.id})); });
    require(waitFor([&] { return entered.load(); }, std::chrono::seconds(10)),
        "the torrent engine was never entered");

    const auto paused = service.execute(PauseTaskCommand{added.task.id});
    require(paused.ok, "torrent pause request failed: " + paused.error);
    const auto firstRun = firstResult.get();
    worker.join();
    require(firstRun.ok, "paused torrent state was not committed: " + firstRun.error);
    require(firstRun.task.state == TaskState::Paused, "torrent pause did not produce paused state");

    // The same stop path covers the second run: without a cancel it completes.
    const auto resumed = service.execute(StartTaskCommand{added.task.id});
    require(resumed.ok, "torrent resume failed: " + resumed.error);
    require(resumed.task.state == TaskState::Completed, "torrent resume did not complete");

    // A stop (not a pause) leaves the task cancelled, not paused. A completed
    // task cannot start again, so this uses a second torrent task.
    entered.store(false);
    AddTaskCommand addTwo;
    addTwo.source = "magnet:?xt=urn:btih:1111222233334444555566667777888899990000";
    addTwo.destination = savePath.string();
    const auto addedTwo = service.execute(addTwo);
    require(addedTwo.ok, "second torrent add failed: " + addedTwo.error);
    std::promise<CommandResult> second;
    auto secondResult = second.get_future();
    std::jthread stopper([&] { second.set_value(service.execute(StartTaskCommand{addedTwo.task.id})); });
    require(waitFor([&] { return entered.load(); }, std::chrono::seconds(10)),
        "the torrent engine was never entered for the stop run");
    require(service.execute(CancelTaskCommand{addedTwo.task.id}).ok, "torrent stop failed");
    const auto stopped = secondResult.get();
    stopper.join();
    require(stopped.ok, "stopped torrent state was not committed: " + stopped.error);
    require(stopped.task.state == TaskState::Cancelled, "torrent stop did not produce the cancelled state");
}
void sameDestinationRace(const std::filesystem::path& root)
{
    const auto bodyA = payload(1024 * 1024);
    auto bodyB = bodyA;
    for (auto& byte : bodyB) byte = static_cast<unsigned char>(byte ^ 0x5a);
    HttpFixture serverA(HttpFixture::Options{bodyA, 200, 8 * 1024, std::chrono::milliseconds(20)});
    HttpFixture serverB(HttpFixture::Options{bodyB, 200, 8 * 1024, std::chrono::milliseconds(20)});
    const auto destination = root / "race.bin";
    const auto database = root / "race.db";
    TaskRepository repository(database);
    TaskService service(repository);
    const auto addedA = service.execute(AddTaskCommand{serverA.url(), destination.string()});
    const auto addedB = service.execute(AddTaskCommand{serverB.url(), destination.string()});
    require(addedA.ok, "first add failed: " + addedA.error);
    require(addedB.ok, "second add failed: " + addedB.error);
    require(addedA.task.id != addedB.task.id, "race sources unexpectedly share identity");

    std::promise<CommandResult> finishedA;
    std::promise<CommandResult> finishedB;
    auto resultA = finishedA.get_future();
    auto resultB = finishedB.get_future();
    std::jthread workerA([&] { finishedA.set_value(service.execute(StartTaskCommand{addedA.task.id})); });
    std::jthread workerB([&] { finishedB.set_value(service.execute(StartTaskCommand{addedB.task.id})); });

    // Two sources aimed at one destination are serialised per destination: the
    // second transfer starts after the first published, finds the file, and
    // fails without touching it.
    const auto startedA = resultA.get();
    const auto startedB = resultB.get();
    workerA.join();
    workerB.join();
    require(startedA.ok, "first start command was not committed: " + startedA.error);
    require(startedB.ok, "second start command was not committed: " + startedB.error);

    const auto published = readFile(destination);
    const bool aWon = published == bodyA;
    const bool bWon = published == bodyB;
    require(aWon != bWon, "published file does not match exactly one transfer");
    const auto& winner = aWon ? startedA : startedB;
    const auto& loser = aWon ? startedB : startedA;
    require(winner.task.state == TaskState::Completed, "winner did not persist completed state");
    require(loser.task.state == TaskState::Failed, "loser did not persist failed state");
    require(loser.task.error.find("destination") != std::string::npos
        || loser.task.error.find("exists") != std::string::npos,
        "loser failure does not identify the destination conflict: " + loser.task.error);
    checkEqual(static_cast<long long>(published.size()),
        static_cast<long long>(aWon ? bodyA.size() : bodyB.size()), "published length matches winner");

    {
        TaskRepository reopened(database);
        TaskService serviceAfterReopen(reopened);
        const auto persistedA = serviceAfterReopen.snapshot(addedA.task.id);
        const auto persistedB = serviceAfterReopen.snapshot(addedB.task.id);
        require(persistedA && persistedB, "race tasks did not survive reopen");
        require(persistedA->state == (aWon ? TaskState::Completed : TaskState::Failed),
            "first task state after reopen is inconsistent");
        require(persistedB->state == (bWon ? TaskState::Completed : TaskState::Failed),
            "second task state after reopen is inconsistent");
    }
    checkNoStaging(root, "race.bin");
}

void cancelThenStart(const std::filesystem::path& root)
{
    HttpFixture server(HttpFixture::Options{payload(1024)});
    const auto destination = root / "cancel-then-start.bin";
    const auto database = root / "cancel-then-start.db";
    TaskRepository repository(database);
    TaskService service(repository);
    const auto added = service.execute(AddTaskCommand{server.url(), destination.string()});
    require(added.ok, "add failed: " + added.error);
    const auto cancelled = service.execute(CancelTaskCommand{added.task.id});
    require(cancelled.ok, "queued cancel failed: " + cancelled.error);
    require(cancelled.task.state == TaskState::Cancelled, "queued cancel did not persist cancelled state");

    const auto started = service.execute(StartTaskCommand{added.task.id});
    require(!started.ok, "start accepted a task after cancel committed");
    const auto persisted = service.snapshot(added.task.id);
    require(persisted.has_value() && persisted->state == TaskState::Cancelled,
        "cancel result was overwritten by a later start");
    check(!std::filesystem::exists(destination), "start after cancel created a file");
}

void cancellation(const std::filesystem::path& root)
{
    const auto body = payload(2 * 1024 * 1024);
    HttpFixture server(HttpFixture::Options{body, 200, 16 * 1024, std::chrono::milliseconds(20)});
    const auto destination = root / "cancel.bin";
    const auto database = root / "cancel.db";
    TaskRepository repository(database);
    TaskService service(repository);
    const auto added = service.execute(AddTaskCommand{server.url(), destination.string()});
    require(added.ok, "add failed: " + added.error);

    std::promise<CommandResult> finished;
    auto result = finished.get_future();
    std::jthread worker([&] { finished.set_value(service.execute(StartTaskCommand{added.task.id})); });
    require(waitFor([&] {
        const auto snapshot = service.snapshot(added.task.id);
        return snapshot && snapshot->state == TaskState::Running && snapshot->received > 0;
    }, std::chrono::seconds(10)), "download did not reach running state with progress");

    const auto requested = service.execute(CancelTaskCommand{added.task.id});
    require(requested.ok, "cancel request failed: " + requested.error);
    const auto started = result.get();
    worker.join();
    require(started.ok, "cancelled transfer state was not committed: " + started.error);
    require(started.task.state == TaskState::Cancelled, "cancel did not produce cancelled state");
    check(!std::filesystem::exists(destination), "cancelled transfer published a destination file");
    // A cancelled transfer keeps its partial bytes so a later start resumes,
    // but never leaves an old-style staging file behind.
    require(std::filesystem::exists(partialPathFor(destination)),
        "cancel discarded the resumable partial data");
    require(std::filesystem::file_size(partialPathFor(destination)) > 0,
        "canceled transfer left an empty partial file");
}

void resumeAfterPause(const std::filesystem::path& root)
{
    const auto body = payload(2 * 1024 * 1024);
    HttpFixture server(HttpFixture::Options{body, 200, 16 * 1024, std::chrono::milliseconds(20)});
    const auto destination = root / "resume.bin";
    const auto database = root / "resume.db";
    TaskRepository repository(database);
    TaskService service(repository);
    const auto added = service.execute(AddTaskCommand{server.url(), destination.string()});
    require(added.ok, "add failed: " + added.error);

    std::promise<CommandResult> first;
    auto firstResult = first.get_future();
    std::jthread worker([&] { first.set_value(service.execute(StartTaskCommand{added.task.id})); });
    require(waitFor([&] {
        const auto snapshot = service.snapshot(added.task.id);
        return snapshot && snapshot->state == TaskState::Running && snapshot->received > 0;
    }, std::chrono::seconds(10)), "download did not reach running state with progress");

    const auto paused = service.execute(PauseTaskCommand{added.task.id});
    require(paused.ok, "pause request failed: " + paused.error);
    const auto firstRun = firstResult.get();
    worker.join();
    require(firstRun.ok, "paused transfer state was not committed: " + firstRun.error);
    require(firstRun.task.state == TaskState::Paused, "pause did not produce paused state");
    require(!std::filesystem::exists(destination), "paused transfer published a destination file");
    const auto partial = std::filesystem::path(partialPathFor(destination));
    require(std::filesystem::exists(partial), "pause discarded the partial data");
    const auto partialBytes = std::filesystem::file_size(partial);
    require(partialBytes > 0 && partialBytes < body.size(), "pause kept an unusable partial size");
    checkEqual(firstRun.task.received, static_cast<long long>(partialBytes),
        "paused task reports the bytes on disk");

    // Second run resumes: the server sees a Range request and answers 206.
    const auto resumed = service.execute(StartTaskCommand{added.task.id});
    require(resumed.ok, "resume start failed: " + resumed.error);
    require(resumed.task.state == TaskState::Completed,
        "resumed transfer did not complete: " + resumed.task.error);
    check(readFile(destination) == body, "resumed file is not byte-identical to the source");
    checkNoStaging(root, "resume.bin");
}

void resumeRefusedThenConfirmed(const std::filesystem::path& root)
{
    const auto body = payload(1024 * 1024);
    HttpFixture server(HttpFixture::Options{body, 200, 16 * 1024, std::chrono::milliseconds(20),
        false, "\"no-range\""});
    const auto destination = root / "no-range.bin";
    const auto database = root / "no-range.db";
    TaskRepository repository(database);
    TaskService service(repository);
    const auto added = service.execute(AddTaskCommand{server.url(), destination.string()});
    require(added.ok, "add failed: " + added.error);

    std::promise<CommandResult> first;
    auto firstResult = first.get_future();
    std::jthread worker([&] { first.set_value(service.execute(StartTaskCommand{added.task.id})); });
    require(waitFor([&] {
        const auto snapshot = service.snapshot(added.task.id);
        return snapshot && snapshot->state == TaskState::Running && snapshot->received > 0;
    }, std::chrono::seconds(10)), "download did not reach running state with progress");
    require(service.execute(PauseTaskCommand{added.task.id}).ok, "pause request failed");
    const auto firstRun = firstResult.get();
    worker.join();
    require(firstRun.task.state == TaskState::Paused, "pause did not produce paused state");
    const auto partial = std::filesystem::path(partialPathFor(destination));
    const auto partialBytes = std::filesystem::file_size(partial);

    // The server ignores the range, so the service must stop and ask instead of
    // appending a second copy of the resource onto the partial file.
    const auto refused = service.execute(StartTaskCommand{added.task.id});
    require(refused.ok, "refused resume was not committed: " + refused.error);
    require(refused.task.state == TaskState::Paused, "refused resume did not stay paused");
    require(!refused.task.error.empty(), "refused resume has no reason");
    checkEqual(std::filesystem::file_size(partial), static_cast<long long>(partialBytes),
        "refused resume wrote into the partial file");
    check(!std::filesystem::exists(destination), "refused resume published a destination file");

    // Only an explicit restart discards the stored bytes and downloads again.
    StartTaskCommand confirm{added.task.id};
    confirm.restartFromZero = true;
    const auto restarted = service.execute(confirm);
    require(restarted.ok, "confirmed restart failed: " + restarted.error);
    require(restarted.task.state == TaskState::Completed,
        "confirmed restart did not complete: " + restarted.task.error);
    check(readFile(destination) == body, "restarted file is not byte-identical to the source");
    checkNoStaging(root, "no-range.bin");
}

void interruptedRowBecomesPaused(const std::filesystem::path& root)
{
    const auto database = root / "interrupted.db";
    TaskRepository repository(database);
    TaskSnapshot task;
    task.id = "interrupted-task";
    task.source = "http://example.invalid/interrupted.bin";
    task.destination = (root / "interrupted.bin").string();
    task.state = TaskState::Running;
    task.received = 4096;
    task.createdAt = 1;
    task.updatedAt = 1;
    repository.insert(task);
    {
        TaskService service(repository);
        checkEqual(service.pauseInterrupted(), 1, "startup reconciliation paused the interrupted row");
        const auto after = service.snapshot(task.id);
        require(after && after->state == TaskState::Paused,
            "interrupted row was not paused on restart");
        checkEqual(after->received, 4096, "reconciliation kept the received bytes");
        checkEqual(service.pauseInterrupted(), 0, "second reconciliation changes nothing");
    }
}

void duplicateIdentityAndDeleteRecord(const std::filesystem::path& root)
{
    const auto body = payload(2 * 1024 * 1024);
    HttpFixture server(HttpFixture::Options{body, 200, 16 * 1024, std::chrono::milliseconds(20)});
    const auto database = root / "duplicate-delete.db";
    const auto originalDestination = root / "original.bin";
    const auto copyDestination = root / "original (2).bin";
    TaskRepository repository(database);
    TaskService service(repository);

    const auto original = service.execute(AddTaskCommand{server.url(), originalDestination.string()});
    require(original.ok, "original add failed: " + original.error);
    require(!original.existing, "a first add is not an existing task");

    const auto located = service.execute(AddTaskCommand{server.url(), originalDestination.string()});
    require(located.ok && located.existing, "same source+destination locates the existing task");
    checkEqual(located.task.id, original.task.id, "located task keeps the same identity");
    checkEqual(static_cast<long long>(service.snapshots().size()), 1, "locate did not insert a row");

    const auto conflicting = service.execute(AddTaskCommand{
        server.url(), copyDestination.string()});
    require(!conflicting.ok, "same source with another destination needs an explicit copy choice");

    const auto copy = service.execute(AddTaskCommand{
        server.url(), copyDestination.string(), true});
    require(copy.ok, "explicit duplicate failed: " + copy.error);
    require(!copy.existing, "explicit duplicate creates a new record");
    require(copy.task.id != original.task.id, "explicit duplicate has a distinct identity");
    checkEqual(static_cast<long long>(service.snapshots().size()), 2, "explicit duplicate added one row");
    const auto persistedCopy = service.snapshot(copy.task.id);
    require(persistedCopy.has_value(), "duplicate identity is persisted and restart-stable");

    const auto sameTarget = service.execute(AddTaskCommand{
        server.url(), originalDestination.string(), true});
    require(!sameTarget.ok, "explicit duplicate refuses to reuse the same file target");

    // A record delete never removes the owned file.
    writeFile(originalDestination, {1, 2, 3, 4});
    const auto deleted = service.execute(DeleteTaskCommand{original.task.id});
    require(deleted.ok, "record delete failed: " + deleted.error);
    check(!service.snapshot(original.task.id).has_value(), "deleted record is gone from storage");
    check(readFile(originalDestination) == std::vector<unsigned char>{1, 2, 3, 4},
        "record delete kept the downloaded file bytes");
    require(service.snapshot(copy.task.id).has_value(), "other records are unaffected");

    // Running work must be stopped (and committed) before the record is removed.
    std::promise<CommandResult> finished;
    auto result = finished.get_future();
    std::jthread worker([&] { finished.set_value(service.execute(StartTaskCommand{copy.task.id})); });
    require(waitFor([&] {
        const auto snapshot = service.snapshot(copy.task.id);
        return snapshot && snapshot->state == TaskState::Running && snapshot->received > 0;
    }, std::chrono::seconds(10)), "duplicate copy did not reach running state");
    const auto rejected = service.execute(DeleteTaskCommand{copy.task.id});
    require(!rejected.ok && rejected.error.find("running") != std::string::npos,
        "delete must reject a task with an active writer");
    const auto cancelled = service.execute(CancelTaskCommand{copy.task.id});
    require(cancelled.ok, "cancel before delete failed: " + cancelled.error);
    const auto started = result.get();
    worker.join();
    require(started.ok && started.task.state == TaskState::Cancelled,
        "cancelled copy did not commit its outcome");
    const auto deletedCopy = service.execute(DeleteTaskCommand{copy.task.id});
    require(deletedCopy.ok, "delete after cancel failed: " + deletedCopy.error);
    check(service.snapshots().empty(), "all records deleted");
    check(!std::filesystem::exists(copyDestination), "cancelled copy did not publish a file");
}

int body()
{
    TempDir root;
    completeRoundTrip(root.path());
    siteAuthentication(root.path());
    segmentedTransfer(root.path());
    noRangeFallback(root.path());
    cancelSegmentedKeepsPrefix(root.path());
    ftpTransfers(root.path());
    ftpResumeAndRestart(root.path());
    torrentSelectionPersists(root.path());
    torrentPauseUsesSharedStop(root.path());
    httpFailure(root.path());
    sameDestinationRace(root.path());
    cancelThenStart(root.path());
    cancellation(root.path());
    resumeAfterPause(root.path());
    resumeRefusedThenConfirmed(root.path());
    interruptedRowBecomesPaused(root.path());
    duplicateIdentityAndDeleteRecord(root.path());
    return 0;
}
} // namespace

int main()
{
    return run("service", &body);
}
