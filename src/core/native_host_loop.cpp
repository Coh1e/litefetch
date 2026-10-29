#include "native_host_loop.h"

#include "native_frame.h"

#include <cstdio>
#include <vector>

namespace litefetch::browser {

std::string errorResponse(const std::string& requestJson, const std::string& reason)
{
    auto response = JsonValue::makeObject();
    response.set("version", JsonValue::makeNumber(1));
    // Keep the id when the request made sense well enough to have one.
    if (const auto request = JsonValue::parse(requestJson)) {
        if (const auto* id = request->find("id")) response.set("id", *id);
    }
    response.set("ok", JsonValue::makeBool(false));
    response.set("error", JsonValue::makeString(reason));
    return response.dump();
}

int runHostLoop(std::istream& input, std::ostream& output,
    const std::function<std::string(const std::string&)>& channel)
{
    for (;;) {
        // The 4 byte length comes first; read it byte by byte, because a stream
        // read for a fixed size blocks until that many bytes arrive and the
        // browser sends exactly one message.
        std::uint32_t size = 0;
        for (int byte = 0; byte < 4; ++byte) {
            const int value = input.get();
            if (value == EOF) return 0; // the browser closed the pipe
            size |= static_cast<std::uint32_t>(static_cast<unsigned char>(value)) << (byte * 8);
        }
        if (size == 0 || size > maxNativeMessageBytes) {
            std::fprintf(stderr, "litefetch browser host: invalid frame length\n");
            return 1;
        }
        std::string message(size, '\0');
        if (!input.read(message.data(), static_cast<std::streamsize>(size))) {
            std::fprintf(stderr, "litefetch browser host: short frame\n");
            return 1;
        }

        std::string response;
        try {
            response = channel(message);
        } catch (const std::exception& error) {
            response = errorResponse(message, error.what());
        }
        if (response.empty()) response = errorResponse(message, "empty answer");
        output.write(frameMessage(response).data(), static_cast<std::streamsize>(response.size() + 4));
        output.flush();
    }
}

} // namespace litefetch::browser
