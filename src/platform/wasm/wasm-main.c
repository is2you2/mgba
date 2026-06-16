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

EMSCRIPTEN_KEEPALIVE
void mgba_run_player(int playerIndex) {
    if (playerIndex < 0 || playerIndex >= MAX_PLAYERS) return;
    struct Player* p = &players[playerIndex];
    printf("WASM: Player %d thread started. Ultimate Hybrid Sync Active.\n", playerIndex);

    // 싱글플레이 전용 타이밍 변수
    const double CYCLES_PER_SECOND = 16777216.0;
    const double MS_TO_CYCLES = CYCLES_PER_SECOND / 1000.0;
    const uint32_t MAX_CYCLE_BUDGET = 280896 * 4; 
    double startTime = emscripten_get_now();
    uint64_t totalCyclesExecuted = 0;

    while (atomic_load(&p->active)) {
        
        // =================================================================
        // [1] 락스텝 슬립 상태 처리 (마스터/슬레이브 동기화 락 인터록)
        // =================================================================
        if (p->lockstepDriver.asleep) {
            pthread_mutex_lock(&p->mutex);
            while (p->lockstepDriver.asleep && atomic_load(&p->active)) {
                // 마스터 노드가 깨어났으나 시그널을 놓쳤을 때를 위한 초고속 해제 안전장치
                if (coordinatorInitialized && p->lockstepDriver.lockstepId == coordinator.attachedPlayers[0] && coordinator.waiting == 0) {
                    p->lockstepDriver.asleep = false;
                    break;
                }
                // 멀티플레이 시에는 긴 대기 대신 짧은 조건 변수 대기 수행
                pthread_cond_wait(&p->cond, &p->mutex);
            }
            pthread_mutex_unlock(&p->mutex);
            
            // 깨어난 후 싱글플레이용 시간축만 갱신
            double currentTime = emscripten_get_now();
            startTime = currentTime - (double)totalCyclesExecuted / MS_TO_CYCLES;
        }

        if (!atomic_load(&p->active)) break;

        // 코어 존재 여부 체크를 위한 최소한의 임계 구역
        pthread_mutex_lock(&p->mutex);
        if (!p->core) {
            pthread_mutex_unlock(&p->mutex);
            emscripten_thread_sleep(10);
            continue;
        }

        // 실시간 멀티플레이 상태 감지
        bool isMultiplayer = coordinatorInitialized && coordinator.nAttached > 1;
        bool isTransferActive = coordinatorInitialized && coordinator.transferActive;

        // =================================================================
        // [2] 오디오 동기화 및 오버플로우 방지 (멀티플레이 정속 구동의 핵심)
        // =================================================================
        struct mAudioBuffer* audio = p->core->getAudioBuffer(p->core);
        if (audio) {
            size_t availableSamples = mAudioBufferAvailable(audio);
            // 멀티플레이어일 때는 버퍼 임계치를 엄격하게 잡아 가속을 원천 차단 (약 2~3프레임 분량)
            size_t audioThreshold = isMultiplayer ? 3072 : 12288; 
            
            if (availableSamples > audioThreshold) {
                // 중요: 이 상태에서 강제로 잠들면 병목이 생기므로, 
                // 락을 풀고 타 스레드에게 즉시 양보(yield)한 뒤 루프를 다시 돕니다.
                pthread_mutex_unlock(&p->mutex);
                emscripten_thread_sleep(0); 
                continue;
            }
        }

        // 키 입력 처리 (락 프리 아토믹 반영)
        p->core->clearKeys(p->core, 0x3FF);
        p->core->addKeys(p->core, (uint16_t)atomic_load(&p->inputState));

        // =================================================================
        // [3] 모드별 에뮬레이션 구동 분기
        // =================================================================
        if (isMultiplayer) {
            // ⭐ [멀티플레이 모드]: 시간 기반 Budget 계산을 완전히 배제합니다.
            // 락스텝 협력 구조와 오디오 버퍼 한계치 제어만으로 정속을 유지합니다.
            pthread_mutex_unlock(&p->mutex);

            if (isTransferActive) {
                // 직렬 통신 데이터 교환 중(가장 민감함): 
                // 1 스텝씩만 정밀하게 밟고 즉시 스레드 컨텍스트를 전환하여 무한 Ack 대기를 방지
                p->core->step(p->core);
                emscripten_thread_sleep(0);
            } else {
                // 일반 멀티플레이 상태: mGBA 내부 인터록이 걸릴 때까지 1프레임 단위 유연 구동
                p->core->runLoop(p->core);
                // 슬레이브 기기들의 민첩한 패킷 처리를 위해 매 프레임 끝날 때마다 힌트 부여
                if (playerIndex != 0) {
                    emscripten_thread_sleep(0);
                }
            }
        } else {
            // 🛡️ [싱글플레이 모드]: 정밀한 60Hz 시간 버젯 기반 구동 (기존 안정성 유지)
            double currentTime = emscripten_get_now();
            double elapsed = currentTime - startTime;
            uint64_t targetTotalCycles = (uint64_t)(elapsed * MS_TO_CYCLES);
            int64_t budget = (int64_t)(targetTotalCycles - totalCyclesExecuted);

            if (budget > (int64_t)MAX_CYCLE_BUDGET) {
                budget = MAX_CYCLE_BUDGET;
                startTime = currentTime - (double)(totalCyclesExecuted + budget) / MS_TO_CYCLES;
            }

            // 구동할 사이클이 부족하면 락을 풀고 브라우저 타이머 해상도만큼 대기
            if (budget < (int64_t)16384) {
                pthread_mutex_unlock(&p->mutex);
                double sleepMs = (double)(16384 - budget) / MS_TO_CYCLES;
                emscripten_thread_sleep(sleepMs < 1.0 ? 0 : (unsigned int)sleepMs);
                continue;
            }

            uint32_t startCycles = mTimingCurrentTime(p->core->timing);
            uint32_t endCycles = startCycles + (uint32_t)budget;

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
    pthread_mutex_unlock(&p->mutex);
    pthread_cond_signal(&p->cond);
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
