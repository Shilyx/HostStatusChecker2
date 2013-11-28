#define _WIN32_IE 0x500

#include <WinSock2.h>
#include <MSTcpIP.h>
#include <MSWSock.h>
#include <Windows.h>
#include <Shlwapi.h>
#include <CommCtrl.h>
#include <tchar.h>
#include "resource.h"

#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "Shlwapi.lib")
#pragma comment(lib, "ComCtl32.lib")

#define NOTIFYWNDCLASS TEXT("__slx_HostStatusChecker_multi_20131121")

static enum
{
    WM_CALLBACK = WM_USER + 112,
    WM_REPORT,
    WM_UPDATETIP,
};

static enum
{
    MI_ABOUT = 11,
    MI_DETAIL,
    MI_USEINFO,
    MI_QUIT,
};

static enum IoType
{
    IT_NONE,
    IT_CONNECT,
    IT_RECV,
};

static enum CompleteKey
{
    CK_NORMAL = 112,
    CK_QUIT,
};

struct HostInfo
{
    OVERLAPPED  ol;
    int         index;
    SOCKET      sock;
    DWORD       dwFlag;
    enum IoType type;
    WSABUF      buf;
    char        szHostAddress[64];
    DWORD       dwAddress;
    USHORT      usPort;
    BOOL        bActive;
#ifdef _DEBUG
    char        buffer[16];
#else
    char        buffer[4096];
#endif
};

static struct HostInfo g_hosts[1024];
static int g_nHostsCount = 0;
static int g_nActiveCount = 0;
static HWND g_hMainWnd = NULL;

static BOOL InitWinSock(WORD ver)
{
    WSADATA wd;
    int nTryTime = 0;

    while (WSAStartup(ver, &wd) != 0)
    {
        Sleep(1000);
        nTryTime += 1;

        if (nTryTime > 20)
        {
            return FALSE;
        }
    }

    return TRUE;
}

BOOL IsIpAddress(LPCSTR lpAddress)
{
    char szAddress[100];
    DWORD dwAddress = inet_addr(lpAddress);

    lstrcpynA(szAddress, inet_ntoa(*(struct in_addr *)&dwAddress), RTL_NUMBER_OF(szAddress));

    return lstrcmpiA(szAddress, lpAddress) == 0;
}

BOOL IsPortW(LPCWSTR lpPort, USHORT *pusPort)
{
    WCHAR szPort[100];
    int nPort = StrToIntW(lpPort);

    if (nPort < 0 || nPort > 65535)
    {
        return FALSE;
    }

    wnsprintfW(szPort, RTL_NUMBER_OF(szPort), L"%d", nPort);

    if (lstrcmpiW(lpPort, szPort) != 0)
    {
        return FALSE;
    }

    if (pusPort != NULL)
    {
        *pusPort = (USHORT)nPort;
    }

    return TRUE;
}

int MessageBoxFormat(HWND hWindow, LPCTSTR lpCaption, UINT uType, LPCTSTR lpFormat, ...)
{
    TCHAR szText[2000];
    va_list val;

    va_start(val, lpFormat);
    wvnsprintf(szText, sizeof(szText) / sizeof(TCHAR), lpFormat, val);
    va_end(val);

    return MessageBox(hWindow, szText, lpCaption, uType);
}

void SafeDebugMessage(LPCTSTR lpFormat, ...)
{
    TCHAR szText[2000];
    va_list val;

    va_start(val, lpFormat);
    wvnsprintf(szText, sizeof(szText) / sizeof(TCHAR), lpFormat, val);
    va_end(val);

    OutputDebugString(szText);
}

BOOL SetSockKeepAlive(
    SOCKET s,
    ULONG  onoff,
    ULONG  keepalivetime,
    ULONG  keepaliveinterval
    )
{
    struct tcp_keepalive tka;
    DWORD dwBytesReturned = 0;

    tka.onoff = onoff;
    tka.keepalivetime = keepalivetime;
    tka.keepaliveinterval = keepaliveinterval;

    return 0 == WSAIoctl(
        s,
        SIO_KEEPALIVE_VALS,
        &tka,
        sizeof(tka),
        NULL,
        0,
        &dwBytesReturned,
        NULL,
        NULL
        );
}

__forceinline BOOL Xor(BOOL b1, BOOL b2)
{
    return !!(!!b1 ^ !!b2);
}

ULONGLONG GetDllVersion(LPCTSTR lpszDllName)
{
    HINSTANCE hinstDll = GetModuleHandle(lpszDllName);
    ULONGLONG ullVersion = 0;

    if (hinstDll == NULL)
    {
        hinstDll = LoadLibrary(lpszDllName);
    }

    if (hinstDll != NULL)
    {
        DLLGETVERSIONPROC pDllGetVersion = (DLLGETVERSIONPROC)GetProcAddress(hinstDll, "DllGetVersion");

        if (pDllGetVersion)
        {
            DLLVERSIONINFO dvi = {sizeof(dvi)};

            if (SUCCEEDED(pDllGetVersion(&dvi)))
            {
                ullVersion = MAKEDLLVERULL(dvi.dwMajorVersion, dvi.dwMinorVersion, dvi.dwBuildNumber, 0);
            }
        }
    }

    return ullVersion;
}

int GetNotifyIconDataSize()
{
    ULONGLONG ullShell32Version = GetDllVersion(TEXT("Shell32.dll"));

    if (ullShell32Version >= MAKEDLLVERULL(6, 0, 0, 0))
    {
        return sizeof(NOTIFYICONDATA);
    }
    else if(ullShell32Version >= MAKEDLLVERULL(5, 0, 0, 0))
    {
        return NOTIFYICONDATA_V2_SIZE;
    }
    else
    {
        return NOTIFYICONDATA_V1_SIZE;
    }
}

static LRESULT __stdcall NotifyWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    static UINT WM_REBUILDTOOLBAR = 0;
    static HINSTANCE s_hInstance;
    static NOTIFYICONDATA s_nid = {sizeof(s_nid)};
    static BOOL s_bShowInfo = TRUE;
    static HANDLE s_hIocp = NULL;
    static int s_nTicks[RTL_NUMBER_OF(g_hosts)] = {0};
    static HWND s_hToolTip = NULL;
    static TOOLINFO s_ti = {sizeof(s_ti)};

    switch (uMsg)
    {
    case WM_CREATE:
        s_hInstance = ((LPCREATESTRUCT)lParam)->hInstance;

        s_nid.cbSize = GetNotifyIconDataSize();
        s_nid.hWnd = hWnd;
        s_nid.uID = 1;
        s_nid.uCallbackMessage = WM_CALLBACK;
        s_nid.hIcon = LoadIcon(s_hInstance, MAKEINTRESOURCE(IDI_MAINFRAME));
        s_nid.uFlags = NIF_TIP | NIF_MESSAGE | NIF_ICON;
        lstrcpyn(s_nid.szInfoTitle, TEXT("信息"), RTL_NUMBER_OF(s_nid.szInfoTitle));
        s_nid.uTimeout = 3000;
        s_nid.dwInfoFlags = NIIF_INFO;
        SendMessage(hWnd, WM_UPDATETIP, 0, 0);

        Shell_NotifyIcon(NIM_ADD, &s_nid);

        WM_REBUILDTOOLBAR = RegisterWindowMessage(TEXT("TaskbarCreated"));
        s_hIocp = (HANDLE)((LPCREATESTRUCT)lParam)->lpCreateParams;
        return 0;

    case WM_CLOSE:
        Shell_NotifyIcon(NIM_DELETE, &s_nid);
        DestroyWindow(hWnd);
        return 0;
        
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;

    case WM_TIMER:
        if (wParam >= 0 && wParam <= RTL_NUMBER_OF(s_nTicks))
        {
            s_nTicks[wParam] -= 1;
            if (s_nTicks[wParam] == 0)
            {
                KillTimer(hWnd, wParam);
                PostQueuedCompletionStatus(s_hIocp, 0, CK_NORMAL, (LPOVERLAPPED)&g_hosts[wParam]);
            }
            else if (s_nTicks[wParam] < 0)
            {
                s_nTicks[wParam] = 10;
            }
        }
        return 0;

    case WM_UPDATETIP:
        return wnsprintf(
            s_nid.szTip,
            RTL_NUMBER_OF(s_nid.szTip),
            TEXT("主机活动情况：%d/%d\r\n")
            TEXT("点击图标查看详情"),
            g_nActiveCount,
            g_nHostsCount
            );

    case WM_ACTIVATE:
        if (!(BOOL)wParam)
        {
            SendMessage(hWnd, WM_MOUSEMOVE, 0, 0);
        }
        break;

    case WM_LBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MOUSEMOVE:
    case WM_MOUSEWHEEL:
        if (IsWindow(s_hToolTip))
        {
            SendMessage(s_hToolTip, TTM_TRACKACTIVATE, FALSE, (LPARAM)&s_ti);
        }
        break;

    case WM_CALLBACK:
        if (lParam == WM_LBUTTONUP)
        {
            static TCHAR szText[RTL_NUMBER_OF(g_hosts) * (sizeof(g_hosts->szHostAddress) + 100)] = TEXT("");

            int i;
            POINT pt;
            RECT rect;

            szText[0] = TEXT('\0');

            for (i = 0; i < g_nHostsCount; i += 1)
            {
                int nLength = lstrlen(szText);

                if (szText[0] != TEXT('\0'))
                {
                    StrCatBuff(szText, TEXT("\r\n"), RTL_NUMBER_OF(szText));
                    nLength += 2;
                }

                wnsprintf(
                    szText + nLength,
                    RTL_NUMBER_OF(szText) - nLength,
                    TEXT("%hs当前处于%s活动状态"),
                    g_hosts[i].szHostAddress,
                    g_hosts[i].bActive ? TEXT("") : TEXT("不")
                    );
            }

            if (!IsWindow(s_hToolTip))
            {
                s_ti.uFlags = 0;
                s_ti.hwnd = hWnd;
                s_ti.hinst = s_hInstance;
                s_ti.uId = 1;
                s_ti.uFlags = TTF_TRANSPARENT | TTF_ABSOLUTE | TTF_TRACK;

                s_hToolTip = CreateWindowEx(
                    WS_EX_TOPMOST,
                    TOOLTIPS_CLASS,
                    NULL,
                    WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
                    0,
                    0,
                    0,
                    0,
                    hWnd,
                    NULL,
                    s_hInstance,
                    NULL
                    );
                SendMessage(s_hToolTip, TTM_SETMAXTIPWIDTH, 0, MAXINT_PTR);
                SendMessage(s_hToolTip, TTM_ADDTOOL, 0, (LPARAM)&s_ti);
            }

            s_ti.lpszText = szText;
            SendMessage(s_hToolTip, TTM_UPDATETIPTEXT, 0, (LPARAM)&s_ti);

            GetWindowRect(s_hToolTip, &rect);
            GetCursorPos(&pt);

            pt.x = min(pt.x, GetSystemMetrics(SM_CXSCREEN) - rect.right + rect.left);
            SendMessage(s_hToolTip, TTM_TRACKPOSITION, 0, MAKELONG(pt.x, pt.y - rect.bottom + rect.top));

            SetForegroundWindow(hWnd);
            SendMessage(s_hToolTip, TTM_TRACKACTIVATE, TRUE, (LPARAM)&s_ti);

            if (rect.left == 0 && rect.right == 0 && rect.top == 0 && rect.bottom == 0)
            {
                GetWindowRect(s_hToolTip, &rect);
                GetCursorPos(&pt);

                SafeDebugMessage(TEXT("%d,%d,%d,%d,%d,%d\n"), rect.left, rect.right, rect.top, rect.bottom, pt.x, pt.y);

                pt.x = min(pt.x, GetSystemMetrics(SM_CXSCREEN) - rect.right + rect.left);
                pt.y = pt.y - rect.bottom + rect.top;

                SetWindowPos(s_hToolTip, NULL, pt.x, pt.y, 0, 0, SWP_NOZORDER | SWP_NOSIZE);
            }
        }
        else if (lParam == WM_RBUTTONUP || lParam == WM_CONTEXTMENU)
        {
            POINT pt;
            HMENU hMenu = CreatePopupMenu();

            GetCursorPos(&pt);

            SetForegroundWindow(hWnd);

            AppendMenu(hMenu, MF_STRING, MI_ABOUT, TEXT("关于(&A)..."));
            AppendMenu(hMenu, MF_SEPARATOR, 0, NULL);
            AppendMenu(hMenu, MF_STRING, MI_DETAIL, TEXT("详细信息(&D)..."));
            AppendMenu(hMenu, MF_STRING, MI_USEINFO, TEXT("启用气泡通知(&B)"));
            AppendMenu(hMenu, MF_STRING, MI_QUIT, TEXT("退出(&Q)"));
            SetMenuDefaultItem(hMenu, MI_DETAIL, MF_BYCOMMAND);

            if (s_bShowInfo)
            {
                CheckMenuItem(hMenu, MI_USEINFO, MF_BYCOMMAND | MF_CHECKED);
            }
            else
            {
                CheckMenuItem(hMenu, MI_USEINFO, MF_BYCOMMAND | MF_UNCHECKED);
            }

            switch (TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, hWnd, NULL))
            {
            case MI_ABOUT:
                MessageBox(hWnd, TEXT("HostStatusChecker_single application"), TEXT("信息"), MB_SYSTEMMODAL | MB_ICONINFORMATION);
                break;

            case MI_DETAIL:
                SendMessage(hWnd, WM_CALLBACK, 0, WM_LBUTTONUP);
                break;

            case MI_USEINFO:
                s_bShowInfo = !s_bShowInfo;
                break;

            case MI_QUIT:
                PostMessage(hWnd, WM_CLOSE, 0, 0);
                break;

            default:
                break;
            }

            DestroyMenu(hMenu);
        }
        return 0;

    case WM_REPORT:
    {
        UINT uLastFlags = s_nid.uFlags;

        s_nid.uFlags = NIF_TIP;
        SendMessage(hWnd, WM_UPDATETIP, 0, 0);

        if (s_bShowInfo)
        {
            DWORD dwIndex = (DWORD)wParam;
            BOOL bActive = (BOOL)lParam;

            wnsprintf(
                s_nid.szInfo,
                RTL_NUMBER_OF(s_nid.szInfo),
                TEXT("主机“%hs”已进入%s活动状态。"),
                g_hosts[dwIndex].szHostAddress,
                bActive ? TEXT("") : TEXT("不")
                );

            s_nid.uFlags |= NIF_INFO;
        }

        Shell_NotifyIcon(NIM_MODIFY, &s_nid);
        s_nid.uFlags = uLastFlags;

        return 0;
    }

    default:
        if (uMsg == WM_REBUILDTOOLBAR)
        {
            Shell_NotifyIcon(NIM_ADD, &s_nid);
        }
        break;
    }

    return DefWindowProc(hWnd, uMsg, wParam, lParam);
}

BOOL GetHostInformation()
{
    BOOL bSuccess = FALSE;
    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);

    if (argc < 3)
    {
        MessageBox(
            NULL,
            TEXT("获取主机信息失败，请加参数运行\r\n")
            TEXT("\r\n第一个参数为主机地址或ip\r\n第二个参数为对应端口")
            TEXT("\r\n第三个参数为主机地址或ip\r\n第四个参数为对应端口")
            TEXT("\r\n以此类推..."),
            NULL,
            MB_SYSTEMMODAL | MB_ICONERROR
            );
    }
    else
    {
        int index = 0;

        if ((argc + 1) % 2 != 0)
        {
            argc -= 1;
        }

        for (bSuccess = TRUE; index < (argc - 1) / 2; ++index)
        {
            LPCWSTR lpAddress = argv[index * 2 + 1];
            LPCWSTR lpPort = argv[index * 2 + 2];
            USHORT usPort = 0;
            char szDomainNameOrIpAddress[1024];
            DWORD dwAddress = 0;

            if (!IsPortW(lpPort, &usPort))
            {
                MessageBoxFormat(NULL, NULL, MB_SYSTEMMODAL | MB_ICONERROR, TEXT("“%ls”不是合法的端口号"), lpPort);
                bSuccess = FALSE;
                break;
            }

            wnsprintfA(szDomainNameOrIpAddress, sizeof(szDomainNameOrIpAddress), "%S", lpAddress);

            if (IsIpAddress(szDomainNameOrIpAddress))
            {
                dwAddress = inet_addr(szDomainNameOrIpAddress);
            }
            else
            {
                struct hostent *pHostEnt = gethostbyname(szDomainNameOrIpAddress);

                if (pHostEnt == NULL)
                {
                    MessageBoxFormat(NULL, NULL, MB_SYSTEMMODAL | MB_ICONERROR, TEXT("“%ls”无法正确解析"), argv[1]);
                    bSuccess = FALSE;
                    break;
                }
                else
                {
                    dwAddress = *(DWORD *)pHostEnt->h_addr_list[0];
                }
            }

            g_hosts[g_nHostsCount].dwAddress = dwAddress;
            g_hosts[g_nHostsCount].usPort = usPort;
            g_hosts[g_nHostsCount].bActive = FALSE;

            if (lstrlenA(szDomainNameOrIpAddress) < RTL_NUMBER_OF(g_hosts[g_nHostsCount].szHostAddress) - 6)
            {
                wnsprintfA(
                    g_hosts[g_nHostsCount].szHostAddress,
                    RTL_NUMBER_OF(g_hosts[g_nHostsCount].szHostAddress),
                    "%s:%u",
                    szDomainNameOrIpAddress,
                    usPort
                    );
            }
            else
            {
                lstrcpynA(
                    g_hosts[g_nHostsCount].szHostAddress,
                    szDomainNameOrIpAddress,
                    RTL_NUMBER_OF(g_hosts[g_nHostsCount].szHostAddress)
                    );
                wnsprintfA(
                    g_hosts[g_nHostsCount].szHostAddress + RTL_NUMBER_OF(g_hosts[g_nHostsCount].szHostAddress) - 6 - 3,
                    8,
                    "..:%lu",
                    usPort
                    );
            }

            g_nHostsCount += 1;
        }
    }

    if (argv != NULL)
    {
        LocalFree(argv);
    }

    return bSuccess;
}

BOOL DoConnectEx(LPFN_CONNECTEX pConnectEx, struct HostInfo *pHostInfo)
{
    struct sockaddr_in sin;

    ZeroMemory(&pHostInfo->ol, sizeof(pHostInfo->ol));
    pHostInfo->type = IT_CONNECT;

    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = pHostInfo->dwAddress;
    sin.sin_port = htons(pHostInfo->usPort);

    return pConnectEx(
        pHostInfo->sock,
        (const struct sockaddr *)&sin,
        sizeof(sin),
        NULL,
        0,
        NULL,
        &pHostInfo->ol
        );
}

BOOL DoRecv(struct HostInfo *pHostInfo)
{
    ZeroMemory(&pHostInfo->ol, sizeof(pHostInfo->ol));
    pHostInfo->buf.buf = pHostInfo->buffer;
    pHostInfo->buf.len = sizeof(pHostInfo->buffer);
    pHostInfo->dwFlag = 0;
    pHostInfo->type = IT_RECV;

    return WSARecv(
        pHostInfo->sock,
        &pHostInfo->buf,
        1,
        NULL,
        &pHostInfo->dwFlag,
        &pHostInfo->ol,
        NULL
        );
}

DWORD __stdcall SnifferProc(LPVOID lpParam)
{
    struct sockaddr_in sin_zero = {AF_INET};
    HANDLE hIocp = (HANDLE)lpParam;
    LPFN_CONNECTEX pConnectEx = NULL;
    int nIndex = 0;

    for (; nIndex < g_nHostsCount; nIndex += 1)
    {
        g_hosts[nIndex].index = nIndex;

        g_hosts[nIndex].sock = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
        bind(g_hosts[nIndex].sock, (const struct sockaddr *)&sin_zero, sizeof(sin_zero));
        CreateIoCompletionPort((HANDLE)g_hosts[nIndex].sock, hIocp, CK_NORMAL, 0);

        if (pConnectEx == NULL)
        {
            GUID guidConnectEx = WSAID_CONNECTEX;
            DWORD dwBytesRead = 0;

            WSAIoctl(
                g_hosts[nIndex].sock,
                SIO_GET_EXTENSION_FUNCTION_POINTER,
                &guidConnectEx,
                sizeof(guidConnectEx),
                &pConnectEx,
                sizeof(pConnectEx),
                &dwBytesRead,
                NULL,
                NULL
                );

            if (pConnectEx == NULL)
            {
                return 0;
            }
        }

        DoConnectEx(pConnectEx, &g_hosts[nIndex]);
    }

    while (TRUE)
    {
        DWORD dwBytesTransferred = 0;
        ULONG_PTR ulCompletionKey = 0;
        struct HostInfo *pHostInfo = NULL;
        BOOL bIoSucceed = GetQueuedCompletionStatus(hIocp, &dwBytesTransferred, &ulCompletionKey, (LPOVERLAPPED *)&pHostInfo, INFINITE);

        if (ulCompletionKey == CK_QUIT/* && dwBytesTransferred == 0 && pHostInfo != NULL*/)
        {
            break;
        }

        if (!IsWindow(g_hMainWnd))
        {
            break;
        }

        if (pHostInfo == NULL)
        {
            Sleep(100);
            continue;
        }

        if (ulCompletionKey != CK_NORMAL)
        {
            continue;
        }

        if (pHostInfo->type == IT_CONNECT)
        {
            if (Xor(bIoSucceed, pHostInfo->bActive))
            {
                if (bIoSucceed)
                {
                    g_nActiveCount += 1;
                }
                else
                {
                    g_nActiveCount -= 1;
                }

                pHostInfo->bActive = bIoSucceed;
                PostMessage(g_hMainWnd, WM_REPORT, pHostInfo->index, bIoSucceed);
            }

            if (bIoSucceed)
            {
                setsockopt(pHostInfo->sock, SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT, NULL, 0);
                SetSockKeepAlive(pHostInfo->sock, 1, 500, 2000);

                DoRecv(pHostInfo);
            }
            else
            {
                closesocket(pHostInfo->sock);
                pHostInfo->sock = INVALID_SOCKET;

                pHostInfo->type = IT_RECV;

                KillTimer(g_hMainWnd, pHostInfo->index);
                SetTimer(g_hMainWnd, pHostInfo->index, 1000, NULL);
            }
        }
        else if (pHostInfo->type == IT_RECV)
        {
            if (dwBytesTransferred == 0)
            {
                if (pHostInfo->sock != INVALID_SOCKET)
                {
                    closesocket(pHostInfo->sock);
                }

                pHostInfo->sock = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
                bind(pHostInfo->sock, (const struct sockaddr *)&sin_zero, sizeof(sin_zero));
                CreateIoCompletionPort((HANDLE)pHostInfo->sock, hIocp, CK_NORMAL, 0);

                DoConnectEx(pConnectEx, pHostInfo);
            }
            else
            {
                DoRecv(pHostInfo);
            }
        }
        else
        {
            // ...
        }
    }

    for (nIndex = 0; nIndex < g_nHostsCount; nIndex += 1)
    {
        CancelIo((HANDLE)g_hosts[nIndex].sock);
        closesocket(g_hosts[nIndex].sock);
    }

    return 0;
}

int APIENTRY _tWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPTSTR lpCmdLine, int nShowCmd)
{
    HANDLE hIocp = NULL;
    HANDLE hSnifferThread = NULL;
    WNDCLASSEX wcex = {sizeof(wcex)};

    InitCommonControls();

    if (!InitWinSock(MAKEWORD(2, 2)))
    {
        MessageBox(NULL, TEXT("初始化网络失败"), NULL, MB_SYSTEMMODAL | MB_ICONERROR);
        return 0;
    }

    if (!GetHostInformation())
    {
        return 0;
    }

    hIocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);

    wcex.lpfnWndProc = NotifyWndProc;
    wcex.lpszClassName = NOTIFYWNDCLASS;
    wcex.hInstance = hInstance;
    wcex.style = CS_HREDRAW | CS_VREDRAW;

    RegisterClassEx(&wcex);

    g_hMainWnd = CreateWindowEx(
        0,
        NOTIFYWNDCLASS,
        NOTIFYWNDCLASS,
        WS_OVERLAPPEDWINDOW,
        0,
        0,
        433,
        334,
        NULL,
        NULL,
        hInstance,
        (LPVOID)hIocp
        );

//     ShowWindow(g_hMainWnd, SW_SHOW);
//     UpdateWindow(g_hMainWnd);

    hSnifferThread = CreateThread(NULL, 0, SnifferProc, (LPVOID)hIocp, 0, NULL);

    while (TRUE)
    {
        MSG msg;
        int nRet = GetMessage(&msg, NULL, 0, 0);

        if (nRet <= 0)
        {
            break;
        }

        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    PostQueuedCompletionStatus(hIocp, 0, CK_QUIT, NULL);

    WaitForSingleObject(hSnifferThread, 3134);
    TerminateThread(hSnifferThread, 0);

    CloseHandle(hSnifferThread);
    CloseHandle(hIocp);

    return 0;
}

#pragma comment(linker, "\"/manifestdependency:type='Win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
