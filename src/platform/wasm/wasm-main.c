/* Copyright (c) 2026 Choi Sung soo
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include <emscripten.h>
#include <emscripten/threading.h>
#include <pthread.h>
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
    uint32_t* videoBuffers[2];
    int currentBuffer;
    unsigned videoWidth;
    unsigned videoHeight;
    struct GBASIOLockstepDriver lockstepDriver;
    struct mLockstepUser lockstepUser;
    uint16_t inputState;
    bool newFrameAvailable;
    pthread_cond_t cond;
    pthread_mutex_t mutex;
    pthread_t thread;
    bool active;
};

static struct Player players[MAX_PLAYERS];
static struct GBASIOLockstepCoordinator coordinator;
static bool coordinatorInitialized = false;

#include <emscripten/html5.h> // emscripten_get_now 사용을 위해 추가

EMSCRIPTEN_KEEPALIVE
void mgba_run_player(int playerIndex) {
    if (playerIndex < 0 || playerIndex >= MAX_PLAYERS) return;
    struct Player* p = &players[playerIndex];
    printf("WASM: Player %d thread started. Frame-Skip Enabled Mode.\n", playerIndex);

    const double CYCLES_PER_SECOND = 16777216.0;
    const double MS_TO_CYCLES = CYCLES_PER_SECOND / 1000.0;
    const uint32_t MAX_CYCLE_BUDGET = 280896 * 4; 
    const uint32_t DEFAULT_THRESHOLD = 16384; 
    const uint32_t HIGH_PRECISION_THRESHOLD = 512; 
    const size_t AUDIO_SAFE_LIMIT = 2048;
    const int64_t FRAME_SKIP_THRESHOLD = 280896; // Skip frame if behind more than 1 frame

    double startTime = emscripten_get_now();
    uint64_t totalCyclesExecuted = 0;

    while (p->active) {
        pthread_mutex_lock(&p->mutex);

        while (p->lockstepDriver.asleep && p->active) {
            pthread_cond_wait(&p->cond, &p->mutex);
            double currentTime = emscripten_get_now();
            startTime = currentTime - (double)totalCyclesExecuted / MS_TO_CYCLES;
        }

        if (!p->active) {
            pthread_mutex_unlock(&p->mutex);
            break;
        }

        if (p->core) {
            bool isTransferActive = coordinatorInitialized && coordinator.transferActive;
            bool isMultiplayer = coordinatorInitialized && coordinator.nAttached > 1;

            size_t currentAudioLimit = isMultiplayer ? 12288 : AUDIO_SAFE_LIMIT;

            struct mAudioBuffer* audio = p->core->getAudioBuffer(p->core);
            if (audio && mAudioBufferAvailable(audio) > currentAudioLimit) {
                pthread_cond_wait(&p->cond, &p->mutex);
                pthread_mutex_unlock(&p->mutex);
                continue;
            }

            double currentTime = emscripten_get_now();
            double elapsed = currentTime - startTime;
            uint64_t targetTotalCycles = (uint64_t)(elapsed * MS_TO_CYCLES);
            
            int64_t budget = (int64_t)(targetTotalCycles - totalCyclesExecuted);

            if (!isMultiplayer && budget > (int64_t)MAX_CYCLE_BUDGET) {
                budget = MAX_CYCLE_BUDGET;
                startTime = currentTime - (double)(totalCyclesExecuted + budget) / MS_TO_CYCLES;
            }

            uint32_t threshold = isTransferActive ? HIGH_PRECISION_THRESHOLD : DEFAULT_THRESHOLD;

            if (budget < (int64_t)threshold) {
                pthread_mutex_unlock(&p->mutex);
                if (isTransferActive) {
                    emscripten_thread_sleep(0); 
                } else {
                    double sleepMs = (double)(threshold - budget) / MS_TO_CYCLES;
                    if (sleepMs < 1.0) sleepMs = 0;
                    emscripten_thread_sleep((unsigned int)sleepMs);
                }
                continue;
            }

            // Frame Skipping Logic: Disable rendering if we are falling behind
            // But NEVER skip if SIO transfer is active to ensure timing stability
            bool shouldSkip = !isTransferActive && (budget > FRAME_SKIP_THRESHOLD);
            if (shouldSkip) {
                p->core->setVideoBuffer(p->core, NULL, 0);
            } else {
                p->core->setVideoBuffer(p->core, (mColor*)p->videoBuffers[p->currentBuffer], p->videoWidth);
            }

            p->core->clearKeys(p->core, 0x3FF);
            p->core->addKeys(p->core, p->inputState);

            uint32_t startCycles = mTimingCurrentTime(p->core->timing);
            uint32_t endCycles = startCycles + (uint32_t)budget;

            while ((int32_t)(endCycles - mTimingCurrentTime(p->core->timing)) > 0 && !p->lockstepDriver.asleep) {
                // If skipping, we can use a even faster runLoop if available, 
                // but standard runLoop is already good.
                p->core->runLoop(p->core);
            }

            uint32_t actualExecuted = mTimingCurrentTime(p->core->timing) - startCycles;
            totalCyclesExecuted += actualExecuted;

            // Re-enable video buffer if it was disabled
            if (shouldSkip) {
                p->core->setVideoBuffer(p->core, (mColor*)p->videoBuffers[p->currentBuffer], p->videoWidth);
            }

            pthread_mutex_unlock(&p->mutex);
        } else {
            pthread_mutex_unlock(&p->mutex);
            emscripten_thread_sleep(10);
        }
    }
    printf("WASM: Player %d thread exiting.\n", playerIndex);
}

static void* player_thread_entry(void* arg) {
    int playerIndex = (int)(intptr_t)arg;
    mgba_run_player(playerIndex);
    return NULL;
}

// 멀티 스레드 WASM 환경을 위한 락스텝 콜백 함수
static void wasm_lockstep_sleep(struct mLockstepUser* user) {
    // 중요: 이 콜백은 coordinator->mutex를 쥐고 호출됩니다.
    // p->mutex를 여기서 잡으면 AB-BA 데드락이 발생할 수 있으므로 시그널만 보냅니다.
    struct Player* p = (struct Player*)((char*)user - offsetof(struct Player, lockstepUser));
    pthread_cond_signal(&p->cond);
}

static void wasm_lockstep_wake(struct mLockstepUser* user) {
    // 중요: 위와 동일한 이유로 p->mutex를 잡지 않고 시그널만 보냅니다.
    struct Player* p = (struct Player*)((char*)user - offsetof(struct Player, lockstepUser));
    pthread_cond_signal(&p->cond);
}

EMSCRIPTEN_KEEPALIVE
int mgba_init() {
    static bool initialized = false;
    if (initialized) return 1;

    printf("WASM: mColor size: %zu bytes\n", sizeof(mColor));

    memset(players, 0, sizeof(players));
    for (int i = 0; i < MAX_PLAYERS; i++) {
        pthread_mutex_init(&players[i].mutex, NULL);
        pthread_cond_init(&players[i].cond, NULL);
        players[i].active = true;
        pthread_create(&players[i].thread, NULL, player_thread_entry, (void*)(intptr_t)i);
    }

    if (!coordinatorInitialized) {
        GBASIOLockstepCoordinatorInit(&coordinator);
        coordinatorInitialized = true;
    }
    initialized = true;
    return 1;
}

// 비디오 프레임이 끝났을 때 호출되는 콜백 함수 수정
static void wasm_video_frame_ended(void* context) {
    struct Player* p = (struct Player*)context;
    if (p->videoBuffers[0] && p->videoBuffers[1]) {
        uint32_t* finishedBuffer = p->videoBuffers[p->currentBuffer];
        for (unsigned j = 0; j < p->videoWidth * p->videoHeight; ++j) {
            finishedBuffer[j] |= 0xFF000000;
        }
        p->currentBuffer = 1 - p->currentBuffer;
        p->core->setVideoBuffer(p->core, (mColor*)p->videoBuffers[p->currentBuffer], p->videoWidth);
        p->newFrameAvailable = true;
    }
}

EMSCRIPTEN_KEEPALIVE
int mgba_load_rom(int playerIndex, uint8_t* buffer, size_t size) {
    if (playerIndex < 0 || playerIndex >= MAX_PLAYERS) return 0;
    struct Player* p = &players[playerIndex];

    // ROM 로딩 시에도 안전을 위해 잠시 락
    pthread_mutex_lock(&p->mutex);

    if (p->core) {
        if (p->core->platform(p->core) == mPLATFORM_GBA) {
            GBASIOLockstepCoordinatorDetach(&coordinator, &p->lockstepDriver);
        }
        p->core->deinit(p->core);
        p->core = NULL;
    }

    struct VFile* vf = VFileMemChunk(buffer, size);
    if (!vf) {
        pthread_mutex_unlock(&p->mutex);
        return 0;
    }

    p->core = mCoreFindVF(vf);
    if (!p->core) {
        vf->close(vf);
        pthread_mutex_unlock(&p->mutex);
        return 0;
    }

    mCoreInitConfig(p->core, NULL);
    mCoreConfigSetDefaultValue(&p->core->config, "useBios", "no");
    mCoreConfigSetDefaultValue(&p->core->config, "skipBios", "yes");
    mCoreConfigSetDefaultValue(&p->core->config, "audioSync", "yes");
    mCoreConfigSetDefaultValue(&p->core->config, "fpsTarget", "60");
    mCoreConfigSetDefaultIntValue(&p->core->config, "volume", 256);
    mCoreConfigSetDefaultValue(&p->core->config, "mute", "no");

    p->core->init(p->core);
    mCoreLoadConfig(p->core);

    if (!p->core->loadROM(p->core, vf)) {
        p->core->deinit(p->core);
        p->core = NULL;
        pthread_mutex_unlock(&p->mutex);
        return 0;
    }

    p->core->reset(p->core);

    p->core->currentVideoSize(p->core, &p->videoWidth, &p->videoHeight);
    if (p->videoBuffers[0]) free(p->videoBuffers[0]);
    if (p->videoBuffers[1]) free(p->videoBuffers[1]);
    p->videoBuffers[0] = (uint32_t*)malloc(p->videoWidth * p->videoHeight * sizeof(uint32_t));
    p->videoBuffers[1] = (uint32_t*)malloc(p->videoWidth * p->videoHeight * sizeof(uint32_t));
    memset(p->videoBuffers[0], 0, p->videoWidth * p->videoHeight * sizeof(uint32_t));
    memset(p->videoBuffers[1], 0, p->videoWidth * p->videoHeight * sizeof(uint32_t));
    
    p->currentBuffer = 0;
    p->core->setVideoBuffer(p->core, (mColor*)p->videoBuffers[p->currentBuffer], p->videoWidth);
    p->newFrameAvailable = false;

    struct mCoreCallbacks callbacks = {
        .context = p,
        .videoFrameEnded = wasm_video_frame_ended
    };
    p->core->addCoreCallbacks(p->core, &callbacks);

    if (p->core->reloadConfigOption) {
        p->core->reloadConfigOption(p->core, "hwaccelVideo", NULL);
    }

    p->core->setAudioBufferSize(p->core, 8192);

    if (p->core->platform(p->core) == mPLATFORM_GBA) {
        p->lockstepUser.sleep = wasm_lockstep_sleep;
        p->lockstepUser.wake = wasm_lockstep_wake;
        GBASIOLockstepDriverCreate(&p->lockstepDriver, &p->lockstepUser);
        GBASIOLockstepCoordinatorAttach(&coordinator, &p->lockstepDriver);

        struct GBA* gba = (struct GBA*)p->core->board;
        GBASIOSetDriver(&gba->sio, &p->lockstepDriver.d);
    }

    p->lockstepDriver.asleep = false;
    p->active = true;
    pthread_mutex_unlock(&p->mutex);
    printf("WASM: Player %d ROM Loaded. Buffer swapping enabled.\n", playerIndex);
    return 1;
}

EMSCRIPTEN_KEEPALIVE
int mgba_post_frame(int playerIndex) {
    if (playerIndex < 0 || playerIndex >= MAX_PLAYERS) return 0;
    struct Player* p = &players[playerIndex];
    int updated = 0;
    pthread_mutex_lock(&p->mutex);
    if (p->newFrameAvailable) {
        p->newFrameAvailable = false;
        updated = 1;
    }
    pthread_mutex_unlock(&p->mutex);
    return updated;
}
EMSCRIPTEN_KEEPALIVE
uint32_t* mgba_get_video_buffer(int playerIndex) {
    if (playerIndex < 0 || playerIndex >= MAX_PLAYERS) return NULL;
    struct Player* p = &players[playerIndex];
    return p->videoBuffers[1 - p->currentBuffer];
}

EMSCRIPTEN_KEEPALIVE
int mgba_get_audio_samples(int playerIndex, int16_t* outBuffer, size_t maxSamples) {
    if (playerIndex < 0 || playerIndex >= MAX_PLAYERS) return 0;
    struct Player* p = &players[playerIndex];
    if (!p->core) return 0;

    pthread_mutex_lock(&p->mutex);
    struct mAudioBuffer* audio = p->core->getAudioBuffer(p->core);
    if (!audio) {
        pthread_mutex_unlock(&p->mutex);
        return 0;
    }
    size_t availablePairs = mAudioBufferAvailable(audio);
    if (availablePairs * 2 > maxSamples) availablePairs = maxSamples / 2;
    mAudioBufferRead(audio, outBuffer, availablePairs);
    
    // Signal the emulator thread that we have consumed audio and there is space in the buffer
    pthread_cond_signal(&p->cond);
    
    pthread_mutex_unlock(&p->mutex);

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
    pthread_mutex_lock(&p->mutex);
    if (pressed)
        p->inputState |= (1 << button);
    else
        p->inputState &= ~(1 << button);
    
    // Interrupt-driven Input: Wake the player thread immediately when input changes
    pthread_cond_signal(&p->cond);
    
    pthread_mutex_unlock(&p->mutex);
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
uint8_t* mgba_save_state(int playerIndex, size_t* outSize) {
    if (playerIndex < 0 || playerIndex >= MAX_PLAYERS) return NULL;
    struct Player* p = &players[playerIndex];
    if (!p->core) return NULL;

    size_t bufferSize = 1024 * 1024;
    uint8_t* buffer = (uint8_t*)malloc(bufferSize);
    if (!buffer) return NULL;

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
uint8_t* mgba_get_save_data(int playerIndex, size_t* outSize) {
    if (playerIndex < 0 || playerIndex >= MAX_PLAYERS) return NULL;
    struct Player* p = &players[playerIndex];
    if (!p->core) return NULL;

    void* sram = NULL;
    size_t size = p->core->savedataClone(p->core, &sram);
    *outSize = size;
    return (uint8_t*)sram;
}

EMSCRIPTEN_KEEPALIVE
int mgba_load_save_data(int playerIndex, uint8_t* buffer, size_t size) {
    if (playerIndex < 0 || playerIndex >= MAX_PLAYERS) return 0;
    struct Player* p = &players[playerIndex];
    if (!p->core) return 0;

    pthread_mutex_lock(&p->mutex);
    bool success = p->core->savedataRestore(p->core, buffer, size, true);
    if (success) {
        p->core->reset(p->core);
    }
    pthread_mutex_unlock(&p->mutex);
    return success ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE
void mgba_free_buffer(void* ptr) {
    if (ptr) free(ptr);
}