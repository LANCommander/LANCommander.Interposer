#include "json.h"

#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <random>

namespace json
{

namespace
{
    // The length of the UTF-8 sequence starting at s[i], or 0 when it is not
    // a valid one (overlong forms, surrogates and code points past U+10FFFF
    // included).
    size_t Utf8SequenceLength(const unsigned char* s, size_t remaining)
    {
        const unsigned char c = s[0];

        if (c < 0x80)
            return 1;

        size_t length;
        unsigned int cp;

        if ((c & 0xE0) == 0xC0)      { length = 2; cp = c & 0x1F; }
        else if ((c & 0xF0) == 0xE0) { length = 3; cp = c & 0x0F; }
        else if ((c & 0xF8) == 0xF0) { length = 4; cp = c & 0x07; }
        else
            return 0;

        if (remaining < length)
            return 0;

        for (size_t i = 1; i < length; ++i)
        {
            if ((s[i] & 0xC0) != 0x80)
                return 0;

            cp = (cp << 6) | (s[i] & 0x3F);
        }

        if ((length == 2 && cp < 0x80) || (length == 3 && cp < 0x800) || (length == 4 && cp < 0x10000))
            return 0;

        if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
            return 0;

        return length;
    }

    // Recursive-descent check of one value. pos is advanced past it.
    class Validator
    {
    public:
        explicit Validator(const char* text) : s_(text), n_(std::strlen(text)) {}

        bool Object()
        {
            SkipSpace();

            if (!Peek('{') || !ValueAt(0))
                return false;

            SkipSpace();
            return pos_ == n_;
        }

    private:
        const char* s_;
        size_t      n_;
        size_t      pos_ = 0;

        static constexpr int kMaxDepth = 64;

        bool Peek(char c) const { return pos_ < n_ && s_[pos_] == c; }

        void SkipSpace()
        {
            while (pos_ < n_ && (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\n' || s_[pos_] == '\r'))
                ++pos_;
        }

        bool Literal(const char* word)
        {
            const size_t length = std::strlen(word);

            if (n_ - pos_ < length || std::memcmp(s_ + pos_, word, length) != 0)
                return false;

            pos_ += length;
            return true;
        }

        static bool IsDigit(char c) { return c >= '0' && c <= '9'; }

        static bool IsHex(char c)
        {
            return IsDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        }

        bool Number()
        {
            if (Peek('-'))
                ++pos_;

            if (Peek('0'))
                ++pos_;
            else if (pos_ < n_ && s_[pos_] >= '1' && s_[pos_] <= '9')
                while (pos_ < n_ && IsDigit(s_[pos_])) ++pos_;
            else
                return false;

            if (Peek('.'))
            {
                ++pos_;

                if (pos_ >= n_ || !IsDigit(s_[pos_]))
                    return false;

                while (pos_ < n_ && IsDigit(s_[pos_])) ++pos_;
            }

            if (Peek('e') || Peek('E'))
            {
                ++pos_;

                if (Peek('+') || Peek('-'))
                    ++pos_;

                if (pos_ >= n_ || !IsDigit(s_[pos_]))
                    return false;

                while (pos_ < n_ && IsDigit(s_[pos_])) ++pos_;
            }

            return true;
        }

        bool String()
        {
            if (!Peek('"'))
                return false;

            ++pos_;

            while (pos_ < n_)
            {
                const unsigned char c = static_cast<unsigned char>(s_[pos_]);

                if (c == '"')
                {
                    ++pos_;
                    return true;
                }

                if (c < 0x20)
                    return false;

                if (c == '\\')
                {
                    ++pos_;

                    if (pos_ >= n_)
                        return false;

                    const char e = s_[pos_];

                    if (e == 'u')
                    {
                        if (n_ - pos_ < 5)
                            return false;

                        for (int i = 1; i <= 4; ++i)
                            if (!IsHex(s_[pos_ + i]))
                                return false;

                        pos_ += 5;
                        continue;
                    }

                    if (!std::strchr("\"\\/bfnrt", e))
                        return false;

                    ++pos_;
                    continue;
                }

                const size_t length = Utf8SequenceLength(reinterpret_cast<const unsigned char*>(s_ + pos_), n_ - pos_);

                if (length == 0)
                    return false;

                pos_ += length;
            }

            return false;
        }

        bool ValueAt(int depth)
        {
            SkipSpace();

            if (pos_ >= n_)
                return false;

            switch (s_[pos_])
            {
                case '{': return Container(depth, '}', true);
                case '[': return Container(depth, ']', false);
                case '"': return String();
                case 't': return Literal("true");
                case 'f': return Literal("false");
                case 'n': return Literal("null");
                default:  return Number();
            }
        }

        bool Container(int depth, char close, bool isObject)
        {
            if (depth >= kMaxDepth)
                return false;

            ++pos_; // the opening bracket
            SkipSpace();

            if (Peek(close))
            {
                ++pos_;
                return true;
            }

            for (;;)
            {
                if (isObject)
                {
                    SkipSpace();

                    if (!String())
                        return false;

                    SkipSpace();

                    if (!Peek(':'))
                        return false;

                    ++pos_;
                }

                if (!ValueAt(depth + 1))
                    return false;

                SkipSpace();

                if (Peek(','))
                {
                    ++pos_;
                    continue;
                }

                if (Peek(close))
                {
                    ++pos_;
                    return true;
                }

                return false;
            }
        }
    };
}

void AppendString(std::string& out, const std::string& value)
{
    out += '"';

    const auto*  bytes = reinterpret_cast<const unsigned char*>(value.data());
    const size_t n     = value.size();

    for (size_t i = 0; i < n;)
    {
        const unsigned char c = bytes[i];

        switch (c)
        {
            case '"':  out += "\\\""; ++i; continue;
            case '\\': out += "\\\\"; ++i; continue;
            case '\n': out += "\\n";  ++i; continue;
            case '\r': out += "\\r";  ++i; continue;
            case '\t': out += "\\t";  ++i; continue;
            default: break;
        }

        if (c < 0x20)
        {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "\\u%04x", c);
            out += buf;
            ++i;
            continue;
        }

        const size_t length = Utf8SequenceLength(bytes + i, n - i);

        if (length == 0)
        {
            out += "\xEF\xBF\xBD"; // U+FFFD
            ++i;
            continue;
        }

        out.append(value, i, length);
        i += length;
    }

    out += '"';
}

static void AppendKey(std::string& out, const char* key)
{
    out += '"';
    out += key;
    out += "\":";
}

void AppendKeyString(std::string& out, const char* key, const std::string& value)
{
    AppendKey(out, key);
    AppendString(out, value);
    out += ',';
}

void AppendKeyStringIfAny(std::string& out, const char* key, const std::string& value)
{
    if (!value.empty())
        AppendKeyString(out, key, value);
}

void AppendKeyNumber(std::string& out, const char* key, long long value)
{
    AppendKey(out, key);
    out += std::to_string(value);
    out += ',';
}

void AppendKeyDouble(std::string& out, const char* key, double value)
{
    AppendKey(out, key);

    if (!std::isfinite(value))
        value = 0;

    char buf[64];
    const auto result = std::to_chars(buf, buf + sizeof(buf), value);
    out.append(buf, result.ptr);
    out += ',';
}

void AppendKeyBool(std::string& out, const char* key, bool value)
{
    AppendKey(out, key);
    out += value ? "true," : "false,";
}

void AppendKeyRaw(std::string& out, const char* key, const std::string& raw)
{
    if (raw.empty())
        return;

    AppendKey(out, key);
    out += raw;
    out += ',';
}

void AppendKeyNull(std::string& out, const char* key)
{
    AppendKey(out, key);
    out += "null,";
}

static void Close(std::string& out, char close)
{
    if (!out.empty() && out.back() == ',')
        out.back() = close;
    else
        out += close;
}

void CloseObject(std::string& out) { Close(out, '}'); }
void CloseArray(std::string& out)  { Close(out, ']'); }

std::string FormatUtc(long long unixMilliseconds)
{
    long long seconds = unixMilliseconds / 1000;
    int       millis  = static_cast<int>(unixMilliseconds % 1000);

    if (millis < 0)
    {
        millis += 1000;
        --seconds;
    }

    // Days since the epoch to a civil date (Howard Hinnant's algorithm), so no
    // gmtime and its thread-safety variants are needed.
    long long days = seconds / 86400;
    long long rem  = seconds % 86400;

    if (rem < 0)
    {
        rem += 86400;
        --days;
    }

    days += 719468;
    const long long era = (days >= 0 ? days : days - 146096) / 146097;
    const unsigned  doe = static_cast<unsigned>(days - era * 146097);
    const unsigned  yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned  doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned  mp  = (5 * doy + 2) / 153;
    const unsigned  day = doy - (153 * mp + 2) / 5 + 1;
    const unsigned  mon = mp < 10 ? mp + 3 : mp - 9;
    const long long yr  = static_cast<long long>(yoe) + era * 400 + (mon <= 2 ? 1 : 0);

    char buf[40];
    std::snprintf(buf, sizeof(buf), "%04lld-%02u-%02uT%02d:%02d:%02d.%03dZ",
        yr, mon, day,
        static_cast<int>(rem / 3600), static_cast<int>((rem / 60) % 60), static_cast<int>(rem % 60),
        millis);

    return buf;
}

long long NowUnixMilliseconds()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

bool IsObject(const char* text)
{
    return text && Validator(text).Object();
}

std::string NewGuid()
{
    static std::mutex      mutex;
    static std::mt19937_64 engine = []
    {
        std::random_device device;
        std::seed_seq seed{ device(), device(), device(), device(),
            static_cast<unsigned>(NowUnixMilliseconds()),
            static_cast<unsigned>(NowUnixMilliseconds() >> 32) };
        return std::mt19937_64(seed);
    }();

    unsigned char bytes[16];

    {
        std::lock_guard<std::mutex> lock(mutex);
        const unsigned long long a = engine();
        const unsigned long long b = engine();
        std::memcpy(bytes, &a, 8);
        std::memcpy(bytes + 8, &b, 8);
    }

    bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0F) | 0x40); // version 4
    bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3F) | 0x80); // RFC 4122 variant

    char buf[40];
    std::snprintf(buf, sizeof(buf),
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7],
        bytes[8], bytes[9], bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);

    return buf;
}

} // namespace json
