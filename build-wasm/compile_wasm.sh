#!/bin/bash
source "/opt/emsdk/emsdk_env.sh"
emcc ../src/platform/wasm/wasm-main.c libmgba.a \
    -I../include \
    -I../src \
    -I. \
    -D_GNU_SOURCE \
    -DENABLE_DIRECTORIES \
    -DENABLE_VFS \
    -DENABLE_VFS_FD \
    -DM_CORE_GB \
    -DM_CORE_GBA \
    -pthread \
    -s WASM=1 \
    -s MODULARIZE=1 \
    -s EXPORT_NAME='createMGBA' \
    -s EXPORTED_FUNCTIONS='["_mgba_init","_mgba_load_rom","_mgba_run_frame","_mgba_get_video_buffer","_mgba_get_width","_mgba_get_height","_mgba_set_button","_mgba_save_state","_mgba_load_state","_mgba_free_buffer","_mgba_get_audio_samples","_mgba_get_audio_sample_rate","_mgba_set_volume","_malloc","_free"]' \
    -s EXPORTED_RUNTIME_METHODS='["ccall","cwrap","HEAP8","HEAPU8","HEAP16","HEAP32"]' \
    -s ALLOW_MEMORY_GROWTH=1 \
    -s PTHREAD_POOL_SIZE=4 \
    -O3 \
    -o mgba.js
