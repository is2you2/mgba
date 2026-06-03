#include <emscripten.h>
#include <mgba/core/core.h>
#include <mgba/core/interface.h>
#include <mgba/core/config.h>
#include <mgba/core/serialize.h>
#include <mgba-util/vfs.h>
#include <mgba-util/audio-buffer.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static struct mCore* core = NULL;
static uint32_t* videoBuffer = NULL;
static unsigned videoWidth = 0;
static unsigned videoHeight = 0;
static uint64_t frameCount = 0;

EMSCRIPTEN_KEEPALIVE
int mgba_init() {
    return 1;
}

EMSCRIPTEN_KEEPALIVE
int mgba_load_rom(uint8_t* buffer, size_t size) {
    if (core) {
        core->deinit(core);
        core = NULL;
    }

    struct VFile* vf = VFileMemChunk(buffer, size);
    if (!vf) return 0;

    core = mCoreFindVF(vf);
    if (!core) {
        vf->close(vf);
        return 0;
    }

    mCoreInitConfig(core, NULL);
    mCoreConfigSetDefaultValue(&core->config, "useBios", "no");
    mCoreConfigSetDefaultValue(&core->config, "skipBios", "yes");
    mCoreConfigSetDefaultIntValue(&core->config, "volume", 256);
    mCoreConfigSetDefaultValue(&core->config, "mute", "no");
    
    core->init(core);
    mCoreLoadConfig(core);

    if (!core->loadROM(core, vf)) {
        core->deinit(core);
        core = NULL;
        return 0;
    }
    
    core->reset(core);

    core->currentVideoSize(core, &videoWidth, &videoHeight);
    if (videoBuffer) free(videoBuffer);
    videoBuffer = (uint32_t*)malloc(videoWidth * videoHeight * sizeof(uint32_t));
    memset(videoBuffer, 0, videoWidth * videoHeight * sizeof(uint32_t));
    core->setVideoBuffer(core, (mColor*)videoBuffer, videoWidth);
    
    if (core->reloadConfigOption) {
        core->reloadConfigOption(core, "hwaccelVideo", NULL);
    }

    core->setAudioBufferSize(core, 16384);
    
    frameCount = 0;
    printf("WASM: ROM Loaded. Resolution: %ux%u\n", videoWidth, videoHeight);

    return 1;
}

EMSCRIPTEN_KEEPALIVE
void mgba_run_frame() {
    if (core) {
        core->runFrame(core);
        frameCount++;
        if (videoBuffer) {
            for (unsigned i = 0; i < videoWidth * videoHeight; ++i) {
                videoBuffer[i] |= 0xFF000000;
            }
        }
    }
}

EMSCRIPTEN_KEEPALIVE
uint32_t* mgba_get_video_buffer() {
    return videoBuffer;
}

EMSCRIPTEN_KEEPALIVE
int mgba_get_audio_samples(int16_t* outBuffer, size_t maxSamples) {
    if (!core) return 0;
    struct mAudioBuffer* audio = core->getAudioBuffer(core);
    if (!audio) return 0;
    size_t available = mAudioBufferAvailable(audio);
    if (available > maxSamples) available = maxSamples;
    mAudioBufferRead(audio, outBuffer, available);
    return (int)available;
}

EMSCRIPTEN_KEEPALIVE
unsigned mgba_get_audio_sample_rate() {
    if (!core) return 0;
    return core->audioSampleRate(core);
}

EMSCRIPTEN_KEEPALIVE
void mgba_set_volume(int volume) {
    if (core) {
        core->opts.volume = volume;
        if (core->reloadConfigOption) {
            core->reloadConfigOption(core, "volume", NULL);
        }
    }
}

EMSCRIPTEN_KEEPALIVE
unsigned mgba_get_width() { return videoWidth; }
EMSCRIPTEN_KEEPALIVE
unsigned mgba_get_height() { return videoHeight; }

EMSCRIPTEN_KEEPALIVE
void mgba_set_button(int button, int pressed) {
    if (core) {
        if (pressed) core->addKeys(core, 1 << button);
        else core->clearKeys(core, 1 << button);
    }
}

EMSCRIPTEN_KEEPALIVE
uint8_t* mgba_save_state(size_t* outSize) {
    if (!core) return NULL;
    size_t bufferSize = 2 * 1024 * 1024;
    uint8_t* buffer = (uint8_t*)malloc(bufferSize);
    struct VFile* vf = VFileFromMemory(buffer, bufferSize);
    if (!mCoreSaveStateNamed(core, vf, SAVESTATE_SCREENSHOT)) {
        vf->close(vf);
        free(buffer);
        return NULL;
    }
    *outSize = vf->size(vf);
    vf->close(vf);
    return buffer;
}

EMSCRIPTEN_KEEPALIVE
int mgba_load_state(uint8_t* buffer, size_t size) {
    if (!core) return 0;
    struct VFile* vf = VFileFromConstMemory(buffer, size);
    bool success = mCoreLoadStateNamed(core, vf, SAVESTATE_SCREENSHOT);
    vf->close(vf);
    return success ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE
void mgba_free_buffer(void* ptr) {
    free(ptr);
}
