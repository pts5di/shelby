/*++

Copyright (c) 1990-2000  Microsoft Corporation

Module Name:

    driver.c

Abstract:

    This driver demonstrates use of a default I/O Queue, its
    request start events, cancellation event, and a synchronized DPC.

    To demonstrate asynchronous operation, the I/O requests are not completed
    immediately, but stored in the drivers private data structure, and a timer
    DPC will complete it next time the DPC runs.

    During the time the request is waiting for the DPC to run, it is
    made cancellable by the call WdfRequestMarkCancelable. This
    allows the test program to cancel the request and exit instantly.

    This rather complicated set of events is designed to demonstrate
    the driver frameworks synchronization of access to a device driver
    data structure, and a pointer which can be a proxy for device hardware
    registers or resources.

    This common data structure, or resource is accessed by new request
    events arriving, the DPC that completes it, and cancel processing.

    Notice the lack of specific lock/unlock operations.

    Even though this example utilizes a serial queue, a parallel queue
    would not need any additional explicit synchronization, just a
    strategy for managing multiple requests outstanding.

--*/

#include "driver.h"


#ifdef ALLOC_PRAGMA
#pragma alloc_text (INIT, DriverEntry)
#pragma alloc_text (INIT, EchoPrintDriverVersion)
#pragma alloc_text (PAGE, EchoEvtDeviceAdd)
#endif
#define IOCTL_OSR_INVERT_NOTIFICATION CTL_CODE(FILE_DEVICE_INVERTED, 2049, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define FILE_DEVICE_INVERTED 0xCF54
#define IOCTL_ADD_READER CTL_CODE(FILE_DEVICE_INVERTED, 2050, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define nullptr 0


NTSTATUS
DriverEntry(
    IN PDRIVER_OBJECT  DriverObject,
    IN PUNICODE_STRING RegistryPath
    )
/*++

Routine Description:
    DriverEntry initializes the driver and is the first routine called by the
    system after the driver is loaded. DriverEntry specifies the other entry
    points in the function driver, such as EvtDevice and DriverUnload.

Parameters Description:

    DriverObject - represents the instance of the function driver that is loaded
    into memory. DriverEntry must initialize members of DriverObject before it
    returns to the caller. DriverObject is allocated by the system before the
    driver is loaded, and it is released by the system after the system unloads
    the function driver from memory.

    RegistryPath - represents the driver specific path in the Registry.
    The function driver can use the path to store driver related data between
    reboots. The path does not store hardware instance specific data.

Return Value:

    STATUS_SUCCESS if successful,
    STATUS_UNSUCCESSFUL otherwise.

--*/
{
    WDF_DRIVER_CONFIG config;
    NTSTATUS status;

    WDF_DRIVER_CONFIG_INIT(&config,
                        EchoEvtDeviceAdd
                        );

    status = WdfDriverCreate(DriverObject,
                            RegistryPath,
                            WDF_NO_OBJECT_ATTRIBUTES,
                            &config,
                            WDF_NO_HANDLE);
    if (!NT_SUCCESS(status)) {
        KdPrint(("Error: WdfDriverCreate failed 0x%x\n", status));
        return status;
    }

#if DBG
    EchoPrintDriverVersion();
#endif

    return status;
}

NTSTATUS
EchoEvtDeviceAdd(
    IN WDFDRIVER       Driver,
    IN PWDFDEVICE_INIT DeviceInit
    )
/*++
Routine Description:

    EvtDeviceAdd is called by the framework in response to AddDevice
    call from the PnP manager. We create and initialize a device object to
    represent a new instance of the device.

Arguments:

    Driver - Handle to a framework driver object created in DriverEntry

    DeviceInit - Pointer to a framework-allocated WDFDEVICE_INIT structure.

Return Value:

    NTSTATUS

--*/
{
    NTSTATUS status;

    UNREFERENCED_PARAMETER(Driver);

    PAGED_CODE();

    KdPrint(("Enter  EchoEvtDeviceAdd\n"));

    status = EchoDeviceCreate(DeviceInit);

    return status;
}

NTSTATUS
EchoPrintDriverVersion(
    )
/*++
Routine Description:

   This routine shows how to retrieve framework version string and
   also how to find out to which version of framework library the
   client driver is bound to.

Arguments:

Return Value:

    NTSTATUS

--*/
{
    NTSTATUS status;
    WDFSTRING string;
    UNICODE_STRING us;
    WDF_DRIVER_VERSION_AVAILABLE_PARAMS ver;

    //
    // 1) Retreive version string and print that in the debugger.
    //
    status = WdfStringCreate(NULL, WDF_NO_OBJECT_ATTRIBUTES, &string);
    if (!NT_SUCCESS(status)) {
        KdPrint(("Error: WdfStringCreate failed 0x%x\n", status));
        return status;
    }

    status = WdfDriverRetrieveVersionString(WdfGetDriver(), string);
    if (!NT_SUCCESS(status)) {
        //
        // No need to worry about delete the string object because
        // by default it's parented to the driver and it will be
        // deleted when the driverobject is deleted when the DriverEntry
        // returns a failure status.
        //
        KdPrint(("Error: WdfDriverRetrieveVersionString failed 0x%x\n", status));
        return status;
    }

    WdfStringGetUnicodeString(string, &us);
    KdPrint(("Echo Sample %wZ\n", &us));

    WdfObjectDelete(string);
    string = NULL; // To avoid referencing a deleted object.

    //
    // 2) Find out to which version of framework this driver is bound to.
    //
    WDF_DRIVER_VERSION_AVAILABLE_PARAMS_INIT(&ver, 1, 0);
    if (WdfDriverIsVersionAvailable(WdfGetDriver(), &ver) == TRUE) {
        KdPrint(("Yes, framework version is 1.0\n"));
    }else {
        KdPrint(("No, framework verison is not 1.0\n"));
    }

    return STATUS_SUCCESS;
}

VOID
InvertedEvtIoDeviceControl(WDFQUEUE Queue,
    WDFREQUEST Request,
    size_t OutputBufferLength,
    size_t InputBufferLength,
    ULONG IoControlCode)
{
    PDEVICE_CONTEXT devContext;
    NTSTATUS status;
    ULONG_PTR info;
    PULONG bufferPointer;
    ULONG readerId;
    ULONG messageId;

    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);

    devContext = WdfObjectGet_DEVICE_CONTEXT(
        WdfIoQueueGetDevice(Queue));

    //
    // Set the default completion status and information field
    // 
    status = STATUS_INVALID_PARAMETER;
    info = 0;

#if DBG
    DbgPrint("InvertedEvtIoDeviceControl\n");
#endif

    switch (IoControlCode) {


    case IOCTL_ADD_READER: {
        
        if (OutputBufferLength < sizeof(LONG)) {

            break;
        }

        status = WdfRequestRetrieveOutputBuffer(Request,
            sizeof(ULONG)*2,
            (PVOID*)&bufferPointer,
            nullptr);
        //
        // Valid OutBuffer?
        // 
        if (!NT_SUCCESS(status)) {

            //
            // The OutBuffer associated with the pending notification Request that
            // we just dequeued is somehow not valid. This doesn't really seem
            // possible, but... you know... they return you a status, you have to
            // check it and handle it.
            // 
#if DBG
            DbgPrint("InvertedEvtIoDeviceControl: WdfRequestRetrieveOutputBuffer failed.  Status = 0x%0x\n",
                status);
#endif

            //
            // Complete the IOCTL_OSR_INVERT_NOTIFICATION with success, but
            // indicate that we're not returning any additional information.
            // 
            status = STATUS_UNSUCCESSFUL;
            info = 0;

        }
        else {
            // READER ID
            bufferPointer[0] = 0;

            //MESSAGE ID
            bufferPointer[1] = 0;
        }

        WdfRequestComplete(Request, status);


        if (!NT_SUCCESS(status)) {
            break;
        }

        //
        // *** RETURN HERE WITH REQUEST PENDING ***
        //     We do not break, we do not fall through.
        //
        return;
    }

        //
        // This IOCTL are sent by the user application, and will be completed
        // by the driver when an event occurs.
        // 
    case IOCTL_OSR_INVERT_NOTIFICATION: {

        //
        // We return an 32-bit value with each completion notification.
        // Be sure the user's data buffer is at least long enough for that.
        // 
        if (OutputBufferLength < sizeof(LONG)) {

            //
            // Not enough space? Complete the request with
            // STATUS_INVALID_PARAMETER (as set previously).
            // 
            break;
        }

        if (InputBufferLength < sizeof(ULONG) * 2) {

            break;
        }

        status = WdfRequestRetrieveInputBuffer(Request,
            sizeof(ULONG) * 2,
            (PVOID*)&bufferPointer,
            nullptr);
        //
        // Valid OutBuffer?
        // 
        if (!NT_SUCCESS(status)) {

            //
            // The InBuffer associated with the pending notification Request that
            // we just dequeued is somehow not valid. This doesn't really seem
            // possible, but... you know... they return you a status, you have to
            // check it and handle it.
            // 
#if DBG
            DbgPrint("InvertedEvtIoDeviceControl: WdfRequestRetrieveInputBuffer failed.  Status = 0x%0x\n",
                status);
#endif

            //
            // Complete the IOCTL_OSR_INVERT_NOTIFICATION with success, but
            // indicate that we're not returning any additional information.
            // 
            status = STATUS_UNSUCCESSFUL;
            info = 0;

        }
        else {
            // READER ID
            readerId = bufferPointer[0];

            KdPrint(("readerId = %u\n", readerId));

            //MESSAGE ID
            messageId = bufferPointer[1];
            KdPrint(("messageId = %u\n", messageId));


        }
        
        status = WdfRequestForwardToIoQueue(Request,
            devContext->NotificationQueue);

        //
        // If we can't forward the Request to our holding queue,
        // we have to complete it.  We'll use whatever status we get
        // back from WdfRequestForwardToIoQueue.
        // 
        if (!NT_SUCCESS(status)) {
            break;
        }

        //
        // *** RETURN HERE WITH REQUEST PENDING ***
        //     We do not break, we do not fall through.
        //
        return;
    }

    default: {
#if DBG
        DbgPrint("InvertedEvtIoDeviceControl: Invalid IOCTL received\n");
#endif
        break;
    }

    }

    //
    // Complete the received Request
    // 
    WdfRequestCompleteWithInformation(Request,
        status,
        info);
}
