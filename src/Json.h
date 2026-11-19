// Copyright (C) 2026 CraftKal
// SPDX-License-Identifier: GPL-3.0-only
//
// Pembaca dan penulis JSON kecil. Skema config dan preset sepenuhnya diketahui
// di sini, jadi pustaka JSON umum tidak diperlukan dan tidak menambah DLL.

#pragma once

#include <cmath>
#include <cstdlib>
#include <cwchar>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace json {

enum class Type { Null, Bool, Number, String, Array, Object };

struct Value {
    Type type = Type::Null;
    bool boolean = false;
    double number = 0.0;
    std::wstring text;
    std::vector<Value> items;
    std::map<std::wstring, Value> fields;

    const Value* Find(const std::wstring& key) const {
        if (type != Type::Object) return nullptr;
        auto it = fields.find(key);
        return it == fields.end() ? nullptr : &it->second;
    }

    const Value* At(size_t index) const {
        if (type != Type::Array || index >= items.size()) return nullptr;
        return &items[index];
    }

    std::wstring AsString(const std::wstring& fallback = std::wstring()) const {
        return type == Type::String ? text : fallback;
    }

    double AsNumber(double fallback = 0.0) const {
        return type == Type::Number ? number : fallback;
    }

    bool AsBool(bool fallback = false) const {
        return type == Type::Bool ? boolean : fallback;
    }
};

inline Value MakeBool(bool v) {
    Value x;
    x.type = Type::Bool;
    x.boolean = v;
    return x;
}

inline Value MakeNumber(double v) {
    Value x;
    x.type = Type::Number;
    x.number = v;
    return x;
}

inline Value MakeString(std::wstring v) {
    Value x;
    x.type = Type::String;
    x.text = std::move(v);
    return x;
}

inline Value MakeArray() {
    Value x;
    x.type = Type::Array;
    return x;
}

inline Value MakeObject() {
    Value x;
    x.type = Type::Object;
    return x;
}

inline Value& Put(Value& object, const std::wstring& key, Value value) {
    object.type = Type::Object;
    return object.fields[key] = std::move(value);
}

inline void Push(Value& array, Value value) {
    array.type = Type::Array;
    array.items.push_back(std::move(value));
}

namespace detail {

class Parser {
public:
    explicit Parser(const std::wstring& text) : text_(text) {}

    bool Run(Value& out, std::wstring* error) {
        SkipSpace();
        if (!ParseValue(out, 0)) {
            if (error) *error = error_;
            return false;
        }
        SkipSpace();
        if (pos_ != text_.size()) {
            Fail(L"teks tambahan setelah JSON");
            if (error) *error = error_;
            return false;
        }
        return true;
    }

private:
    void SkipSpace() {
        while (pos_ < text_.size()) {
            const wchar_t c = text_[pos_];
            if (c != L' ' && c != L'\t' && c != L'\r' && c != L'\n') break;
            ++pos_;
        }
    }

    bool Fail(const wchar_t* message) {
        if (error_.empty()) {
            error_ = message;
            error_ += L" (posisi ";
            error_ += std::to_wstring(pos_);
            error_ += L")";
        }
        return false;
    }

    bool ParseValue(Value& out, int depth) {
        if (depth > 64) return Fail(L"JSON terlalu dalam");
        SkipSpace();
        if (pos_ >= text_.size()) return Fail(L"JSON tidak lengkap");

        switch (text_[pos_]) {
            case L'{': return ParseObject(out, depth + 1);
            case L'[': return ParseArray(out, depth + 1);
            case L'"': {
                std::wstring s;
                if (!ParseString(s)) return false;
                out = MakeString(std::move(s));
                return true;
            }
            case L't':
                if (text_.compare(pos_, 4, L"true") != 0) return Fail(L"nilai tidak dikenal");
                pos_ += 4;
                out = MakeBool(true);
                return true;
            case L'f':
                if (text_.compare(pos_, 5, L"false") != 0) return Fail(L"nilai tidak dikenal");
                pos_ += 5;
                out = MakeBool(false);
                return true;
            case L'n':
                if (text_.compare(pos_, 4, L"null") != 0) return Fail(L"nilai tidak dikenal");
                pos_ += 4;
                out = Value{};
                return true;
            default: return ParseNumber(out);
        }
    }

    bool ParseNumber(Value& out) {
        const size_t start = pos_;
        if (pos_ < text_.size() && text_[pos_] == L'-') ++pos_;
        while (pos_ < text_.size() && text_[pos_] >= L'0' && text_[pos_] <= L'9') ++pos_;
        if (pos_ < text_.size() && text_[pos_] == L'.') {
            ++pos_;
            while (pos_ < text_.size() && text_[pos_] >= L'0' && text_[pos_] <= L'9') ++pos_;
        }
        if (pos_ < text_.size() && (text_[pos_] == L'e' || text_[pos_] == L'E')) {
            ++pos_;
            if (pos_ < text_.size() && (text_[pos_] == L'+' || text_[pos_] == L'-')) ++pos_;
            while (pos_ < text_.size() && text_[pos_] >= L'0' && text_[pos_] <= L'9') ++pos_;
        }
        if (pos_ == start) return Fail(L"angka tidak valid");

        const std::wstring token = text_.substr(start, pos_ - start);
        wchar_t* end = nullptr;
        const double v = std::wcstod(token.c_str(), &end);
        if (!end || *end != L'\0') return Fail(L"angka tidak valid");
        out = MakeNumber(v);
        return true;
    }

    bool ReadHex4(unsigned& value) {
        if (pos_ + 4 > text_.size()) return Fail(L"escape unicode tidak lengkap");
        value = 0;
        for (int i = 0; i < 4; ++i) {
            const wchar_t c = text_[pos_++];
            int d = -1;
            if (c >= L'0' && c <= L'9') d = c - L'0';
            else if (c >= L'a' && c <= L'f') d = c - L'a' + 10;
            else if (c >= L'A' && c <= L'F') d = c - L'A' + 10;
            if (d < 0) return Fail(L"escape unicode tidak valid");
            value = value * 16 + static_cast<unsigned>(d);
        }
        return true;
    }

    bool ParseString(std::wstring& out) {
        if (pos_ >= text_.size() || text_[pos_] != L'"') return Fail(L"string tidak diawali kutip");
        ++pos_;
        out.clear();
        while (pos_ < text_.size()) {
            wchar_t c = text_[pos_++];
            if (c == L'"') return true;
            if (c == L'\\') {
                if (pos_ >= text_.size()) return Fail(L"escape tidak lengkap");
                const wchar_t e = text_[pos_++];
                switch (e) {
                    case L'"': out += L'"'; break;
                    case L'\\': out += L'\\'; break;
                    case L'/': out += L'/'; break;
                    case L'b': out += L'\b'; break;
                    case L'f': out += L'\f'; break;
                    case L'n': out += L'\n'; break;
                    case L'r': out += L'\r'; break;
                    case L't': out += L'\t'; break;
                    case L'u': {
                        unsigned cp = 0;
                        if (!ReadHex4(cp)) return false;
                        out += static_cast<wchar_t>(cp);
                        break;
                    }
                    default: return Fail(L"escape tidak dikenal");
                }
                continue;
            }
            if (c < 0x20) return Fail(L"karakter kontrol di dalam string");
            out += c;
        }
        return Fail(L"string tidak ditutup");
    }

    bool ParseArray(Value& out, int depth) {
        ++pos_;
        out = MakeArray();
        SkipSpace();
        if (pos_ < text_.size() && text_[pos_] == L']') {
            ++pos_;
            return true;
        }
        while (true) {
            Value item;
            if (!ParseValue(item, depth)) return false;
            Push(out, std::move(item));
            SkipSpace();
            if (pos_ >= text_.size()) return Fail(L"array tidak ditutup");
            if (text_[pos_] == L']') {
                ++pos_;
                return true;
            }
            if (text_[pos_] != L',') return Fail(L"array butuh koma");
            ++pos_;
        }
    }

    bool ParseObject(Value& out, int depth) {
        ++pos_;
        out = MakeObject();
        SkipSpace();
        if (pos_ < text_.size() && text_[pos_] == L'}') {
            ++pos_;
            return true;
        }
        while (true) {
            SkipSpace();
            std::wstring key;
            if (!ParseString(key)) return false;
            SkipSpace();
            if (pos_ >= text_.size() || text_[pos_] != L':') return Fail(L"objek butuh titik dua");
            ++pos_;
            Value item;
            if (!ParseValue(item, depth)) return false;
            out.fields[std::move(key)] = std::move(item);
            SkipSpace();
            if (pos_ >= text_.size()) return Fail(L"objek tidak ditutup");
            if (text_[pos_] == L'}') {
                ++pos_;
                return true;
            }
            if (text_[pos_] != L',') return Fail(L"objek butuh koma");
            ++pos_;
        }
    }

    const std::wstring& text_;
    size_t pos_ = 0;
    std::wstring error_;
};

}  // namespace detail

inline bool Parse(const std::wstring& text, Value& out, std::wstring* error = nullptr) {
    detail::Parser parser(text);
    return parser.Run(out, error);
}

namespace detail {

inline void EscapeTo(const std::wstring& s, std::wstring& out) {
    out += L'"';
    for (const wchar_t c : s) {
        switch (c) {
            case L'"': out += L"\\\""; break;
            case L'\\': out += L"\\\\"; break;
            case L'\b': out += L"\\b"; break;
            case L'\f': out += L"\\f"; break;
            case L'\n': out += L"\\n"; break;
            case L'\r': out += L"\\r"; break;
            case L'\t': out += L"\\t"; break;
            default:
                if (c < 0x20) {
                    wchar_t buf[8];
                    swprintf(buf, 8, L"\\u%04x", static_cast<unsigned>(c));
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    out += L'"';
}

inline void Indent(std::wstring& out, int depth) {
    for (int i = 0; i < depth; ++i) out += L"  ";
}

inline void Write(const Value& v, std::wstring& out, int depth) {
    switch (v.type) {
        case Type::Null: out += L"null"; break;
        case Type::Bool: out += v.boolean ? L"true" : L"false"; break;
        case Type::Number: {
            wchar_t buf[40];
            if (std::fabs(v.number) < 1e15 && v.number == std::floor(v.number))
                swprintf(buf, 40, L"%lld", static_cast<long long>(v.number));
            else
                swprintf(buf, 40, L"%.10g", v.number);
            out += buf;
            break;
        }
        case Type::String: EscapeTo(v.text, out); break;
        case Type::Array: {
            if (v.items.empty()) {
                out += L"[]";
                break;
            }
            out += L"[\n";
            for (size_t i = 0; i < v.items.size(); ++i) {
                Indent(out, depth + 1);
                Write(v.items[i], out, depth + 1);
                if (i + 1 < v.items.size()) out += L',';
                out += L'\n';
            }
            Indent(out, depth);
            out += L']';
            break;
        }
        case Type::Object: {
            if (v.fields.empty()) {
                out += L"{}";
                break;
            }
            out += L"{\n";
            size_t i = 0;
            for (const auto& kv : v.fields) {
                Indent(out, depth + 1);
                EscapeTo(kv.first, out);
                out += L": ";
                Write(kv.second, out, depth + 1);
                if (++i < v.fields.size()) out += L',';
                out += L'\n';
            }
            Indent(out, depth);
            out += L'}';
            break;
        }
    }
}

}  // namespace detail

inline std::wstring Stringify(const Value& value) {
    std::wstring out;
    detail::Write(value, out, 0);
    return out;
}

}  // namespace json
