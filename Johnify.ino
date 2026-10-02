#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <stdarg.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <mbedtls/gcm.h>
#include <mbedtls/sha256.h>

// ---------------------------------------------------------------------------
// Settings. Change these before uploading if you need to.
// ---------------------------------------------------------------------------
#define AP_SSID_PREFIX "Johnify-"   // Wi-Fi name is this plus 4 characters from the board's MAC
#define AP_PASSWORD    ""           // Empty means an open network. Use 8 or more characters to set a password.
#define WIFI_CHANNEL   1            // Must be the same on every board, or they will not hear each other.
#define MAX_HOPS       4            // How many repeaters a message may pass through.
#define MAX_ROOMS      24           // Chats a single board remembers.
#define MAX_MSGS       200          // Recent messages a board keeps in memory.
#define MAX_TEXT       190          // Longest message in bytes. ESP-NOW packets are limited to 250 bytes.
#define ROOM_LEN       10           // Length of a chat code.
#define SEEN_SIZE      256          // How many recent message IDs are remembered to spot duplicates.
#define TXQ_SIZE       16           // Packets waiting to be sent.
#define RXQ_SIZE       16           // Packets waiting to be processed.

struct __attribute__((packed)) Pkt {
  uint8_t  magic[2];
  uint8_t  hops;
  uint8_t  gid[8];
  uint32_t msgId;
  uint32_t devId;
  uint8_t  nonce[12];
  uint8_t  len;
  uint8_t  body[12 + MAX_TEXT + 16];
};
#define PKT_HDR 32
static_assert(offsetof(Pkt, body) == PKT_HDR, "packet header should be 32 bytes");
static_assert(sizeof(Pkt) <= 250, "ESP-NOW packets can be at most 250 bytes");

struct Out { char *b; size_t cap; size_t n; };

WebServer   server(80);
DNSServer   dns;
Preferences prefs;

const uint8_t BCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
char     apSsid[32];
uint32_t bootId;
bool     repeaterOn = false;
uint32_t statRx = 0, statTx = 0, statRelay = 0;
uint32_t ledOffAt = 0;

struct Room {
  bool     used;
  char     code[ROOM_LEN + 1];
  uint8_t  key[32];
  uint8_t  gid[8];
  uint32_t lastUse;
} rooms[MAX_ROOMS];

struct Msg {
  uint32_t seq;
  uint32_t msgId, devId, ts;
  char     room[ROOM_LEN + 1];
  char     name[13];
  char     text[MAX_TEXT + 1];
} msgs[MAX_MSGS];
uint32_t nextSeq = 1;

uint64_t seenKeys[SEEN_SIZE];
int      seenPos = 0;

struct TxItem { bool used; uint32_t at; uint8_t len; uint8_t tries; Pkt p; } txq[TXQ_SIZE];

struct RxItem { uint8_t len; Pkt p; } rxq[RXQ_SIZE];
volatile int rxHead = 0, rxTail = 0;
portMUX_TYPE rxMux = portMUX_INITIALIZER_UNLOCKED;

char pollBuf[6144];

const char INDEX_HTML[] PROGMEM = R"HTML(<!DOCTYPE html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<meta name="color-scheme" content="light dark">
<meta name="theme-color" content="#1f8a70">
<title>Johnify</title>
<style>
:root{
  --bg:#f1f4f2;--sf:#ffffff;--tx:#16201c;--mu:#66756f;--ln:#e0e7e3;
  --pri:#1f8a70;--pritx:#ffffff;--soft:#e2f1ec;
  --out:#1f8a70;--outtx:#ffffff;--in:#ffffff;
  --danger:#c0392b;--ok:#25915f;--sh:0 1px 2px rgba(16,32,26,.08)}
@media(prefers-color-scheme:dark){:root{
  --bg:#0d1411;--sf:#16201c;--tx:#e6eee9;--mu:#8a9a93;--ln:#24312b;
  --pri:#35b28f;--pritx:#04130d;--soft:#1b2b25;
  --out:#1c7a64;--outtx:#ffffff;--in:#1c2823;
  --danger:#e8695c;--ok:#4cc38a;--sh:none}}
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent}
html,body{margin:0;height:100%}
body{display:flex;flex-direction:column;height:100dvh;background:var(--bg);color:var(--tx);
  font:16px/1.4 system-ui,-apple-system,"Segoe UI",Roboto,"Helvetica Neue",sans-serif}
button,input,textarea{font:inherit;color:inherit}
svg{display:block}

header{flex:none;display:flex;align-items:center;gap:8px;background:var(--sf);border-bottom:1px solid var(--ln);
  padding:10px 12px;padding-top:max(10px,env(safe-area-inset-top))}
.brand{flex:1;min-width:0;display:flex;align-items:center;gap:8px}
.name{font-weight:700;font-size:19px;letter-spacing:-.3px;overflow:hidden;text-overflow:ellipsis}
.dot{flex:none;width:8px;height:8px;border-radius:50%;background:var(--mu)}
.dot.on{background:var(--ok)}.dot.off{background:var(--danger)}
.btn{display:inline-flex;align-items:center;gap:6px;border:0;border-radius:999px;padding:9px 14px;
  font-weight:600;font-size:14px;background:var(--soft);color:var(--pri);cursor:pointer;white-space:nowrap}
.btn:active{opacity:.7}
.btn.pri{background:var(--pri);color:var(--pritx)}
.btn.ghost{background:none;color:var(--danger);padding:8px 10px}
.icon{flex:none;width:38px;height:38px;border-radius:50%;border:0;background:none;color:var(--tx);
  display:grid;place-items:center;cursor:pointer}
.icon:active{background:var(--soft)}.icon.on{background:var(--soft);color:var(--pri)}
@media(max-width:370px){.btn{padding:8px 11px;font-size:13px}}

#view{flex:1;min-height:0;display:flex;flex-direction:column}
.scr{flex:1;min-height:0;display:flex;flex-direction:column}

.list{flex:1;overflow-y:auto;padding:12px}
.row{display:flex;align-items:center;gap:12px;padding:12px;margin-bottom:8px;background:var(--sf);
  border:1px solid var(--ln);border-radius:16px;box-shadow:var(--sh);cursor:pointer}
.row:active{background:var(--soft)}
.av{flex:none;width:46px;height:46px;border-radius:50%;display:grid;place-items:center;color:#fff;font-weight:700;font-size:15px}
.av.sm{width:36px;height:36px;font-size:13px}
.rt{flex:1;min-width:0}
.rcode{font-weight:700;font-size:16px;letter-spacing:.6px}
.rprev{color:var(--mu);font-size:14px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.rside{flex:none;display:flex;flex-direction:column;align-items:flex-end;gap:6px}
.rtime{font-size:12px;color:var(--mu)}
.badge{min-width:20px;height:20px;padding:0 6px;border-radius:10px;background:var(--pri);color:var(--pritx);
  font-size:12px;font-weight:700;display:grid;place-items:center}

.empty{margin:auto;text-align:center;padding:24px;max-width:340px}
.empty h2{margin:16px 0 6px;font-size:22px;letter-spacing:-.3px}
.empty p{margin:0;color:var(--mu)}
.steps{margin:18px 0 0;padding:0;list-style:none;text-align:left}
.steps li{display:flex;gap:12px;align-items:flex-start;margin-bottom:12px}
.steps b{flex:none;width:24px;height:24px;border-radius:50%;background:var(--soft);color:var(--pri);
  display:grid;place-items:center;font-size:13px}

.chead{flex:none;display:flex;align-items:center;gap:8px;padding:8px 10px;background:var(--sf);border-bottom:1px solid var(--ln)}
.cmid{flex:1;min-width:0;display:flex;flex-direction:column;align-items:flex-start;background:none;border:0;padding:0;cursor:pointer;text-align:left}
.ccode{font-weight:700;font-size:16px;letter-spacing:.6px}
.csub{display:flex;align-items:center;gap:4px;font-size:12px;color:var(--ok)}
.msgs{flex:1;min-height:0;overflow-y:auto;display:flex;flex-direction:column;gap:6px;padding:14px 12px}
.hint{margin:auto;color:var(--mu);font-size:14px}
.b{max-width:80%;padding:8px 12px 6px;border-radius:18px;word-break:break-word;box-shadow:var(--sh)}
.b.me{align-self:flex-end;background:var(--out);color:var(--outtx);border-bottom-right-radius:5px}
.b.them{align-self:flex-start;background:var(--in);border:1px solid var(--ln);border-bottom-left-radius:5px}
.who{font-size:12.5px;font-weight:700;margin-bottom:1px}
.tx{white-space:pre-wrap}
.tm{font-size:11px;opacity:.6;text-align:right;margin-top:1px}
.comp{flex:none;display:flex;align-items:flex-end;gap:8px;background:var(--sf);border-top:1px solid var(--ln);
  padding:8px 10px;padding-bottom:max(8px,env(safe-area-inset-bottom))}
textarea{flex:1;min-height:42px;max-height:120px;resize:none;border:0;outline:none;background:var(--bg);
  border-radius:21px;padding:10px 16px;line-height:1.35}
.send{flex:none;width:42px;height:42px;border-radius:50%;border:0;background:var(--pri);color:var(--pritx);
  display:grid;place-items:center;cursor:pointer}
.send:active{opacity:.7}

.page{flex:1;overflow-y:auto;padding:4px 12px 24px}
.lab{margin:18px 6px 6px;font-size:12px;font-weight:700;letter-spacing:.6px;text-transform:uppercase;color:var(--mu)}
.grp{background:var(--sf);border:1px solid var(--ln);border-radius:16px;overflow:hidden}
.item{display:flex;align-items:center;gap:12px;padding:13px 14px;border-top:1px solid var(--ln)}
.item:first-child{border-top:0}
.itx{flex:1;min-width:0}.it{font-weight:600}.is{font-size:13px;color:var(--mu);margin-top:2px}
.iv{color:var(--mu);font-size:14px;text-align:right;word-break:break-all}
.field{flex:1;min-width:0;border:0;outline:none;background:none;text-align:right;font-size:16px}
.switch{position:relative;flex:none;width:48px;height:29px}
.switch input{position:absolute;inset:0;opacity:0;margin:0;cursor:pointer;z-index:1}
.knob{position:absolute;inset:0;border-radius:99px;background:var(--ln);transition:background .2s}
.knob:after{content:"";position:absolute;top:3px;left:3px;width:23px;height:23px;border-radius:50%;background:#fff;
  box-shadow:0 1px 3px rgba(0,0,0,.25);transition:transform .2s}
.switch input:checked+.knob{background:var(--pri)}
.switch input:checked+.knob:after{transform:translateX(19px)}

#modal{position:fixed;inset:0;background:rgba(8,14,11,.5);display:flex;align-items:center;justify-content:center;padding:20px}
#modal[hidden]{display:none}
.box{width:100%;max-width:340px;background:var(--sf);border-radius:20px;padding:20px;box-shadow:0 10px 40px rgba(0,0,0,.25)}
.box h3{margin:0 0 4px;font-size:20px}.box p{margin:0;color:var(--mu);font-size:14px}
.box input{width:100%;margin:14px 0;padding:13px;border:1.5px solid var(--ln);border-radius:12px;background:var(--bg);
  text-align:center;font-weight:700;font-size:20px;letter-spacing:2px;text-transform:uppercase;outline:none}
.box input:focus{border-color:var(--pri)}
.acts{display:flex;gap:8px}.acts .btn{flex:1;justify-content:center;padding:12px}
#toast{position:fixed;left:50%;bottom:90px;transform:translateX(-50%);background:#16201c;color:#fff;padding:10px 16px;
  border-radius:999px;font-size:14px;opacity:0;pointer-events:none;transition:opacity .2s;max-width:90%;text-align:center}
#toast.on{opacity:1}
</style></head><body>
<header>
 <div class="brand"><span class="name">Johnify</span><span id="dot" class="dot"></span></div>
 <button id="bCreate" class="btn pri"></button>
 <button id="bJoin" class="btn">Join</button>
 <button id="bSet" class="icon" aria-label="Settings"></button>
</header>
<main id="view"></main>
<div id="modal" hidden><div class="box">
 <h3>Join a chat</h3>
 <p>Enter the 10-character code you were given.</p>
 <input id="jc" maxlength="10" placeholder="CODE" autocapitalize="characters" autocomplete="off" spellcheck="false">
 <div class="acts"><button id="jx" class="btn">Cancel</button><button id="jg" class="btn pri">Join</button></div>
</div></div>
<div id="toast"></div>
<script>
const $=s=>document.querySelector(s);
const el=(t,c,x)=>{const e=document.createElement(t);if(c)e.className=c;if(x!=null)e.textContent=x;return e};
const bytes=s=>new TextEncoder().encode(s).length;
const fmtUp=s=>Math.floor(s/3600)+'h '+Math.floor(s%3600/60)+'m';
const fmtTime=t=>new Date(t).toLocaleTimeString([],{hour:'numeric',minute:'2-digit'});

const ICON={
  plus:'<path d="M12 5v14M5 12h14"/>',
  back:'<path d="M15 5l-7 7 7 7"/>',
  lock:'<rect x="5" y="11" width="14" height="9" rx="2"/><path d="M8 11V8a4 4 0 0 1 8 0v3"/>',
  up:'<path d="M12 19V5M6 11l6-6 6 6"/>',
  sliders:'<path d="M4 7h9M17 7h3M4 17h3M11 17h9"/><circle cx="15" cy="7" r="2"/><circle cx="9" cy="17" r="2"/>'};
const svg=(n,s)=>'<svg viewBox="0 0 24 24" width="'+s+'" height="'+s+'" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">'+ICON[n]+'</svg>';

const PAL=['#1f8a70','#d0743c','#b5483e','#6e8b3d','#c29a2c','#3f7f8c'];
const colorOf=s=>{let h=0;for(const c of s)h=(h*31+c.charCodeAt(0))>>>0;return PAL[h%PAL.length]};
function avatar(code,cls){const a=el('div','av '+(cls||''),code.slice(0,2));a.style.background=colorOf(code);return a}

let S={};try{S=JSON.parse(localStorage.getItem('jf')||'{}')}catch(e){}
S.rooms=S.rooms||[];S.msgs=S.msgs||{};S.since=S.since||0;S.name=S.name||'';
S.unread=S.unread||{};S.boot=S.boot||'';S.dev=S.dev||'';
const save=()=>{try{localStorage.setItem('jf',JSON.stringify(S))}catch(e){}};

let cur=null,view='list',busy=false,again=false,stT=null;

function toast(t){const e=$('#toast');e.textContent=t;e.classList.add('on');
  clearTimeout(toast.t);toast.t=setTimeout(()=>e.classList.remove('on'),2600)}
function setLink(ok){$('#dot').className='dot '+(ok?'on':'off');$('#dot').title=ok?'Connected to the board':'Not connected'}

async function api(p,o,post){
  const q=new URLSearchParams(o||{}),c=new AbortController(),t=setTimeout(()=>c.abort(),5000);
  try{
    const r=post?await fetch(p,{method:'POST',body:q,signal:c.signal}):await fetch(p+'?'+q,{signal:c.signal});
    if(!r.ok)throw new Error(await r.text());
    return await r.json();
  }finally{clearTimeout(t)}}

async function hello(){const h=await api('/api/hello',{dev:S.dev});S.dev=h.dev;save();return h}

function gen(){
  const A='ABCDEFGHJKMNPQRSTUVWXYZ23456789',a=new Uint8Array(10);
  if(window.crypto&&crypto.getRandomValues)crypto.getRandomValues(a);
  else for(let i=0;i<10;i++)a[i]=Math.random()*256;
  return[...a].map(x=>A[x%A.length]).join('')}

function copy(t){try{const a=document.createElement('textarea');a.value=t;document.body.appendChild(a);
  a.select();document.execCommand('copy');a.remove();toast('Code copied')}catch(e){toast('Code: '+t)}}

async function joinRoom(raw){
  const code=(raw||'').toUpperCase().replace(/[^A-Z0-9]/g,'');
  if(code.length!==10){toast('A chat code is 10 letters and numbers');return null}
  if(!S.rooms.includes(code)&&S.rooms.length>=12){toast('You can have up to 12 chats. Leave one first.');return null}
  try{if(!S.dev)await hello();await api('/api/join',{room:code},true)}
  catch(e){toast('Cannot reach the board. Check your Wi-Fi.');return null}
  if(!S.rooms.includes(code)){S.rooms.push(code);S.msgs[code]=S.msgs[code]||[];save()}
  try{const j=await api('/api/poll',{since:0,boot:'',r:code});ingest(j.msgs)}catch(e){}
  return code}

function ingest(ms){
  let changed=false;
  for(const m of ms){
    if(!S.rooms.includes(m.r))continue;
    const L=S.msgs[m.r]=S.msgs[m.r]||[],id=m.d+m.i;
    if(L.some(x=>x.id===id))continue;
    L.push({id,dev:m.d,name:m.n,text:m.x,t:Date.now()-m.a*1000,mine:m.d===S.dev});
    L.sort((a,b)=>a.t-b.t);if(L.length>300)L.shift();
    changed=true;
    if(!(view==='chat'&&cur===m.r)&&m.d!==S.dev)S.unread[m.r]=(S.unread[m.r]||0)+1}
  if(changed){save();if(view==='chat')drawMsgs();else if(view==='list')render()}}

async function poll(){
  if(busy){again=true;return}
  busy=true;
  try{
    if(!S.dev)await hello();
    const j=await api('/api/poll',{since:S.since,boot:S.boot,r:S.rooms.join(',')});
    S.boot=j.boot;S.since=j.next;ingest(j.msgs);save();setLink(true)}
  catch(e){setLink(false)}
  busy=false;
  if(again){again=false;poll()}}

async function send(){
  const box=$('#txt');if(!box||!cur)return;
  const t=box.value.trim();if(!t)return;
  if(bytes(t)>190){toast('Too long. Limit is 190 bytes.');return}
  box.value='';box.style.height='auto';
  try{
    if(!S.dev)await hello();
    await api('/api/send',{room:cur,dev:S.dev,name:S.name||('User-'+S.dev.slice(0,4)),text:t},true);
    poll()}
  catch(e){box.value=t;toast('Did not send. Check your Wi-Fi.')}}

function goList(){view='list';cur=null;render()}
function openRoom(c){cur=c;view='chat';S.unread[c]=0;save();render()}

function render(){
  $('#bSet').classList.toggle('on',view==='settings');
  clearInterval(stT);
  const v=$('#view');v.innerHTML='';
  if(view==='list')drawList(v);else if(view==='chat')drawChat(v);else drawSettings(v)}

function backBtn(){const b=el('button','icon');b.innerHTML=svg('back',22);b.setAttribute('aria-label','Back');b.onclick=goList;return b}

function drawList(v){
  const scr=el('div','scr');
  if(!S.rooms.length){
    const c=el('div','empty');
    c.innerHTML='<svg viewBox="0 0 120 84" width="120" height="84" aria-hidden="true">'+
      '<rect x="6" y="6" width="70" height="40" rx="16" fill="var(--pri)"/>'+
      '<circle cx="28" cy="26" r="4" fill="var(--pritx)"/><circle cx="41" cy="26" r="4" fill="var(--pritx)"/><circle cx="54" cy="26" r="4" fill="var(--pritx)"/>'+
      '<rect x="44" y="38" width="70" height="40" rx="16" fill="var(--sf)" stroke="var(--ln)" stroke-width="2"/>'+
      '<rect x="60" y="52" width="38" height="5" rx="2.5" fill="var(--ln)"/><rect x="60" y="62" width="24" height="5" rx="2.5" fill="var(--ln)"/></svg>';
    c.append(el('h2',0,'No chats yet'),el('p',0,'Messages travel between Johnify boards by radio. No internet needed.'));
    const ol=el('ol','steps');
    ['Tap Create chat to get a 10-character code.','Give the code to the other person.','They tap Join and type it in.']
      .forEach((x,i)=>{const li=el('li');li.append(el('b',0,String(i+1)),el('span',0,x));ol.append(li)});
    c.append(ol);scr.append(c);v.append(scr);return}
  const list=el('div','list');
  for(const code of S.rooms.slice().reverse()){
    const L=S.msgs[code]||[],last=L[L.length-1],r=el('div','row');
    const mid=el('div','rt');
    mid.append(el('div','rcode',code),el('div','rprev',last?(last.mine?'You: ':'')+last.text:'No messages yet'));
    const side=el('div','rside');
    if(last)side.append(el('span','rtime',fmtTime(last.t)));
    if(S.unread[code])side.append(el('span','badge',S.unread[code]));
    r.append(avatar(code),mid,side);
    r.onclick=()=>openRoom(code);list.append(r)}
  scr.append(list);v.append(scr)}

function drawChat(v){
  const scr=el('div','scr'),head=el('div','chead');
  const mid=el('button','cmid');
  const sub=el('span','csub');sub.innerHTML=svg('lock',12)+'<span>Encrypted \u00b7 tap to copy code</span>';
  mid.append(el('span','ccode',cur),sub);mid.onclick=()=>copy(cur);
  const lv=el('button','btn ghost','Leave');
  lv.onclick=()=>{if(confirm('Remove this chat from this phone?')){
    S.rooms=S.rooms.filter(x=>x!==cur);delete S.msgs[cur];delete S.unread[cur];save();goList()}};
  head.append(backBtn(),avatar(cur,'sm'),mid,lv);

  const m=el('div','msgs');m.id='msgs';
  const f=el('div','comp'),t=el('textarea');t.id='txt';t.rows=1;t.placeholder='Message';
  t.oninput=()=>{t.style.height='auto';t.style.height=Math.min(t.scrollHeight,120)+'px'};
  t.onkeydown=e=>{if(e.key==='Enter'&&!e.shiftKey){e.preventDefault();send()}};
  const s=el('button','send');s.innerHTML=svg('up',22);s.setAttribute('aria-label','Send');s.onclick=send;
  f.append(t,s);scr.append(head,m,f);v.append(scr);drawMsgs(true)}

function drawMsgs(force){
  const m=$('#msgs');if(!m)return;
  const stick=force||m.scrollHeight-m.scrollTop-m.clientHeight<90;
  m.innerHTML='';
  const L=S.msgs[cur]||[];
  if(!L.length)m.append(el('div','hint','No messages yet. Say hello.'));
  for(const x of L){
    const b=el('div','b '+(x.mine?'me':'them'));
    if(!x.mine){const w=el('div','who',x.name||'?');w.style.color=colorOf(x.dev||'');b.append(w)}
    b.append(el('div','tx',x.text),el('div','tm',fmtTime(x.t)));
    m.append(b)}
  if(stick)m.scrollTop=m.scrollHeight}

function drawSettings(v){
  const scr=el('div','scr'),head=el('div','chead');
  const ttl=el('div','cmid');ttl.append(el('span','ccode','Settings'));ttl.firstChild.style.letterSpacing='0';
  head.append(backBtn(),ttl);
  const page=el('div','page');

  const group=(label,rows)=>{page.append(el('div','lab',label));const g=el('div','grp');rows.forEach(r=>g.append(r));page.append(g)};
  const item=(title,sub)=>{const i=el('div','item'),t=el('div','itx');t.append(el('div','it',title));if(sub)t.append(el('div','is',sub));i.append(t);return i};

  const nm=item('Display name');
  const inp=el('input','field');inp.placeholder='Your name';inp.maxLength=12;inp.value=S.name;
  inp.oninput=()=>{S.name=inp.value.trim();save()};
  nm.append(inp);
  const id=item('Device ID');id.append(el('div','iv',S.dev||'-'));
  group('Profile',[nm,id]);

  const rp=item('Repeater mode','Re-sends every message this board hears from other boards, for any chat. Use it to bridge boards that are too far apart to hear each other. A repeater cannot read the messages it passes on.');
  const sw=el('label','switch'),cb=el('input');cb.type='checkbox';
  sw.append(cb,el('span','knob'));rp.append(sw);
  cb.onchange=async()=>{
    try{await api('/api/repeater',{on:cb.checked?1:0},true);toast(cb.checked?'Repeater on':'Repeater off')}
    catch(e){cb.checked=!cb.checked;toast('Could not change the setting')}};
  group('Network',[rp]);

  const stat=el('div','grp');page.append(el('div','lab','This board'),stat);
  scr.append(head,page);v.append(scr);

  const update=async()=>{
    try{
      const s=await api('/api/status');cb.checked=!!s.repeater;stat.innerHTML='';
      [['Wi-Fi network',s.ssid],['Phones connected',s.clients],['Chats stored',s.rooms],
       ['Packets heard',s.rx],['Packets sent',s.tx],['Relayed for others',s.relayed],['Uptime',fmtUp(s.up)]]
        .forEach(([k,x])=>{const i=item(k);i.append(el('div','iv',String(x)));stat.append(i)})
    }catch(e){}};
  update();stT=setInterval(update,2500)}

$('#bCreate').innerHTML=svg('plus',16)+'<span>Create chat</span>';
$('#bSet').innerHTML=svg('sliders',22);
$('#bSet').onclick=()=>{if(view==='settings')goList();else{view='settings';render()}};
$('#bCreate').onclick=async()=>{const c=await joinRoom(gen());if(c){openRoom(c);toast('Chat created. Share the code.')}};
$('#bJoin').onclick=()=>{$('#modal').hidden=false;$('#jc').value='';$('#jc').focus()};
$('#jx').onclick=()=>{$('#modal').hidden=true};
$('#jg').onclick=async()=>{const c=await joinRoom($('#jc').value);if(c){$('#modal').hidden=true;openRoom(c)}};
$('#jc').onkeydown=e=>{if(e.key==='Enter')$('#jg').click()};
document.addEventListener('visibilitychange',()=>{if(!document.hidden)poll()});

render();hello().catch(()=>{});poll();setInterval(poll,1500);
</script></body></html>)HTML";

void blinkLed() {
#ifdef LED_BUILTIN
  digitalWrite(LED_BUILTIN, LOW);
  ledOffAt = millis() + 60;
#endif
}

bool normRoomC(const char *in, size_t n, char *out) {
  while (n && isspace((unsigned char)*in)) { in++; n--; }
  while (n && isspace((unsigned char)in[n - 1])) n--;
  if (n != ROOM_LEN) return false;
  for (size_t i = 0; i < n; i++) {
    char c = (char)toupper((unsigned char)in[i]);
    if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) return false;
    out[i] = c;
  }
  out[n] = 0;
  return true;
}

bool normRoom(const String &s, char *out) { return normRoomC(s.c_str(), s.length(), out); }

bool parseHex8(const String &s, uint32_t &v) {
  if (s.length() != 8) return false;
  for (int i = 0; i < 8; i++) if (!isxdigit((unsigned char)s[i])) return false;
  v = (uint32_t)strtoul(s.c_str(), NULL, 16);
  return true;
}

size_t copyUtf8(const String &s, char *dst, size_t maxB) {
  size_t n = s.length();
  if (n > maxB) {
    n = maxB;
    while (n > 0 && (((uint8_t)s[n]) & 0xC0) == 0x80) n--;
  }
  memcpy(dst, s.c_str(), n);
  return n;
}

void outPut(Out &o, const char *s) {
  size_t l = strlen(s);
  if (o.n + l + 1 > o.cap) return;
  memcpy(o.b + o.n, s, l);
  o.n += l;
  o.b[o.n] = 0;
}

void outF(Out &o, const char *fmt, ...) {
  if (o.n + 1 >= o.cap) return;
  va_list ap;
  va_start(ap, fmt);
  int w = vsnprintf(o.b + o.n, o.cap - o.n, fmt, ap);
  va_end(ap);
  if (w < 0) return;
  if ((size_t)w >= o.cap - o.n) o.n = o.cap - 1; else o.n += (size_t)w;
}

void outJson(Out &o, const char *s) {
  outPut(o, "\"");
  for (; *s; s++) {
    unsigned char c = (unsigned char)*s;
    if (c == '"' || c == '\\') { char t[3] = {'\\', (char)c, 0}; outPut(o, t); }
    else if (c < 0x20) { char t[8]; snprintf(t, sizeof t, "\\u%04x", c); outPut(o, t); }
    else { char t[2] = {(char)c, 0}; outPut(o, t); }
  }
  outPut(o, "\"");
}

bool seenHas(uint64_t k) { for (int i = 0; i < SEEN_SIZE; i++) if (seenKeys[i] == k) return true; return false; }
void seenAdd(uint64_t k) { seenKeys[seenPos] = k; seenPos = (seenPos + 1) % SEEN_SIZE; }
uint64_t pktKey(uint32_t dev, uint32_t msg) { return ((uint64_t)dev << 32) | msg; }

void deriveRoom(Room &r) {
  uint8_t h[32], buf[48];
  size_t cl = strlen(r.code);
  uint8_t seed[32];
  size_t sl = snprintf((char *)seed, sizeof seed, "johnify-v1|%s", r.code);
  mbedtls_sha256(seed, sl, h, 0);
  for (int i = 0; i < 5000; i++) {
    memcpy(buf, h, 32); memcpy(buf + 32, r.code, cl);
    mbedtls_sha256(buf, 32 + cl, h, 0);
  }
  memcpy(r.key, h, 32);
  memcpy(buf, h, 32); memcpy(buf + 32, "gid", 3);
  mbedtls_sha256(buf, 35, h, 0);
  memcpy(r.gid, h, 8);
}

void makeAad(const Pkt &p, uint8_t *a) {
  uint32_t m = p.msgId, d = p.devId;
  memcpy(a, p.gid, 8); memcpy(a + 8, &m, 4); memcpy(a + 12, &d, 4);
}

void saveRooms() {
  String s;
  for (int i = 0; i < MAX_ROOMS; i++) if (rooms[i].used) { if (s.length()) s += ','; s += rooms[i].code; }
  if (prefs.getString("rooms", "") != s) prefs.putString("rooms", s);
}

void loadRooms() {
  String s = prefs.getString("rooms", "");
  const char *p = s.c_str();
  while (*p) {
    const char *e = strchr(p, ',');
    size_t n = e ? (size_t)(e - p) : strlen(p);
    char tmp[ROOM_LEN + 1];
    if (normRoomC(p, n, tmp)) {
      for (int i = 0; i < MAX_ROOMS; i++) if (!rooms[i].used) {
        rooms[i].used = true; strcpy(rooms[i].code, tmp); rooms[i].lastUse = millis();
        deriveRoom(rooms[i]);
        break;
      }
    }
    if (!e) break;
    p = e + 1;
  }
}

int findRoom(const char *code) {
  for (int i = 0; i < MAX_ROOMS; i++) if (rooms[i].used && strcmp(rooms[i].code, code) == 0) return i;
  return -1;
}

int ensureRoom(const char *code, bool evict) {
  int i = findRoom(code);
  if (i >= 0) { rooms[i].lastUse = millis(); return i; }
  int slot = -1;
  for (int k = 0; k < MAX_ROOMS; k++) if (!rooms[k].used) { slot = k; break; }
  if (slot < 0) {
    if (!evict) return -1;
    for (int k = 0; k < MAX_ROOMS; k++) {
      if (slot < 0 || (millis() - rooms[k].lastUse) > (millis() - rooms[slot].lastUse)) slot = k;
    }
  }
  rooms[slot].used = true;
  strcpy(rooms[slot].code, code);
  rooms[slot].lastUse = millis();
  deriveRoom(rooms[slot]);
  saveRooms();
  return slot;
}

void storeMsg(const char *room, const char *name, const char *text, uint32_t dev, uint32_t id) {
  Msg &m = msgs[(nextSeq - 1) % MAX_MSGS];
  m.seq = nextSeq++; m.msgId = id; m.devId = dev; m.ts = millis();
  strncpy(m.room, room, ROOM_LEN); m.room[ROOM_LEN] = 0;
  strncpy(m.name, name, 12);       m.name[12] = 0;
  strncpy(m.text, text, MAX_TEXT); m.text[MAX_TEXT] = 0;
}

void enqueueTx(const Pkt &p, uint8_t len, uint32_t delayMs) {
  for (int i = 0; i < TXQ_SIZE; i++) if (!txq[i].used) {
    txq[i].used = true; txq[i].at = millis() + delayMs; txq[i].len = len; txq[i].tries = 0;
    memcpy(&txq[i].p, &p, len);
    return;
  }
}

void processTx() {
  for (int i = 0; i < TXQ_SIZE; i++) {
    if (!txq[i].used || (int32_t)(millis() - txq[i].at) < 0) continue;
    esp_err_t e = esp_now_send(BCAST, (const uint8_t *)&txq[i].p, txq[i].len);
    if (e == ESP_OK) { txq[i].used = false; statTx++; blinkLed(); }
    else if (++txq[i].tries >= 5) txq[i].used = false;
    else txq[i].at = millis() + 10;
  }
}

#if ESP_ARDUINO_VERSION_MAJOR >= 3
void onRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len)
#else
void onRecv(const uint8_t *mac, const uint8_t *data, int len)
#endif
{
  if (len < PKT_HDR || len > (int)sizeof(Pkt)) return;
  portENTER_CRITICAL(&rxMux);
  int next = (rxHead + 1) % RXQ_SIZE;
  if (next != rxTail) {
    rxq[rxHead].len = len;
    memcpy(&rxq[rxHead].p, data, len);
    rxHead = next;
  }
  portEXIT_CRITICAL(&rxMux);
}

void handlePacket(const Pkt &p, int len) {
  if (p.magic[0] != 'J' || p.magic[1] != 'F') return;
  if (p.len > MAX_TEXT || len != PKT_HDR + 12 + p.len + 16) return;

  uint64_t key = pktKey(p.devId, p.msgId);
  if (seenHas(key)) return;
  seenAdd(key);
  statRx++; blinkLed();

  size_t plen = 12 + p.len;
  for (int i = 0; i < MAX_ROOMS; i++) {
    if (!rooms[i].used || memcmp(rooms[i].gid, p.gid, 8) != 0) continue;
    uint8_t aad[16]; makeAad(p, aad);
    uint8_t pt[12 + MAX_TEXT];
    mbedtls_gcm_context g; mbedtls_gcm_init(&g);
    int rc = mbedtls_gcm_setkey(&g, MBEDTLS_CIPHER_ID_AES, rooms[i].key, 256);
    if (!rc) rc = mbedtls_gcm_auth_decrypt(&g, plen, p.nonce, 12, aad, 16, p.body + plen, 16, p.body, pt);
    mbedtls_gcm_free(&g);
    if (rc == 0) {
      char name[13]; memcpy(name, pt, 12); name[12] = 0;
      char text[MAX_TEXT + 1]; memcpy(text, pt + 12, p.len); text[p.len] = 0;
      storeMsg(rooms[i].code, name, text, p.devId, p.msgId);
    }
    break;
  }

  if (repeaterOn && p.hops < MAX_HOPS) {
    Pkt c; memcpy(&c, &p, len); c.hops++;
    enqueueTx(c, len, 10 + (esp_random() % 50));
    statRelay++;
  }
}

void processRx() {
  while (true) {
    RxItem it;
    portENTER_CRITICAL(&rxMux);
    if (rxTail == rxHead) { portEXIT_CRITICAL(&rxMux); break; }
    it = rxq[rxTail];
    rxTail = (rxTail + 1) % RXQ_SIZE;
    portEXIT_CRITICAL(&rxMux);
    handlePacket(it.p, it.len);
  }
}

void sendJson(const char *body) {
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", body);
}

void bad(const char *m) {
  server.sendHeader("Cache-Control", "no-store");
  server.send(400, "text/plain", m);
}

void handleHello() {
  uint32_t dev;
  if (!parseHex8(server.arg("dev"), dev) || dev == 0) {
    do { dev = esp_random(); } while (dev == 0);
  }
  char out[80];
  snprintf(out, sizeof out, "{\"dev\":\"%08x\",\"ssid\":\"%s\"}", (unsigned)dev, apSsid);
  sendJson(out);
}

void handleJoin() {
  char room[ROOM_LEN + 1];
  if (!normRoom(server.arg("room"), room)) return bad("bad room");
  ensureRoom(room, true);
  sendJson("{\"ok\":true}");
}

void handleSend() {
  char room[ROOM_LEN + 1];
  if (!normRoom(server.arg("room"), room)) return bad("bad room");
  uint32_t dev;
  if (!parseHex8(server.arg("dev"), dev)) return bad("bad dev");
  String text = server.arg("text"); text.trim();
  if (!text.length()) return bad("empty");

  int ri = ensureRoom(room, true);
  String name = server.arg("name"); name.trim();
  if (!name.length()) name = "User";

  uint8_t pt[12 + MAX_TEXT]; memset(pt, 0, sizeof pt);
  copyUtf8(name, (char *)pt, 12);
  size_t tl = copyUtf8(text, (char *)pt + 12, MAX_TEXT);
  if (tl == 0) return bad("empty");

  Pkt p; memset(&p, 0, sizeof p);
  p.magic[0] = 'J'; p.magic[1] = 'F'; p.hops = 0;
  memcpy(p.gid, rooms[ri].gid, 8);
  p.msgId = esp_random(); p.devId = dev;
  esp_fill_random(p.nonce, 12);
  p.len = (uint8_t)tl;

  size_t plen = 12 + tl;
  uint8_t aad[16]; makeAad(p, aad);
  mbedtls_gcm_context g; mbedtls_gcm_init(&g);
  int rc = mbedtls_gcm_setkey(&g, MBEDTLS_CIPHER_ID_AES, rooms[ri].key, 256);
  if (!rc) rc = mbedtls_gcm_crypt_and_tag(&g, MBEDTLS_GCM_ENCRYPT, plen, p.nonce, 12, aad, 16, pt, p.body, 16, p.body + plen);
  mbedtls_gcm_free(&g);
  if (rc) return bad("crypto");

  seenAdd(pktKey(p.devId, p.msgId));
  char n[13], t[MAX_TEXT + 1];
  memcpy(n, pt, 12); n[12] = 0;
  memcpy(t, pt + 12, tl); t[tl] = 0;
  storeMsg(room, n, t, p.devId, p.msgId);

  uint8_t len = (uint8_t)(PKT_HDR + plen + 16);
  enqueueTx(p, len, 0);
  enqueueTx(p, len, 30 + (esp_random() % 40));

  char out[48];
  snprintf(out, sizeof out, "{\"ok\":true,\"i\":\"%08x\"}", (unsigned)p.msgId);
  sendJson(out);
}

void handlePoll() {
  uint32_t since = (uint32_t)server.arg("since").toInt();
  char bootHex[9]; snprintf(bootHex, sizeof bootHex, "%08x", (unsigned)bootId);
  if (server.arg("boot") != String(bootHex)) since = 0;
  if (since > nextSeq - 1) since = nextSeq - 1;

  char codes[MAX_ROOMS][ROOM_LEN + 1]; int nc = 0;
  String r = server.arg("r");
  const char *p = r.c_str();
  while (*p && nc < MAX_ROOMS) {
    const char *e = strchr(p, ',');
    size_t n = e ? (size_t)(e - p) : strlen(p);
    char tmp[ROOM_LEN + 1];
    if (normRoomC(p, n, tmp) && ensureRoom(tmp, false) >= 0) strcpy(codes[nc++], tmp);
    if (!e) break;
    p = e + 1;
  }

  uint32_t first = since + 1;
  uint32_t oldest = (nextSeq > MAX_MSGS) ? nextSeq - MAX_MSGS : 1;
  if (first < oldest) first = oldest;
  uint32_t last = first - 1;
  int count = 0;

  pollBuf[0] = 0;
  Out o = { pollBuf, sizeof pollBuf, 0 };
  outF(o, "{\"boot\":\"%s\",\"msgs\":[", bootHex);
  for (uint32_t seq = first; seq < nextSeq; seq++) {
    Msg &m = msgs[(seq - 1) % MAX_MSGS];
    bool want = false;
    for (int k = 0; k < nc; k++) if (strcmp(m.room, codes[k]) == 0) { want = true; break; }
    if (!want) { last = seq; continue; }
    if (count >= 30 || o.cap - o.n < 1400) break;
    if (count) outPut(o, ",");
    outF(o, "{\"i\":\"%08x\",\"d\":\"%08x\",\"r\":\"%s\",\"n\":", (unsigned)m.msgId, (unsigned)m.devId, m.room);
    outJson(o, m.name);
    outPut(o, ",\"x\":");
    outJson(o, m.text);
    outF(o, ",\"a\":%lu}", (unsigned long)((millis() - m.ts) / 1000));
    count++;
    last = seq;
  }
  outF(o, "],\"next\":%lu}", (unsigned long)last);
  sendJson(o.b);
}

void handleStatus() {
  int n = 0; for (int i = 0; i < MAX_ROOMS; i++) if (rooms[i].used) n++;
  char out[240];
  snprintf(out, sizeof out,
           "{\"repeater\":%s,\"ssid\":\"%s\",\"clients\":%d,\"rooms\":%d,\"rx\":%lu,\"tx\":%lu,\"relayed\":%lu,\"up\":%lu}",
           repeaterOn ? "true" : "false", apSsid, (int)WiFi.softAPgetStationNum(), n,
           (unsigned long)statRx, (unsigned long)statTx, (unsigned long)statRelay, (unsigned long)(millis() / 1000));
  sendJson(out);
}

void handleRepeater() {
  repeaterOn = server.arg("on") == "1";
  if (prefs.getBool("rep", false) != repeaterOn) prefs.putBool("rep", repeaterOn);
  sendJson(repeaterOn ? "{\"repeater\":true}" : "{\"repeater\":false}");
}

void handleRoot() { server.send_P(200, "text/html", INDEX_HTML); }

void handleNotFound() {
  IPAddress ip = WiFi.softAPIP();
  char loc[40];
  snprintf(loc, sizeof loc, "http://%u.%u.%u.%u/", ip[0], ip[1], ip[2], ip[3]);
  server.sendHeader("Location", loc, true);
  server.send(302, "text/plain", "");
}

void setup() {
  Serial.begin(115200);
#ifdef LED_BUILTIN
  pinMode(LED_BUILTIN, OUTPUT); digitalWrite(LED_BUILTIN, HIGH);
#endif
  bootId = esp_random();
  memset(rooms, 0, sizeof rooms); memset(txq, 0, sizeof txq); memset(seenKeys, 0, sizeof seenKeys);

  prefs.begin("johnify", false);
  repeaterOn = prefs.getBool("rep", false);
  loadRooms();

  WiFi.mode(WIFI_AP_STA);
  uint8_t mac[6]; WiFi.macAddress(mac);
  snprintf(apSsid, sizeof apSsid, "%s%02X%02X", AP_SSID_PREFIX, mac[4], mac[5]);
  WiFi.softAP(apSsid, strlen(AP_PASSWORD) ? AP_PASSWORD : NULL, WIFI_CHANNEL, 0, 8);
  WiFi.setSleep(false);
  WiFi.setTxPower(WIFI_POWER_19_5dBm);
  delay(200);

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW failed to start. Restarting.");
    delay(1000); ESP.restart();
  }
  esp_now_register_recv_cb(onRecv);

  esp_now_peer_info_t peer; memset(&peer, 0, sizeof peer);
  memcpy(peer.peer_addr, BCAST, 6);
  peer.channel = 0;
  peer.ifidx = WIFI_IF_STA;
  peer.encrypt = false;
  if (esp_now_add_peer(&peer) != ESP_OK) Serial.println("Could not add broadcast peer.");

  dns.start(53, "*", WiFi.softAPIP());
  server.on("/", handleRoot);
  server.on("/api/hello", handleHello);
  server.on("/api/join", handleJoin);
  server.on("/api/send", handleSend);
  server.on("/api/poll", handlePoll);
  server.on("/api/status", handleStatus);
  server.on("/api/repeater", handleRepeater);
  server.on("/favicon.ico", []() { server.send(204); });
  server.onNotFound(handleNotFound);
  server.begin();

  Serial.printf("Wi-Fi: %s   Open: http://%s   Repeater: %s\n",
                apSsid, WiFi.softAPIP().toString().c_str(), repeaterOn ? "on" : "off");
}

void loop() {
  dns.processNextRequest();
  server.handleClient();
  processRx();
  processTx();
#ifdef LED_BUILTIN
  if (ledOffAt && (int32_t)(millis() - ledOffAt) >= 0) { digitalWrite(LED_BUILTIN, HIGH); ledOffAt = 0; }
#endif
  delay(1);
}
