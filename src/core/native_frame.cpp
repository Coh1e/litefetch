#include "native_frame.h"

#include <cctype>

namespace litefetch::browser {
namespace {
void appendEscaped(std::string& out, const std::string& text)
{
    out.push_back('"');
    for (const char c : text) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default: out.push_back(c); break;
        }
    }
    out.push_back('"');
}

class Parser {
public:
    explicit Parser(const std::string& text) : text_(text) {}

    std::optional<JsonValue> parseValue()
    {
        skipSpace();
        if (position_ >= text_.size()) return std::nullopt;
        switch (text_[position_]) {
        case '{': return parseObject();
        case '"': {
            auto text = parseString();
            if (!text) return std::nullopt;
            return JsonValue::makeString(std::move(*text));
        }
        case 't':
        case 'f': return parseBool();
        default: return parseNumber();
        }
    }

    bool atEnd()
    {
        skipSpace();
        return position_ >= text_.size();
    }

private:
    void skipSpace()
    {
        while (position_ < text_.size()
            && (text_[position_] == ' ' || text_[position_] == '\t' || text_[position_] == '\r'
                || text_[position_] == '\n'))
            ++position_;
    }

    std::optional<std::string> parseString()
    {
        if (position_ >= text_.size() || text_[position_] != '"') return std::nullopt;
        ++position_;
        std::string out;
        while (position_ < text_.size()) {
            const char c = text_[position_++];
            if (c == '"') return out;
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (position_ >= text_.size()) return std::nullopt;
            switch (text_[position_++]) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'n': out.push_back('\n'); break;
            case 't': out.push_back('\t'); break;
            case 'r': out.push_back('\r'); break;
            default: return std::nullopt; // unicode escapes are not needed here
            }
        }
        return std::nullopt;
    }

    std::optional<JsonValue> parseNumber()
    {
        const auto start = position_;
        if (position_ < text_.size() && (text_[position_] == '-' || text_[position_] == '+'))
            ++position_;
        while (position_ < text_.size()
            && std::isdigit(static_cast<unsigned char>(text_[position_])))
            ++position_;
        if (position_ == start) return std::nullopt;
        // Only integers are used by the protocol; a fractional number is refused
        // instead of being quietly truncated.
        if (position_ < text_.size()
            && (text_[position_] == '.' || text_[position_] == 'e' || text_[position_] == 'E'))
            return std::nullopt;
        return JsonValue::makeNumber(std::stoll(text_.substr(start, position_ - start)));
    }

    std::optional<JsonValue> parseBool()
    {
        if (text_.compare(position_, 4, "true") == 0) {
            position_ += 4;
            return JsonValue::makeBool(true);
        }
        if (text_.compare(position_, 5, "false") == 0) {
            position_ += 5;
            return JsonValue::makeBool(false);
        }
        return std::nullopt;
    }

    std::optional<JsonValue> parseObject()
    {
        ++position_; // '{'
        auto object = JsonValue::makeObject();
        skipSpace();
        if (position_ < text_.size() && text_[position_] == '}') {
            ++position_;
            return object;
        }
        for (;;) {
            skipSpace();
            auto key = parseString();
            if (!key) return std::nullopt;
            skipSpace();
            if (position_ >= text_.size() || text_[position_] != ':') return std::nullopt;
            ++position_;
            auto value = parseValue();
            if (!value) return std::nullopt;
            object.set(*key, std::move(*value));
            skipSpace();
            if (position_ < text_.size() && text_[position_] == ',') {
                ++position_;
                continue;
            }
            if (position_ < text_.size() && text_[position_] == '}') {
                ++position_;
                return object;
            }
            return std::nullopt;
        }
    }

    const std::string& text_;
    std::size_t position_ = 0;
};
} // namespace

std::string frameMessage(const std::string& json)
{
    const auto size = static_cast<std::uint32_t>(json.size());
    std::string frame;
    frame.reserve(json.size() + 4);
    frame.push_back(static_cast<char>(size & 0xff));
    frame.push_back(static_cast<char>((size >> 8) & 0xff));
    frame.push_back(static_cast<char>((size >> 16) & 0xff));
    frame.push_back(static_cast<char>((size >> 24) & 0xff));
    frame += json;
    return frame;
}

bool popMessage(std::string& buffer, std::string& json)
{
    if (buffer.size() < 4) return false;
    const auto size = static_cast<std::size_t>(static_cast<unsigned char>(buffer[0]))
        | (static_cast<std::size_t>(static_cast<unsigned char>(buffer[1])) << 8)
        | (static_cast<std::size_t>(static_cast<unsigned char>(buffer[2])) << 16)
        | (static_cast<std::size_t>(static_cast<unsigned char>(buffer[3])) << 24);
    if (size > maxNativeMessageBytes) {
        buffer.clear();
        return false;
    }
    if (buffer.size() < size + 4) return false;
    json = buffer.substr(4, size);
    buffer.erase(0, size + 4);
    return true;
}

JsonValue JsonValue::makeBool(bool value)
{
    JsonValue result;
    result.kind_ = Kind::Bool;
    result.bool_ = value;
    return result;
}

JsonValue JsonValue::makeNumber(std::int64_t value)
{
    JsonValue result;
    result.kind_ = Kind::Number;
    result.number_ = value;
    return result;
}

JsonValue JsonValue::makeString(std::string value)
{
    JsonValue result;
    result.kind_ = Kind::String;
    result.string_ = std::move(value);
    return result;
}

JsonValue JsonValue::makeObject()
{
    JsonValue result;
    result.kind_ = Kind::Object;
    return result;
}

std::optional<JsonValue> JsonValue::parse(const std::string& text)
{
    Parser parser(text);
    auto value = parser.parseValue();
    if (!value) return std::nullopt;
    if (!parser.atEnd()) return std::nullopt; // trailing bytes are a mistake
    return value;
}

void JsonValue::set(const std::string& key, JsonValue value)
{
    if (kind_ != Kind::Object) return;
    for (auto& member : members_) {
        if (member.first == key) {
            member.second = std::move(value);
            return;
        }
    }
    members_.emplace_back(key, std::move(value));
}

const JsonValue* JsonValue::find(const std::string& key) const
{
    if (kind_ != Kind::Object) return nullptr;
    for (const auto& member : members_) {
        if (member.first == key) return &member.second;
    }
    return nullptr;
}

std::string JsonValue::stringOr(const std::string& key, const std::string& fallback) const
{
    const auto* value = find(key);
    return value && value->kind_ == Kind::String ? value->string_ : fallback;
}

std::int64_t JsonValue::numberOr(const std::string& key, std::int64_t fallback) const
{
    const auto* value = find(key);
    return value && value->kind_ == Kind::Number ? value->number_ : fallback;
}

bool JsonValue::boolOr(const std::string& key, bool fallback) const
{
    const auto* value = find(key);
    return value && value->kind_ == Kind::Bool ? value->bool_ : fallback;
}

std::string JsonValue::dump() const
{
    switch (kind_) {
    case Kind::Null: return "null";
    case Kind::Bool: return bool_ ? "true" : "false";
    case Kind::Number: return std::to_string(number_);
    case Kind::String: {
        std::string out;
        appendEscaped(out, string_);
        return out;
    }
    case Kind::Object: {
        std::string out = "{";
        bool first = true;
        for (const auto& member : members_) {
            if (!first) out += ',';
            first = false;
            appendEscaped(out, member.first);
            out += ':';
            out += member.second.dump();
        }
        out += '}';
        return out;
    }
    }
    return "null";
}

} // namespace litefetch::browser
