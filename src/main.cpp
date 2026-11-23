// RE6Tweaks: graphics and camera tweaks for Resident Evil 6, as a ReShade add-on (32-bit, D3D9).
//
// 0.0.1 is a probe. It changes nothing in the game. It maps the game's engine objects so the
// real options (camera field of view and distance, shadows, render settings) can be built on
// them later:
//   - logs the exe, the graphics API and the back buffer;
//   - finds MT Framework singletons (sMain, sCamera, sRender, sShadow, ...). Every engine object
//     has a virtual getDTI() that is "mov eax, <type record>; ret", and the type record (MtDTI)
//     holds the class name at +4. So any pointer in the exe's data section that points to an
//     object can be named without calling game code;
//   - on Ctrl+F9 (or the overlay button) writes a numbered snapshot: every dword of the camera
//     and render singletons and of the camera objects they point to, as hex, float and class
//     name. Two snapshots taken in different camera states (walking, aiming, cutscene) are
//     diffed offline to find the field of view and camera distance.
//   - Ctrl+F10 measures the frame rate for 10 s.
// Output goes to <game>\RE6Tweaks.log and <game>\RE6Tweaks-probe\.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <imgui.h>
#include <reshade.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "version.h"

using namespace reshade::api;

extern "C" __declspec(dllexport) const char* NAME = DT_PRODUCT;
extern "C" __declspec(dllexport) const char* DESCRIPTION =
    "Probe for " DT_GAME ": maps the engine's camera and render objects (Ctrl+F9 snapshot, "
    "Ctrl+F10 frame rate). Changes nothing in the game.";

namespace {

// ---------------------------------------------------------------- log

std::wstring g_gameDir;  // with trailing backslash
std::mutex g_logLock;
std::recursive_mutex g_state;

void Log(const char* fmt, ...) {
    std::lock_guard<std::mutex> lock(g_logLock);
    FILE* f = _wfopen((g_gameDir + L"" DT_PRODUCT ".log").c_str(), L"a");
    if (!f) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(f, "%02d:%02d:%02d ", t.wHour, t.wMinute, t.wSecond);
    va_list args;
    va_start(args, fmt);
    vfprintf(f, fmt, args);
    va_end(args);
    fputc('\n', f);
    fclose(f);
}

// ---------------------------------------------------------------- memory

struct Range {
    uintptr_t lo = 0, hi = 0;
    bool In(uintptr_t a, size_t n = 1) const { return a >= lo && a + n <= hi && a + n >= a; }
};

uintptr_t g_base = 0;
Range g_image, g_text, g_rdata, g_data;
std::vector<Range> g_readable;  // committed, readable regions, sorted

void MapImage() {
    g_base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(g_base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(g_base + dos->e_lfanew);
    g_image = {g_base, g_base + nt->OptionalHeader.SizeOfImage};
    auto* sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        char name[9] = {};
        memcpy(name, sec->Name, 8);
        const Range r{g_base + sec->VirtualAddress, g_base + sec->VirtualAddress + sec->Misc.VirtualSize};
        if (!strcmp(name, ".text")) g_text = r;
        else if (!strcmp(name, ".rdata")) g_rdata = r;
        else if (!strcmp(name, ".data")) g_data = r;
        Log("section %-8s %08x..%08x", name, (unsigned)r.lo, (unsigned)r.hi);
    }
    Log("exe base %08x, timestamp %08x, image size %08x, large-address-aware %d", (unsigned)g_base,
        (unsigned)nt->FileHeader.TimeDateStamp, (unsigned)nt->OptionalHeader.SizeOfImage,
        (nt->FileHeader.Characteristics & IMAGE_FILE_LARGE_ADDRESS_AWARE) ? 1 : 0);
}

void MapReadable() {
    g_readable.clear();
    MEMORY_BASIC_INFORMATION mbi;
    uintptr_t a = 0x10000;
    while (VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof(mbi)) == sizeof(mbi)) {
        const uintptr_t lo = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        const uintptr_t hi = lo + mbi.RegionSize;
        const DWORD p = mbi.Protect & 0xff;
        const bool ok = mbi.State == MEM_COMMIT && !(mbi.Protect & PAGE_GUARD) && p != PAGE_NOACCESS &&
                        p != PAGE_EXECUTE;
        if (ok) {
            if (!g_readable.empty() && g_readable.back().hi == lo) g_readable.back().hi = hi;
            else g_readable.push_back({lo, hi});
        }
        if (hi <= a) break;
        a = hi;
    }
}

bool Readable(uintptr_t a, size_t n) {
    auto it = std::upper_bound(g_readable.begin(), g_readable.end(), a,
                               [](uintptr_t v, const Range& r) { return v < r.lo; });
    if (it == g_readable.begin()) return false;
    --it;
    return it->In(a, n);
}

size_t ReadableFrom(uintptr_t a) {  // bytes readable from a to the end of its region
    auto it = std::upper_bound(g_readable.begin(), g_readable.end(), a,
                               [](uintptr_t v, const Range& r) { return v < r.lo; });
    if (it == g_readable.begin()) return 0;
    --it;
    return it->In(a) ? it->hi - a : 0;
}

// The memory map is taken once per scan; the game can free memory in between, so every read
// is also guarded.
bool Read32(uintptr_t a, uint32_t& out) {
    if (!Readable(a, 4)) return false;
    __try {
        out = *reinterpret_cast<const volatile uint32_t*>(a);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ReadBytes(uintptr_t a, void* out, size_t n) {
    if (!Readable(a, n)) return false;
    __try {
        memcpy(out, reinterpret_cast<const void*>(a), n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ---------------------------------------------------------------- MT Framework type records

// Returns the DTI (type record) of an object, or 0. getDTI() is a vtable entry whose code is
// B8 imm32 C3 (mov eax, dti; ret), and the DTI's name pointer (+4) leads to a class name.
std::map<uintptr_t, std::string> g_dtiNames;

const std::string* DtiName(uintptr_t dti) {
    auto it = g_dtiNames.find(dti);
    if (it != g_dtiNames.end()) return it->second.empty() ? nullptr : &it->second;
    std::string name;
    uint32_t vt = 0, np = 0;
    if (g_data.In(dti, 8) && Read32(dti, vt) && g_rdata.In(vt) && Read32(dti + 4, np) && g_image.In(np)) {
        char buf[72] = {};
        if (ReadBytes(np, buf, sizeof(buf) - 1)) {
            size_t n = 0;
            while (n < 64 && buf[n] && (isalnum((unsigned char)buf[n]) || buf[n] == '_' || buf[n] == ':')) ++n;
            if (n >= 2 && buf[n] == 0) name.assign(buf, n);
        }
    }
    auto& slot = g_dtiNames[dti];
    slot = name;
    return slot.empty() ? nullptr : &slot;
}

uintptr_t DtiOf(uintptr_t obj, int* slotOut = nullptr) {
    uint32_t vt = 0;
    if (g_image.In(obj) || !Read32(obj, vt) || !g_rdata.In(vt, 4)) return 0;
    for (int i = 0; i < 32; ++i) {
        uint32_t f = 0;
        if (!Read32(vt + 4 * i, f)) return 0;
        if (!g_text.In(f, 6)) {
            if (i > 0) return 0;  // end of the vtable
            continue;
        }
        uint8_t code[6];
        if (!ReadBytes(f, code, 6) || code[0] != 0xB8 || code[5] != 0xC3) continue;
        uint32_t dti;
        memcpy(&dti, code + 1, 4);
        if (DtiName(dti)) {
            if (slotOut) *slotOut = i;
            return dti;
        }
    }
    return 0;
}

const char* ClassOf(uintptr_t obj) {
    const uintptr_t dti = DtiOf(obj);
    const std::string* n = dti ? DtiName(dti) : nullptr;
    return n ? n->c_str() : nullptr;
}

// ---------------------------------------------------------------- scan

struct Found {
    uintptr_t where;  // static pointer in .data
    uintptr_t obj;
    uintptr_t dti;
    int slot;
    std::string cls;
};
std::vector<Found> g_found;   // every .data pointer to a named object
std::string g_scanStatus = "not scanned yet";
int g_snapshot = 0;

void Scan() {
    MapReadable();
    g_found.clear();
    std::map<std::string, int> counts;
    for (uintptr_t a = g_data.lo; a + 4 <= g_data.hi; a += 4) {
        uint32_t p = 0;
        if (!Read32(a, p) || p < 0x10000 || g_image.In(p)) continue;
        int slot = -1;
        const uintptr_t dti = DtiOf(p, &slot);
        if (!dti) continue;
        g_found.push_back({a, p, dti, slot, *DtiName(dti)});
        ++counts[g_found.back().cls];
    }
    char buf[128];
    snprintf(buf, sizeof(buf), "%zu static pointers to %zu classes", g_found.size(), counts.size());
    g_scanStatus = buf;
    Log("scan: %s (%zu readable regions)", buf, g_readable.size());

    CreateDirectoryW((g_gameDir + L"" DT_PRODUCT "-probe").c_str(), nullptr);
    FILE* f = _wfopen((g_gameDir + L"" DT_PRODUCT "-probe\\statics.txt").c_str(), L"w");
    if (f) {
        fprintf(f, "# static (.data) address -> object : class (getDTI vtable slot, DTI address, DTI dwords)\n");
        for (const Found& x : g_found) {
            uint32_t d[8] = {};
            ReadBytes(x.dti, d, sizeof(d));
            fprintf(f, "%08x -> %08x : %-36s slot %2d dti %08x [%08x %08x %08x %08x %08x %08x %08x %08x]\n",
                    (unsigned)x.where, (unsigned)x.obj, x.cls.c_str(), x.slot, (unsigned)x.dti, d[0], d[1],
                    d[2], d[3], d[4], d[5], d[6], d[7]);
        }
        fclose(f);
    }
    for (const Found& x : g_found)
        if (x.cls.size() > 1 && x.cls[0] == 's' && isupper((unsigned char)x.cls[1]))
            Log("singleton %-24s static %08x -> %08x (getDTI slot %d, DTI %08x)", x.cls.c_str(),
                (unsigned)x.where, (unsigned)x.obj, x.slot, (unsigned)x.dti);
}

bool IsCameraClass(const std::string& c) {
    std::string l(c);
    std::transform(l.begin(), l.end(), l.begin(), [](unsigned char ch) { return (char)tolower(ch); });
    return l.find("camera") != std::string::npos;
}

void DumpObject(FILE* f, const char* label, uintptr_t obj, size_t size, std::vector<uintptr_t>* cameras) {
    size = std::min(size, ReadableFrom(obj));
    const char* cls = ClassOf(obj);
    fprintf(f, "\n=== %s %08x %s, %zu bytes\n", label, (unsigned)obj, cls ? cls : "?", size);
    for (size_t o = 0; o + 4 <= size; o += 4) {
        uint32_t v = 0;
        if (!Read32(obj + o, v)) {
            fprintf(f, "+%05zx  (unreadable)\n", o);
            break;
        }
        float fl;
        memcpy(&fl, &v, 4);
        char fs[32] = "";
        const float a = std::fabs(fl);
        if (v != 0 && std::isfinite(fl) && a >= 1e-4f && a <= 1e6f) snprintf(fs, sizeof(fs), "f=%.6g", fl);
        const char* pc = (v >= 0x10000 && !g_image.In(v)) ? ClassOf(v) : nullptr;
        fprintf(f, "+%05zx  %08x  %-16s %s%s\n", o, v, fs, pc ? "-> " : "", pc ? pc : "");
        if (pc && cameras && IsCameraClass(pc) &&
            std::find(cameras->begin(), cameras->end(), v) == cameras->end())
            cameras->push_back(v);
    }
}

void Snapshot() {
    Scan();
    const int n = ++g_snapshot;
    wchar_t name[64];
    swprintf(name, 64, L"" DT_PRODUCT "-probe\\snapshot-%02d.txt", n);
    FILE* f = _wfopen((g_gameDir + name).c_str(), L"w");
    if (!f) {
        Log("snapshot %d: cannot write", n);
        return;
    }
    fprintf(f, "# RE6Tweaks probe snapshot %d. Lines: offset, dword, float view, class of the object it points to.\n", n);
    std::set<std::string> want = {"sCamera", "sRender", "sShadow", "sScene", "sMain"};
    std::vector<uintptr_t> cameras;
    std::set<uintptr_t> done;
    for (const Found& x : g_found) {
        const bool singleton = want.count(x.cls) > 0;
        if (!singleton && !IsCameraClass(x.cls)) continue;
        if (!done.insert(x.obj).second) continue;
        char label[96];
        snprintf(label, sizeof(label), "%s (static %08x)", x.cls.c_str(), (unsigned)x.where);
        // sMain is large (fields past 0xff00); the others are dumped in full up to 64 KB.
        DumpObject(f, label, x.obj, x.cls == "sMain" ? 0x10000 : 0x4000, &cameras);
    }
    // Camera objects reached from the singletons (one more level).
    for (size_t i = 0; i < cameras.size() && i < 32; ++i)
        if (done.insert(cameras[i]).second) DumpObject(f, "camera object", cameras[i], 0x1000, nullptr);
    fclose(f);
    Log("snapshot %d written: %zu objects", n, done.size() + 0);
}

// ---------------------------------------------------------------- device / frame

LARGE_INTEGER g_freq{}, g_last{}, g_measureEnd{}, g_firstPresent{};
bool g_measuring = false, g_autoScanned = false;
std::vector<double> g_frameMs;
std::string g_measureResult, g_deviceInfo;
bool g_f9 = false, g_f10 = false;

void OnInitDevice(device* dev) {
    static const char* const apis[] = {"?", "d3d9", "d3d10", "d3d11", "d3d12", "opengl", "vulkan"};
    const auto api = dev->get_api();
    const int i = api == device_api::d3d9 ? 1 : api == device_api::d3d10 ? 2 : api == device_api::d3d11 ? 3
                : api == device_api::d3d12 ? 4 : api == device_api::opengl ? 5 : api == device_api::vulkan ? 6 : 0;
    Log("device: %s", apis[i]);
}

void OnInitSwapchain(swapchain* sc, bool resize) {
    const resource bb = sc->get_current_back_buffer();
    const resource_desc d = sc->get_device()->get_resource_desc(bb);
    char buf[160];
    snprintf(buf, sizeof(buf), "back buffer %ux%u, format %u, samples %u, %u buffers%s", d.texture.width,
             d.texture.height, (unsigned)d.texture.format, (unsigned)d.texture.samples,
             sc->get_back_buffer_count(), resize ? " (resize)" : "");
    std::lock_guard<std::recursive_mutex> lock(g_state);
    g_deviceInfo = buf;
    Log("swapchain: %s", buf);
}

bool KeyPressed(int vk, bool& was) {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    const bool down = pid == GetCurrentProcessId() && (GetAsyncKeyState(VK_CONTROL) & 0x8000) &&
                      (GetAsyncKeyState(vk) & 0x8000);
    const bool pressed = down && !was;
    was = down;
    return pressed;
}

void StartMeasure(double seconds) {
    g_frameMs.clear();
    QueryPerformanceCounter(&g_measureEnd);
    g_measureEnd.QuadPart += static_cast<LONGLONG>(seconds * g_freq.QuadPart);
    g_measureResult = "measuring...";
    g_measuring = true;
}

void OnPresent(command_queue*, swapchain*, const rect*, const rect*, uint32_t, const rect*) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    std::lock_guard<std::recursive_mutex> lock(g_state);
    if (!g_firstPresent.QuadPart) g_firstPresent = now;
    // One automatic scan 20 s after the first frame (title screen), for the singleton list.
    if (!g_autoScanned && now.QuadPart - g_firstPresent.QuadPart > 20 * g_freq.QuadPart) {
        g_autoScanned = true;
        Scan();
    }
    if (KeyPressed(VK_F9, g_f9)) Snapshot();
    if (KeyPressed(VK_F10, g_f10) && !g_measuring) StartMeasure(10.0);
    if (g_measuring && g_last.QuadPart) {
        g_frameMs.push_back((now.QuadPart - g_last.QuadPart) * 1000.0 / g_freq.QuadPart);
        if (now.QuadPart >= g_measureEnd.QuadPart) {
            g_measuring = false;
            std::vector<double> v = g_frameMs;
            double sum = 0;
            for (double ms : v) sum += ms;
            std::sort(v.begin(), v.end());
            double worst = 0;  // mean of the slowest 1%
            const size_t n = std::max<size_t>(1, v.size() / 100);
            for (size_t i = v.size() - n; i < v.size(); ++i) worst += v[i];
            worst /= n;
            const double avg = sum / v.size();
            char buf[160];
            snprintf(buf, sizeof(buf), "%.1f fps (%.2f ms), 1%% low %.1f fps, %zu frames", 1000.0 / avg, avg,
                     1000.0 / worst, v.size());
            g_measureResult = buf;
            Log("measure: %s", buf);
        }
    }
    g_last = now;
}

// ---------------------------------------------------------------- overlay

void DrawOverlay(effect_runtime*) {
    std::lock_guard<std::recursive_mutex> lock(g_state);
    ImGui::TextDisabled(DT_PRODUCT " " DT_VERSION " (probe: changes nothing in the game)");
    ImGui::TextUnformatted(g_deviceInfo.c_str());
    ImGui::Separator();
    if (ImGui::Button("Take snapshot (Ctrl+F9)")) Snapshot();
    ImGui::SameLine();
    ImGui::Text("%d taken", g_snapshot);
    ImGui::TextDisabled("Scan: %s", g_scanStatus.c_str());
    ImGui::TextWrapped("Snapshots go to " DT_PRODUCT "-probe\\ in the game folder. Take one while walking, "
                       "one while aiming, one in the pause menu.");
    std::set<std::string> shown;
    for (const Found& x : g_found)
        if (x.cls.size() > 1 && x.cls[0] == 's' && isupper((unsigned char)x.cls[1]) && shown.insert(x.cls).second)
            ImGui::TextDisabled("%-20s %08x -> %08x", x.cls.c_str(), (unsigned)x.where, (unsigned)x.obj);
    ImGui::Separator();
    ImGui::BeginDisabled(g_measuring);
    if (ImGui::Button("Measure frame rate (10 s, Ctrl+F10)")) StartMeasure(10.0);
    ImGui::EndDisabled();
    if (!g_measureResult.empty()) {
        ImGui::SameLine();
        ImGui::TextUnformatted(g_measureResult.c_str());
    }
}

void Init() {
    wchar_t path[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    g_gameDir.assign(path, n);
    g_gameDir.resize(g_gameDir.find_last_of(L'\\') + 1);
    QueryPerformanceFrequency(&g_freq);
    Log("---- " DT_PRODUCT " " DT_VERSION " loaded into %ls", path);
    MapImage();
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        if (!reshade::register_addon(module)) return FALSE;
        Init();
        reshade::register_event<reshade::addon_event::init_device>(&OnInitDevice);
        reshade::register_event<reshade::addon_event::init_swapchain>(&OnInitSwapchain);
        reshade::register_event<reshade::addon_event::present>(&OnPresent);
        reshade::register_overlay(DT_PRODUCT, &DrawOverlay);
        break;
    case DLL_PROCESS_DETACH:
        reshade::unregister_overlay(DT_PRODUCT, &DrawOverlay);
        reshade::unregister_addon(module);
        break;
    }
    return TRUE;
}
