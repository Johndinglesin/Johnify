
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
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
#define MAX_ROOMS      16           // Chats a single board remembers.
#define MAX_MSGS       200          // Recent messages a board keeps in memory.
#define MAX_TEXT       190          // Longest message in bytes. ESP-NOW packets are limited to 250 bytes.
#define ROOM_LEN       10           // Length of a chat code.
#define SEEN_SIZE      256          // How many recent message IDs are remembered to spot duplicates.
#define TXQ_SIZE       16           // Packets waiting to be sent.
#define RXQ_SIZE       16           // Packets waiting to be processed.

// ---------------------------------------------------------------------------
// Packet format. This is exactly what goes over the air.
//
// Everything before "body" is readable by anyone. The name and message text
// are inside "body", encrypted. See the README for the full byte layout.
// ---------------------------------------------------------------------------
struct __attribute__((packed)) Pkt {
  uint8_t  magic[2];    // The letters 'J','F'. Lets us ignore ESP-NOW traffic from other projects.
  uint8_t  hops;        // How many repeaters have passed this packet on.
  uint8_t  gid[8];      // Group ID. A hash of the chat key, so it does not reveal the chat code.
  uint32_t msgId;       // Random number, different for every message.
  uint32_t devId;       // Random ID the board gave to the sender's phone.
  uint8_t  nonce[12];   // Random value needed by the encryption. Used once per message.
  uint8_t  len;         // Length of the message text in bytes.
  uint8_t  body[12 + MAX_TEXT + 16];  // Encrypted name (12 bytes) + text, then a 16-byte check value.
};
#define PKT_HDR 32
static_assert(offsetof(Pkt, body) == PKT_HDR, "packet header should be 32 bytes");
static_assert(sizeof(Pkt) <= 250, "ESP-NOW packets can be at most 250 bytes");

// ---------------------------------------------------------------------------
// Global state
// ---------------------------------------------------------------------------
WebServer   server(80);
DNSServer   dns;
Preferences prefs;                      // Small storage in flash. Survives power loss.

const uint8_t BCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};   // "send to everyone nearby"
char     apSsid[32];                    // The Wi-Fi name this board uses
uint32_t bootId;                        // Random number picked at each start. Phones use it to notice a reboot.
bool     repeaterOn = false;
uint32_t statRx = 0, statTx = 0, statRelay = 0;   // Counters shown on the Settings page
uint32_t ledOffAt = 0;

// A chat this board knows about. The key and group ID are worked out from the code.
struct Room {
  bool     used;
  char     code[ROOM_LEN + 1];
  uint8_t  key[32];
  uint8_t  gid[8];
  uint32_t lastUse;
} rooms[MAX_ROOMS];

// A decrypted message waiting for phones to pick it up. The list is a ring:
// when it is full, the oldest message is overwritten.
struct Msg {
  uint32_t seq;                         // Counts up forever. Phones use it to ask "what is new?"
  uint32_t msgId, devId, ts;
  char     room[ROOM_LEN + 1];
  char     name[13];
  char     text[MAX_TEXT + 1];
} msgs[MAX_MSGS];
uint32_t nextSeq = 1;

// IDs of messages we have already handled, so the same one is never shown twice.
uint64_t seenKeys[SEEN_SIZE];
int      seenPos = 0;

// Packets waiting to go out. "at" is the time (in ms) when each one should be sent.
struct TxItem { bool used; uint32_t at; uint8_t len; uint8_t tries; Pkt p; } txq[TXQ_SIZE];

// Packets that arrived from the radio. The radio callback only drops them here,
// and loop() handles them later (see onRecv).
struct RxItem { uint8_t len; Pkt p; } rxq[RXQ_SIZE];
volatile int rxHead = 0, rxTail = 0;
portMUX_TYPE rxMux = portMUX_INITIALIZER_UNLOCKED;

// ---------------------------------------------------------------------------
// The web app
// The whole page (HTML, CSS and JavaScript) is stored in flash as one string,
// and the board serves it at "/". There are no external files, fonts or
// scripts, because the phone has no internet while it is connected to the board.
// ---------------------------------------------------------------------------
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
.logo{flex:none;width:30px;height:30px;border-radius:9px;background:var(--pri);color:var(--pritx);
  display:grid;place-items:center;font-weight:800;font-size:16px}
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
@media(max-width:370px){.logo{display:none}.btn{padding:8px 11px;font-size:13px}}

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
 <div class="brand"><span class="logo">J</span><span class="name">Johnify</span><span id="dot" class="dot"></span></div>
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
// ---- small helpers -------------------------------------------------------
const $=s=>document.querySelector(s);
const el=(t,c,x)=>{const e=document.createElement(t);if(c)e.className=c;if(x!=null)e.textContent=x;return e};
const bytes=s=>new TextEncoder().encode(s).length;
const fmtUp=s=>Math.floor(s/3600)+'h '+Math.floor(s%3600/60)+'m';
const fmtTime=t=>new Date(t).toLocaleTimeString([],{hour:'numeric',minute:'2-digit'});

// Simple line icons, drawn inline so nothing has to be downloaded.
const ICON={
  plus:'<path d="M12 5v14M5 12h14"/>',
  back:'<path d="M15 5l-7 7 7 7"/>',
  lock:'<rect x="5" y="11" width="14" height="9" rx="2"/><path d="M8 11V8a4 4 0 0 1 8 0v3"/>',
  up:'<path d="M12 19V5M6 11l6-6 6 6"/>',
  sliders:'<path d="M4 7h9M17 7h3M4 17h3M11 17h9"/><circle cx="15" cy="7" r="2"/><circle cx="9" cy="17" r="2"/>'};
const svg=(n,s)=>'<svg viewBox="0 0 24 24" width="'+s+'" height="'+s+'" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">'+ICON[n]+'</svg>';

// Each chat and each sender gets a colour picked from their code or ID.
const PAL=['#1f8a70','#d0743c','#b5483e','#6e8b3d','#c29a2c','#3f7f8c'];
const colorOf=s=>{let h=0;for(const c of s)h=(h*31+c.charCodeAt(0))>>>0;return PAL[h%PAL.length]};
function avatar(code,cls){const a=el('div','av '+(cls||''),code.slice(0,2));a.style.background=colorOf(code);return a}

// ---- saved state (kept in the phone's localStorage) ------------------------
// rooms: chat codes this phone has joined   msgs: message history per chat
// since/boot: where we are in the board's message list   dev: ID the board gave us
let S={};try{S=JSON.parse(localStorage.getItem('jf')||'{}')}catch(e){}
S.rooms=S.rooms||[];S.msgs=S.msgs||{};S.since=S.since||0;S.name=S.name||'';
S.unread=S.unread||{};S.boot=S.boot||'';S.dev=S.dev||'';
const save=()=>{try{localStorage.setItem('jf',JSON.stringify(S))}catch(e){}};

let cur=null,view='list',busy=false,stT=null;

function toast(t){const e=$('#toast');e.textContent=t;e.classList.add('on');
  clearTimeout(toast.t);toast.t=setTimeout(()=>e.classList.remove('on'),2600)}
function setLink(ok){$('#dot').className='dot '+(ok?'on':'off');$('#dot').title=ok?'Connected to the board':'Not connected'}

// Talks to the board. Gives up after 5 seconds so the page never hangs.
async function api(p,o,post){
  const q=new URLSearchParams(o||{}),c=new AbortController(),t=setTimeout(()=>c.abort(),5000);
  try{
    const r=post?await fetch(p,{method:'POST',body:q,signal:c.signal}):await fetch(p+'?'+q,{signal:c.signal});
    if(!r.ok)throw new Error(await r.text());
    return await r.json();
  }finally{clearTimeout(t)}}

// Asks the board for a device ID. If we already have one it is kept.
async function hello(){const h=await api('/api/hello',{dev:S.dev});S.dev=h.dev;save();return h}

// New chat codes use letters and digits without look-alikes (no 0/O, 1/I/L).
function gen(){
  const A='ABCDEFGHJKMNPQRSTUVWXYZ23456789',a=new Uint8Array(10);
  if(window.crypto&&crypto.getRandomValues)crypto.getRandomValues(a);
  else for(let i=0;i<10;i++)a[i]=Math.random()*256;
  return[...a].map(x=>A[x%A.length]).join('')}

// The clipboard API needs https, which we don't have, so use the old way.
function copy(t){try{const a=document.createElement('textarea');a.value=t;document.body.appendChild(a);
  a.select();document.execCommand('copy');a.remove();toast('Code copied')}catch(e){toast('Code: '+t)}}

// ---- chats ---------------------------------------------------------------
async function joinRoom(raw){
  const code=(raw||'').toUpperCase().replace(/[^A-Z0-9]/g,'');
  if(code.length!==10){toast('A chat code is 10 letters and numbers');return null}
  try{if(!S.dev)await hello();await api('/api/join',{room:code},true)}
  catch(e){toast('Cannot reach the board. Check your Wi-Fi.');return null}
  if(!S.rooms.includes(code)){S.rooms.push(code);S.msgs[code]=S.msgs[code]||[];save()}
  // Fetch whatever history the board already holds for this chat.
  try{const j=await api('/api/poll',{since:0,boot:'',r:code});ingest(j.msgs)}catch(e){}
  return code}

// Adds new messages from the board to our saved history. Messages we already
// have (same id) are skipped, so asking twice is harmless.
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

// Runs every 1.5 seconds. Asks the board for anything new in our chats.
async function poll(){
  if(busy)return;busy=true;
  try{
    if(!S.dev)await hello();
    const j=await api('/api/poll',{since:S.since,boot:S.boot,r:S.rooms.join(',')});
    S.boot=j.boot;S.since=j.next;ingest(j.msgs);save();setLink(true)}
  catch(e){setLink(false)}
  busy=false}

async function send(){
  const box=$('#txt');if(!box||!cur)return;
  const t=box.value.trim();if(!t)return;
  if(bytes(t)>190){toast('Too long. Limit is 190 bytes.');return}
  box.value='';box.style.height='auto';
  try{
    if(!S.dev)await hello();
    await api('/api/send',{room:cur,dev:S.dev,name:S.name||('User-'+S.dev.slice(0,4)),text:t},true);
    busy=false;poll()}
  catch(e){box.value=t;toast('Did not send. Check your Wi-Fi.')}}

// ---- screens -------------------------------------------------------------
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
  const stick=force||m.scrollHeight-m.scrollTop-m.clientHeight<90;   // only auto-scroll if already at the bottom
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

  // Profile
  const nm=item('Display name');
  const inp=el('input','field');inp.placeholder='Your name';inp.maxLength=12;inp.value=S.name;
  inp.oninput=()=>{S.name=inp.value.trim();save()};
  nm.append(inp);
  const id=item('Device ID');id.append(el('div','iv',S.dev||'-'));
  group('Profile',[nm,id]);

  // Repeater
  const rp=item('Repeater mode','Re-sends every message this board hears from other boards, for any chat. Use it to bridge boards that are too far apart to hear each other. A repeater cannot read the messages it passes on.');
  const sw=el('label','switch'),cb=el('input');cb.type='checkbox';
  sw.append(cb,el('span','knob'));rp.append(sw);
  cb.onchange=async()=>{
    try{await api('/api/repeater',{on:cb.checked?1:0},true);toast(cb.checked?'Repeater on':'Repeater off')}
    catch(e){cb.checked=!cb.checked;toast('Could not change the setting')}};
  group('Network',[rp]);

  // Live numbers from the board
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

// ---- buttons and start-up ------------------------------------------------
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

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

// Flash the board's LED briefly. On the XIAO ESP32S3 the LED turns on when the pin is LOW.
void blinkLed() {
#ifdef LED_BUILTIN
  digitalWrite(LED_BUILTIN, LOW);
  ledOffAt = millis() + 60;
#endif
}

// Clean up a chat code and check it is valid: exactly ROOM_LEN letters or digits, upper case.
bool normRoom(const String &in, char *out) {
  String s = in; s.trim(); s.toUpperCase();
  if (s.length() != ROOM_LEN) return false;
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) return false;
  }
  strcpy(out, s.c_str());
  return true;
}

// Read an 8-digit hex string (like a device ID) into a number.
bool parseHex8(const String &s, uint32_t &v) {
  if (s.length() != 8) return false;
  for (int i = 0; i < 8; i++) if (!isxdigit((unsigned char)s[i])) return false;
  v = (uint32_t)strtoul(s.c_str(), NULL, 16);
  return true;
}

// Copy at most maxB bytes of text. If that would cut a multi-byte character
// (an emoji or accented letter) in half, stop before it.
size_t copyUtf8(const String &s, char *dst, size_t maxB) {
  size_t n = s.length();
  if (n > maxB) {
    n = maxB;
    while (n > 0 && (((uint8_t)s[n]) & 0xC0) == 0x80) n--;
  }
  memcpy(dst, s.c_str(), n);
  return n;
}

// Append a string to a JSON document, escaping quotes, backslashes and control characters.
void jsonStr(String &o, const char *s) {
  o += '"';
  for (; *s; s++) {
    unsigned char c = *s;
    if (c == '"' || c == '\\') { o += '\\'; o += (char)c; }
    else if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
    else o += (char)c;
  }
  o += '"';
}

// Duplicate detection. A message is identified by its device ID plus message ID.
bool seenHas(uint64_t k) { for (int i = 0; i < SEEN_SIZE; i++) if (seenKeys[i] == k) return true; return false; }
void seenAdd(uint64_t k) { seenKeys[seenPos] = k; seenPos = (seenPos + 1) % SEEN_SIZE; }   // overwrites the oldest entry
uint64_t pktKey(uint32_t dev, uint32_t msg) { return ((uint64_t)dev << 32) | msg; }

// ---------------------------------------------------------------------------
// Chats and keys
// ---------------------------------------------------------------------------

// Turn a chat code into an encryption key and a group ID.
// The code is hashed with SHA-256 5000 times in a row. This makes each guess
// slower for someone trying to crack a captured message by trying many codes.
// The group ID is a second hash of the key, cut to 8 bytes. It is safe to
// broadcast because the key cannot be worked out from it.
void deriveRoom(Room &r) {
  uint8_t h[32], buf[48];
  size_t cl = strlen(r.code);
  String seed = String("johnify-v1|") + r.code;
  mbedtls_sha256((const uint8_t *)seed.c_str(), seed.length(), h, 0);
  for (int i = 0; i < 5000; i++) {
    memcpy(buf, h, 32); memcpy(buf + 32, r.code, cl);
    mbedtls_sha256(buf, 32 + cl, h, 0);
  }
  memcpy(r.key, h, 32);
  memcpy(buf, h, 32); memcpy(buf + 32, "gid", 3);
  mbedtls_sha256(buf, 35, h, 0);
  memcpy(r.gid, h, 8);
}

// The header fields that stay readable but are still protected from tampering.
// If anyone changes them, decryption fails. The hop counter is left out on
// purpose, because repeaters have to change it.
void makeAad(const Pkt &p, uint8_t *a) {
  uint32_t m = p.msgId, d = p.devId;
  memcpy(a, p.gid, 8); memcpy(a + 8, &m, 4); memcpy(a + 12, &d, 4);
}

// Save the list of chat codes to flash so they are still there after a restart.
void saveRooms() {
  String s;
  for (int i = 0; i < MAX_ROOMS; i++) if (rooms[i].used) { if (s.length()) s += ','; s += rooms[i].code; }
  prefs.putString("rooms", s);
}

void loadRooms() {
  String s = prefs.getString("rooms", "");
  int start = 0;
  while (start < (int)s.length()) {
    int c = s.indexOf(',', start); if (c < 0) c = s.length();
    char tmp[ROOM_LEN + 1];
    if (normRoom(s.substring(start, c), tmp)) {
      for (int i = 0; i < MAX_ROOMS; i++) if (!rooms[i].used) {
        rooms[i].used = true; strcpy(rooms[i].code, tmp); rooms[i].lastUse = millis();
        deriveRoom(rooms[i]);
        break;
      }
    }
    start = c + 1;
  }
}

int findRoom(const char *code) {
  for (int i = 0; i < MAX_ROOMS; i++) if (rooms[i].used && strcmp(rooms[i].code, code) == 0) return i;
  return -1;
}

// Return the slot for a chat code, adding it if the board has not seen it before.
// If every slot is taken, the chat that was used least recently is replaced.
int ensureRoom(const char *code) {
  int i = findRoom(code);
  if (i >= 0) { rooms[i].lastUse = millis(); return i; }
  int slot = -1;
  for (int k = 0; k < MAX_ROOMS; k++) {
    if (!rooms[k].used) { slot = k; break; }
    if (slot < 0 || (millis() - rooms[k].lastUse) > (millis() - rooms[slot].lastUse)) slot = k;
  }
  rooms[slot].used = true; strcpy(rooms[slot].code, code); rooms[slot].lastUse = millis();
  deriveRoom(rooms[slot]);
  saveRooms();
  return slot;
}

// Put a decrypted message in the ring so phones can fetch it.
void storeMsg(const char *room, const char *name, const char *text, uint32_t dev, uint32_t id) {
  Msg &m = msgs[(nextSeq - 1) % MAX_MSGS];
  m.seq = nextSeq++; m.msgId = id; m.devId = dev; m.ts = millis();
  strncpy(m.room, room, ROOM_LEN); m.room[ROOM_LEN] = 0;
  strncpy(m.name, name, 12);       m.name[12] = 0;
  strncpy(m.text, text, MAX_TEXT); m.text[MAX_TEXT] = 0;
}

// ---------------------------------------------------------------------------
// Sending and receiving over the radio
// ---------------------------------------------------------------------------

// Queue a packet to be sent after delayMs milliseconds.
void enqueueTx(const Pkt &p, uint8_t len, uint32_t delayMs) {
  for (int i = 0; i < TXQ_SIZE; i++) if (!txq[i].used) {
    txq[i].used = true; txq[i].at = millis() + delayMs; txq[i].len = len; txq[i].tries = 0;
    memcpy(&txq[i].p, &p, len);
    return;
  }
}

// Send every queued packet whose time has come. If the radio is busy,
// try again in 10 ms, and give up after 5 tries.
void processTx() {
  for (int i = 0; i < TXQ_SIZE; i++) {
    if (!txq[i].used || (int32_t)(millis() - txq[i].at) < 0) continue;
    esp_err_t e = esp_now_send(BCAST, (const uint8_t *)&txq[i].p, txq[i].len);
    if (e == ESP_OK) { txq[i].used = false; statTx++; blinkLed(); }
    else if (++txq[i].tries >= 5) txq[i].used = false;
    else txq[i].at = millis() + 10;
  }
}

// Called by the ESP32 whenever a packet arrives. This runs inside the Wi-Fi
// driver, so it must be quick. It only copies the packet into a queue.
// The real work happens later in processRx(). (The function signature
// changed between version 2 and version 3 of the ESP32 board package.)
#if ESP_ARDUINO_VERSION_MAJOR >= 3
void onRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len)
#else
void onRecv(const uint8_t *mac, const uint8_t *data, int len)
#endif
{
  if (len < PKT_HDR || len > (int)sizeof(Pkt)) return;
  portENTER_CRITICAL(&rxMux);
  int next = (rxHead + 1) % RXQ_SIZE;
  if (next != rxTail) {              // if the queue is full, the packet is dropped
    rxq[rxHead].len = len;
    memcpy(&rxq[rxHead].p, data, len);
    rxHead = next;
  }
  portEXIT_CRITICAL(&rxMux);
}

// Deal with one received packet.
void handlePacket(const Pkt &p, int len) {
  // Ignore anything that is not ours or is the wrong size.
  if (p.magic[0] != 'J' || p.magic[1] != 'F') return;
  if (p.len > MAX_TEXT || len != PKT_HDR + 12 + p.len + 16) return;

  // Have we handled this exact message before? Then ignore it. This is what
  // stops a message from looping forever between repeaters.
  uint64_t key = pktKey(p.devId, p.msgId);
  if (seenHas(key)) return;
  seenAdd(key);
  statRx++; blinkLed();

  // Find the chat this packet belongs to. If we know it, decrypt the message.
  // If the check value does not match, the packet was changed or forged and is dropped.
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

  // Repeater mode: send the packet on, whether or not we know the chat.
  // A repeater cannot read what it passes on. The short random delay keeps
  // several repeaters from all transmitting at the same instant.
  if (repeaterOn && p.hops < MAX_HOPS) {
    Pkt c; memcpy(&c, &p, len); c.hops++;
    enqueueTx(c, len, 10 + (esp_random() % 50));
    statRelay++;
  }
}

// Take packets out of the receive queue and handle them one at a time.
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

// ---------------------------------------------------------------------------
// Web server. The phone's web page talks to these URLs.
// ---------------------------------------------------------------------------
void noCache() { server.sendHeader("Cache-Control", "no-store"); }
void bad(const char *m) { noCache(); server.send(400, "text/plain", m); }

// /api/hello : gives the phone a random device ID the first time it connects.
// If the phone already has one, it is kept.
void handleHello() {
  noCache();
  uint32_t dev;
  if (!parseHex8(server.arg("dev"), dev) || dev == 0) {
    do { dev = esp_random(); } while (dev == 0);
  }
  char b[9]; snprintf(b, sizeof b, "%08x", (unsigned)dev);
  server.send(200, "application/json", String("{\"dev\":\"") + b + "\",\"ssid\":\"" + apSsid + "\"}");
}

// /api/join : tell the board about a chat code so it starts listening for it.
void handleJoin() {
  char room[ROOM_LEN + 1];
  if (!normRoom(server.arg("room"), room)) return bad("bad room");
  ensureRoom(room);
  noCache(); server.send(200, "application/json", "{\"ok\":true}");
}

// /api/send : encrypt a message and broadcast it.
void handleSend() {
  char room[ROOM_LEN + 1];
  if (!normRoom(server.arg("room"), room)) return bad("bad room");
  uint32_t dev;
  if (!parseHex8(server.arg("dev"), dev)) return bad("bad dev");
  String text = server.arg("text"); text.trim();
  if (!text.length()) return bad("empty");

  int ri = ensureRoom(room);
  String name = server.arg("name"); name.trim();
  if (!name.length()) name = "User";

  // The part that gets encrypted: 12 bytes of name, then the text.
  uint8_t pt[12 + MAX_TEXT]; memset(pt, 0, sizeof pt);
  copyUtf8(name, (char *)pt, 12);
  size_t tl = copyUtf8(text, (char *)pt + 12, MAX_TEXT);
  if (tl == 0) return bad("empty");

  // Fill in the packet header.
  Pkt p; memset(&p, 0, sizeof p);
  p.magic[0] = 'J'; p.magic[1] = 'F'; p.hops = 0;
  memcpy(p.gid, rooms[ri].gid, 8);
  p.msgId = esp_random(); p.devId = dev;
  esp_fill_random(p.nonce, 12);
  p.len = (uint8_t)tl;

  // Encrypt with AES-256-GCM. The 16-byte check value goes right after the encrypted data.
  size_t plen = 12 + tl;
  uint8_t aad[16]; makeAad(p, aad);
  mbedtls_gcm_context g; mbedtls_gcm_init(&g);
  int rc = mbedtls_gcm_setkey(&g, MBEDTLS_CIPHER_ID_AES, rooms[ri].key, 256);
  if (!rc) rc = mbedtls_gcm_crypt_and_tag(&g, MBEDTLS_GCM_ENCRYPT, plen, p.nonce, 12, aad, 16, pt, p.body, 16, p.body + plen);
  mbedtls_gcm_free(&g);
  if (rc) return bad("crypto");

  // Remember our own message so we ignore it if a repeater sends it back,
  // and store it so other phones on this same board can see it.
  seenAdd(pktKey(p.devId, p.msgId));
  char n[13], t[MAX_TEXT + 1];
  memcpy(n, pt, 12); n[12] = 0;
  memcpy(t, pt + 12, tl); t[tl] = 0;
  storeMsg(room, n, t, p.devId, p.msgId);

  // Broadcasts are not acknowledged, so a lost packet would mean a lost message.
  // To make that less likely, send the packet twice. Receivers throw away the
  // second copy because they have already seen it.
  uint8_t len = PKT_HDR + plen + 16;
  enqueueTx(p, len, 0);
  enqueueTx(p, len, 30 + (esp_random() % 40));

  char b[9]; snprintf(b, sizeof b, "%08x", (unsigned)p.msgId);
  noCache(); server.send(200, "application/json", String("{\"ok\":true,\"i\":\"") + b + "\"}");
}

// /api/poll : the phone asks "anything new?". It sends the number of the last
// message it got ("since") and its list of chat codes. We answer with newer
// messages for those chats only.
void handlePoll() {
  char bootHex[9]; snprintf(bootHex, sizeof bootHex, "%08x", (unsigned)bootId);
  uint32_t since = (uint32_t)server.arg("since").toInt();
  // If the phone's boot ID is not ours, it last talked to a different board
  // (or we restarted), so its "since" number means nothing here. Start over.
  if (server.arg("boot") != String(bootHex)) since = 0;
  if (since > nextSeq - 1) since = nextSeq - 1;

  // Read the comma-separated list of chat codes.
  char codes[MAX_ROOMS][ROOM_LEN + 1]; int nc = 0;
  String r = server.arg("r"); int start = 0;
  while (start <= (int)r.length() && nc < MAX_ROOMS) {
    int c = r.indexOf(',', start); if (c < 0) c = r.length();
    char tmp[ROOM_LEN + 1];
    if (normRoom(r.substring(start, c), tmp)) { ensureRoom(tmp); strcpy(codes[nc++], tmp); }
    start = c + 1;
  }

  // Work out which messages to look at. Anything older than the ring holds is gone.
  uint32_t first = since + 1;
  uint32_t oldest = (nextSeq > MAX_MSGS) ? nextSeq - MAX_MSGS : 1;
  if (first < oldest) first = oldest;
  uint32_t last = first - 1;
  int count = 0;

  String o; o.reserve(1500);
  o += "{\"boot\":\""; o += bootHex; o += "\",\"msgs\":[";
  for (uint32_t seq = first; seq < nextSeq; seq++) {
    last = seq;
    Msg &m = msgs[(seq - 1) % MAX_MSGS];
    bool want = false;
    for (int k = 0; k < nc; k++) if (strcmp(m.room, codes[k]) == 0) { want = true; break; }
    if (!want) continue;
    char h1[9], h2[9];
    snprintf(h1, sizeof h1, "%08x", (unsigned)m.msgId);
    snprintf(h2, sizeof h2, "%08x", (unsigned)m.devId);
    if (count++) o += ',';
    o += "{\"i\":\""; o += h1; o += "\",\"d\":\""; o += h2; o += "\",\"r\":\""; o += m.room; o += "\",\"n\":";
    jsonStr(o, m.name); o += ",\"x\":"; jsonStr(o, m.text);
    o += ",\"a\":"; o += String((millis() - m.ts) / 1000); o += "}";   // "a" = age in seconds
    if (count >= 30) break;      // cap the reply size; the phone will ask again for the rest
  }
  o += "],\"next\":"; o += String(last); o += "}";
  noCache(); server.send(200, "application/json", o);
}

// /api/status : numbers shown on the Settings page.
void handleStatus() {
  int n = 0; for (int i = 0; i < MAX_ROOMS; i++) if (rooms[i].used) n++;
  String o = String("{\"repeater\":") + (repeaterOn ? "true" : "false") +
             ",\"ssid\":\"" + apSsid + "\",\"clients\":" + WiFi.softAPgetStationNum() +
             ",\"rooms\":" + n + ",\"rx\":" + statRx + ",\"tx\":" + statTx +
             ",\"relayed\":" + statRelay + ",\"up\":" + (millis() / 1000) + "}";
  noCache(); server.send(200, "application/json", o);
}

// /api/repeater : turn repeater mode on or off. The choice is saved in flash.
void handleRepeater() {
  repeaterOn = server.arg("on") == "1";
  prefs.putBool("rep", repeaterOn);
  noCache(); server.send(200, "application/json", String("{\"repeater\":") + (repeaterOn ? "true}" : "false}"));
}

void handleRoot() { server.send_P(200, "text/html", INDEX_HTML); }

// Any other address redirects to the app. Combined with the DNS server in
// setup(), this makes phones show their "sign in to network" prompt and lets
// you open the app by typing any web address.
void handleNotFound() {
  server.sendHeader("Location", String("http://") + WiFi.softAPIP().toString() + "/", true);
  server.send(302, "text/plain", "");
}

// ---------------------------------------------------------------------------
// Start-up and main loop
// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
#ifdef LED_BUILTIN
  pinMode(LED_BUILTIN, OUTPUT); digitalWrite(LED_BUILTIN, HIGH);   // HIGH = off on this board
#endif
  bootId = esp_random();
  memset(rooms, 0, sizeof rooms); memset(txq, 0, sizeof txq); memset(seenKeys, 0, sizeof seenKeys);

  prefs.begin("johnify", false);
  repeaterOn = prefs.getBool("rep", false);
  loadRooms();

  // Use both Wi-Fi modes at once. The access point is for phones. The station
  // side is never connected to anything, but ESP-NOW uses it to send.
  WiFi.mode(WIFI_AP_STA);
  uint8_t mac[6]; WiFi.macAddress(mac);
  snprintf(apSsid, sizeof apSsid, "%s%02X%02X", AP_SSID_PREFIX, mac[4], mac[5]);
  WiFi.softAP(apSsid, strlen(AP_PASSWORD) ? AP_PASSWORD : NULL, WIFI_CHANNEL, 0, 8);
  WiFi.setSleep(false);                       // keep the radio awake so it hears every packet
  WiFi.setTxPower(WIFI_POWER_19_5dBm);        // maximum transmit power, for range
  delay(200);

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW failed to start. Restarting.");
    delay(1000); ESP.restart();
  }
  esp_now_register_recv_cb(onRecv);

  // Add the "everyone" address as a peer so we can broadcast to it.
  esp_now_peer_info_t peer; memset(&peer, 0, sizeof peer);
  memcpy(peer.peer_addr, BCAST, 6);
  peer.channel = 0;                           // 0 means "use the channel the radio is already on"
  peer.ifidx = WIFI_IF_STA;
  peer.encrypt = false;                       // ESP-NOW's own encryption does not work for broadcasts; we encrypt ourselves
  if (esp_now_add_peer(&peer) != ESP_OK) Serial.println("Could not add broadcast peer.");

  dns.start(53, "*", WiFi.softAPIP());        // answer every DNS lookup with the board's own address
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
