#define _WIN32_IE 0x500

#include <WinSock2.h>
#include <MSTcpIP.h>
#include <Windows.h>
#include <Shlwapi.h>
#include <tchar.h>
#include "resource.h"

#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "Shlwapi.lib")

#define NOTIFYWNDCLASS TEXT("__slx_HostStatusChecker_single_20131121")

static enum
{
    WM_CALLBACK = WM_USER + 112,
    WM_REPORT,
};

static enum
{
    MI_ABOUT = 11,
    MI_USEINFO,
    MI_RESTART,
    MI_QUIT,
};

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

    lstrcpynA(szAddress, inet_ntoa(*(in_addr *)&dwAddress), RTL_NUMBER_OF(szAddress));

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

static void RunSelf()
{
    TCHAR szImagePath[MAX_PATH];
    TCHAR szCmdLine[MAX_PATH + 4096];
    STARTUPINFO si = {sizeof(si)};
    PROCESS_INFORMATION pi;

    GetModuleFileName(GetModuleHandle(NULL), szImagePath, RTL_NUMBER_OF(szImagePath));
    wnsprintf(szCmdLine, RTL_NUMBER_OF(szCmdLine), TEXT("\"%s\" %hs %u"), szImagePath, g_szDomainName, g_usPort);

    if (CreateProcess(NULL, szCmdLine, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi))
    {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
}

static LRESULT __stdcall NotifyWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    static NOTIFYICONDATA nid = {sizeof(nid)};
    static HICON hGreenIco = NULL;
    static HICON hGrayIco = NULL;
    static UINT WM_REBUILDTOOLBAR = 0;
    static BOOL bShowInfo = TRUE;

    if (uMsg == WM_CREATE)
    {
        LPCREATESTRUCT lpCs = (LPCREATESTRUCT)lParam;

        hGreenIco = LoadIcon(lpCs->hInstance, MAKEINTRESOURCE(IDI_GREEN));
        hGrayIco = LoadIcon(lpCs->hInstance, MAKEINTRESOURCE(IDI_GRAY));

        WM_REBUILDTOOLBAR = RegisterWindowMessage(TEXT("TaskbarCreated"));

        //add icon to tray
        nid.hWnd = hWnd;
        nid.uID = 1;
        nid.uCallbackMessage = WM_CALLBACK;
        nid.hIcon = hGreenIco;
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
        UINT uOldFlags = nid.uFlags;

        nid.uFlags |= (NIF_ICON | NIF_TIP);

        wnsprintf(
            nid.szTip,
            RTL_NUMBER_OF(nid.szTip),
            TEXT("域名：%hs\r\n")
            TEXT("IP地址：%hs\r\n")
            TEXT("端口：%u\r\n")
            TEXT("当前状态：%s\r\n"),
            g_szDomainName,
            inet_ntoa(*(in_addr *)&g_dwAddress),
            g_usPort,
            bActive ? TEXT("活动") : TEXT("不活动")
            );

        if (bActive)
        {
            nid.hIcon = hGreenIco;
        }
        else
        {
            nid.hIcon = hGrayIco;
        }

        if (bShowInfoThisTime && bShowInfo)
        {
            wnsprintf(
                nid.szInfo,
                RTL_NUMBER_OF(nid.szInfo),
                TEXT("主机“%hs:%u”已进入%s活动状态。"),
                lstrlenA(g_szDomainName) > 0 ? g_szDomainName : inet_ntoa(*(in_addr *)&g_dwAddress),
                g_usPort,
                bActive ? TEXT("") : TEXT("不")
                );

            nid.uFlags |= NIF_INFO;
        }

        Shell_NotifyIcon(NIM_MODIFY, &nid);
        nid.uFlags = uOldFlags;
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

            GetCursorPos(&pt);

            SetForegroundWindow(hWnd);

            HMENU hMenu = CreatePopupMenu();

            AppendMenu(hMenu, MF_STRING, MI_ABOUT, TEXT("关于(&A)..."));
            AppendMenu(hMenu, MF_SEPARATOR, 0, NULL);
            AppendMenu(hMenu, MF_STRING, MI_USEINFO, TEXT("启用气泡通知(&B)"));
            AppendMenu(hMenu, MF_STRING, MI_RESTART, TEXT("重新启动(&R)"));
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

            case MI_RESTART:
                RunSelf();

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
        MessageBox(NULL, TEXT("获取主机信息失败，请加参数运行\r\n\r\n第一个参数为主机地址或ip\r\n第二个参数为端口"), NULL, MB_SYSTEMMODAL | MB_ICONERROR);
    }
    else
    {
        if (!IsPortW(argv[2], &g_usPort))
        {
            MessageBoxFormat(NULL, NULL, MB_SYSTEMMODAL | MB_ICONERROR, TEXT("“%ls”不是合法的端口号"), argv[2]);
        }
        else
        {
            char szDomainNameOrIpAddress[1024];

            wnsprintfA(szDomainNameOrIpAddress, sizeof(szDomainNameOrIpAddress), "%S", argv[1]);

            if (IsIpAddress(szDomainNameOrIpAddress))
            {
                g_dwAddress = inet_addr(szDomainNameOrIpAddress);
                bSuccess = TRUE;
            }
            else
            {
                struct hostent *pHostEnt = gethostbyname(szDomainNameOrIpAddress);

                if (pHostEnt == NULL)
                {
                    MessageBoxFormat(NULL, NULL, MB_SYSTEMMODAL | MB_ICONERROR, TEXT("“%ls”无法正确解析"), argv[1]);
                }
                else
                {
                    g_dwAddress = *(DWORD *)pHostEnt->h_addr_list[0];
                    lstrcpynA(g_szDomainName, szDomainNameOrIpAddress, sizeof(g_szDomainName));
                    bSuccess = TRUE;
                }
            }
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
    BOOL bActive = FALSE;
    HWND hMainWindow = (HWND)lpParam;
    sockaddr_in sin;

    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = g_dwAddress;
    sin.sin_port = htons(g_usPort);

    if (!IsWindow(hMainWindow))
    {
        return 0;
    }
 
    while (TRUE)
    {
        SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

        while (sock == INVALID_SOCKET && IsWindow(hMainWindow))
        {
            Sleep(3000);

            sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        }

        if (!IsWindow(hMainWindow))
        {
            closesocket(sock);
            break;
        }

        int ret = connect(sock, (const sockaddr *)&sin, sizeof(sin));

        if (!IsWindow(hMainWindow))
        {
            closesocket(sock);
            break;
        }

        if (ret != 0)
        {
            if (bActive)
            {
                bActive = FALSE;
                PostMessage(hMainWindow, WM_REPORT, bActive, TRUE);
            }

            Sleep(3000);
        }
        else
        {
            if (!bActive)
            {
                bActive = TRUE;
                PostMessage(hMainWindow, WM_REPORT, bActive, TRUE);
            }

            SetSockKeepAlive(sock, 1, 3000, 5000);

            while (IsWindow(hMainWindow))
            {
                char szBuffer[4096];

                ret = recv(sock, szBuffer, sizeof(szBuffer), 0);

                if (ret <= 0)
                {
                    break;
                }
            }
        }

        closesocket(sock);
    }

    return 0;
}

int APIENTRY _tWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPTSTR lpCmdLine, int nShowCmd)
{
    if (!InitWinSock(MAKEWORD(2, 2)))
    {
        MessageBox(NULL, TEXT("初始化网络失败"), NULL, MB_SYSTEMMODAL | MB_ICONERROR);
        return 0;
    }

    if (!GetHostInformation())
    {
        return 0;
    }

    WNDCLASSEX wcex = {sizeof(wcex)};
    wcex.lpfnWndProc = NotifyWndProc;
    wcex.lpszClassName = NOTIFYWNDCLASS;
    wcex.hInstance = hInstance;
    wcex.style = CS_HREDRAW | CS_VREDRAW;

    RegisterClassEx(&wcex);

    HWND hMainWnd = CreateWindowEx(
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

//     ShowWindow(hMainWnd, SW_SHOW);
//     UpdateWindow(hMainWnd);

    HANDLE hThread = CreateThread(NULL, 0, SnifferProc, (LPVOID)hMainWnd, 0, NULL);

    MSG msg;

    while (TRUE)
    {
        int nRet = GetMessage(&msg, NULL, 0, 0);

        if (nRet <= 0)
        {
            break;
        }

        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    //工作线程不管了

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