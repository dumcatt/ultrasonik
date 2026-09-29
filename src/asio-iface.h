#pragma once

#include <windows.h>
#include <stddef.h>
#include <stdint.h>

/*  ASIO drivers use __thiscall calling convention (this in ECX on x86-32).
    GCC supports this via __attribute__((thiscall)). On x86-64 there is only
    one calling convention, so the attribute is not needed. */

#if defined(__i386__) || defined(_M_IX86)
#define ASIO_METHOD __attribute__((thiscall))
#else
#define ASIO_METHOD
#endif

typedef long ASIOBool;

enum {
    ASIOFalse = 0,
    ASIOTrue = 1,
};

typedef long ASIOError;

enum {
    ASE_OK = 0,
    ASE_SUCCESS = 0x3f4847a0,
    ASE_NotPresent = -1000,
    ASE_HWMalfunction,
    ASE_InvalidParameter,
    ASE_InvalidMode,
    ASE_SPNotAdvancing,
    ASE_NoClock,
    ASE_NoMemory,
};

typedef double ASIOSampleRate;

typedef enum ASIOSampleType {
    ASIOSTInt16MSB = 0,
    ASIOSTInt24MSB = 1,
    ASIOSTInt32MSB = 2,
    ASIOSTFloat32MSB = 3,
    ASIOSTFloat64MSB = 4,

    ASIOSTInt32MSB16 = 8,
    ASIOSTInt32MSB18 = 9,
    ASIOSTInt32MSB20 = 10,
    ASIOSTInt32MSB24 = 11,

    ASIOSTInt16LSB = 16,
    ASIOSTInt24LSB = 17,
    ASIOSTInt32LSB = 18,
    ASIOSTFloat32LSB = 19,
    ASIOSTFloat64LSB = 20,

    ASIOSTInt32LSB16 = 24,
    ASIOSTInt32LSB18 = 25,
    ASIOSTInt32LSB20 = 26,
    ASIOSTInt32LSB24 = 27,

    ASIOSTDSDInt8LSB1 = 32,
    ASIOSTDSDInt8MSB1 = 33,
    ASIOSTDSDInt8NER8 = 40,
} ASIOSampleType;

#pragma pack(push, 4)

typedef struct ASIOSamples {
    uint32_t hi;
    uint32_t lo;
} ASIOSamples;

typedef struct ASIOTimeStamp {
    uint32_t hi;
    uint32_t lo;
} ASIOTimeStamp;

typedef struct ASIOClockSource {
    long index;
    long associatedChannel;
    long associatedGroup;
    ASIOBool isCurrentSource;
    char name[128];
} ASIOClockSource;

typedef struct ASIOChannelInfo {
    long channel;
    ASIOBool isInput;
    ASIOBool isActive;
    long channelGroup;
    ASIOSampleType type;
    char name[32];
} ASIOChannelInfo;

typedef struct ASIOBufferInfo {
    ASIOBool isInput;
    long channelNum;
    void *buffers[2];
} ASIOBufferInfo;

typedef struct ASIOTime {
    char reserved[128];
} ASIOTime;

typedef struct ASIOCallbacks {
    void (*bufferSwitch)(long doubleBufferIndex, ASIOBool directProcess);
    void (*sampleRateDidChange)(ASIOSampleRate sRate);
    long (*asioMessage)(
            long selector,
            long value,
            void *message,
            double *opt);
    ASIOTime *(*bufferSwitchTimeInfo)(
            ASIOTime *params,
            long doubleBufferIndex,
            ASIOBool directProcess);
} ASIOCallbacks;

#pragma pack(pop)

typedef struct IASIO IASIO;
typedef struct IASIOVtbl IASIOVtbl;

struct IASIOVtbl {
    /* IUnknown methods (standard COM, __stdcall) */
    HRESULT (__stdcall *QueryInterface)(IASIO *self, REFIID riid, void **ppv);
    ULONG (__stdcall *AddRef)(IASIO *self);
    ULONG (__stdcall *Release)(IASIO *self);

    /* ASIO methods (__thiscall on x86-32) */
    ASIOBool (ASIO_METHOD *init)(IASIO *self, void *sysRef);
    void (ASIO_METHOD *getDriverName)(IASIO *self, char *name);
    long (ASIO_METHOD *getDriverVersion)(IASIO *self);
    void (ASIO_METHOD *getErrorMessage)(IASIO *self, char *string);
    ASIOError (ASIO_METHOD *start)(IASIO *self);
    ASIOError (ASIO_METHOD *stop)(IASIO *self);
    ASIOError (ASIO_METHOD *getChannels)(
            IASIO *self,
            long *numInputChannels,
            long *numOutputChannels);
    ASIOError (ASIO_METHOD *getLatencies)(
            IASIO *self,
            long *inputLatency,
            long *outputLatency);
    ASIOError (ASIO_METHOD *getBufferSize)(
            IASIO *self,
            long *minSize,
            long *maxSize,
            long *preferredSize,
            long *granularity);
    ASIOError (ASIO_METHOD *canSampleRate)(
            IASIO *self,
            ASIOSampleRate sampleRate);
    ASIOError (ASIO_METHOD *getSampleRate)(
            IASIO *self,
            ASIOSampleRate *sampleRate);
    ASIOError (ASIO_METHOD *setSampleRate)(
            IASIO *self,
            ASIOSampleRate sampleRate);
    ASIOError (ASIO_METHOD *getClockSources)(
            IASIO *self,
            ASIOClockSource *clocks,
            long *numSources);
    ASIOError (ASIO_METHOD *setClockSource)(IASIO *self, long reference);
    ASIOError (ASIO_METHOD *getSamplePosition)(
            IASIO *self,
            ASIOSamples *sPos,
            ASIOTimeStamp *tStamp);
    ASIOError (ASIO_METHOD *getChannelInfo)(
            IASIO *self,
            ASIOChannelInfo *info);
    ASIOError (ASIO_METHOD *createBuffers)(
            IASIO *self,
            ASIOBufferInfo *bufferInfos,
            long numChannels,
            long bufferSize,
            ASIOCallbacks *callbacks);
    ASIOError (ASIO_METHOD *disposeBuffers)(IASIO *self);
    ASIOError (ASIO_METHOD *controlPanel)(IASIO *self);
    ASIOError (ASIO_METHOD *future)(IASIO *self, long selector, void *opt);
    ASIOError (ASIO_METHOD *outputReady)(IASIO *self);
};

struct IASIO {
    const struct IASIOVtbl *lpVtbl;
};

HRESULT asio_driver_open_by_name(
        IASIO **out,
        const char *driver_name);

HRESULT asio_driver_open_first(
        IASIO **out,
        char *name_out,
        size_t name_out_size);
