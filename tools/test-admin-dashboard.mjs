// Run the actual dashboard script against a deterministic USB transport.
import assert from 'node:assert/strict';
import fs from 'node:fs';
import vm from 'node:vm';

const html = fs.readFileSync(new URL('./badge-provision/index.html', import.meta.url), 'utf8');
const script = html.match(/<script>([\s\S]*?)<\/script>/)[1].split('// ---------------------------------------------------------------- wiring')[0];
const password = 'test-admin-password-never-deployed';
const fields = {display_name:'Badge QA',role:'hacker',app_mode:'default',account_email:'qa@example.com',attendee_id:'123',badge_id:'badge-qa'};
function harness(auth='ADMIN AUTH OK', mode='APP MODE OK default') {
  const elements = new Map();
  const element = id => {
    if (!elements.has(id)) elements.set(id, {
      value: id === '#admin-password' ? password : '', textContent:'', innerHTML:'',
      append(){}, focus(){}, classList:{toggle(){}},
      querySelectorAll(){return Object.entries(fields).map(([name,value])=>({name,value,parentElement:{querySelector(){return null;}},classList:{toggle(){}}}));},
    });
    return elements.get(id);
  };
  const calls=[], records=[], statuses=[];
  let payload='';
  const ctx = vm.createContext({TextEncoder,TextDecoder,console,setTimeout,clearTimeout,
    document:{querySelector:element,createElement:()=>element('new')},
    localStorage:{getItem:()=>null,setItem(){throw Error('unexpected credential persistence');}},
    recordEntry:r=>records.push(r),statusEntry:(...s)=>statuses.push(s),
    send:s=>calls.push(s),payload:b=>{payload+=new TextDecoder().decode(b);},
    fakeAsk:async cmd=>{
      calls.push(cmd);
      if(cmd.startsWith('appmode check '))return `${cmd}\r\n${auth}\r\nbadge> `;
      if(cmd.startsWith('appmode '))return `${cmd}\r\n${mode}\r\nbadge> `;
      if(cmd.startsWith('cat '))return payload+'\r\nbadge> ';
      if(cmd==='prov apply')return 'PROV OK\nmac=12:34:56:78:90:ab\nbadge> ';
      throw Error('Unexpected command: '+cmd);
    }});
  vm.runInContext(script,ctx);
  vm.runInContext(`
    sync=async()=>true; recoverConnection=async()=>true;
    sendLine=async s=>send(s); writeBytes=async b=>payload(b);
    waitFor=async token=>token; ask=fakeAsk;
    record=recordEntry; setStatus=statusEntry; resetForm=()=>{};
  `,ctx);
  return {ctx,calls,records,statuses,elements,run:()=>vm.runInContext('provision({preventDefault(){}})',ctx)};
}
const ok=harness();await ok.run();
assert.equal(ok.records.length,1,JSON.stringify({calls:ok.calls,statuses:ok.statuses}));
assert.ok(ok.calls[0].startsWith('appmode check '));
assert.ok(ok.calls[1].startsWith('put '));
assert.equal(ok.calls.at(-1),`appmode default ${password}`);
assert.ok(!JSON.stringify(ok.records).includes(password));
assert.equal(ok.elements.get('#admin-password').value,password);
for(const reply of ['ADMIN AUTH FAIL','Unrecognized command','usage: appmode <default|public|all>']){
  const h=harness(reply);await h.run();assert.equal(h.records.length,0);
  assert.ok(h.calls.every(c=>c.startsWith('appmode check ')));
  assert.ok(!JSON.stringify(h.statuses).includes(password));
}
const missing=harness();missing.elements.get('#admin-password').value='';await missing.run();assert.equal(missing.calls.length,0);
const partial=harness('ADMIN AUTH OK','ADMIN AUTH FAIL');await partial.run();assert.equal(partial.records.length,0);
assert.equal(partial.statuses.at(-1)[0],'Identity programmed — app mode still needs attention');
assert.ok(!JSON.stringify(partial.statuses).includes(password));
// Password is outside the identity form and has no serializable field name.
const form=html.match(/<form id="form"[\s\S]*?<\/form>/)[0];assert.ok(!form.includes('admin-password'));
assert.ok(html.includes('if (!sensitiveReply) log(text)'));
// Real askAdmin sanitizes the echoed command even when the transport returns it.
const redacted=await vm.runInContext(`askAdmin('check', '${password}')`,ok.ctx);
assert.ok(!redacted.includes(password));assert.ok(redacted.includes('[hidden]'));
// Both scripts must parse, and the engineer command must use the same protocol.
const eng=fs.readFileSync(new URL('./web-flasher/eng/index.html', import.meta.url),'utf8');
// Neither operator UI commits the Admin password; the operator types it into a masked field.
function assertNoCommittedAdminPassword(page) {
  const input = page.match(/<input\b[^>]*\bid="admin-password"[^>]*>/)?.[0];
  assert.ok(input, 'Admin password field must exist');
  assert.ok(!/\bvalue="[^"]+"/.test(input), 'Admin password must not have a committed default');
  assert.ok(input.includes('type="password"'), 'Admin password must stay masked');
}
assertNoCommittedAdminPassword(html);
assertNoCommittedAdminPassword(eng);
const authSource=fs.readFileSync(new URL('../main/hal/admin_auth.cpp', import.meta.url),'utf8');
assert.ok(/kAdminPasswordSha256\[[^\]]+\]\s*=\s*\{[^}]+\}/.test(authSource),
  'Firmware Admin password verifier must exist');
console.log('Admin password: both masked UI fields ship without a committed default PASS');
const engScript=eng.match(/<script type="module">([\s\S]*?)<\/script>/)[1].replace(/^\s*import .*;$/m,'');
new vm.Script(engScript);
assert.ok(engScript.includes('`appmode ${mode} ${password}\\r`'));
assert.ok(!engScript.includes('\\r\\n'));
assert.ok(engScript.includes('ADMIN AUTH FAIL'));
console.log('Dashboard: auth preflight before identity, old firmware, missing/wrong passwords, partial success, credential isolation and engineer protocol PASS');

// Exercise the actual engineer post-flash handshake with one-byte USB chunks.
const engContext = vm.createContext({TextEncoder,TextDecoder,Date,
  document:{getElementById:()=>({})},
});
const engFunctions=engScript.split('// Show the firmware build hash')[0]
  .replace('const sleep = (ms) => new Promise((r) => setTimeout(r, ms));',
           'const sleep = ms => ms === 3000 ? Promise.resolve() : new Promise(()=>{});');
vm.runInContext(engFunctions,engContext);
for(const result of ['APP MODE OK default','ADMIN AUTH FAIL','Unrecognized command']) {
  let pending=[],writes=[],closed=false;
  const port={
    async open(){},async close(){closed=true;},
    readable:{getReader:()=>({
      async read(){return pending.length?{value:Uint8Array.of(pending.shift()),done:false}:{done:true};},
      async cancel(){},releaseLock(){},
    })},
    writable:{getWriter:()=>({
      async write(bytes){const cmd=new TextDecoder().decode(bytes);writes.push(cmd);
        pending=[...new TextEncoder().encode(cmd.startsWith('appmode')?`${cmd}\r\n${result}\r\nbadge> `:'badge> ')];},
      releaseLock(){},
    })},
  };
  engContext.testPort=port;engContext.testPassword=password;
  const operation=vm.runInContext("applyAppMode(testPort, 'default', testPassword)",engContext);
  if(result.startsWith('APP MODE OK'))await operation;
  else await assert.rejects(operation,e=>!e.message.includes(password));
  assert.equal(writes[1],`appmode default ${password}\r`);assert.ok(closed);
}
console.log('Engineer flasher: fragmented replies, authenticated mode changes, rejection and port cleanup PASS');
