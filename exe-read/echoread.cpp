/*++

Copyright (c) Microsoft Corporation

Module Name:

    EchoApp.cpp

Abstract:

    An application to exercise the WDF "echo" sample driver.


Environment:

    user mode only

--*/


#include <DriverSpecs.h>
_Analysis_mode_(_Analysis_code_type_user_code_)

#define INITGUID

#include <windows.h>
#include <strsafe.h>
#include <cfgmgr32.h>
#include <stdio.h>
#include <stdlib.h>
#include "public.h"

#define NUM_ASYNCH_IO   100
#define BUFFER_SIZE     (40*1024)

#define READER_TYPE   1
#define WRITER_TYPE   2

#define MAX_DEVPATH_LENGTH                       256

typedef struct _OVL_WRAPPER {
    OVERLAPPED  Overlapped;
    CHAR        ReturnedSequence[512];
} OVL_WRAPPER, * POVL_WRAPPER;

BOOLEAN G_PerformAsyncIo;
BOOLEAN G_LimitedLoops;
ULONG G_AsyncIoLoopsNum;
WCHAR G_DevicePath[MAX_DEVPATH_LENGTH];


ULONG
AsyncIo(
    PVOID   ThreadParameter
    );

BOOL
GetDevicePath(
    IN  LPGUID InterfaceGuid,
    _Out_writes_(BufLen) PWCHAR DevicePath,
    _In_ size_t BufLen
    );


int __cdecl
main(
    _In_ int argc,
    _In_reads_(argc) char* argv[]
    )
{
    DWORD byteCount = 0;
    ULONG_PTR compKey = 0;
    OVERLAPPED* overlapped = nullptr;
    HANDLE driverHandle = INVALID_HANDLE_VALUE;
    HANDLE completionPortHandle = INVALID_HANDLE_VALUE;
    PCHAR stringToUse = "Hello, world!";
    DWORD   code;
    BOOLEAN result = TRUE;
    POVL_WRAPPER wrap;

    if (argc == 2) {
        stringToUse = argv[1];
        printf("%s\n", stringToUse);
    }


    if (!GetDevicePath(
        (LPGUID)&GUID_DEVINTERFACE_ECHO,
        G_DevicePath,
        sizeof(G_DevicePath) / sizeof(G_DevicePath[0])))
    {
        result = FALSE;
        goto exit;
    }

    printf("DevicePath: %ws\n", G_DevicePath);

    driverHandle = CreateFile(G_DevicePath,
                         GENERIC_READ|GENERIC_WRITE,
                         0,
                         NULL,
                         OPEN_EXISTING,
                         FILE_FLAG_OVERLAPPED,
                         NULL );

    if (driverHandle == INVALID_HANDLE_VALUE) {

        code = GetLastError();

        printf("CreateFile failed with error 0x%lx\n",
            code);

        return(code);
    }

    printf("Opened device successfully\n");
    completionPortHandle = CreateIoCompletionPort(driverHandle,
        nullptr,
        0,
        0);

    if (completionPortHandle == nullptr) {

        code = GetLastError();

        printf("CreateIoCompletionPort failed with error 0x%lx\n", code);
        printf("driverHandle: %llu\n", reinterpret_cast<ULONGLONG>(driverHandle));

        return(code);

    }
    //
   // Infinitely print out the list of choices, ask for input, process
   // the request
   //
    
    wrap = static_cast<POVL_WRAPPER>(malloc(sizeof(OVL_WRAPPER)));
    memset(wrap, 0, sizeof(OVL_WRAPPER));

    //
    // Test the IOCTL interface
    //
    DeviceIoControl(driverHandle,
        static_cast<DWORD>(IOCTL_OSR_INVERT_NOTIFICATION),
        nullptr,                      // Ptr to InBuffer
        0,                            // Length of InBuffer
        &wrap->ReturnedSequence,      // Ptr to OutBuffer
        sizeof(char)*512,                 // Length of OutBuffer
        nullptr,                      // BytesReturned
        &wrap->Overlapped);           // Ptr to Overlapped structure

    code = GetLastError();

    if (code != ERROR_IO_PENDING) {

        printf("DeviceIoControl failed with error 0x%lx\n", code);

        return(code);

    }

    if (!GetQueuedCompletionStatus(completionPortHandle,                // Completion port handle
        &byteCount,                // Bytes transferred
        &compKey,                  // Completion key... don't care
        &overlapped,               // OVERLAPPED structure
        1000000)) 
    {                  // Notification time-out interval
        code = GetLastError();

        printf("GetQueuedCompletionStatus failed with error 0x%lx\n", code);

        return(code);

    
    }   
    wrap = reinterpret_cast<POVL_WRAPPER>(overlapped);
    printf(">>> Notification received.\n");
    printf("Sequence = %s\n", wrap->ReturnedSequence);

exit:
    if (completionPortHandle != INVALID_HANDLE_VALUE) {
        CloseHandle(completionPortHandle);
    }

    return ((result == TRUE) ? 0 : 1);


}

DWORD
WINAPI CompletionPortThread(LPVOID PortHandle)
{
    DWORD byteCount = 0;
    ULONG_PTR compKey = 0;
    OVERLAPPED* overlapped = nullptr;
    POVL_WRAPPER wrap;
    DWORD code;

    while (TRUE) {

        // Wait for a completion notification.
        overlapped = nullptr;

        GetQueuedCompletionStatus(PortHandle,                // Completion port handle
            &byteCount,                // Bytes transferred
            &compKey,                  // Completion key... don't care
            &overlapped,               // OVERLAPPED structure
            500);                 // Notification time-out interval

        //
        // If it's our notification ioctl that's just been completed...
        // don't do anything special.
        // 
        if (byteCount == 0) {
            continue;
        }

        if (overlapped == nullptr) {

            // An unrecoverable error occurred in the completion port.
            // Wait for the next notification.
            continue;
        }

        //
        // Because the wrapper structure STARTS with the OVERLAPPED structure,
        // the pointers are the same.  It would be nicer to use
        // CONTAINING_RECORD here... however you do that in user-mode.
        // 
        wrap = reinterpret_cast<POVL_WRAPPER>(overlapped);

        code = GetLastError();

        printf(">>> Notification received.  Sequence = %s\n",
            wrap->ReturnedSequence);
    }
}





BOOL
GetDevicePath(
    _In_ LPGUID InterfaceGuid,
    _Out_writes_(BufLen) PWCHAR DevicePath,
    _In_ size_t BufLen
    )
{
    CONFIGRET cr = CR_SUCCESS;
    PWSTR deviceInterfaceList = NULL;
    ULONG deviceInterfaceListLength = 0;
    PWSTR nextInterface;
    HRESULT hr = E_FAIL;
    BOOL bRet = TRUE;

    cr = CM_Get_Device_Interface_List_Size(
                &deviceInterfaceListLength,
                InterfaceGuid,
                NULL,
                CM_GET_DEVICE_INTERFACE_LIST_PRESENT);
    if (cr != CR_SUCCESS) {
        printf("Error 0x%x retrieving device interface list size.\n", cr);
        goto clean0;
    }

    if (deviceInterfaceListLength <= 1) {
        bRet = FALSE;
        printf("Error: No active device interfaces found.\n"
            " Is the sample driver loaded?");
        goto clean0;
    }

    deviceInterfaceList = (PWSTR)malloc(deviceInterfaceListLength * sizeof(WCHAR));
    if (deviceInterfaceList == NULL) {
        printf("Error allocating memory for device interface list.\n");
        goto clean0;
    }
    ZeroMemory(deviceInterfaceList, deviceInterfaceListLength * sizeof(WCHAR));

    cr = CM_Get_Device_Interface_List(
                InterfaceGuid,
                NULL,
                deviceInterfaceList,
                deviceInterfaceListLength,
                CM_GET_DEVICE_INTERFACE_LIST_PRESENT);
    if (cr != CR_SUCCESS) {
        printf("Error 0x%x retrieving device interface list.\n", cr);
        goto clean0;
    }

    nextInterface = deviceInterfaceList + wcslen(deviceInterfaceList) + 1;
    if (*nextInterface != UNICODE_NULL) {
        printf("Warning: More than one device interface instance found. \n"
            "Selecting first matching device.\n\n");
    }

    hr = StringCchCopy(DevicePath, BufLen, deviceInterfaceList);
    if (FAILED(hr)) {
        bRet = FALSE;
        printf("Error: StringCchCopy failed with HRESULT 0x%x", hr);
        goto clean0;
    }

clean0:
    if (deviceInterfaceList != NULL) {
        free(deviceInterfaceList);
    }
    if (CR_SUCCESS != cr) {
        bRet = FALSE;
    }

    return bRet;
}

