/*++

Copyright (c) 1990-2000  Microsoft Corporation

Module Name:

    queue.c

Abstract:

    This is a C version of a very simple sample driver that illustrates
    how to use the driver framework and demonstrates best practices.

--*/

#include "driver.h"

#ifdef ALLOC_PRAGMA
#pragma alloc_text (PAGE, EchoQueueInitialize)
#pragma alloc_text (PAGE, EchoTimerCreate)
#endif

#define nullptr 0


NTSTATUS
InvertedNotify(PDEVICE_CONTEXT DevContext, IN WDFMEMORY Memory, IN size_t Length);

NTSTATUS
EchoQueueInitialize(
    WDFDEVICE Device
    )
/*++

Routine Description:


     The I/O dispatch callbacks for the frameworks device object
     are configured in this function.

     A single default I/O Queue is configured for serial request
     processing, and a driver context memory allocation is created
     to hold our structure QUEUE_CONTEXT.

     This memory may be used by the driver automatically synchronized
     by the Queue's presentation lock.

     The lifetime of this memory is tied to the lifetime of the I/O
     Queue object, and we register an optional destructor callback
     to release any private allocations, and/or resources.


Arguments:

    Device - Handle to a framework device object.

Return Value:

    NTSTATUS

--*/
{
    WDFQUEUE queue;
    NTSTATUS status;
    PQUEUE_CONTEXT queueContext;
    WDF_IO_QUEUE_CONFIG    queueConfig;
    WDF_OBJECT_ATTRIBUTES  queueAttributes;
    PDEVICE_CONTEXT deviceContext;




    PAGED_CODE();

    //
    // Configure a default queue so that requests that are not
    // configure-fowarded using WdfDeviceConfigureRequestDispatching to goto
    // other queues get dispatched here.
    //
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(
         &queueConfig,
        WdfIoQueueDispatchSequential
        );

    queueConfig.EvtIoRead   = EchoEvtIoRead;
    queueConfig.EvtIoWrite  = EchoEvtIoWrite;
    queueConfig.EvtIoDeviceControl = InvertedEvtIoDeviceControl;

    //
    // Fill in a callback for destroy, and our QUEUE_CONTEXT size
    //
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&queueAttributes, QUEUE_CONTEXT);

    //
    // Set synchronization scope on queue and have the timer to use queue as
    // the parent object so that queue and timer callbacks are synchronized
    // with the same lock.
    //
    queueAttributes.SynchronizationScope = WdfSynchronizationScopeQueue;

    queueAttributes.EvtDestroyCallback = EchoEvtIoQueueContextDestroy;

    status = WdfIoQueueCreate(
                 Device,
                 &queueConfig,
                 &queueAttributes,
                 &queue
                 );

    if( !NT_SUCCESS(status) ) {
        KdPrint(("WdfIoQueueCreate failed 0x%x\n",status));
        return status;
    }

    deviceContext = WdfObjectGet_DEVICE_CONTEXT(Device);
    deviceContext->PrivateDeviceData = 0;

    WDF_IO_QUEUE_CONFIG_INIT(&queueConfig,
        WdfIoQueueDispatchManual);

    queueConfig.PowerManaged = WdfFalse;

    status = WdfIoQueueCreate(Device,
        &queueConfig,
        WDF_NO_OBJECT_ATTRIBUTES,
        &deviceContext->NotificationQueue);

    if (!NT_SUCCESS(status)) {
        KdPrint(("WdfIoQueueCreate failed 0x%x\n", status));
        return status;
    }

    // Get our Driver Context memory from the returned Queue handle
    queueContext = QueueGetContext(queue);

    queueContext->Buffer = NULL;

    queueContext->CurrentRequest = NULL;
    queueContext->CurrentStatus = STATUS_INVALID_DEVICE_REQUEST;



    return status;
}


NTSTATUS
EchoTimerCreate(
    IN WDFTIMER*       Timer,
    IN ULONG           Period,
    IN WDFQUEUE        Queue
    )
/*++

Routine Description:

    Subroutine to create periodic timer. By associating the timerobject with
    the queue, we are basically telling the framework to serialize the queue
    callbacks with the dpc callback. By doing so, we don't have to worry
    about protecting queue-context structure from multiple threads accessing
    it simultaneously.

Arguments:


Return Value:

    NTSTATUS

--*/
{
    NTSTATUS Status;
    WDF_TIMER_CONFIG       timerConfig;
    WDF_OBJECT_ATTRIBUTES  timerAttributes;

    PAGED_CODE();

    //
    // Create a periodic timer.
    //
    // WDF_TIMER_CONFIG_INIT_PERIODIC sets AutomaticSerialization to TRUE by default.
    //
    WDF_TIMER_CONFIG_INIT_PERIODIC(&timerConfig, EchoEvtTimerFunc, Period);

    WDF_OBJECT_ATTRIBUTES_INIT(&timerAttributes);
    timerAttributes.ParentObject = Queue; // Synchronize with the I/O Queue

    Status = WdfTimerCreate(&timerConfig,
                             &timerAttributes,
                             Timer     // Output handle
                             );

    return Status;
}



VOID
EchoEvtIoQueueContextDestroy(
    WDFOBJECT Object
)
/*++

Routine Description:

    This is called when the Queue that our driver context memory
    is associated with is destroyed.

Arguments:

    Context - Context that's being freed.

Return Value:

    VOID

--*/
{
    PQUEUE_CONTEXT queueContext = QueueGetContext(Object);

    //
    // Release any resources pointed to in the queue context.
    //
    // The body of the queue context will be released after
    // this callback handler returns
    //

    //
    // If Queue context has an I/O buffer, release it
    //
    if( queueContext->Buffer != NULL ) {
        ExFreePool(queueContext->Buffer);
    }

    return;
}


VOID
EchoEvtRequestCancel(
    IN WDFREQUEST Request
    )
/*++

Routine Description:


    Called when an I/O request is cancelled after the driver has marked
    the request cancellable. This callback is automatically synchronized
    with the I/O callbacks since we have chosen to use frameworks Device
    level locking.

Arguments:

    Request - Request being cancelled.

Return Value:

    VOID

--*/
{
    PQUEUE_CONTEXT queueContext = QueueGetContext(WdfRequestGetIoQueue(Request));

    KdPrint(("EchoEvtRequestCancel called on Request 0x%p\n",  Request));

    //
    // The following is race free by the callside or DPC side
    // synchronizing completion by calling
    // WdfRequestMarkCancelable(Queue, Request, FALSE) before
    // completion and not calling WdfRequestComplete if the
    // return status == STATUS_CANCELLED.
    //
    WdfRequestCompleteWithInformation(Request, STATUS_CANCELLED, 0L);

    //
    // This book keeping is synchronized by the common
    // Queue presentation lock
    //
    ASSERT(queueContext->CurrentRequest == Request);
    queueContext->CurrentRequest = NULL;

    return;
}

VOID
EchoEvtIoRead(
    IN WDFQUEUE   Queue,
    IN WDFREQUEST Request,
    IN size_t      Length
    )
/*++

Routine Description:

    This event is called when the framework receives IRP_MJ_READ request.
    It will copy the content from the queue-context buffer to the request buffer.
    If the driver hasn't received any write request earlier, the read returns zero.

Arguments:

    Queue -  Handle to the framework queue object that is associated with the
             I/O request.

    Request - Handle to a framework request object.

    Length  - number of bytes to be read.
              The default property of the queue is to not dispatch
              zero lenght read & write requests to the driver and
              complete is with status success. So we will never get
              a zero length request.

Return Value:

    VOID

--*/
{
    NTSTATUS Status;
    PQUEUE_CONTEXT queueContext = QueueGetContext(Queue);
    WDFMEMORY memory;

    _Analysis_assume_(Length > 0);

    KdPrint(("EchoEvtIoRead Called! Queue 0x%p, Request 0x%p Length %Iu\n",
             Queue,Request,Length));
    //
    // No data to read
    //
    if( (queueContext->Buffer == NULL)  ) {
        WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, (ULONG_PTR)0L);
        return;
    }
    _Analysis_assume_(queueContext->Length > 0);

    //
    // Read what we have
    //
    if( queueContext->Length < Length ) {
        Length = queueContext->Length;
    }

    //
    // Get the request memory
    //
    Status = WdfRequestRetrieveOutputMemory(Request, &memory);
    if( !NT_SUCCESS(Status) ) {
        KdPrint(("EchoEvtIoRead Could not get request memory buffer 0x%x\n", Status));
        WdfVerifierDbgBreakPoint();
        WdfRequestCompleteWithInformation(Request, Status, 0L);
        return;
    }

    // Copy the memory out
    Status = WdfMemoryCopyFromBuffer( memory, // destination
                             0,      // offset into the destination memory
                             queueContext->Buffer,
                             Length );
    if( !NT_SUCCESS(Status) ) {
        KdPrint(("EchoEvtIoRead: WdfMemoryCopyFromBuffer failed 0x%x\n", Status));
        WdfRequestComplete(Request, Status);
        return;
    }

    // Set transfer information
    WdfRequestSetInformation(Request, (ULONG_PTR)Length);

    // Mark the request is cancelable
    WdfRequestMarkCancelable(Request, EchoEvtRequestCancel);


    // Defer the completion to another thread from the timer dpc
    queueContext->CurrentRequest = Request;
    queueContext->CurrentStatus  = Status;

    return;
}

VOID
EchoEvtIoWrite(
    IN WDFQUEUE   Queue,
    IN WDFREQUEST Request,
    IN size_t     Length
    )
/*++

Routine Description:

    This event is invoked when the framework receives IRP_MJ_WRITE request.
    This routine allocates memory buffer, copies the data from the request to it,
    and stores the buffer pointer in the queue-context with the length variable
    representing the buffers length. The actual completion of the request
    is defered to the periodic timer dpc.

Arguments:

    Queue -  Handle to the framework queue object that is associated with the
             I/O request.

    Request - Handle to a framework request object.

    Length  - number of bytes to be read.
              The default property of the queue is to not dispatch
              zero lenght read & write requests to the driver and
              complete is with status success. So we will never get
              a zero length request.

Return Value:

    VOID

--*/
{
    NTSTATUS Status;
    WDFMEMORY memory;
    PDEVICE_CONTEXT deviceContext = WdfObjectGet_DEVICE_CONTEXT(WdfIoQueueGetDevice(Queue));
    deviceContext->PrivateDeviceData = 0;



    _Analysis_assume_(Length > 0);

    KdPrint(("EchoEvtIoWrite Called! Queue 0x%p, Request 0x%p Length %Iu\n",
             Queue,Request,Length));

    if( Length > MAX_WRITE_LENGTH ) {
        KdPrint(("EchoEvtIoWrite Buffer Length to big %Iu, Max is %d\n",
                 Length,MAX_WRITE_LENGTH));
        WdfRequestCompleteWithInformation(Request, STATUS_BUFFER_OVERFLOW, 0L);
        return;
    }
    
    // Get the memory buffer
    Status = WdfRequestRetrieveInputMemory(Request, &memory);
    if( !NT_SUCCESS(Status) ) {
        KdPrint(("EchoEvtIoWrite Could not get request memory buffer 0x%x\n",
                    Status));
        WdfVerifierDbgBreakPoint();
        WdfRequestComplete(Request, Status);
        return;
    }

    WdfRequestSetInformation(Request, (ULONG_PTR)Length);
    do {
        Status = InvertedNotify(deviceContext, memory, Length);
        if (Status == STATUS_NO_MORE_ENTRIES) {
            Status = STATUS_SUCCESS;
            break;
        }
        if (!NT_SUCCESS(Status)) {
            KdPrint(("EchoEvtIoWrite InvertedNotify failed 0x%x\n", Status));
            WdfVerifierDbgBreakPoint();

            WdfRequestSetInformation(Request, (ULONG_PTR)Length);
            WdfRequestComplete(Request, Status);
            return;
        } 
    
    } while (Status != STATUS_NO_MORE_ENTRIES);

    WdfRequestComplete(Request, Status);

    return;
}


VOID
EchoEvtTimerFunc(
    IN WDFTIMER     Timer
    )
/*++

Routine Description:

    This is the TimerDPC the driver sets up to complete requests.
    This function is registered when the WDFTIMER object is created, and
    will automatically synchronize with the I/O Queue callbacks
    and cancel routine.

Arguments:

    Timer - Handle to a framework Timer object.

Return Value:

    VOID

--*/
{
    NTSTATUS      Status;
    WDFREQUEST     Request;
    WDFQUEUE queue;
    PQUEUE_CONTEXT queueContext ;

    queue = WdfTimerGetParentObject(Timer);
    queueContext = QueueGetContext(queue);

    //
    // DPC is automatically synchronized to the Queue lock,
    // so this is race free without explicit driver managed locking.
    //
    Request = queueContext->CurrentRequest;
    if( Request != NULL ) {

        //
        // Attempt to remove cancel status from the request.
        //
        // The request is not completed if it is already cancelled
        // since the EchoEvtIoCancel function has run, or is about to run
        // and we are racing with it.
        //
        Status = WdfRequestUnmarkCancelable(Request);
        if( Status != STATUS_CANCELLED ) {

            queueContext->CurrentRequest = NULL;
            Status = queueContext->CurrentStatus;

            KdPrint(("CustomTimerDPC Completing request 0x%p, Status 0x%x \n", Request,Status));

            WdfRequestComplete(Request, Status);
        }
        else {
            KdPrint(("CustomTimerDPC Request 0x%p is STATUS_CANCELLED, not completing\n",
                                Request));
        }
    }

    return;
}



NTSTATUS
InvertedNotify(PDEVICE_CONTEXT DevContext,
    IN WDFMEMORY Memory,
    IN size_t Length
)
{
    NTSTATUS status;
    ULONG_PTR info;
    WDFREQUEST notifyRequest;
    PCSTR  bufferPointer;


    status = WdfIoQueueRetrieveNextRequest(DevContext->NotificationQueue,
        &notifyRequest);

    //
    // Be sure we got a Request
    // 
    if (!NT_SUCCESS(status)) {

        //
        // Nope!  We were NOT able to successfully remove a Request from the
        // notification queue.  Well, perhaps there aren't any right now.
        // Whatever... not much we can do in this example about this.
        // 
#if DBG
        DbgPrint("InvertedNotify: Failed to retrieve request. Status = 0x%0x\n",
            status);
#endif
        return status;
    }

    //
    // We've successfully removed a Request from the queue of pending 
    // notification IOCTLs.
    // 

    //
    // Get a a pointer to the output buffer that was passed-in with the user
    // notification IOCTL.  We'll use this to return additional info about
    // the event. The minimum size Output Buffer that we need is sizeof(LONG).
    // We don't need this call to return us what the actual size is.
    // 
    status = WdfRequestRetrieveOutputBuffer(notifyRequest,
        Length,
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
        DbgPrint("InvertedNotify: WdfRequestRetrieveOutputBuffer failed.  Status = 0x%0x\n",
            status);
#endif

        //
        // Complete the IOCTL_OSR_INVERT_NOTIFICATION with success, but
        // indicate that we're not returning any additional information.
        // 
        status = STATUS_SUCCESS;
        info = 0;

    }
    else {

        //
        // We successfully retrieved a Request from the notification Queue
        // AND we retrieved an output buffer into which to return some
        // additional information.
        // 

        //
        // As part of this example, we return a monotonically increasing
        // sequence number with each notification.  In a real driver, this
        // could be information (of any size) describing or identifying the
        // event.
        // 
        // Of course, you don't HAVE to return any information with the
        // notification.  In that case, you can skip the call to 
        // WdfRequestRetrieveOutputBuffer and such.
        // 
        DbgPrint("ASDF");
        DbgPrint(("%s\n", bufferPointer));
        status = WdfMemoryCopyToBuffer(Memory,
            0,  // offset into the source memory
            (PVOID)bufferPointer,
            Length);
        if (!NT_SUCCESS(status)) {
            KdPrint(("EchoEvtIoWrite WdfMemoryCopyToBuffer failed 0x%x\n", status));
            WdfVerifierDbgBreakPoint();

            
            return status;
        }

        //
        // Complete the IOCTL_OSR_INVERT_NOTIFICATION with success, indicating
        // we're returning a longword of data in the user's OutBuffer
        // 
        status = STATUS_SUCCESS;
        info = (ULONG_PTR)Length;
    }

    //
    // And now... NOTIFY the user about the event. We do this just
    // by completing the dequeued Request.
    //
    WdfRequestCompleteWithInformation(notifyRequest, status, info);
    return status;
}
