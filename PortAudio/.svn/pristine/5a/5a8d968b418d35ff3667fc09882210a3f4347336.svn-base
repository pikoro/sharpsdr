/*
 * $Id$
 * PortAudio Portable Real-Time Audio Library
 * Latest Version at: http://www.portaudio.com
 * sndio implementation by Alexandre Ratchov
 *
 * Copyright (c) 2009 Alexandre Ratchov <alex@caoua.org>
 *
 * Based on the Open Source API proposed by Ross Bencina
 * Copyright (c) 1999-2002 Ross Bencina, Phil Burk
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files
 * (the "Software"), to deal in the Software without restriction,
 * including without limitation the rights to use, copy, modify, merge,
 * publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF
 * CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

/*
 * The text above constitutes the entire PortAudio license; however,
 * the PortAudio community also makes the following non-binding requests:
 *
 * Any person wishing to distribute modifications to the Software is
 * requested to send the modifications to the original developer so that
 * they can be incorporated into the canonical version. It is also
 * requested that these non-binding requests be included along with the
 * license above.
 */

#include <sys/types.h>
#include <pthread.h>
#include <poll.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <sndio.h>

#include "pa_util.h"
#include "pa_hostapi.h"
#include "pa_stream.h"
#include "pa_process.h"
#include "pa_allocation.h"
#include "pa_debugprint.h"

/*
 * per-stream data
 */
typedef struct PaSndioStream
{
    PaUtilStreamRepresentation base;
    PaUtilBufferProcessor bufferProcessor;    /* format conversion */
    struct sio_hdl *hdl;                      /* handle for device i/o */
    struct sio_par par;                       /* device parameters */    
    unsigned mode;                            /* SIO_PLAY, SIO_REC or both */
    int stopped;                              /* stop requested */
    int active;                               /* call-back thread is running */
    unsigned long long framesProcessed;       /* frames h/w has processed */
    char *readBuffer;
    char *writeBuffer;
    unsigned long long bytesRead;
    unsigned long long bytesWritten;
    pthread_t thread;                         /* for the callback interface */
    struct pollfd *pfds;
    int nfds;
} PaSndioStream;

/*
 * api "class" data, common to all streams
 */
typedef struct PaSndioHostApiRepresentation
{
    PaUtilHostApiRepresentation base;
    PaUtilStreamInterface callback;
    PaUtilStreamInterface blocking;
    /*
     * sndio has no device discovery mechanism, so expose only
     * the default device, the user will have a chance to change it
     * using the environment variable
     */
    PaDeviceInfo *infos[1], default_info;
} PaSndioHostApiRepresentation;

/*
 * callback invoked when blocks are processed by the hardware
 */
static void sndioOnMove(void *addr, int delta)
{
    PaSndioStream *s = (PaSndioStream *)addr;

    s->framesProcessed += delta;
}

/*
 * convert PA encoding to sndio encoding, return true on success
 */
static int SampleFormatToSndioParameters(struct sio_par *sio, PaSampleFormat fmt)
{
    switch (fmt & ~paNonInterleaved) {
    case paInt32:
    case paFloat32:
        sio->sig = 1;
        sio->bits = 32;
        break;
    case paInt24:
        sio->sig = 1;
        sio->bits = 24;
        sio->bps = 3;    /* paInt24 is packed format */
        break;
    case paInt16:
        sio->sig = 1;
        sio->bits = 16;
        break;
    case paInt8:
        sio->sig = 1;
        sio->bits = 8;
        break;
    case paUInt8:
        sio->sig = 0;
        sio->bits = 8;
        break;
    default:
        PA_DEBUG(("SampleFormatToSndioParameters: %x: unsupported\n", fmt));
        return 0;
    }
    sio->le = SIO_LE_NATIVE;
    return 1;
}

/*
 * convert sndio encoding to PA encoding, return true on success
 */
static int SndioParametersToSampleFormat(struct sio_par *sio, PaSampleFormat *fmt)
{
    if ((sio->bps * 8 != sio->bits && !sio->msb) ||
        (sio->bps > 1 && sio->le != SIO_LE_NATIVE)) {
	PA_DEBUG(("SndioParametersToSampleFormat: bits = %u, le = %u, msb = %u, bps = %u\n",
	    sio->bits, sio->le, sio->msb, sio->bps));
        return 0;
    }

    switch (sio->bits) {
    case 32:
        if (!sio->sig)
            return 0;
        *fmt = paInt32;
        break;
    case 24:
        if (!sio->sig)
            return 0;
        *fmt = (sio->bps == 3) ? paInt24 : paInt32;
        break;
    case 16:
        if (!sio->sig)
            return 0;
        *fmt = paInt16;
        break;
    case 8:
        *fmt = sio->sig ? paInt8 : paUInt8;
        break;
    default:
        PA_DEBUG(("SndioParametersToSampleFormat: %u: unsupported\n", sio->bits));
        return 0;
    }
    return 1;
}

/*
 * I/O loop for callback interface
 */
static void *sndioThread(void *arg)
{
    PaSndioStream *s = (PaSndioStream *)arg;
    PaStreamCallbackTimeInfo ti;
    unsigned char *data;
    unsigned todo, rblksz, wblksz;
    int n, result;
    
    rblksz = s->par.round * s->par.rchan * s->par.bps;
    wblksz = s->par.round * s->par.pchan * s->par.bps;
    
    PA_DEBUG(("sndioThread: mode = %x, round = %u, rblksz = %u, wblksz = %u\n",
	s->mode, s->par.round, rblksz, wblksz));
    
    while (!s->stopped) {
        if (s->mode & SIO_REC) {
            todo = rblksz;
            data = s->readBuffer;
            while (todo > 0) {
                n = sio_read(s->hdl, data, todo);
                if (n == 0) {
                    PA_DEBUG(("sndioThread: sio_read failed\n"));
                    goto failed;
                }
                todo -= n;
                data += n;
            }
            s->bytesRead += s->par.round;
            ti.inputBufferAdcTime = 
                (double)s->framesProcessed / s->par.rate;
        }
        if (s->mode & SIO_PLAY) {
            ti.outputBufferDacTime =
                (double)(s->framesProcessed + s->par.bufsz) / s->par.rate;
        }
        ti.currentTime = s->framesProcessed / (double)s->par.rate;
        PaUtil_BeginBufferProcessing(&s->bufferProcessor, &ti, 0);
        if (s->mode & SIO_PLAY) {
            PaUtil_SetOutputFrameCount(&s->bufferProcessor, s->par.round);
            PaUtil_SetInterleavedOutputChannels(&s->bufferProcessor,
                0, s->writeBuffer, s->par.pchan);
        }
        if (s->mode & SIO_REC) {
            PaUtil_SetInputFrameCount(&s->bufferProcessor, s->par.round);
            PaUtil_SetInterleavedInputChannels(&s->bufferProcessor,
                0, s->readBuffer, s->par.rchan);
        }
        result = paContinue;
        n = PaUtil_EndBufferProcessing(&s->bufferProcessor, &result);
        if (n != s->par.round) {
            PA_DEBUG(("sndioThread: %d < %u frames, result = %d\n",
		      n, s->par.round, result));
        }
        if (result != paContinue) {
            break;
        }
        if (s->mode & SIO_PLAY) {
            n = sio_write(s->hdl, s->writeBuffer, wblksz);
            if (n < wblksz) {
                PA_DEBUG(("sndioThread: sio_write failed\n"));
                goto failed;
            }
            s->bytesWritten += s->par.round;
        }
    }
 failed:
    s->active = 0;
    PA_DEBUG(("sndioThread: done\n"));
}

static PaError OpenStream(struct PaUtilHostApiRepresentation *hostApi,
    PaStream **stream,
    const PaStreamParameters *inputPar,
    const PaStreamParameters *outputPar,
    double sampleRate,
    unsigned long framesPerBuffer,
    PaStreamFlags streamFlags,
    PaStreamCallback *streamCallback,
    void *userData)
{
    PaSndioHostApiRepresentation *sndioHostApi = (PaSndioHostApiRepresentation *)hostApi;
    PaSndioStream *s;
    PaError err;
    struct sio_hdl *hdl;
    struct sio_par par;
    unsigned mode;
    int readChannels, writeChannels;
    PaSampleFormat readFormat, writeFormat, deviceFormat;

    PA_DEBUG(("OpenStream:\n"));

    mode = 0;
    readChannels = writeChannels = 0;
    readFormat = writeFormat = 0;
    sio_initpar(&par);

    if (outputPar && outputPar->channelCount > 0) {
        if (outputPar->device != 0) {
            PA_DEBUG(("OpenStream: %d: bad output device\n", outputPar->device));
            return paInvalidDevice;
        }
        if (outputPar->hostApiSpecificStreamInfo) {
            PA_DEBUG(("OpenStream: output specific info\n"));
            return paIncompatibleHostApiSpecificStreamInfo;
        }
        if (!SampleFormatToSndioParameters(&par, outputPar->sampleFormat)) {
            return paSampleFormatNotSupported;
        }
        writeFormat = outputPar->sampleFormat;
        writeChannels = par.pchan = outputPar->channelCount;
        mode |= SIO_PLAY;
    }
    if (inputPar && inputPar->channelCount > 0) {
        if (inputPar->device != 0) {
            PA_DEBUG(("OpenStream: %d: bad input device\n", inputPar->device));
            return paInvalidDevice;
        }
        if (inputPar->hostApiSpecificStreamInfo) {
            PA_DEBUG(("OpenStream: input specific info\n"));
            return paIncompatibleHostApiSpecificStreamInfo;
        }
        if (!SampleFormatToSndioParameters(&par, inputPar->sampleFormat)) {
            return paSampleFormatNotSupported;
        }
        readFormat = inputPar->sampleFormat;
        readChannels = par.rchan = inputPar->channelCount;
        mode |= SIO_REC;
    }
    par.rate = sampleRate;
    if (framesPerBuffer != paFramesPerBufferUnspecified)
        par.round = framesPerBuffer;

    PA_DEBUG(("OpenStream: mode = %x, trying rate = %u\n", mode, par.rate));

    hdl = sio_open(SIO_DEVANY, mode, 0);
    if (hdl == NULL)
        return paUnanticipatedHostError;
    if (!sio_setpar(hdl, &par)) {
        sio_close(hdl);
        return paUnanticipatedHostError;
    }
    if (!sio_getpar(hdl, &par)) {
        sio_close(hdl);
        return paUnanticipatedHostError;
    }
    if (!SndioParametersToSampleFormat(&par, &deviceFormat)) {
        sio_close(hdl);
        return paSampleFormatNotSupported;
    }
    if ((mode & SIO_REC) && par.rchan != inputPar->channelCount) {
        PA_DEBUG(("OpenStream: rchan(%u) != %d\n", par.rchan, inputPar->channelCount));
        sio_close(hdl);
        return paInvalidChannelCount;
    }
    if ((mode & SIO_PLAY) && par.pchan != outputPar->channelCount) {
        PA_DEBUG(("OpenStream: pchan(%u) != %d\n", par.pchan, outputPar->channelCount));
        sio_close(hdl);
        return paInvalidChannelCount;
    }
    if ((double)par.rate < sampleRate * 0.995 ||
        (double)par.rate > sampleRate * 1.005) {
        PA_DEBUG(("OpenStream: rate(%u) != %g\n", par.rate, sampleRate));
        sio_close(hdl);
        return paInvalidSampleRate;
    }
    
    s = (PaSndioStream *)PaUtil_AllocateMemory(sizeof(PaSndioStream));
    if (s == NULL) {
        sio_close(hdl);
        return paInsufficientMemory;
    }
    PaUtil_InitializeStreamRepresentation(&s->base, 
        streamCallback ? &sndioHostApi->callback : &sndioHostApi->blocking,
        streamCallback, userData);
    PA_DEBUG(("readChannels = %d, writeChannels = %d, readFormat = %x, writeFormat = %x\n", 
        readChannels, writeChannels, readFormat, writeFormat));
    err = PaUtil_InitializeBufferProcessor(&s->bufferProcessor,
        readChannels, readFormat, deviceFormat,
        writeChannels, writeFormat, deviceFormat,
        sampleRate,
        streamFlags,
        framesPerBuffer,
        par.round,
        paUtilFixedHostBufferSize, 
        streamCallback, userData);
    if (err) {
        PA_DEBUG(("OpenStream: PaUtil_InitializeBufferProcessor failed\n"));
        PaUtil_FreeMemory(s);
        sio_close(hdl);
        return err;
    }
    if (mode & SIO_REC) {
        s->readBuffer = malloc(par.round * par.rchan * par.bps);
        if (s->readBuffer == NULL) {
            PA_DEBUG(("OpenStream: failed to allocate readBuffer\n"));
            PaUtil_FreeMemory(s);
            sio_close(hdl);
            return paInsufficientMemory;
        }
    }
    if (mode & SIO_PLAY) {
        s->writeBuffer = malloc(par.round * par.pchan * par.bps);
        if (s->writeBuffer == NULL) {
            PA_DEBUG(("OpenStream: failed to allocate writeBuffer\n"));
            free(s->readBuffer);
            PaUtil_FreeMemory(s);
            sio_close(hdl);
            return paInsufficientMemory;
        }
    }
    s->nfds = sio_nfds(hdl);
    s->pfds = malloc(sizeof(struct pollfd) * s->nfds);
    s->base.streamInfo.inputLatency = 0;
    s->base.streamInfo.outputLatency = (mode & SIO_PLAY) ?
        (double)(par.bufsz + PaUtil_GetBufferProcessorOutputLatencyFrames(&s->bufferProcessor)) / (double)par.rate : 0;
    s->base.streamInfo.sampleRate = par.rate;
    s->active = 0;
    s->stopped = 1;
    s->mode = mode;
    s->hdl = hdl;
    s->par = par;
    *stream = s;    
    PA_DEBUG(("OpenStream: done\n"));
    return paNoError;
}

static PaError BlockingReadStream(PaStream *stream, void *data, unsigned long numFrames)
{
    PaSndioStream *s = (PaSndioStream *)stream;
    unsigned n, res, todo;
    void *buf;
    
    while (numFrames > 0) {
        n = s->par.round;
        if (n > numFrames)
            n = numFrames;
        buf = s->readBuffer;
        todo = n * s->par.rchan * s->par.bps;
        while (todo > 0) {
            res = sio_read(s->hdl, buf, todo);
            if (res == 0)
                return paUnanticipatedHostError;
            buf = (char *)buf + res;
            todo -= res;
        }
        s->bytesRead += n;
        PaUtil_SetInputFrameCount(&s->bufferProcessor, n);
        PaUtil_SetInterleavedInputChannels(&s->bufferProcessor, 0, s->readBuffer, s->par.rchan);
        res = PaUtil_CopyInput(&s->bufferProcessor, &data, n);
        if (res != n) {
            PA_DEBUG(("BlockingReadStream: copyInput: %u != %u\n"));
            return paUnanticipatedHostError;
        }
        numFrames -= n;
    }
    return paNoError;
}

static PaError BlockingWriteStream(PaStream* stream, const void *data, unsigned long numFrames)
{
    PaSndioStream *s = (PaSndioStream *)stream;
    unsigned n, res;

    while (numFrames > 0) {
        n = s->par.round;
        if (n > numFrames)
            n = numFrames;
        PaUtil_SetOutputFrameCount(&s->bufferProcessor, n);
        PaUtil_SetInterleavedOutputChannels(&s->bufferProcessor, 0, s->writeBuffer, s->par.pchan);
        res = PaUtil_CopyOutput(&s->bufferProcessor, &data, n);
        if (res != n) {
            PA_DEBUG(("BlockingWriteStream: copyOutput: %u != %u\n"));
            return paUnanticipatedHostError;
        }
        res = sio_write(s->hdl, s->writeBuffer, n * s->par.pchan * s->par.bps);
        if (res == 0)
            return paUnanticipatedHostError;        
        s->bytesWritten += n;
        numFrames -= n;
    }
    return paNoError;
}

static signed long BlockingGetStreamReadAvailable(PaStream *stream)
{
    PaSndioStream *s = (PaSndioStream *)stream;
    int n, events;

    n = sio_pollfd(s->hdl, s->pfds, POLLIN);
    while (poll(s->pfds, n, 0) < 0) {
        if (errno == EINTR)
            continue;
        perror("poll");
        abort();
    }
    events = sio_revents(s->hdl, s->pfds);
    if (!(events & POLLIN))
        return 0;

    return s->framesProcessed - s->bytesRead;
}

static signed long BlockingGetStreamWriteAvailable(PaStream *stream)
{
    PaSndioStream *s = (PaSndioStream *)stream;
    int n, events;

    n = sio_pollfd(s->hdl, s->pfds, POLLOUT);
    while (poll(s->pfds, n, 0) < 0) {
        if (errno == EINTR)
            continue;
        perror("poll");
        abort();
    }
    events = sio_revents(s->hdl, s->pfds);
    if (!(events & POLLOUT))
        return 0;

    return s->par.bufsz - (s->bytesWritten - s->framesProcessed);
}

static PaError BlockingWaitEmpty( PaStream *stream )
{
    PaSndioStream *s = (PaSndioStream *)stream;

    /*
     * drain playback buffers; sndio always does it in background
     * and there is no way to wait for completion
     */
    PA_DEBUG(("BlockingWaitEmpty: s=%d, a=%d\n", s->stopped, s->active));

    return paNoError;
}

static PaError StartStream(PaStream *stream)
{
    PaSndioStream *s = (PaSndioStream *)stream;
    unsigned primes, wblksz;
    int err;

    PA_DEBUG(("StartStream: s=%d, a=%d\n", s->stopped, s->active));

    if (!s->stopped) {
        PA_DEBUG(("StartStream: already started\n"));
        return paNoError;
    }
    s->stopped = 0;
    s->active = 1;
    s->framesProcessed = 0;
    s->bytesWritten = 0;
    s->bytesRead = 0;
    PaUtil_ResetBufferProcessor(&s->bufferProcessor);
    if (!sio_start(s->hdl))
        return paUnanticipatedHostError;

    /*
     * send a complete buffer of silence
     */
    if (s->mode & SIO_PLAY) {
        wblksz = s->par.round * s->par.pchan * s->par.bps;
        memset(s->writeBuffer, 0, wblksz);
        for (primes = s->par.bufsz / s->par.round; primes > 0; primes--)
            s->bytesWritten += sio_write(s->hdl, s->writeBuffer, wblksz);
    }
    if (s->base.streamCallback) {
        err = pthread_create(&s->thread, NULL, sndioThread, s);
        if (err) {
            PA_DEBUG(("SndioStartStream: couldn't create thread\n"));
            return paUnanticipatedHostError;
        }
        PA_DEBUG(("StartStream: started...\n"));
    }
    return paNoError;
}

static PaError StopStream(PaStream *stream)
{
    PaSndioStream *s = (PaSndioStream *)stream;
    void *ret;
    int err;

    PA_DEBUG(("StopStream: s=%d, a=%d\n", s->stopped, s->active));

    if (s->stopped) {
        PA_DEBUG(("StartStream: already started\n"));
        return paNoError;
    }
    s->stopped = 1;
    if (s->base.streamCallback) {
        err = pthread_join(s->thread, &ret);
        if (err) {
            PA_DEBUG(("SndioStop: couldn't join thread\n"));
            return paUnanticipatedHostError;
        }
    }
    if (!sio_stop(s->hdl))
        return paUnanticipatedHostError;
    return paNoError;
}

static PaError CloseStream(PaStream *stream)
{
    PaSndioStream *s = (PaSndioStream *)stream;

    PA_DEBUG(("CloseStream:\n"));

    if (!s->stopped)
        StopStream(stream);

    if (s->mode & SIO_REC)
        free(s->readBuffer);
    if (s->mode & SIO_PLAY)
        free(s->writeBuffer);
    sio_close(s->hdl);
        PaUtil_TerminateStreamRepresentation(&s->base);
    free(s->pfds);
    PaUtil_TerminateBufferProcessor(&s->bufferProcessor);
    PaUtil_FreeMemory(s);
    return paNoError;
}

static PaError AbortStream(PaStream *stream)
{
    PA_DEBUG(("AbortStream:\n"));

    return StopStream(stream);
}

static PaError IsStreamStopped(PaStream *stream)
{
    PaSndioStream *s = (PaSndioStream *)stream;

    //PA_DEBUG(("IsStreamStopped: s=%d, a=%d\n", s->stopped, s->active));

    return s->stopped;
}

static PaError IsStreamActive(PaStream *stream)
{
    PaSndioStream *s = (PaSndioStream *)stream;

    //PA_DEBUG(("IsStreamActive: s=%d, a=%d\n", s->stopped, s->active));

    return s->active;
}

static PaTime GetStreamTime(PaStream *stream)
{
    PaSndioStream *s = (PaSndioStream *)stream;

    return (double)s->framesProcessed / s->base.streamInfo.sampleRate;
}

static PaError IsFormatSupported(struct PaUtilHostApiRepresentation *hostApi,
    const PaStreamParameters *inputPar,
    const PaStreamParameters *outputPar,
    double sampleRate)
{
    return paFormatIsSupported;
}

static void Terminate(struct PaUtilHostApiRepresentation *hostApi)
{
    PaUtil_FreeMemory(hostApi);
}

PaError PaSndio_Initialize(PaUtilHostApiRepresentation **hostApi, PaHostApiIndex hostApiIndex)
{
    PaSndioHostApiRepresentation *sndioHostApi;
    PaDeviceInfo *info;
    struct sio_hdl *hdl;
    
    PA_DEBUG(("PaSndio_Initialize: initializing...\n"));

    /* unusable APIs should return paNoError and a NULL hostApi */
    *hostApi = NULL;

    sndioHostApi = PaUtil_AllocateMemory(sizeof(PaSndioHostApiRepresentation));
    if (sndioHostApi == NULL)
        return paNoError;

    info = &sndioHostApi->default_info;
    info->structVersion = 2;
    info->name = "default";
    info->hostApi = hostApiIndex;
    info->maxInputChannels = 128;
    info->maxOutputChannels = 128;
    info->defaultLowInputLatency = 0.01;
    info->defaultLowOutputLatency = 0.01;
    info->defaultHighInputLatency = 0.5;
    info->defaultHighOutputLatency = 0.5;
    info->defaultSampleRate = 48000;
    sndioHostApi->infos[0] = info;
    
    *hostApi = &sndioHostApi->base;
    (*hostApi)->info.structVersion = 1;
    (*hostApi)->info.type = paSndio;
    (*hostApi)->info.name = "sndio";
    (*hostApi)->info.deviceCount = 1;
    (*hostApi)->info.defaultInputDevice = 0;
    (*hostApi)->info.defaultOutputDevice = 0;
    (*hostApi)->deviceInfos = sndioHostApi->infos;
    (*hostApi)->Terminate = Terminate;
    (*hostApi)->OpenStream = OpenStream;
    (*hostApi)->IsFormatSupported = IsFormatSupported;
    
    PaUtil_InitializeStreamInterface(&sndioHostApi->blocking,
        CloseStream,
        StartStream,
        StopStream,
        AbortStream,
        IsStreamStopped,
        IsStreamActive,
        GetStreamTime,
        PaUtil_DummyGetCpuLoad,
        BlockingReadStream,
        BlockingWriteStream,
        BlockingGetStreamReadAvailable,
        BlockingGetStreamWriteAvailable);

    PaUtil_InitializeStreamInterface(&sndioHostApi->callback,
        CloseStream,
        StartStream,
        StopStream,
        AbortStream,
        IsStreamStopped,
        IsStreamActive,
        GetStreamTime,
        PaUtil_DummyGetCpuLoad,
        PaUtil_DummyRead,
        PaUtil_DummyWrite,
        PaUtil_DummyGetReadAvailable,
        PaUtil_DummyGetWriteAvailable);

    PA_DEBUG(("PaSndio_Initialize: done\n"));
    return paNoError;
}
