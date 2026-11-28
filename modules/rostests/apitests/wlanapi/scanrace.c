/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Races scan ioctls against vwifi removal to reproduce the
 *              nwifi ioctl-vs-detach use-after-free
 * COPYRIGHT:   Copyright 2026 LiberNT contributors
 */

#include <apitest.h>

#include <winsock2.h>
#include <wlanapi.h>
#include <setupapi.h>

#include "vwifi.h"

#define RACE_ITERATIONS 25

typedef struct _SCAN_THREAD
{
    HANDLE Client;
    GUID Interface;
    volatile LONG Stop;
    volatile LONG ScansIssued;
    volatile LONG ScanErrors;
} SCAN_THREAD, *PSCAN_THREAD;

static DWORD WINAPI
ScanThreadProc(PVOID Parameter)
{
    PSCAN_THREAD Context = Parameter;
    DWORD Error;

    while (!Context->Stop)
    {
        Error = WlanScan(Context->Client, &Context->Interface, NULL, NULL, NULL);
        if (Error == ERROR_SUCCESS)
        {
            InterlockedIncrement(&Context->ScansIssued);
        }
        else
        {
            InterlockedIncrement(&Context->ScanErrors);
            /* The interface is going away or already gone: a few errors are
             * expected during teardown, continuous ones mean we are done. */
            if (Context->ScanErrors > 20)
                break;
        }
        Sleep(0);
    }
    return 0;
}

typedef struct _CONNECT_THREAD
{
    HANDLE Client;
    GUID Interface;
    volatile LONG Stop;
    volatile LONG ConnectsIssued;
    volatile LONG ConnectErrors;
} CONNECT_THREAD, *PCONNECT_THREAD;

static const WCHAR RaceProfileXml[] =
    L"<?xml version=\"1.0\"?>"
    L"<WLANProfile xmlns=\"http://www.microsoft.com/networking/WLAN/profile/v1\">"
    L"<name>racenet</name>"
    L"<SSIDConfig><SSID><name>racenet</name></SSID></SSIDConfig>"
    L"<connectionType>ESS</connectionType>"
    L"<connectionMode>manual</connectionMode>"
    L"<MSM><security><authEncryption><authentication>open</authentication>"
    L"<encryption>none</encryption><useOneX>false</useOneX></authEncryption></security></MSM>"
    L"</WLANProfile>";

static DWORD WINAPI
ConnectThreadProc(PVOID Parameter)
{
    PCONNECT_THREAD Context = Parameter;
    WLAN_CONNECTION_PARAMETERS Params;
    DWORD Error;

    Error = WlanSetProfile(Context->Client, &Context->Interface, 0,
                           RaceProfileXml, NULL, TRUE, NULL, NULL);
    if (Error != ERROR_SUCCESS)
    {
        InterlockedExchange(&Context->ConnectErrors, 1);
        return 0;
    }

    while (!Context->Stop)
    {
        ZeroMemory(&Params, sizeof(Params));
        Params.wlanConnectionMode = wlan_connection_mode_profile;
        Params.strProfile = L"racenet";
        Params.dot11BssType = dot11_BSS_type_infrastructure;
        Error = WlanConnect(Context->Client, &Context->Interface, &Params, NULL);
        if (Error == ERROR_SUCCESS)
        {
            InterlockedIncrement(&Context->ConnectsIssued);
        }
        else
        {
            if (InterlockedIncrement(&Context->ConnectErrors) > 20)
                break;
        }
        Sleep(0);
    }
    return 0;
}

START_TEST(WlanScanDetachRace)
{
    SP_DEVINFO_DATA Device;
    HDEVINFO Devices = INVALID_HANDLE_VALUE;
    DWORD NegotiatedVersion;
    HANDLE Client;
    GUID Interface;
    DWORD Error;
    DWORD Iteration;
    DWORD CompletedIterations = 0;
    ULONG TotalScans = 0;
    ULONG TotalConnects = 0;

    Error = WlanOpenHandle(WLAN_API_VERSION_2_0, NULL, &NegotiatedVersion, &Client);
    if (Error == ERROR_SERVICE_NOT_ACTIVE)
    {
        skip("WlanSvc is not running\n");
        return;
    }
    ok(Error == ERROR_SUCCESS, "WlanOpenHandle failed: %lu\n", Error);
    if (Error != ERROR_SUCCESS)
        return;

    /*
     * Each iteration installs the vwifi adapter, hammers it with scan ioctls
     * and removes the adapter while the ioctls are in flight. Without
     * reference counting in the nwifi ioctl path the detach frees the adapter
     * while an ioctl still uses it, crashing the kernel or hanging the scan;
     * either one ends this test process, so every finished iteration is
     * evidence the race is closed.
     */
    for (Iteration = 0; Iteration < RACE_ITERATIONS; Iteration++)
    {
        SCAN_THREAD Context;
        CONNECT_THREAD ConnectContext;
        HANDLE Thread;
        HANDLE ConnectThread;

        if (!InstallVwifi(&Devices, &Device))
        {
            skip("Could not install the vwifi adapter\n");
            break;
        }
        if (!WaitInterface(Client, Devices, &Device, &Interface))
        {
            ok(FALSE, "Wireless interface never appeared\n");
            RemoveVwifi(Devices, &Device);
            break;
        }

        Context.Client = Client;
        Context.Interface = Interface;
        Context.Stop = 0;
        Context.ScansIssued = 0;
        Context.ScanErrors = 0;
        ConnectContext.Client = Client;
        ConnectContext.Interface = Interface;
        ConnectContext.Stop = 0;
        ConnectContext.ConnectsIssued = 0;
        ConnectContext.ConnectErrors = 0;
        Thread = CreateThread(NULL, 0, ScanThreadProc, &Context, 0, NULL);
        ok(Thread != NULL, "CreateThread failed: %lu\n", GetLastError());
        ConnectThread = CreateThread(NULL, 0, ConnectThreadProc, &ConnectContext, 0, NULL);

        /* Vary the teardown delay so the detach lands at different points
         * of the scan pipeline. */
        Sleep((Iteration * 7) % 23);

        RemoveVwifi(Devices, &Device);

        if (Thread != NULL)
        {
            InterlockedExchange(&Context.Stop, 1);
            WaitForSingleObject(Thread, INFINITE);
            CloseHandle(Thread);
        }
        if (ConnectThread != NULL)
        {
            InterlockedExchange(&ConnectContext.Stop, 1);
            WaitForSingleObject(ConnectThread, INFINITE);
            CloseHandle(ConnectThread);
        }
        TotalScans += Context.ScansIssued;
        TotalConnects += ConnectContext.ConnectsIssued;
        CompletedIterations++;
    }

    ok(CompletedIterations == RACE_ITERATIONS,
       "Completed %lu of %u race iterations\n", CompletedIterations, RACE_ITERATIONS);
    trace("Issued %lu scan and %lu connect ioctls across %lu iterations\n",
          TotalScans, TotalConnects, CompletedIterations);

    WlanCloseHandle(Client, NULL);
}
