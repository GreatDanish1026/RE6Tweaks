// RE6Tweaks: graphics and camera tweaks for Resident Evil 6, as a ReShade add-on (32-bit, D3D9).
//
// It fixes the startup crash on displays with many modes (see "display-mode fix" below).
// Otherwise 0.0.1 is a probe that changes nothing in the game. It maps the engine objects so the
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
#include <winternl.h>
#include <tlhelp32.h>
#include <d3d9.h>
#include <cwctype>

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
    static std::set<std::string> logged;  // each singleton once per session
    for (const Found& x : g_found)
        if (x.cls.size() > 1 && x.cls[0] == 's' && isupper((unsigned char)x.cls[1]) && logged.insert(x.cls).second)
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

// ---------------------------------------------------------------- display-mode fix
//
// The game copies every D3D9 display mode into a fixed table of 256 entries (0x5C bytes each,
// at sRender+0x4540E4) without a bounds check. A display with many resolutions and refresh
// rates (DSR/DLDSR, 165 Hz: 377 modes here) overflows it into the counters after it, and the
// game crashes at startup (BH6.exe+0xb429f6, string compare on a bad pointer). This hands the
// game a shorter list: every resolution at its highest refresh rate, then 60/120/144/165 Hz
// and the other rates while the list stays within kModeCap.
// It must be in place before the game enumerates modes, which is before ReShade normally
// loads add-ons, so ReShade.ini needs [ADDON] LoadFromDllMain=1.

constexpr UINT kModeCap = 128;
using CreateFn = IDirect3D9*(WINAPI*)(UINT);
using CountFn = UINT(STDMETHODCALLTYPE*)(IDirect3D9*, UINT, D3DFORMAT);
using EnumFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3D9*, UINT, D3DFORMAT, UINT, D3DDISPLAYMODE*);
CreateFn g_create = nullptr;
CountFn g_count = nullptr;
EnumFn g_enum = nullptr;
std::map<std::pair<UINT, int>, std::vector<D3DDISPLAYMODE>> g_modes;  // (adapter, format)
std::mutex g_modeLock;
bool g_modeFixActive = false;
std::string g_modeStatus = "display-mode fix: not active (add-on loaded after the game started)";

const std::vector<D3DDISPLAYMODE>& Modes(IDirect3D9* d3d, UINT adapter, D3DFORMAT fmt) {
    auto key = std::make_pair(adapter, (int)fmt);
    auto it = g_modes.find(key);
    if (it != g_modes.end()) return it->second;
    std::vector<D3DDISPLAYMODE> all;
    const UINT n = g_count(d3d, adapter, fmt);
    for (UINT i = 0; i < n; ++i) {
        D3DDISPLAYMODE m;
        if (SUCCEEDED(g_enum(d3d, adapter, fmt, i, &m))) all.push_back(m);
    }
    std::vector<D3DDISPLAYMODE>& out = g_modes[key];
    if (all.size() <= kModeCap) {
        out = all;
        return out;
    }
    std::vector<bool> keep(all.size(), false);
    size_t kept = 0;
    std::map<std::pair<UINT, UINT>, size_t> best;  // resolution -> index of its highest rate
    for (size_t i = 0; i < all.size(); ++i) {
        auto r = std::make_pair(all[i].Width, all[i].Height);
        auto b = best.find(r);
        if (b == best.end() || all[i].RefreshRate > all[b->second].RefreshRate) best[r] = i;
    }
    for (auto& b : best) keep[b.second] = true, ++kept;
    std::set<UINT> rates;
    for (auto& m : all) rates.insert(m.RefreshRate);
    std::vector<UINT> order = {60, 120, 144, 165};
    for (auto r = rates.rbegin(); r != rates.rend(); ++r)
        if (std::find(order.begin(), order.end(), *r) == order.end()) order.push_back(*r);
    for (UINT rate : order) {
        size_t add = 0;
        for (size_t i = 0; i < all.size(); ++i) add += !keep[i] && all[i].RefreshRate == rate;
        if (!add || kept + add > kModeCap) continue;
        for (size_t i = 0; i < all.size(); ++i)
            if (all[i].RefreshRate == rate) keep[i] = true;
        kept += add;
    }
    for (size_t i = 0; i < all.size(); ++i)
        if (keep[i]) out.push_back(all[i]);
    Log("display modes: adapter %u format %d: %zu -> %zu (%zu resolutions)", adapter, (int)fmt, all.size(),
        out.size(), best.size());
    return out;
}

UINT STDMETHODCALLTYPE HookCount(IDirect3D9* d3d, UINT adapter, D3DFORMAT fmt) {
    std::lock_guard<std::mutex> lock(g_modeLock);
    return (UINT)Modes(d3d, adapter, fmt).size();
}

HRESULT STDMETHODCALLTYPE HookEnum(IDirect3D9* d3d, UINT adapter, D3DFORMAT fmt, UINT i, D3DDISPLAYMODE* m) {
    std::lock_guard<std::mutex> lock(g_modeLock);
    const auto& v = Modes(d3d, adapter, fmt);
    if (!m) return D3DERR_INVALIDCALL;
    if (i >= v.size()) return D3DERR_INVALIDCALL;
    *m = v[i];
    return D3D_OK;
}

// The game lists the modes before it creates its device. At CreateDevice everything is put back
// (vtable and import), so add-ons that load during CreateDevice (RenoDX patches the same import
// and crashed on the hooked vtable) see the original d3d9 state.
using CreateDeviceFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD,
                                                   D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
CreateDeviceFn g_createDevice = nullptr;
void** g_hookedVt = nullptr;
uintptr_t* g_iatSlot = nullptr;

void SetSlots(void** vt, void* count, void* enumModes, void* createDevice) {
    DWORD old;
    if (!VirtualProtect(vt + 6, 11 * 4, PAGE_EXECUTE_READWRITE, &old)) return;
    vt[6] = count;
    vt[7] = enumModes;
    vt[16] = createDevice;
    VirtualProtect(vt + 6, 11 * 4, old, &old);
}

void HookDeviceSamplers(IDirect3DDevice9* dev);

HRESULT STDMETHODCALLTYPE HookCreateDevice(IDirect3D9* d3d, UINT adapter, D3DDEVTYPE type, HWND wnd, DWORD flags,
                                           D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** out) {
    const CreateDeviceFn original = g_createDevice;
    {
        std::lock_guard<std::mutex> lock(g_modeLock);
        if (g_hookedVt) {
            SetSlots(g_hookedVt, reinterpret_cast<void*>(g_count), reinterpret_cast<void*>(g_enum),
                     reinterpret_cast<void*>(g_createDevice));
            g_hookedVt = nullptr;
        }
        if (g_iatSlot) {
            DWORD old;
            if (VirtualProtect(g_iatSlot, 4, PAGE_READWRITE, &old)) {
                *g_iatSlot = reinterpret_cast<uintptr_t>(g_create);
                VirtualProtect(g_iatSlot, 4, old, &old);
            }
            g_iatSlot = nullptr;
        }
        g_modeStatus = "display-mode fix: applied at startup";
    }
    Log("display-mode fix: removed at CreateDevice (%ux%u)", pp ? pp->BackBufferWidth : 0,
        pp ? pp->BackBufferHeight : 0);
    const HRESULT hr = original(d3d, adapter, type, wnd, flags, pp, out);
    if (SUCCEEDED(hr) && out) HookDeviceSamplers(*out);
    return hr;
}

IDirect3D9* WINAPI HookCreate(UINT sdk) {
    IDirect3D9* d3d = g_create(sdk);
    if (!d3d) return d3d;
    std::lock_guard<std::mutex> lock(g_modeLock);
    void** vt = *reinterpret_cast<void***>(d3d);
    // IDirect3D9 vtable: 6 GetAdapterModeCount, 7 EnumAdapterModes, 16 CreateDevice.
    if (!g_hookedVt && !g_modeFixActive) {
        g_count = reinterpret_cast<CountFn>(vt[6]);
        g_enum = reinterpret_cast<EnumFn>(vt[7]);
        g_createDevice = reinterpret_cast<CreateDeviceFn>(vt[16]);
        SetSlots(vt, reinterpret_cast<void*>(&HookCount), reinterpret_cast<void*>(&HookEnum),
                 reinterpret_cast<void*>(&HookCreateDevice));
        g_hookedVt = vt;
        g_modeFixActive = true;
        g_modeStatus = "display-mode fix: active";
        Log("display-mode fix: hooked IDirect3D9 vtable %p", vt);
    }
    return d3d;
}

// Replaces BH6.exe's import of d3d9.dll!Direct3DCreate9 (already bound when DllMain runs).
void InstallModeFix() {
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(g_base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(g_base + dos->e_lfanew);
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return;
    for (auto* imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(g_base + dir.VirtualAddress); imp->Name; ++imp) {
        if (_stricmp(reinterpret_cast<const char*>(g_base + imp->Name), "d3d9.dll")) continue;
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(g_base + imp->OriginalFirstThunk);
        auto* iat = reinterpret_cast<IMAGE_THUNK_DATA*>(g_base + imp->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++iat) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            auto* byName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(g_base + names->u1.AddressOfData);
            if (strcmp(reinterpret_cast<const char*>(byName->Name), "Direct3DCreate9")) continue;
            if (g_create) return;
            DWORD old;
            if (!VirtualProtect(&iat->u1.Function, 4, PAGE_READWRITE, &old)) return;
            g_create = reinterpret_cast<CreateFn>(iat->u1.Function);
            iat->u1.Function = reinterpret_cast<uintptr_t>(&HookCreate);
            g_iatSlot = reinterpret_cast<uintptr_t*>(&iat->u1.Function);
            VirtualProtect(&iat->u1.Function, 4, old, &old);
            Log("display-mode fix: Direct3DCreate9 import redirected");
            return;
        }
    }
    Log("display-mode fix: Direct3DCreate9 import not found");
}

// ---------------------------------------------------------------- RenoDX alignment shim
//
// RenoDX for RE6 (build 6AB159E0) allocates a 0x1840-byte object with HeapAlloc, which only
// guarantees 8-byte alignment in a 32-bit process, and its constructor clears it with movaps
// (16-byte alignment required). Whether it crashes depends on the heap layout of that launch
// (renodx-residentevil6.addon32+0x72a9c). Loading RE6Tweaks early made it crash every time.
// This redirects that add-on's own HeapAlloc/HeapFree/HeapReAlloc/HeapSize imports so its
// blocks are 16-byte aligned: a block that comes back misaligned is moved up 8 bytes inside a
// 16-byte larger allocation and remembered. It's installed as the DLL is mapped (after its
// imports are bound, before its code runs), via LdrRegisterDllNotification.

constexpr DWORD kRenoDxStamp = 0x6AB159E0;
struct Shifted {
    void* real;
    SIZE_T size;
};
std::mutex g_heapLock;
std::map<uintptr_t, Shifted> g_shifted;
std::string g_renoStatus = "RenoDX alignment fix: RenoDX not loaded";

LPVOID WINAPI AlignedHeapAlloc(HANDLE heap, DWORD flags, SIZE_T size) {
    void* p = HeapAlloc(heap, flags, size + 16);
    if (!p || !(reinterpret_cast<uintptr_t>(p) & 15)) return p;
    const uintptr_t q = (reinterpret_cast<uintptr_t>(p) + 15) & ~static_cast<uintptr_t>(15);
    std::lock_guard<std::mutex> lock(g_heapLock);
    g_shifted[q] = {p, size};
    return reinterpret_cast<void*>(q);
}

bool TakeShifted(LPCVOID mem, Shifted& out, bool erase) {
    std::lock_guard<std::mutex> lock(g_heapLock);
    auto it = g_shifted.find(reinterpret_cast<uintptr_t>(mem));
    if (it == g_shifted.end()) return false;
    out = it->second;
    if (erase) g_shifted.erase(it);
    return true;
}

BOOL WINAPI AlignedHeapFree(HANDLE heap, DWORD flags, LPVOID mem) {
    Shifted s;
    return HeapFree(heap, flags, TakeShifted(mem, s, true) ? s.real : mem);
}

SIZE_T WINAPI AlignedHeapSize(HANDLE heap, DWORD flags, LPCVOID mem) {
    Shifted s;
    if (TakeShifted(mem, s, false)) return s.size;
    const SIZE_T n = HeapSize(heap, flags, mem);
    return n == static_cast<SIZE_T>(-1) || n < 16 ? n : n - 16;  // unshifted blocks carry 16 spare bytes
}

LPVOID WINAPI AlignedHeapReAlloc(HANDLE heap, DWORD flags, LPVOID mem, SIZE_T size) {
    if (!mem) return AlignedHeapAlloc(heap, flags & ~HEAP_REALLOC_IN_PLACE_ONLY, size);
    Shifted s;
    const bool shifted = TakeShifted(mem, s, false);
    if (!shifted) {
        void* p = HeapReAlloc(heap, flags, mem, size + 16);
        if (!p || !(reinterpret_cast<uintptr_t>(p) & 15)) return p;
        if (flags & HEAP_REALLOC_IN_PLACE_ONLY) return p;  // can't move it; same address as before
        // Landed misaligned: move it into an aligned block.
        void* q = AlignedHeapAlloc(heap, flags & HEAP_ZERO_MEMORY, size);
        if (!q) return p;  // mem already moved to p: return it (valid, just misaligned)
        memcpy(q, p, size);
        HeapFree(heap, 0, p);
        return q;
    }
    if (flags & HEAP_REALLOC_IN_PLACE_ONLY) return size <= s.size ? mem : nullptr;
    void* q = AlignedHeapAlloc(heap, flags & HEAP_ZERO_MEMORY, size);
    if (!q) return nullptr;
    memcpy(q, mem, std::min(size, s.size));
    AlignedHeapFree(heap, 0, mem);
    return q;
}

void PatchRenoDx(HMODULE mod) {
    const auto base = reinterpret_cast<uintptr_t>(mod);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    if (nt->FileHeader.TimeDateStamp != kRenoDxStamp) {
        g_renoStatus = "RenoDX alignment fix: not needed for this RenoDX build";
        Log("RenoDX build %08x: alignment shim not applied (made for %08x)",
            (unsigned)nt->FileHeader.TimeDateStamp, (unsigned)kRenoDxStamp);
        return;
    }
    const struct { const char* name; void* fn; } hooks[] = {
        {"HeapAlloc", reinterpret_cast<void*>(&AlignedHeapAlloc)},
        {"HeapFree", reinterpret_cast<void*>(&AlignedHeapFree)},
        {"HeapReAlloc", reinterpret_cast<void*>(&AlignedHeapReAlloc)},
        {"HeapSize", reinterpret_cast<void*>(&AlignedHeapSize)},
    };
    int patched = 0;
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    for (auto* imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); imp->Name; ++imp) {
        if (_stricmp(reinterpret_cast<const char*>(base + imp->Name), "kernel32.dll")) continue;
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->OriginalFirstThunk);
        auto* iat = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++iat) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            const char* n = reinterpret_cast<const char*>(
                reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData)->Name);
            for (const auto& h : hooks)
                if (!strcmp(n, h.name)) {
                    DWORD old;
                    if (VirtualProtect(&iat->u1.Function, 4, PAGE_READWRITE, &old)) {
                        iat->u1.Function = reinterpret_cast<uintptr_t>(h.fn);
                        VirtualProtect(&iat->u1.Function, 4, old, &old);
                        ++patched;
                    }
                }
        }
    }
    g_renoStatus = patched == 4 ? "RenoDX alignment fix: active" : "RenoDX alignment fix: incomplete";
    Log("RenoDX alignment shim: %d of 4 heap imports redirected", patched);
}

struct LdrDllLoadedData {
    ULONG flags;
    const void* fullName;  // UNICODE_STRING*
    const UNICODE_STRING* baseName;
    PVOID base;
    ULONG size;
};
using LdrRegisterFn = LONG(NTAPI*)(ULONG, void(CALLBACK*)(ULONG, const LdrDllLoadedData*, PVOID), PVOID, PVOID*);

bool IsRenoDxName(const wchar_t* s, size_t n) {
    std::wstring w(s, n);
    std::transform(w.begin(), w.end(), w.begin(), ::towlower);
    static const std::wstring ext = L".addon32";
    return w.rfind(L"renodx-", 0) == 0 && w.size() > ext.size() &&
           w.compare(w.size() - ext.size(), ext.size(), ext) == 0;
}

void CALLBACK OnDllNotification(ULONG reason, const LdrDllLoadedData* data, PVOID) {
    if (reason != 1 || !data || !data->baseName) return;  // 1 = loaded
    if (IsRenoDxName(data->baseName->Buffer, data->baseName->Length / 2))
        PatchRenoDx(static_cast<HMODULE>(data->base));
}

void InstallRenoDxShim() {
    // Already loaded (RE6Tweaks loaded late): too late to help this launch, but log it.
    auto* reg = reinterpret_cast<LdrRegisterFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "LdrRegisterDllNotification"));
    PVOID cookie = nullptr;
    if (reg && reg(0, &OnDllNotification, nullptr, &cookie) == 0) Log("RenoDX alignment shim: waiting for RenoDX");
    else Log("RenoDX alignment shim: LdrRegisterDllNotification failed");
}

// ---------------------------------------------------------------- write trace (probe)
//
// Ctrl+F8: hardware data breakpoints (DR0..DR3) on camera fields, in every thread, for 2 s.
// A vectored handler counts the code address after each write (data breakpoints trap after
// the instruction), so the code that sets the field of view and camera position can be found.
// Threads are changed one at a time from a helper thread, with no allocation while a thread
// is suspended.

constexpr int kMaxHits = 64;
struct Hit {
    volatile LONG addr;
    volatile LONG count;
};
Hit g_hits[4][kMaxHits];
uintptr_t g_watch[4] = {};
const char* g_watchName[4] = {};
bool g_tracing = false;
LARGE_INTEGER g_traceEnd{};
PVOID g_veh = nullptr;

LONG CALLBACK TraceHandler(EXCEPTION_POINTERS* e) {
    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT* c = e->ContextRecord;
    const DWORD hit = c->Dr6 & 0xf;
    if (!hit) return EXCEPTION_CONTINUE_SEARCH;
    for (int i = 0; i < 4; ++i) {
        if (!(hit & (1u << i))) continue;
        const LONG eip = static_cast<LONG>(c->Eip);
        for (int k = 0; k < kMaxHits; ++k) {
            const LONG cur = InterlockedCompareExchange(&g_hits[i][k].addr, eip, 0);
            if (cur == 0 || cur == eip) {
                InterlockedIncrement(&g_hits[i][k].count);
                break;
            }
        }
    }
    c->Dr6 = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
}

struct DrJob {
    DWORD caller;
    bool enable;
};

DWORD WINAPI SetDebugRegisters(LPVOID p) {
    const DrJob* job = static_cast<DrJob*>(p);
    DWORD ids[1024];
    int n = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        THREADENTRY32 te{sizeof(te)};
        for (BOOL ok = Thread32First(snap, &te); ok && n < 1024; ok = Thread32Next(snap, &te))
            if (te.th32OwnerProcessID == GetCurrentProcessId() && te.th32ThreadID != GetCurrentThreadId())
                ids[n++] = te.th32ThreadID;
        CloseHandle(snap);
    }
    DWORD dr7 = 0;
    if (job->enable)
        for (int i = 0; i < 4; ++i)
            if (g_watch[i]) dr7 |= (1u << (2 * i)) | (1u << (16 + 4 * i)) | (3u << (18 + 4 * i));  // local, write, 4 bytes
    for (int i = 0; i < n; ++i) {
        HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, ids[i]);
        if (!t) continue;
        if (SuspendThread(t) != (DWORD)-1) {
            CONTEXT c{};
            c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (GetThreadContext(t, &c)) {
                c.Dr0 = job->enable ? g_watch[0] : 0;
                c.Dr1 = job->enable ? g_watch[1] : 0;
                c.Dr2 = job->enable ? g_watch[2] : 0;
                c.Dr3 = job->enable ? g_watch[3] : 0;
                c.Dr6 = 0;
                c.Dr7 = dr7;
                SetThreadContext(t, &c);
            }
            ResumeThread(t);
        }
        CloseHandle(t);
    }
    return n;
}

int RunDrJob(bool enable) {
    DrJob job{GetCurrentThreadId(), enable};
    HANDLE h = CreateThread(nullptr, 0, &SetDebugRegisters, &job, 0, nullptr);
    if (!h) return 0;
    WaitForSingleObject(h, 5000);
    DWORD n = 0;
    GetExitCodeThread(h, &n);
    CloseHandle(h);
    return static_cast<int>(n);
}

uintptr_t FindPointee(uintptr_t obj, size_t size, const char* cls) {
    size = std::min(size, ReadableFrom(obj));
    for (size_t o = 0; o + 4 <= size; o += 4) {
        uint32_t v = 0;
        if (!Read32(obj + o, v) || v < 0x10000 || g_image.In(v)) continue;
        const char* c = ClassOf(v);
        if (c && !strcmp(c, cls)) return v;
    }
    return 0;
}

uintptr_t g_bioCamera = 0, g_cameraCtrl = 0;

bool FindCamera() {
    MapReadable();
    uint32_t cam = 0;
    g_bioCamera = Read32(0x186e23c, cam) && cam && ClassOf(cam) && !strcmp(ClassOf(cam), "sBioCamera") ? cam : 0;
    if (!g_bioCamera) {
        Scan();
        for (const Found& x : g_found)
            if (x.cls == "sBioCamera") g_bioCamera = x.obj;
    }
    g_cameraCtrl = g_bioCamera ? FindPointee(g_bioCamera, 0x4000, "uCameraCtrl") : 0;
    Log("camera: sBioCamera %08x, uCameraCtrl %08x", (unsigned)g_bioCamera, (unsigned)g_cameraCtrl);
    return g_bioCamera && g_cameraCtrl;
}

void StartTrace() {
    if (g_tracing || !FindCamera()) return;
    memset(g_hits, 0, sizeof(g_hits));
    g_watch[0] = g_cameraCtrl + 0x4c, g_watchName[0] = "uCameraCtrl+0x4c fov";
    g_watch[1] = g_bioCamera + 0xe60, g_watchName[1] = "sBioCamera+0xe60 fov";
    g_watch[2] = g_cameraCtrl + 0x50, g_watchName[2] = "uCameraCtrl+0x50 pos.x";
    g_watch[3] = g_cameraCtrl + 0x70, g_watchName[3] = "uCameraCtrl+0x70 target.x";
    if (!g_veh) g_veh = AddVectoredExceptionHandler(1, &TraceHandler);
    const int n = RunDrJob(true);
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&g_traceEnd);
    g_traceEnd.QuadPart += 2 * freq.QuadPart;
    g_tracing = true;
    Log("trace: watching %d threads for 2 s", n);
}

void StopTrace() {
    RunDrJob(false);
    g_tracing = false;
    for (int i = 0; i < 4; ++i) {
        Log("trace %s (%08x):", g_watchName[i], (unsigned)g_watch[i]);
        for (int k = 0; k < kMaxHits && g_hits[i][k].addr; ++k)
            Log("   writer: code before %08x  x%ld", (unsigned)g_hits[i][k].addr, g_hits[i][k].count);
    }
}

// ---------------------------------------------------------------- camera options
//
// The gameplay camera unit (uCameraCtrl) fills its standard camera fields once per frame at
// 0x60DF18..0x60DF76 from its own results: far +0x40, near +0x44, aspect +0x48, vertical
// field of view in degrees +0x4C (from +0x62C), position +0x50 (from +0x630), up +0x60,
// look-at +0x70 (from +0x650). The view is built from those fields; the results it copies
// from are recomputed every frame, so changing the fields does not feed back.
// (sBioCamera's copy at 0x4F9950 is only a saved-camera slot; changing it shows nothing.)
// A jump at 0x60DF70, right after the field of view is written, calls OnCameraCtrl:
//   - field of view: tan(fov/2) is multiplied, so the aiming zoom keeps its ratio;
//   - distance: the position moves along the look-at line, so the crosshair stays on target.
//     The game's wall collision was computed for the original distance, so far settings can
//     clip into walls in tight spaces.
// The replaced instructions (fld [esi+6A4h]; fstp [esi+0C4h]) run first, and the flags from
// the test at 0x60DF60 are kept for the je at 0x60DF7C. Cutscenes use other camera units.

constexpr DWORD kExeStamp = 0x5a543b8a;
constexpr uintptr_t kCameraPatch = 0x60DF70;
const uint8_t kCameraBytes[12] = {0xD9, 0x86, 0xA4, 0x06, 0x00, 0x00, 0xD9, 0x9E, 0xC4, 0x00, 0x00, 0x00};

struct Choice {
    const char* label;
    float value;
};
const Choice kFovChoices[] = {{"Game", 1.0f}, {"+10%", 1.1f}, {"+20%", 1.2f}, {"+30%", 1.3f}, {"+40%", 1.4f}, {"+50%", 1.5f}};
const Choice kDistChoices[] = {{"Game", 1.0f}, {"1.15x", 1.15f}, {"1.3x", 1.3f}, {"1.5x", 1.5f}, {"1.75x", 1.75f}};
int g_fovChoice = 0, g_distChoice = 0;
volatile float g_fovScale = 1.0f, g_distScale = 1.0f;
std::string g_cameraStatus = "camera options: not installed";
volatile LONG g_viewportCalls = 0, g_viewportChanged = 0;

bool g_loggedChange = false;
void ApplyShadows();       // shadow options; runs on the game thread from the camera hook
void ApplyDrawDistance();  // draw distance option; same
void ApplyFilters();       // ambient occlusion / god rays / motion blur quality; same

void __stdcall OnCameraCtrl(uintptr_t unit) {
    InterlockedIncrement(&g_viewportCalls);
    ApplyShadows();
    ApplyDrawDistance();
    ApplyFilters();
    const float fs = g_fovScale, ds = g_distScale;
    if (fs == 1.0f && ds == 1.0f) return;
    float* fov = reinterpret_cast<float*>(unit + 0x4C);
    float* pos = reinterpret_cast<float*>(unit + 0x50);
    const float* at = reinterpret_cast<const float*>(unit + 0x70);
    const float fov0 = *fov;
    if (fs != 1.0f && fov0 > 1.0f && fov0 < 170.0f) {
        const float half = fov0 * (3.14159265f / 360.0f);
        *fov = std::min(std::atan(std::tan(half) * fs) * (360.0f / 3.14159265f), 150.0f);
    }
    if (ds != 1.0f)
        for (int i = 0; i < 3; ++i) pos[i] = at[i] + (pos[i] - at[i]) * ds;
    InterlockedIncrement(&g_viewportChanged);
    if (!g_loggedChange) {
        g_loggedChange = true;
        Log("camera: first change, unit %08x, fov %.2f -> %.2f, distance x%.2f", (unsigned)unit, fov0, *fov, ds);
    }
}

__declspec(naked) void CameraStub() {
    __asm {
        fld dword ptr [esi + 6A4h]
        fstp dword ptr [esi + 0C4h]
        pushad
        pushfd
        push esi
        call OnCameraCtrl
        popfd
        popad
        push 0060DF7Ch  // back to the je after the replaced instructions
        ret
    }
}

void InstallCameraHook() {
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(g_base + reinterpret_cast<IMAGE_DOS_HEADER*>(g_base)->e_lfanew);
    if (nt->FileHeader.TimeDateStamp != kExeStamp || g_base != 0x400000) {
        g_cameraStatus = "camera options: unsupported game version";
        Log("camera hook: exe timestamp %08x not supported", (unsigned)nt->FileHeader.TimeDateStamp);
        return;
    }
    auto* site = reinterpret_cast<uint8_t*>(kCameraPatch);
    if (memcmp(site, kCameraBytes, sizeof(kCameraBytes))) {
        g_cameraStatus = "camera options: code not as expected (another mod?)";
        Log("camera hook: unexpected bytes at %08x", (unsigned)kCameraPatch);
        return;
    }
    DWORD old;
    if (!VirtualProtect(site, sizeof(kCameraBytes), PAGE_EXECUTE_READWRITE, &old)) return;
    uint8_t patch[12] = {0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
    const int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&CameraStub) - (kCameraPatch + 5));
    memcpy(patch + 1, &rel, 4);
    memcpy(site, patch, sizeof(patch));
    VirtualProtect(site, sizeof(kCameraBytes), old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, sizeof(kCameraBytes));
    g_cameraStatus = "camera options: active";
    Log("camera hook installed at %08x", (unsigned)kCameraPatch);
}

// ---------------------------------------------------------------- shadow options
//
// Shadows are scene units derived from uShadow: uLightSpaceShadow, uStableShadow (sun),
// uUniformShadow, uSpotShadow (lamps, flashlight). uShadow fields (from its property list):
// +0x58 mShadowViewDistance, +0x70 mBackforwardViewDistance, +0x78 mShadowMapType,
// +0x84 mMapSize (setter 0x1039680 frees and rebuilds the maps), +0xCC mDispersion,
// +0xEC mPCFType, +0xF0 mVSMBlurType.
//
// Live units are tracked through their own vtables, never by scanning memory (a memory scan
// also finds stale copies of freed units; changing those hung the game). Every vtable entry
// from slot 5 on is replaced by a small stub that records `this` and jumps to the original,
// so any call the engine makes marks the unit as live. Slot 0 (the destructor) removes it
// first. The game thread (camera hook, once per gameplay frame) applies the settings to the
// live units through the game's setter, after checking their values are sane.

struct ShadowClass {
    uint32_t vt;
    const char* name;
    int maxEntries = 64;  // vtable length cap (the walk stops earlier at non-code values)
};
const ShadowClass kShadowClasses[] = {
    {0x016ea768, "uLightSpaceShadow"}, {0x016ea800, "uStableShadow"},
    {0x016ea888, "uUniformShadow"},    {0x016ea910, "uSpotShadow"},
};
// Every class whose live units are tracked through vtable hooks: the shadows, and the
// post-processing filters with quality settings (see "filter quality" below).
const ShadowClass kTrackedClasses[] = {
    {0x016ea768, "uLightSpaceShadow"},       {0x016ea800, "uStableShadow"},
    {0x016ea888, "uUniformShadow"},          {0x016ea910, "uSpotShadow"},
    // Filters: 18 entries (the base filter vtable; uMultiGodRaysFilter's walk otherwise runs on
    // into the following data).
    {0x016e8390, "uAmbientOcclusionFilter", 18}, {0x016e8640, "uGodRaysFilter", 18},
    {0x016fa0e0, "uMultiGodRaysFilter", 18},     {0x016e7640, "uMotionBlurFilter", 18},
};
constexpr int kTrackedCount = static_cast<int>(sizeof(kTrackedClasses) / sizeof(kTrackedClasses[0]));
using SetU32Fn = void(__thiscall*)(void*, uint32_t);
const SetU32Fn kSetMapSize = reinterpret_cast<SetU32Fn>(0x1039680);
const SetU32Fn kSetPcfType = reinterpret_cast<SetU32Fn>(0x1028c50);  // mPCFType (+0xEC), plain store
// The shadow shaders switch on mPCFType: 0, 1 (the game's choice), 2, and 3 or more.
const Choice kShadowFilterChoices[] = {{"Game", 0.0f}, {"Filter 2", 2.0f}, {"Filter 3", 3.0f}};
int g_shadowFilterChoice = 0;
volatile int g_shadowFilter = 0;  // 0 = game

const Choice kShadowResChoices[] = {{"Game", 1.0f}, {"2x", 2.0f}, {"4x", 4.0f}};
const Choice kShadowDistChoices[] = {{"Game", 1.0f}, {"1.5x", 1.5f}, {"2x", 2.0f}};
int g_shadowResChoice = 0, g_shadowDistChoice = 0;
volatile float g_shadowResScale = 1.0f, g_shadowDistScale = 1.0f;
constexpr uint32_t kMaxShadowMap = 8192;

struct ShadowUnit {
    uint32_t vt = 0;
    uint32_t origSize = 0, appliedSize = 0;
    float origDist = 0, appliedDist = 0;
    bool logged = false;
    uint32_t origPcf = 0, appliedPcf = 0;
    int resets = 0;  // times the game put its own map size back after we set ours
    bool gaveUp = false;
};
SRWLOCK g_liveLock = SRWLOCK_INIT;
std::set<uintptr_t> g_live;                     // units the engine has called and not destroyed
std::map<uintptr_t, ShadowUnit> g_shadowUnits;  // game thread only
std::string g_shadowStatus = "shadows: none seen yet (gameplay only)";
std::string g_shadowHookStatus = "shadow hooks: not installed";

bool IsTrackedVt(uint32_t vt) {
    for (const auto& c : kTrackedClasses)
        if (c.vt == vt) return true;
    return false;
}

void __stdcall RecordUnit(uintptr_t obj) {
    // Only units of the tracked classes (a vtable walk that ran into a neighbouring class's
    // entries must not fill the set with other objects). Called by the engine on a live object.
    uint32_t vt = 0;
    __try {
        vt = *reinterpret_cast<const uint32_t*>(obj);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    if (!IsTrackedVt(vt)) return;
    AcquireSRWLockShared(&g_liveLock);
    const bool known = g_live.count(obj) != 0;
    ReleaseSRWLockShared(&g_liveLock);
    if (known) return;
    AcquireSRWLockExclusive(&g_liveLock);
    g_live.insert(obj);
    ReleaseSRWLockExclusive(&g_liveLock);
}

void __stdcall ForgetUnit(uintptr_t obj) {
    AcquireSRWLockExclusive(&g_liveLock);
    g_live.erase(obj);
    ReleaseSRWLockExclusive(&g_liveLock);
}

// Stub: pushad; pushfd; push ecx; call fn; popfd; popad; jmp [orig]
uint8_t* MakeStub(uint8_t*& mem, void* fn, const uint32_t* orig) {
    uint8_t* s = mem;
    s[0] = 0x60;
    s[1] = 0x9C;
    s[2] = 0x51;
    s[3] = 0xE8;
    const int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(fn) - reinterpret_cast<uintptr_t>(s + 8));
    memcpy(s + 4, &rel, 4);
    s[8] = 0x9D;
    s[9] = 0x61;
    s[10] = 0xFF;
    s[11] = 0x25;
    const uint32_t p = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(orig));
    memcpy(s + 12, &p, 4);
    mem += 16;
    return s;
}

void InstallShadowHooks() {
    // Original entries are kept here (the stubs jump through them).
    static uint32_t originals[kTrackedCount][64];
    auto* mem = static_cast<uint8_t*>(
        VirtualAlloc(nullptr, kTrackedCount * 64 * 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!mem) return;
    int hooked = 0;
    for (int c = 0; c < kTrackedCount; ++c) {
        auto* vt = reinterpret_cast<uint32_t*>(kTrackedClasses[c].vt);
        int n = 0;
        // Entries until a non-code value or the start of another tracked class's vtable.
        auto otherVtable = [&](uintptr_t a) {
            for (const auto& o : kTrackedClasses)
                if (o.vt != kTrackedClasses[c].vt && o.vt == a) return true;
            return false;
        };
        const int cap = std::min(64, kTrackedClasses[c].maxEntries);
        while (n < cap && g_text.In(vt[n]) && !otherVtable(reinterpret_cast<uintptr_t>(vt + n))) ++n;
        if (n < 8) {
            Log("unit hooks: %s vtable looks wrong (%d entries)", kTrackedClasses[c].name, n);
            continue;
        }
        DWORD old;
        if (!VirtualProtect(vt, n * 4, PAGE_READWRITE, &old)) continue;
        for (int i = 0; i < n; ++i) {
            if (i >= 1 && i <= 4) continue;  // createUI/isEnable/createProperty/getDTI: not per-frame
            originals[c][i] = vt[i];
            vt[i] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(
                MakeStub(mem, i == 0 ? reinterpret_cast<void*>(&ForgetUnit) : reinterpret_cast<void*>(&RecordUnit),
                         &originals[c][i])));
            ++hooked;
        }
        VirtualProtect(vt, n * 4, old, &old);
        Log("unit hooks: %s, %d vtable entries", kTrackedClasses[c].name, n);
    }
    FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
    g_shadowHookStatus = hooked ? "shadow hooks: active" : "shadow hooks: failed";
}

const char* ShadowName(uint32_t vt) {
    for (const auto& c : kShadowClasses)
        if (c.vt == vt) return c.name;
    return "?";
}

struct UnitValues {
    uint32_t vt, size, pcf, vsm;
    float dist, backForward, dispersion;
};

bool ReadUnit(uintptr_t obj, UnitValues& v) {
    __try {
        v.vt = *reinterpret_cast<const uint32_t*>(obj);
        v.size = *reinterpret_cast<const uint32_t*>(obj + 0x84);
        v.pcf = *reinterpret_cast<const uint32_t*>(obj + 0xEC);
        v.vsm = *reinterpret_cast<const uint32_t*>(obj + 0xF0);
        v.dist = *reinterpret_cast<const float*>(obj + 0x58);
        v.backForward = *reinterpret_cast<const float*>(obj + 0x70);
        v.dispersion = *reinterpret_cast<const float*>(obj + 0xCC);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool Sane(const UnitValues& v) {
    bool cls = false;
    for (const auto& c : kShadowClasses) cls |= c.vt == v.vt;
    return cls && v.size >= 32 && v.size <= kMaxShadowMap && !(v.size & 31) && std::isfinite(v.dist) &&
           v.dist > 0.0f && v.dist < 1e6f && v.pcf < 16 && v.vsm < 16;
}

bool CallSetMapSize(uintptr_t obj, uint32_t size) {
    __try {
        kSetMapSize(reinterpret_cast<void*>(obj), size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool CallSetU32(SetU32Fn fn, uintptr_t obj, uint32_t v) {
    __try {
        fn(reinterpret_cast<void*>(obj), v);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool CopyGuarded(uintptr_t src, void* dst, size_t n) {  // read without the probe's memory map
    __try {
        memcpy(dst, reinterpret_cast<const void*>(src), n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool WriteFloat(uintptr_t a, float v) {
    __try {
        *reinterpret_cast<float*>(a) = v;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void ApplyShadows() {
    std::vector<uintptr_t> live;
    AcquireSRWLockShared(&g_liveLock);
    live.assign(g_live.begin(), g_live.end());
    ReleaseSRWLockShared(&g_liveLock);
    // Forget units that are gone.
    for (auto it = g_shadowUnits.begin(); it != g_shadowUnits.end();)
        it = std::binary_search(live.begin(), live.end(), it->first) ? std::next(it) : g_shadowUnits.erase(it);

    const float rs = g_shadowResScale, ds = g_shadowDistScale;
    static LONG sizeSets = 0, distSets = 0, gaveUp = 0;
    static DWORD lastSummary = GetTickCount();
    int active = 0;
    for (uintptr_t obj : live) {
        UnitValues v;
        if (!ReadUnit(obj, v) || !Sane(v)) continue;
        ShadowUnit& u = g_shadowUnits[obj];
        if (u.vt != v.vt) u = ShadowUnit{v.vt};
        if (!u.logged) {
            u.logged = true;
            Log("shadow unit %08x %s: map %u, view distance %.1f, back/forward %.1f, PCF %u, VSM blur %u, dispersion %.2f",
                (unsigned)obj, ShadowName(v.vt), v.size, v.dist, v.backForward, v.pcf, v.vsm, v.dispersion);
        }
        ++active;
        // Map size: origSize is the game's value (anything that isn't the value we set). The
        // setter rebuilds the maps, so a unit whose size the game keeps resetting is left alone
        // after 3 resets (rebuilding every frame would cost far more than it gains).
        if (u.appliedSize && v.size != u.appliedSize) ++u.resets;
        if (!u.appliedSize || v.size != u.appliedSize) u.origSize = v.size;
        if (rs == 1.0f) {
            if (u.appliedSize && v.size == u.appliedSize) CallSetMapSize(obj, u.origSize);
            u.appliedSize = 0;
            u.resets = 0;
        } else if (u.resets > 3) {
            if (!u.gaveUp) {
                u.gaveUp = true;
                ++gaveUp;
                Log("shadow unit %08x: the game keeps resetting its map size (%u); left at the game's value",
                    (unsigned)obj, v.size);
            }
            u.appliedSize = 0;
        } else {
            const uint32_t want = std::min(static_cast<uint32_t>(u.origSize * rs), std::max(kMaxShadowMap, u.origSize));
            if (v.size != want && CallSetMapSize(obj, want)) ++sizeSets;
            u.appliedSize = want;
        }
        // View distance.
        if (u.appliedDist == 0.0f || v.dist != u.appliedDist) u.origDist = v.dist;
        if (ds == 1.0f) {
            if (u.appliedDist != 0.0f && v.dist == u.appliedDist) WriteFloat(obj + 0x58, u.origDist);
            u.appliedDist = 0.0f;
        } else {
            const float want = u.origDist * ds;
            if (v.dist != want && WriteFloat(obj + 0x58, want)) ++distSets;
            u.appliedDist = want;
        }
        // Shadow filter (plain field; the game's own setter).
        const int pf = g_shadowFilter;
        if (!u.appliedPcf || v.pcf != u.appliedPcf) u.origPcf = v.pcf;
        if (pf == 0) {
            if (u.appliedPcf && v.pcf == u.appliedPcf) CallSetU32(kSetPcfType, obj, u.origPcf);
            u.appliedPcf = 0;
        } else if (u.origPcf != 0) {  // 0 = filtering off for this light; leave it
            if (v.pcf != static_cast<uint32_t>(pf)) CallSetU32(kSetPcfType, obj, pf);
            u.appliedPcf = pf;
        }
    }
    // Summary every 10 s while something was set (the game recomputes some view distances
    // every frame, so distance writes repeat; they are cheap).
    if (GetTickCount() - lastSummary >= 10000) {
        if (sizeSets || distSets)
            Log("shadows (10 s): %d units, %ld map-size sets, %ld distance writes, %ld units left to the game "
                "(resolution x%.0f, distance x%.1f)",
                active, sizeSets, distSets, gaveUp, rs, ds);
        sizeSets = distSets = 0;
        lastSummary = GetTickCount();
    }
    char buf[96];
    snprintf(buf, sizeof(buf), "shadows: %d live shadow units", active);
    g_shadowStatus = buf;
}

// ---------------------------------------------------------------- filter quality
//
// Post-processing filters are units too, tracked like the shadows (vtable hooks). Fields from
// their property lists:
//   uAmbientOcclusionFilter (vt 0x16e8390): mResolution +0x2B8 (setter 0xF885C0 rebuilds the
//     AO targets; 0 = screen size, 1 = half, 2 = half/mixed, 3 = quarter, 4 = ...),
//     mBlurTapNum +0x2C0;
//   uGodRaysFilter (vt 0x16e8640) and uMultiGodRaysFilter (0x16fa0e0): mIterativeNum, byte +0x81
//     (its setter is a plain store);
//   uMotionBlurFilter (vt 0x16e7640): mResolution +0x7C, mVelocityResolution +0x84 (0..3).
//     Logged only: in game both are already 0 (full resolution), so there is no option for it.
// In the first test area (Leon, chapter 1) no AO or god rays filter existed at all; the
// options only act where a level creates them (the overlay counts them).
// Applied on the game thread from the camera hook; the game's values come back with Game.

const Choice kAoChoices[] = {{"Game", 0.0f}, {"Full resolution", 1.0f}, {"Full resolution, smoother", 2.0f}};
const Choice kGodRayChoices[] = {{"Game", 0.0f}, {"Smoother (2x iterations)", 1.0f}};
int g_aoChoice = 0, g_godRayChoice = 0;
constexpr int g_motionBlurChoice = 0;  // no option: the game already uses full resolution
const SetU32Fn kSetAoResolution = reinterpret_cast<SetU32Fn>(0xF885C0);

struct FilterUnit {
    uint32_t vt = 0;
    bool logged = false;
    uint32_t a0 = 0, a1 = 0;      // game values (meaning per class)
    uint32_t w0 = ~0u, w1 = ~0u;  // values written by us (~0 = none)
};
std::map<uintptr_t, FilterUnit> g_filterUnits;  // game thread only
std::string g_filterStatus = "filters: none seen yet (gameplay only)";

bool ReadU32(uintptr_t a, uint32_t& v) { return CopyGuarded(a, &v, 4); }
bool WriteU32(uintptr_t a, uint32_t v) {
    __try {
        *reinterpret_cast<uint32_t*>(a) = v;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool WriteU8(uintptr_t a, uint8_t v) {
    __try {
        *reinterpret_cast<uint8_t*>(a) = v;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// One field: the game's value is tracked in `orig` (anything that isn't what we wrote);
// want(orig) is written while the option is on, and the game's value put back when it's off.
template <typename Set, typename Want>
void ApplyField(uint32_t cur, uint32_t& orig, uint32_t& written, bool on, Set set, Want want) {
    if (written == ~0u || cur != written) orig = cur;
    if (!on) {
        if (written != ~0u && cur == written) set(orig);
        written = ~0u;
        return;
    }
    const uint32_t w = want(orig);
    if (cur != w) set(w);
    written = w;
}

void ApplyFilters() {
    std::vector<uintptr_t> live;
    AcquireSRWLockShared(&g_liveLock);
    live.assign(g_live.begin(), g_live.end());
    ReleaseSRWLockShared(&g_liveLock);
    for (auto it = g_filterUnits.begin(); it != g_filterUnits.end();)
        it = std::binary_search(live.begin(), live.end(), it->first) ? std::next(it) : g_filterUnits.erase(it);
    int ao = 0, rays = 0, blur = 0;
    for (uintptr_t obj : live) {
        uint32_t vt = 0;
        if (!ReadU32(obj, vt)) continue;
        if (vt != 0x016e8390 && vt != 0x016e8640 && vt != 0x016fa0e0 && vt != 0x016e7640) continue;
        FilterUnit& u = g_filterUnits[obj];
        if (u.vt != vt) u = FilterUnit{vt};
        if (vt == 0x016e8390) {  // ambient occlusion
            uint32_t res = 0, taps = 0, bounce = 0;
            if (!ReadU32(obj + 0x2B8, res) || !ReadU32(obj + 0x2C0, taps) || !ReadU32(obj + 0x2AC, bounce) || res > 4 ||
                taps > 256)
                continue;
            if (!u.logged) {
                u.logged = true;
                Log("filter %08x uAmbientOcclusionFilter: resolution %u, blur taps %u, indirect bounce %u", (unsigned)obj,
                    res, taps, bounce & 0xFF);
            }
            ++ao;
            ApplyField(res, u.a0, u.w0, g_aoChoice >= 1, [&](uint32_t v) { CallSetU32(kSetAoResolution, obj, v); },
                       [](uint32_t) { return 0u; });
            ApplyField(taps, u.a1, u.w1, g_aoChoice >= 2, [&](uint32_t v) { WriteU32(obj + 0x2C0, v); },
                       [](uint32_t o) { return std::min(std::max(o * 2, 4u), 32u); });
        } else if (vt == 0x016e8640 || vt == 0x016fa0e0) {  // god rays
            uint8_t iter = 0;
            if (!CopyGuarded(obj + 0x81, &iter, 1)) continue;
            if (!u.logged) {
                u.logged = true;
                uint32_t bits = 0;
                ReadU32(obj + 0x80, bits);
                Log("filter %08x %s: iterations %u, quality %u, mode %u", (unsigned)obj,
                    vt == 0x016e8640 ? "uGodRaysFilter" : "uMultiGodRaysFilter", iter, (bits >> 4) & 15, bits & 15);
            }
            ++rays;
            ApplyField(iter, u.a0, u.w0, g_godRayChoice >= 1,
                       [&](uint32_t v) { WriteU8(obj + 0x81, static_cast<uint8_t>(v)); },
                       [](uint32_t o) { return std::min(std::max(o * 2, 2u), 64u); });
        } else {  // motion blur
            uint32_t res = 0, vres = 0, samples = 0;
            if (!ReadU32(obj + 0x7C, res) || !ReadU32(obj + 0x84, vres) || !ReadU32(obj + 0x78, samples) || res > 3 ||
                vres > 3)
                continue;
            if (!u.logged) {
                u.logged = true;
                Log("filter %08x uMotionBlurFilter: resolution %u, velocity resolution %u, sample level %u",
                    (unsigned)obj, res, vres, samples);
            }
            ++blur;
            ApplyField(res, u.a0, u.w0, g_motionBlurChoice >= 1, [&](uint32_t v) { WriteU32(obj + 0x7C, v); },
                       [](uint32_t) { return 0u; });
            ApplyField(vres, u.a1, u.w1, g_motionBlurChoice >= 1, [&](uint32_t v) { WriteU32(obj + 0x84, v); },
                       [](uint32_t) { return 0u; });
        }
    }
    char buf[128];
    snprintf(buf, sizeof(buf), "active here: %d ambient occlusion filter(s), %d god rays filter(s)%s", ao, rays,
             ao + rays ? "" : " (this area has neither)");
    (void)blur;
    g_filterStatus = buf;
}

// ---------------------------------------------------------------- draw distance
//
// RE6 has no single model-LOD distance (uBaseModel's LOD type is a per-class virtual); its
// distance limits are fields of singletons (from their property lists):
//   sEffect (instance sBioEffect, static 0x186e430): mCullDist, 8 entries of {distance,
//     distance squared} at +0x238 (count function 0xB1A030 returns 8); the setter 0xF0C5C0
//     stores both, so both are scaled here;
//   sGrass (static 0x1870818): mFadeBeginDistance +0x54, mFadeEndDistance +0x58,
//     mLODBeginDistance +0xD4, mLODEndDistance +0xD8.
// (sEnemy's LOD mLength[] fields belong to a property function whose owner isn't certain, and
// only change enemy update rates, so they are not touched.)
// Each singleton's class is checked by name before writing. Values are scaled from the game's
// own (a value that isn't the one written here is taken as the game's new value), so Game
// restores them. Applied on the game thread, once per gameplay frame.

const Choice kDrawChoices[] = {{"Game", 1.0f}, {"1.5x", 1.5f}, {"2x", 2.0f}, {"3x", 3.0f}};
int g_drawChoice = 0;
volatile float g_drawScale = 1.0f;

struct ScaledField {
    uintptr_t staticPtr;
    const char* cls;
    uint32_t off;
    bool squaredNext;  // the next float holds this value squared
    float orig = 0, applied = 0;
};
ScaledField g_drawFields[] = {
    {0x186e430, "sBioEffect", 0x238, true}, {0x186e430, "sBioEffect", 0x240, true},
    {0x186e430, "sBioEffect", 0x248, true}, {0x186e430, "sBioEffect", 0x250, true},
    {0x186e430, "sBioEffect", 0x258, true}, {0x186e430, "sBioEffect", 0x260, true},
    {0x186e430, "sBioEffect", 0x268, true}, {0x186e430, "sBioEffect", 0x270, true},
    {0x1870818, "sGrass", 0x54, false},     {0x1870818, "sGrass", 0x58, false},
    {0x1870818, "sGrass", 0xD4, false},     {0x1870818, "sGrass", 0xD8, false},
};
std::map<uintptr_t, uint32_t> g_singletonVt;  // static -> verified vtable, 0 = wrong class (game thread)
bool g_drawLogged = false;

uintptr_t VerifiedSingleton(uintptr_t staticPtr, const char* cls) {
    uint32_t obj = 0, vt = 0;
    if (!CopyGuarded(staticPtr, &obj, 4) || !obj || !CopyGuarded(obj, &vt, 4)) return 0;
    auto it = g_singletonVt.find(staticPtr);
    if (it != g_singletonVt.end()) return it->second && it->second == vt ? obj : 0;
    // First time: name the class through getDTI (slot 4: mov eax, dti; ret; DTI+4 = name).
    uint32_t getDti = 0, dti = 0, namePtr = 0;
    uint8_t code[6];
    char name[40] = {};
    if (!CopyGuarded(vt + 16, &getDti, 4) || !CopyGuarded(getDti, code, 6) || code[0] != 0xB8 || code[5] != 0xC3)
        return 0;
    memcpy(&dti, code + 1, 4);
    if (!CopyGuarded(dti + 4, &namePtr, 4) || !CopyGuarded(namePtr, name, sizeof(name) - 1) || strcmp(name, cls)) {
        Log("draw distance: %08x is not %s (%s); skipped", (unsigned)staticPtr, cls, name);
        g_singletonVt[staticPtr] = 0;
        return 0;
    }
    g_singletonVt[staticPtr] = vt;
    return obj;
}

void ApplyDrawDistance() {
    const float k = g_drawScale;
    int written = 0;
    for (ScaledField& f : g_drawFields) {
        const uintptr_t obj = VerifiedSingleton(f.staticPtr, f.cls);
        if (!obj) continue;
        float v = 0;
        if (!CopyGuarded(obj + f.off, &v, 4) || !std::isfinite(v) || v <= 0.0f || v > 1e7f) continue;
        if (f.applied == 0.0f || v != f.applied) f.orig = v;  // the game's value
        if (k == 1.0f && f.applied == 0.0f) continue;          // Game and never changed: leave it
        const float want = k == 1.0f ? f.orig : f.orig * k;
        if (v != want) {
            WriteFloat(obj + f.off, want);
            if (f.squaredNext) WriteFloat(obj + f.off + 4, want * want);
            ++written;
        }
        f.applied = k == 1.0f ? 0.0f : want;
    }
    if (written && !g_drawLogged) {
        g_drawLogged = true;
        std::string s;
        for (const ScaledField& f : g_drawFields) {
            char b[48];
            snprintf(b, sizeof(b), " %s+%x=%.0f", f.cls, (unsigned)f.off, f.orig);
            s += b;
        }
        Log("draw distance x%.1f; game values:%s", k, s.c_str());
    }
}

// ---------------------------------------------------------------- texture options
//
// The game sets D3D9 sampler states per draw (IDirect3DDevice9::SetSamplerState, vtable slot
// 69). RenoDX turns them into D3D11 samplers inside its device proxy, without ReShade's
// create_sampler event, so the filter is changed where the game sets it: for a sampler that
// wraps or mirrors (surface textures) with linear minification and mips, the filter becomes
// 16x anisotropic and the chosen mip bias is added. Clamped samplers (full-screen passes,
// shadow maps, which need linear filtering for hardware shadow comparison) are left as the
// game set them. The game sets its sampler states every draw, so changes apply at once.

const Choice kAnisoChoices[] = {{"Game", 0.0f}, {"16x anisotropic", 16.0f}};
const Choice kSharpChoices[] = {{"Game", 0.0f}, {"Sharper (-0.5)", -0.5f}, {"Sharpest (-1)", -1.0f}};
int g_anisoChoice = 0, g_sharpChoice = 0;
volatile LONG g_samplersSeen = 0, g_samplersChanged = 0;

using SetSamplerStateFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD, D3DSAMPLERSTATETYPE, DWORD);
SetSamplerStateFn g_setSamplerState = nullptr;

struct SamplerRequest {  // what the game asked for (D3D9 defaults)
    DWORD minf = D3DTEXF_POINT, magf = D3DTEXF_POINT, mipf = D3DTEXF_NONE, addrU = D3DTADDRESS_WRAP;
    DWORD aniso = 1, bias = 0;
    bool upgraded = false;
};
SamplerRequest g_samplers[16];

enum : unsigned { kPushMin = 1, kPushMag = 2, kPushAniso = 4, kPushBias = 8, kPushAll = 15 };

void PushSampler(IDirect3DDevice9* dev, DWORD s, unsigned what) {
    SamplerRequest& r = g_samplers[s];
    const float aniso = kAnisoChoices[g_anisoChoice].value, extra = kSharpChoices[g_sharpChoice].value;
    const bool surface = (r.addrU == D3DTADDRESS_WRAP || r.addrU == D3DTADDRESS_MIRROR) &&
                         (r.minf == D3DTEXF_LINEAR || r.minf == D3DTEXF_ANISOTROPIC) && r.mipf != D3DTEXF_NONE;
    const bool up = surface && (aniso > 0.0f || extra != 0.0f);
    if (up != r.upgraded) {
        what = kPushAll;  // the sampler changed between upgraded and the game's own state
        if (up) InterlockedIncrement(&g_samplersChanged);
    }
    r.upgraded = up;
    const bool af = up && aniso > 0.0f;
    if (what & kPushMin) g_setSamplerState(dev, s, D3DSAMP_MINFILTER, af ? D3DTEXF_ANISOTROPIC : r.minf);
    if (what & kPushMag)
        g_setSamplerState(dev, s, D3DSAMP_MAGFILTER, af && r.magf == D3DTEXF_LINEAR ? D3DTEXF_ANISOTROPIC : r.magf);
    if (what & kPushAniso) g_setSamplerState(dev, s, D3DSAMP_MAXANISOTROPY, af ? static_cast<DWORD>(aniso) : r.aniso);
    if (what & kPushBias) {
        float bias;
        memcpy(&bias, &r.bias, 4);
        if (up) bias += extra;
        DWORD bits;
        memcpy(&bits, &bias, 4);
        g_setSamplerState(dev, s, D3DSAMP_MIPMAPLODBIAS, bits);
    }
}

HRESULT STDMETHODCALLTYPE HookSetSamplerState(IDirect3DDevice9* dev, DWORD s, D3DSAMPLERSTATETYPE type, DWORD v) {
    InterlockedIncrement(&g_samplersSeen);
    if (s >= 16) return g_setSamplerState(dev, s, type, v);  // displacement / vertex samplers
    SamplerRequest& r = g_samplers[s];
    switch (type) {
    case D3DSAMP_MINFILTER: r.minf = v; break;
    case D3DSAMP_MAGFILTER: r.magf = v; break;
    case D3DSAMP_MIPFILTER: r.mipf = v; break;
    case D3DSAMP_ADDRESSU: r.addrU = v; break;
    case D3DSAMP_MAXANISOTROPY: r.aniso = v; break;
    case D3DSAMP_MIPMAPLODBIAS: r.bias = v; break;
    default: return g_setSamplerState(dev, s, type, v);
    }
    const bool wanted = kAnisoChoices[g_anisoChoice].value > 0.0f || kSharpChoices[g_sharpChoice].value != 0.0f;
    if (!wanted && !r.upgraded) return g_setSamplerState(dev, s, type, v);  // all Game: pass through
    HRESULT hr = D3D_OK;
    unsigned what = 0;
    switch (type) {
    case D3DSAMP_MINFILTER: what = kPushMin; break;
    case D3DSAMP_MAGFILTER: what = kPushMag; break;
    case D3DSAMP_MAXANISOTROPY: what = kPushAniso; break;
    case D3DSAMP_MIPMAPLODBIAS: what = kPushBias; break;
    default:  // MIPFILTER / ADDRESSU: set it, then re-evaluate (PushSampler pushes all if it flips)
        hr = g_setSamplerState(dev, s, type, v);
        break;
    }
    PushSampler(dev, s, what);
    return hr;
}

std::string g_textureStatus = "texture options: not installed";

void HookDeviceSamplers(IDirect3DDevice9* dev) {
    if (!dev) return;
    void** vt = *reinterpret_cast<void***>(dev);
    if (vt[69] == reinterpret_cast<void*>(&HookSetSamplerState)) return;
    DWORD old;
    if (!VirtualProtect(vt + 69, 4, PAGE_EXECUTE_READWRITE, &old)) return;
    g_setSamplerState = reinterpret_cast<SetSamplerStateFn>(vt[69]);
    vt[69] = reinterpret_cast<void*>(&HookSetSamplerState);
    VirtualProtect(vt + 69, 4, old, &old);
    g_textureStatus = "texture options: active";
    Log("texture options: hooked SetSamplerState on device %p (vtable %p)", dev, vt);
}

void ApplyCameraChoices() {
    g_fovScale = kFovChoices[g_fovChoice].value;
    g_distScale = kDistChoices[g_distChoice].value;
    g_shadowResScale = kShadowResChoices[g_shadowResChoice].value;
    g_shadowDistScale = kShadowDistChoices[g_shadowDistChoice].value;
    g_shadowFilter = static_cast<int>(kShadowFilterChoices[g_shadowFilterChoice].value);
    g_drawScale = kDrawChoices[g_drawChoice].value;
}

void LoadCameraSettings() {
    int v = 0;
    if (reshade::get_config_value(nullptr, DT_PRODUCT, "ShadowResolution", v) && v >= 0 &&
        v < (int)std::size(kShadowResChoices))
        g_shadowResChoice = v;
    if (reshade::get_config_value(nullptr, DT_PRODUCT, "ShadowDistance", v) && v >= 0 &&
        v < (int)std::size(kShadowDistChoices))
        g_shadowDistChoice = v;
    if (reshade::get_config_value(nullptr, DT_PRODUCT, "AmbientOcclusion", v) && v >= 0 &&
        v < (int)std::size(kAoChoices))
        g_aoChoice = v;
    if (reshade::get_config_value(nullptr, DT_PRODUCT, "GodRays", v) && v >= 0 && v < (int)std::size(kGodRayChoices))
        g_godRayChoice = v;
    Log("settings: ambient occlusion %s, god rays %s", kAoChoices[g_aoChoice].label,
        kGodRayChoices[g_godRayChoice].label);
    if (reshade::get_config_value(nullptr, DT_PRODUCT, "DrawDistance", v) && v >= 0 &&
        v < (int)std::size(kDrawChoices))
        g_drawChoice = v;
    Log("settings: draw distance %s", kDrawChoices[g_drawChoice].label);
    if (reshade::get_config_value(nullptr, DT_PRODUCT, "ShadowFilter", v) && v >= 0 &&
        v < (int)std::size(kShadowFilterChoices))
        g_shadowFilterChoice = v;
    if (reshade::get_config_value(nullptr, DT_PRODUCT, "TextureFiltering", v) && v >= 0 &&
        v < (int)std::size(kAnisoChoices))
        g_anisoChoice = v;
    if (reshade::get_config_value(nullptr, DT_PRODUCT, "TextureSharpness", v) && v >= 0 &&
        v < (int)std::size(kSharpChoices))
        g_sharpChoice = v;
    Log("settings: shadow resolution %s, shadow distance %s, shadow filter %s, texture filtering %s, sharpness %s",
        kShadowResChoices[g_shadowResChoice].label, kShadowDistChoices[g_shadowDistChoice].label,
        kShadowFilterChoices[g_shadowFilterChoice].label, kAnisoChoices[g_anisoChoice].label,
        kSharpChoices[g_sharpChoice].label);
    if (reshade::get_config_value(nullptr, DT_PRODUCT, "FieldOfView", v) && v >= 0 && v < (int)std::size(kFovChoices))
        g_fovChoice = v;
    if (reshade::get_config_value(nullptr, DT_PRODUCT, "CameraDistance", v) && v >= 0 && v < (int)std::size(kDistChoices))
        g_distChoice = v;
    ApplyCameraChoices();
    Log("settings: field of view %s, camera distance %s", kFovChoices[g_fovChoice].label,
        kDistChoices[g_distChoice].label);
}

void SaveCameraSettings() {
    reshade::set_config_value(nullptr, DT_PRODUCT, "FieldOfView", g_fovChoice);
    reshade::set_config_value(nullptr, DT_PRODUCT, "CameraDistance", g_distChoice);
    reshade::set_config_value(nullptr, DT_PRODUCT, "ShadowResolution", g_shadowResChoice);
    reshade::set_config_value(nullptr, DT_PRODUCT, "ShadowDistance", g_shadowDistChoice);
    reshade::set_config_value(nullptr, DT_PRODUCT, "ShadowFilter", g_shadowFilterChoice);
    reshade::set_config_value(nullptr, DT_PRODUCT, "DrawDistance", g_drawChoice);
    reshade::set_config_value(nullptr, DT_PRODUCT, "AmbientOcclusion", g_aoChoice);
    reshade::set_config_value(nullptr, DT_PRODUCT, "GodRays", g_godRayChoice);
    reshade::set_config_value(nullptr, DT_PRODUCT, "TextureFiltering", g_anisoChoice);
    reshade::set_config_value(nullptr, DT_PRODUCT, "TextureSharpness", g_sharpChoice);
    ApplyCameraChoices();
}

// ---------------------------------------------------------------- device / frame

LARGE_INTEGER g_freq{}, g_last{}, g_measureEnd{}, g_firstPresent{};
bool g_measuring = false, g_autoScanned = false;
std::vector<double> g_frameMs;
std::string g_measureResult, g_deviceInfo;
bool g_f8 = false, g_f9 = false, g_f10 = false;

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
    {  // sampler counts in the log, at most every 10 s when they change
        static LONG lastSeen = -1;
        static DWORD lastLog = 0;
        if (g_samplersChanged != lastSeen && GetTickCount() - lastLog > 10000) {
            lastSeen = g_samplersChanged;
            lastLog = GetTickCount();
            Log("samplers: %ld sampler states set, %ld surface samplers upgraded", g_samplersSeen, g_samplersChanged);
        }
    }
    if (KeyPressed(VK_F9, g_f9)) Snapshot();
    if (KeyPressed(VK_F8, g_f8)) StartTrace();
    if (g_tracing && now.QuadPart >= g_traceEnd.QuadPart) StopTrace();
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
    ImGui::TextDisabled("%s", g_modeStatus.c_str());
    ImGui::TextDisabled("%s", g_renoStatus.c_str());
    ImGui::Separator();

    auto combo = [](const char* label, int& cur, const Choice* choices, int n, const char* help) {
        if (ImGui::BeginCombo(label, choices[cur].label)) {
            for (int i = 0; i < n; ++i)
                if (ImGui::Selectable(choices[i].label, i == cur) && i != cur) {
                    cur = i;
                    SaveCameraSettings();
                }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
            ImGui::TextUnformatted(help);
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
    };
    combo("Field of view", g_fovChoice, kFovChoices, (int)std::size(kFovChoices),
          "Wider view in gameplay (walking and aiming; aiming keeps its zoom). Cutscenes are unchanged. "
          "Applies at once.");
    combo("Camera distance", g_distChoice, kDistChoices, (int)std::size(kDistChoices),
          "Moves the gameplay camera further back along its view line, so the crosshair stays on target. "
          "The game's wall collision uses the normal distance, so far settings can clip into walls in "
          "tight spaces. Cutscenes are unchanged. Applies at once.");
    ImGui::TextDisabled("%s (frames changed: %ld of %ld)", g_cameraStatus.c_str(), g_viewportChanged,
                        g_viewportCalls);
    ImGui::Separator();
    combo("Shadow resolution", g_shadowResChoice, kShadowResChoices, (int)std::size(kShadowResChoices),
          "Sharper shadows: multiplies the size of every shadow map the game uses (up to 8192), for the "
          "sun, lamps and the flashlight. Applies at once, in gameplay.");
    combo("Shadow distance", g_shadowDistChoice, kShadowDistChoices, (int)std::size(kShadowDistChoices),
          "Shadows drawn further from the camera. With cascaded sun shadows this spreads the map over a "
          "larger area, so pair it with a higher shadow resolution. Applies at once, in gameplay.");
    combo("Shadow filtering", g_shadowFilterChoice, kShadowFilterChoices, (int)std::size(kShadowFilterChoices),
          "The shadow edge filter. The game uses filter 1; filters 2 and 3 are other filters its shaders "
          "support but the game doesn't use. Applies at once, in gameplay.");
    ImGui::TextDisabled("%s; %s", g_shadowHookStatus.c_str(), g_shadowStatus.c_str());
    ImGui::Separator();
    combo("Draw distance", g_drawChoice, kDrawChoices, (int)std::size(kDrawChoices),
          "Effects (particles, smoke, sparks) and grass are drawn further from the camera. RE6 draws "
          "level geometry and characters in full already; these are its distance limits. Applies at "
          "once, in gameplay.");
    ImGui::Separator();
    combo("Ambient occlusion", g_aoChoice, kAoChoices, (int)std::size(kAoChoices),
          "The game's ambient occlusion (soft shadowing in corners and creases) at full screen resolution "
          "instead of reduced; 'smoother' also doubles its blur samples. Only where a level uses the game's "
          "AO filter (the line below counts them; 0 = this area has none). Applies at once, in gameplay.");
    combo("God rays", g_godRayChoice, kGodRayChoices, (int)std::size(kGodRayChoices),
          "Twice the iterations for light shafts: less banding and stepping. Only where a level has god rays "
          "(counted below). Applies at once, in gameplay.");
    ImGui::TextDisabled("%s", g_filterStatus.c_str());
    ImGui::Separator();
    combo("Texture filtering", g_anisoChoice, kAnisoChoices, (int)std::size(kAnisoChoices),
          "16x anisotropic filtering for surface textures: floors and walls stay sharp at an angle. "
          "Applies at once.");
    combo("Texture sharpness", g_sharpChoice, kSharpChoices, (int)std::size(kSharpChoices),
          "Uses more detailed texture mips. Sharper surfaces at a distance; can add shimmer. "
          "Applies at once.");
    ImGui::TextDisabled("%s (sampler states set: %ld, surface samplers upgraded: %ld)", g_textureStatus.c_str(),
                        g_samplersSeen, g_samplersChanged);
    ImGui::Separator();
    if (!ImGui::CollapsingHeader("Probe tools")) return;
    if (ImGui::Button("Take snapshot (Ctrl+F9)")) Snapshot();
    ImGui::SameLine();
    ImGui::Text("%d taken", g_snapshot);
    ImGui::BeginDisabled(g_tracing);
    if (ImGui::Button("Trace camera writers, 2 s (Ctrl+F8)")) StartTrace();
    ImGui::EndDisabled();
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
    InstallModeFix();
    InstallRenoDxShim();
    InstallCameraHook();
    LoadCameraSettings();
    InstallShadowHooks();
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
