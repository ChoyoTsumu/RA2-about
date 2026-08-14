#include <windows.h>
#include <ddraw.h>
#include <cstdio>
#include <windowsx.h>
#include "YRpp/Syringe.h"
#include "MinHook/include/MinHook.h"

typedef HRESULT(WINAPI* BltFunc)(
    LPDIRECTDRAWSURFACE7 self,
    LPRECT destRect,
    LPDIRECTDRAWSURFACE7 srcSurface,
    LPRECT srcRect,
    DWORD flags,
    LPDDBLTFX fx
    );

typedef BOOL(WINAPI* GetCursorPosFunc)(LPPOINT lpPoint);

BltFunc              OriginalBlt = nullptr;
GetCursorPosFunc     OriginalGetCursorPos = nullptr;
WNDPROC              OriginalWndProc = nullptr;

HWND                 g_hWnd = nullptr;
float                g_zoom = 1.0f;
float                g_zoomMin = 1.0f;
float                g_zoomMax = 2.0f;
LONG                 g_centerX = 400;
LONG                 g_centerY = 300;

void UpdateCenter(HWND hWnd);
bool IsMapArea(LPRECT rect);
bool IsPointInMapArea(POINT pt);
void ApplyZoomToRect(LPRECT rect);
void ClipRectToMap(LPRECT rect);
HRESULT WINAPI HookedBlt(
    LPDIRECTDRAWSURFACE7 self,
    LPRECT destRect,
    LPDIRECTDRAWSURFACE7 srcSurface,
    LPRECT srcRect,
    DWORD flags,
    LPDDBLTFX fx
);
BOOL WINAPI HookedGetCursorPos(LPPOINT lpPoint);
LRESULT CALLBACK NewWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
DWORD WINAPI InitHook(LPVOID lpParam);


void UpdateCenter(HWND hWnd)
{
    if (!hWnd) return;

    RECT client;
    GetClientRect(hWnd, &client);

    g_centerX = (client.right + client.left) / 2;
    g_centerY = (client.bottom + client.top) / 2;


}

bool IsMapArea(LPRECT rect)
{
    if (!rect || !g_hWnd) return false;

    LONG centerX = (rect->left + rect->right) / 2;
    LONG centerY = (rect->top + rect->bottom) / 2;

    RECT client;
    GetClientRect(g_hWnd, &client);
    int width = client.right - client.left;
    int height = client.bottom - client.top;

    int mapRight = client.left + (int)(width * 0.88);
    int mapBottom = client.top + (int)(height * 0.98);

    if (centerX >= mapRight)  return false;
    if (centerY >= mapBottom) return false;

    return true;
}

bool IsPointInMapArea(POINT pt)
{
    if (!g_hWnd) return false;

    RECT client;
    GetClientRect(g_hWnd, &client);
    int width = client.right - client.left;
    int height = client.bottom - client.top;

    int mapRight = client.left + (int)(width * 0.88);
    int mapBottom = client.top + (int)(height * 0.98);

    if (pt.x >= mapRight)  return false;
    if (pt.y >= mapBottom) return false;
    return true;
}

void ApplyZoomToRect(LPRECT rect)
{
    if (!rect) return;

    rect->left = g_centerX + (LONG)((rect->left - g_centerX) * g_zoom);
    rect->top = g_centerY + (LONG)((rect->top - g_centerY) * g_zoom);
    rect->right = g_centerX + (LONG)((rect->right - g_centerX) * g_zoom);
    rect->bottom = g_centerY + (LONG)((rect->bottom - g_centerY) * g_zoom);
}

void ClipRectToMap(LPRECT rect)
{
    if (!rect || !g_hWnd) return;

    RECT client;
    GetClientRect(g_hWnd, &client);
    int width = client.right - client.left;
    int height = client.bottom - client.top;

    int mapRight = client.left + (int)(width * 0.88);
    int mapBottom = client.top + (int)(height * 0.99);

    if (rect->right > mapRight)  rect->right = mapRight;
    if (rect->bottom > mapBottom) rect->bottom = mapBottom;

    if (rect->left > mapRight)  rect->left = mapRight;
    if (rect->top > mapBottom) rect->top = mapBottom;
}

bool ClipDestAndAdjustSrc(RECT* destRect, RECT* srcRect, IDirectDrawSurface7* srcSurface)
{
    if (!destRect || !g_hWnd) return false;

    RECT client;
    GetClientRect(g_hWnd, &client);
    int width = client.right - client.left;
    int height = client.bottom - client.top;
    int mapRight = client.left + (int)(width * 0.88);
    int mapBottom = client.top + (int)(height * 0.99);

    RECT originalDest = *destRect;
    RECT newDest = originalDest;

    if (newDest.right > mapRight)  newDest.right = mapRight;
    if (newDest.bottom > mapBottom) newDest.bottom = mapBottom;
    if (newDest.left > mapRight)   newDest.left = mapRight;
    if (newDest.top > mapBottom)  newDest.top = mapBottom;

    if (newDest.right <= newDest.left || newDest.bottom <= newDest.top)
        return false;

    LONG origWidth = originalDest.right - originalDest.left;
    LONG origHeight = originalDest.bottom - originalDest.top;
    LONG newWidth = newDest.right - newDest.left;
    LONG newHeight = newDest.bottom - newDest.top;

    if (origWidth <= 0 || origHeight <= 0)
        return false;

    RECT src;
    if (srcRect)
    {
        src = *srcRect;
    }
    else
    {
        DDSURFACEDESC2 desc;
        ZeroMemory(&desc, sizeof(desc));
        desc.dwSize = sizeof(desc);
        if (FAILED(srcSurface->GetSurfaceDesc(&desc)))
            return false;
        src.left = 0;
        src.top = 0;
        src.right = desc.dwWidth;
        src.bottom = desc.dwHeight;
    }

    LONG srcWidth = src.right - src.left;
    LONG srcHeight = src.bottom - src.top;
    if (srcWidth <= 0 || srcHeight <= 0)
        return false;

    RECT newSrc = src;
    if (origWidth != newWidth)
    {
        LONG cutPixels = origWidth - newWidth;
        LONG srcCut = (LONG)((double)cutPixels / origWidth * srcWidth);
        newSrc.right -= srcCut;
    }
    if (origHeight != newHeight)
    {
        LONG cutPixels = origHeight - newHeight;
        LONG srcCut = (LONG)((double)cutPixels / origHeight * srcHeight);
        newSrc.bottom -= srcCut;
    }

    *destRect = newDest;
    if (srcRect)
        *srcRect = newSrc;

    return true;
}

bool IsPrimaryOrBackBuffer(IDirectDrawSurface7* surface)
{
    if (!surface) return false;

    DDSURFACEDESC2 desc;
    ZeroMemory(&desc, sizeof(desc));
    desc.dwSize = sizeof(desc);

    if (FAILED(surface->GetSurfaceDesc(&desc)))
        return false;

    if (desc.ddsCaps.dwCaps & DDSCAPS_PRIMARYSURFACE) return true;
    if (desc.ddsCaps.dwCaps & DDSCAPS_BACKBUFFER)     return true;

    return false;
}


bool IsRectOverlapUI(const RECT& rect)
{
    if (!g_hWnd) return false;

    RECT client;
    GetClientRect(g_hWnd, &client);
    int width = client.right - client.left;
    int height = client.bottom - client.top;

    int uiRight = client.left + (int)(width * 0.93);
    int uiBottom = client.top + (int)(height * 0.99);

    RECT uiRect;
    uiRect.left = uiRight;
    uiRect.top = 0;
    uiRect.right = client.right;
    uiRect.bottom = uiBottom;

    return (rect.left < uiRect.right && rect.right > uiRect.left &&
        rect.top < uiRect.bottom && rect.bottom > uiRect.top);
}

HRESULT WINAPI HookedBlt(
    LPDIRECTDRAWSURFACE7 self,
    LPRECT destRect,
    LPDIRECTDRAWSURFACE7 srcSurface,
    LPRECT srcRect,
    DWORD flags,
    LPDDBLTFX fx)
{
    if (g_zoom != 1.0f && IsPrimaryOrBackBuffer(self) && IsMapArea(destRect))
    {
        RECT newDest = *destRect;
        ApplyZoomToRect(&newDest);
        RECT tempSrc;
        RECT* pSrcRect = srcRect;
        if (!pSrcRect)
        {

            DDSURFACEDESC2 desc;
            ZeroMemory(&desc, sizeof(desc));
            desc.dwSize = sizeof(desc);
            if (SUCCEEDED(srcSurface->GetSurfaceDesc(&desc)))
            {
                tempSrc.left = 0;
                tempSrc.top = 0;
                tempSrc.right = desc.dwWidth;
                tempSrc.bottom = desc.dwHeight;
                pSrcRect = &tempSrc;
            }
        }


        if (ClipDestAndAdjustSrc(&newDest, pSrcRect, srcSurface))
        {
            destRect = &newDest;
            srcRect = pSrcRect;
        }
        else
        {
            return DD_OK;
        }
    }

    return OriginalBlt(self, destRect, srcSurface, srcRect, flags, fx);
}

void ClampToViewport(POINT* pt)
{
    if (!pt || !g_hWnd) return;

    RECT client;
    GetClientRect(g_hWnd, &client);
    int width = client.right - client.left;
    int height = client.bottom - client.top;
    int viewportLeft = client.left;
    int viewportTop = client.top;
    int viewportRight = client.left + (int)(width * 0.95);
    int viewportBottom = client.top + (int)(height * 0.99);

    if (pt->x < viewportLeft)   pt->x = viewportLeft;
    if (pt->x > viewportRight)  pt->x = viewportRight;
    if (pt->y < viewportTop)    pt->y = viewportTop;
    if (pt->y > viewportBottom) pt->y = viewportBottom;

}

BOOL WINAPI HookedGetCursorPos(LPPOINT lpPoint)
{
    if (!OriginalGetCursorPos || !lpPoint)
        return FALSE;

    BOOL result = OriginalGetCursorPos(lpPoint);

    if (g_zoom != 1.0f && g_hWnd)
    {
        POINT clientPt = *lpPoint;
        ScreenToClient(g_hWnd, &clientPt);

        if (IsPointInMapArea(clientPt))
        {
            clientPt.x = g_centerX + (LONG)((clientPt.x - g_centerX) / g_zoom);
            clientPt.y = g_centerY + (LONG)((clientPt.y - g_centerY) / g_zoom);
            ClampToViewport(&clientPt);
            ClientToScreen(g_hWnd, &clientPt);
            *lpPoint = clientPt;
        }
    }

    return result;
}


LRESULT CALLBACK NewWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_MOUSEWHEEL)
    {
        POINT pt;
        pt.x = GET_X_LPARAM(lParam);
        pt.y = GET_Y_LPARAM(lParam);
        ScreenToClient(hWnd, &pt);

        if (IsPointInMapArea(pt))
        {
            g_centerX = pt.x;
            g_centerY = pt.y;

            short delta = GET_WHEEL_DELTA_WPARAM(wParam);

            if (delta > 0)
            {
                g_zoom *= 1.1f;
            }
            else
            {
                if (g_zoom > 1.0f)
                    g_zoom /= 1.1f;
            }

            if (g_zoom > g_zoomMax) g_zoom = g_zoomMax;
            if (g_zoom < 1.0f)      g_zoom = 1.0f;
        }

        return 0;
    }

    if (msg == WM_SIZE || msg == WM_MOVE)
        UpdateCenter(hWnd);
    if (g_zoom != 1.0f)
    {
        switch (msg)
        {
        case WM_MOUSEMOVE:
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
        case WM_MBUTTONDOWN:
        case WM_MBUTTONUP:
        case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDBLCLK:
        {
            int screenX = GET_X_LPARAM(lParam);
            int screenY = GET_Y_LPARAM(lParam);
            POINT pt = { screenX, screenY };
            if (IsPointInMapArea(pt))
            {
                int originalX = g_centerX + (LONG)((screenX - g_centerX) / g_zoom);
                int originalY = g_centerY + (LONG)((screenY - g_centerY) / g_zoom);
                lParam = MAKELPARAM(originalX, originalY);
            }
        }
        break;
        }
    }
    return CallWindowProc(OriginalWndProc, hWnd, msg, wParam, lParam);
}

void HookDirectDraw()
{
    LPDIRECTDRAW7 ddraw = nullptr;

    if (FAILED(DirectDrawCreateEx(nullptr, (void**)&ddraw, IID_IDirectDraw7, nullptr)))
        return;

    if (FAILED(ddraw->SetCooperativeLevel(g_hWnd, DDSCL_NORMAL)))
    {
        ddraw->Release();
        return;
    }

    DDSURFACEDESC2 ddsd = { 0 };
    ddsd.dwSize = sizeof(ddsd);
    ddsd.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT;
    ddsd.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN;
    ddsd.dwWidth = 64;
    ddsd.dwHeight = 64;

    LPDIRECTDRAWSURFACE7 surf = nullptr;
    if (FAILED(ddraw->CreateSurface(&ddsd, &surf, nullptr)))
    {
        ddraw->Release();
        return;
    }

    void** vtable = *reinterpret_cast<void***>(surf);
    const int BLT_INDEX = 5;

    OriginalBlt = (BltFunc)vtable[BLT_INDEX];

    DWORD oldProtect = 0;
    VirtualProtect(&vtable[BLT_INDEX], sizeof(void*), PAGE_READWRITE, &oldProtect);
    vtable[BLT_INDEX] = HookedBlt;
    VirtualProtect(&vtable[BLT_INDEX], sizeof(void*), oldProtect, &oldProtect);

    surf->Release();
    ddraw->Release();
}


DWORD WINAPI InitHook(LPVOID lpParam)
{
    DWORD pid = GetCurrentProcessId();
    HWND hWnd = NULL;
    while ((hWnd = FindWindowEx(NULL, hWnd, NULL, NULL)) != NULL) {
    DWORD dwPid = 0;
    GetWindowThreadProcessId(hWnd, &dwPid);
    if (dwPid == pid && IsWindowVisible(hWnd)) break;
    }
    g_hWnd = hWnd;
    UpdateCenter(g_hWnd);

    OriginalWndProc = (WNDPROC)SetWindowLongPtrW(
        g_hWnd, GWLP_WNDPROC, (LONG_PTR)NewWndProc
    );

    HookDirectDraw();

    if (MH_Initialize() == MH_OK)
    {
        MH_CreateHookApi(
            L"user32.dll",
            "GetCursorPos",
            HookedGetCursorPos,
            (void**)&OriginalGetCursorPos
        );
        MH_EnableHook(MH_ALL_HOOKS);
    }

    ShowCursor(FALSE);

    return 0;
}


BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpReserved)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hModule);
    }
    return TRUE;
}

DEFINE_HOOK(0x52CAE9, GameInt, 0x5)
{
    CreateThread(nullptr, 0, InitHook, nullptr, 0, nullptr);
    return 0;
}