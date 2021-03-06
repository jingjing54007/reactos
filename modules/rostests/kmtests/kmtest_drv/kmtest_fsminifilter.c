/*
 * PROJECT:         ReactOS kernel-mode tests - Filter Manager
 * LICENSE:         GPLv2+ - See COPYING in the top level directory
 * PURPOSE:         FS Mini-filter wrapper to host the filter manager tests
 * PROGRAMMER:      Ged Murphy <ged.murphy@reactos.org>
 */

#include <ntifs.h>
#include <ndk/ketypes.h>
#include <fltkernel.h>

#define KMT_DEFINE_TEST_FUNCTIONS
#include <kmt_test.h>

#define NDEBUG
#include <debug.h>

#include <kmt_public.h>

#define KMTEST_FILTER_POOL_TAG  'fTMK'


typedef struct _FILTER_DATA
{
    PDRIVER_OBJECT DriverObject;
    FLT_REGISTRATION FilterRegistration;
    PFLT_FILTER Filter;
    PFLT_PORT ServerPort;

} FILTER_DATA, *PFILTER_DATA;


/* Prototypes */
DRIVER_INITIALIZE DriverEntry;

/* Globals */
static PDRIVER_OBJECT TestDriverObject;
static FILTER_DATA FilterData;
static PFLT_OPERATION_REGISTRATION Callbacks = NULL;
static PFLT_CONTEXT_REGISTRATION Contexts = NULL;

static PFLT_CONNECT_NOTIFY FilterConnect = NULL;
static PFLT_DISCONNECT_NOTIFY FilterDisconnect = NULL;
static PFLT_MESSAGE_NOTIFY FilterMessage = NULL;
static LONG MaxConnections = 0;

NTSTATUS
FLTAPI
FilterUnload(
    _In_ FLT_FILTER_UNLOAD_FLAGS Flags
);

NTSTATUS
FLTAPI
FilterInstanceSetup(
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _In_ FLT_INSTANCE_SETUP_FLAGS Flags,
    _In_ DEVICE_TYPE VolumeDeviceType,
    _In_ FLT_FILESYSTEM_TYPE VolumeFilesystemType
);

NTSTATUS
FLTAPI
FilterQueryTeardown(
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _In_ FLT_INSTANCE_QUERY_TEARDOWN_FLAGS Flags
);

FLT_PREOP_CALLBACK_STATUS
FLTAPI
FilterPreOperation(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _Outptr_result_maybenull_ PVOID *CompletionContext
);

FLT_POSTOP_CALLBACK_STATUS
FLTAPI
FilterPostOperation(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _In_opt_ PVOID CompletionContext,
    _In_ FLT_POST_OPERATION_FLAGS Flags
);



FLT_REGISTRATION FilterRegistration =
{
    sizeof(FLT_REGISTRATION),               //  Size
    FLT_REGISTRATION_VERSION,               //  Version
    0,                                      //  Flags
    NULL,                                   //  ContextRegistration
    NULL,                                   //  OperationRegistration
    FilterUnload,                           //  FilterUnloadCallback
    FilterInstanceSetup,                    //  InstanceSetupCallback
    FilterQueryTeardown,                    //  InstanceQueryTeardownCallback
    NULL,                                   //  InstanceTeardownStartCallback
    NULL,                                   //  InstanceTeardownCompleteCallback
    NULL,                                   //  AmFilterGenerateFileNameCallback
    NULL,                                   //  AmFilterNormalizeNameComponentCallback
    NULL,                                   //  NormalizeContextCleanupCallback
#if FLT_MGR_LONGHORN
    NULL,                                   //  TransactionNotificationCallback
    NULL,                                   //  AmFilterNormalizeNameComponentExCallback
#endif
};



/* Filter Interface Routines ****************************/

/**
 * @name DriverEntry
 *
 * Driver entry point.
 *
 * @param DriverObject
 *        Driver Object
 * @param RegistryPath
 *        Driver Registry Path
 *
 * @return Status
 */
NTSTATUS
NTAPI
DriverEntry(
    IN PDRIVER_OBJECT DriverObject,
    IN PUNICODE_STRING RegistryPath)
{
    NTSTATUS Status = STATUS_SUCCESS;
    OBJECT_ATTRIBUTES ObjectAttributes;
    PSECURITY_DESCRIPTOR SecurityDescriptor;
    UNICODE_STRING DeviceName;
    WCHAR DeviceNameBuffer[128] = L"\\Device\\Kmtest-";
    PCWSTR DeviceNameSuffix;
    INT Flags = 0;
    PKPRCB Prcb;

    PAGED_CODE();
    //__debugbreak();
    DPRINT("DriverEntry\n");

    RtlZeroMemory(&FilterData, sizeof(FILTER_DATA));

    Prcb = KeGetCurrentPrcb();
    KmtIsCheckedBuild = (Prcb->BuildType & PRCB_BUILD_DEBUG) != 0;
    KmtIsMultiProcessorBuild = (Prcb->BuildType & PRCB_BUILD_UNIPROCESSOR) == 0;
    TestDriverObject = DriverObject;


    /* call TestEntry */
    RtlInitUnicodeString(&DeviceName, DeviceNameBuffer);
    DeviceName.MaximumLength = sizeof DeviceNameBuffer;
    TestEntry(DriverObject, RegistryPath, &DeviceNameSuffix, &Flags);

    RtlAppendUnicodeToString(&DeviceName, DeviceNameSuffix);

    /* Register with the filter manager */
    if (!(Flags & TESTENTRY_NO_REGISTER_FILTER))
    {
        Status = FltRegisterFilter(DriverObject,
                                   &FilterRegistration,
                                   &FilterData.Filter);
        if (!NT_SUCCESS(Status))
        {
            DPRINT1("Failed to register the filter driver %wZ\n", &DeviceName);
            goto cleanup;
        }
    }

    if (!(Flags & TESTENTRY_NO_CREATE_COMMS_PORT))
    {
        /* Create a security descriptor */
        Status = FltBuildDefaultSecurityDescriptor(&SecurityDescriptor,
                                                   FLT_PORT_ALL_ACCESS);
        if (!NT_SUCCESS(Status))
        {
            goto cleanup;
        }

        /* Initialize the security descriptor object */
        InitializeObjectAttributes(&ObjectAttributes,
                                   &DeviceName,
                                   OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE,
                                   NULL,
                                   SecurityDescriptor);


        /* Create the usermode communication port */
        Status = FltCreateCommunicationPort(FilterData.Filter,
                                            &FilterData.ServerPort,
                                            &ObjectAttributes,
                                            NULL,
                                            FilterConnect,
                                            FilterDisconnect,
                                            FilterMessage,
                                            MaxConnections);

        /* Free the security descriptor */
        FltFreeSecurityDescriptor(SecurityDescriptor);

        if (!NT_SUCCESS(Status))
        {
            goto cleanup;
        }
    }

    if (!(Flags & TESTENTRY_NO_START_FILTERING))
    {
        /* Start filtering the requests */
        Status = FltStartFiltering(FilterData.Filter);
    }

cleanup:
    if (!NT_SUCCESS(Status))
    {
        if (FilterData.ServerPort)
        {
            FltCloseCommunicationPort(FilterData.ServerPort);
        }
        if (FilterData.Filter)
        {
            FltUnregisterFilter(FilterData.Filter);
        }
    }

    return Status;
}

/**
 * @name DriverUnload
 *
 * Driver cleanup funtion.
 *
 * @param Flags
 *        Flags describing the unload request
 */
NTSTATUS
FLTAPI
FilterUnload(
    _In_ FLT_FILTER_UNLOAD_FLAGS Flags)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(Flags);

    DPRINT("DriverUnload\n");

    TestFilterUnload(Flags);

    /* Close the port and unregister the filter */
    if (FilterData.ServerPort)
    {
        FltCloseCommunicationPort(FilterData.ServerPort);
        FilterData.ServerPort = NULL;
    }
    if (FilterData.Filter)
    {
        FltUnregisterFilter(FilterData.Filter);
        FilterData.Filter = NULL;
    }

    if (Callbacks)
    {
        ExFreePoolWithTag(Callbacks, KMTEST_FILTER_POOL_TAG);
        Callbacks = NULL;
    }

    return STATUS_SUCCESS;
}


/**
 * @name FilterInstanceSetup
 *
 * Volume attach routine
 *
 * @param FltObjects
 *        Filter Object Pointers
 *        Pointer to an FLT_RELATED_OBJECTS structure that contains opaque pointers
 *        for the objects related to the current operation
 * @param Flags
 *        Bitmask of flags that indicate why the instance is being attached
 * @param VolumeDeviceType
 *        Device type of the file system volume
 * @param VolumeFilesystemType
 *        File system type of the volume
 *
 * @return Status.
 *         Return STATUS_SUCCESS to attach or STATUS_FLT_DO_NOT_ATTACH to ignore
 */
NTSTATUS
FLTAPI
FilterInstanceSetup(
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _In_ FLT_INSTANCE_SETUP_FLAGS Flags,
    _In_ DEVICE_TYPE VolumeDeviceType,
    _In_ FLT_FILESYSTEM_TYPE VolumeFilesystemType)
{
#if 0
    UCHAR VolPropBuffer[sizeof(FLT_VOLUME_PROPERTIES) + 512];
    PFLT_VOLUME_PROPERTIES VolumeProperties = (PFLT_VOLUME_PROPERTIES)VolPropBuffer;
#endif
    PDEVICE_OBJECT DeviceObject = NULL;
    UNICODE_STRING VolumeName;
    ULONG ReportedSectorSize = 0;
    ULONG SectorSize = 0;
    NTSTATUS Status;

    PAGED_CODE();

    UNREFERENCED_PARAMETER(FltObjects);
    UNREFERENCED_PARAMETER(Flags);

    RtlInitUnicodeString(&VolumeName, NULL);
#if 0 // FltGetVolumeProperties is not yet implemented
    /* Get the properties of this volume */
    Status = FltGetVolumeProperties(Volume,
                                    VolumeProperties,
                                    sizeof(VolPropBuffer),
                                    &LengthReturned);
    if (NT_SUCCESS(Status))
    {
        FLT_ASSERT((VolumeProperties->SectorSize == 0) || (VolumeProperties->SectorSize >= MIN_SECTOR_SIZE));
        SectorSize = max(VolumeProperties->SectorSize, MIN_SECTOR_SIZE);
        ReportedSectorSize = VolumeProperties->SectorSize;
    }
    else
    {
        DPRINT1("Failed to get the volume properties : 0x%X", Status);
        return Status;
    }
#endif
    /*  Get the storage device object we want a name for */
    Status = FltGetDiskDeviceObject(FltObjects->Volume, &DeviceObject);
    if (NT_SUCCESS(Status))
    {
        /* Get the dos device name */
        Status = IoVolumeDeviceToDosName(DeviceObject, &VolumeName);
        if (NT_SUCCESS(Status))
        {
            DPRINT("VolumeDeviceType %lu, VolumeFilesystemType %lu, Real SectSize=0x%04x, Reported SectSize=0x%04x, Name=\"%wZ\"",
                   VolumeDeviceType,
                   VolumeFilesystemType,
                   SectorSize,
                   ReportedSectorSize,
                   &VolumeName);

            Status = TestInstanceSetup(FltObjects,
                                       Flags,
                                       VolumeDeviceType,
                                       VolumeFilesystemType,
                                       &VolumeName,
                                       SectorSize,
                                       ReportedSectorSize);

            /* The buffer was allocated by the IoMgr */
            ExFreePool(VolumeName.Buffer);
        }
    }

    return Status;
}


/**
 * @name FilterQueryTeardown
 *
 * Volume detatch routine
 *
 * @param FltObjects
 *        Filter Object Pointers
 *        Pointer to an FLT_RELATED_OBJECTS structure that contains opaque pointers
 *        for the objects related to the current operation
 * @param Flags
 *        Flag that indicates why the minifilter driver instance is being torn down
 *
 */
NTSTATUS
FLTAPI
FilterQueryTeardown(
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _In_ FLT_INSTANCE_QUERY_TEARDOWN_FLAGS Flags)
{
    PAGED_CODE();

    TestQueryTeardown(FltObjects, Flags);

    /* We always allow a volume to detach */
    return STATUS_SUCCESS;
}


/* Public Routines **************************************/

NTSTATUS
KmtFilterRegisterCallbacks(
    _In_ CONST FLT_OPERATION_REGISTRATION *OperationRegistration)
{
    ULONG Count = 0;
    INT i;

    if (Callbacks)
    {
        return STATUS_ALREADY_REGISTERED;
    }

    /* Count how many IRPs being registered */
    for (i = 0; i < IRP_MJ_MAXIMUM_FUNCTION; i++)
    {
        if (OperationRegistration[i].MajorFunction == IRP_MJ_OPERATION_END)
            break;
        Count++;
    }

    /* Allocate enough pool to hold a copy of the array */
    Callbacks = ExAllocatePoolWithTag(NonPagedPool,
                                      sizeof(FLT_OPERATION_REGISTRATION) * (Count + 1),
                                      KMTEST_FILTER_POOL_TAG);
    if (Callbacks == NULL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /* Copy the array, but using the our own pre/post callbacks */
    for (i = 0; i < Count; i++)
    {
        Callbacks[i].MajorFunction = OperationRegistration[i].MajorFunction;
        Callbacks[i].Flags = OperationRegistration[i].Flags;
        Callbacks[i].PreOperation = FilterPreOperation;
        Callbacks[i].PostOperation = FilterPostOperation;
    }

    /* Terminate the array */
    Callbacks[Count].MajorFunction = IRP_MJ_OPERATION_END;

    return STATUS_SUCCESS;
}

NTSTATUS
KmtFilterRegisterContexts(
    _In_ PFLT_CONTEXT_REGISTRATION ContextRegistration,
    _In_ PVOID Callback)
{
    UNREFERENCED_PARAMETER(ContextRegistration);
    UNREFERENCED_PARAMETER(Callback);
    UNREFERENCED_PARAMETER(Contexts);
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS
KmtFilterRegisterComms(
    _In_ PFLT_CONNECT_NOTIFY ConnectNotifyCallback,
    _In_ PFLT_DISCONNECT_NOTIFY DisconnectNotifyCallback,
    _In_opt_ PFLT_MESSAGE_NOTIFY MessageNotifyCallback,
    _In_ LONG MaxClientConnections)
{
    FilterConnect = ConnectNotifyCallback;
    FilterDisconnect = DisconnectNotifyCallback;
    FilterMessage = MessageNotifyCallback;
    MaxConnections = MaxClientConnections;

    return STATUS_SUCCESS;
}


/* Private Routines ******************************************/

FLT_PREOP_CALLBACK_STATUS
FLTAPI
FilterPreOperation(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _Outptr_result_maybenull_ PVOID *CompletionContext)
{
    FLT_PREOP_CALLBACK_STATUS Status;
    UCHAR MajorFunction;
    INT i;

    Status = FLT_PREOP_SUCCESS_NO_CALLBACK;
    MajorFunction = Data->Iopb->MajorFunction;

    for (i = 0; i < sizeof(Callbacks) / sizeof(Callbacks[0]); i++)
    {
        if (MajorFunction == Callbacks[i].MajorFunction)
        {
            // Call their pre-callback
            Status = Callbacks[i].PreOperation(Data,
                                               FltObjects,
                                               CompletionContext);
        }
    }

    return Status;
}

FLT_POSTOP_CALLBACK_STATUS
FLTAPI
FilterPostOperation(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _In_opt_ PVOID CompletionContext,
    _In_ FLT_POST_OPERATION_FLAGS Flags)
{
    FLT_POSTOP_CALLBACK_STATUS Status;
    UCHAR MajorFunction;
    INT i;

    Status = FLT_POSTOP_FINISHED_PROCESSING;
    MajorFunction = Data->Iopb->MajorFunction;

    for (i = 0; i < sizeof(Callbacks) / sizeof(Callbacks[0]); i++)
    {
        if (MajorFunction == Callbacks[i].MajorFunction)
        {
            // Call their post-callback
            Status = Callbacks[i].PostOperation(Data,
                                                FltObjects,
                                                CompletionContext,
                                                Flags);
        }
    }

    return Status;
}
