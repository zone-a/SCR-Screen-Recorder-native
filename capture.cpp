
#include "capture.h"

using namespace android;

void setupInput() {
#ifdef SCR_FB
    ALOGV("Setting up FB mmap");
    const char* fbpath = "/dev/graphics/fb0";
    fbFd = open(fbpath, O_RDONLY);

    if (fbFd < 0) {
        stop(202, "Error opening FB device");
    }

    if (ioctl(fbFd, FBIOGET_VSCREENINFO, &fbInfo) != 0) {
        stop(203, "FB ioctl failed");
    }

    int bytespp = fbInfo.bits_per_pixel / 8;

    size_t mapsize, size;
    size_t offset = (fbInfo.xoffset + fbInfo.yoffset * fbInfo.xres) * bytespp;
    inputWidth = fbInfo.xres;
    inputHeight = fbInfo.yres;
    inputStride = inputWidth;
    ALOGV("FB width: %d hieght: %d bytespp: %d", inputWidth, inputHeight, bytespp);

    size = inputWidth * inputHeight * bytespp;

    mapsize = size * 4; // For triple buffering 3 should be enough, setting to 4 for padding
    fbMapBase = mmap(0, mapsize, PROT_READ, MAP_SHARED, fbFd, 0);
    if (fbMapBase == MAP_FAILED) {
        stop(204, "mmap failed");
    }
    inputBase = (void const *)((char const *)fbMapBase + offset);
#else
    #if SCR_SDK_VERSION >= 17
    display = SurfaceComposerClient::getBuiltInDisplay(ISurfaceComposer::eDisplayIdMain);
    if (display == NULL) {
        stop(205, "Can't access display");
    }
    if (screenshot.update(display) != NO_ERROR) {
        stop(217, "screenshot.update() failed");
    }
    #else
    if (screenshot.update() != NO_ERROR) {
        stop(217, "screenshot.update() failed");
    }
    #endif // SCR_SDK_VERSION

    if ((screenshot.getWidth() < screenshot.getHeight()) != (reqWidth < reqHeight)) {
        ALOGI("swapping dimensions");
        int tmp = reqWidth;
        reqWidth = reqHeight;
        reqHeight = tmp;
    }
    screenshotUpdate(reqWidth, reqHeight);
    inputWidth = screenshot.getWidth();
    inputHeight = screenshot.getHeight();
    inputStride = screenshot.getStride();
    if (useOes) {
        screenshot.release();
    }
    ALOGV("Screenshot width: %d, height: %d, stride: %d, format %d, size: %d", inputWidth, inputHeight, inputStride, screenshot.getFormat(), screenshot.getSize());

#endif // SCR_FB

    if (allowVerticalFrames && inputWidth < inputHeight && (rotation == 0 || rotation == 180)) {
        swapPadding();
        videoWidth = inputWidth + 2 * paddingWidth;
        videoHeight = inputHeight + 2 * paddingHeight;
        rotateView = false;
    } else if (allowVerticalFrames && inputWidth > inputHeight && (rotation == 90 || rotation == 270)) {
        swapPadding();
        videoWidth = inputHeight + 2 * paddingWidth;
        videoHeight = inputWidth + 2 * paddingHeight;
        rotateView = true;
    } else {
        if (inputWidth > inputHeight) {
            videoWidth = inputWidth + 2 * paddingWidth;
            videoHeight = inputHeight + 2 * paddingHeight;
            rotateView = false;
        } else {
            videoWidth = inputHeight + 2 * paddingWidth;
            videoHeight = inputWidth + 2 * paddingHeight;
            rotateView = true;
        }
    }
}

void swapPadding() {
    int tmp = paddingWidth;
    paddingWidth = paddingHeight;
    paddingHeight = tmp;
}

void adjustRotation() {
    if (rotateView) {
        rotation = (rotation + 90) % 360;
    }
}


void updateInput() {
#ifdef SCR_FB
    // it's still flickering, maybe ioctl(fd, FBIO_WAITFORVSYNC, &crt); would help
    if (ioctl(fbFd, FBIOGET_VSCREENINFO, &fbInfo) != 0) {
        stop(223, "FB ioctl failed");
    }
    int bytespp = fbInfo.bits_per_pixel / 8;
    size_t offset = (fbInfo.xoffset + fbInfo.yoffset * fbInfo.xres) * bytespp;
    inputBase = (void const *)((char const *)fbMapBase + offset);
#else

    if (useOes) {
        #if SCR_SDK_VERSION >= 18
        if (glConsumer.get() != NULL) {
            glConsumer.clear();
        }
        glConsumer = new GLConsumer(1);
        glConsumer->setName(String8("scr_consumer"));
             if (ScreenshotClient::capture(display, glConsumer->getBufferQueue(),
                reqWidth, reqHeight, 0, -1) != NO_ERROR) {
            stop(227, "capture failed");
        }
        #endif // SCR_SDK_VERSION >= 18
    } else {
        screenshotUpdate(reqWidth, reqHeight);
        inputBase = screenshot.getPixels();
    }
#endif
}

void screenshotUpdate(int reqWidth, int reqHeight) {
    #ifndef SCR_FB
    #if SCR_SDK_VERSION >= 18
        screenshot.release();
    #endif
    #if SCR_SDK_VERSION >= 17
    if (screenshot.update(display, reqWidth, reqHeight) != NO_ERROR) {
        stop(217, "update failed");
    }
    #else
    if (screenshot.update(reqWidth, reqHeight) != NO_ERROR) {
        stop(217, "update failed");
    }
    #endif // SCR_SDK_VERSION
    #endif // ndef SCR_FB
}

void closeInput() {
#ifdef SCR_FB
    if (fbFd >= 0) {
        close(fbFd);
    fbFd = -1;
    }
#endif // SCR_FB
}


void updateTexImage() {
    #if SCR_SDK_VERSION >= 18 && !defined SCR_FB
    if (useOes) {
        if (glConsumer->updateTexImage() != NO_ERROR) {
            if (!stopping) {
                stop(226, "texture update failed");
            }
        }
    }
    #endif
}
