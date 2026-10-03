// Road Trip Overhaul - telemetry-SDK plugin for American Truck Simulator (Road Trip car mode; trucks and
// Euro Truck Simulator 2 optionally). Throttle-based automatic shifting, real 2H / 4H on 4x4 cars and
// surface-dependent tyre grip.
//
// Shifting: light throttle keeps the game's own (early) upshift points, more throttle moves the upshift point
// up smoothly, full throttle holds each gear until the next one pulls harder.
//
// The game computes each gear's shift range (min = downshift rpm, max = upshift rpm) in one function: it
// blends the engine's rpm_range_low_gear / rpm_range_high_gear by gear, leans toward rpm_range_power on hills
// and caps the result at 99 % of rpm_limit. This plugin hooks that function and, after the game's own result,
// raises only the upshift point. The downshift point is left alone, so coasting, braking and hills behave as
// stock and gears cannot hunt. Nothing in the game's vehicle data is changed.

#include <windows.h>
#include <tlhelp32.h>

#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "MinHook.h"
#include "common/scssdk_telemetry_common_configs.h"
#include "common/scssdk_telemetry_truck_common_channels.h"
#include "ini_upgrade.h"
#include "scssdk_telemetry.h"
#include "version.h"

namespace rto {

// ---------------------------------------------------------------- settings

struct Config {
    int enabled = 1;
    float lightThrottle = 0.25f;     // at or below: the game's own upshift points
    float fullThrottle = 0.95f;      // at or above: upshift near the redline
    float fullUpshift = 0.95f;       // full-throttle upshift point, as a fraction of the engine's rpm limit
    float releaseTime = 0.8f;        // seconds for the remembered throttle to fall from 1 to 0 after lifting
    float minGearTime = 1.5f;        // seconds after an upshift during which the next upshift needs the engine to rev up
    float gameAdaptive = 10.0f;      // value for the game's g_adaptive_shift (0 = leave the game's setting alone)
    int maxSkipPower = 1;            // most gears one upshift may jump while on the throttle (above light_throttle)
    int maxSkipLight = 2;            // ... at light throttle or after lifting off
    int carOnly = 1;                 // 1 = only in car mode (Road Trip cars), 0 = every vehicle
    int toggleVk = VK_INSERT;        // key that switches the plugin on/off in game (0 = no key)
    int fourWd = 1;                  // real 2H/4H on 4x4 cars (car mode)
    int lockDiffs = 1;               // 2H/4H cars: differentials always locked (V only switches 2H/4H)
    float gripGrass = 0.7f;          // car mode: tyre grip on grass (x the game's value)
    float gripDirt = 0.9f;           //   dirt, gravel, dirt roads
    float gripRoad = 1.0f;           //   paved roads and concrete
    char fourWdInclude[512] = "";    // vehicle ids that always get 2H/4H
    char fourWdExclude[512] = "";    // vehicle ids that never do
    int toggleMods = 0;              // modifiers that must be held with it (kModCtrl | kModShift | kModAlt)
    int log = 0;                     // 0 off (no log file), 1 shifts and status
};
const int kModCtrl = 1, kModShift = 2, kModAlt = 4;
static Config g_cfg;

static const char kDefaultIni[] =
    "[road_trip_overhaul]\n"
    "; 1 = on, 0 = off\n"
    "enabled=1\n"
    "; which vehicles it works on: car = only when driving a car (Road Trip), all = cars and trucks\n"
    "vehicles=car\n"
    "; key that switches the plugin on and off while you drive (a beep tells you which):\n"
    ";   a key name, optionally with ctrl+ / shift+ / alt+ in front, e.g. insert, scrolllock, pause, ctrl+shift+g, alt+f9\n"
    ";   (letters, digits, f1-f24, numpad0-9, insert, delete, home, end, pageup, pagedown); none = no key\n"
    "toggle_key=scrolllock\n"
    "; 4x4 cars (car mode): 1 = the diff lock key (V) switches real 2H / 4H - rear-wheel drive normally, front axle\n"
    ";   engaged in 4H (the F-150's '4H' light and the Bronco's 4x4 lever follow it); 0 = the game's default\n"
    ";   (always all-wheel drive). Applies to cars whose data drives both axles (Bronco, F-150), never to\n"
    ";   two-wheel-drive cars (Mustang). Tip: turn off the game's automatic differential lock option, or it may\n"
    ";   switch to 4H by itself.\n"
    "four_wd=1\n"
    "; 2H / 4H cars: 1 = differentials always locked (both modes), 0 = locked only in 4H (as the game does)\n"
    "lock_diffs=1\n"
    "; tyre grip per surface while you drive a car, as a multiple of the game's own grip (1 = unchanged). The full\n"
    ";   value applies to sliding / spinning grip; holding grip (parked, rolling) gets half the change, so cars\n"
    ";   can still park on a slope. Trucks are never affected.\n"
    "grip_grass=0.7\n"
    "; dirt, gravel and dirt roads\n"
    "grip_dirt=0.9\n"
    "; paved roads and concrete\n"
    "grip_road=1.0\n"
    "; vehicle ids that never get 2H/4H (e.g. a mod car that is really full-time AWD), comma separated. The id of\n"
    ";   the car you drive is in the log: 'vehicle configuration: ... (vehicle.ford.bronco_24)'\n"
    "four_wd_exclude=\n"
    "; vehicle ids that always get 2H/4H, even if their data drives only one axle. Built in already (no need to\n"
    ";   add them): LORD G350 pickup mod (vehicle.ford.350c)\n"
    "four_wd_include=\n"
    "; throttle (0..1) at or below which the game's own upshift points are used\n"
    "light_throttle=0.25\n"
    "; throttle (0..1) at or above which each gear is held to near the redline\n"
    "full_throttle=0.95\n"
    "; full throttle: each gear is held until the next gear pulls harder (worked out from the engine's torque\n"
    ";   curve), but never past this fraction of the engine's rpm limit (0.95 = 95 %)\n"
    "full_upshift=0.95\n"
    "; seconds the transmission takes to 'forget' a throttle you lifted off (stops early upshifts on short lifts)\n"
    "release_time=0.8\n"
    "; seconds after an upshift before the next upshift may come early (stops several gears in a row)\n"
    "min_gear_time=1.5\n"
    "; the game's own adaptive automatic mode, which lets the gearbox upshift when you lift off the throttle:\n"
    ";   10 = Eco (default: on, with this plugin deciding the shift points), 3 = Normal, 1.67 = Power,\n"
    ";   0 = leave the game's setting alone (no upshifts when you lift off)\n"
    "game_adaptive_mode=10\n"
    "; most gears a single upshift may jump on the throttle (1 = one gear at a time, as a real automatic does)\n"
    "max_upshift_power=1\n"
    "; most gears a single upshift may jump at light throttle or after lifting off (0 = no limit)\n"
    "max_upshift_light=2\n"
    "; 0 = no log file (default). For troubleshooting: 1 = log every shift\n"
    "log=0\n";

static float Clamp(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

// "ctrl+shift+g", "scrolllock", "f9", "none" -> virtual key + modifiers. False if not recognised.
static bool ParseHotkey(const char* text, int& vk, int& mods) {
    static const struct { const char* name; int vk; } kNamed[] = {
        {"scrolllock", VK_SCROLL}, {"pause", VK_PAUSE}, {"insert", VK_INSERT}, {"delete", VK_DELETE},
        {"home", VK_HOME}, {"end", VK_END}, {"pageup", VK_PRIOR}, {"pagedown", VK_NEXT},
        {"numlock", VK_NUMLOCK}, {"capslock", VK_CAPITAL}, {"tab", VK_TAB}, {"space", VK_SPACE},
        {"backspace", VK_BACK}, {"enter", VK_RETURN}, {"up", VK_UP}, {"down", VK_DOWN}, {"left", VK_LEFT},
        {"right", VK_RIGHT}, {"multiply", VK_MULTIPLY}, {"add", VK_ADD}, {"subtract", VK_SUBTRACT},
        {"decimal", VK_DECIMAL}, {"divide", VK_DIVIDE},
    };
    vk = 0, mods = 0;
    char buf[64];
    strncpy_s(buf, text, _TRUNCATE);
    for (char* c = buf; *c; c++) *c = (char)tolower((unsigned char)*c);
    if (!buf[0] || strcmp(buf, "none") == 0 || strcmp(buf, "0") == 0) return true;
    char* key = buf;
    for (char* plus; (plus = strchr(key, '+')) != nullptr; key = plus + 1) {
        *plus = 0;
        if (strcmp(key, "ctrl") == 0 || strcmp(key, "control") == 0) mods |= kModCtrl;
        else if (strcmp(key, "shift") == 0) mods |= kModShift;
        else if (strcmp(key, "alt") == 0) mods |= kModAlt;
        else return false;
    }
    size_t n = strlen(key);
    if (n == 1 && ((key[0] >= 'a' && key[0] <= 'z') || (key[0] >= '0' && key[0] <= '9'))) vk = toupper(key[0]);
    else if (key[0] == 'f' && n >= 2 && n <= 3 && atoi(key + 1) >= 1 && atoi(key + 1) <= 24) vk = VK_F1 + atoi(key + 1) - 1;
    else if (strncmp(key, "numpad", 6) == 0 && n == 7 && key[6] >= '0' && key[6] <= '9') vk = VK_NUMPAD0 + (key[6] - '0');
    else
        for (const auto& k : kNamed)
            if (strcmp(key, k.name) == 0) vk = k.vk;
    return vk != 0;
}

static float ReadFloat(const char* ini, const char* key, float def) {
    char buf[32];
    GetPrivateProfileStringA("road_trip_overhaul", key, "", buf, sizeof(buf), ini);
    return buf[0] ? (float)atof(buf) : def;
}

// Writes `text` to `path` through a temporary file (Windows line endings), so a crash never leaves a half-written ini.
static bool WriteFileAtomic(const std::string& path, const std::string& text) {
    std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        for (char c : text) {
            if (c == '\n') f << '\r';
            f << c;
        }
        if (!f.flush()) return false;
    }
    if (MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
    DeleteFileA(tmp.c_str());
    return false;
}

// Reads the ini, writing the defaults first if it does not exist, or bringing an older one up to date.
// Returns what was changed in the file ("" if nothing), for the log.
static std::string LoadConfig(const std::string& path) {
    std::string note;
    // Test builds were called Adaptive Shift: their settings carry over the first time.
    std::string old = path.substr(0, path.find_last_of("\\/") + 1) + "adaptive_shift.ini";
    if (GetFileAttributesA(path.c_str()) == INVALID_FILE_ATTRIBUTES &&
        GetFileAttributesA(old.c_str()) != INVALID_FILE_ATTRIBUTES) {
        std::ifstream f(old, std::ios::binary);
        std::stringstream text;
        text << f.rdbuf();
        f.close();
        std::string t = text.str();
        size_t at = t.find("[adaptive_shift]");
        if (at != std::string::npos) t.replace(at, 16, "[road_trip_overhaul]");
        if (WriteFileAtomic(path, t)) note = "settings taken over from adaptive_shift.ini; ";
    }
    if (GetFileAttributesA(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        WriteFileAtomic(path, kDefaultIni);
    } else {
        std::ifstream f(path, std::ios::binary);
        std::stringstream text;
        text << f.rdbuf();
        f.close();
        std::string upgraded;
        std::string changes = UpgradeIni(text.str(), kDefaultIni, upgraded);
        if (!changes.empty())
            note += WriteFileAtomic(path, upgraded) ? "updated road_trip_overhaul.ini: " + changes
                                                   : "could not update road_trip_overhaul.ini (" + changes + ")";
    }
    const char* ini = path.c_str();
    g_cfg.enabled = GetPrivateProfileIntA("road_trip_overhaul", "enabled", 1, ini);
    g_cfg.lightThrottle = Clamp(ReadFloat(ini, "light_throttle", 0.25f), 0.0f, 0.95f);
    g_cfg.fullThrottle = Clamp(ReadFloat(ini, "full_throttle", 0.95f), g_cfg.lightThrottle + 0.05f, 1.0f);
    g_cfg.fullUpshift = Clamp(ReadFloat(ini, "full_upshift", 0.95f), 0.5f, 0.99f);
    g_cfg.releaseTime = Clamp(ReadFloat(ini, "release_time", 0.8f), 0.0f, 10.0f);
    g_cfg.minGearTime = Clamp(ReadFloat(ini, "min_gear_time", 1.5f), 0.0f, 10.0f);
    g_cfg.gameAdaptive = Clamp(ReadFloat(ini, "game_adaptive_mode", 10.0f), 0.0f, 100.0f);
    g_cfg.maxSkipPower = GetPrivateProfileIntA("road_trip_overhaul", "max_upshift_power", 1, ini);
    g_cfg.maxSkipLight = GetPrivateProfileIntA("road_trip_overhaul", "max_upshift_light", 2, ini);
    char vehicles[16];
    GetPrivateProfileStringA("road_trip_overhaul", "vehicles", "car", vehicles, sizeof(vehicles), ini);
    g_cfg.carOnly = _stricmp(vehicles, "all") != 0;
    char key[64];
    GetPrivateProfileStringA("road_trip_overhaul", "toggle_key", "insert", key, sizeof(key), ini);
    if (!ParseHotkey(key, g_cfg.toggleVk, g_cfg.toggleMods)) {
        note += std::string(note.empty() ? "" : "; ") + "toggle_key '" + key + "' not recognised - no toggle key";
        g_cfg.toggleVk = 0, g_cfg.toggleMods = 0;
    }
    g_cfg.fourWd = GetPrivateProfileIntA("road_trip_overhaul", "four_wd", 1, ini);
    g_cfg.lockDiffs = GetPrivateProfileIntA("road_trip_overhaul", "lock_diffs", 1, ini);
    g_cfg.gripGrass = Clamp(ReadFloat(ini, "grip_grass", 0.7f), 0.1f, 2.0f);
    g_cfg.gripDirt = Clamp(ReadFloat(ini, "grip_dirt", 0.9f), 0.1f, 2.0f);
    g_cfg.gripRoad = Clamp(ReadFloat(ini, "grip_road", 1.0f), 0.1f, 2.0f);
    GetPrivateProfileStringA("road_trip_overhaul", "four_wd_include", "", g_cfg.fourWdInclude, sizeof(g_cfg.fourWdInclude), ini);
    GetPrivateProfileStringA("road_trip_overhaul", "four_wd_exclude", "", g_cfg.fourWdExclude, sizeof(g_cfg.fourWdExclude), ini);
    g_cfg.log = GetPrivateProfileIntA("road_trip_overhaul", "log", 0, ini);
    return note;
}

// ---------------------------------------------------------------- log

static FILE* g_log;

static void LogOpen(const std::string& path) { fopen_s(&g_log, path.c_str(), "w"); }

static void LogClose() {
    if (g_log) fclose(g_log);
    g_log = nullptr;
}

static void Log(const char* fmt, ...) {
    if (!g_log) return;
    fprintf(g_log, "[%10llu] ", GetTickCount64());
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

// ---------------------------------------------------------------- signature

static uintptr_t g_base, g_size;

// The game's per-gear shift range calculation: range* (this, range* out, int gear); out = {min, max} rpm.
static const char kShiftRangeSig[] =
    "RTOSIG:ShiftRange=48 89 5C 24 10 48 89 6C 24 18 56 57 41 56 48 81 EC D0 00 00 00 48 8B 41 10 48 8B F1 45 8B F0 "
    "48 8B EA 48 8B 88 ? ? ? ? E8 ? ? ? ? 48 8B D8";

// Finds `pattern` ("48 8B ? ..") in .text; returns the match if there is exactly one.
static uintptr_t FindUnique(const char* pattern) {
    std::vector<int> bytes;
    for (const char* p = strchr(pattern, '=') + 1; *p;) {
        while (*p == ' ') p++;
        if (!*p) break;
        if (*p == '?') {
            bytes.push_back(-1);
            p++;
        } else {
            bytes.push_back((int)strtoul(p, nullptr, 16));
            p += 2;
        }
    }
    auto nt = (IMAGE_NT_HEADERS64*)(g_base + ((IMAGE_DOS_HEADER*)g_base)->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    uintptr_t text = 0, size = 0;
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++)
        if (memcmp(sec->Name, ".text", 6) == 0) text = g_base + sec->VirtualAddress, size = sec->Misc.VirtualSize;
    if (!text || bytes.empty()) return 0;
    uintptr_t found = 0;
    int count = 0;
    const uint8_t* t = (const uint8_t*)text;
    for (size_t i = 0; i + bytes.size() <= size; i++) {
        if (t[i] != bytes[0]) continue;
        size_t k = 1;
        while (k < bytes.size() && (bytes[k] < 0 || t[i + k] == bytes[k])) k++;
        if (k == bytes.size() && ++count == 1) found = text + i;
    }
    return count == 1 ? found : 0;
}

// ---------------------------------------------------------------- the game's adaptive mode (g_adaptive_shift)
// The game has its own adaptive automatic, off by default and not in the options (a key cycles it: 0 disabled,
// 10 Eco, 3 Normal, 1.6667 Power; the value is the exponent of its throttle response). While it is off, the
// gear choice refuses every higher gear when the engine isn't pulling, so the gearbox never upshifts after you
// lift off. With it on (and no trailer attached) that check is skipped. The plugin sets it the same way the
// game's key does: through the setting's own vtable[1] set-from-string.

// Finds the setting object from its registration code:
//   lea rax, [obj + 8] ; mov edx, 0x20 ; lea r8, [name]   (the name is copied into obj + 8)
static uintptr_t FindGameSetting(const char* name) {
    auto nt = (IMAGE_NT_HEADERS64*)(g_base + ((IMAGE_DOS_HEADER*)g_base)->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    uintptr_t text = 0, size = 0;
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++)
        if (memcmp(sec->Name, ".text", 6) == 0) text = g_base + sec->VirtualAddress, size = sec->Misc.VirtualSize;
    size_t n = strlen(name);
    uintptr_t found = 0;
    int count = 0;
    const uint8_t* t = (const uint8_t*)text;
    for (size_t i = 0; text && i + 19 <= size; i++) {
        if (t[i] != 0x48 || t[i + 1] != 0x8D || t[i + 2] != 0x05 || t[i + 7] != 0xBA || t[i + 8] != 0x20 ||
            t[i + 9] || t[i + 10] || t[i + 11] || t[i + 12] != 0x4C || t[i + 13] != 0x8D || t[i + 14] != 0x05)
            continue;
        const char* str = (const char*)(text + i + 19 + *(const int32_t*)(t + i + 15));
        if ((uintptr_t)str < g_base || (uintptr_t)str + n + 1 > g_base + g_size || memcmp(str, name, n + 1) != 0) continue;
        uintptr_t obj = text + i + 7 + *(const int32_t*)(t + i + 3) - 8;
        if (memcmp((const char*)(obj + 8), name, n + 1) != 0) continue;  // registered: the name was copied in
        found = obj;
        count++;
    }
    return count == 1 ? found : 0;
}

// Calls setting->vtable[1](setting, value). POD only (__try).
static bool SetGameSetting(uintptr_t setting, const char* value) {
    __try {
        typedef void(__fastcall * SetFromString_t)(void* setting, const char* value);
        void** vt = *(void***)setting;
        if ((uintptr_t)vt < g_base || (uintptr_t)vt >= g_base + g_size) return false;
        ((SetFromString_t)vt[1])((void*)setting, value);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ---------------------------------------------------------------- throttle and vehicle

// The pedal is the driver's input throttle. The effective throttle is after the game's own limiting and stays
// low (about 0.23) even with the pedal floored, so it is only logged for comparison.
static volatile float g_pedal;       // input throttle (telemetry)
static volatile float g_effective;   // effective throttle (telemetry, log only)
static volatile float g_remembered;  // throttle the transmission acts on: rises at once, falls over release_time
static volatile float g_rpmLimit;    // engine rpm limit of the current vehicle (telemetry configuration)
static volatile float g_rpm;
static volatile float g_speed;  // m/s (telemetry)
static volatile float g_wheelSpin[8];  // wheel angular velocity, rotations per second (telemetry)
static float g_wheelLogTime;
// Last range per gear (for the shift log): the game's own result and the upshift point the plugin returned.
static volatile float g_gameMin[33], g_gameMax[33], g_ourMax[33];
// Forward gear ratios of the current vehicle (telemetry configuration), index 1..g_gearCount.
static float g_ratio[33];
static int g_gearCount;

// Full-throttle upshift point per gear, for the fastest acceleration: the rpm where the wheel torque in the
// next gear (after the shift) becomes higher than in this one, from the engine's torque curve and the gear ratios.
// Petrol engines that pull to the redline get points near full_upshift; engines whose torque falls away early
// (diesels) get much lower points. Read from the engine data (accessory_engine_data via the vehicle the game's
// shift-range function works on); 0 = unknown, use full_upshift * rpm limit.
static float g_bestUp[33];
static volatile uintptr_t g_shiftVehicle;  // vehicle passed to the shift-range function (this + kVehicleOff)
static volatile LONG g_shiftCalls;         // shift-range calls so far
static LONG g_curvePendingSince = -1;      // calls count at the last configuration change (-1 = none pending)
static uintptr_t g_curveVehicle;           // vehicle the points were worked out for
static float g_curveLimit;                 // and its rpm limit
static uint32_t g_vehicleOff = 0x10, g_engineOff = 0x1040;  // read from the ShiftRange signature
const uint32_t kAccData = 0x38;            // accessory -> data (valid if bit 31 of data+8 is set)
const uint32_t kEngRpmLimit = 0x234, kEngTorqueCurve = 0x1d0;  // accessory_engine_data (reflection)

// Copies the torque curve (rpm, factor pairs) of the vehicle's engine; returns the point count (0 = unreadable).
// The engine's rpm limit must match the telemetry one, so a wrong offset is never trusted. POD only (__try).
static int ReadTorqueCurve(uintptr_t vehicle, float limit, float* rpm, float* val, int max) {
    __try {
        uintptr_t acc = *(uintptr_t*)(vehicle + g_engineOff);
        if (!acc) return 0;
        uintptr_t data = *(uintptr_t*)(acc + kAccData);
        if (!data || !(*(uint32_t*)(data + 8) & 0x80000000u)) return 0;
        if (fabsf(*(float*)(data + kEngRpmLimit) - limit) > 1.0f) return 0;
        const float* pts = *(const float**)(data + kEngTorqueCurve + 8);
        int64_t n = *(int64_t*)(data + kEngTorqueCurve + 16);
        if (!pts || n < 2 || n > max) return 0;
        for (int i = 0; i < n; i++) {
            rpm[i] = pts[i * 2], val[i] = pts[i * 2 + 1];
            if (val[i] < 0 || val[i] > 5 || (i && rpm[i] <= rpm[i - 1])) return 0;
        }
        return (int)n;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

static float CurveAt(const float* rpm, const float* val, int n, float x) {
    if (x <= rpm[0]) return val[0];
    for (int i = 1; i < n; i++)
        if (x <= rpm[i]) return val[i - 1] + (val[i] - val[i - 1]) * (x - rpm[i - 1]) / (rpm[i] - rpm[i - 1]);
    return val[n - 1];
}

static void WorkOutBestUpshifts(uintptr_t vehicle) {
    memset(g_bestUp, 0, sizeof(g_bestUp));
    g_curveVehicle = vehicle, g_curveLimit = g_rpmLimit;
    float rpm[64], val[64];
    int n = ReadTorqueCurve(vehicle, g_rpmLimit, rpm, val, 64);
    if (!n) {
        Log("torque curve not readable - full-throttle upshifts at %.0f%% of the rpm limit", (double)g_cfg.fullUpshift * 100);
        return;
    }
    char line[640];
    int len = snprintf(line, sizeof(line), "torque curve:");
    for (int i = 0; i < n && len < (int)sizeof(line) - 24; i++) len += snprintf(line + len, sizeof(line) - len, " %.0f:%.2f", (double)rpm[i], (double)val[i]);
    Log("%s", line);
    float cap = g_rpmLimit * g_cfg.fullUpshift;
    float peak = rpm[0];  // start at the torque peak: never upshift below it
    for (int i = 1; i < n; i++)
        if (val[i] > CurveAt(rpm, val, n, peak)) peak = rpm[i];
    len = snprintf(line, sizeof(line), "full-throttle upshifts:");
    for (int g = 1; g < g_gearCount && g <= 32; g++) {
        float k = g_ratio[g + 1] > 0 ? g_ratio[g] / g_ratio[g + 1] : 0;
        float best = cap;
        if (k > 1.0f)
            for (float x = peak; x <= cap; x += 10)
                if (CurveAt(rpm, val, n, x) * k <= CurveAt(rpm, val, n, x / k)) {
                    best = x;
                    break;
                }
        g_bestUp[g] = best;
        len += snprintf(line + len, sizeof(line) - len, " %d:%.0f", g, (double)best);
    }
    Log("%s", line);
}

// Plateau upshift. Some engines (governed diesels) stop gaining rpm before the plugin's full-throttle upshift
// point; the gearbox would then wait in that gear for an rpm it can never reach. As a real automatic does, when
// the driver is on the throttle, the rpm is close to the upshift point and has stopped rising for kPlateauTime,
// the plugin lets it upshift now - if the next gear would still land at a sensible rpm.
const float kPlateauTime = 1.0f;       // seconds without the rpm rising
const float kPlateauRise = 0.01f;      // "rising" = more than 1 % above the reference
const float kPlateauNear = 0.85f;      // rpm at least 85 % of the upshift point
const float kPlateauLand = 0.6f;       // next gear must land at >= 60 % of the rpm limit
static float g_plateauRef, g_plateauTime;
static volatile int g_plateauGear;     // gear that may upshift now (0 = none)
static volatile int g_gear;

// ---------------------------------------------------------------- performance timing (log only)
// A run starts when the vehicle is at a standstill and the pedal goes to full throttle; it logs 0-60 mph,
// 0-100 km/h and the quarter mile, and is abandoned if the pedal drops below 0.9 first. The highest speed
// held at full throttle is logged when the pedal is released.

struct Run {
    bool active;
    double t, dist;                  // seconds, metres since the start
    double t60, t100, tQuarter, vQuarter;
};
static Run g_run;
static double g_wotTop;              // highest speed (m/s) in the current full-throttle stretch
static float g_wotTopRpm;
static int g_wotTopGear;
static double g_wotTime;             // seconds of the current full-throttle stretch

// ---------------------------------------------------------------- on/off key (toggle_key)
// Switches everything the plugin does on and off: shift points, skip limits and the game's adaptive mode, so
// "off" is the game's stock behaviour. Only while the game window has focus.

static volatile bool g_active = true;   // switched by the toggle key
static uintptr_t g_adaptiveSetting;
static scs_log_t g_gameLog;
static bool g_toggleWasDown;

// ---------------------------------------------------------------- vehicle mode (car / truck)
// The shift-range function loads the game core pointer ("mov rax, [rip+X]; mov rcx, [rax+0x31b0]"); the gameplay
// mode at core+0x2b30 reports the vehicle mode through its virtual +0xe0: 0 = truck, 2 = car (ATS Road Trip).
// The same call is what the game's own activation checks use.
const int kModeCar = 2;
static uintptr_t g_coreGlobal;          // address of the game core pointer
static volatile int g_mode = -2;        // -1 = unknown, -2 = not read yet
static volatile bool g_applies;         // plugin active for the current vehicle
static char g_vehicle[96] = "?";        // brand and name from the telemetry configuration
static char g_vehicleId[96] = "";       // unit id, e.g. vehicle.ford.bronco_24

static bool InModule(uintptr_t a) { return a >= g_base && a < g_base + g_size; }

static uintptr_t FindCoreGlobal(uintptr_t fn) {
    const uint8_t* p = (const uint8_t*)fn;
    for (int i = 0; fn && i < 0x600; i++)
        if (p[i] == 0x48 && p[i + 1] == 0x8B && p[i + 2] == 0x05 && p[i + 7] == 0x48 && p[i + 8] == 0x8B && p[i + 9] == 0x88 &&
            *(const uint32_t*)(p + i + 10) == 0x31B0) {
            uintptr_t g = fn + i + 7 + *(const int32_t*)(p + i + 3);
            return InModule(g) ? g : 0;
        }
    return 0;
}

// POD only (__try).
static int ReadVehicleMode() {
    if (!g_coreGlobal) return -1;
    __try {
        uintptr_t core = *(uintptr_t*)g_coreGlobal;
        if (!core) return -1;
        uintptr_t gm = *(uintptr_t*)(core + 0x2b30);
        if (!gm) return -1;
        uintptr_t* vt = *(uintptr_t**)gm;
        if (!InModule((uintptr_t)vt) || !InModule(vt[0xe0 / 8])) return -1;
        typedef int(__fastcall * Fn)(void*);
        int mode = ((Fn)vt[0xe0 / 8])((void*)gm);
        return mode >= 0 && mode < 16 ? mode : -1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

// The player's own value of g_adaptive_shift (the game's "adaptive automatic transmission" mode) is remembered
// before the plugin changes it and put back whenever the plugin stops applying, so the player's choice holds for
// trucks. The game reads the setting through a getter: inside the shift-range function it does
// "lea rcx,[setting]; call getter", which is where the getter is found (only if the lea is this same setting).
typedef float(__fastcall* SettingGet_t)(void* setting);
static SettingGet_t g_settingGet;
static float g_playerAdaptive = -1;     // the player's value (-1 = not read yet)
// The game saves g_adaptive_shift in the profile's config.cfg at moments of its own choosing - also while a car has
// the plugin's value set. So the player's own value is also kept in road_trip_overhaul.ini [state] player_adaptive_mode.
static std::string g_iniPath;

static void SavePlayerAdaptive(float v) {
    char value[32];
    snprintf(value, sizeof(value), "%g", (double)v);
    WritePrivateProfileStringA("state", "player_adaptive_mode", value, g_iniPath.c_str());
}

static float LoadPlayerAdaptive() {
    char value[32];
    GetPrivateProfileStringA("state", "player_adaptive_mode", "", value, sizeof(value), g_iniPath.c_str());
    return value[0] ? (float)atof(value) : -1.0f;
}
static float g_lastSeenAdaptive = -1;   // last value seen, to log changes

static void FindSettingGetter(uintptr_t fn) {
    const uint8_t* p = (const uint8_t*)fn;
    for (int i = 0; fn && g_adaptiveSetting && i < 0x600; i++)
        if (p[i] == 0x48 && p[i + 1] == 0x8D && p[i + 2] == 0x0D && p[i + 7] == 0xE8 &&
            fn + i + 7 + *(const int32_t*)(p + i + 3) == g_adaptiveSetting) {
            uintptr_t get = fn + i + 12 + *(const int32_t*)(p + i + 8);
            if (InModule(get)) g_settingGet = (SettingGet_t)get;
            return;
        }
}

// POD only (__try).
static float ReadGameAdaptive() {
    if (!g_settingGet || !g_adaptiveSetting) return -1;
    __try {
        return g_settingGet((void*)g_adaptiveSetting);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

static void SetGameAdaptive(float v) {
    char value[32];
    snprintf(value, sizeof(value), "%g", (double)v);
    bool set = SetGameSetting(g_adaptiveSetting, value);
    g_lastSeenAdaptive = v;
    Log("game adaptive mode (g_adaptive_shift) %s %s", set ? "set to" : "could not be set to", value);
}

static void ApplyGameAdaptive(bool on) {
    if (!g_adaptiveSetting) return;
    if (on) {
        float now = ReadGameAdaptive();
        if (now >= 0 && now != g_cfg.gameAdaptive) g_playerAdaptive = now;  // remember the player's value
        SetGameAdaptive(g_cfg.gameAdaptive);
    } else {
        SetGameAdaptive(g_playerAdaptive >= 0 ? g_playerAdaptive : 0.0f);  // back to the player's value
    }
}

// Logs changes the plugin didn't make (e.g. the game's options menu or the mode key) and keeps the player's value.
static void WatchGameAdaptive(bool pluginOwnsIt) {
    float now = ReadGameAdaptive();
    if (now < 0 || now == g_lastSeenAdaptive) return;
    Log("game adaptive mode (g_adaptive_shift) changed by the game: %g -> %g%s", (double)g_lastSeenAdaptive, (double)now,
        pluginOwnsIt ? " (kept as your setting for trucks)" : "");
    g_lastSeenAdaptive = now;
    g_playerAdaptive = now;
    SavePlayerAdaptive(now);
    if (pluginOwnsIt) SetGameAdaptive(g_cfg.gameAdaptive);  // the car keeps the plugin's mode
}

// Works out whether the plugin applies to the current vehicle; switches the game's adaptive mode with it.
static void UpdateApplies() {
    if (g_adaptiveSetting) WatchGameAdaptive(g_applies);
    int mode = ReadVehicleMode();
    if (mode != g_mode) {
        g_mode = mode;
        Log("vehicle mode %d (%s)", mode, mode == kModeCar ? "car" : mode == 0 ? "truck" : "unknown");
    }
    bool applies = g_cfg.enabled && g_active && (!g_cfg.carOnly || mode == kModeCar);
    if (applies == g_applies) return;
    g_applies = applies;
    if (g_cfg.gameAdaptive > 0) ApplyGameAdaptive(applies);
    Log("plugin %s for this vehicle", applies ? "ACTIVE" : "inactive (game's stock shifting)");
}

static bool KeyDown(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

// True once per press of vk with exactly `mods` held, while the game has focus.
static bool GameHasFocus();
static bool KeyPressed(int vk, int mods, bool& wasDown) {
    if (!vk) return false;
    int held = (KeyDown(VK_CONTROL) ? kModCtrl : 0) | (KeyDown(VK_SHIFT) ? kModShift : 0) | (KeyDown(VK_MENU) ? kModAlt : 0);
    bool down = KeyDown(vk) && held == mods && GameHasFocus();
    bool pressed = down && !wasDown;
    wasDown = down;
    return pressed;
}

static bool GameHasFocus() {
    DWORD pid = 0;
    HWND w = GetForegroundWindow();
    return w && GetWindowThreadProcessId(w, &pid) && pid == GetCurrentProcessId();
}

static void PollToggle() {
    if (KeyPressed(g_cfg.toggleVk, g_cfg.toggleMods, g_toggleWasDown)) {
        g_active = !g_active;
        MessageBeep(g_active ? MB_ICONASTERISK : MB_ICONHAND);
        const char* msg = g_active ? "[Road Trip Overhaul] ON" : "[Road Trip Overhaul] OFF (game's stock shifting)";
        if (g_gameLog) g_gameLog(SCS_LOG_TYPE_message, msg);
        Log("%s", msg);
    }
}

// ---------------------------------------------------------------- 2H / 4H (four_wd)
// The game has no 4WD selector: a 4x4 car's chassis powers both axles all the time, and the diff lock key only
// locks the differentials (the DLC shows it as "4H" on the F-150's dash and moves the Bronco's 4x4 lever). In car
// mode the plugin makes it real: front axle unpowered normally (2H, rear-wheel drive), powered while the diff lock
// is on (4H). The flags are the chassis data's powered_axle array (vehicle+0x200 -> chassis data, +0x520 array
// {+8 data, +0x10 count}, one bool per axle, front first) - the same values the SDK reports as wheel.powered.
const uint32_t kVehChassis = 0x200, kChsPoweredAxle = 0x520;
// The diff lock key's handler (found by signature) flips actor+A (the switch: telemetry, dash light, lever) and
// writes the same value to vehicle+B (the lock the drivetrain uses): "test cl,cl; mov rax,[rdi+18h]; sete cl;
// mov [rdi+A],cl; mov [rax+B],cl". The switch selects 2H/4H; with lock_diffs the vehicle's lock is held on.
static const char kDiffLockSig[] = "RTOSIG:DiffLockKey=84 C9 48 8B 47 18 0F 94 C1 88 8F ? ? ? ? 88 88 ? ? ? ?";
static uint32_t g_actorDiffLock, g_vehDiffLock;  // offsets (0 = not found)
struct FourWd {
    uintptr_t chassis;      // chassis data being managed (0 = none)
    uintptr_t vehicle;      // the player vehicle using it
    uintptr_t actor;        // and the player actor (diff lock switch)
    bool diffsHeld;         // we hold the vehicle's diff lock on
    uint8_t* powered;       // its powered_axle bools
    int count;
    uint8_t orig[8];        // as the game loaded them
    bool eligible;
    bool changed;           // we have written powered[0]
    int state;              // last applied: 2 = 2H, 4 = 4H, 0 = untouched
};
static FourWd g_4wd;

// Modded vehicles that get 2H/4H out of the box although their data drives only the rear axle (four_wd_exclude
// still switches them off; four_wd_include adds more).
struct BuiltinFourWd {
    const char* id;
    const char* what;
};
static const BuiltinFourWd kBuiltinFourWd[] = {
    {"vehicle.ford.350c", "LORD G350 pickup mod (Jon Ruda) - a 4x4 Super Duty, rear-wheel drive in its data"},
};

static const BuiltinFourWd* BuiltinFourWdFor(const char* id) {
    for (const BuiltinFourWd& b : kBuiltinFourWd)
        if (_stricmp(b.id, id) == 0) return &b;
    return nullptr;
}

static bool IdListed(const char* list, const char* id) {
    if (!id[0]) return false;
    const char* bare = _strnicmp(id, "vehicle.", 8) == 0 ? id + 8 : id;
    char buf[512];
    strncpy_s(buf, list, _TRUNCATE);
    char* ctx = nullptr;
    for (char* t = strtok_s(buf, ", \t", &ctx); t; t = strtok_s(nullptr, ", \t", &ctx))
        if (_stricmp(t, id) == 0 || _stricmp(t, bare) == 0) return true;
    return false;
}

// Player actor and vehicle through the game core (actor = core+0x31b0, vehicle = actor+0x18). POD only (__try).
static uintptr_t PlayerVehicle(uintptr_t* actorOut = nullptr) {
    if (actorOut) *actorOut = 0;
    if (!g_coreGlobal) return 0;
    __try {
        uintptr_t core = *(uintptr_t*)g_coreGlobal;
        uintptr_t actor = core ? *(uintptr_t*)(core + 0x31b0) : 0;
        if (actorOut) *actorOut = actor;
        return actor ? *(uintptr_t*)(actor + 0x18) : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

static int ReadByte(uintptr_t p) {
    __try {
        return *(volatile uint8_t*)p;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

// Reads the chassis data and its powered_axle array. POD only (__try).
static bool ReadChassis(uintptr_t vehicle, uintptr_t* chassis, uint8_t** powered, int* count) {
    __try {
        uintptr_t c = *(uintptr_t*)(vehicle + kVehChassis);
        if (!c || !(*(uint32_t*)(c + 8) & 0x80000000u)) return false;
        uint8_t* p = *(uint8_t**)(c + kChsPoweredAxle + 8);
        int64_t n = *(int64_t*)(c + kChsPoweredAxle + 16);
        if (!p || n < 1 || n > 8) return false;
        for (int i = 0; i < n; i++)
            if (p[i] > 1) return false;
        *chassis = c, *powered = p, *count = (int)n;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool WriteByte(uint8_t* p, uint8_t v) {
    __try {
        *p = v;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The physics does not read powered_axle while driving: a drivetrain setup routine sorts the axles into
// "powered" and "unpowered" lists once, and the physics uses those lists. The game re-runs that routine itself
// whenever an axle's drive status changes (lifting an axle), always as
//     rebuild(vehicle->physics (+0x150), vehicle->lifted-axle info (+0x6a0))
// so after changing the flag the plugin makes the same call. The routine reads the vehicle back from
// physics+0x10, which is checked first. Both vehicle offsets are read from the game's own call sites.
static const char kRebuildDriveSig[] =
    "RTOSIG:RebuildDrive=48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 41 56 48 83 EC 50 44 8B B1 ? ? ? ? 33 DB 49 D1 EE "
    "48 8B EA 48 8B F1";
typedef void(__fastcall* RebuildDrive_t)(void* physics, void* lifted);
static RebuildDrive_t g_rebuildDrive;
static uint32_t g_vehPhysics, g_vehLifted;  // vehicle offsets from the call sites (0 = unknown)

// Every "mov rdx, [reg+disp32]; mov rcx, [reg+disp32]; call rebuild" in .text must give the same two offsets.
static bool FindRebuildCallOffsets(uintptr_t fn) {
    auto nt = (IMAGE_NT_HEADERS64*)(g_base + ((IMAGE_DOS_HEADER*)g_base)->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    uintptr_t text = 0, size = 0;
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++)
        if (memcmp(sec->Name, ".text", 6) == 0) text = g_base + sec->VirtualAddress, size = sec->Misc.VirtualSize;
    const uint8_t* t = (const uint8_t*)text;
    uint32_t phys = 0, lifted = 0;
    int sites = 0;
    for (size_t i = 14; text && i + 5 <= size; i++) {
        if (t[i] != 0xE8 || text + i + 5 + *(const int32_t*)(t + i + 1) != fn) continue;
        const uint8_t* a = t + i - 14;  // 48 8B /r disp32 (rdx), 48 8B /r disp32 (rcx)
        if (a[0] != 0x48 || a[1] != 0x8B || (a[2] & 0xF8) != 0x90 || (a[2] & 7) == 4 || a[7] != 0x48 || a[8] != 0x8B ||
            (a[9] & 0xF8) != 0x88 || (a[9] & 7) == 4)
            continue;  // a call site with a different shape (e.g. the drivetrain's own update)
        uint32_t l = *(const uint32_t*)(a + 3), p = *(const uint32_t*)(a + 10);
        if (sites++ && (l != lifted || p != phys)) return false;
        lifted = l, phys = p;
    }
    if (sites < 1 || phys >= 0x2000 || lifted >= 0x2000) return false;
    g_vehPhysics = phys, g_vehLifted = lifted;
    return true;
}

// POD only (__try).
static bool RebuildDrive(uintptr_t vehicle) {
    if (!g_rebuildDrive || !g_vehPhysics) return false;
    __try {
        uintptr_t physics = *(uintptr_t*)(vehicle + g_vehPhysics);
        if (!physics || *(uintptr_t*)(physics + 0x10) != vehicle) return false;
        g_rebuildDrive((void*)physics, *(void**)(vehicle + g_vehLifted));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Puts the game's values back. The chassis data (shared definition) always stays valid; the vehicle object is only
// touched while it is still the player's vehicle - after a vehicle switch the old one may already be freed.
static void Restore4wd(const char* why) {
    uintptr_t actorNow = 0;
    bool alive = g_4wd.vehicle && PlayerVehicle(&actorNow) == g_4wd.vehicle && actorNow == g_4wd.actor;
    if (g_4wd.changed && g_4wd.powered && WriteByte(g_4wd.powered, g_4wd.orig[0])) {
        bool rebuilt = alive && RebuildDrive(g_4wd.vehicle);
        Log("4WD: front axle back to the game's setting (%s)%s", why,
            rebuilt ? "" : alive ? " - drivetrain not rebuilt" : " - vehicle gone, next one is built with it");
    }
    if (alive && g_4wd.diffsHeld && g_4wd.actor) {  // the game's lock follows its switch again
        int sw = ReadByte(g_4wd.actor + g_actorDiffLock);
        if (sw >= 0) WriteByte((uint8_t*)(g_4wd.vehicle + g_vehDiffLock), (uint8_t)sw);
    }
    g_4wd.changed = false, g_4wd.diffsHeld = false, g_4wd.state = 0;
}


static void Update4wd() {
    uintptr_t actor = 0;
    uintptr_t veh = PlayerVehicle(&actor);
    uintptr_t chassis = 0;
    uint8_t* powered = nullptr;
    int count = 0;
    if (!veh || !ReadChassis(veh, &chassis, &powered, &count)) chassis = 0;
    if (chassis != g_4wd.chassis) {
        Restore4wd("vehicle changed");
        memset(&g_4wd, 0, sizeof(g_4wd));
        if (chassis) {
            g_4wd.chassis = chassis, g_4wd.powered = powered, g_4wd.count = count, g_4wd.vehicle = veh;
            memcpy(g_4wd.orig, powered, count);
            bool awd = count == 2 && g_4wd.orig[0] && g_4wd.orig[1];
            const BuiltinFourWd* builtin = count == 2 ? BuiltinFourWdFor(g_vehicleId) : nullptr;
            bool included = count == 2 && IdListed(g_cfg.fourWdInclude, g_vehicleId);
            bool excluded = IdListed(g_cfg.fourWdExclude, g_vehicleId);
            g_4wd.eligible = !excluded && (awd || builtin || included);
            const char* why = excluded ? "excluded in the ini" : awd ? "its data drives both axles" : builtin ? builtin->what
                            : included ? "four_wd_include in the ini" : "two-wheel drive";
            Log("4WD: %s - %d axle(s), powered %d/%d -> %s (%s)", g_vehicleId[0] ? g_vehicleId : "vehicle", count, g_4wd.orig[0],
                count > 1 ? g_4wd.orig[1] : 0, g_4wd.eligible ? "2H/4H with the diff lock key" : "left as the game has it", why);
        }
    }
    g_4wd.vehicle = veh, g_4wd.actor = actor;
    bool want = g_4wd.chassis && g_4wd.eligible && g_cfg.fourWd && g_applies && g_mode == kModeCar && g_rebuildDrive &&
                g_actorDiffLock && actor;
    if (!want) {
        Restore4wd("not active for this vehicle");
        return;
    }
    // Hold the drivetrain's differentials locked whatever the switch says.
    if (g_cfg.lockDiffs && ReadByte(veh + g_vehDiffLock) == 0 && WriteByte((uint8_t*)(veh + g_vehDiffLock), 1)) {
        if (!g_4wd.diffsHeld) Log("4WD: differentials locked");
        g_4wd.diffsHeld = true;
    }
    int sw = ReadByte(actor + g_actorDiffLock);
    if (sw < 0) return;
    int state = sw ? 4 : 2;
    if (state == g_4wd.state) return;
    if (WriteByte(g_4wd.powered, state == 4 ? 1 : 0)) {
        g_4wd.changed = true, g_4wd.state = state;
        bool rebuilt = RebuildDrive(veh);
        Log("4WD: %s%s", state == 4 ? "4H (front axle engaged)" : "2H (rear-wheel drive)", rebuilt ? "" : " - drivetrain NOT rebuilt");
    }
}

// ---------------------------------------------------------------- surface grip (car mode)
// Each surface is a game_substance unit (reflection): name +0x10, pt_dynamic_friction +0x38 (float2, sliding),
// pt_static_friction +0x40 (float2, holding). The game reads them live. The objects are found once by their vtable,
// which the constructor loads first: "lea rax,[vtable]; mov [rdi],rax; lea rax,[..]; mov [rdi+30h],0.1; mov [rdi+34h],2.0".
// While the plugin applies to a car, grass / dirt / road surfaces get grip_* (sliding) and half that change
// (holding); otherwise the game's values are back.
static const char kSubstanceCtorSig[] =
    "RTOSIG:SubstanceCtor=48 8D 05 ? ? ? ? 48 89 07 48 8D 05 ? ? ? ? C7 47 30 CD CC CC 3D C7 47 34 00 00 00 40";
static char g_substanceNames[64][32];   // SDK substances configuration (index -> name)
static int g_substanceCount;
static volatile uint32_t g_wheelSubstance[8];
struct Surface {
    uintptr_t obj;
    char name[32];
    float dyn[2], stat[2];   // as the game loaded them
    float scale;             // grip_* for this surface (1 = not managed)
};
static Surface g_surfaces[64];
static int g_surfaceCount;
static bool g_surfacesScanned;
static bool g_gripApplied;

// Reads a name: string_t at +0x10 holding a char pointer (tries +0x10 and +0x18). POD only (__try).
static bool SurfaceName(uintptr_t obj, char* out, size_t cap) {
    __try {
        for (int off : {0x10, 0x18}) {
            const char* p = *(const char**)(obj + off);
            if ((uintptr_t)p < 0x10000) continue;
            size_t n = 0;
            while (n + 1 < cap && p[n] && p[n] >= 32 && p[n] < 127) out[n] = p[n], n++;
            out[n] = 0;
            if (n >= 2 && !p[n]) return true;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    out[0] = 0;
    return false;
}

static bool ReadSurface(uintptr_t obj, Surface& sf) {
    __try {
        memcpy(sf.dyn, (const void*)(obj + 0x38), 8);
        memcpy(sf.stat, (const void*)(obj + 0x40), 8);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool WriteSurface(uintptr_t obj, const float* dyn, const float* stat) {
    __try {
        memcpy((void*)(obj + 0x38), dyn, 8);
        memcpy((void*)(obj + 0x40), stat, 8);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static void ScanRange(const uint8_t* p, size_t n, uintptr_t vtable, uintptr_t* hits, int& count, int max) {
    __try {
        for (size_t i = 0; i + 8 <= n && count < max; i += 8)
            if (*(const uintptr_t*)(p + i) == vtable) hits[count++] = (uintptr_t)(p + i);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

static float SurfaceScale(const char* name) {
    static const char* kGrass[] = {"grass"};
    static const char* kDirt[] = {"dirt", "gravel", "road_dirt"};
    static const char* kRoad[] = {"road", "road_smooth", "road_coarse", "concrete"};
    for (const char* n : kGrass) if (strcmp(name, n) == 0) return g_cfg.gripGrass;
    for (const char* n : kDirt) if (strcmp(name, n) == 0) return g_cfg.gripDirt;
    for (const char* n : kRoad) if (strcmp(name, n) == 0) return g_cfg.gripRoad;
    return 1.0f;
}

// Finds the surface objects (once, the first time a car is driven); only ones whose name the SDK also lists.
static void ScanSurfaces() {
    g_surfacesScanned = true;
    uintptr_t ctor = FindUnique(kSubstanceCtorSig);
    if (!ctor) {
        Log("surface grip: substance code NOT found - surface grip off");
        return;
    }
    uintptr_t vtable = ctor + 7 + *(const int32_t*)(ctor + 3);
    uintptr_t hits[128];
    int count = 0;
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    MEMORY_BASIC_INFORMATION mbi;
    for (uint8_t* a = (uint8_t*)si.lpMinimumApplicationAddress;
         a < (uint8_t*)si.lpMaximumApplicationAddress && VirtualQuery(a, &mbi, sizeof(mbi)) == sizeof(mbi);
         a = (uint8_t*)mbi.BaseAddress + mbi.RegionSize)
        if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE && mbi.Protect == PAGE_READWRITE)
            ScanRange((const uint8_t*)mbi.BaseAddress, mbi.RegionSize, vtable, hits, count, 128);
    for (int i = 0; i < count && g_surfaceCount < 64; i++) {
        Surface sf = {};
        sf.obj = hits[i];
        if (!SurfaceName(sf.obj, sf.name, sizeof(sf.name)) || !ReadSurface(sf.obj, sf)) continue;
        bool listed = false;
        for (int k = 0; k < g_substanceCount && !listed; k++) listed = strcmp(g_substanceNames[k], sf.name) == 0;
        if (!listed || sf.dyn[0] <= 0 || sf.dyn[0] > 5 || sf.stat[0] <= 0 || sf.stat[0] > 5) continue;
        sf.scale = SurfaceScale(sf.name);
        g_surfaces[g_surfaceCount++] = sf;
        if (sf.scale != 1.0f)
            Log("surface grip: '%s' sliding %.2f -> %.2f, holding %.2f -> %.2f (car mode)", sf.name, (double)sf.dyn[0],
                (double)(sf.dyn[0] * sf.scale), (double)sf.stat[0], (double)(sf.stat[0] * (1 + (sf.scale - 1) * 0.5f)));
    }
    Log("surface grip: %d surface(s) found", g_surfaceCount);
}

static void ApplySurfaceGrip(bool on) {
    for (int i = 0; i < g_surfaceCount; i++) {
        Surface& sf = g_surfaces[i];
        if (sf.scale == 1.0f) continue;
        float hold = 1 + (sf.scale - 1) * 0.5f;
        float dyn[2] = {sf.dyn[0] * sf.scale, sf.dyn[1] * sf.scale};
        float stat[2] = {sf.stat[0] * hold, sf.stat[1] * hold};
        WriteSurface(sf.obj, on ? dyn : sf.dyn, on ? stat : sf.stat);
    }
    g_gripApplied = on;
    Log("surface grip %s", on ? "ON (car)" : "off (game values)");
}

static void SurfaceGripFrame() {
    bool any = g_cfg.gripGrass != 1.0f || g_cfg.gripDirt != 1.0f || g_cfg.gripRoad != 1.0f;
    if (!any) return;
    if (!g_surfacesScanned && g_mode == kModeCar && g_substanceCount) ScanSurfaces();
    bool want = g_applies && g_mode == kModeCar && g_surfaceCount > 0;
    if (want != g_gripApplied) ApplySurfaceGrip(want);
}

static void SurfaceGripShutdown() {
    if (g_gripApplied) ApplySurfaceGrip(false);
}

// Logs the surface under each wheel when it changes.
static void LogWheelSurfaces() {
    static uint32_t last[4] = {~0u, ~0u, ~0u, ~0u};
    bool changed = false;
    for (int i = 0; i < 4; i++) changed |= g_wheelSubstance[i] != last[i];
    if (!changed) return;
    char line[256];
    int n = snprintf(line, sizeof(line), "surfaces under wheels (front first):");
    for (int i = 0; i < 4; i++) {
        last[i] = g_wheelSubstance[i];
        uint32_t k = last[i];
        n += snprintf(line + n, sizeof(line) - n, " %s", k < (uint32_t)g_substanceCount ? g_substanceNames[k] : "?");
    }
    Log("%s", line);
}

static void TimingFrame(float dt, float pedal) {
    const double kMph60 = 60 * 0.44704, kKmh100 = 100 / 3.6, kQuarter = 402.336;
    double v = g_speed;
    if (!g_run.active && fabs(v) < 0.3 && pedal >= 0.95) {
        memset(&g_run, 0, sizeof(g_run));
        g_run.active = true;
        Log("timing: run started (%s)", g_vehicle);
    } else if (g_run.active) {
        if (pedal < 0.9) {
            Log("timing: run abandoned (pedal %.2f after %.1f s)", (double)pedal, g_run.t);
            g_run.active = false;
        } else {
            g_run.t += dt;
            g_run.dist += v * dt;
            if (!g_run.t60 && v >= kMph60) Log("timing: 0-60 mph %.2f s (%s)", g_run.t60 = g_run.t, g_vehicle);
            if (!g_run.t100 && v >= kKmh100) Log("timing: 0-100 km/h %.2f s", g_run.t100 = g_run.t);
            if (!g_run.tQuarter && g_run.dist >= kQuarter) {
                g_run.tQuarter = g_run.t, g_run.vQuarter = v;
                Log("timing: quarter mile %.2f s at %.1f mph (%.1f km/h) (%s)", g_run.tQuarter, v / 0.44704, v * 3.6, g_vehicle);
                g_run.active = false;
            }
        }
    }
    if (pedal >= 0.95) {
        g_wotTime += dt;
        if (v > g_wotTop) g_wotTop = v, g_wotTopRpm = g_rpm, g_wotTopGear = g_gear;
    } else if (g_wotTime > 0) {
        if (g_wotTime >= 10)
            Log("timing: highest speed in %.0f s at full throttle: %.1f mph (%.1f km/h) at %.0f rpm in gear %d (%s)", g_wotTime,
                g_wotTop / 0.44704, g_wotTop * 3.6, (double)g_wotTopRpm, g_wotTopGear, g_vehicle);
        g_wotTime = 0, g_wotTop = 0;
    }
}
static int g_lastGear;
static float g_lastRpm;
static scs_timestamp_t g_lastTime;
static bool g_haveTime;

// Minimum time in gear: after an upshift, the next upshift needs the engine to rev up from where it landed
// (until min_gear_time has passed), so a dip in throttle can't run the gearbox through several gears in a row.
// The rpm reported in the shift frame is still the pre-shift rpm, so for the first kSettle seconds the gear is
// simply held, and the landing rpm is the lowest rpm seen in that window.
const float kRevUpMargin = 500;            // rpm above the post-shift rpm
const float kSettle = 0.3f;                // seconds
static volatile scs_timestamp_t g_now;     // simulation time, microseconds
static volatile scs_timestamp_t g_holdUntil, g_settleUntil;
static volatile float g_holdFloor;
// After a downshift (coasting, braking, kickdown) no upshift for min_gear_time unless the driver is on the
// throttle: a two-gear coasting downshift can land above the light-throttle upshift point, and the gearbox
// would otherwise shift straight back up and hunt.
static volatile scs_timestamp_t g_downHoldUntil;
static float g_landRpm;
static bool g_landLogged = true;

static SCSAPI_VOID OnFloat(const scs_string_t, const scs_u32_t, const scs_value_t* const v, const scs_context_t ctx) {
    if (v) *(volatile float*)ctx = v->value_float.value;
}

static SCSAPI_VOID OnGear(const scs_string_t, const scs_u32_t, const scs_value_t* const v, const scs_context_t) {
    if (v) g_gear = v->value_s32.value;
}

static SCSAPI_VOID OnU32(const scs_string_t, const scs_u32_t, const scs_value_t* const v, const scs_context_t ctx) {
    if (v) *(volatile uint32_t*)ctx = v->value_u32.value;
}

static SCSAPI_VOID OnConfiguration(const scs_event_t, const void* const info, const scs_context_t) {
    auto cfg = (const scs_telemetry_configuration_t*)info;
    if (strcmp(cfg->id, SCS_TELEMETRY_CONFIG_substances) == 0) {
        g_substanceCount = 0;
        for (const scs_named_value_t* a = cfg->attributes; a->name; a++)
            if (a->value.type == SCS_VALUE_TYPE_string && a->index < 64) {
                strncpy_s(g_substanceNames[a->index], a->value.value_string.value, _TRUNCATE);
                if ((int)a->index + 1 > g_substanceCount) g_substanceCount = a->index + 1;
            }
        char line[1024];
        int n = snprintf(line, sizeof(line), "SDK substances:");
        for (int i = 0; i < g_substanceCount && n < (int)sizeof(line) - 40; i++)
            n += snprintf(line + n, sizeof(line) - n, " %d=%s", i, g_substanceNames[i]);
        Log("%s", line);
        return;
    }
    if (strcmp(cfg->id, SCS_TELEMETRY_CONFIG_truck) != 0) return;
    float limit = 0, diff = 0, ratios[32] = {};
    int gears = 0;
    const char *brand = "", *name = "", *id = "", *shifter = "";
    for (const scs_named_value_t* a = cfg->attributes; a->name; a++) {
        if (strcmp(a->name, SCS_TELEMETRY_CONFIG_ATTRIBUTE_forward_gear_count) == 0 && a->value.type == SCS_VALUE_TYPE_u32)
            gears = (int)a->value.value_u32.value;
        else if (strcmp(a->name, SCS_TELEMETRY_CONFIG_ATTRIBUTE_differential_ratio) == 0 && a->value.type == SCS_VALUE_TYPE_float)
            diff = a->value.value_float.value;
        else if (strcmp(a->name, SCS_TELEMETRY_CONFIG_ATTRIBUTE_forward_ratio) == 0 && a->value.type == SCS_VALUE_TYPE_float &&
                 a->index < 32)
            ratios[a->index] = a->value.value_float.value;
        else if (strcmp(a->name, SCS_TELEMETRY_CONFIG_ATTRIBUTE_id) == 0 && a->value.type == SCS_VALUE_TYPE_string)
            id = a->value.value_string.value;
        else if (strcmp(a->name, SCS_TELEMETRY_CONFIG_ATTRIBUTE_shifter_type) == 0 && a->value.type == SCS_VALUE_TYPE_string)
            shifter = a->value.value_string.value;
        if (strcmp(a->name, SCS_TELEMETRY_CONFIG_ATTRIBUTE_rpm_limit) == 0 && a->value.type == SCS_VALUE_TYPE_float)
            limit = a->value.value_float.value;
        else if (strcmp(a->name, SCS_TELEMETRY_CONFIG_ATTRIBUTE_brand) == 0 && a->value.type == SCS_VALUE_TYPE_string)
            brand = a->value.value_string.value;
        else if (strcmp(a->name, SCS_TELEMETRY_CONFIG_ATTRIBUTE_name) == 0 && a->value.type == SCS_VALUE_TYPE_string)
            name = a->value.value_string.value;
    }
    g_rpmLimit = limit;
    snprintf(g_vehicle, sizeof(g_vehicle), "%s %s", brand, name);
    strncpy_s(g_vehicleId, id, _TRUNCATE);
    g_gearCount = gears < 32 ? gears : 32;
    g_curvePendingSince = g_shiftCalls;  // work the points out again once the game has reported the new vehicle
    for (int i = 0; i < 32; i++) g_ratio[i + 1] = ratios[i];
    Log("vehicle configuration: %s (%s), rpm limit %.0f, shifter %s, %d forward gears, differential %.3f", g_vehicle, id,
        (double)limit, shifter, gears, (double)diff);
    if (g_cfg.log && gears > 0) {
        char line[512];
        int n = snprintf(line, sizeof(line), "  gear ratios:");
        for (int i = 0; i < gears && i < 32 && n < (int)sizeof(line) - 16; i++) n += snprintf(line + n, sizeof(line) - n, " %.3f", (double)ratios[i]);
        Log("%s", line);
    }
}

static SCSAPI_VOID OnFrameStart(const scs_event_t, const void* const info, const scs_context_t) {
    auto fs = (const scs_telemetry_frame_start_t*)info;
    float dt = g_haveTime && fs->simulation_time > g_lastTime ? (float)(fs->simulation_time - g_lastTime) / 1e6f : 0.0f;
    g_lastTime = fs->simulation_time;
    g_now = fs->simulation_time;
    g_haveTime = true;
    float t = g_pedal, r = g_remembered;
    if (t >= r)
        r = t;
    else
        r = g_cfg.releaseTime > 0 ? (r - dt / g_cfg.releaseTime > t ? r - dt / g_cfg.releaseTime : t) : t;
    g_remembered = r;

    int gear = g_gear;
    if (gear > 0 && g_lastGear > 0 && gear > g_lastGear) {
        g_holdFloor = 1e9f;  // hold outright until the shift has settled
        g_landRpm = 1e9f;
        g_landLogged = false;
        g_settleUntil = g_now + (scs_timestamp_t)(kSettle * 1e6f);
        g_holdUntil = g_now + (scs_timestamp_t)(g_cfg.minGearTime * 1e6f);
    } else if (gear != g_lastGear) {
        g_holdUntil = 0;  // downshift or neutral: no hold
        if (gear > 0 && g_lastGear > 0) g_downHoldUntil = g_now + (scs_timestamp_t)(g_cfg.minGearTime * 1e6f);
    }
    if (g_now < g_settleUntil) {
        if (g_rpm < g_landRpm) g_landRpm = g_rpm;
    } else if (!g_landLogged) {
        g_landLogged = true;
        g_holdFloor = g_landRpm + kRevUpMargin;
        if (g_cfg.log) Log("  landed at %.0f rpm, next upshift not before %.0f rpm", (double)g_landRpm, (double)g_holdFloor);
    }
    if (g_cfg.log && gear > 0 && g_lastGear > 0 && gear != g_lastGear)
    {
        int from = g_lastGear <= 32 ? g_lastGear : 32;
        Log("shift %d -> %d at %.0f rpm, pedal %.2f, remembered %.2f, effective %.2f | gear %d: game range %.0f-%.0f, upshift used %.0f",
            g_lastGear, gear, (double)g_lastRpm, (double)t, (double)r, (double)g_effective, g_lastGear,
            (double)g_gameMin[from], (double)g_gameMax[from], (double)g_ourMax[from]);
    }
    g_lastGear = gear;
    g_lastRpm = g_rpm;

    uintptr_t veh = g_shiftVehicle;
    bool pending = g_curvePendingSince >= 0 && g_shiftCalls != g_curvePendingSince;
    if (veh && g_rpmLimit >= 1000 && g_gearCount > 1 && (pending || (g_curvePendingSince < 0 && veh != g_curveVehicle))) {
        g_curvePendingSince = -1;
        WorkOutBestUpshifts(veh);
    }

    // Plateau upshift (see g_plateauGear).
    float rpm = g_rpm;
    int pg = 0;
    if (gear > 0 && gear < g_gearCount && gear <= 32 && t >= g_cfg.lightThrottle && g_ourMax[gear] > g_gameMax[gear] &&
        rpm >= kPlateauNear * g_ourMax[gear] && g_now >= g_holdUntil) {
        if (rpm > g_plateauRef * (1 + kPlateauRise)) {
            g_plateauRef = rpm, g_plateauTime = 0;
        } else {
            g_plateauTime += dt;
            float land = g_ratio[gear] > 0 ? rpm * g_ratio[gear + 1] / g_ratio[gear] : 0;
            if (g_plateauTime >= kPlateauTime && land >= kPlateauLand * g_rpmLimit) pg = gear;
        }
    } else {
        g_plateauRef = rpm, g_plateauTime = 0;
    }
    if (pg && g_plateauGear != pg && g_cfg.log)
        Log("  rpm stopped rising at %.0f in gear %d (upshift point %.0f) - upshifting now", (double)rpm, gear, (double)g_ourMax[gear]);
    g_plateauGear = pg;
    if (g_cfg.log) TimingFrame(dt, t);
    if (g_cfg.enabled) PollToggle();
    // Wall test: nearly stationary on the throttle -> each wheel's rotation speed twice a second.
    g_wheelLogTime += dt;
    if (g_cfg.log && fabs(g_speed) < 1.0f && t > 0.5f && g_wheelLogTime >= 0.5f) {
        g_wheelLogTime = 0;
        Log("wheels (rot/s, front first): %.2f %.2f | %.2f %.2f  - %s, pedal %.2f", (double)g_wheelSpin[0], (double)g_wheelSpin[1],
            (double)g_wheelSpin[2], (double)g_wheelSpin[3], g_4wd.state == 4 ? "4H" : g_4wd.state == 2 ? "2H" : "game drive", (double)t);
    }
    UpdateApplies();
    Update4wd();
    SurfaceGripFrame();
    if (g_cfg.log >= 2) LogWheelSurfaces();
}

// ---------------------------------------------------------------- the hook

typedef float*(__fastcall* ShiftRange_t)(void* self, float* out, int gear);
static ShiftRange_t OrigShiftRange;
static volatile bool g_passthrough;

static float* __fastcall HookShiftRange(void* self, float* out, int gear) {
    float* r = OrigShiftRange(self, out, gear);
    if (out && gear > 0 && gear <= 32) g_gameMin[gear] = out[0], g_gameMax[gear] = out[1], g_ourMax[gear] = out[1];
    if (self) g_shiftVehicle = *(uintptr_t*)((char*)self + g_vehicleOff), g_shiftCalls++;
    if (g_passthrough || !g_applies || !out) return r;
    float limit = g_rpmLimit;
    if (limit < 1000) return r;  // unknown vehicle: leave the game's result
    float span = g_cfg.fullThrottle - g_cfg.lightThrottle;
    float s = Clamp((g_remembered - g_cfg.lightThrottle) / span, 0.0f, 1.0f);

    // Skip limit. The game's gear choice asks for the range of every higher gear and takes the one whose
    // predicted rpm lands closest to a point inside its range; a gear is only a candidate if its predicted rpm
    // is inside [min, max]. A gear too far above the current one gets a range no rpm can be in.
    int current = g_gear, skip = s > 0 ? g_cfg.maxSkipPower : g_cfg.maxSkipLight;
    if (current > 0 && gear > current && g_now < g_downHoldUntil && s <= 0) {  // just downshifted: no upshift yet
        out[0] = out[1] = limit * 10;
        return r;
    }
    if (skip > 0 && current > 0 && gear > current + skip) {
        out[0] = out[1] = limit * 10;
        return r;
    }
    s = s * s * (3 - 2 * s);  // smoothstep: gentle near both ends
    float stock = out[1];
    float target = gear > 0 && gear <= 32 && g_bestUp[gear] > 0 ? g_bestUp[gear] : limit * g_cfg.fullUpshift;
    float up = target > stock ? stock + (target - stock) * s : stock;  // never earlier than the game
    if (g_now < g_holdUntil && g_holdFloor > up) up = g_holdFloor;     // minimum time in gear
    float cap = limit * 0.99f;  // the game's own cap
    if (up > cap) up = cap;
    if (gear == g_plateauGear && g_rpm - 10 < up && g_rpm - 10 > out[0] + 100) up = g_rpm - 10;  // plateau upshift
    if (up <= stock || up < out[0] + 100) return r;
    out[1] = up;
    if (gear > 0 && gear <= 32) g_ourMax[gear] = up;
    return r;
}

// ---------------------------------------------------------------- setup / shutdown

static std::string g_dir;
static uintptr_t g_target;
static bool g_hooked;

// True if the game function still jumps straight to our detour (see Service At Garage: another plugin that
// hooked it later would be overwritten by MH_DisableHook).
static bool StillOurs() {
    __try {
        const uint8_t* site = (const uint8_t*)g_target;
        if (site[0] == 0xEB && site[1] == 0xF9) site -= 5;
        if (site[0] != 0xE9) return false;
        const uint8_t* relay = site + 5 + *(const int32_t*)(site + 1);
        static const uint8_t kAbs[6] = {0xFF, 0x25, 0, 0, 0, 0};
        return memcmp(relay, kAbs, 6) == 0 && *(void* const*)(relay + 6) == (void*)HookShiftRange;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

}  // namespace rto

using namespace rto;

SCSAPI_RESULT scs_telemetry_init(const scs_u32_t version, const scs_telemetry_init_params_t* const params) {
    if (version != SCS_TELEMETRY_VERSION_1_01) return SCS_RESULT_unsupported;
    auto p = (const scs_telemetry_init_params_v101_t*)params;
    std::string iniNote = LoadConfig(g_dir + "road_trip_overhaul.ini");
    if (g_cfg.log) LogOpen(g_dir + "road_trip_overhaul.log");
    Log("Road Trip Overhaul v" RTO_VERSION ", %s", p->common.game_name);
    if (!iniNote.empty()) Log("%s", iniNote.c_str());

    g_target = FindUnique(kShiftRangeSig);
    if (g_target) {
        g_vehicleOff = *(const uint8_t*)(g_target + 24);   // mov rax, [rcx + disp8]
        g_engineOff = *(const uint32_t*)(g_target + 37);   // mov rcx, [rax + disp32]
        Log("vehicle +0x%X, engine accessory +0x%X", g_vehicleOff, g_engineOff);
    }
    Log("signature ShiftRange %s", g_target ? "found" : "NOT FOUND");
    bool ok = g_target && MH_Initialize() == MH_OK &&
              MH_CreateHook((void*)g_target, (void*)HookShiftRange, (void**)&OrigShiftRange) == MH_OK &&
              MH_EnableHook((void*)g_target) == MH_OK;
    if (!ok) {
        if (g_target) MH_Uninitialize();
        p->common.log(SCS_LOG_TYPE_warning,
                      "[Road Trip Overhaul] v" RTO_VERSION " INACTIVE: the game's shift code was not found (game updated?)");
        return SCS_RESULT_ok;
    }
    g_hooked = true;

    g_gameLog = p->common.log;
    if (g_cfg.enabled && g_cfg.gameAdaptive > 0) {
        g_adaptiveSetting = FindGameSetting("g_adaptive_shift");
        if (!g_adaptiveSetting) Log("game adaptive mode (g_adaptive_shift) setting not found");
        FindSettingGetter(g_target);
        g_iniPath = g_dir + "road_trip_overhaul.ini";
        float atStart = ReadGameAdaptive(), saved = LoadPlayerAdaptive();
        g_playerAdaptive = g_lastSeenAdaptive = atStart;
        Log("game adaptive mode (g_adaptive_shift) is %g at start, your saved value %g%s", (double)atStart, (double)saved,
            g_settingGet ? "" : " (getter not found - restored to 0)");
        if (atStart >= 0 && atStart == g_cfg.gameAdaptive) {
            // The game saved the plugin's own value: put the player's back (off if none is known yet).
            g_playerAdaptive = saved >= 0 ? saved : 0.0f;
            SetGameAdaptive(g_playerAdaptive);
            Log("game adaptive mode: that is the plugin's car value - your setting %g restored", (double)g_playerAdaptive);
        }
        if (g_playerAdaptive >= 0) SavePlayerAdaptive(g_playerAdaptive);
    }
    g_coreGlobal = FindCoreGlobal(g_target);
    if (g_cfg.fourWd) {
        uintptr_t dl = FindUnique(kDiffLockSig);
        if (dl) g_actorDiffLock = *(const uint32_t*)(dl + 11), g_vehDiffLock = *(const uint32_t*)(dl + 17);
        if (g_actorDiffLock >= 0x10000 || g_vehDiffLock >= 0x10000) g_actorDiffLock = g_vehDiffLock = 0;
        Log("4WD: diff lock switch %s (actor +0x%X, vehicle +0x%X)", g_actorDiffLock ? "found" : "NOT found - 2H/4H off",
            g_actorDiffLock, g_vehDiffLock);
        uintptr_t fn = FindUnique(kRebuildDriveSig);
        if (fn && FindRebuildCallOffsets(fn)) g_rebuildDrive = (RebuildDrive_t)fn;
        Log("4WD: drivetrain rebuild %s (physics +0x%X, lifted axles +0x%X)", g_rebuildDrive ? "found" : "NOT found - 2H/4H off",
            g_vehPhysics, g_vehLifted);
    }
    Log("vehicle mode detection %s; works on %s", g_coreGlobal ? "available" : "NOT available",
        g_cfg.carOnly ? "cars only" : "all vehicles");

    p->register_for_event(SCS_TELEMETRY_EVENT_frame_start, OnFrameStart, nullptr);
    p->register_for_event(SCS_TELEMETRY_EVENT_configuration, OnConfiguration, nullptr);
    p->register_for_channel(SCS_TELEMETRY_TRUCK_CHANNEL_input_throttle, SCS_U32_NIL, SCS_VALUE_TYPE_float,
                            SCS_TELEMETRY_CHANNEL_FLAG_none, OnFloat, (scs_context_t)&g_pedal);
    p->register_for_channel(SCS_TELEMETRY_TRUCK_CHANNEL_effective_throttle, SCS_U32_NIL, SCS_VALUE_TYPE_float,
                            SCS_TELEMETRY_CHANNEL_FLAG_none, OnFloat, (scs_context_t)&g_effective);
    p->register_for_channel(SCS_TELEMETRY_TRUCK_CHANNEL_speed, SCS_U32_NIL, SCS_VALUE_TYPE_float,
                            SCS_TELEMETRY_CHANNEL_FLAG_none, OnFloat, (scs_context_t)&g_speed);
    p->register_for_channel(SCS_TELEMETRY_TRUCK_CHANNEL_engine_rpm, SCS_U32_NIL, SCS_VALUE_TYPE_float,
                            SCS_TELEMETRY_CHANNEL_FLAG_none, OnFloat, (scs_context_t)&g_rpm);
    for (scs_u32_t w = 0; w < 8; w++) {
        p->register_for_channel(SCS_TELEMETRY_TRUCK_CHANNEL_wheel_velocity, w, SCS_VALUE_TYPE_float,
                                SCS_TELEMETRY_CHANNEL_FLAG_none, OnFloat, (scs_context_t)&g_wheelSpin[w]);
        p->register_for_channel(SCS_TELEMETRY_TRUCK_CHANNEL_wheel_substance, w, SCS_VALUE_TYPE_u32,
                                SCS_TELEMETRY_CHANNEL_FLAG_none, OnU32, (scs_context_t)&g_wheelSubstance[w]);
    }
    p->register_for_channel(SCS_TELEMETRY_TRUCK_CHANNEL_engine_gear, SCS_U32_NIL, SCS_VALUE_TYPE_s32,
                            SCS_TELEMETRY_CHANNEL_FLAG_none, OnGear, nullptr);

    char msg[160];
    snprintf(msg, sizeof(msg), "[Road Trip Overhaul] v" RTO_VERSION " active%s", g_cfg.enabled ? "" : " - disabled in road_trip_overhaul.ini");
    p->common.log(SCS_LOG_TYPE_message, msg);
    Log("%s (light %.2f, full %.2f, full upshift %.0f%% of rpm limit, release %.1f s, min gear time %.1f s)", msg,
        (double)g_cfg.lightThrottle, (double)g_cfg.fullThrottle, (double)g_cfg.fullUpshift * 100, (double)g_cfg.releaseTime,
        (double)g_cfg.minGearTime);
    return SCS_RESULT_ok;
}

SCSAPI_VOID scs_telemetry_shutdown(void) {
    Restore4wd("shutdown");
    if (g_applies && g_cfg.gameAdaptive > 0) ApplyGameAdaptive(false);  // leave the player's value in place
    SurfaceGripShutdown();
    if (g_hooked) {
        g_passthrough = true;
        if (StillOurs() && MH_DisableHook((void*)g_target) == MH_OK) {
            MH_Uninitialize();
        } else {
            // Another plugin hooked the function after us: leave ours in place (pass-through) and keep the DLL
            // loaded so neither jump ever points at freed code.
            Log("shift code is hooked by another plugin on top of ours; leaving ours in place (pass-through)");
            HMODULE pinned;
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                               (LPCWSTR)HookShiftRange, &pinned);
        }
        g_hooked = false;
    }
    Log("shutdown");
    LogClose();
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        char path[MAX_PATH];
        GetModuleFileNameA(inst, path, MAX_PATH);
        g_dir = path;
        g_dir = g_dir.substr(0, g_dir.find_last_of("\\/") + 1);
        g_base = (uintptr_t)GetModuleHandleW(nullptr);
        g_size = ((IMAGE_NT_HEADERS64*)(g_base + ((IMAGE_DOS_HEADER*)g_base)->e_lfanew))->OptionalHeader.SizeOfImage;
    }
    return TRUE;
}
