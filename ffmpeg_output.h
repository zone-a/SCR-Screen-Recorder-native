#ifndef SCREENREC_FFMPEG_OUTPUT_H
#define SCREENREC_FFMPEG_OUTPUT_H

#include "screenrec.h"

#include <math.h>

#include <media/AudioRecord.h>
#include <media/AudioSystem.h>

using namespace android;

extern "C" {
#include <libavutil/opt.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavformat/url.h>
#include <libavutil/channel_layout.h>
#include <libavutil/common.h>
#include <libavutil/imgutils.h>
#include <libavutil/mathematics.h>
#include <libavutil/samplefmt.h>
#include <libswscale/swscale.h>
}

AVCodec *codec;
AVCodec *audioCodec;
AVCodecContext *c= NULL;
AVOutputFormat *fmt;
AVFormatContext *oc;
AVStream *videoStream;
AVStream *audioStream;
int audioFrameSize;
float *audioSamples;

AudioRecord *audioRecord;

float t, tincr, tincr2;

AVFrame *frame, *inframe;
AVPacket pkt;
uint8_t endcode[] = { 0, 0, 1, 0xb7 };
int frame_count = 1;
int64_t ptsOffset = 0;

int64_t getTimeMs();
void setupAudio();
status_t writeAudioFrame();
void audioRecordCallback(int event, void* user, void *info);

#endif