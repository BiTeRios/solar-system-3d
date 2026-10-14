#include "json.hpp"

#include <cctype>
#include <charconv>
#include <cmath>

namespace sc {

namespace {
const Json kNull;
}

class JsonParser {
public:
    explicit JsonParser(const std::string &t) : s_(t) {}

    bool run(Json &out) {
        ws();
        if (!value(out, 0)) return false;
        ws();
        if (i_ != s_.size()) return fail("лишние символы после значения");
        return true;
    }
    std::string error;

private:
    const std::string &s_;
    size_t i_ = 0;

    bool fail(const char *what) {
        error = std::string(what) + " (позиция " + std::to_string(i_) + ")";
        return false;
    }
    void ws() {
        while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\n' || s_[i_] == '\r' || s_[i_] == '\t')) ++i_;
    }
    bool lit(const char *w) {
        size_t n = 0;
        while (w[n]) ++n;
        if (s_.compare(i_, n, w) != 0) return fail("неизвестное слово");
        i_ += n;
        return true;
    }
    static void utf8(std::string &o, unsigned cp) {
        if (cp < 0x80) {
            o += static_cast<char>(cp);
        } else if (cp < 0x800) {
            o += static_cast<char>(0xC0 | (cp >> 6));
            o += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            o += static_cast<char>(0xE0 | (cp >> 12));
            o += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            o += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            o += static_cast<char>(0xF0 | (cp >> 18));
            o += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            o += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            o += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }
    bool hex4(unsigned &v) {
        if (i_ + 4 > s_.size()) return fail("обрыв \\u");
        v = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = s_[i_++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') v |= static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= static_cast<unsigned>(c - 'A' + 10);
            else return fail("плохая цифра в \\u");
        }
        return true;
    }
    bool string(std::string &o) {
        ++i_;  // "
        while (i_ < s_.size()) {
            const char c = s_[i_++];
            if (c == '"') return true;
            if (c != '\\') {
                o += c;
                continue;
            }
            if (i_ >= s_.size()) break;
            const char e = s_[i_++];
            switch (e) {
                case '"': o += '"'; break;
                case '\\': o += '\\'; break;
                case '/': o += '/'; break;
                case 'b': o += '\b'; break;
                case 'f': o += '\f'; break;
                case 'n': o += '\n'; break;
                case 'r': o += '\r'; break;
                case 't': o += '\t'; break;
                case 'u': {
                    unsigned cp = 0;
                    if (!hex4(cp)) return false;
                    if (cp >= 0xD800 && cp < 0xDC00 && i_ + 1 < s_.size() && s_[i_] == '\\' && s_[i_ + 1] == 'u') {
                        i_ += 2;
                        unsigned lo = 0;
                        if (!hex4(lo)) return false;
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    utf8(o, cp);
                    break;
                }
                default: return fail("неизвестная escape-последовательность");
            }
        }
        return fail("незакрытая строка");
    }
    bool number(Json &out) {
        const size_t start = i_;
        while (i_ < s_.size() && (std::isdigit(static_cast<unsigned char>(s_[i_])) || s_[i_] == '-' || s_[i_] == '+' ||
                                  s_[i_] == '.' || s_[i_] == 'e' || s_[i_] == 'E'))
            ++i_;
        // from_chars не зависит от локали и округляет правильно
        const auto r = std::from_chars(s_.data() + start, s_.data() + i_, out.num_);
        if (r.ec != std::errc() || r.ptr != s_.data() + i_) return fail("плохое число");
        out.type_ = Json::Type::Number;
        return true;
    }
    bool value(Json &out, int depth) {
        if (depth > 200) return fail("слишком глубокая вложенность");
        if (i_ >= s_.size()) return fail("обрыв файла");
        const char c = s_[i_];
        if (c == '{') {
            ++i_;
            out.type_ = Json::Type::Object;
            ws();
            if (i_ < s_.size() && s_[i_] == '}') {
                ++i_;
                return true;
            }
            for (;;) {
                ws();
                if (i_ >= s_.size() || s_[i_] != '"') return fail("ожидался ключ");
                std::string key;
                if (!string(key)) return false;
                ws();
                if (i_ >= s_.size() || s_[i_] != ':') return fail("ожидалось «:»");
                ++i_;
                ws();
                Json v;
                if (!value(v, depth + 1)) return false;
                if (!out.obj_.count(key)) out.order_.push_back(key);
                out.obj_[key] = std::move(v);
                ws();
                if (i_ < s_.size() && s_[i_] == ',') {
                    ++i_;
                    continue;
                }
                if (i_ < s_.size() && s_[i_] == '}') {
                    ++i_;
                    return true;
                }
                return fail("ожидалось «,» или «}»");
            }
        }
        if (c == '[') {
            ++i_;
            out.type_ = Json::Type::Array;
            ws();
            if (i_ < s_.size() && s_[i_] == ']') {
                ++i_;
                return true;
            }
            for (;;) {
                ws();
                Json v;
                if (!value(v, depth + 1)) return false;
                out.arr_.push_back(std::move(v));
                ws();
                if (i_ < s_.size() && s_[i_] == ',') {
                    ++i_;
                    continue;
                }
                if (i_ < s_.size() && s_[i_] == ']') {
                    ++i_;
                    return true;
                }
                return fail("ожидалось «,» или «]»");
            }
        }
        if (c == '"') {
            out.type_ = Json::Type::String;
            return string(out.str_);
        }
        if (c == 't') {
            out.type_ = Json::Type::Bool;
            out.bool_ = true;
            return lit("true");
        }
        if (c == 'f') {
            out.type_ = Json::Type::Bool;
            return lit("false");
        }
        if (c == 'n') return lit("null");
        if (c == 'N') return lit("NaN") && (out.type_ = Json::Type::Number, out.num_ = std::nan(""), true);
        return number(out);
    }
};

Json Json::parse(const std::string &text, std::string *error) {
    Json out;
    JsonParser p(text);
    if (!p.run(out)) {
        if (error) *error = p.error;
        return Json();
    }
    if (error) error->clear();
    return out;
}

const Json &Json::operator[](size_t i) const { return (type_ == Type::Array && i < arr_.size()) ? arr_[i] : kNull; }

const Json &Json::operator[](const std::string &key) const {
    if (type_ != Type::Object) return kNull;
    auto it = obj_.find(key);
    return it == obj_.end() ? kNull : it->second;
}

std::vector<double> Json::numbers() const {
    std::vector<double> out;
    for (const Json &v : arr_) out.push_back(v.num());
    return out;
}

}  // namespace sc
