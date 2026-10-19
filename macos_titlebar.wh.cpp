// ==WindhawkMod==
// @id              macbuttons
// @name            MacButtons
// @description     Replaces classic Win32 caption buttons with macOS-style circles
// @version         0.3.2
// @author          Canoka
// @license         MIT
// @architecture    x86-64
// @include         *
// @compilerOptions -lcomctl32 -lgdi32 -luser32 -ldwmapi
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# MacButtons

Adds macOS-style title bar buttons to compatible classic Win32 window title bars.

Applications that use custom title bars, such as Spotify, are excluded to prevent duplicate buttons. Other custom title bars may not be compatible with this mod.

**Technical Improvements:** When a window opens, the mod briefly retries if the title bar geometry is not yet ready. Whenever possible, the existing button overlay is reused to avoid creating unnecessary overlays.

**Disclaimer:** This mod is still experimental. The user assumes all responsibility for any errors, compatibility issues, data loss, or other adverse consequences that may occur. Use this mod entirely at your own risk.
*/
// ==/WindhawkModReadme==

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <math.h>
#include <windhawk_utils.h>

static const wchar_t kOverlayClass[] = L"MacButtonsOverlay.Windhawk";
static const wchar_t kPropOverlay[]  = L"MacButtons.Overlay";
static const wchar_t kPropAttached[] = L"MacButtons.Attached";
static const wchar_t kPropRejected[] = L"MacButtons.Rejected";
static const wchar_t kPropStripped[] = L"MacButtons.Stripped";
static const wchar_t kPropBusy[]     = L"MacButtons.Busy";
static const wchar_t kPropRetryCount[] = L"MacButtons.RetryCount";

static const UINT_PTR kRetryTimerId = 0x4D42;
static const UINT kRetryIntervalMs = 35;
static const int kMaxRetryCount = 15;

static UINT g_msgInit = 0;
static UINT g_msgCleanup = 0;
static volatile LONG g_unloading = 0;
static volatile LONG g_logBudget = 60;

#define LOG_LIMITED(...) \
    do { if (InterlockedDecrement(&g_logBudget) >= 0) Wh_Log(__VA_ARGS__); } while (0)

// Overlay positions: left to right = minimize, maximize, close.
// Kinds: 0 close, 1 minimize, 2 maximize.
static const int kKind[3] = {1, 2, 0};

struct Overlay {
    HWND target = nullptr;
    int dpi = 96;
    int w = 0, h = 0;
    int d = 16, pitch = 24, btnLeft = 8, btnTop = 0;
    int hover = -1;
    int pressed = -1;
    bool tracking = false;
    bool active = true;
    bool nccreated = false;
};

struct Geometry {
    int x, y, w, h;
    int dpi, d, pitch, btnLeft, btnTop;
};

static int S(int v, int dpi) { return MulDiv(v, dpi, 96); }

static bool ButtonEnabled(HWND t, int kind) {
    LONG s = GetWindowLongW(t, GWL_STYLE);
    if (kind == 1) return (s & WS_MINIMIZEBOX) != 0;
    if (kind == 2) return (s & WS_MAXIMIZEBOX) != 0;

    HMENU sm = GetSystemMenu(t, FALSE);
    if (sm) {
        UINT st = GetMenuState(sm, SC_CLOSE, MF_BYCOMMAND);
        if (st != (UINT)-1 && (st & (MF_GRAYED | MF_DISABLED))) return false;
    }
    return true;
}

static void StripSysMenu(HWND t) {
    LONG style = GetWindowLongW(t, GWL_STYLE);
    if (!(style & WS_SYSMENU)) return;

    SetPropW(t, kPropBusy, (HANDLE)1);
    SetPropW(t, kPropStripped, (HANDLE)1);
    SetWindowLongW(t, GWL_STYLE, style & ~WS_SYSMENU);
    SetWindowPos(t, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
                 SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    RemovePropW(t, kPropBusy);
    LOG_LIMITED(L"MacButtons: removed system buttons of window %p", t);
}

static void RestoreSysMenu(HWND t) {
    if (!GetPropW(t, kPropStripped)) return;

    SetPropW(t, kPropBusy, (HANDLE)1);
    RemovePropW(t, kPropStripped);
    LONG style = GetWindowLongW(t, GWL_STYLE);
    SetWindowLongW(t, GWL_STYLE, style | WS_SYSMENU);
    SetWindowPos(t, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
                 SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    RemovePropW(t, kPropBusy);
}

static bool ComputeGeometry(HWND t, Geometry* g, int* why) {
    *why = 0;
    if (!IsWindowVisible(t) || IsIconic(t)) { *why = 1; return false; }

    LONG style = GetWindowLongW(t, GWL_STYLE);
    LONG ex = GetWindowLongW(t, GWL_EXSTYLE);
    bool stripped = GetPropW(t, kPropStripped) != nullptr;

    if ((style & WS_CAPTION) != WS_CAPTION ||
        (!(style & WS_SYSMENU) && !stripped)) {
        *why = 2;
        return false;
    }
    if (ex & (WS_EX_TOOLWINDOW | WS_EX_LAYOUTRTL)) { *why = 3; return false; }

    TITLEBARINFO tbi = {};
    tbi.cbSize = sizeof(tbi);
    if (!GetTitleBarInfo(t, &tbi) ||
        (tbi.rgstate[0] & (STATE_SYSTEM_INVISIBLE | STATE_SYSTEM_OFFSCREEN))) {
        *why = 4;
        return false;
    }

    RECT wr;
    if (!GetWindowRect(t, &wr)) { *why = 4; return false; }

    int dpi = (int)GetDpiForWindow(t);
    if (dpi <= 0) dpi = 96;
    bool zoomed = IsZoomed(t) != 0;

    int visTop, visRight;
    RECT vis = {};
    if (SUCCEEDED(DwmGetWindowAttribute(
            t, DWMWA_EXTENDED_FRAME_BOUNDS, &vis, sizeof(vis)))) {
        visTop = vis.top;
        visRight = vis.right;
    } else {
        int inset = 0;
        if (style & WS_THICKFRAME) {
            inset = GetSystemMetricsForDpi(SM_CXSIZEFRAME, dpi) +
                    GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
        }
        visTop = zoomed ? wr.top + inset : wr.top;
        visRight = wr.right - inset;
    }

    int barBottom = tbi.rcTitleBar.bottom;
    int barH = barBottom - visTop;
    if (barH < S(16, dpi)) { *why = 6; return false; }

    POINT c = {0, 0};
    ClientToScreen(t, &c);
    // Do not use the client-origin test here: some genuine classic Win32
    // title bars (including Notepad variants) report a client origin that
    // overlaps the title-bar rectangle. Custom-titlebar apps are filtered
    // separately above where their process identity is known.

    g->dpi = dpi;
    g->d = S(16, dpi);
    g->pitch = S(24, dpi);
    g->btnLeft = S(8, dpi);
    g->w = g->btnLeft + g->d + 2 * g->pitch + S(8, dpi);
    g->h = barH;
    g->x = visRight - g->w - S(6, dpi);
    g->y = visTop;
    g->btnTop = (g->h - g->d) / 2;
    return true;
}

static float Clamp01(float v) {
    return v < 0.f ? 0.f : (v > 1.f ? 1.f : v);
}

static float SegDist(float px, float py, float ax, float ay, float bx, float by) {
    float vx = bx - ax, vy = by - ay, wx = px - ax, wy = py - ay;
    float len2 = vx * vx + vy * vy;
    float t = len2 > 0.f ? (wx * vx + wy * vy) / len2 : 0.f;
    t = Clamp01(t);
    float dx = px - (ax + t * vx), dy = py - (ay + t * vy);
    return sqrtf(dx * dx + dy * dy);
}

static void RenderOverlay(HWND ov, Overlay* o, const POINT* dst) {
    int w = o->w, h = o->h;
    if (w <= 0 || h <= 0) return;

    HDC screen = GetDC(nullptr);
    if (!screen) return;
    HDC mem = CreateCompatibleDC(screen);

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);

    if (mem && bmp && bits) {
        static const float kFill[3][3] = {
            {255, 95, 87}, {254, 188, 46}, {40, 200, 64}
        };
        static const float kBorder[3][3] = {
            {226, 64, 56}, {222, 161, 35}, {26, 171, 41}
        };
        static const float kGlyph[3][3] = {
            {76, 0, 2}, {100, 62, 0}, {0, 80, 6}
        };
        static const float kGrayFill[3] = {170, 170, 170};
        static const float kGrayBorder[3] = {145, 145, 145};

        bool en[3];
        for (int k = 0; k < 3; k++) en[k] = ButtonEnabled(o->target, k);
        bool anyHover = o->hover >= 0;

        float r = o->d / 2.0f;
        float bw = fmaxf(1.0f, o->dpi / 96.0f);
        float sc = o->d / 16.0f;
        float cy = o->btnTop + r;

        BYTE* px = (BYTE*)bits;
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                float fx = x + 0.5f, fy = y + 0.5f;
                float cr = 0, cg = 0, cb = 0, ca = 0;

                for (int i = 0; i < 3; i++) {
                    int kd = kKind[i];
                    float cx = o->btnLeft + i * o->pitch + r;
                    float dx = fx - cx, dy = fy - cy;
                    float dist = sqrtf(dx * dx + dy * dy);
                    float outer = Clamp01(r + 0.5f - dist);
                    if (outer <= 0.f) continue;

                    bool colored = en[kd] && (o->active || anyHover);
                    const float* f = colored ? kFill[kd] : kGrayFill;
                    const float* b = colored ? kBorder[kd] : kGrayBorder;
                    float k = (o->pressed == i && en[kd]) ? 0.75f : 1.0f;
                    float inner = Clamp01(r - bw + 0.5f - dist);

                    float col[3];
                    for (int ch = 0; ch < 3; ch++)
                        col[ch] = (b[ch] * (1.f - inner) + f[ch] * inner) * k;

                    cr = col[0] * outer + cr * (1.f - outer);
                    cg = col[1] * outer + cg * (1.f - outer);
                    cb = col[2] * outer + cb * (1.f - outer);
                    ca = outer + ca * (1.f - outer);

                    if (anyHover && en[kd]) {
                        float gd;
                        if (kd == 0) {
                            gd = fminf(
                                SegDist(dx, dy, -2.3f*sc, -2.3f*sc, 2.3f*sc, 2.3f*sc),
                                SegDist(dx, dy, -2.3f*sc, 2.3f*sc, 2.3f*sc, -2.3f*sc));
                        } else if (kd == 1) {
                            gd = SegDist(dx, dy, -3.f*sc, 0.f, 3.f*sc, 0.f);
                        } else {
                            gd = fminf(
                                SegDist(dx, dy, -3.f*sc, 0.f, 3.f*sc, 0.f),
                                SegDist(dx, dy, 0.f, -3.f*sc, 0.f, 3.f*sc));
                        }
                        float gc = Clamp01(0.65f*sc + 0.5f - gd) * 0.85f * inner;
                        cr = kGlyph[kd][0] * gc + cr * (1.f - gc);
                        cg = kGlyph[kd][1] * gc + cg * (1.f - gc);
                        cb = kGlyph[kd][2] * gc + cb * (1.f - gc);
                        ca = gc + ca * (1.f - gc);
                    }
                }

                BYTE* p = px + ((size_t)y * w + x) * 4;
                p[0] = (BYTE)fminf(cb + 0.5f, 255.f);
                p[1] = (BYTE)fminf(cg + 0.5f, 255.f);
                p[2] = (BYTE)fminf(cr + 0.5f, 255.f);
                p[3] = (BYTE)fminf(ca * 255.f + 0.5f, 255.f);
            }
        }

        HGDIOBJ old = SelectObject(mem, bmp);
        POINT src = {0, 0};
        SIZE sz = {w, h};
        BLENDFUNCTION bf = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        POINT dstCopy = {0, 0};
        if (dst) dstCopy = *dst;
        UpdateLayeredWindow(ov, screen, dst ? &dstCopy : nullptr,
                            &sz, mem, &src, 0, &bf, ULW_ALPHA);
        SelectObject(mem, old);
    }

    if (bmp) DeleteObject(bmp);
    if (mem) DeleteDC(mem);
    ReleaseDC(nullptr, screen);
}

static int HitTestButton(const Overlay* o, int x, int y) {
    int r = o->d / 2 + S(3, o->dpi);
    int cy = o->btnTop + o->d / 2;
    for (int i = 0; i < 3; i++) {
        int cx = o->btnLeft + i * o->pitch + o->d / 2;
        int dx = x - cx, dy = y - cy;
        if (dx * dx + dy * dy <= r * r) return i;
    }
    return -1;
}

static void DoAction(HWND t, int pos) {
    int kd = kKind[pos];
    if (!IsWindow(t) || !IsWindowEnabled(t) || !ButtonEnabled(t, kd)) return;

    if (kd == 0) {
        PostMessageW(t, WM_CLOSE, 0, 0);
    } else if (kd == 1) {
        ShowWindow(t, SW_MINIMIZE);
    } else {
        ShowWindow(t, IsZoomed(t) ? SW_RESTORE : SW_MAXIMIZE);
    }
}

static LRESULT CALLBACK OverlayProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    Overlay* o = (Overlay*)GetWindowLongPtrW(h, GWLP_USERDATA);

    switch (m) {
    case WM_NCCREATE: {
        CREATESTRUCTW* cs = (CREATESTRUCTW*)lp;
        Overlay* created = (Overlay*)cs->lpCreateParams;
        if (created) created->nccreated = true;
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)created);
        return TRUE;
    }
    case WM_NCDESTROY:
        SetWindowLongPtrW(h, GWLP_USERDATA, 0);
        delete o;
        break;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_NCHITTEST: {
        if (!o) return HTTRANSPARENT;
        POINT p = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(h, &p);
        return HitTestButton(o, p.x, p.y) >= 0 ? HTCLIENT : HTTRANSPARENT;
    }
    case WM_MOUSEMOVE: {
        if (!o) break;
        if (!o->tracking) {
            TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, h, 0};
            if (TrackMouseEvent(&tme)) o->tracking = true;
        }
        int hit = HitTestButton(o, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        if (hit != o->hover) {
            o->hover = hit;
            RenderOverlay(h, o, nullptr);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        if (o) {
            o->tracking = false;
            if (o->pressed < 0 && o->hover != -1) {
                o->hover = -1;
                RenderOverlay(h, o, nullptr);
            }
        }
        return 0;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK: {
        if (!o) break;
        int hit = HitTestButton(o, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        if (hit >= 0) {
            o->pressed = hit;
            o->hover = hit;
            SetCapture(h);
            RenderOverlay(h, o, nullptr);
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        if (!o) break;
        int p = o->pressed;
        o->pressed = -1;
        if (GetCapture() == h) ReleaseCapture();
        int hit = HitTestButton(o, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        o->hover = hit;
        RenderOverlay(h, o, nullptr);
        if (p >= 0 && hit == p) DoAction(o->target, p);
        return 0;
    }
    case WM_CAPTURECHANGED:
        if (o && o->pressed >= 0 && (HWND)lp != h) {
            o->pressed = -1;
            RenderOverlay(h, o, nullptr);
        }
        return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

static HWND CreateOverlay(HWND t) {
    Overlay* o = new Overlay();
    o->target = t;
    o->active = (GetForegroundWindow() == t);

    HWND ov = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        kOverlayClass, L"", WS_POPUP, 0, 0, 1, 1, t, nullptr,
        GetModuleHandleW(nullptr), o);

    if (!ov) {
        LOG_LIMITED(L"MacButtons: CreateWindowExW(overlay) failed, error %lu",
                    GetLastError());
        if (!o->nccreated) delete o;
        return nullptr;
    }

    SetPropW(t, kPropOverlay, (HANDLE)ov);
    LOG_LIMITED(L"MacButtons: overlay %p created for window %p", ov, t);
    return ov;
}

static void DestroyOverlay(HWND t) {
    HWND ov = (HWND)GetPropW(t, kPropOverlay);
    if (!ov) return;
    RemovePropW(t, kPropOverlay);
    if (IsWindow(ov)) DestroyWindow(ov);
}

static void UpdateOverlay(HWND t, int active) {
    if (g_unloading || !IsWindow(t)) return;
    if (GetPropW(t, kPropBusy)) return;

    HWND ov = (HWND)GetPropW(t, kPropOverlay);
    if (ov && !IsWindow(ov)) {
        RemovePropW(t, kPropOverlay);
        ov = nullptr;
    }

    Geometry g;
    int why = 0;
    if (!ComputeGeometry(t, &g, &why)) {
        if (ov) ShowWindow(ov, SW_HIDE);
        return;
    }

    if (GetWindowLongW(t, GWL_STYLE) & WS_SYSMENU) {
        StripSysMenu(t);
        if (!ComputeGeometry(t, &g, &why)) {
            if (ov) ShowWindow(ov, SW_HIDE);
            return;
        }
    }

    if (!ov) {
        ov = CreateOverlay(t);
        if (!ov) return;
    }

    Overlay* o = (Overlay*)GetWindowLongPtrW(ov, GWLP_USERDATA);
    if (!o) return;

    o->dpi = g.dpi;
    o->d = g.d;
    o->pitch = g.pitch;
    o->btnLeft = g.btnLeft;
    o->btnTop = g.btnTop;
    o->w = g.w;
    o->h = g.h;
    if (active >= 0) o->active = (active != 0);

    bool wasVisible = IsWindowVisible(ov) != 0;
    POINT dst = {g.x, g.y};
    RenderOverlay(ov, o, &dst);

    if (!wasVisible) {
        SetWindowPos(ov, t, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
}

static void StopOverlayRetry(HWND t) {
    KillTimer(t, kRetryTimerId);
    RemovePropW(t, kPropRetryCount);
}

static void ScheduleOverlayRetry(HWND t) {
    if (!IsWindow(t) || g_unloading) return;
    SetPropW(t, kPropRetryCount, (HANDLE)(INT_PTR)1);
    SetTimer(t, kRetryTimerId, kRetryIntervalMs, nullptr);
}

LRESULT CALLBACK MacButtonsSubclassProc(HWND hWnd, UINT uMsg, WPARAM wParam,
                                        LPARAM lParam, DWORD_PTR dwRefData) {
    (void)dwRefData;

    if (g_msgInit && uMsg == g_msgInit) {
        UpdateOverlay(hWnd, -1);
        return 0;
    }

    if (g_msgCleanup && uMsg == g_msgCleanup) {
        StopOverlayRetry(hWnd);
        DestroyOverlay(hWnd);
        RestoreSysMenu(hWnd);
        return 0;
    }

    if (uMsg == WM_TIMER && wParam == kRetryTimerId) {
        if (g_unloading || !IsWindowVisible(hWnd) || IsIconic(hWnd)) {
            StopOverlayRetry(hWnd);
            return 0;
        }

        UpdateOverlay(hWnd, -1);

        HWND ov = (HWND)GetPropW(hWnd, kPropOverlay);
        if (ov && IsWindow(ov) && IsWindowVisible(ov)) {
            StopOverlayRetry(hWnd);
            return 0;
        }

        int retries = (int)(INT_PTR)GetPropW(hWnd, kPropRetryCount);
        retries++;
        if (retries >= kMaxRetryCount) {
            StopOverlayRetry(hWnd);
        } else {
            SetPropW(hWnd, kPropRetryCount, (HANDLE)(INT_PTR)retries);
        }
        return 0;
    }

    if (uMsg == WM_DESTROY || uMsg == WM_NCDESTROY) {
        StopOverlayRetry(hWnd);
        DestroyOverlay(hWnd);
        if (uMsg == WM_NCDESTROY) {
            RemovePropW(hWnd, kPropAttached);
            RemovePropW(hWnd, kPropStripped);
            RemovePropW(hWnd, kPropBusy);
            RemovePropW(hWnd, kPropRejected);
        }
        return DefSubclassProc(hWnd, uMsg, wParam, lParam);
    }

    if (uMsg == WM_SHOWWINDOW && !wParam) {
        HWND ov = (HWND)GetPropW(hWnd, kPropOverlay);
        if (ov && IsWindow(ov)) ShowWindow(ov, SW_HIDE);
        StopOverlayRetry(hWnd);
    }

    LRESULT result = DefSubclassProc(hWnd, uMsg, wParam, lParam);

    switch (uMsg) {
    case WM_WINDOWPOSCHANGED:
        UpdateOverlay(hWnd, -1);
        break;

    case WM_NCACTIVATE:
        UpdateOverlay(hWnd, wParam ? 1 : 0);
        break;

    case WM_SHOWWINDOW:
        if (wParam) {
            // Try immediately, then retry briefly if title-bar geometry is not ready.
            UpdateOverlay(hWnd, -1);
            HWND ov = (HWND)GetPropW(hWnd, kPropOverlay);
            if (!ov || !IsWindowVisible(ov)) {
                ScheduleOverlayRetry(hWnd);
            } else {
                StopOverlayRetry(hWnd);
            }
        }
        break;

    case WM_DPICHANGED:
    case WM_THEMECHANGED:
    case WM_SETTINGCHANGE:
    case WM_DWMCOMPOSITIONCHANGED:
    case WM_STYLECHANGED:
        UpdateOverlay(hWnd, -1);
        break;
    }

    return result;
}

static bool IsKnownCustomTitleBarProcess() {
    wchar_t path[MAX_PATH] = {};
    DWORD size = ARRAYSIZE(path);
    if (!QueryFullProcessImageNameW(GetCurrentProcess(), 0, path, &size))
        return false;

    const wchar_t* base = wcsrchr(path, L'\\');
    base = base ? base + 1 : path;

    // These apps draw their own title-bar controls. Altering WS_SYSMENU does
    // not remove those controls and can leave duplicate buttons, so leave them
    // untouched rather than pretending this classic-Win32 overlay supports them.
    return _wcsicmp(base, L"Spotify.exe") == 0 ||
           _wcsicmp(base, L"ApplicationFrameHost.exe") == 0;
}

static void TryAttach(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return;
    if (IsKnownCustomTitleBarProcess()) return;
    if (GetPropW(hwnd, kPropAttached) || GetPropW(hwnd, kPropRejected)) return;

    LONG style = GetWindowLongW(hwnd, GWL_STYLE);
    LONG ex = GetWindowLongW(hwnd, GWL_EXSTYLE);

    if ((style & WS_CHILD) || (style & WS_CAPTION) != WS_CAPTION ||
        !(style & WS_SYSMENU) || (ex & WS_EX_TOOLWINDOW)) {
        if (IsWindowVisible(hwnd)) {
            SetPropW(hwnd, kPropRejected, (HANDLE)1);
            LOG_LIMITED(L"MacButtons: not attaching %p style=%08X ex=%08X",
                        hwnd, (unsigned)style, (unsigned)ex);
        }
        return;
    }

    WindhawkUtils::SetWindowSubclassFromAnyThread(hwnd, MacButtonsSubclassProc, 0);
    SetPropW(hwnd, kPropAttached, (HANDLE)1);
    LOG_LIMITED(L"MacButtons: subclassed window %p", hwnd);
    PostMessageW(hwnd, g_msgInit, 0, 0);
}

static BOOL CALLBACK AttachProc(HWND hwnd, LPARAM) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == GetCurrentProcessId()) TryAttach(hwnd);
    return TRUE;
}

static BOOL CALLBACK DetachProc(HWND hwnd, LPARAM) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != GetCurrentProcessId() || !GetPropW(hwnd, kPropAttached))
        return TRUE;

    DWORD_PTR res = 0;
    SendMessageTimeoutW(hwnd, g_msgCleanup, 0, 0,
                        SMTO_ABORTIFHUNG | SMTO_BLOCK, 1000, &res);
    WindhawkUtils::RemoveWindowSubclassFromAnyThread(hwnd, MacButtonsSubclassProc);
    RemovePropW(hwnd, kPropAttached);
    RemovePropW(hwnd, kPropRejected);
    return TRUE;
}

static HANDLE g_scanThread = nullptr;
static HANDLE g_stopEvent = nullptr;

static DWORD WINAPI ScanThreadProc(LPVOID) {
    while (WaitForSingleObject(g_stopEvent, 300) == WAIT_TIMEOUT) {
        EnumWindows(AttachProc, 0);
    }
    return 0;
}

using CreateWindowExW_t = decltype(&CreateWindowExW);
static CreateWindowExW_t CreateWindowExW_Original;

static HWND WINAPI CreateWindowExW_Hook(DWORD exStyle, LPCWSTR className,
                                        LPCWSTR windowName, DWORD style, int x,
                                        int y, int width, int height,
                                        HWND parent, HMENU menu, HINSTANCE instance,
                                        LPVOID param) {
    HWND hwnd = CreateWindowExW_Original(exStyle, className, windowName, style,
                                         x, y, width, height, parent, menu,
                                         instance, param);
    if (hwnd && !g_unloading) TryAttach(hwnd);
    return hwnd;
}

BOOL Wh_ModInit() {
    Wh_Log(L"MacButtons: Wh_ModInit in process %lu", GetCurrentProcessId());

    g_msgInit = RegisterWindowMessageW(L"MacButtons.Init");
    g_msgCleanup = RegisterWindowMessageW(L"MacButtons.Cleanup");

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = OverlayProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kOverlayClass;

    if (!RegisterClassExW(&wc)) {
        Wh_Log(L"MacButtons: RegisterClassExW failed, error %lu", GetLastError());
        return FALSE;
    }

    BOOL hooked = Wh_SetFunctionHook((void*)CreateWindowExW,
                                     (void*)CreateWindowExW_Hook,
                                     (void**)&CreateWindowExW_Original);
    Wh_Log(L"MacButtons: CreateWindowExW hook %s", hooked ? L"installed" : L"FAILED");

    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (g_stopEvent) {
        g_scanThread = CreateThread(nullptr, 0, ScanThreadProc, nullptr, 0, nullptr);
    }

    return TRUE;
}

void Wh_ModAfterInit() {
    Wh_Log(L"MacButtons: scanning existing windows");
    EnumWindows(AttachProc, 0);
}

void Wh_ModUninit() {
    Wh_Log(L"MacButtons: unloading");
    InterlockedExchange(&g_unloading, 1);

    if (g_stopEvent) SetEvent(g_stopEvent);
    if (g_scanThread) {
        WaitForSingleObject(g_scanThread, 2000);
        CloseHandle(g_scanThread);
        g_scanThread = nullptr;
    }
    if (g_stopEvent) {
        CloseHandle(g_stopEvent);
        g_stopEvent = nullptr;
    }

    EnumWindows(DetachProc, 0);
    UnregisterClassW(kOverlayClass, GetModuleHandleW(nullptr));
}
