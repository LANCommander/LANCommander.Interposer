#pragma once

// The little JSON the Interposer writes and checks: the rich presence it hands
// the launcher, and the match statistics it reports. Bodies are built by
// appending "key":value, pairs and closing the object; nothing here allocates
// a document.
//
// Every writer appends a trailing comma; CloseObject / CloseArray replace it
// with the closing bracket, or append the bracket to an empty container.

#include <string>

namespace json
{
    // "value", escaped. Invalid UTF-8 becomes U+FFFD, so the result is always
    // something a strict reader (System.Text.Json) accepts.
    void AppendString(std::string& out, const std::string& value);

    // "key":"value",
    void AppendKeyString(std::string& out, const char* key, const std::string& value);

    // "key":"value", or nothing when value is empty.
    void AppendKeyStringIfAny(std::string& out, const char* key, const std::string& value);

    // "key":123,
    void AppendKeyNumber(std::string& out, const char* key, long long value);

    // "key":1.5,  -- shortest round-trip form, independent of the C locale.
    // A non-finite value is written as 0.
    void AppendKeyDouble(std::string& out, const char* key, double value);

    // "key":true,
    void AppendKeyBool(std::string& out, const char* key, bool value);

    // "key":<raw>, -- raw must already be valid JSON. Nothing when raw is empty.
    void AppendKeyRaw(std::string& out, const char* key, const std::string& raw);

    // "key":null,
    void AppendKeyNull(std::string& out, const char* key);

    // Replace the trailing comma with '}' (or ']'), or append it to "{" / "[".
    void CloseObject(std::string& out);
    void CloseArray(std::string& out);

    // A UTC instant as ISO 8601 with milliseconds: "2026-11-07T20:29:12.345Z".
    std::string FormatUtc(long long unixMilliseconds);

    // Milliseconds since the Unix epoch, now.
    long long NowUnixMilliseconds();

    // True when `text` is exactly one JSON object, by RFC 8259: no comments,
    // no trailing commas, no single quotes, strings valid UTF-8, nested no
    // deeper than 64. Whitespace around it is allowed.
    bool IsObject(const char* text);

    // A random (version 4) GUID, lower case with hyphens.
    std::string NewGuid();
}
