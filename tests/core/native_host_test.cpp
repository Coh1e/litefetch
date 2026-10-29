// The browser handoff loop and its local channel, driven without a browser: a
// named pipe server in this process stands in for the app.
#include "native_frame.h"
#include "native_host_loop.h"
#include "pipe_channel.h"
#include "test_support.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <sstream>
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
} // namespace

int main()
{
    return run("native-host", [] {
        loopCopiesMessagesThrough();
        loopReportsChannelFailure();
        pipeCarriesOneRequestAndAnswer();
        pipeFailsWithoutAServer();
        return 0;
    });
}
