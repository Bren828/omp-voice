// ============================================================
//  VoiceChat — tiny INI reader (req. H validation rule)
//  File: shared/ini.h
//
//  Header-only. Sections + key=value, ';'/'#' comments (whole-line and
//  inline). Typed getters CLAMP/validate: an out-of-range or unparsable
//  value logs a warning and returns the default — it never throws and
//  never aborts (req. H: "invalid value -> warning + default, do NOT crash
//  the relay"). Missing file => all defaults (zero-config).
// ============================================================
#pragma once

#include <string>
#include <map>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cstdio>
#include <cctype>

namespace vc {

class Ini {
public:
    // Returns false if the file is absent (caller proceeds with defaults).
    bool load(const std::string& path)
    {
        std::ifstream f(path);
        if (!f.is_open()) return false;
        std::string line, section;
        while (std::getline(f, line)) {
            line = stripComment(line);
            std::string t = trim(line);
            if (t.empty()) continue;
            if (t.front() == '[' && t.back() == ']') {
                section = lower(trim(t.substr(1, t.size() - 2)));
                continue;
            }
            auto eq = t.find('=');
            if (eq == std::string::npos) continue;
            std::string k = lower(trim(t.substr(0, eq)));
            std::string v = trim(t.substr(eq + 1));
            m_[section][k] = v;
        }
        return true;
    }

    std::string str(const char* sec, const char* key, const std::string& def) const
    {
        auto s = m_.find(lower(sec));
        if (s == m_.end()) return def;
        auto k = s->second.find(lower(key));
        return k == s->second.end() ? def : k->second;
    }

    int getInt(const char* sec, const char* key, int def, int lo, int hi) const
    {
        std::string v = str(sec, key, "");
        if (v.empty()) return def;
        try {
            int x = std::stoi(v);
            if (x < lo || x > hi) return warn(sec, key, def, "out of range");
            return x;
        } catch (...) { return warn(sec, key, def, "not an integer"); }
    }

    float getFloat(const char* sec, const char* key, float def, float lo, float hi) const
    {
        std::string v = str(sec, key, "");
        if (v.empty()) return def;
        try {
            float x = std::stof(v);
            if (x < lo || x > hi) return warn(sec, key, def, "out of range");
            return x;
        } catch (...) { return warn(sec, key, def, "not a number"); }
    }

    bool getBool(const char* sec, const char* key, bool def) const
    {
        std::string v = lower(str(sec, key, ""));
        if (v.empty()) return def;
        if (v == "true" || v == "1" || v == "yes" || v == "on")  return true;
        if (v == "false"|| v == "0" || v == "no"  || v == "off") return false;
        return warn(sec, key, def, "not a boolean");
    }

    // For enums like mode=ptt|vad. Returns matched index or def.
    int getEnum(const char* sec, const char* key,
                std::initializer_list<const char*> opts, int def) const
    {
        std::string v = lower(str(sec, key, ""));
        if (v.empty()) return def;
        int i = 0;
        for (auto* o : opts) { if (v == o) return i; ++i; }
        return warn(sec, key, def, "unknown value");
    }

private:
    static std::string lower(std::string s)
    { std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c){ return (char)std::tolower(c); }); return s; }
    static std::string trim(const std::string& s)
    { size_t a = s.find_first_not_of(" \t\r\n"); size_t b = s.find_last_not_of(" \t\r\n");
      return a == std::string::npos ? "" : s.substr(a, b - a + 1); }
    static std::string stripComment(const std::string& s)
    { size_t p = s.find_first_of(";#"); return p == std::string::npos ? s : s.substr(0, p); }

    template<typename T>
    static T warn(const char* sec, const char* key, T def, const char* why)
    { std::fprintf(stderr, "[voice.ini] [%s] %s: %s, using default\n", sec, key, why); return def; }

    std::map<std::string, std::map<std::string, std::string>> m_;
};

} // namespace vc
