#define _WIN32_IE 0x500

#include <WinSock2.h>
#include <MSTcpIP.h>
#include <MSWSock.h>
#include <Windows.h>
#include <Shlwapi.h>
#include <tchar.h>
#include "resource.h"

#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "Shlwapi.lib")

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
    enum IoType type;
    WSABUF      buf;
    char        szHostAddress[64];
    DWORD       dwAddress;
    USHORT      usPort;
    BOOL        bActive;
    char        buffer[4096];
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

DWORD GetDllVersion(LPCTSTR lpszDllName)
{
    HINSTANCE hinstDll = GetModuleHandle(lpszDllName);
    DWORD dwVersion = 0;

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
                dwVersion = MAKELONG(dvi.dwMajorVersion, dvi.dwMinorVersion);
            }
        }
    }

    return dwVersion;
}

int GetNotifyIconDataSize()
{
    ULONGLONG dwShell32Version = GetDllVersion(TEXT("Shell32.dll"));

    if (dwShell32Version >= MAKEDLLVERULL(6, 0, 0, 0))
    {
        return sizeof(NOTIFYICONDATA);
    }
    else if(dwShell32Version >= MAKEDLLVERULL(5, 0, 0, 0))
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
    static NOTIFYICONDATA s_nid = {sizeof(s_nid)};
    static BOOL s_bShowInfo = TRUE;

    switch (uMsg)
    {
    case WM_CREATE:
        s_nid.cbSize = GetNotifyIconDataSize();
        s_nid.hWnd = hWnd;
        s_nid.uID = 1;
        s_nid.uCallbackMessage = WM_CALLBACK;
        s_nid.hIcon = LoadIcon(((LPCREATESTRUCT)lParam)->hInstance, MAKEINTRESOURCE(IDI_MAINFRAME));
        s_nid.uFlags = NIF_TIP | NIF_MESSAGE | NIF_ICON;
        lstrcpyn(s_nid.szInfoTitle, TEXT("信息"), RTL_NUMBER_OF(s_nid.szInfoTitle));
        s_nid.uTimeout = 3000;
        s_nid.dwInfoFlags = NIIF_INFO;
        SendMessage(hWnd, WM_UPDATETIP, 0, 0);

        Shell_NotifyIcon(NIM_ADD, &s_nid);

        WM_REBUILDTOOLBAR = RegisterWindowMessage(TEXT("TaskbarCreated"));
        return 0;

    case WM_CLOSE:
        Shell_NotifyIcon(NIM_DELETE, &s_nid);
        DestroyWindow(hWnd);
        return 0;
        
    case WM_DESTROY:
        PostQuitMessage(0);
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

    case WM_CALLBACK:
        if (lParam == WM_LBUTTONUP)
        {
            //显示详细信息
        }
        else if (lParam == WM_RBUTTONUP)
        {
            POINT pt;
            HMENU hMenu = CreatePopupMenu();

            GetCursorPos(&pt);

            SetForegroundWindow(hWnd);

            AppendMenu(hMenu, MF_STRING, MI_ABOUT, TEXT("关于(&A)..."));
            AppendMenu(hMenu, MF_SEPARATOR, 0, NULL);
            AppendMenu(hMenu, MF_STRING, MI_USEINFO, TEXT("启用气泡通知(&B)"));
            AppendMenu(hMenu, MF_STRING, MI_QUIT, TEXT("退出(&Q)"));

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
                TEXT("主机“%hs:%u”已进入%s活动状态。"),
                g_hosts[dwIndex].szHostAddress,
                g_hosts[dwIndex].usPort,
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

DWORD __stdcall SnifferProc(LPVOID lpParam)
{
    HANDLE hIocp = (HANDLE)lpParam;
    struct sockaddr_in sin = {AF_INET};
    LPFN_CONNECTEX pConnectEx = NULL;
    int nIndex = 0;

    for (; nIndex < g_nHostsCount; nIndex += 1)
    {
        g_hosts[nIndex].index = nIndex;
        g_hosts[nIndex].buf.buf = g_hosts[nIndex].buffer;
        g_hosts[nIndex].buf.len = sizeof(g_hosts[nIndex].buffer);
        g_hosts[nIndex].sock = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
        g_hosts[nIndex].type = IT_CONNECT;

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

        sin.sin_addr.s_addr = g_hosts[nIndex].dwAddress;
        sin.sin_port = htons(g_hosts[nIndex].usPort);

        CreateIoCompletionPort((HANDLE)g_hosts[nIndex].sock, hIocp, CK_NORMAL, 0);
        g_hosts[nIndex].ol.hEvent = WSACreateEvent();
        pConnectEx(
            g_hosts[nIndex].sock,
            (const struct sockaddr *)&sin,
            sizeof(sin),
            NULL,
            0,
            NULL,
            &g_hosts[nIndex].ol
            );
    }

    while (TRUE)
    {
        DWORD dwBytesTransferred = 0;
        ULONG_PTR ulCompletionKey = 0;
        struct HostInfo *pHostInfo = NULL;
        BOOL bIoSucceed = GetQueuedCompletionStatus(hIocp, &dwBytesTransferred, &ulCompletionKey, (LPOVERLAPPED *)&pHostInfo, INFINITE);

        if (dwBytesTransferred == 0 && ulCompletionKey == CK_QUIT && pHostInfo != NULL)
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
    }

    for (nIndex = 0; nIndex < g_nHostsCount; nIndex += 1)
    {
        CancelIo((HANDLE)g_hosts[nIndex].sock);
        closesocket(g_hosts[nIndex].sock);
    }


//     BOOL bActive = FALSE;
//     HWND hMainWindow = (HWND)lpParam;
//     struct sockaddr_in sin;
// 
//     sin.sin_family = AF_INET;
//     sin.sin_addr.s_addr = g_dwAddress;
//     sin.sin_port = htons(g_usPort);
// 
//     if (!IsWindow(hMainWindow))
//     {
//         return 0;
//     }
//  
//     while (TRUE)
//     {
//         int ret = 0;
//         SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
// 
//         while (sock == INVALID_SOCKET && IsWindow(hMainWindow))
//         {
//             Sleep(3000);
// 
//             sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
//         }
// 
//         if (!IsWindow(hMainWindow))
//         {
//             closesocket(sock);
//             break;
//         }
// 
//         ret = connect(sock, (const struct sockaddr *)&sin, sizeof(sin));
// 
//         if (!IsWindow(hMainWindow))
//         {
//             closesocket(sock);
//             break;
//         }
// 
//         if (ret != 0)
//         {
//             if (bActive)
//             {
//                 bActive = FALSE;
//                 PostMessage(hMainWindow, WM_REPORT, bActive, TRUE);
//             }
// 
//             Sleep(3000);
//         }
//         else
//         {
//             if (!bActive)
//             {
//                 bActive = TRUE;
//                 PostMessage(hMainWindow, WM_REPORT, bActive, TRUE);
//             }
// 
//             SetSockKeepAlive(sock, 1, 3000, 5000);
// 
//             while (IsWindow(hMainWindow))
//             {
//                 char szBuffer[4096];
// 
//                 ret = recv(sock, szBuffer, sizeof(szBuffer), 0);
// 
//                 if (ret <= 0)
//                 {
//                     break;
//                 }
//             }
//         }
// 
//         closesocket(sock);
//     }
// 
//     return 0;
}

int APIENTRY _tWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPTSTR lpCmdLine, int nShowCmd)
{
    HANDLE hIocp = NULL;
    HANDLE hSnifferThread = NULL;
    WNDCLASSEX wcex = {sizeof(wcex)};

    if (!InitWinSock(MAKEWORD(2, 2)))
    {
        MessageBox(NULL, TEXT("初始化网络失败"), NULL, MB_SYSTEMMODAL | MB_ICONERROR);
        return 0;
    }

    if (!GetHostInformation())
    {
        return 0;
    }

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
        NULL
        );

    ShowWindow(g_hMainWnd, SW_SHOW);
    UpdateWindow(g_hMainWnd);

    hIocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
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

#if _MSC_VER <= 1200
#ifndef _DEBUG

#pragma comment(linker, "/Entry:s")
#pragma comment(linker, "/Opt:nowin98")

void s()
{
    ExitProcess(_tWinMain(GetModuleHandle(NULL), NULL, NULL, SW_SHOW));
}

#endif
#endif