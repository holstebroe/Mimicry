#include "core/Json.hpp"

#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <format>

namespace mimicry::json {

namespace {
const Value kNull;
const std::string kEmptyString;
const Array kEmptyArray;
const Object kEmptyObject;

void escape(std::string& out, const std::string& s) {
    out += '"';
    for (const unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) out += std::format("\\u{:04x}", static_cast<unsigned>(c));
                else out += static_cast<char>(c);
        }
    }
    out += '"';
}

void write(const Value& v, std::string& out, int indent, int depth) {
    const auto newline = [&](int d) {
        if (indent < 0) return;
        out += '\n';
        out.append(static_cast<std::size_t>(indent * d), ' ');
    };
    if (v.is_null()) {
        out += "null";
    } else if (v.is_bool()) {
        out += v.as_bool() ? "true" : "false";
    } else if (v.is_number()) {
        const double d = v.as_number();
        if (!std::isfinite(d)) {
            out += "null";  // JSON has no NaN / Inf
        } else if (d == std::floor(d) && std::fabs(d) < 9e15) {
            out += std::format("{}", static_cast<std::int64_t>(d));
        } else {
            char buf[40];
            const auto r = std::to_chars(buf, buf + sizeof buf, d);
            out.append(buf, r.ptr);
        }
    } else if (v.is_string()) {
        escape(out, v.as_string());
    } else if (v.is_array()) {
        const auto& a = v.as_array();
        out += '[';
        for (std::size_t i = 0; i < a.size(); ++i) {
            if (i) out += ',';
            newline(depth + 1);
            write(a[i], out, indent, depth + 1);
        }
        if (!a.empty()) newline(depth);
        out += ']';
    } else {
        const auto& o = v.as_object();
        out += '{';
        bool first = true;
        for (const auto& [k, val] : o) {
            if (!first) out += ',';
            first = false;
            newline(depth + 1);
            escape(out, k);
            out += indent < 0 ? ":" : ": ";
            write(val, out, indent, depth + 1);
        }
        if (!o.empty()) newline(depth);
        out += '}';
    }
}

class Parser {
public:
    explicit Parser(std::string_view t) : t_(t) {}

    expected<Value, std::string> run() {
        auto v = value(0);
        if (!v) return v;
        skip();
        if (pos_ != t_.size()) return fail("trailing characters");
        return v;
    }

private:
    static constexpr int kMaxDepth = 128;

    unexpected<std::string> fail(std::string_view what) const {
        return unexpected(std::format("json: {} at offset {}", what, pos_));
    }
    void skip() {
        while (pos_ < t_.size() && (t_[pos_] == ' ' || t_[pos_] == '\n' || t_[pos_] == '\r' || t_[pos_] == '\t')) ++pos_;
    }
    bool eat(char c) {
        if (pos_ < t_.size() && t_[pos_] == c) {
            ++pos_;
            return true;
        }
        return false;
    }
    bool literal(std::string_view word) {
        if (t_.substr(pos_, word.size()) == word) {
            pos_ += word.size();
            return true;
        }
        return false;
    }

    expected<Value, std::string> value(int depth) {
        if (depth > kMaxDepth) return fail("nesting too deep");
        skip();
        if (pos_ >= t_.size()) return fail("unexpected end");
        const char c = t_[pos_];
        if (c == '{') return object(depth);
        if (c == '[') return array(depth);
        if (c == '"') {
            auto s = string();
            if (!s) return unexpected(s.error());
            return Value(std::move(*s));
        }
        if (literal("true")) return Value(true);
        if (literal("false")) return Value(false);
        if (literal("null")) return Value();
        return number();
    }

    expected<Value, std::string> number() {
        const std::size_t start = pos_;
        while (pos_ < t_.size() && (std::isdigit(static_cast<unsigned char>(t_[pos_])) || t_[pos_] == '-' ||
                                    t_[pos_] == '+' || t_[pos_] == '.' || t_[pos_] == 'e' || t_[pos_] == 'E'))
            ++pos_;
        if (start == pos_) return fail("unexpected character");
        double d = 0;
        const auto r = std::from_chars(t_.data() + start, t_.data() + pos_, d);
        if (r.ec != std::errc() || r.ptr != t_.data() + pos_) {
            pos_ = start;
            return fail("bad number");
        }
        return Value(d);
    }

    static void utf8(std::string& out, unsigned cp) {
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    bool hex4(unsigned& out) {
        if (pos_ + 4 > t_.size()) return false;
        unsigned v = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = t_[pos_ + static_cast<std::size_t>(i)];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') v |= static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= static_cast<unsigned>(c - 'A' + 10);
            else return false;
        }
        pos_ += 4;
        out = v;
        return true;
    }

    expected<std::string, std::string> string() {
        ++pos_;  // opening quote
        std::string out;
        while (pos_ < t_.size()) {
            const char c = t_[pos_++];
            if (c == '"') return out;
            if (static_cast<unsigned char>(c) < 0x20) return fail("control character in string");
            if (c != '\\') {
                out += c;
                continue;
            }
            if (pos_ >= t_.size()) break;
            const char e = t_[pos_++];
            switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    unsigned cp = 0;
                    if (!hex4(cp)) return fail("bad \\u escape");
                    if (cp >= 0xD800 && cp < 0xDC00) {  // surrogate pair
                        unsigned lo = 0;
                        if (!literal("\\u") || !hex4(lo) || lo < 0xDC00 || lo > 0xDFFF) return fail("bad surrogate pair");
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    utf8(out, cp);
                    break;
                }
                default: return fail("bad escape");
            }
        }
        return fail("unterminated string");
    }

    expected<Value, std::string> array(int depth) {
        ++pos_;
        Array a;
        skip();
        if (eat(']')) return Value(std::move(a));
        for (;;) {
            auto v = value(depth + 1);
            if (!v) return v;
            a.push_back(std::move(*v));
            skip();
            if (eat(',')) continue;
            if (eat(']')) return Value(std::move(a));
            return fail("expected ',' or ']'");
        }
    }

    expected<Value, std::string> object(int depth) {
        ++pos_;
        Object o;
        skip();
        if (eat('}')) return Value(std::move(o));
        for (;;) {
            skip();
            if (pos_ >= t_.size() || t_[pos_] != '"') return fail("expected object key");
            auto k = string();
            if (!k) return unexpected(k.error());
            skip();
            if (!eat(':')) return fail("expected ':'");
            auto v = value(depth + 1);
            if (!v) return v;
            o[std::move(*k)] = std::move(*v);
            skip();
            if (eat(',')) continue;
            if (eat('}')) return Value(std::move(o));
            return fail("expected ',' or '}'");
        }
    }

    std::string_view t_;
    std::size_t pos_ = 0;
};
}  // namespace

bool Value::as_bool(bool fallback) const { return is_bool() ? std::get<bool>(v_) : fallback; }
double Value::as_number(double fallback) const { return is_number() ? std::get<double>(v_) : fallback; }
const std::string& Value::as_string() const { return is_string() ? std::get<std::string>(v_) : kEmptyString; }
const Array& Value::as_array() const { return is_array() ? std::get<Array>(v_) : kEmptyArray; }
const Object& Value::as_object() const { return is_object() ? std::get<Object>(v_) : kEmptyObject; }

const Value& Value::operator[](std::string_view key) const {
    if (!is_object()) return kNull;
    const auto& o = std::get<Object>(v_);
    const auto it = o.find(key);
    return it == o.end() ? kNull : it->second;
}

Value& Value::set(std::string key, Value value) {
    if (!is_object()) v_ = Object{};
    std::get<Object>(v_)[std::move(key)] = std::move(value);
    return *this;
}

Value& Value::push(Value value) {
    if (!is_array()) v_ = Array{};
    std::get<Array>(v_).push_back(std::move(value));
    return *this;
}

std::string Value::dump(int indent) const {
    std::string out;
    write(*this, out, indent, 0);
    return out;
}

expected<Value, std::string> parse(std::string_view text) { return Parser(text).run(); }

}  // namespace mimicry::json
