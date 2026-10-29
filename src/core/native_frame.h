#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace litefetch::browser {

// Chrome and Edge speak native messaging as: a 4 byte little endian length,
// then that many bytes of UTF-8 JSON. One place owns that framing so the host
// and its tests agree on bytes instead of on an intention.
inline constexpr std::size_t maxNativeMessageBytes = 1024 * 1024;

std::string frameMessage(const std::string& json);

// Pops one message from `buffer`. Returns false when the buffer does not hold a
// whole frame yet, leaving it untouched. A frame claiming more than
// maxNativeMessageBytes clears the buffer: that length is a broken stream, not a
// message worth waiting for.
bool popMessage(std::string& buffer, std::string& json);

// A small JSON value: null, bool, integer, string or object of those. The
// handoff protocol needs exactly this much, and a strict reader that refuses
// what it does not understand is safer than a forgiving one.
class JsonValue {
public:
    enum class Kind { Null, Bool, Number, String, Object };

    JsonValue() = default;
    static JsonValue makeBool(bool value);
    static JsonValue makeNumber(std::int64_t value);
    static JsonValue makeString(std::string value);
    static JsonValue makeObject();

    static std::optional<JsonValue> parse(const std::string& text);

    Kind kind() const { return kind_; }
    bool asBool() const { return bool_; }
    std::int64_t asNumber() const { return number_; }
    const std::string& asString() const { return string_; }

    void set(const std::string& key, JsonValue value);
    const JsonValue* find(const std::string& key) const;
    std::string stringOr(const std::string& key, const std::string& fallback) const;
    std::int64_t numberOr(const std::string& key, std::int64_t fallback) const;
    bool boolOr(const std::string& key, bool fallback) const;

    std::string dump() const;

private:
    Kind kind_ = Kind::Null;
    bool bool_ = false;
    std::int64_t number_ = 0;
    std::string string_;
    std::vector<std::pair<std::string, JsonValue>> members_;
};

} // namespace litefetch::browser
