const http = require('node:http');
const fs = require('node:fs');
const path = require('node:path');
const { spawn } = require('node:child_process');
const { SerialPort } = require('serialport');

const HTTP_PORT = parseInt(process.env.PORT || '3001', 10);
const SERIAL_PATH = process.env.SERIAL_PORT;
const BAUD = parseInt(process.env.BAUD || '115200', 10);

const FFMPEG = process.env.FFMPEG || 'ffmpeg';
const CAM_FORMAT = process.env.CAM_FORMAT
    || (process.platform === 'darwin' ? 'avfoundation'
        : process.platform === 'linux' ? 'v4l2' : 'dshow');
const CAM_INPUT = process.env.CAM_INPUT
    || (process.platform === 'darwin' ? '0' : '/dev/video0');
const CAM_FRAMERATE = process.env.CAM_FRAMERATE || '30';
const CAM_SIZE = process.env.CAM_SIZE || '1280x720';
const CAM_QUALITY = process.env.CAM_QUALITY || '8'; // mjpeg -q:v
const H264_ENCODER = process.env.H264_ENCODER
    || (process.platform === 'darwin' ? 'h264_videotoolbox' : 'libx264');
const H264_PRESET = process.env.H264_PRESET || 'ultrafast'; // libx264 only
const H264_GOP = process.env.H264_GOP || CAM_FRAMERATE; // keyframe ~every 1s
const H264_BITRATE = process.env.H264_BITRATE || '2M';

const VALID = new Set(['A', 'B', 'Home', 'Down', 'Left', 'Right', 'Up', 'Aux1', 'Start']);

const COMMON_INPUT_ARGS = [
    '-hide_banner', '-loglevel', 'error',
    '-fflags', 'nobuffer',
    '-flags', 'low_delay',
    '-probesize', '32',
    '-analyzeduration', '0',
    '-f', CAM_FORMAT,
    '-framerate', CAM_FRAMERATE,
    '-video_size', CAM_SIZE,
    '-i', CAM_INPUT,
];

// ---------- MJPEG pipeline ----------
const SOI = Buffer.from([0xff, 0xd8, 0xff]);
const EOI = Buffer.from([0xff, 0xd9]);
const mjpegSubs = new Set();
let mjpegProc = null;

function startMjpeg() {
    if (mjpegProc) return;
    const args = [
        ...COMMON_INPUT_ARGS,
        '-c:v', 'mjpeg', '-q:v', CAM_QUALITY,
        '-f', 'mjpeg', 'pipe:1',
    ];
    console.log(`spawning mjpeg ffmpeg`);
    mjpegProc = spawn(FFMPEG, args, { stdio: ['ignore', 'pipe', 'pipe'] });
    mjpegProc.on('error', (err) => { console.error('mjpeg ffmpeg error:', err.message); mjpegProc = null; });
    mjpegProc.on('exit', (code, signal) => { console.log(`mjpeg ffmpeg exited (${code}/${signal})`); mjpegProc = null; });
    mjpegProc.stderr.on('data', (d) => process.stderr.write(d));

    let buf = Buffer.alloc(0);
    mjpegProc.stdout.on('data', (chunk) => {
        buf = Buffer.concat([buf, chunk]);
        while (true) {
            const start = buf.indexOf(SOI);
            if (start < 0) { buf = Buffer.alloc(0); break; }
            const end = buf.indexOf(EOI, start + 3);
            if (end < 0) { if (start > 0) buf = buf.subarray(start); break; }
            broadcastMjpeg(buf.subarray(start, end + 2));
            buf = buf.subarray(end + 2);
        }
    });
}
function stopMjpegIfIdle() {
    if (mjpegProc && mjpegSubs.size === 0) { mjpegProc.kill('SIGTERM'); mjpegProc = null; }
}
function broadcastMjpeg(frame) {
    const header = `--frame\r\nContent-Type: image/jpeg\r\nContent-Length: ${frame.length}\r\n\r\n`;
    for (const res of mjpegSubs) {
        if (res.writableEnded || res.destroyed) continue;
        res.write(header); res.write(frame); res.write('\r\n');
    }
}

// ---------- MP4 (fMP4 / H.264) pipeline ----------
const mp4Subs = new Set();
let mp4Proc = null;
let mp4Init = null;            // cached init segment (ftyp+moov)
let mp4InitAccum = Buffer.alloc(0);

// Walk top-level mp4 boxes; return offset of first 'moof', or -1 if not yet present.
function findFirstMoof(buf) {
    let i = 0;
    while (i + 8 <= buf.length) {
        const size = buf.readUInt32BE(i);
        const type = buf.toString('ascii', i + 4, i + 8);
        if (type === 'moof') return i;
        if (size === 0) return -1;             // box extends to EOF; can't advance
        if (size === 1) {                      // 64-bit largesize
            if (i + 16 > buf.length) return -1;
            const big = Number(buf.readBigUInt64BE(i + 8));
            if (big < 16) return -1;
            i += big;
        } else {
            if (size < 8) return -1;
            i += size;
        }
        if (i > buf.length) return -1;          // partial box; wait for more
    }
    return -1;
}

function startMp4() {
    if (mp4Proc) return;
    mp4Init = null;
    mp4InitAccum = Buffer.alloc(0);
    const encoderArgs = H264_ENCODER === 'h264_videotoolbox'
        ? [
            '-c:v', 'h264_videotoolbox',
            '-realtime', '1',
            '-allow_sw', '1',
            '-profile:v', 'baseline', '-level', '4.0',
            '-pix_fmt', 'yuv420p',
            '-b:v', H264_BITRATE,
            '-g', String(H264_GOP), '-keyint_min', String(H264_GOP),
        ]
        : [
            '-c:v', 'libx264',
            '-preset', H264_PRESET,
            '-tune', 'zerolatency',
            '-profile:v', 'baseline', '-level', '4.0',
            '-pix_fmt', 'yuv420p',
            '-bf', '0',
            '-g', String(H264_GOP), '-keyint_min', String(H264_GOP),
            '-b:v', H264_BITRATE,
        ];
    const args = [
        ...COMMON_INPUT_ARGS,
        '-an',
        ...encoderArgs,
        '-f', 'mp4',
        '-movflags', '+frag_keyframe+empty_moov+default_base_moof+omit_tfhd_offset',
        '-frag_duration', '100000', // 100ms fragments
        'pipe:1',
    ];
    console.log(`mp4 encoder: ${H264_ENCODER} ${CAM_SIZE}@${CAM_FRAMERATE} ${H264_BITRATE}`);
    console.log(`spawning mp4 ffmpeg`);
    mp4Proc = spawn(FFMPEG, args, { stdio: ['ignore', 'pipe', 'pipe'] });
    mp4Proc.on('error', (err) => { console.error('mp4 ffmpeg error:', err.message); mp4Proc = null; });
    mp4Proc.on('exit', (code, signal) => { console.log(`mp4 ffmpeg exited (${code}/${signal})`); mp4Proc = null; mp4Init = null; });
    mp4Proc.stderr.on('data', (d) => process.stderr.write(d));

    mp4Proc.stdout.on('data', (chunk) => {
        if (!mp4Init) {
            mp4InitAccum = Buffer.concat([mp4InitAccum, chunk]);
            const moof = findFirstMoof(mp4InitAccum);
            if (moof < 0) return;              // wait for more
            mp4Init = mp4InitAccum.subarray(0, moof);
            const tail = mp4InitAccum.subarray(moof);
            mp4InitAccum = Buffer.alloc(0);
            console.log(`mp4 init segment: ${mp4Init.length} bytes`);
            // Push init to anyone already waiting.
            for (const res of mp4Subs) {
                if (!res.initSent && !res.writableEnded && !res.destroyed) {
                    res.write(mp4Init); res.initSent = true;
                }
            }
            broadcastMp4(tail);
        } else {
            broadcastMp4(chunk);
        }
    });
}
function stopMp4IfIdle() {
    if (mp4Proc && mp4Subs.size === 0) {
        mp4Proc.kill('SIGTERM');
        mp4Proc = null;
        mp4Init = null;
        mp4InitAccum = Buffer.alloc(0);
    }
}
function broadcastMp4(data) {
    if (!data || data.length === 0) return;
    for (const res of mp4Subs) {
        if (res.writableEnded || res.destroyed) continue;
        if (!res.initSent) {
            if (mp4Init) { res.write(mp4Init); res.initSent = true; }
            else continue;
        }
        res.write(data);
    }
}

// ---------- HTTP ----------
async function main() {
    if (!SERIAL_PATH) {
        console.error('Set SERIAL_PORT env var, e.g.:');
        console.error('  SERIAL_PORT=/dev/tty.usbmodem1101 node server.js');
        console.error('\nDetected ports:');
        const ports = await SerialPort.list();
        if (ports.length === 0) console.error('  (none)');
        for (const p of ports) console.error(`  ${p.path}  ${p.manufacturer || ''}`);
        process.exit(1);
    }

    const sp = new SerialPort({ path: SERIAL_PATH, baudRate: BAUD });
    sp.on('open', () => console.log(`serial open: ${SERIAL_PATH} @ ${BAUD}`));
    sp.on('error', (err) => console.error('serial error:', err.message));
    sp.on('data', (d) => process.stdout.write(d));

    const server = http.createServer((req, res) => {
        res.setHeader('access-control-allow-origin', '*');
        res.setHeader('access-control-allow-methods', 'GET, POST, OPTIONS');
        res.setHeader('access-control-allow-headers', 'content-type');
        if (req.method === 'OPTIONS') { res.writeHead(204); return res.end(); }

        const urlPath = req.url.split('?', 1)[0];

        if (req.method === 'GET' && (urlPath === '/' || urlPath === '/index.html')) {
            res.setHeader('content-type', 'text/html; charset=utf-8');
            fs.createReadStream(path.join(__dirname, 'index.html')).pipe(res);
            return;
        }
        if (req.method === 'GET' && urlPath === '/stream.mjpg') {
            res.writeHead(200, {
                'content-type': 'multipart/x-mixed-replace; boundary=frame',
                'cache-control': 'no-cache, no-store, must-revalidate',
                'connection': 'close',
            });
            mjpegSubs.add(res);
            console.log(`mjpeg client +1 (total ${mjpegSubs.size})`);
            startMjpeg();
            const cleanup = () => {
                if (mjpegSubs.delete(res)) console.log(`mjpeg client -1 (total ${mjpegSubs.size})`);
                stopMjpegIfIdle();
            };
            req.on('close', cleanup); res.on('close', cleanup);
            return;
        }
        // if (req.method === 'GET' && urlPath === '/stream.mp4') {
        //     res.writeHead(200, {
        //         'content-type': 'video/mp4',
        //         'cache-control': 'no-cache, no-store, must-revalidate',
        //         'connection': 'close',
        //     });
        //     res.initSent = false;
        //     mp4Subs.add(res);
        //     console.log(`mp4 client +1 (total ${mp4Subs.size})`);
        //     startMp4();
        //     // If init is already known, send it immediately so this client doesn't wait.
        //     if (mp4Init) { res.write(mp4Init); res.initSent = true; }
        //     const cleanup = () => {
        //         if (mp4Subs.delete(res)) console.log(`mp4 client -1 (total ${mp4Subs.size})`);
        //         stopMp4IfIdle();
        //     };
        //     req.on('close', cleanup); res.on('close', cleanup);
        //     return;
        // }
        if (req.method === 'POST' && urlPath === '/press') {
            let body = '';
            req.on('data', (c) => (body += c));
            req.on('end', () => {
                let btn;
                try { btn = JSON.parse(body).button; }
                catch { res.writeHead(400); return res.end('bad json'); }
                if (!VALID.has(btn)) { res.writeHead(400); return res.end('bad button'); }
                // Wire protocol now talks to esp_console on the badge:
                // "press <name>\n" runs the registered command. Console
                // prompt ("badge> ") is echoed back over the same serial
                // port — sp.on('data') just forwards it to stdout, which
                // is harmless. Set REPL prompt to "" if it gets noisy.
                sp.write(`press ${btn}\n`, (err) => {
                    if (err) { res.writeHead(500); return res.end(err.message); }
                    console.log(`-> press ${btn}`);
                    res.writeHead(204); res.end();
                });
            });
            return;
        }
        res.writeHead(404); res.end();
    });
    server.listen(HTTP_PORT, () => console.log(`http://localhost:${HTTP_PORT}`));
}

main();
