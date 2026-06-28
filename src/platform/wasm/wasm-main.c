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
    atomic_bool newFrameAvailable;
    pthread_cond_t cond;
    pthread_mutex_t mutex;
    pthread_t thread;
    atomic_bool active;
    struct mCoreCallbacks callbacks;
    atomic_int frameTicks;
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
        atomic_store(&p->newFrameAvailable, true);
    }
}

// --- Main Player Loop ---

// =================================================================
// 외부(JS)에서 프레임 실행 권한을 부여하는 API
// =================================================================
EMSCRIPTEN_KEEPALIVE
void mgba_grant_frame_tick(int playerIndex) {
    if (playerIndex < 0 || playerIndex >= MAX_PLAYERS) return;
    struct Player* p = &players[playerIndex];
    
    // 프레임 틱을 하나 증가시키고, 혹시 잠들어 있다면 깨웁니다.
    atomic_fetch_add(&p->frameTicks, 1);
    pthread_cond_signal(&p->cond);
}

// =================================================================
// 1. 메인 루프 내부: 외부 틱 주입 기반 정속 제어
// =================================================================
EMSCRIPTEN_KEEPALIVE
void mgba_run_player(int playerIndex) {
    if (playerIndex < 0 || playerIndex >= MAX_PLAYERS) return;
    struct Player* p = &players[playerIndex];
    printf("WASM: Player %d thread active. Synchronized Audio-Backpressure Mode.\n", playerIndex);

    while (atomic_load(&p->active)) {
        
        pthread_mutex_lock(&p->mutex);
        
        // ⭐ [핵심 변경] 임의의 emscripten_thread_sleep을 완전히 제거하고,
        // 락스텝 대기 상태이거나 오디오 버퍼가 가득 찬 경우 '커널 레벨(cond_wait)'에서 안전하게 대기합니다.
        while (atomic_load(&p->active)) {
            // 1. 락스텝 통신 대기 중인 경우 -> 계속 대기
            if (p->lockstepDriver.asleep) {
                if (coordinatorInitialized && p->lockstepDriver.lockstepId == coordinator.attachedPlayers[0] && coordinator.waiting == 0) {
                    p->lockstepDriver.asleep = false;
                    break; // 락스텝이 풀렸으므로 탈출
                }
            } 
            // 2. 락스텝 대기가 아닐 때, 오디오 버퍼 여유 공간 확인
            else if (p->core) {
                struct mAudioBuffer* audio = p->core->getAudioBuffer(p->core);
                if (audio) {
                    unsigned int sampleRate = p->core->audioSampleRate(p->core);
                    if (sampleRate == 0) sampleRate = 32768;
                    
                    // 약 3프레임 분량(50ms)의 오디오가 쌓여있다면 JS가 가져갈 때까지 "완전히" 잠듭니다.
                    // 이 상태에서는 CPU를 전혀 쓰지 않으며, JS가 mgba_get_audio_samples를 호출하면 깨어납니다.
                    size_t maxBufferThreshold = (size_t)(sampleRate * 0.05);
                    if (mAudioBufferAvailable(audio) < maxBufferThreshold) {
                        break; // 버퍼에 여유가 있으므로 에뮬레이터를 돌리러 나갑니다.
                    }
                } else {
                    break;
                }
            } else {
                break;
            }

            // 조건이 만족되지 않으면 스레드를 안전하게 동결시킵니다.
            pthread_cond_wait(&p->cond, &p->mutex);
        }
        pthread_mutex_unlock(&p->mutex);

        if (!atomic_load(&p->active)) break;

        // [2] 입력 상태 반영
        pthread_mutex_lock(&p->mutex);
        if (!p->core) {
            pthread_mutex_unlock(&p->mutex);
            emscripten_thread_sleep(1);
            continue;
        }
        p->core->clearKeys(p->core, 0x3FF);
        p->core->addKeys(p->core, (uint16_t)atomic_load(&p->inputState));
        pthread_mutex_unlock(&p->mutex);

        // [3] 에뮬레이터 코어 실행
        // mGBA의 runLoop 대신 1프레임만 구동하는 단일 틱 함수가 있다면 베스트입니다.
        // gba 코어 구조상 runLoop가 1프레임마다 제어권을 반환한다면 이대로 사용합니다.
        p->core->runLoop(p->core); 
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

    p->callbacks.context = p;
    p->callbacks.videoFrameEnded = wasm_video_frame_ended;

    p->core->addCoreCallbacks(p->core, &p->callbacks);

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
