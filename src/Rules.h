// Copyright (C) 2026 CraftKal
// SPDX-License-Identifier: GPL-3.0-only
//
// Model aturan dan preset. Preset disimpan sebagai JSON agar aplikasi baru
// bisa ditambahkan tanpa membangun ulang program.

#pragma once

#include <algorithm>
#include <cwctype>
#include <string>
#include <vector>

#include "Json.h"

struct RuleConditions {
    bool mic = false;
    bool audio = false;
    std::vector<std::wstring> processes;
    std::vector<std::wstring> titles;
    std::wstring match = L"all";
};

struct Rule {
    std::wstring id;
    std::wstring name;
    std::wstring preset;
    std::vector<std::wstring> apps;
    bool enabled = true;
    bool useMaster = true;
    float target = 0.20f;
    int delayMs = 3000;
    bool restore = true;
    int priority = 0;
    RuleConditions cond;
};

struct Preset {
    std::wstring id;
    std::wstring name;
    std::wstring note;
    std::vector<std::wstring> apps;
    float target = 0.20f;
    int delayMs = 3000;
    bool restore = true;
    bool useMaster = true;
    RuleConditions cond;
};

namespace rules {

inline std::wstring Lower(std::wstring s) {
    for (wchar_t& c : s) c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

inline bool ContainsNoCase(const std::wstring& haystack, const std::wstring& needle) {
    if (needle.empty()) return true;
    return Lower(haystack).find(Lower(needle)) != std::wstring::npos;
}

inline std::vector<std::wstring> Strings(const json::Value* value, bool lower) {
    std::vector<std::wstring> out;
    if (!value || value->type != json::Type::Array) return out;
    for (const json::Value& item : value->items) {
        if (item.type != json::Type::String || item.text.empty()) continue;
        out.push_back(lower ? Lower(item.text) : item.text);
    }
    return out;
}

inline json::Value ToArray(const std::vector<std::wstring>& values) {
    json::Value array = json::MakeArray();
    for (const std::wstring& value : values) json::Push(array, json::MakeString(value));
    return array;
}

inline RuleConditions ConditionsFromJson(const json::Value* value) {
    RuleConditions cond;
    if (!value || value->type != json::Type::Object) return cond;
    cond.mic = value->Find(L"mic") ? value->Find(L"mic")->AsBool(false) : false;
    cond.audio = value->Find(L"audio") ? value->Find(L"audio")->AsBool(false) : false;
    cond.processes = Strings(value->Find(L"processes"), true);
    cond.titles = Strings(value->Find(L"titles"), false);
    if (const json::Value* match = value->Find(L"match")) {
        const std::wstring m = Lower(match->AsString(L"all"));
        cond.match = (m == L"any") ? L"any" : L"all";
    }
    return cond;
}

inline json::Value ConditionsToJson(const RuleConditions& cond) {
    json::Value value = json::MakeObject();
    json::Put(value, L"mic", json::MakeBool(cond.mic));
    json::Put(value, L"audio", json::MakeBool(cond.audio));
    json::Put(value, L"processes", ToArray(cond.processes));
    json::Put(value, L"titles", ToArray(cond.titles));
    json::Put(value, L"match", json::MakeString(cond.match == L"any" ? L"any" : L"all"));
    return value;
}

inline bool FromJson(const json::Value& value, Rule& rule) {
    if (value.type != json::Type::Object) return false;
    if (const json::Value* v = value.Find(L"id")) rule.id = v->AsString();
    if (const json::Value* v = value.Find(L"name")) rule.name = v->AsString();
    if (const json::Value* v = value.Find(L"preset")) rule.preset = v->AsString();
    rule.apps = Strings(value.Find(L"apps"), true);
    if (const json::Value* v = value.Find(L"enabled")) rule.enabled = v->AsBool(true);
    if (const json::Value* v = value.Find(L"useMaster")) rule.useMaster = v->AsBool(true);
    if (const json::Value* v = value.Find(L"target"))
        rule.target = static_cast<float>(std::clamp(v->AsNumber(20.0), 0.0, 100.0) / 100.0);
    if (const json::Value* v = value.Find(L"delayMs"))
        rule.delayMs = static_cast<int>(std::clamp(v->AsNumber(3000.0), 0.0, 60000.0));
    if (const json::Value* v = value.Find(L"restore")) rule.restore = v->AsBool(true);
    if (const json::Value* v = value.Find(L"priority"))
        rule.priority = static_cast<int>(std::clamp(v->AsNumber(0.0), -100.0, 100.0));
    rule.cond = ConditionsFromJson(value.Find(L"conditions"));
    return !rule.apps.empty();
}

inline json::Value ToJson(const Rule& rule) {
    json::Value value = json::MakeObject();
    json::Put(value, L"id", json::MakeString(rule.id));
    json::Put(value, L"name", json::MakeString(rule.name));
    json::Put(value, L"preset", json::MakeString(rule.preset));
    json::Put(value, L"apps", ToArray(rule.apps));
    json::Put(value, L"enabled", json::MakeBool(rule.enabled));
    json::Put(value, L"useMaster", json::MakeBool(rule.useMaster));
    json::Put(value, L"target", json::MakeNumber(std::lround(rule.target * 1000.0) / 10.0));
    json::Put(value, L"delayMs", json::MakeNumber(rule.delayMs));
    json::Put(value, L"restore", json::MakeBool(rule.restore));
    json::Put(value, L"priority", json::MakeNumber(rule.priority));
    json::Put(value, L"conditions", ConditionsToJson(rule.cond));
    return value;
}

inline bool FromJson(const json::Value& value, Preset& preset) {
    if (value.type != json::Type::Object) return false;
    if (const json::Value* v = value.Find(L"id")) preset.id = v->AsString();
    if (const json::Value* v = value.Find(L"name")) preset.name = v->AsString();
    if (const json::Value* v = value.Find(L"note")) preset.note = v->AsString();
    preset.apps = Strings(value.Find(L"apps"), true);
    if (preset.apps.empty()) return false;
    if (const json::Value* v = value.Find(L"target"))
        preset.target = static_cast<float>(std::clamp(v->AsNumber(20.0), 0.0, 100.0) / 100.0);
    if (const json::Value* v = value.Find(L"delayMs"))
        preset.delayMs = static_cast<int>(std::clamp(v->AsNumber(3000.0), 0.0, 60000.0));
    if (const json::Value* v = value.Find(L"restore")) preset.restore = v->AsBool(true);
    if (const json::Value* v = value.Find(L"useMaster")) preset.useMaster = v->AsBool(true);
    preset.cond = ConditionsFromJson(value.Find(L"conditions"));
    return true;
}

inline std::vector<Preset> ParsePresets(const std::wstring& text, bool* ok) {
    std::vector<Preset> out;
    json::Value root;
    if (!json::Parse(text, root)) {
        if (ok) *ok = false;
        return out;
    }

    const json::Value* list = &root;
    if (root.type == json::Type::Object) list = root.Find(L"presets");
    if (!list || list->type != json::Type::Array) {
        if (ok) *ok = false;
        return out;
    }

    for (const json::Value& item : list->items) {
        Preset preset;
        if (FromJson(item, preset)) out.push_back(std::move(preset));
    }
    if (ok) *ok = !out.empty();
    return out;
}

inline const wchar_t* DefaultPresetsJson() {
    return LR"json({
  "version": 1,
  "presets": [
    {
      "id": "discord",
      "name": "Discord",
      "note": "Panggilan suara atau video",
      "apps": ["discord.exe"],
      "target": 20,
      "delayMs": 3000,
      "restore": true,
      "useMaster": true,
      "conditions": { "mic": true, "audio": true, "match": "all", "processes": [], "titles": [] }
    },
    {
      "id": "zoom",
      "name": "Zoom",
      "note": "Meeting aktif lewat CptHost atau mikrofon",
      "apps": ["zoom.exe"],
      "target": 20,
      "delayMs": 3000,
      "restore": true,
      "useMaster": true,
      "conditions": { "mic": true, "audio": false, "match": "any", "processes": ["cphost.exe"], "titles": [] }
    },
    {
      "id": "teams",
      "name": "Microsoft Teams",
      "note": "Panggilan mikrofon dan suara keluar",
      "apps": ["ms-teams.exe", "teams.exe"],
      "target": 20,
      "delayMs": 3000,
      "restore": true,
      "useMaster": true,
      "conditions": { "mic": true, "audio": true, "match": "all", "processes": [], "titles": [] }
    },
    {
      "id": "meet",
      "name": "Google Meet (browser)",
      "note": "Judul jendela Meet dan mikrofon aktif",
      "apps": ["chrome.exe", "msedge.exe", "firefox.exe"],
      "target": 20,
      "delayMs": 3000,
      "restore": true,
      "useMaster": true,
      "conditions": { "mic": true, "audio": false, "match": "all", "processes": [], "titles": ["Meet"] }
    },
    {
      "id": "skype",
      "name": "Skype",
      "note": "Panggilan suara atau video",
      "apps": ["skype.exe"],
      "target": 20,
      "delayMs": 3000,
      "restore": true,
      "useMaster": true,
      "conditions": { "mic": true, "audio": true, "match": "all", "processes": [], "titles": [] }
    },
    {
      "id": "slack",
      "name": "Slack (Huddle)",
      "note": "Huddle dengan mikrofon aktif",
      "apps": ["slack.exe"],
      "target": 20,
      "delayMs": 3000,
      "restore": true,
      "useMaster": true,
      "conditions": { "mic": true, "audio": true, "match": "all", "processes": [], "titles": [] }
    },
    {
      "id": "webex",
      "name": "Webex",
      "note": "Meeting Webex aktif",
      "apps": ["webex.exe", "webexmta.exe"],
      "target": 20,
      "delayMs": 3000,
      "restore": true,
      "useMaster": true,
      "conditions": { "mic": true, "audio": true, "match": "all", "processes": [], "titles": [] }
    },
    {
      "id": "telegram",
      "name": "Telegram",
      "note": "Panggilan suara atau video",
      "apps": ["telegram.exe"],
      "target": 20,
      "delayMs": 3000,
      "restore": true,
      "useMaster": true,
      "conditions": { "mic": true, "audio": true, "match": "all", "processes": [], "titles": [] }
    },
    {
      "id": "whatsapp",
      "name": "WhatsApp Desktop",
      "note": "Panggilan suara atau video",
      "apps": ["whatsapp.exe"],
      "target": 20,
      "delayMs": 3000,
      "restore": true,
      "useMaster": true,
      "conditions": { "mic": true, "audio": true, "match": "all", "processes": [], "titles": [] }
    }
  ]
})json";
}

}  // namespace rules
