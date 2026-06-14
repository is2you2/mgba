/* Copyright (c) 2026 Choi Sung soo
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

importScripts('mgba.js');

let Module;
let playerIndex;

self.onmessage = async (e) => {
    const { type, data } = e.data;

    if (type === 'init') {
        playerIndex = data.playerIndex;
        
        // Emscripten 모듈 초기화 (메인 스레드와 메모리 공유)
        Module = await createMGBA({
            wasmMemory: data.wasmMemory,
            onRuntimeInitialized: () => {
                console.log(`WASM Worker: Player ${playerIndex} initialized.`);
                // 실행 루프 시작 (C단에서 루프이므로 여기서 블록됨)
                Module._mgba_run_player(playerIndex);
            }
        });
    } else if (type === 'setButton') {
        if (Module) {
            Module._mgba_set_button(playerIndex, data.button, data.pressed);
        }
    }
};
