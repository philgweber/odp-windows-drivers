/*++

Copyright (c) Microsoft Corporation.  All rights reserved.

Module Name:

    controller.cpp

Abstract:

    This module contains the controller-specific functions
    for the QEMU "ODP socket-backed I2C controller".

    The controller is interrupt-driven. SPB read/write/sequence
    callbacks kick off a transfer and return; completion (and any NAK /
    protocol error) is signalled by the CMD_DONE interrupt and the
    transfer is advanced asynchronously by a DPC state machine.

    Hardware command model (see hw/odp/i2c-controller.c):
      - Write DATA = (CMD_START << 8) | (addr7 << 1 | rw) to address a
        target. The controller raises CMD_DONE when the target ACKs (or
        NAK on no response).
      - Write DATA = (CMD_TX << 8) | byte to transmit a byte; CMD_DONE on
        ACK, NAK on data NAK.
      - Write DATA = (CMD_RX << 8) | (count - 1) to read a burst of up to
        256 bytes; CMD_DONE when the whole burst has arrived in the RX
        FIFO. Read DATA to pop bytes while STATUS_RX_RDY is set.
      - Write DATA = (CMD_STOP << 8) to release the bus; CMD_DONE follows.

Environment:

    kernel-mode only

Revision History:

--*/

#include "internal.h"
#include "controller.h"
#include "device.h"

#include "controller.tmh"

const PBC_TRANSFER_SETTINGS g_TransferSettings[] =
    {
        // IsEnd
        {FALSE}, // SpbRequestSequencePositionInvalid
        {TRUE},  // SpbRequestSequencePositionSingle
        {FALSE}, // SpbRequestSequencePositionFirst
        {FALSE}, // SpbRequestSequencePositionContinue
        {TRUE}   // SpbRequestSequencePositionLast
};

/////////////////////////////////////////////////
//
// Low-level hardware helpers.
//
/////////////////////////////////////////////////

//
// Posts a command word to the DATA register without waiting. Command
// completion (and any NAK / protocol error) is signalled asynchronously
// through the CMD_DONE interrupt and serviced by the DPC state machine.
//

static VOID
QemuI2cPostCommand(
    _In_ PPBC_DEVICE pDevice,
    _In_ ULONG Command,
    _In_ UCHAR Data)
{
    pDevice->pRegisters->Data.Write(QEMU_I2C_MAKE_DATA(Command, Data));
}

/////////////////////////////////////////////////
//
// Controller initialization.
//
/////////////////////////////////////////////////

VOID ControllerInitialize(
    _In_ PPBC_DEVICE pDevice)
/*++

  Routine Description:

    This routine initializes the controller hardware.

--*/
{
    FuncEntry(TRACE_FLAG_PBCLOADING);

    NT_ASSERT(pDevice != NULL);

    if (pDevice->pRegisters != NULL)
    {
        //
        // Soft reset the controller state machine and RX FIFO, clear any
        // latched status, and leave interrupts disabled until a transfer
        // enables them.
        //

        pDevice->pRegisters->Ctrl.Write(QEMU_I2C_CTRL_RESET);
        pDevice->pRegisters->Ctrl.Write(QEMU_I2C_CTRL_CLEAR_FIFO);
        pDevice->pRegisters->Status.Write(QEMU_I2C_STATUS_W1C_MASK);
        pDevice->pRegisters->Ctrl.Write(0);
    }

    FuncExit(TRACE_FLAG_PBCLOADING);
}

VOID ControllerUninitialize(
    _In_ PPBC_DEVICE pDevice)
/*++

  Routine Description:

    This routine uninitializes the controller hardware.

--*/
{
    FuncEntry(TRACE_FLAG_PBCLOADING);

    NT_ASSERT(pDevice != NULL);

    if (pDevice->pRegisters != NULL)
    {
        //
        // Disable interrupts and quiesce the controller.
        //

        pDevice->pRegisters->Ctrl.Write(QEMU_I2C_CTRL_RESET);
        pDevice->pRegisters->Ctrl.Write(0);
    }

    FuncExit(TRACE_FLAG_PBCLOADING);
}

/////////////////////////////////////////////////
//
// Transfer handling (interrupt-driven).
//
/////////////////////////////////////////////////

VOID ControllerConfigureForTransfer(
    _In_ PPBC_DEVICE pDevice,
    _In_ PPBC_REQUEST pRequest)
/*++

  Routine Description:

    This routine begins a (sub)transfer by enabling the command-complete
    interrupt and posting the addressing (repeated) START. The transfer
    then advances asynchronously, one controller command per CMD_DONE
    interrupt, in ControllerProcessInterrupts.

  Arguments:

    pDevice - a pointer to the PBC device context
    pRequest - a pointer to the PBC request context

  Return Value:

    None. The request transfer is completed via ControllerCompleteTransfer
    (synchronously here only on an immediate validation failure).

--*/
{
    FuncEntry(TRACE_FLAG_TRANSFER);

    PPBC_TARGET pTarget;
    UCHAR address7Bit;
    BOOLEAN read;

    NT_ASSERT(pDevice != NULL);
    NT_ASSERT(pRequest != NULL);

    pTarget = pDevice->pCurrentTarget;
    NT_ASSERT(pTarget != NULL);

    //
    // Initialize request context for transfer.
    //

    pRequest->Settings = g_TransferSettings[pRequest->SequencePosition];
    pRequest->Status = STATUS_SUCCESS;
    pRequest->Information = 0;

    //
    // The QEMU controller only supports 7-bit addressing.
    //

    if (pTarget->Settings.AddressMode != AddressMode7Bit)
    {
        Trace(
            TRACE_LEVEL_ERROR,
            TRACE_FLAG_TRANSFER,
            "10-bit addressing is not supported by the QEMU I2C controller "
            "(WDFDEVICE %p)",
            pDevice->FxDevice);

        pRequest->Status = STATUS_NOT_SUPPORTED;
        pRequest->Information = 0;
        ControllerCompleteTransfer(pDevice, pRequest, TRUE);
        goto exit;
    }

    address7Bit = (UCHAR)(pTarget->Settings.Address & 0x7F);
    read = (pRequest->Direction == SpbTransferDirectionFromDevice);

    Trace(
        TRACE_LEVEL_VERBOSE,
        TRACE_FLAG_TRANSFER,
        "Controller starting %s of %Iu bytes to address 0x%lx "
        "(SPBREQUEST %p, WDFDEVICE %p, IsEnd=%d)",
        read ? "read" : "write",
        pRequest->Length,
        pTarget->Settings.Address,
        pRequest->SpbRequest,
        pDevice->FxDevice,
        pRequest->Settings.IsEnd);

    //
    // Enable the command-complete interrupt and post the (repeated) START
    // carrying the direction bit. The QEMU controller accepts a START both
    // from the idle bus (Single / First) and from a bus that is already
    // open (repeated START for Continue / Last and direction changes).
    // Completion arrives via the CMD_DONE interrupt and is serviced by
    // ControllerProcessInterrupts.
    //

    //
    // Clear the latched ISR/DPC status under the interrupt lock so every
    // access to InterruptStatus is uniformly serialized with the ISR.
    // (The controller interrupt is disabled at this point either way, but
    // taking the lock here removes the unwritten "interrupts are off"
    // invariant. Lock order is device-lock -> interrupt-lock, matching
    // OnInterruptDpc.)
    //
    WdfInterruptAcquireLock(pDevice->InterruptObject);
    pDevice->InterruptStatus = 0;
    WdfInterruptReleaseLock(pDevice->InterruptObject);

    PbcDeviceSetInterruptMask(
        pDevice,
        QEMU_I2C_STATUS_CMD_DONE | QEMU_I2C_STATUS_NAK | QEMU_I2C_STATUS_PROTO_ERR);

    //
    // Arm CMD_DONE. On the sequence-chaining path the DPC tail re-enables
    // interrupts idempotently from the mask; this enable is what covers the
    // initial (non-DPC, PASSIVE_LEVEL) transfer that has no DPC tail.
    //
    ControllerEnableInterrupts(pDevice, QEMU_I2C_STATUS_CMD_DONE);

    pRequest->Phase = TransferPhaseAddress;
    QemuI2cPostCommand(
        pDevice,
        QEMU_I2C_CMD_START,
        (UCHAR)((address7Bit << 1) | (read ? 0x01 : 0x00)));

exit:

    FuncExit(TRACE_FLAG_TRANSFER);
}

VOID ControllerCompleteTransfer(
    _In_ PPBC_DEVICE pDevice,
    _In_ PPBC_REQUEST pRequest,
    _In_ BOOLEAN AbortSequence)
/*++

  Routine Description:

    This routine completes a data transfer. Unless there are
    more transfers remaining in the sequence, the request is
    marked complete.

  Arguments:

    pDevice - a pointer to the PBC device context
    pRequest - a pointer to the PBC request context
    AbortSequence - specifies whether the driver should abort the
        ongoing sequence or begin the next transfer

  Return Value:

    None.

--*/
{
    FuncEntry(TRACE_FLAG_TRANSFER);

    NT_ASSERT(pDevice != NULL);
    NT_ASSERT(pRequest != NULL);

    Trace(
        TRACE_LEVEL_INFORMATION,
        TRACE_FLAG_TRANSFER,
        "Transfer (index %lu) %s with %Iu bytes for address 0x%lx "
        "(SPBREQUEST %p)",
        pRequest->TransferIndex,
        NT_SUCCESS(pRequest->Status) ? "complete" : "error",
        pRequest->Information,
        pDevice->pCurrentTarget->Settings.Address,
        pRequest->SpbRequest);

    //
    // Update request context with information from this transfer.
    //

    pRequest->TotalInformation += pRequest->Information;
    pRequest->Information = 0;

    //
    // Check if there are more transfers
    // in the sequence.
    //

    if (!AbortSequence)
    {
        pRequest->TransferIndex++;

        if (pRequest->TransferIndex < pRequest->TransferCount)
        {
            //
            // Configure the request for the next transfer.
            //

            pRequest->Status = PbcRequestConfigureForIndex(
                pRequest,
                pRequest->TransferIndex);

            if (NT_SUCCESS(pRequest->Status))
            {
                //
                // Configure controller and start the next transfer.
                //

                PbcRequestDoTransfer(pDevice, pRequest);
                goto exit;
            }
        }
    }

    //
    // Done or error occurred. Set interrupt mask to 0.
    //

    PbcDeviceSetInterruptMask(pDevice, 0);

    //
    // Clear the target's current request. This will prevent
    // the request context from being accessed once the request
    // is completed (and the context is invalid).
    //

    pDevice->pCurrentTarget->pCurrentRequest = NULL;

    //
    // Clear the controller's current target if any of
    //   1. request is type sequence
    //   2. request position is single
    //      (did not come between lock/unlock)
    // Otherwise wait until unlock.
    //

    if ((pRequest->Type == SpbRequestTypeSequence) ||
        (pRequest->SequencePosition == SpbRequestSequencePositionSingle))
    {
        pDevice->pCurrentTarget = NULL;
    }

    //
    // Mark the IO complete. The interrupt DPC completes the SPB
    // request once it sees bIoComplete set.
    //

    pRequest->bIoComplete = TRUE;

exit:

    FuncExit(TRACE_FLAG_TRANSFER);
}

/////////////////////////////////////////////////
//
// Interrupt helpers.
//
// These register accessors are driven by the ISR/DPC to enable, read,
// acknowledge, and disable the controller's interrupts while servicing a
// transfer.
//
/////////////////////////////////////////////////

VOID ControllerEnableInterrupts(
    _In_ PPBC_DEVICE pDevice,
    _In_ ULONG InterruptMask)
/*++

  Routine Description:

    Enables the controller interrupt(s) corresponding to the supplied
    STATUS-register bit mask by setting the matching CTRL interrupt-enable
    bits.

--*/
{
    FuncEntry(TRACE_FLAG_TRANSFER);

    NT_ASSERT(pDevice != NULL);

    if (pDevice->pRegisters != NULL)
    {
        ULONG ctrlBits = 0;

        if (TestAnyBits(InterruptMask, QEMU_I2C_STATUS_CMD_DONE))
        {
            ctrlBits |= QEMU_I2C_CTRL_CMD_DONE_IE;
        }

        if (TestAnyBits(InterruptMask, QEMU_I2C_STATUS_RX_RDY))
        {
            ctrlBits |= QEMU_I2C_CTRL_RX_RDY_IE;
        }

        pDevice->pRegisters->Ctrl.SetBits(ctrlBits);
    }

    FuncExit(TRACE_FLAG_TRANSFER);
}

VOID ControllerDisableInterrupts(
    _In_ PPBC_DEVICE pDevice)
{
    FuncEntry(TRACE_FLAG_TRANSFER);

    NT_ASSERT(pDevice != NULL);

    if (pDevice->pRegisters != NULL)
    {
        pDevice->pRegisters->Ctrl.ClearBits(QEMU_I2C_CTRL_IE_MASK);
    }

    FuncExit(TRACE_FLAG_TRANSFER);
}

ULONG
ControllerGetInterruptStatus(
    _In_ PPBC_DEVICE pDevice,
    _In_ ULONG InterruptMask)
{
    FuncEntry(TRACE_FLAG_TRANSFER);

    ULONG interruptStatus = 0;

    NT_ASSERT(pDevice != NULL);

    if (pDevice->pRegisters != NULL)
    {
        interruptStatus = pDevice->pRegisters->Status.Read() & InterruptMask;
    }

    FuncExit(TRACE_FLAG_TRANSFER);

    return interruptStatus;
}

VOID ControllerAcknowledgeInterrupts(
    _In_ PPBC_DEVICE pDevice,
    _In_ ULONG InterruptMask)
{
    FuncEntry(TRACE_FLAG_TRANSFER);

    NT_ASSERT(pDevice != NULL);

    if (pDevice->pRegisters != NULL)
    {
        pDevice->pRegisters->Status.Write(InterruptMask & QEMU_I2C_STATUS_W1C_MASK);
    }

    FuncExit(TRACE_FLAG_TRANSFER);
}

//
// Issues a STOP (if this transfer ends the bus sequence) or completes the
// (sub)transfer immediately. Called from the interrupt state machine once
// the data phase has finished.
//

static VOID
ControllerFinishDataPhase(
    _In_ PPBC_DEVICE pDevice,
    _In_ PPBC_REQUEST pRequest)
{
    if (pRequest->Settings.IsEnd)
    {
        pRequest->Phase = TransferPhaseStop;
        QemuI2cPostCommand(pDevice, QEMU_I2C_CMD_STOP, 0);
    }
    else
    {
        ControllerCompleteTransfer(pDevice, pRequest, FALSE);
    }
}

//
// Posts the next TX byte (at offset pRequest->Information) and advances to
// the TX phase. Completes the request on a buffer-access failure.
//

static VOID
ControllerStartTxByte(
    _In_ PPBC_DEVICE pDevice,
    _In_ PPBC_REQUEST pRequest)
{
    UCHAR dataByte = 0;
    NTSTATUS status = PbcRequestGetByte(pRequest, pRequest->Information, &dataByte);

    if (!NT_SUCCESS(status))
    {
        pRequest->Status = status;
        pRequest->Information = 0;
        ControllerCompleteTransfer(pDevice, pRequest, TRUE);
        return;
    }

    pRequest->Phase = TransferPhaseTx;
    QemuI2cPostCommand(pDevice, QEMU_I2C_CMD_TX, dataByte);
}

//
// Posts the next RX burst (up to the FIFO size) and advances to the RX
// phase. The DATA field is (count - 1).
//

static VOID
ControllerStartRxBurst(
    _In_ PPBC_DEVICE pDevice,
    _In_ PPBC_REQUEST pRequest)
{
    size_t remaining = pRequest->Length - pRequest->Information;
    ULONG chunk = (remaining > QEMU_I2C_RX_FIFO_SIZE)
                      ? QEMU_I2C_RX_FIFO_SIZE
                      : (ULONG)remaining;

    pRequest->Phase = TransferPhaseRx;
    QemuI2cPostCommand(pDevice, QEMU_I2C_CMD_RX, (UCHAR)(chunk - 1));
}

VOID ControllerProcessInterrupts(
    _In_ PPBC_DEVICE pDevice,
    _In_ PPBC_REQUEST pRequest,
    _In_ ULONG InterruptStatus)
/*++

  Routine Description:

    Advances the interrupt-driven transfer state machine in response to a
    CMD_DONE interrupt. Each invocation processes the completion of the
    command posted for the current phase and posts the next command (or
    completes the transfer). Runs at DISPATCH_LEVEL from the DPC.

  Arguments:

    pDevice - a pointer to the PBC device context
    pRequest - a pointer to the PBC request context
    InterruptStatus - the latched STATUS bits captured by the ISR

  Return Value:

    None.

--*/
{
    FuncEntry(TRACE_FLAG_TRANSFER);

    PPBC_TARGET pTarget;
    UCHAR address7Bit;

    NT_ASSERT(pDevice != NULL);
    NT_ASSERT(pRequest != NULL);

    pTarget = pDevice->pCurrentTarget;
    NT_ASSERT(pTarget != NULL);

    address7Bit = (UCHAR)(pTarget->Settings.Address & 0x7F);

    //
    // A protocol error indicates illegal command ordering and is fatal.
    //

    if (TestAnyBits(InterruptStatus, QEMU_I2C_STATUS_PROTO_ERR))
    {
        pRequest->Status = STATUS_IO_DEVICE_ERROR;
        pRequest->Information = 0;
        ControllerCompleteTransfer(pDevice, pRequest, TRUE);
        goto exit;
    }

    switch (pRequest->Phase)
    {
    case TransferPhaseAddress:

        //
        // Addressing completed. A NAK here means no device answered.
        //

        if (TestAnyBits(InterruptStatus, QEMU_I2C_STATUS_NAK))
        {
            Trace(
                TRACE_LEVEL_ERROR,
                TRACE_FLAG_TRANSFER,
                "NACK addressing target 0x%lx (WDFDEVICE %p)",
                pTarget->Settings.Address,
                pDevice->FxDevice);

            pRequest->Status = STATUS_NO_SUCH_DEVICE;
            pRequest->Information = 0;
            ControllerCompleteTransfer(pDevice, pRequest, TRUE);
            goto exit;
        }

        if (pRequest->Length == 0)
        {
            ControllerFinishDataPhase(pDevice, pRequest);
        }
        else if (pRequest->Direction == SpbTransferDirectionToDevice)
        {
            ControllerStartTxByte(pDevice, pRequest);
        }
        else
        {
            ControllerStartRxBurst(pDevice, pRequest);
        }

        break;

    case TransferPhaseTx:

        if (TestAnyBits(InterruptStatus, QEMU_I2C_STATUS_NAK))
        {
            //
            // Data NAK: the target accepted fewer bytes than offered. This
            // is reported to the client as a short (successful) transfer.
            //

            Trace(
                TRACE_LEVEL_WARNING,
                TRACE_FLAG_TRANSFER,
                "Data NACK after %Iu byte(s) for address 0x%lx",
                pRequest->Information,
                pTarget->Settings.Address);

            ControllerFinishDataPhase(pDevice, pRequest);
            goto exit;
        }

        //
        // Byte accepted.
        //

        pRequest->Information++;

        if (pRequest->Information < pRequest->Length)
        {
            ControllerStartTxByte(pDevice, pRequest);
        }
        else
        {
            ControllerFinishDataPhase(pDevice, pRequest);
        }

        break;

    case TransferPhaseRx:
    {
        size_t remaining;
        ULONG chunk;

        if (TestAnyBits(InterruptStatus, QEMU_I2C_STATUS_NAK))
        {
            pRequest->Status = STATUS_NO_SUCH_DEVICE;
            pRequest->Information = 0;
            ControllerCompleteTransfer(pDevice, pRequest, TRUE);
            goto exit;
        }

        //
        // The whole burst has arrived in the RX FIFO; drain it.
        //

        remaining = pRequest->Length - pRequest->Information;
        chunk = (remaining > QEMU_I2C_RX_FIFO_SIZE)
                    ? QEMU_I2C_RX_FIFO_SIZE
                    : (ULONG)remaining;

        for (ULONG j = 0; j < chunk; j++)
        {
            ULONG value = pDevice->pRegisters->Data.Read();
            NTSTATUS st = PbcRequestSetByte(
                pRequest,
                pRequest->Information,
                (UCHAR)(value & QEMU_I2C_DATA_DATA_MASK));

            if (!NT_SUCCESS(st))
            {
                pRequest->Status = st;
                pRequest->Information = 0;
                ControllerCompleteTransfer(pDevice, pRequest, TRUE);
                goto exit;
            }

            pRequest->Information++;
        }

        if (pRequest->Information < pRequest->Length)
        {
            //
            // More to read: the controller requires a repeated START
            // before the next RX burst.
            //

            pRequest->Phase = TransferPhaseRxRestart;
            QemuI2cPostCommand(
                pDevice,
                QEMU_I2C_CMD_START,
                (UCHAR)((address7Bit << 1) | 0x01));
        }
        else
        {
            ControllerFinishDataPhase(pDevice, pRequest);
        }

        break;
    }

    case TransferPhaseRxRestart:

        if (TestAnyBits(InterruptStatus, QEMU_I2C_STATUS_NAK))
        {
            pRequest->Status = STATUS_NO_SUCH_DEVICE;
            pRequest->Information = 0;
            ControllerCompleteTransfer(pDevice, pRequest, TRUE);
            goto exit;
        }

        ControllerStartRxBurst(pDevice, pRequest);
        break;

    case TransferPhaseStop:

        //
        // The bus has been released; the (sub)transfer succeeded.
        //

        ControllerCompleteTransfer(pDevice, pRequest, FALSE);
        break;

    default:

        NT_ASSERTMSG("Unexpected transfer phase in ControllerProcessInterrupts", FALSE);
        break;
    }

exit:

    FuncExit(TRACE_FLAG_TRANSFER);
}
