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
};

static enum
{
    MI_ABOUT = 11,
    MI_USEINFO,
    MI_QUIT,
};

struct HostInfo
{
    char szHostAddress[64];
    DWORD dwAddress;
    USHORT usPort;
    BOOL bActive;
};

static struct HostInfo g_hosts[1024];
static int g_nHostsCount = 0;
static HWND g_hMainWnd = NULL;

static char g_szDomainName[64] = "";
static DWORD g_dwAddress = 0;
static USHORT g_usPort = 0;

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

static LRESULT __stdcall NotifyWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    static NOTIFYICONDATA nid = {sizeof(nid)};
    static HICON hIco = NULL;
    static UINT WM_REBUILDTOOLBAR = 0;
    static BOOL bShowInfo = TRUE;

    if (uMsg == WM_CREATE)
    {
        LPCREATESTRUCT lpCs = (LPCREATESTRUCT)lParam;

        hIco = LoadIcon(lpCs->hInstance, MAKEINTRESOURCE(IDI_MAINFRAME));

        WM_REBUILDTOOLBAR = RegisterWindowMessage(TEXT("TaskbarCreated"));

        //add icon to tray
        nid.hWnd = hWnd;
        nid.uID = 1;
        nid.uCallbackMessage = WM_CALLBACK;
        nid.hIcon = hIco;
        nid.uFlags = NIF_TIP | NIF_MESSAGE | NIF_ICON;
        lstrcpyn(nid.szInfoTitle, TEXT("信息"), sizeof(nid.szTip));
        nid.uTimeout = 3000;
        nid.dwInfoFlags = NIIF_INFO;

        Shell_NotifyIcon(NIM_ADD, &nid);

        //
        PostMessage(hWnd, WM_REPORT, FALSE, FALSE);

        return 0;
    }
    else if (uMsg == WM_REPORT)
    {
        BOOL bActive = (BOOL)wParam;
        BOOL bShowInfoThisTime = (BOOL)lParam;

        nid.uFlags = NIF_TIP;

        wnsprintf(
            nid.szTip,
            RTL_NUMBER_OF(nid.szTip),
            TEXT("域名：%hs\r\n")
            TEXT("IP地址：%hs\r\n")
            TEXT("端口：%u\r\n")
            TEXT("当前状态：%s\r\n"),
            g_szDomainName,
            inet_ntoa(*(struct in_addr *)&g_dwAddress),
            g_usPort,
            bActive ? TEXT("活动") : TEXT("不活动")
            );

        if (bShowInfoThisTime && bShowInfo)
        {
            wnsprintf(
                nid.szInfo,
                RTL_NUMBER_OF(nid.szInfo),
                TEXT("主机“%hs:%u”已进入%s活动状态。"),
                lstrlenA(g_szDomainName) > 0 ? g_szDomainName : inet_ntoa(*(struct in_addr *)&g_dwAddress),
                g_usPort,
                bActive ? TEXT("") : TEXT("不")
                );

            nid.uFlags |= NIF_INFO;
        }

        Shell_NotifyIcon(NIM_MODIFY, &nid);
    }
    else if (uMsg == WM_CLOSE)
    {
        Shell_NotifyIcon(NIM_DELETE, &nid);
        DestroyWindow(hWnd);
    }
    else if (uMsg == WM_DESTROY)
    {
        PostQuitMessage(0);
    }
    else if (uMsg == WM_CALLBACK)
    {
        if (lParam == WM_RBUTTONUP)
        {
            POINT pt;
            HMENU hMenu = CreatePopupMenu();

            GetCursorPos(&pt);

            SetForegroundWindow(hWnd);

            AppendMenu(hMenu, MF_STRING, MI_ABOUT, TEXT("关于(&A)..."));
            AppendMenu(hMenu, MF_SEPARATOR, 0, NULL);
            AppendMenu(hMenu, MF_STRING, MI_USEINFO, TEXT("启用气泡通知(&B)"));
            AppendMenu(hMenu, MF_STRING, MI_QUIT, TEXT("退出(&Q)"));

            if (bShowInfo)
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
                bShowInfo = !bShowInfo;
                break;

            case MI_QUIT:
                PostMessage(hWnd, WM_CLOSE, 0, 0);
                break;

            default:
                break;
            }

            DestroyMenu(hMenu);
        }
    }
    else if (uMsg == WM_REBUILDTOOLBAR)
    {
        Shell_NotifyIcon(NIM_ADD, &nid);
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

        for (; index < (argc - 1) / 2; ++index)
        {
            LPCWSTR lpAddress = argv[index * 2 + 1];
            LPCWSTR lpPort = argv[index * 2 + 2];
            USHORT usPort = 0;
            char szDomainNameOrIpAddress[1024];
            DWORD dwAddress = 0;

            if (!IsPortW(lpPort, &usPort))
            {
                MessageBoxFormat(NULL, NULL, MB_SYSTEMMODAL | MB_ICONERROR, TEXT("“%ls”不是合法的端口号"), lpPort);
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
    int nIndex = 0;
    LPFN_CONNECTEX pConnectEx = NULL;

    for (; nIndex < g_nHostsCount; nIndex += 1)
    {
        sin.sin_addr.s_addr = g_hosts[nIndex].dwAddress;
        sin.sin_port = htons(g_hosts[nIndex].usPort);


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

    PostQueuedCompletionStatus(hIocp, 0, 0, NULL);

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