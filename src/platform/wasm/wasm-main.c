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
#include <stdatomic.h>

#define MAX_PLAYERS 4

struct Player {
    struct mCore* core;
    uint32_t* videoBuffers[2];
    int currentBuffer;
    unsigned videoWidth;
    unsigned videoHeight;
    struct GBASIOLockstepDriver lockstepDriver;
    struct mLockstepUser lockstepUser;
    atomic_uint_fast16_t inputState;
    bool newFrameAvailable;
    pthread_cond_t cond;
    pthread_mutex_t mutex;
    pthread_t thread;
    atomic_bool active;
};

static struct Player players[MAX_PLAYERS];
static struct GBASIOLockstepCoordinator coordinator;
static bool coordinatorInitialized = false;

#include <emscripten/html5.h> 

// --- Helper Functions & Callbacks ---

static void wasm_lockstep_sleep(struct mLockstepUser* user) {
    struct Player* p = (struct Player*)((char*)user - offsetof(struct Player, lockstepUser));
    pthread_cond_signal(&p->cond);
}

static void wasm_lockstep_wake(struct mLockstepUser* user) {
    struct Player* p = (struct Player*)((char*)user - offsetof(struct Player, lockstepUser));
    pthread_cond_signal(&p->cond);
}

static void wasm_video_frame_ended(void* context) {
    struct Player* p = (struct Player*)context;
    if (p->videoBuffers[0] && p->videoBuffers[1]) {
        uint32_t* finishedBuffer = p->videoBuffers[p->currentBuffer];
        // Ensure alpha channel is set (GBA output is often 0 or garbage in alpha)
        for (unsigned j = 0; j < p->videoWidth * p->videoHeight; ++j) {
            finishedBuffer[j] |= 0xFF000000;
        }
        p->currentBuffer = 1 - p->currentBuffer;
        // The core will now draw to the other buffer
        p->core->setVideoBuffer(p->core, (mColor*)p->videoBuffers[p->currentBuffer], p->videoWidth);
        p->newFrameAvailable = true;
    }
}

// --- Main Player Loop ---

// =================================================================
// 1. 메인 루프 내부: 완벽한 정속 제어 및 오디오 보호 락 메커니즘
// =================================================================
EMSCRIPTEN_KEEPALIVE
void mgba_run_player(int playerIndex) {
    if (playerIndex < 0 || playerIndex >= MAX_PLAYERS) return;
    struct Player* p = &players[playerIndex];
    printf("WASM: Player %d thread started. Perfect Audio-Lock Mode Active.\n", playerIndex);

    // 싱글플레이 전용 타이밍 변수
    const double CYCLES_PER_SECOND = 16777216.0;
    const double MS_TO_CYCLES = CYCLES_PER_SECOND / 1000.0;
    const uint32_t MAX_CYCLE_BUDGET = 280896 * 4; 
    double startTime = emscripten_get_now();
    uint64_t totalCyclesExecuted = 0;

    while (atomic_load(&p->active)) {
        
        // [1] 락스텝 슬립 상태 처리
        if (p->lockstepDriver.asleep) {
            pthread_mutex_lock(&p->mutex);
            while (p->lockstepDriver.asleep && atomic_load(&p->active)) {
                if (coordinatorInitialized && p->lockstepDriver.lockstepId == coordinator.attachedPlayers[0] && coordinator.waiting == 0) {
                    p->lockstepDriver.asleep = false;
                    break;
                }
                pthread_cond_wait(&p->cond, &p->mutex);
            }
            pthread_mutex_unlock(&p->mutex);
            
            double currentTime = emscripten_get_now();
            startTime = currentTime - (double)totalCyclesExecuted / MS_TO_CYCLES;
        }

        if (!atomic_load(&p->active)) break;

        // 에뮬레이션 상태 확인 및 오디오 체크를 위한 안전한 락 획득
        pthread_mutex_lock(&p->mutex);
        if (!p->core) {
            pthread_mutex_unlock(&p->mutex);
            emscripten_thread_sleep(10);
            continue;
        }

        bool isMultiplayer = coordinatorInitialized && coordinator.nAttached > 1;
        bool isTransferActive = coordinatorInitialized && coordinator.transferActive;

        // ⭐ [핵심 개선] 오디오 동기화 및 가속 억제 구조화
        struct mAudioBuffer* audio = p->core->getAudioBuffer(p->core);
        if (audio) {
            size_t availablePairs = mAudioBufferAvailable(audio);
            // 멀티플레이 시 약 1~1.5프레임 분량인 1024 샘플 쌍으로 극단적인 제어 수행
            size_t audioThreshold = isMultiplayer ? 1024 : 12288; 
            
            if (availablePairs > audioThreshold) {
                // 브라우저가 사운드를 채가서 버퍼가 비워질 때까지 현재 스레드를 완벽히 정지
                while (mAudioBufferAvailable(audio) > audioThreshold && atomic_load(&p->active)) {
                    pthread_cond_wait(&p->cond, &p->mutex);
                }
                // 깨어난 후 안전하게 상태를 재확인하기 위해 루프 처음으로 리턴
                pthread_mutex_unlock(&p->mutex);
                continue;
            }
        }

        // 입력 상태 안전하게 주입 (락을 쥐고 있으므로 레이스 컨디션 방지)
        p->core->clearKeys(p->core, 0x3FF);
        p->core->addKeys(p->core, (uint16_t)atomic_load(&p->inputState));

        // 구동 구역 진입 전 락 해제 (멀티플레이 락스텝 통신을 위해 타 스레드에 양보)
        pthread_mutex_unlock(&p->mutex);

        // [3] 모드별 에뮬레이션 구동
        if (isMultiplayer) {
            if (isTransferActive) {
                p->core->step(p->core);
                emscripten_thread_sleep(0);
            } else {
                p->core->runLoop(p->core);
                if (playerIndex != 0) {
                    emscripten_thread_sleep(0);
                }
            }
        } else {
            // 싱글플레이 모드: 정밀한 시간축 버젯 제어
            pthread_mutex_lock(&p->mutex);
            double currentTime = emscripten_get_now();
            double elapsed = currentTime - startTime;
            uint64_t targetTotalCycles = (uint64_t)(elapsed * MS_TO_CYCLES);
            int64_t budget = (int64_t)(targetTotalCycles - totalCyclesExecuted);

            if (budget > (int64_t)MAX_CYCLE_BUDGET) {
                budget = MAX_CYCLE_BUDGET;
                startTime = currentTime - (double)(totalCyclesExecuted + budget) / MS_TO_CYCLES;
            }

            if (budget < (int64_t)16384) {
                pthread_mutex_unlock(&p->mutex);
                double sleepMs = (double)(16384 - budget) / MS_TO_CYCLES;
                emscripten_thread_sleep(sleepMs < 1.0 ? 0 : (unsigned int)sleepMs);
                continue;
            }

            uint32_t startCycles = mTimingCurrentTime(p->core->timing);
            uint32_t endCycles = startCycles + (uint32_t)budget;
            pthread_unlock:
            pthread_mutex_unlock(&p->mutex);

            while ((int32_t)(endCycles - mTimingCurrentTime(p->core->timing)) > 0 && !p->lockstepDriver.asleep) {
                p->core->runLoop(p->core);
            }

            pthread_mutex_lock(&p->mutex);
            uint32_t actualExecuted = mTimingCurrentTime(p->core->timing) - startCycles;
            totalCyclesExecuted += actualExecuted;
            pthread_mutex_unlock(&p->mutex);
        }
    }
    printf("WASM: Player %d thread exiting.\n", playerIndex);
}

static void* player_thread_entry(void* arg) {
    int playerIndex = (int)(intptr_t)arg;
    mgba_run_player(playerIndex);
    return NULL;
}

// --- Public API ---

EMSCRIPTEN_KEEPALIVE
int mgba_init() {
    static bool initialized = false;
    if (initialized) return 1;

    memset(players, 0, sizeof(players));
    for (int i = 0; i < MAX_PLAYERS; i++) {
        pthread_mutex_init(&players[i].mutex, NULL);
        pthread_cond_init(&players[i].cond, NULL);
        atomic_init(&players[i].active, true);
        atomic_init(&players[i].inputState, 0);
        pthread_create(&players[i].thread, NULL, player_thread_entry, (void*)(intptr_t)i);
    }

    if (!coordinatorInitialized) {
        GBASIOLockstepCoordinatorInit(&coordinator);
        coordinatorInitialized = true;
    }
    initialized = true;
    return 1;
}

EMSCRIPTEN_KEEPALIVE
int mgba_load_rom(int playerIndex, uint8_t* buffer, size_t size) {
    if (playerIndex < 0 || playerIndex >= MAX_PLAYERS) return 0;
    struct Player* p = &players[playerIndex];

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
    // CRITICAL: Restore essential defaults to prevent black screen (waiting for BIOS)
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
    atomic_store(&p->active, true);
    pthread_mutex_unlock(&p->mutex);
    return 1;
}

EMSCRIPTEN_KEEPALIVE
void mgba_set_button(int playerIndex, int button, int pressed) {
    if (playerIndex < 0 || playerIndex >= MAX_PLAYERS) return;
    struct Player* p = &players[playerIndex];
    uint_fast16_t current = atomic_load(&p->inputState);
    if (pressed) current |= (1 << button);
    else current &= ~(1 << button);
    atomic_store(&p->inputState, current);
    pthread_cond_signal(&p->cond);
}

// =================================================================
// 2. 외부 오디오 소비 API: 동시성 보호 강화
// =================================================================
EMSCRIPTEN_KEEPALIVE
int mgba_get_audio_samples(int playerIndex, int16_t* outBuffer, size_t maxSamples) {
    if (playerIndex < 0 || playerIndex >= MAX_PLAYERS) return 0;
    struct Player* p = &players[playerIndex];
    if (!p->core) return 0;

    // 메인 루프가 데이터를 쓰는 도중 가로채지 못하도록 강한 Mutex 잠금 보장
    pthread_mutex_lock(&p->mutex);
    struct mAudioBuffer* audio = p->core->getAudioBuffer(p->core);
    if (!audio) {
        pthread_mutex_unlock(&p->mutex);
        return 0;
    }
    
    size_t availablePairs = mAudioBufferAvailable(audio);
    if (availablePairs * 2 > maxSamples) availablePairs = maxSamples / 2;
    
    // 안전하게 동기화된 상태에서 사운드 데이터를 읽어갑니다 (지직임 근본적 해결)
    mAudioBufferRead(audio, outBuffer, availablePairs);
    
    // 오디오가 정상 소비되어 버퍼가 비워졌으므로 잠들어 있던 코어 스레드를 깨움
    pthread_cond_broadcast(&p->cond); 
    pthread_mutex_unlock(&p->mutex);
    
    return (int)(availablePairs * 2);
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
    if (success) p->core->reset(p->core);
    pthread_mutex_unlock(&p->mutex);
    return success ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE
void mgba_free_buffer(void* ptr) {
    if (ptr) free(ptr);
}
