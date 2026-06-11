const express = require('express');
const path = require('path');

const app = express();
const port = 8100;

// SharedArrayBuffer 활성화용 헤더
app.use((req, res, next) => {
    res.setHeader('Cross-Origin-Opener-Policy', 'same-origin');
    res.setHeader('Cross-Origin-Embedder-Policy', 'require-corp');
    next();
});

app.use(express.static(__dirname, {
    setHeaders: (res, filePath) => {
        if (filePath.endsWith('.wasm')) {
            res.setHeader('Content-Type', 'application/wasm');
        }
    }
}));

app.listen(port, () => {
    console.log(`mGBA WASM server running at http://localhost:${port}`);
});