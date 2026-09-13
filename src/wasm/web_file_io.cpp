#include "web_file_io.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <emscripten/html5.h>
#include <filesystem>
#include <iostream>

namespace fs = std::filesystem;

EM_JS(void, js_download_file, (const char* path_ptr, const char* name_ptr), {
    const path = UTF8ToString(path_ptr);
    const name = name_ptr ? UTF8ToString(name_ptr) : path.split('/').pop();
    try {
        const data = FS.readFile(path);
        const blob = new Blob([data], { type: 'application/octet-stream' });
        const url = URL.createObjectURL(blob);
        const a = document.createElement('a');
        a.href = url;
        a.download = name;
        document.body.appendChild(a);
        a.click();
        setTimeout(() => {
            document.body.removeChild(a);
            URL.revokeObjectURL(url);
        }, 3000);
    } catch (err) {
        console.error('Download error for', path, err);
    }
});

EM_JS(void, js_trigger_file_input, (const char* accept_ptr, int target_type), {
    const accept = accept_ptr ? UTF8ToString(accept_ptr) : "";
    let input = document.getElementById('wasm-file-input');
    if (!input) {
        input = document.createElement('input');
        input.id = 'wasm-file-input';
        input.type = 'file';
        input.style.display = 'none';
        document.body.appendChild(input);
    }
    input.accept = accept;
    input.multiple = true;
    input.onchange = (e) => {
        const fileList = Array.from(e.target.files || []);
        if (fileList.length === 0) return;

        let remaining = fileList.length;
        let terrainFile = null;
        let projectFile = null;
        let mapFile = null;
        let pngFile = null;

        for (const f of fileList) {
            const lower = f.name.toLowerCase();
            if (lower.endsWith('.terrain')) terrainFile = f;
            else if (lower.endsWith('.tilesetproj')) projectFile = f;
            else if (lower.endsWith('.json')) mapFile = f;
            else if (lower.endsWith('.png')) pngFile = f;
        }

        fileList.forEach((file) => {
            const reader = new FileReader();
            reader.onload = () => {
                const bytes = new Uint8Array(reader.result);
                try {
                    FS.mkdir('/uploads');
                } catch (err) {}
                const uploadPath = '/uploads/' + file.name;
                FS.writeFile(uploadPath, bytes);
                remaining--;
                if (remaining === 0 && Module._wasm_on_file_uploaded) {
                    let primary = fileList[0];
                    if (target_type === 1) { // WebFileTarget_MapJson
                        primary = mapFile || fileList[0];
                    } else if (target_type === 2 || target_type === 4) { // Tileset or Import12x4
                        primary = projectFile || terrainFile || pngFile || fileList[0];
                    } else if (target_type === 3) { // TilesetProj
                        primary = projectFile || fileList[0];
                    } else if (target_type === 7) { // PendingTerrainPng
                        primary = pngFile || fileList[0];
                    } else {
                        primary = terrainFile || projectFile || mapFile || pngFile || fileList[0];
                    }
                    const pathPtr = stringToNewUTF8('/uploads/' + primary.name);
                    Module._wasm_on_file_uploaded(pathPtr, target_type);
                    _free(pathPtr);
                }
            };
            reader.readAsArrayBuffer(file);
        });
        input.value = "";
    };
    input.click();
});

EM_JS(void, js_init_drag_drop, (), {
    window.addEventListener('dragover', (e) => {
        e.preventDefault();
        e.stopPropagation();
        const dropOverlay = document.getElementById('drop-overlay');
        if (dropOverlay) dropOverlay.style.display = 'flex';
    });

    window.addEventListener('dragleave', (e) => {
        e.preventDefault();
        e.stopPropagation();
        if (e.clientX <= 0 || e.clientY <= 0 || e.clientX >= window.innerWidth || e.clientY >= window.innerHeight) {
            const dropOverlay = document.getElementById('drop-overlay');
            if (dropOverlay) dropOverlay.style.display = 'none';
        }
    });

    window.addEventListener('drop', (e) => {
        e.preventDefault();
        e.stopPropagation();
        const dropOverlay = document.getElementById('drop-overlay');
        if (dropOverlay) dropOverlay.style.display = 'none';

        const fileList = Array.from(e.dataTransfer && e.dataTransfer.files ? e.dataTransfer.files : []);
        if (fileList.length === 0) return;

        let remaining = fileList.length;
        let terrainFile = null;
        let projectFile = null;
        let mapFile = null;
        let pngFile = null;

        for (const f of fileList) {
            const lower = f.name.toLowerCase();
            if (lower.endsWith('.terrain')) terrainFile = f;
            else if (lower.endsWith('.tilesetproj')) projectFile = f;
            else if (lower.endsWith('.json')) mapFile = f;
            else if (lower.endsWith('.png')) pngFile = f;
        }

        fileList.forEach((file) => {
            const reader = new FileReader();
            reader.onload = () => {
                const bytes = new Uint8Array(reader.result);
                try {
                    FS.mkdir('/uploads');
                } catch (err) {}
                const uploadPath = '/uploads/' + file.name;
                FS.writeFile(uploadPath, bytes);
                remaining--;
                if (remaining === 0 && Module._wasm_on_file_uploaded) {
                    let primary = terrainFile || projectFile || mapFile || pngFile || fileList[0];
                    const pathPtr = stringToNewUTF8('/uploads/' + primary.name);
                    Module._wasm_on_file_uploaded(pathPtr, 0); // Auto-detect
                    _free(pathPtr);
                }
            };
            reader.readAsArrayBuffer(file);
        });
    });
});

EM_JS(void, js_init_pinch_zoom, (), {
    const canvas = document.getElementById('canvas');
    if (!canvas) return;

    let isGestureActive = false;
    let lastGestureScale = 1.0;
    let prevTouchDist = null;

    // 1. Trackpad pinch-to-zoom (Chrome, Firefox, Edge, Safari wheel with ctrlKey or metaKey)
    window.addEventListener('wheel', (e) => {
        if (e.ctrlKey || e.metaKey) {
            e.preventDefault();
            e.stopImmediatePropagation();

            if (isGestureActive) return;

            let dy = e.deltaY;
            if (e.deltaMode === 1) dy *= 20;
            else if (e.deltaMode === 2) dy *= 60;
            dy = Math.max(-100, Math.min(100, dy));
            const factor = Math.exp(-dy * 0.01);

            if (Module._wasm_on_pinch) {
                const rect = canvas.getBoundingClientRect();
                const x = e.clientX - rect.left;
                const y = e.clientY - rect.top;
                Module._wasm_on_pinch(factor, x, y);
            }
        }
    }, { capture: true, passive: false });

    // 2. Safari / WebKit native gesture events (macOS Safari trackpad & iOS Safari)
    canvas.addEventListener('gesturestart', (e) => {
        e.preventDefault();
        isGestureActive = true;
        lastGestureScale = 1.0;
    }, { passive: false });

    canvas.addEventListener('gesturechange', (e) => {
        e.preventDefault();
        if (lastGestureScale > 0) {
            const factor = e.scale / lastGestureScale;
            lastGestureScale = e.scale;
            if (Module._wasm_on_pinch) {
                const rect = canvas.getBoundingClientRect();
                const x = e.clientX - rect.left;
                const y = e.clientY - rect.top;
                Module._wasm_on_pinch(factor, x, y);
            }
        }
    }, { passive: false });

    canvas.addEventListener('gestureend', (e) => {
        e.preventDefault();
        isGestureActive = false;
        lastGestureScale = 1.0;
    }, { passive: false });

    // 3. Touchscreen two-finger pinch-to-zoom (iOS, Android, touch laptops)
    canvas.addEventListener('touchstart', (e) => {
        if (e.touches.length === 2) {
            const t0 = e.touches[0];
            const t1 = e.touches[1];
            prevTouchDist = Math.hypot(t1.clientX - t0.clientX, t1.clientY - t0.clientY);
            e.preventDefault();
        } else {
            prevTouchDist = null;
        }
    }, { passive: false });

    canvas.addEventListener('touchmove', (e) => {
        if (e.touches.length === 2 && prevTouchDist !== null && prevTouchDist > 0) {
            e.preventDefault();
            const t0 = e.touches[0];
            const t1 = e.touches[1];
            const dist = Math.hypot(t1.clientX - t0.clientX, t1.clientY - t0.clientY);
            if (dist > 0) {
                const factor = dist / prevTouchDist;
                prevTouchDist = dist;
                const rect = canvas.getBoundingClientRect();
                const midX = (t0.clientX + t1.clientX) * 0.5 - rect.left;
                const midY = (t0.clientY + t1.clientY) * 0.5 - rect.top;
                if (Module._wasm_on_pinch) {
                    Module._wasm_on_pinch(factor, midX, midY);
                }
            }
        }
    }, { passive: false });

    canvas.addEventListener('touchend', (e) => {
        if (e.touches.length < 2) {
            prevTouchDist = null;
        }
    });

    canvas.addEventListener('touchcancel', (e) => {
        prevTouchDist = null;
    });
});

extern "C" {
EMSCRIPTEN_KEEPALIVE void wasm_on_file_uploaded(const char* path, int target_type) {
    if (!path) return;
    tmm::handle_web_file_upload(path, target_type);
}
EMSCRIPTEN_KEEPALIVE void wasm_on_pinch(float factor, float center_x, float center_y) {
    tmm::handle_web_pinch(factor, center_x, center_y);
}
}

namespace tmm {

void web_init_file_io() {
    std::error_code ec;
    fs::create_directories("/downloads", ec);
    fs::create_directories("/uploads", ec);
    js_init_drag_drop();
    js_init_pinch_zoom();
}

void web_trigger_file_dialog(const char* accept, int target_type) {
    js_trigger_file_input(accept, target_type);
}

void web_download_file(const char* virtual_path, const char* download_name) {
    js_download_file(virtual_path, download_name);
}

void web_flush_downloads() {
    std::error_code ec;
    if (!fs::exists("/downloads", ec)) return;

    for (const auto& entry : fs::directory_iterator("/downloads", ec)) {
        if (entry.is_regular_file()) {
            const std::string path = entry.path().string();
            const std::string filename = entry.path().filename().string();
            web_download_file(path.c_str(), filename.c_str());
            fs::remove(entry.path(), ec);
        }
    }
}

} // namespace tmm

#endif // __EMSCRIPTEN__
