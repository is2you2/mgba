/* Copyright (c) 2026 Choi Sung soo
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include <emscripten.h>
#include <mgba/core/core.h>
#include <mgba/core/interface.h>
#include <mgba/core/config.h>
#include <mgba/core/serialize.h>
#include <mgba/core/lockstep.h>
#include <mgba/internal/gba/gba.h>
#include <mgba/internal/gba/sio.h>
#include <mgba/internal/gba/sio/lockstep.h>
#include <mgba-util/vfs.h>
#include <mgba-util/audio-buffer.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define MAX_PLAYERS 4

struct Player {
    struct mCore* core;
    uint32_t* videoBuffer;
    unsigned videoWidth;
    unsigned videoHeight;
    struct GBASIOLockstepDriver lockstepDriver;
    struct mLockstepUser lockstepUser;
};

static struct Player players[MAX_PLAYERS];
static struct GBASIOLockstepCoordinator coordinator;
static bool coordinatorInitialized = false;

// Mock lockstep user implementation for single-threaded WASM
static void wasm_lockstep_sleep(struct mLockstepUser* user) {
    (void)user;
}

static void wasm_lockstep_wake(struct mLockstepUser* user) {
    (void)user;
}

EMSCRIPTEN_KEEPALIVE
int mgba_init() {
    memset(players, 0, sizeof(players));
    if (!coordinatorInitialized) {
        GBASIOLockstepCoordinatorInit(&coordinator);
        coordinatorInitialized = true;
    }
    return 1;
}

EMSCRIPTEN_KEEPALIVE
int mgba_load_rom(int playerIndex, uint8_t* buffer, size_t size) {
    if (playerIndex < 0 || playerIndex >= MAX_PLAYERS) return 0;
    struct Player* p = &players[playerIndex];

    if (p->core) {
        if (p->core->platform(p->core) == mPLATFORM_GBA) {
            GBASIOLockstepCoordinatorDetach(&coordinator, &p->lockstepDriver);
        }
        p->core->deinit(p->core);
        p->core = NULL;
    }

    struct VFile* vf = VFileMemChunk(buffer, size);
    if (!vf) return 0;

    p->core = mCoreFindVF(vf);
    if (!p->core) {
        vf->close(vf);
        return 0;
    }

    mCoreInitConfig(p->core, NULL);
    mCoreConfigSetDefaultValue(&p->core->config, "useBios", "no");
    mCoreConfigSetDefaultValue(&p->core->config, "skipBios", "yes");
    mCoreConfigSetDefaultIntValue(&p->core->config, "volume", 256);
    mCoreConfigSetDefaultValue(&p->core->config, "mute", "no");
    
    p->core->init(p->core);
    mCoreLoadConfig(p->core);

    if (!p->core->loadROM(p->core, vf)) {
        p->core->deinit(p->core);
        p->core = NULL;
        return 0;
    }
    
    p->core->reset(p->core);

    p->core->currentVideoSize(p->core, &p->videoWidth, &p->videoHeight);
    if (p->videoBuffer) free(p->videoBuffer);
    p->videoBuffer = (uint32_t*)malloc(p->videoWidth * p->videoHeight * sizeof(uint32_t));
    memset(p->videoBuffer, 0, p->videoWidth * p->videoHeight * sizeof(uint32_t));
    p->core->setVideoBuffer(p->core, (mColor*)p->videoBuffer, p->videoWidth);
    
    if (p->core->reloadConfigOption) {
        p->core->reloadConfigOption(p->core, "hwaccelVideo", NULL);
    }

    p->core->setAudioBufferSize(p->core, 16384);

    // Setup Link Cable for GBA
    if (p->core->platform(p->core) == mPLATFORM_GBA) {
        p->lockstepUser.sleep = wasm_lockstep_sleep;
        p->lockstepUser.wake = wasm_lockstep_wake;
        GBASIOLockstepDriverCreate(&p->lockstepDriver, &p->lockstepUser);
        GBASIOLockstepCoordinatorAttach(&coordinator, &p->lockstepDriver);
        
        struct GBA* gba = (struct GBA*)p->core->board;
        GBASIOSetDriver(&gba->sio, &p->lockstepDriver.d);
    }
    
    p->lockstepDriver.asleep = false;
    printf("WASM: Player %d ROM Loaded Successfully. Res: %ux%u\n", playerIndex, p->videoWidth, p->videoHeight);
    return 1;
}

EMSCRIPTEN_KEEPALIVE
void mgba_run_frame() {
    const uint32_t CYCLES_PER_FRAME = 280896;
    uint32_t startCycles[MAX_PLAYERS];
    bool active[MAX_PLAYERS];
    
    for (int i = 0; i < MAX_PLAYERS; i++) {
        struct Player* p = &players[i];
        if (p->core) {
            startCycles[i] = mTimingCurrentTime(p->core->timing);
            active[i] = true;
        } else {
            active[i] = false;
        }
    }

    bool anyRunning;
    do {
        anyRunning = false;
        for (int i = 0; i < MAX_PLAYERS; i++) {
            struct Player* p = &players[i];
            if (!active[i] || p->lockstepDriver.asleep) continue;

            uint32_t currentCycles = mTimingCurrentTime(p->core->timing);
            int32_t remaining = (int32_t)(targetCycles[i] - currentCycles);
            if (remaining > 0) {
                uint32_t endCycles = currentCycles + (remaining > CHUNK_SIZE ? CHUNK_SIZE : remaining);
                while ((int32_t)(endCycles - mTimingCurrentTime(p->core->timing)) > 0 && !p->lockstepDriver.asleep) {
                    p->core->step(p->core);
                }
                anyRunning = true;
            }
        }
    } while (anyRunning);

    // Post-frame processing
    for (int i = 0; i < MAX_PLAYERS; i++) {
        struct Player* p = &players[i];
        if (active[i] && p->videoBuffer) {
            for (unsigned j = 0; j < p->videoWidth * p->videoHeight; ++j) {
                p->videoBuffer[j] |= 0xFF000000;
            }
        }
    }
}

EMSCRIPTEN_KEEPALIVE
uint32_t* mgba_get_video_buffer(int playerIndex) {
    if (playerIndex < 0 || playerIndex >= MAX_PLAYERS) return NULL;
    return players[playerIndex].videoBuffer;
}

EMSCRIPTEN_KEEPALIVE
int mgba_get_audio_samples(int playerIndex, int16_t* outBuffer, size_t maxSamples) {
    if (playerIndex < 0 || playerIndex >= MAX_PLAYERS) return 0;
    struct Player* p = &players[playerIndex];
    if (!p->core) return 0;
    struct mAudioBuffer* audio = p->core->getAudioBuffer(p->core);
    if (!audio) return 0;
    size_t availablePairs = mAudioBufferAvailable(audio);
    if (availablePairs * 2 > maxSamples) availablePairs = maxSamples / 2;
    mAudioBufferRead(audio, outBuffer, availablePairs);
    return (int)(availablePairs * 2);
}

EMSCRIPTEN_KEEPALIVE
unsigned mgba_get_audio_sample_rate(int playerIndex) {
    if (playerIndex < 0 || playerIndex >= MAX_PLAYERS) return 0;
    struct Player* p = &players[playerIndex];
    if (!p->core) return 0;
    return p->core->audioSampleRate(p->core);
}

EMSCRIPTEN_KEEPALIVE
void mgba_set_volume(int playerIndex, int volume) {
    if (playerIndex < 0 || playerIndex >= MAX_PLAYERS) return;
    struct Player* p = &players[playerIndex];
    if (p->core) {
        p->core->opts.volume = volume;
        if (p->core->reloadConfigOption) {
            p->core->reloadConfigOption(p->core, "volume", NULL);
        }
    }
}

EMSCRIPTEN_KEEPALIVE
unsigned mgba_get_width(int playerIndex) { 
    if (playerIndex < 0 || playerIndex >= MAX_PLAYERS) return 0;
    return players[playerIndex].videoWidth; 
}

EMSCRIPTEN_KEEPALIVE
unsigned mgba_get_height(int playerIndex) { 
    if (playerIndex < 0 || playerIndex >= MAX_PLAYERS) return 0;
    return players[playerIndex].videoHeight; 
}

EMSCRIPTEN_KEEPALIVE
void mgba_set_button(int playerIndex, int button, int pressed) {
    if (playerIndex < 0 || playerIndex >= MAX_PLAYERS) return;
    struct Player* p = &players[playerIndex];
    if (p->core) {
        if (pressed) p->core->addKeys(p->core, 1 << button);
        else p->core->clearKeys(p->core, 1 << button);
    }
}

EMSCRIPTEN_KEEPALIVE
uint8_t* mgba_save_state(int playerIndex, size_t* outSize) {
    if (playerIndex < 0 || playerIndex >= MAX_PLAYERS) return NULL;
    struct Player* p = &players[playerIndex];
    if (!p->core) return NULL;
    size_t bufferSize = 2 * 1024 * 1024;
    uint8_t* buffer = (uint8_t*)malloc(bufferSize);
    struct VFile* vf = VFileFromMemory(buffer, bufferSize);
    if (!mCoreSaveStateNamed(p->core, vf, SAVESTATE_SCREENSHOT)) {
        vf->close(vf);
        free(buffer);
        return NULL;
    }
    *outSize = vf->size(vf);
    vf->close(vf);
    return buffer;
}

EMSCRIPTEN_KEEPALIVE
int mgba_load_state(int playerIndex, uint8_t* buffer, size_t size) {
    if (playerIndex < 0 || playerIndex >= MAX_PLAYERS) return 0;
    struct Player* p = &players[playerIndex];
    if (!p->core) return 0;
    struct VFile* vf = VFileFromConstMemory(buffer, size);
    bool success = mCoreLoadStateNamed(p->core, vf, SAVESTATE_SCREENSHOT);
    vf->close(vf);
    return success ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE
void mgba_free_buffer(void* ptr) {
    free(ptr);
}
