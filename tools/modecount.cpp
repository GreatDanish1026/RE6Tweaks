// Counts the D3D9 display modes per adapter and format (Resident Evil 6 crashes at startup
// when there are more than its fixed table holds). Build: tools\modecount.ps1
#include <d3d9.h>
#include <cstdio>
#include <map>
#include <set>
#include <utility>

int main() {
    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d) return 1;
    const D3DFORMAT fmts[] = {D3DFMT_X8R8G8B8, D3DFMT_A8R8G8B8, D3DFMT_R5G6B5, D3DFMT_A2R10G10B10};
    for (UINT a = 0; a < d3d->GetAdapterCount(); ++a) {
        D3DADAPTER_IDENTIFIER9 id = {};
        d3d->GetAdapterIdentifier(a, 0, &id);
        printf("adapter %u: %s (%s)\n", a, id.Description, id.DeviceName);
        for (D3DFORMAT f : fmts) {
            const UINT n = d3d->GetAdapterModeCount(a, f);
            std::set<std::pair<UINT, UINT>> res;
            std::set<UINT> hz;
            for (UINT i = 0; i < n; ++i) {
                D3DDISPLAYMODE m;
                if (SUCCEEDED(d3d->EnumAdapterModes(a, f, i, &m))) {
                    res.insert({m.Width, m.Height});
                    hz.insert(m.RefreshRate);
                }
            }
            printf("  format %3d: %u modes, %zu resolutions, %zu refresh rates:", (int)f, n, res.size(), hz.size());
            for (UINT h : hz) printf(" %u", h);
            printf("\n");
        }
    }
    d3d->Release();
    return 0;
}
