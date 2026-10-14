// Run: node tools/test-badgectl-apps.mjs
// Optional example execution: BADGE_LUA=/path/to/lua node tools/test-badgectl-apps.mjs
import assert from 'node:assert/strict';
import fs from 'node:fs';
import vm from 'node:vm';
import os from 'node:os';
import path from 'node:path';
import { execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = new URL('./badgectl/', import.meta.url);
const guide = fs.readFileSync(new URL('web/README.md', root), 'utf8');
const ctx = vm.createContext({ TextEncoder });
vm.runInContext(fs.readFileSync(new URL('web/app-bundle.js', root), 'utf8'), ctx);
const { parse, serialize } = ctx.BadgeAppBundle;
const examples = [...guide.matchAll(/```lua\n(--\[==\[badge-app\n[\s\S]*?)\n```/g)].map(m => m[1]);
assert.equal(examples.length, 8, 'hello template plus seven complete apps');
assert.equal(new Set(examples.map(s => parse(s).slug)).size, 8);
for (const source of examples) {
  const app = parse(source);
  assert.match(app.manifest, /^api=2$/m);
  assert.match(app.main, /function on_enter\(root\)/);
  assert.deepEqual(parse(serialize(app.manifest, app.main)), app);
  assert.deepEqual(parse('```lua\n' + source + '\n```'), app);
  assert.deepEqual(parse('\uFEFF' + source.replaceAll('\n', '\r\n')), app);
}
const hello = examples[0];
for (const [input, error] of [
  [hello.replace('my_hello', '../oops'), /slug/],
  [hello.replace('api=2', 'api=3'), /api must/],
  [hello.replace('api=2', 'api=2\napi=1'), /Duplicate/],
  [hello.replace('api=2', 'api=2 # comment'), /separate lines/],
  [hello.replace('api=2', 'heap_kb=64'), /heap_kb/],
  [hello.replace('api=2', 'home_button=1\nconfirm_home=1'), /cannot be combined/],
  [hello.replace('api=2', 'made_up=1'), /Unknown/],
  [hello.replace('name=My Hello', 'name=' + 'é'.repeat(25)), /48 UTF-8 bytes/],
  [hello.replace('icon=HI', 'icon=' + 'é'.repeat(7)), /12 UTF-8 bytes/],
  [hello.replace(']==]', ']]'), /End the manifest/],
  [hello.replace('api=2', '# ]==] injected'), /delimiter/],
  [hello + '\0', /NUL/],
  ['Here is your app:\n' + hello, /Start the file/],
  [hello.slice(0, hello.indexOf('function')), /full Lua/],
  [hello + 'x'.repeat(65536), /64 KiB/],
  ['```lua\n' + hello + '\n```\n```lua\nprint("extra")\n```', /multiple/],
]) assert.throws(() => parse(input), error);

// Import uses the actual application handler: malformed input preserves the
// workspace; success removes old assets/models and saves the split app.
const elements = new Map();
function element(id) {
  if (!elements.has(id)) elements.set(id, {
    textContent: '', value: '', className: '', disabled: false,
    classList: { add() {} },
    appendChild() {}, close() { this.closed = true; },
    getContext() { return { clearRect() {} }; },
  });
  return elements.get(id);
}
const storage = new Map();
const appCtx = vm.createContext({
  TextEncoder, Uint8Array, Uint8ClampedArray, BadgeAppBundle: ctx.BadgeAppBundle,
  document: { getElementById: element, createElement: () => element(Symbol()) },
  window: { addEventListener() {} },
  localStorage: { setItem: (k, v) => storage.set(k, v) },
  monaco: { editor: { createModel: content => ({ getValue: () => content, onDidChangeContent() {}, dispose() {} }) } },
});
vm.runInContext(fs.readFileSync(new URL('web/app.js', root), 'utf8'), appCtx);
vm.runInContext(`state.editor = {setModel(){}};
  state.files.set('main.lua', 'old code');
  state.files.set('manifest.cfg', 'slug=old');
  state.files.set('README.md', 'guide');
  state.files.set('icon.bin', new Uint8Array([1]));
  state.files.set('old-module.lua', 'old module');
  var disposed = false;
  state.models.set('main.lua', {dispose(){disposed=true;}});`, appCtx);
element('app-source').value = 'invalid';
vm.runInContext('importApp()', appCtx);
assert.equal(vm.runInContext("state.files.get('main.lua')", appCtx), 'old code');
assert.equal(vm.runInContext('disposed', appCtx), false);
element('app-source').value = hello;
vm.runInContext('importApp()', appCtx);
assert.equal(vm.runInContext('disposed', appCtx), true);
assert.deepEqual(Array.from(vm.runInContext('[...state.files.keys()]', appCtx)), ['manifest.cfg', 'main.lua', 'README.md']);
assert.equal(vm.runInContext("state.files.get('main.lua')", appCtx), parse(hello).main);
assert.equal(element('import-app-dialog').closed, true);
assert.match(storage.get('badgectl:files:v1'), /Hello, badge!/);
assert.equal(vm.runInContext('state.writer', appCtx), null, 'import never opens USB');

// Ensure documentation and static script links ship without a build step.
const html = fs.readFileSync(new URL('web/index.html', root), 'utf8');
assert.ok(html.indexOf('src="app-bundle.js') < html.indexOf('src="app.js'));
assert.match(html, /href="README.md" download=/);
console.log('PASS: all 8 documented apps import/round-trip; malformed manifests and size limits; workspace replacement preserves guide and rejects invalid imports without mutations');

if (process.env.BADGE_LUA) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'badge-guide-'));
  try {
    for (const source of examples) {
      const app = parse(source);
      const file = path.join(dir, app.slug + '.lua');
      fs.writeFileSync(file, source);
      const result = execFileSync(process.env.BADGE_LUA, [fileURLToPath(new URL('test-guide.lua', root)), file, app.slug], { encoding: 'utf8' });
      process.stdout.write(result);
    }
  } finally { fs.rmSync(dir, { recursive: true }); }
} else {
  console.log('Lua lifecycle checks skipped; set BADGE_LUA to a host Lua interpreter to run them.');
}
