/*
 * patest_sine_each_channel.c
 *
 * This program uses the PortAudio Portable Audio Library.
 * For more information see: http://www.portaudio.com/
 * Copyright (c) 1999-2000 Ross Bencina and Phil Burk
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

/** @file patest_sine_each_channel.c
	@ingroup test_src
	@brief Plays sine waves with different frequencies on all available channels
	@author Robert Bielik <robert@xponaut.com>
*/

#include <stdio.h>
#include <math.h>
#include "portaudio.h"
#include "pa_util.h"

#ifndef _WIN32
/* Emulates _kbhit on Unix-like platform (http://stackoverflow.com/questions/448944/c-non-blocking-keyboard-input) */
#include <sys/select.h>
int _kbhit()
{
    struct timeval tv = { 0L, 0L };
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(0, &fds);
    return select(1, &fds, NULL, NULL, &tv);
}
#else
#include <conio.h>
#endif

#define NUM_SECONDS_PER_CHANNEL   (1)
#define SAMPLE_RATE   (44100)
#define FRAMES_PER_BUFFER  (64)

#ifndef M_PI
#define M_PI  (3.14159265)
#endif

#define TABLE_SIZE   (4096)
float gSineTable[TABLE_SIZE+1];

const unsigned gTableMask = TABLE_SIZE-1;

typedef struct
{
    float phase_;
    float phaseIncr_;
    unsigned cntr;
}
paChannelData;

typedef struct  
{
    paChannelData* channels;
    unsigned currentChannel;
    unsigned noOfChannels;
} 
paTestData;


/* This routine will be called by the PortAudio engine when audio is needed.
** It may called at interrupt level on some machines so don't do anything
** that could mess up the system like calling malloc() or free().
*/
static int patestCallback( const void *inputBuffer, void *outputBuffer,
                            unsigned long framesPerBuffer,
                            const PaStreamCallbackTimeInfo* timeInfo,
                            PaStreamCallbackFlags statusFlags,
                            void *userData )
{
    const unsigned kCount = SAMPLE_RATE * NUM_SECONDS_PER_CHANNEL;
    paTestData *data = (paTestData*)userData;
    unsigned long i = 0;

    (void) timeInfo; /* Prevent unused variable warnings. */
    (void) statusFlags;
    (void) inputBuffer;
    

wrap:
    {
        paChannelData* channel = data->channels + data->currentChannel;
        float * out = ((float*)outputBuffer) + data->currentChannel;
        for(; i<framesPerBuffer; ++i, out += data->noOfChannels)
        {
            unsigned whole = (unsigned)channel->phase_;
            const float fraction = channel->phase_ - whole;
            float value;
            whole &= gTableMask;

            value = (gSineTable[whole+1] - gSineTable[whole]) * fraction + gSineTable[whole];
            *out = value * sinf((M_PI * channel->cntr)/ kCount);

            channel->phase_ += channel->phaseIncr_;
            if (++channel->cntr >= kCount)
            {
                if (++data->currentChannel >= data->noOfChannels) {
                    data->currentChannel = 0;
                }
                /* Reset current channel */
                channel->cntr = 0;
                channel->phase_ = 0;

                goto wrap;
            }
        }
    }
    
    return paContinue;
}

/*******************************************************************/
int main(void);
int main(void)
{
    PaStreamParameters outputParameters;
    PaStream *stream;
    const PaDeviceInfo *info;
    PaError err;
    paTestData data = {0};
    unsigned i;
    
    printf("PortAudio Test: output sine wave on each available channel. SR = %d, BufSize = %d\n", SAMPLE_RATE, FRAMES_PER_BUFFER);
    
    /* initialise sinusoidal wavetable */
    for( i=0; i<=TABLE_SIZE; i++ )
    {
        gSineTable[i] = (float) sin( ((double)i/(double)TABLE_SIZE) * M_PI * 2. );
    }

    
    err = Pa_Initialize();
    if( err != paNoError ) goto error;

    outputParameters.device = Pa_GetDefaultOutputDevice(); /* default output device */
    if (outputParameters.device == paNoDevice) {
        fprintf(stderr,"Error: No default output device.\n");
        goto error;
    }
    info = Pa_GetDeviceInfo( outputParameters.device );

    outputParameters.channelCount = info->maxOutputChannels;       /* ALL outputs */
    if (outputParameters.channelCount == 0) {
        fprintf(stderr,"Error: No output channels!\n");
        goto error;
    }

    outputParameters.sampleFormat = paFloat32; /* 32 bit floating point output, interleaved */
    outputParameters.suggestedLatency = info->defaultLowOutputLatency;
    outputParameters.hostApiSpecificStreamInfo = NULL;

    data.noOfChannels = outputParameters.channelCount;
    data.channels = (paChannelData*)PaUtil_AllocateMemory(data.noOfChannels * sizeof(paChannelData));
    if (data.channels == 0) {
        fprintf(stderr,"Error: Failed to allocation memory.\n");
        goto error;
    }

    for (i = 0; i < data.noOfChannels; ++i)
    {
        paChannelData* channel = data.channels + i;
        channel->cntr = 0;
        channel->phase_ = 0.f;
        channel->phaseIncr_ = (440.0f * powf(4.f, i / 12.f)) * TABLE_SIZE / SAMPLE_RATE;
    }

    printf("Using device '%s' with %d channels\n", info->name, info->maxOutputChannels);

    err = Pa_OpenStream(
              &stream,
              NULL, /* no input */
              &outputParameters,
              SAMPLE_RATE,
              FRAMES_PER_BUFFER,
              paClipOff,      /* we won't output out of range samples so don't bother clipping them */
              patestCallback,
              &data );
    if( err != paNoError ) goto error;

    err = Pa_StartStream( stream );
    if( err != paNoError ) goto error;

    printf("Hit any key to stop...\n");
    {
        unsigned channelOld = (unsigned)-1;
        while (_kbhit() == 0) {
            if (data.currentChannel != channelOld) 
            {
                printf("Playing on channel %u\n", data.currentChannel + 1);
                channelOld = data.currentChannel;
            }
            Pa_Sleep(20);
        }
    }

    err = Pa_StopStream( stream );
    if( err != paNoError ) goto error;

    err = Pa_CloseStream( stream );
    if( err != paNoError ) goto error;

    Pa_Terminate();
    printf("Test finished.\n");
    
    return err;
error:
    if (data.channels) 
    {
        PaUtil_FreeMemory(data.channels);
    }
    Pa_Terminate();
    fprintf( stderr, "An error occured while using the portaudio stream\n" );
    fprintf( stderr, "Error number: %d\n", err );
    fprintf( stderr, "Error message: %s\n", Pa_GetErrorText( err ) );
    return err;
}
