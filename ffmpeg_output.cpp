#include "ffmpeg_output.h"

void FFmpegOutput::setupOutput() {
    int ret;

    loadFFmpegComponents();
    setupOutputContext();

    setupVideoStream();
    setupFrames();

    if (audioSource != SCR_AUDIO_MUTE) {
        setupAudioOutput();
    }

    setupOutputFile();

    if (audioSource != SCR_AUDIO_MUTE) {
        startAudioInput();
    }

    startTimeMs = getTimeMs();

    mrRunning = true;

    pthread_mutex_lock(&frameReadyMutex);
    pthread_create(&encodingThread, NULL, FFmpegOutput::encodingThreadStart, this);
}


void FFmpegOutput::loadFFmpegComponents() {
    extern AVCodec ff_mpeg4_encoder;
    avcodec_register(&ff_mpeg4_encoder);

    if (audioSource != SCR_AUDIO_MUTE) {
        extern AVCodec ff_aac_encoder;
        avcodec_register(&ff_aac_encoder);
    }

    extern AVOutputFormat ff_mp4_muxer;
    av_register_output_format(&ff_mp4_muxer);

    extern URLProtocol ff_file_protocol;
    ffurl_register_protocol(&ff_file_protocol, sizeof(ff_file_protocol));
}


void FFmpegOutput::setupOutputContext() {
    avformat_alloc_output_context2(&oc, NULL, NULL, outputName);
    if (!oc) {
        stop(234, "Can't alloc output context");
    }
}


void FFmpegOutput::setupVideoStream() {
    AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_MPEG4);

    if (!codec) {
        stop(235, "Video codec not found");
    }

    videoStream = avformat_new_stream(oc, codec);
    if (!videoStream) {
        stop(234, "Could not allocate video stream");
    }

    AVCodecContext *c = videoStream->codec;

    c->bit_rate = videoBitrate;
    c->width = videoWidth;
    c->height = videoHeight;
    /* frames per second */
    c->time_base= (AVRational){1,10};
    c->gop_size = 10; /* emit one intra frame every ten frames */
    c->max_b_frames=1;
    c->pix_fmt = AV_PIX_FMT_YUV420P;
    c->thread_count = 4;
    c->mb_decision = 2;

    int rot = rotation;
    if (rot) {
        char value[16];
        sprintf(value, "%d", rot);
        av_dict_set(&videoStream->metadata, "rotate", value, 0);
    }

    /* Some formats want stream headers to be separate. */
    if (oc->oformat->flags & AVFMT_GLOBALHEADER)
        c->flags |= CODEC_FLAG_GLOBAL_HEADER;

    if (avcodec_open2(c, codec, NULL) < 0) {
        ALOGE("Could not open video codec");
        stop(235, "Could not open video codec");
    }
}

void FFmpegOutput::setupFrames() {
    frames[0] = createFrame();
    frames[1] = createFrame();
}


AVFrame *FFmpegOutput::createFrame() {
    int ret;
    AVCodecContext *c = videoStream->codec;
    AVFrame * frame = avcodec_alloc_frame();
    if (!frame) {
        stop(234, "Could not allocate video frame");
    }
    frame->format = c->pix_fmt;
    frame->width  = c->width;
    frame->height = c->height;

    ret = av_image_alloc(frame->data, frame->linesize, c->width, c->height, c->pix_fmt, 32);
    if (ret < 0) {
        stop(234, "Could not allocate raw picture buffer");
    }
    memset(frame->data[1], 128, c->height / 2 * frame->linesize[1]);
    memset(frame->data[2], 128, c->height / 2 * frame->linesize[2]);
    return frame;
}

void FFmpegOutput::setupAudioOutput() {
    int ret;

    AVCodec *audioCodec = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if (!audioCodec) {
        stop(235, "AAC Codec not found");
    }

    audioStream = avformat_new_stream(oc, audioCodec);
    if (!audioStream) {
        stop(236, "Could not allocate audio stream");
    }

    AVCodecContext *c = audioStream->codec;
    c->sample_fmt  = AV_SAMPLE_FMT_FLTP;
    c->bit_rate    = audioChannels * 64000;
    c->sample_rate = audioSamplingRate;
    c->channels    = audioChannels;
    c->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;

    /* Some formats want stream headers to be separate. */
    if (oc->oformat->flags & AVFMT_GLOBALHEADER)
        c->flags |= CODEC_FLAG_GLOBAL_HEADER;

    /* open it */
    ret = avcodec_open2(c, audioCodec, NULL);
    if (ret < 0) {
        stop(235, "Could not open audio codec");
    }
    audioFrameSize = c->frame_size;

    outSamples = (float*) av_malloc(audioFrameSize *
                        av_get_bytes_per_sample(c->sample_fmt) *
                        c->channels);
    if (!outSamples) {
        stop(236, "Could not allocate audio samples buffer");
    }

    audioStream->time_base= (AVRational){1,audioSamplingRate};
}


void FFmpegOutput::setupOutputFile() {
    int ret;
    ret = avio_open(&oc->pb, outputName, AVIO_FLAG_WRITE);
    if (ret < 0 && fixOutputName()) {
        ret = avio_open(&oc->pb, outputName, AVIO_FLAG_WRITE);
    }
    if (ret < 0) {
        stop(201, "Could not open the output file");
    }

    /* Write the stream header, if any. */
    ret = avformat_write_header(oc, NULL);
    if (ret < 0) {
        stop(247, "Error occurred when writing file header");
    }
}

void FFmpegOutput::startAudioInput() {
    int err;

    inSamplesSize = audioSamplingRate; // buffer up to one second of input audio data
    inSamples = new float[inSamplesSize];

    audioRecord = new AudioRecord(AUDIO_SOURCE_MIC,
                        audioSamplingRate,
                        AUDIO_FORMAT_PCM_16_BIT,
                        audioChannels == 2 ? AUDIO_CHANNEL_IN_STEREO : AUDIO_CHANNEL_IN_MONO,
    #if SCR_SDK_VERSION >= 23
                        String16("com.iwobanas.screenrecorder.pro"),
    #endif // SCR_SDK_VERSION >= 23
                        0,
    #if SCR_SDK_VERSION < 17
                        (AudioRecord::record_flags) 0,
    #endif // SCR_SDK_VERSION < 17
                        &staticAudioRecordCallback,
                        this);

    err = audioRecord->initCheck();
    if (err != NO_ERROR) {
        stop(250, "audioRecord->initCheck() failed");
        return;
    }
    err = audioRecord->start();
    if (err != NO_ERROR) {
        stop(237, "Can't start audio source");
    }
    audioRecordStarted = true;
}

static void staticAudioRecordCallback(int event, void* user, void *info) {
   FFmpegOutput *output = (FFmpegOutput*)user;
   output->audioRecordCallback(event, info);
}

void FFmpegOutput::audioRecordCallback(int event, void *info) {
    if (event != 0) return;

    AudioRecord::Buffer *buffer = (AudioRecord::Buffer*) info;

    pthread_mutex_lock(&inSamplesMutex);
    for (unsigned int i = 0; i < buffer->frameCount * audioChannels; i++) {
        inSamples[inSamplesEnd++] = (float)buffer->i16[i] / 32769.0;
        inSamplesEnd %= inSamplesSize;
        if (inSamplesEnd == inSamplesStart) {
            ALOGE("SCR audio buffer overrun");
            //TODO: handle overrun
        }
    }
    pthread_mutex_unlock(&inSamplesMutex);
}

inline int FFmpegOutput::availableSamplesCount() {
    return ((inSamplesSize + inSamplesEnd - inSamplesStart) % inSamplesSize) / audioChannels;
}

void FFmpegOutput::getAudioFrame()
{
    int samplesWritten = 0;

    pthread_mutex_lock(&inSamplesMutex);
    while (samplesWritten < audioFrameSize && availableSamplesCount() > 0) {
        outSamples[samplesWritten] = inSamples[inSamplesStart++];
        inSamplesStart %= inSamplesSize;
        if (audioChannels == 2) {
            outSamples[audioFrameSize + samplesWritten] = inSamples[inSamplesStart++];
            inSamplesStart %= inSamplesSize;
        }
        samplesWritten++;
    }
    pthread_mutex_unlock(&inSamplesMutex);
}

void FFmpegOutput::writeAudioFrame() {
    AVCodecContext *c;
    AVPacket pkt;
    AVFrame *frame = avcodec_alloc_frame();
    int pktReceived, ret;

    av_init_packet(&pkt);
    pkt.data = NULL;    // packet data will be allocated by the encoder
    pkt.size = 0;
    c = audioStream->codec;

    getAudioFrame();

    frame->nb_samples = audioFrameSize;
    frame->pts = sampleCount;
    sampleCount += audioFrameSize;

    avcodec_fill_audio_frame(frame, c->channels, c->sample_fmt, (uint8_t *)outSamples,
                            audioFrameSize * av_get_bytes_per_sample(c->sample_fmt) * c->channels, 1);

    ret = avcodec_encode_audio2(c, &pkt, frame, &pktReceived);
    if (ret < 0) {
        stop(238, "Error encoding audio frame");
    }

    if (pktReceived) {
        pkt.stream_index = audioStream->index;

        pthread_mutex_lock(&outputWriteMutex);
        /* Write the compressed frame to the media file. */
        ret = av_interleaved_write_frame(oc, &pkt);
        pthread_mutex_unlock(&outputWriteMutex);
        if (ret != 0) {
            stop(246, "Error while writing audio frame");
        }
    }
    avcodec_free_frame(&frame);
    av_free_packet(&pkt);
}

void FFmpegOutput::writeVideoFrame() {
    pthread_mutex_lock(&frameEncMutex);
    videoFrame = frames[frameCount % 2];
    //fprintf(stderr, "Populate frame %d\n", (videoFrame == frames[0]) ? 0 : 1);fflush(stderr);

    int64_t ptsMs = getTimeMs() - startTimeMs;
    videoFrame->pts = av_rescale_q(ptsMs, (AVRational){1,1000}, videoStream->time_base);

    if (inputBase != NULL) {
        if (rotateView) {
            copyRotateYUVBuf(videoFrame->data, (uint8_t*)inputBase, videoFrame->linesize);
        } else {
            copyYUVBuf(videoFrame->data, (uint8_t*)inputBase, videoFrame->linesize);
        }
    }
    //fprintf(stderr, "Frame ready %d\n", (videoFrame == frames[0]) ? 0 : 1);fflush(stderr);
    pthread_mutex_unlock(&frameReadyMutex);
}

void* FFmpegOutput::encodingThreadStart(void* args) {
    FFmpegOutput *output = static_cast<FFmpegOutput*>(args);
    while (1) {
        pthread_mutex_lock(&output->frameReadyMutex);
        if (!mrRunning) {
            break;
        }
        AVFrame *f = output->videoFrame;
        pthread_mutex_unlock(&output->frameEncMutex);
        //fprintf(stderr, "Encode frame %d\n", (f == frames[0]) ? 0 : 1);fflush(stderr);
        output->encodeAndSaveVideoFrame(f);
        //fprintf(stderr, "Frame encoded %d\n", (f == frames[0]) ? 0 : 1);fflush(stderr);
    }
    pthread_exit(NULL);
    return NULL;
}

void FFmpegOutput::encodeAndSaveVideoFrame(AVFrame *frame) {

    int ret, pktReceived;
    AVPacket pkt;
    av_init_packet(&pkt);
    pkt.data = NULL;    // packet data will be allocated by the encoder
    pkt.size = 0;

    /* encode the image */
    ret = avcodec_encode_video2(videoStream->codec, &pkt, frame, &pktReceived);
    if (ret < 0) {
        stop(239, "Error encoding video frame");
    }

    if (pktReceived) {
        //fprintf(stderr, "VIDEO frame %3d (size=%5d)\n", frameCount, pkt.size);

        if (videoStream->codec->coded_frame->key_frame)
            pkt.flags |= AV_PKT_FLAG_KEY;

        pkt.stream_index = videoStream->index;

        pthread_mutex_lock(&outputWriteMutex);
        /* Write the compressed frame to the media file. */
        ret = av_interleaved_write_frame(oc, &pkt);
        pthread_mutex_unlock(&outputWriteMutex);
        if (ret != 0) {
            stop(240, "Error while writing video frame");
        }
    }
    av_free_packet(&pkt);
}

void FFmpegOutput::renderFrame() {
    updateInput();

    writeVideoFrame();

    while (audioSource != SCR_AUDIO_MUTE && availableSamplesCount() >= audioFrameSize) {
        writeAudioFrame();
    }
}

void FFmpegOutput::closeOutput(bool fromMainThread) {
    ALOGV("Closing FFmpeg output %d", fromMainThread);
    if (mrRunning) {
        mrRunning = false;
        pthread_mutex_unlock(&frameReadyMutex);
        pthread_join(encodingThread, NULL);
    }

    if (oc) {
        if (oc->pb) {
            ALOGV("Writing trailer");
            av_write_trailer(oc);
            avio_close(oc->pb);
        }

        if (videoStream) {
            ALOGV("streams and codecs cleanup");
            avcodec_close(videoStream->codec);
        }
        /* The process will terminate soon anyway we don't bother freeing this
        as it requires checking what's allocated to avoid SIGSEGV
        av_freep(&frames[0]->data[0]);
        avcodec_free_frame(&frames[0]);
        av_freep(&frames[1]->data[0]);
        avcodec_free_frame(&frames[1]); */


        /* free the stream */
        avformat_free_context(oc);
    }

    if (audioRecordStarted) {
        ALOGV("Stopping audio");
        audioRecord->stop();
        // don't free audioRecord as the destructor causes SIGSEGV on many devices
    }
    ALOGV("FFmpeg output closed");
}

void FFmpegOutput::copyRotateYUVBuf(uint8_t** yuvPixels, uint8_t* screen, int* stride) {
    for (int x = paddingWidth; x < videoWidth - paddingWidth; x++) {
        for (int y = videoHeight - paddingHeight - 1; y >= paddingHeight; y--) {
            int idx = ((x - paddingWidth) * inputStride + videoHeight - paddingHeight - y - 1) * 4;
            uint8_t r,g,b;
            if (useBGRA) {
                b = screen[idx];
                g = screen[idx + 1];
                r = screen[idx + 2];
            } else {
                r = screen[idx];
                g = screen[idx + 1];
                b = screen[idx + 2];
            }
            uint16_t Y = ( (  66 * r + 129 * g +  25 * b + 128) >> 8) +  16;
            yuvPixels[0][y * stride[0] + x] = Y;
            if (y % 2 == 0 && x % 2 == 0) {
                uint16_t U = ( ( -38 * r -  74 * g + 112 * b + 128) >> 8) + 128;
                uint16_t V = ( ( 112 * r -  94 * g -  18 * b + 128) >> 8) + 128;
                yuvPixels[1][y * stride[1] / 2 + x / 2 ] = U;
                yuvPixels[2][y * stride[2] / 2 + x / 2 ] = V;
            }
        }
    }
}

void FFmpegOutput::copyYUVBuf(uint8_t** yuvPixels, uint8_t* screen, int* stride) {
    for (int y = paddingHeight; y < videoHeight - paddingHeight; y++) {
        for (int x = paddingWidth; x < videoWidth - paddingWidth; x++) {

            int idx = ((y - paddingHeight) * inputStride + x - paddingWidth) * 4;
            uint8_t r,g,b;
            if (useBGRA) {
                b = screen[idx];
                g = screen[idx + 1];
                r = screen[idx + 2];
            } else {
                r = screen[idx];
                g = screen[idx + 1];
                b = screen[idx + 2];
            }
            uint16_t Y = ( (  66 * r + 129 * g +  25 * b + 128) >> 8) +  16;
            yuvPixels[0][y * stride[0] + x] = Y;
            if (y % 2 == 0 && x % 2 == 0) {
                uint16_t U = ( ( -38 * r -  74 * g + 112 * b + 128) >> 8) + 128;
                uint16_t V = ( ( 112 * r -  94 * g -  18 * b + 128) >> 8) + 128;
                yuvPixels[1][y * stride[1] / 2 + x / 2 ] = U;
                yuvPixels[2][y * stride[2] / 2 + x / 2 ] = V;
            }
        }
    }
}
