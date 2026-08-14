#include <windows.h>
#include <ddraw.h>
#include <cstdio>

#include "MinHook.h"
#include <windowsx.h>

// =====================================================
// 函数指针类型
// =====================================================
typedef HRESULT(WINAPI* BltFunc)(
    LPDIRECTDRAWSURFACE7 self,
    LPRECT destRect,
    LPDIRECTDRAWSURFACE7 srcSurface,
    LPRECT srcRect,
    DWORD flags,
    LPDDBLTFX fx
    );

typedef BOOL(WINAPI* GetCursorPosFunc)(LPPOINT lpPoint);

// =====================================================
// 全局变量
// =====================================================
BltFunc              OriginalBlt = nullptr;
GetCursorPosFunc     OriginalGetCursorPos = nullptr;
WNDPROC              OriginalWndProc = nullptr;

HWND                 g_hWnd = nullptr;
float                g_zoom = 1.0f;
float                g_zoomMin = 1.0f;
float                g_zoomMax = 2.0f;
LONG                 g_centerX = 400;
LONG                 g_centerY = 300;

// =====================================================
// 前向声明
// =====================================================
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

// =====================================================
// 获取游戏窗口
// =====================================================
HWND FindGameWindow()
{
    // TODO: 改成游戏实际窗口标题
    // 常见红警2标题："Red Alert 2" 或 "Yuri's Revenge"
    HWND h = FindWindowW(nullptr, L"Red Alert 2");
    if (h) return h;

    h = FindWindowW(nullptr, L"Yuri's Revenge");
    if (h) return h;

    // 如果找不到，可以返回前台窗口
    return GetForegroundWindow();
}

// =====================================================
// 更新屏幕中心（窗口大小变化时调用）
// =====================================================
void UpdateCenter(HWND hWnd)
{
    if (!hWnd) return;

    RECT client;
    GetClientRect(hWnd, &client);

    g_centerX = (client.right + client.left) / 2;
    g_centerY = (client.bottom + client.top) / 2;


}

// =====================================================
// 判断矩形是否在地图区域（用于绘制缩放）
// 使用中心点判断，避免 UI 按钮被缩放
// =====================================================
bool IsMapArea(LPRECT rect)
{
    if (!rect || !g_hWnd) return false;

    LONG centerX = (rect->left + rect->right) / 2;
    LONG centerY = (rect->top + rect->bottom) / 2;

    RECT client;
    GetClientRect(g_hWnd, &client);
    int width = client.right - client.left;
    int height = client.bottom - client.top;

    // 地图区域：右侧 5% 和底部 2% 为 UI，其余都是地图
    int mapRight = client.left + (int)(width * 0.88);
    int mapBottom = client.top + (int)(height * 0.98);

    if (centerX >= mapRight)  return false;
    if (centerY >= mapBottom) return false;

    return true;
}

// =====================================================
// 判断鼠标是否在地图区域（用于鼠标坐标修正）
// 比例更宽松，几乎覆盖整个可视地图区域
// =====================================================
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
// =====================================================
// 围绕缩放中心缩放矩形
// =====================================================
void ApplyZoomToRect(LPRECT rect)
{
    if (!rect) return;

    rect->left = g_centerX + (LONG)((rect->left - g_centerX) * g_zoom);
    rect->top = g_centerY + (LONG)((rect->top - g_centerY) * g_zoom);
    rect->right = g_centerX + (LONG)((rect->right - g_centerX) * g_zoom);
    rect->bottom = g_centerY + (LONG)((rect->bottom - g_centerY) * g_zoom);
}

// =====================================================
// 将矩形裁剪到地图区域内，防止放大后覆盖 UI
// =====================================================
void ClipRectToMap(LPRECT rect)
{
    if (!rect || !g_hWnd) return;

    RECT client;
    GetClientRect(g_hWnd, &client);
    int width = client.right - client.left;
    int height = client.bottom - client.top;

    // 地图区域边界（与 IsMapArea 使用相同比例）
    int mapRight = client.left + (int)(width * 0.88);
    int mapBottom = client.top + (int)(height * 0.99);

    // 裁剪右边界和底边界
    if (rect->right > mapRight)  rect->right = mapRight;
    if (rect->bottom > mapBottom) rect->bottom = mapBottom;

    // 确保矩形有效（防止完全越界）
    if (rect->left > mapRight)  rect->left = mapRight;
    if (rect->top > mapBottom) rect->top = mapBottom;
}

// 将目标矩形裁剪到地图区域内，并同步调整源矩形以保持比例
// 返回 false 表示目标矩形完全在区域外，不应绘制
bool ClipDestAndAdjustSrc(RECT* destRect, RECT* srcRect, IDirectDrawSurface7* srcSurface)
{
    if (!destRect || !g_hWnd) return false;

    // 获取地图区域边界（与 IsMapArea 使用相同比例）
    RECT client;
    GetClientRect(g_hWnd, &client);
    int width = client.right - client.left;
    int height = client.bottom - client.top;
    int mapRight = client.left + (int)(width * 0.88);   // 地图右边界
    int mapBottom = client.top + (int)(height * 0.99);   // 地图底边界

    RECT originalDest = *destRect;
    RECT newDest = originalDest;

    // 裁剪目标矩形到地图区域
    if (newDest.right > mapRight)  newDest.right = mapRight;
    if (newDest.bottom > mapBottom) newDest.bottom = mapBottom;
    if (newDest.left > mapRight)   newDest.left = mapRight;
    if (newDest.top > mapBottom)  newDest.top = mapBottom;

    // 确保矩形有效
    if (newDest.right <= newDest.left || newDest.bottom <= newDest.top)
        return false;   // 完全越界，不绘制

    // 计算裁剪前后的宽高比
    LONG origWidth = originalDest.right - originalDest.left;
    LONG origHeight = originalDest.bottom - originalDest.top;
    LONG newWidth = newDest.right - newDest.left;
    LONG newHeight = newDest.bottom - newDest.top;

    if (origWidth <= 0 || origHeight <= 0)
        return false;

    // 获取源矩形
    RECT src;
    if (srcRect)
    {
        src = *srcRect;
    }
    else
    {
        // 如果 srcRect 为 NULL，使用整个源表面
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

    // 计算源矩形中对应的裁剪区域
    // 裁剪比例 = newDest / originalDest
    // 注意：我们只裁剪右侧和底部，所以左侧和顶部比例不变
    LONG srcWidth = src.right - src.left;
    LONG srcHeight = src.bottom - src.top;
    if (srcWidth <= 0 || srcHeight <= 0)
        return false;

    // 计算源矩形中保留的部分
    RECT newSrc = src;
    if (origWidth != newWidth)
    {
        // 右侧被裁剪
        LONG cutPixels = origWidth - newWidth;
        LONG srcCut = (LONG)((double)cutPixels / origWidth * srcWidth);
        newSrc.right -= srcCut;
    }
    if (origHeight != newHeight)
    {
        // 底部被裁剪
        LONG cutPixels = origHeight - newHeight;
        LONG srcCut = (LONG)((double)cutPixels / origHeight * srcHeight);
        newSrc.bottom -= srcCut;
    }

    // 更新参数
    *destRect = newDest;
    if (srcRect)
        *srcRect = newSrc;
    // 如果 srcRect 为 NULL，我们需要创建一个新的 RECT 并传递
    // 但函数签名不支持新建 srcRect 传出，所以我们稍后在 HookedBlt 中处理

    return true;
}

// =====================================================
// 判断 DirectDraw 表面是否为主表面或后备缓冲
// =====================================================
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


// 判断矩形是否与 UI 区域（建造栏、命令栏等）有重叠
// 如果重叠，则不应缩放该矩形，以免覆盖 UI 导致闪烁
bool IsRectOverlapUI(const RECT& rect)
{
    if (!g_hWnd) return false;

    RECT client;
    GetClientRect(g_hWnd, &client);
    int width = client.right - client.left;
    int height = client.bottom - client.top;

    // UI 区域边界：右侧 7% 和底部 4% 为 UI（比地图边界更小）
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
// =====================================================
// Hooked DirectDraw Blt
// =====================================================
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

        // 如果 srcRect 为 NULL，我们需要传递一个临时的源矩形
        RECT tempSrc;
        RECT* pSrcRect = srcRect;
        if (!pSrcRect)
        {
            // 获取源表面尺寸
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

        // 调用裁剪函数
        if (ClipDestAndAdjustSrc(&newDest, pSrcRect, srcSurface))
        {
            destRect = &newDest;
            srcRect = pSrcRect;   // 注意：如果原 srcRect 为 NULL，这里仍然需要传递非NULL的指针
        }
        // 如果裁剪后完全越界，则跳过绘制（不调用 OriginalBlt）
        else
        {
            return DD_OK;  // 或直接返回，表示不绘制
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

    // 原始视口边界（请根据实际游戏内部视口调整比例）
    int viewportLeft = client.left;
    int viewportTop = client.top;
    int viewportRight = client.left + (int)(width * 0.95);
    int viewportBottom = client.top + (int)(height * 0.99);

    if (pt->x < viewportLeft)   pt->x = viewportLeft;
    if (pt->x > viewportRight)  pt->x = viewportRight;
    if (pt->y < viewportTop)    pt->y = viewportTop;
    if (pt->y > viewportBottom) pt->y = viewportBottom;

}




// =====================================================
// Hooked GetCursorPos（修正鼠标坐标）
// 只在地图区域修正，UI 区域保持原始坐标
// =====================================================
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
            // 逆缩放
            clientPt.x = g_centerX + (LONG)((clientPt.x - g_centerX) / g_zoom);
            clientPt.y = g_centerY + (LONG)((clientPt.y - g_centerY) / g_zoom);

            // 钳制到原始视口内，避免超出有效区域
            ClampToViewport(&clientPt);

            // 转回屏幕坐标
            ClientToScreen(g_hWnd, &clientPt);
            *lpPoint = clientPt;
        }
    }

    return result;
}



// =====================================================
// 新的窗口过程：处理滚轮缩放和鼠标坐标修正
// =====================================================
LRESULT CALLBACK NewWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_MOUSEWHEEL)
    {
        // 从 lParam 中获取鼠标屏幕坐标（不受 Hook 影响）
        POINT pt;
        pt.x = GET_X_LPARAM(lParam);
        pt.y = GET_Y_LPARAM(lParam);
        ScreenToClient(hWnd, &pt);   // 转换为客户区坐标

        // 只有鼠标在地图区域时才允许缩放
        if (IsPointInMapArea(pt))
        {
            // 以鼠标当前位置为缩放中心
            g_centerX = pt.x;
            g_centerY = pt.y;

            short delta = GET_WHEEL_DELTA_WPARAM(wParam);

            if (delta > 0)
            {
                g_zoom *= 1.1f;   // 放大
            }
            else
            {
                if (g_zoom > 1.0f)   // 不允许缩小到原始大小以下
                    g_zoom /= 1.1f;
            }

            // 限制缩放范围
            if (g_zoom > g_zoomMax) g_zoom = g_zoomMax;
            if (g_zoom < 1.0f)      g_zoom = 1.0f;
        }

        return 0;   // 吞掉消息，不传递给原窗口
    }

    // 窗口大小或位置变化时，更新屏幕中心
    if (msg == WM_SIZE || msg == WM_MOVE)
        UpdateCenter(hWnd);

    // 修正鼠标消息中的坐标（逆缩放）
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

            // 只有在地图区域才逆缩放，UI 区域保持原坐标
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

    // 调用原来的窗口过程
    return CallWindowProc(OriginalWndProc, hWnd, msg, wParam, lParam);
}

// =====================================================
// Hook DirectDraw Blt
// =====================================================
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
    const int BLT_INDEX = 5;   // IDirectDrawSurface7 的 Blt 索引

    OriginalBlt = (BltFunc)vtable[BLT_INDEX];

    DWORD oldProtect = 0;
    VirtualProtect(&vtable[BLT_INDEX], sizeof(void*), PAGE_READWRITE, &oldProtect);
    vtable[BLT_INDEX] = HookedBlt;
    VirtualProtect(&vtable[BLT_INDEX], sizeof(void*), oldProtect, &oldProtect);

    surf->Release();
    ddraw->Release();
}

// =====================================================
// 初始化线程
// =====================================================
DWORD WINAPI InitHook(LPVOID lpParam)
{
    // 等待游戏窗口创建
    for (int i = 0; i < 100; i++)
    {
        g_hWnd = FindGameWindow();
        if (g_hWnd)
            break;
        Sleep(100);
    }

    if (!g_hWnd)
        return 0;

    UpdateCenter(g_hWnd);

    // 子类化窗口，处理滚轮和鼠标消息
    OriginalWndProc = (WNDPROC)SetWindowLongPtrW(
        g_hWnd, GWLP_WNDPROC, (LONG_PTR)NewWndProc
    );

    // Hook DirectDraw Blt
    HookDirectDraw();

    // Hook GetCursorPos（使用 MinHook）
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

    // 隐藏系统光标
    ShowCursor(FALSE);

    return 0;
}

// =====================================================
// DLL 入口点
// =====================================================
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpReserved)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hModule);
        CreateThread(nullptr, 0, InitHook, nullptr, 0, nullptr);
    }
    return TRUE;
}