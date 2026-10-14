// Run with node tools/test-badgectl-icons.mjs. No browser or badge required.
import assert from 'node:assert/strict';
import fs from 'node:fs';
import vm from 'node:vm';
const source = fs.readFileSync(new URL('./badgectl/web/app.js', import.meta.url), 'utf8');
function harness(binarySupport, binary=true) {
  const calls=[],payloads=[],elements=new Map();
  const elem=id=>{if(!elements.has(id))elements.set(id,{textContent:'',className:'',disabled:false,appendChild(){}});return elements.get(id);};
  const ctx=vm.createContext({Uint8Array,Uint8ClampedArray,TextEncoder,TextDecoder,performance,setTimeout,clearTimeout,
    document:{getElementById:elem,createElement:()=>({})},window:{addEventListener(){}},calls,payloads,
    btoa:s=>Buffer.from(s,'binary').toString('base64'),atob:s=>Buffer.from(s,'base64').toString('binary')});
  vm.runInContext(source,ctx);
  vm.runInContext(`state.writer={};state.files.set('manifest.cfg','slug=hello\\n');state.files.set('main.lua','hello');
    sendLine=async line=>{calls.push(line);state.readBuffer += line==='put --binary' ? ${JSON.stringify(binarySupport?'PUT BINARY OK\nbadge> ':'usage: put <path> <size>\nbadge> ')} : line.startsWith('put ') ? 'READY' : line==='reload' ? 'reload: 0 new apps\\nbadge> ' : 'badge> ';};
    sendBytes=async bytes=>{payloads.push([...bytes]);state.readBuffer+='OK '+bytes.length+'\\nbadge> ';};
    if(${binary}) state.files.set('icon.bin', Uint8Array.from({length:256},(_,i)=>i));`,ctx);
  return {ctx,calls,payloads,elements};
}
const old=harness(false);await vm.runInContext('push()',old.ctx);
assert.deepEqual(old.calls,['','put --binary','']);assert.equal(old.payloads.length,0);
assert.match(old.elements.get('status').textContent,/firmware needs an update/);
const ok=harness(true);await vm.runInContext('push()',ok.ctx);
assert.ok(ok.calls.includes('reload'));assert.deepEqual(Array.from(ok.payloads[0]),Array.from({length:256},(_,i)=>i));
assert.match(ok.elements.get('status').textContent,/pushed hello/);
const text=harness(false,false);await vm.runInContext('push()',text.ctx);
assert.ok(!text.calls.includes('put --binary'));assert.match(text.elements.get('status').textContent,/pushed hello/);
const uploading=harness(false,false);
const pendingPush=vm.runInContext('push()',uploading.ctx);
assert.equal(uploading.elements.get('import-app').disabled,true,'do not replace files during a push');
await pendingPush;
assert.equal(uploading.elements.get('import-app').disabled,false);
await vm.runInContext('disconnect()',uploading.ctx);
assert.equal(uploading.elements.get('import-app').disabled,false,'import is available without a badge connection');
const result=vm.runInContext(`(()=>{const rgba=new Uint8ClampedArray(42*42*4);rgba.set([255,0,0,255,0,255,0,13,0,0,255,0]);const bin=encodeIconBin(rgba,42,42);return {bin:[...bin],pixels:[...decodeIconBin(bin).rgba.slice(0,12)]};})()`,ok.ctx);
assert.deepEqual(Array.from(result.bin.slice(0,12)),[25,20,0,0,42,0,42,0,84,0,0,0]);
assert.deepEqual(Array.from(result.bin.slice(12,18)),[0,248,224,7,31,0]);
assert.deepEqual(Array.from(result.pixels),[255,0,0,255,0,255,0,13,0,0,255,0]);
assert.equal(result.bin.length,5304);
console.log('PASS: LVGL header/colors/alpha, binary byte preservation, old-firmware rejection before writes, text-only compatibility, reload after push');
