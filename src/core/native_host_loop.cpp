#include "native_host_loop.h"

#include "native_frame.h"

#include <cstdio>

namespace litefetch::browser {

std::string errorResponse(const std::string& requestJson, const std::string& reason)
{
    auto response = JsonValue::makeObject();
    response.set("version", JsonValue::makeNumber(1));
    // Keep the id when the request made sense well enough to have one.
    if (const auto request = JsonValue::parse(requestJson)) {
        const auto* id = request->find("id");
        if (id && id->kind() == JsonValue::Kind::Number)
            response.set("id", JsonValue::makeNumber(id->asNumber()));
    }
    response.set("ok", JsonValue::makeBool(false));
    response.set("error", JsonValue::makeString(reason));
    return response.dump();
}

int runHostLoop(std::istream& input, std::ostream& output,
    const std::function<std::string(const std::string&)>& channel)
{
    std::string buffer;
    char chunk[4096];
    for (;;) {
        std::string message;
        while (popMessage(buffer, message)) {
            std::string response;
            try {
                response = channel(message);
            } catch (const std::exception& error) {
                response = errorResponse(message, error.what());
            }
            if (response.empty()) response = errorResponse(message, "empty answer");
            output << frameMessage(response);
            output.flush();
        }
        if (!input.read(chunk, sizeof(chunk)) && input.gcount() == 0) break;
        buffer.append(chunk, static_cast<std::size_t>(input.gcount()));
        // A frame longer than the limit is a broken stream; stop rather than grow
        // without bound.
        if (buffer.size() > maxNativeMessageBytes + 4) {
            std::fprintf(stderr, "litefetch browser host: message too large\n");
            return 1;
        }
    }
    return 0;
}

} // namespace litefetch::browser
