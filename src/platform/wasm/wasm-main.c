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
    uint32_t* videoBuffer;
    unsigned videoWidth;
    unsigned videoHeight;
    struct GBASIOLockstepDriver lockstepDriver;
    struct mLockstepUser lockstepUser;
    uint16_t inputState;
    pthread_cond_t cond;
    pthread_mutex_t mutex;
    pthread_t thread;
    bool active;
};

static struct Player players[MAX_PLAYERS];
static struct GBASIOLockstepCoordinator coordinator;
static bool coordinatorInitialized = false;

// 플레이어 실행 루프 (모든 플레이어용)
EMSCRIPTEN_KEEPALIVE
void mgba_run_player(int playerIndex) {
    if (playerIndex < 0 || playerIndex >= MAX_PLAYERS) return;
    struct Player* p = &players[playerIndex];

    printf("WASM: Player %d thread started.\n", playerIndex);

    while (p->active) {
        pthread_mutex_lock(&p->mutex);

        // Lockstep 동기화로 인해 잠든 상태면 조건 변수로 대기 (뮤텍스 자동 해제)
        while (p->lockstepDriver.asleep) {
            pthread_cond_wait(&p->cond, &p->mutex);
        }

        if (p->core) {
            // 오디오 버퍼가 일정 수준 이상 차면 잠시 대기하여 실행 속도 조절 (Throttling)
            struct mAudioBuffer* audio = p->core->getAudioBuffer(p->core);
            if (audio && mAudioBufferAvailable(audio) > 2048) {
                pthread_mutex_unlock(&p->mutex);
                emscripten_thread_sleep(1); 
                continue;
            }

            p->core->clearKeys(p->core, 0x3FF);
            p->core->addKeys(p->core, p->inputState);

            // 100 사이클씩 실행하여 뮤텍스 오버헤드와 속도 조절 사이의 균형을 맞춤
            for (int i = 0; i < 100; ++i) {
                p->core->step(p->core);
                if (p->lockstepDriver.asleep) break; 
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
    // 중요: 이 콜백은 lockstep.c 내부에서 coordinator->mutex를 쥐고 호출됩니다.
    // 여기서 pthread_cond_wait 등으로 블록하면 다른 스레드가 coordinator->mutex를 얻지 못해 
    // Ack를 보낼 수 없게 되고 결국 전체 시스템이 데드락에 빠집니다.
    // 따라서 여기서는 블록하지 않고 즉시 리턴하며, 실제 대기는 mgba_run_player 루프에서 수행합니다.
    (void)user;
}

static void wasm_lockstep_wake(struct mLockstepUser* user) {
    struct Player* p = (struct Player*)((char*)user - offsetof(struct Player, lockstepUser));
    pthread_mutex_lock(&p->mutex);
    pthread_cond_signal(&p->cond);
    pthread_mutex_unlock(&p->mutex);
}

EMSCRIPTEN_KEEPALIVE
int mgba_init() {
    static bool initialized = false;
    if (initialized) return 1;

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
    if (p->videoBuffer) free(p->videoBuffer);
    p->videoBuffer = (uint32_t*)malloc(p->videoWidth * p->videoHeight * sizeof(uint32_t));
    memset(p->videoBuffer, 0, p->videoWidth * p->videoHeight * sizeof(uint32_t));
    p->core->setVideoBuffer(p->core, (mColor*)p->videoBuffer, p->videoWidth);

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
    printf("WASM: Player %d ROM Loaded. Thread-safe lockstep ready.\n", playerIndex);
    return 1;
}

// 메인 스레드에서는 더 이상 직접 프레임을 실행하지 않음 (비디오 버퍼 후처리용으로만 남김)
EMSCRIPTEN_KEEPALIVE
void mgba_post_frame(int playerIndex) {
    if (playerIndex < 0 || playerIndex >= MAX_PLAYERS) return;
    struct Player* p = &players[playerIndex];
    pthread_mutex_lock(&p->mutex);
    if (p->videoBuffer) {
        for (unsigned j = 0; j < p->videoWidth * p->videoHeight; ++j) {
            p->videoBuffer[j] |= 0xFF000000;
        }
    }
    pthread_mutex_unlock(&p->mutex);
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
    if (pressed)
        p->inputState |= (1 << button);
    else
        p->inputState &= ~(1 << button);
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
void mgba_free_buffer(void* ptr) {
    if (ptr) free(ptr);
}