const express = require('express');
const cors = require('cors');
const path = require('path');

const app = express();
const port = 8100;

app.use(cors());

app.use(express.static(__dirname, {
    setHeaders: (res, path) => {
        if (path.endsWith('.wasm')) {
            res.set('Content-Type', 'application/wasm');
        }
    }
}));

app.listen(port, () => {
    console.log(`mGBA WASM server running at http://localhost:${port}`);
});
