// The browser handoff loop and its local channel, driven without a browser: a
// named pipe server in this process stands in for the app.
#include "native_frame.h"
#include "native_host_loop.h"
#include "pipe_channel.h"
#include "pipe_stream.h"
#include "test_support.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <sstream>
#include <atomic>
#include <thread>

using namespace litefetch;
using namespace litefetch::browser;
using namespace litefetch::test;

namespace {
// Answers every request with {"echo":<request>} so the loop can be checked
// without the app.
void loopCopiesMessagesThrough()
{
    std::istringstream input(frameMessage(R"({"id":1,"method":"query"})")
        + frameMessage(R"({"id":2,"method":"abort"})"));
    std::ostringstream output;
    int handled = 0;
    const auto code = runHostLoop(input, output, [&](const std::string& request) {
        ++handled;
        auto parsed = JsonValue::parse(request);
        auto answer = JsonValue::makeObject();
        answer.set("version", JsonValue::makeNumber(1));
        answer.set("id", JsonValue::makeNumber(parsed ? parsed->numberOr("id", 0) : 0));
        answer.set("ok", JsonValue::makeBool(true));
        return answer.dump();
    });
    checkEqual(static_cast<long long>(code), 0LL, "the loop exits cleanly at end of input");
    checkEqual(static_cast<long long>(handled), 2LL, "both messages were handled");

    std::string buffer = output.str();
    std::string first;
    std::string second;
    check(popMessage(buffer, first), "the first answer is framed");
    check(popMessage(buffer, second), "the second answer is framed");
    check(first.find("\"id\":1") != std::string::npos, "the first answer keeps its id");
    check(second.find("\"id\":2") != std::string::npos, "the second answer keeps its id");
    check(buffer.empty(), "no extra bytes are written");
}

void loopReportsChannelFailure()
{
    std::istringstream input(frameMessage(R"({"id":7,"method":"prepare"})"));
    std::ostringstream output;
    runHostLoop(input, output, [](const std::string&) -> std::string {
        throw std::runtime_error("app not running");
    });
    std::string buffer = output.str();
    std::string answer;
    check(popMessage(buffer, answer), "a failure still produces an answer");
    check(answer.find("\"id\":7") != std::string::npos, "the failing answer keeps the id");
    check(answer.find("\"ok\":false") != std::string::npos, "the failing answer says so");
    check(answer.find("app not running") != std::string::npos, "the reason is included");
}

// A stream that hands out one byte per read, like a pipe with a single short
// message in it. The first implementation read a fixed 4096 byte block and hung
// forever here, which only a real browser revealed.
class TrickleBuf : public std::streambuf {
public:
    explicit TrickleBuf(std::string data) : data_(std::move(data)) {}

protected:
    int_type underflow() override
    {
        if (position_ >= data_.size()) return traits_type::eof();
        current_ = data_[position_];
        setg(&current_, &current_, &current_ + 1);
        ++position_;
        return traits_type::to_int_type(current_);
    }

private:
    std::string data_;
    std::size_t position_ = 0;
    char current_ = 0;
};

void theLoopAnswersWhileThePipeStaysOpen()
{
    // The shape of a real native messaging pipe: one message is written and the
    // writing side stays open. A host that waits for a full block never answers.
    HANDLE read = INVALID_HANDLE_VALUE;
    HANDLE write = INVALID_HANDLE_VALUE;
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = FALSE;
    check(CreatePipe(&read, &write, &attributes, 0) != FALSE, "the test pipe is created");

    PipeStreamBuf buffer(read);
    std::istream input(&buffer);
    std::ostringstream output;
    std::atomic<int> handled{0};
    std::thread worker([&] {
        runHostLoop(input, output, [&](const std::string&) {
            ++handled;
            return std::string(R"({"ok":true})");
        });
    });

    const auto framed = frameMessage(R"({"id":"a","method":"query","requestId":"r"})");
    DWORD written = 0;
    WriteFile(write, framed.data(), static_cast<DWORD>(framed.size()), &written, nullptr);

    const auto deadline = GetTickCount64() + 5000;
    while (output.str().empty() && GetTickCount64() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    checkEqual(static_cast<long long>(handled.load()), 1LL, "the message is handled before the pipe closes");
    check(!output.str().empty(), "the answer is written before the pipe closes");

    CloseHandle(write); // now the host sees end of input
    worker.join();
}
void aByteAtATimeStreamStillWorks()
{
    std::string framed = frameMessage(R"({"id":"a","method":"query","requestId":"r"})");
    TrickleBuf buffer(std::move(framed));
    std::istream input(&buffer);
    std::ostringstream output;
    int handled = 0;
    const auto code = runHostLoop(input, output, [&](const std::string& request) {
        ++handled;
        (void)request;
        auto answer = JsonValue::makeObject();
        answer.set("version", JsonValue::makeNumber(1));
        answer.set("id", JsonValue::makeString("a"));
        answer.set("ok", JsonValue::makeBool(true));
        return answer.dump();
    });
    checkEqual(static_cast<long long>(code), 0LL, "a trickling stream ends cleanly");
    checkEqual(static_cast<long long>(handled), 1LL, "one message arrives over a trickling stream");
    check(!output.str().empty(), "the answer was written");
}

void aShortFrameIsReported()
{
    std::string framed = frameMessage(R"({"id":"b"})");
    framed.pop_back(); // the payload is one byte short
    std::istringstream input(framed);
    std::ostringstream output;
    const auto code = runHostLoop(input, output, [](const std::string&) { return std::string("{}"); });
    check(code != 0, "a short frame is an error, not a silent wait");
}
void pipeCarriesOneRequestAndAnswer()
{
    const auto pipeName = defaultPipeName() + "-test";
    const auto fullName = L"\\\\.\\pipe\\" + std::wstring(pipeName.begin(), pipeName.end());
    std::string serverSaw;
    std::thread server([&] {
        HANDLE pipe = CreateNamedPipeW(fullName.c_str(), PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 4096, 4096, 0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE) return;
        if (!ConnectNamedPipe(pipe, nullptr) && GetLastError() != ERROR_PIPE_CONNECTED) {
            CloseHandle(pipe);
            return;
        }
        char header[4]{};
        DWORD read = 0;
        if (ReadFile(pipe, header, 4, &read, nullptr) && read == 4) {
            const auto size = static_cast<std::size_t>(static_cast<unsigned char>(header[0]))
                | (static_cast<std::size_t>(static_cast<unsigned char>(header[1])) << 8)
                | (static_cast<std::size_t>(static_cast<unsigned char>(header[2])) << 16)
                | (static_cast<std::size_t>(static_cast<unsigned char>(header[3])) << 24);
            serverSaw.resize(size);
            DWORD got = 0;
            ReadFile(pipe, serverSaw.data(), static_cast<DWORD>(size), &got, nullptr);
            serverSaw.resize(got);
            const auto answer = frameMessage(R"({"version":1,"ok":true})");
            DWORD written = 0;
            WriteFile(pipe, answer.data(), static_cast<DWORD>(answer.size()), &written, nullptr);
        }
        // Disconnecting flushes buffered data the client has not read yet, so
        // the handle is closed instead and the data stays readable.
        CloseHandle(pipe);
    });

    std::string error;
    const auto answer = pipeRoundTrip(pipeName, R"({"id":3,"method":"query"})", 3000, error);
    server.join();
    check(error.empty(), ("the pipe failed: " + error).c_str());
    checkEqual(answer, std::string(R"({"version":1,"ok":true})"), "the answer crosses the pipe");
    check(serverSaw.find("\"method\":\"query\"") != std::string::npos,
        "the server received the request");
}

void pipeFailsWithoutAServer()
{
    std::string error;
    const auto answer = pipeRoundTrip("litefetch-browser-no-such-pipe",
        R"({"id":1})", 300, error);
    check(answer.empty(), "no answer without a server");
    check(!error.empty(), "the failure is explained");
}

// The host starts the application when nobody is listening, because a browser
// extension cannot: this is the difference between "nothing happened" and the
// downloader opening itself.
void pipeStartingTheApplicationRetries()
{
    const auto pipeName = defaultPipeName() + "-start-test";
    const auto fullName = L"\\\\.\\pipe\\" + std::wstring(pipeName.begin(), pipeName.end());
    std::thread server;
    bool started = false;
    const auto startApplication = [&] {
        started = true;
        server = std::thread([&] {
            HANDLE pipe = CreateNamedPipeW(fullName.c_str(), PIPE_ACCESS_DUPLEX,
                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 4096, 4096, 0, nullptr);
            if (pipe == INVALID_HANDLE_VALUE) return;
            if (!ConnectNamedPipe(pipe, nullptr) && GetLastError() != ERROR_PIPE_CONNECTED) {
                CloseHandle(pipe);
                return;
            }
            char header[4]{};
            DWORD read = 0;
            if (ReadFile(pipe, header, 4, &read, nullptr) && read == 4) {
                const auto size = static_cast<std::size_t>(static_cast<unsigned char>(header[0]))
                    | (static_cast<std::size_t>(static_cast<unsigned char>(header[1])) << 8)
                    | (static_cast<std::size_t>(static_cast<unsigned char>(header[2])) << 16)
                    | (static_cast<std::size_t>(static_cast<unsigned char>(header[3])) << 24);
                std::string payload(size, '\0');
                DWORD got = 0;
                ReadFile(pipe, payload.data(), static_cast<DWORD>(size), &got, nullptr);
                const auto answer = frameMessage(R"({"version":1,"ok":true})");
                DWORD written = 0;
                WriteFile(pipe, answer.data(), static_cast<DWORD>(answer.size()), &written, nullptr);
            }
            CloseHandle(pipe);
        });
    };
    std::string error;
    const auto answer = pipeRoundTripStarting(pipeName, R"({"id":7,"method":"ping"})",
        startApplication, 30, 50, &error);
    if (server.joinable()) server.join();
    check(started, "the application is started when nobody answers");
    check(answer.find("\"ok\":true") != std::string::npos,
        ("the retry reaches the started application: " + error).c_str());
}
} // namespace

int main()
{
    return run("native-host", [] {
        loopCopiesMessagesThrough();
        loopReportsChannelFailure();
        theLoopAnswersWhileThePipeStaysOpen();
        aByteAtATimeStreamStillWorks();
        aShortFrameIsReported();
        pipeCarriesOneRequestAndAnswer();
        pipeFailsWithoutAServer();
        pipeStartingTheApplicationRetries();
        return 0;
    });
}
