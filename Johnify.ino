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

#define VERSION   "1.1.0"
#define LOG_LINES 16
#define LOG_WIDTH 72

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
uint32_t statTxFail = 0, statBad = 0, statAuth = 0, statReinit = 0;
volatile uint32_t statRxDrop = 0;
uint32_t ledOffAt = 0;
uint32_t lastHealth = 0;
uint8_t  lowHeapCount = 0;
uint8_t  txFailStreak = 0;

char     logLines[LOG_LINES][LOG_WIDTH];
uint8_t  logHead = 0;
uint32_t logCount = 0;

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
<meta name="theme-color" content="#bf5a14">
<title>Johnify</title>
<style>
:root{--bg:#f6f2ea;--sf:#fffdf9;--sf2:#eee8dc;--tx:#241f19;--mu:#7b7266;--ln:#e5ddcf;
  --ac:#bf5a14;--actx:#fff;--out:#bf5a14;--outtx:#fff;--in:#fffdf9;--ok:#3f8a55;--bad:#c9453a}
@media(prefers-color-scheme:dark){:root{--bg:#14110e;--sf:#1c1813;--sf2:#26211b;--tx:#f2eadd;--mu:#a1968a;--ln:#2e2820;
  --ac:#f0a53a;--actx:#1c1306;--out:#f0a53a;--outtx:#1c1306;--in:#27211a;--ok:#6cc08a;--bad:#ee7466}}
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent}
html,body{margin:0;height:100%}
body{display:flex;flex-direction:column;height:100dvh;background:var(--bg);color:var(--tx);
  font:16px/1.4 system-ui,-apple-system,"Segoe UI",Roboto,sans-serif}
button,input,textarea{font:inherit;color:inherit}
svg{display:block}
header{flex:none;display:flex;align-items:center;gap:8px;padding:10px 12px;padding-top:max(10px,env(safe-area-inset-top));border-bottom:1px solid var(--ln)}
.brand{flex:1;min-width:0;display:flex;align-items:center;gap:7px;color:var(--ac)}
.name{color:var(--tx);font-weight:800;font-size:20px;letter-spacing:-.5px}
.dot{width:7px;height:7px;border-radius:50%;background:var(--mu);margin-left:2px}
.dot.on{background:var(--ok)}.dot.off{background:var(--bad)}
.btn{display:inline-flex;align-items:center;gap:6px;border:1px solid var(--ln);border-radius:10px;padding:8px 13px;
  font-weight:600;font-size:14px;background:var(--sf);cursor:pointer;white-space:nowrap}
.btn:active{opacity:.7}
.btn.pri{background:var(--ac);border-color:var(--ac);color:var(--actx)}
.btn.ghost{border:0;background:none;color:var(--bad);padding:8px 6px}
.btn.wide{width:100%;justify-content:center;padding:12px}
.icon{flex:none;width:38px;height:38px;border-radius:10px;border:0;background:none;display:grid;place-items:center;cursor:pointer}
.icon:active,.icon.on{background:var(--sf2)}
@media(max-width:370px){.btn{padding:8px 10px;font-size:13px}}
#warn{flex:none;padding:8px 14px;font-size:13px;background:rgba(201,69,58,.13);color:var(--bad);border-bottom:1px solid var(--ln)}
#warn[hidden]{display:none}
#view{flex:1;min-height:0;display:flex;flex-direction:column}
.scr{flex:1;min-height:0;display:flex;flex-direction:column}
.list{flex:1;overflow-y:auto}
.row{display:flex;align-items:center;gap:13px;padding:13px 16px;border-bottom:1px solid var(--ln);cursor:pointer}
.row:active{background:var(--sf2)}
.tile{flex:none;width:42px;height:42px;border-radius:12px;display:grid;place-items:center;font:700 14px ui-monospace,"SF Mono",Menlo,Consolas,monospace;letter-spacing:.5px}
.tile.sm{width:34px;height:34px;border-radius:10px;font-size:12px}
.rt{flex:1;min-width:0}
.rcode{font:700 15px ui-monospace,"SF Mono",Menlo,Consolas,monospace;letter-spacing:.8px}
.rprev{color:var(--mu);font-size:14px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis;margin-top:1px}
.rside{flex:none;display:flex;flex-direction:column;align-items:flex-end;gap:6px}
.rtime{font-size:12px;color:var(--mu)}
.badge{min-width:20px;height:20px;padding:0 6px;border-radius:6px;background:var(--ac);color:var(--actx);font-size:12px;font-weight:700;display:grid;place-items:center}
.empty{margin:auto;text-align:center;padding:24px;max-width:340px}
.wave{margin:0 auto}.wave path{fill:none;stroke:var(--ac);stroke-width:4;stroke-linecap:round;opacity:.25;animation:pulse 2.4s infinite}
.wave .a2{animation-delay:.3s}.wave .a3{animation-delay:.6s}
@keyframes pulse{0%,100%{opacity:.2}40%{opacity:1}}
@media(prefers-reduced-motion:reduce){.wave path{animation:none;opacity:.7}}
.empty h2{margin:14px 0 6px;font-size:23px;letter-spacing:-.4px}
.empty p{margin:0;color:var(--mu)}
.steps{margin:20px 0 0;padding:0;list-style:none;text-align:left}
.steps li{display:flex;gap:12px;align-items:flex-start;margin-bottom:12px}
.steps b{flex:none;width:24px;height:24px;border-radius:7px;background:var(--sf2);color:var(--ac);display:grid;place-items:center;font-size:13px}
.chead{flex:none;display:flex;align-items:center;gap:8px;padding:7px 10px;border-bottom:1px solid var(--ln)}
.cmid{flex:1;min-width:0;display:flex;flex-direction:column;align-items:flex-start;background:none;border:0;padding:0;cursor:pointer;text-align:left}
.ccode{font:700 15px ui-monospace,"SF Mono",Menlo,Consolas,monospace;letter-spacing:.8px}
.csub{display:flex;align-items:center;gap:4px;font-size:12px;color:var(--ok)}
.msgs{flex:1;min-height:0;overflow-y:auto;display:flex;flex-direction:column;gap:7px;padding:14px 12px}
.hint{margin:auto;color:var(--mu);font-size:14px}
.b{max-width:80%;padding:8px 12px 6px;border-radius:16px;word-break:break-word}
.b.g{margin-top:-4px}
.b.me{align-self:flex-end;background:var(--out);color:var(--outtx);border-bottom-right-radius:4px}
.b.them{align-self:flex-start;background:var(--in);border:1px solid var(--ln);border-bottom-left-radius:4px}
.who{font-size:12px;font-weight:700;margin-bottom:1px}
.tx{white-space:pre-wrap}
.tm{font-size:11px;opacity:.6;text-align:right;margin-top:1px}
.comp{flex:none;display:flex;align-items:flex-end;gap:8px;border-top:1px solid var(--ln);padding:8px 10px;padding-bottom:max(8px,env(safe-area-inset-bottom))}
.inbox{flex:1;position:relative;background:var(--sf2);border-radius:14px}
textarea{display:block;width:100%;min-height:42px;max-height:120px;resize:none;border:0;outline:none;background:none;padding:10px 14px;line-height:1.35}
.cnt{position:absolute;right:10px;top:-18px;font-size:11px;color:var(--mu)}.cnt.over{color:var(--bad);font-weight:700}
.send{flex:none;width:42px;height:42px;border-radius:12px;border:0;background:var(--ac);color:var(--actx);display:grid;place-items:center;cursor:pointer}
.send:active{opacity:.7}
.page{flex:1;overflow-y:auto;padding:2px 12px 28px}
.lab{margin:20px 4px 7px;font-size:12px;font-weight:700;letter-spacing:.7px;text-transform:uppercase;color:var(--mu)}
.grp{background:var(--sf);border:1px solid var(--ln);border-radius:14px;overflow:hidden}
.item{display:flex;align-items:center;gap:12px;padding:12px 14px;border-top:1px solid var(--ln)}
.item:first-child{border-top:0}
.itx{flex:1;min-width:0}.it{font-weight:600}.is{font-size:13px;color:var(--mu);margin-top:2px}
.iv{color:var(--mu);font-size:14px;text-align:right;word-break:break-all}.iv.warnv{color:var(--bad);font-weight:700}
.field{flex:1;min-width:0;border:0;outline:none;background:none;text-align:right;font-size:16px}
.switch{position:relative;flex:none;width:48px;height:29px}
.switch input{position:absolute;inset:0;opacity:0;margin:0;cursor:pointer;z-index:1}
.knob{position:absolute;inset:0;border-radius:99px;background:var(--ln);transition:background .2s}
.knob:after{content:"";position:absolute;top:3px;left:3px;width:23px;height:23px;border-radius:50%;background:#fff;box-shadow:0 1px 3px rgba(0,0,0,.25);transition:transform .2s}
.switch input:checked+.knob{background:var(--ac)}.switch input:checked+.knob:after{transform:translateX(19px)}
.rep{width:100%;height:170px;margin-top:10px;border:1px solid var(--ln);border-radius:10px;background:var(--sf2);padding:10px;font:12px/1.4 ui-monospace,Menlo,Consolas,monospace;resize:vertical}
#modal{position:fixed;inset:0;background:rgba(10,8,5,.55);display:flex;align-items:center;justify-content:center;padding:20px}
#modal[hidden]{display:none}
.box{width:100%;max-width:340px;background:var(--sf);border:1px solid var(--ln);border-radius:16px;padding:20px;box-shadow:0 12px 40px rgba(0,0,0,.3)}
.box h3{margin:0 0 4px;font-size:20px}.box p{margin:0;color:var(--mu);font-size:14px}
.box input{width:100%;margin:14px 0;padding:13px;border:1.5px solid var(--ln);border-radius:10px;background:var(--bg);text-align:center;
  font:700 20px ui-monospace,"SF Mono",Menlo,Consolas,monospace;letter-spacing:2px;text-transform:uppercase;outline:none}
.box input:focus{border-color:var(--ac)}
.acts{display:flex;gap:8px}.acts .btn{flex:1;justify-content:center;padding:12px}
#toast{position:fixed;left:50%;bottom:92px;transform:translateX(-50%);background:var(--tx);color:var(--bg);padding:10px 16px;border-radius:10px;
  font-size:14px;opacity:0;pointer-events:none;transition:opacity .2s;max-width:90%;text-align:center}
#toast.on{opacity:1}
</style></head><body>
<header>
 <div class="brand"><svg width="24" height="24" viewBox="0 0 24 24" fill="currentColor" aria-hidden="true"><rect x="3" y="14" width="4" height="7" rx="1.5"/><rect x="10" y="9" width="4" height="12" rx="1.5"/><rect x="17" y="3" width="4" height="18" rx="1.5"/></svg><span class="name">Johnify</span><span id="dot" class="dot"></span></div>
 <button id="bCreate" class="btn pri"></button>
 <button id="bJoin" class="btn">Join</button>
 <button id="bSet" class="icon" aria-label="Settings"></button>
</header>
<div id="warn" hidden></div>
<main id="view"></main>
<div id="modal" hidden><div class="box">
 <h3>Join a chat</h3>
 <p>Enter the 10-character code you were given.</p>
 <input id="jc" maxlength="12" placeholder="CODE" autocapitalize="characters" autocomplete="off" spellcheck="false">
 <div class="acts"><button id="jx" class="btn">Cancel</button><button id="jg" class="btn pri">Join</button></div>
</div></div>
<div id="toast"></div>
<script>
const $=s=>document.querySelector(s);
const el=(t,c,x)=>{const e=document.createElement(t);if(c)e.className=c;if(x!=null)e.textContent=x;return e};
const bytes=s=>new TextEncoder().encode(s).length;
const fmtUp=s=>{const d=Math.floor(s/86400),h=Math.floor(s%86400/3600),m=Math.floor(s%3600/60);return(d?d+'d ':'')+h+'h '+m+'m'};
const fmtTime=t=>new Date(t).toLocaleTimeString([],{hour:'numeric',minute:'2-digit'});
const fmtCode=c=>c.slice(0,5)+' '+c.slice(5);
const ICON={
  plus:'<path d="M12 5v14M5 12h14"/>',
  back:'<path d="M15 5l-7 7 7 7"/>',
  lock:'<rect x="5" y="11" width="14" height="9" rx="2"/><path d="M8 11V8a4 4 0 0 1 8 0v3"/>',
  up:'<path d="M12 19V5M6 11l6-6 6 6"/>',
  sliders:'<path d="M4 7h9M17 7h3M4 17h3M11 17h9"/><circle cx="15" cy="7" r="2"/><circle cx="9" cy="17" r="2"/>'};
const svg=(n,s)=>'<svg viewBox="0 0 24 24" width="'+s+'" height="'+s+'" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">'+ICON[n]+'</svg>';
const DARK=matchMedia('(prefers-color-scheme:dark)');
const PAL=['#bf5a14','#3f8a55','#a97c1c','#c9453a','#3f8a96','#7d6a3f'],PALD=['#f0a53a','#6cc08a','#e0c060','#ee7466','#6ac0cc','#c9a574'];
const colorOf=s=>{let h=0;for(const c of s)h=(h*31+c.charCodeAt(0))>>>0;return(DARK.matches?PALD:PAL)[h%6]};
function tile(code,cls){const a=el('div','tile '+(cls||''),code.slice(0,2)),c=colorOf(code);a.style.color=c;a.style.background=c+'26';return a}

let S={};try{S=JSON.parse(localStorage.getItem('jf')||'{}')}catch(e){}
S.rooms=S.rooms||[];S.msgs=S.msgs||{};S.since=S.since||0;S.name=S.name||'';
S.unread=S.unread||{};S.boot=S.boot||'';S.dev=S.dev||'';S.ssid=S.ssid||'';
const save=()=>{try{localStorage.setItem('jf',JSON.stringify(S))}catch(e){}};
let cur=null,view='list',busy=false,again=false,fails=0,stT=null;

const MSG={'bad room':'That chat code is not valid.','bad dev':'Your phone ID is missing. Reload the page.','empty':'Type a message first.',
  'crypto':'The board could not encrypt that message.','radio busy':'The radio is busy. Try again in a moment.'};
const why=e=>e.net?(e.message==='timeout'?'The board is not answering.':'Cannot reach the board. Check your Wi-Fi.'):(MSG[e.message]||'Board error: '+e.message);

function toast(t){const e=$('#toast');e.textContent=t;e.classList.add('on');clearTimeout(toast.t);toast.t=setTimeout(()=>e.classList.remove('on'),2800)}
function setLink(ok){
  $('#dot').className='dot '+(ok?'on':'off');
  if(ok){fails=0;$('#warn').hidden=true;return}
  if(++fails>=2){const w=$('#warn');w.textContent='No connection to the board. Join the Wi-Fi network '+(S.ssid||'Johnify-XXXX')+' and keep this page open.';w.hidden=false}}

async function api(p,o,post){
  const q=new URLSearchParams(o||{}),c=new AbortController(),t=setTimeout(()=>c.abort(),5000);
  try{
    const r=post?await fetch(p,{method:'POST',body:q,signal:c.signal}):await fetch(p+'?'+q,{signal:c.signal});
    if(!r.ok){const e=new Error((await r.text()).trim()||'HTTP '+r.status);e.board=true;throw e}
    return await r.json();
  }catch(e){
    if(e.board)throw e;
    const n=new Error(e.name==='AbortError'?'timeout':'offline');n.net=true;throw n;
  }finally{clearTimeout(t)}}

async function hello(){const h=await api('/api/hello',{dev:S.dev});S.dev=h.dev;S.ssid=h.ssid;save();return h}
function gen(){
  const A='ABCDEFGHJKMNPQRSTUVWXYZ23456789',a=new Uint8Array(10);
  if(window.crypto&&crypto.getRandomValues)crypto.getRandomValues(a);else for(let i=0;i<10;i++)a[i]=Math.random()*256;
  return[...a].map(x=>A[x%A.length]).join('')}
function copy(t){let ok=false;try{const a=document.createElement('textarea');a.value=t;document.body.appendChild(a);a.select();ok=document.execCommand('copy');a.remove()}catch(e){}
  toast(ok?'Code copied':'Code: '+t)}

async function joinRoom(raw){
  const code=(raw||'').toUpperCase().replace(/[^A-Z0-9]/g,'');
  if(code.length!==10){toast('A chat code is 10 letters and numbers.');return null}
  if(!S.rooms.includes(code)&&S.rooms.length>=12){toast('You can have up to 12 chats. Leave one first.');return null}
  try{if(!S.dev)await hello();await api('/api/join',{room:code},true)}catch(e){toast(why(e));return null}
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
  if(bytes(t)>190){toast('Too long. The limit is 190 bytes.');return}
  box.value='';box.style.height='auto';count();
  try{
    if(!S.dev)await hello();
    await api('/api/send',{room:cur,dev:S.dev,name:S.name||('User-'+S.dev.slice(0,4)),text:t},true);
    poll()}
  catch(e){box.value=t;count();toast(why(e))}}
function count(){
  const b=$('#txt'),c=$('#cnt');if(!b||!c)return;
  const left=190-bytes(b.value);
  c.hidden=left>40;c.textContent=left>=0?left+' left':(-left)+' too long';c.className='cnt'+(left<0?' over':'')}

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
    c.innerHTML='<svg class="wave" viewBox="0 0 120 76" width="132" height="84" aria-hidden="true"><circle cx="60" cy="62" r="5" fill="var(--ac)"/>'+
      '<path class="a1" d="M48.7 50.7A16 16 0 0 1 71.3 50.7"/><path class="a2" d="M38.8 40.8A30 30 0 0 1 81.2 40.8"/><path class="a3" d="M28.9 30.9A44 44 0 0 1 91.1 30.9"/></svg>';
    c.append(el('h2',0,'No chats yet'),el('p',0,'Messages travel between Johnify boards by radio. No internet needed.'));
    const ol=el('ol','steps');
    ['Tap Create chat to get a 10-character code.','Give the code to the other person.','They tap Join and type it in.']
      .forEach((x,i)=>{const li=el('li');li.append(el('b',0,String(i+1)),el('span',0,x));ol.append(li)});
    c.append(ol);scr.append(c);v.append(scr);return}
  const list=el('div','list');
  for(const code of S.rooms.slice().reverse()){
    const L=S.msgs[code]||[],last=L[L.length-1],r=el('div','row'),mid=el('div','rt');
    mid.append(el('div','rcode',fmtCode(code)),el('div','rprev',last?(last.mine?'You: ':'')+last.text:'No messages yet'));
    const side=el('div','rside');
    if(last)side.append(el('span','rtime',fmtTime(last.t)));
    if(S.unread[code])side.append(el('span','badge',S.unread[code]));
    r.append(tile(code),mid,side);r.onclick=()=>openRoom(code);list.append(r)}
  scr.append(list);v.append(scr)}

function drawChat(v){
  const scr=el('div','scr'),head=el('div','chead'),mid=el('button','cmid'),sub=el('span','csub');
  sub.innerHTML=svg('lock',12)+'<span>Encrypted \u00b7 tap to copy code</span>';
  mid.append(el('span','ccode',fmtCode(cur)),sub);mid.onclick=()=>copy(cur);
  const lv=el('button','btn ghost','Leave');
  lv.onclick=()=>{if(confirm('Remove this chat from this phone?')){
    S.rooms=S.rooms.filter(x=>x!==cur);delete S.msgs[cur];delete S.unread[cur];save();goList()}};
  head.append(backBtn(),tile(cur,'sm'),mid,lv);
  const m=el('div','msgs');m.id='msgs';
  const f=el('div','comp'),ib=el('div','inbox'),t=el('textarea'),cn=el('span','cnt');
  t.id='txt';t.rows=1;t.placeholder='Message';cn.id='cnt';cn.hidden=true;
  t.oninput=()=>{t.style.height='auto';t.style.height=Math.min(t.scrollHeight,120)+'px';count()};
  t.onkeydown=e=>{if(e.key==='Enter'&&!e.shiftKey){e.preventDefault();send()}};
  const s=el('button','send');s.innerHTML=svg('up',22);s.setAttribute('aria-label','Send');s.onclick=send;
  ib.append(cn,t);f.append(ib,s);scr.append(head,m,f);v.append(scr);drawMsgs(true)}

function drawMsgs(force){
  const m=$('#msgs');if(!m)return;
  const stick=force||m.scrollHeight-m.scrollTop-m.clientHeight<90;
  m.innerHTML='';
  const L=S.msgs[cur]||[];
  if(!L.length)m.append(el('div','hint','No messages yet. Say hello.'));
  let prev=null;
  for(const x of L){
    const grp=prev&&prev.dev===x.dev&&x.t-prev.t<120000;
    const b=el('div','b '+(x.mine?'me':'them')+(grp?' g':''));
    if(!x.mine&&!grp){const w=el('div','who',x.name||'?');w.style.color=colorOf(x.dev||'');b.append(w)}
    b.append(el('div','tx',x.text),el('div','tm',fmtTime(x.t)));
    m.append(b);prev=x}
  if(stick)m.scrollTop=m.scrollHeight}

async function report(box){
  try{
    const[s,l]=await Promise.all([api('/api/status'),api('/api/log')]);
    box.value=['Johnify bug report','Firmware: '+s.ver+'   Chip: '+s.chip+'   SDK: '+s.sdk,
      'Uptime: '+fmtUp(s.up)+'   Channel: '+s.ch+'   Max hops: '+s.hops+'   Repeater: '+(s.repeater?'on':'off'),
      'Memory: '+Math.round(s.heap/1024)+' KB free, lowest '+Math.round(s.minHeap/1024)+' KB',
      'Packets: heard '+s.rx+', sent '+s.tx+', relayed '+s.relayed,
      'Problems: dropped '+s.rxDrop+', send failures '+s.txFail+', rejected '+s.bad+', failed checks '+s.auth+', radio resets '+s.reinit,
      'Phones: '+s.clients+'   Chats on board: '+s.rooms,'Browser: '+navigator.userAgent,'','Recent events:']
      .concat(l.log.length?l.log:['(none)']).join('\n');
    box.hidden=false;box.focus();box.select();
    let ok=false;try{ok=document.execCommand('copy')}catch(e){}
    toast(ok?'Bug report copied. Paste it where you need it.':'Could not copy automatically. Select the text and copy it.')
  }catch(e){toast(why(e))}}

function drawSettings(v){
  const scr=el('div','scr'),head=el('div','chead'),ttl=el('div','cmid');
  ttl.append(el('span','it','Settings'));ttl.firstChild.style.fontSize='17px';
  head.append(backBtn(),ttl);
  const page=el('div','page');
  const group=(label,rows)=>{page.append(el('div','lab',label));const g=el('div','grp');rows.forEach(r=>g.append(r));page.append(g);return g};
  const item=(title,sub)=>{const i=el('div','item'),t=el('div','itx');t.append(el('div','it',title));if(sub)t.append(el('div','is',sub));i.append(t);return i};
  const nm=item('Display name'),inp=el('input','field');
  inp.placeholder='Your name';inp.maxLength=12;inp.value=S.name;inp.oninput=()=>{S.name=inp.value.trim();save()};nm.append(inp);
  const id=item('Device ID');id.append(el('div','iv',S.dev||'-'));
  group('Profile',[nm,id]);
  const rp=item('Repeater mode','Re-sends every message this board hears from other boards, for any chat, to bridge boards that are too far apart. A repeater cannot read the messages it passes on.');
  const sw=el('label','switch'),cb=el('input');cb.type='checkbox';sw.append(cb,el('span','knob'));rp.append(sw);
  cb.onchange=async()=>{try{await api('/api/repeater',{on:cb.checked?1:0},true);toast(cb.checked?'Repeater on':'Repeater off')}
    catch(e){cb.checked=!cb.checked;toast(why(e))}};
  group('Network',[rp]);
  const stat=group('This board',[]),diag=group('Health',[]);
  const bt=el('button','btn wide','Copy bug report');bt.style.marginTop='12px';
  const box=el('textarea','rep');box.readOnly=true;box.hidden=true;
  bt.onclick=()=>report(box);
  page.append(bt,box);
  scr.append(head,page);v.append(scr);
  const row=(g,k,x,bad)=>{const i=item(k),d=el('div','iv'+(bad?' warnv':''),String(x));i.append(d);g.append(i)};
  const update=async()=>{
    try{
      const s=await api('/api/status');cb.checked=!!s.repeater;stat.innerHTML='';diag.innerHTML='';
      row(stat,'Wi-Fi network',s.ssid);row(stat,'Phones connected',s.clients);row(stat,'Chats stored',s.rooms);
      row(stat,'Packets heard',s.rx);row(stat,'Packets sent',s.tx);row(stat,'Relayed for others',s.relayed);row(stat,'Uptime',fmtUp(s.up));
      row(diag,'Firmware',s.ver);row(diag,'Free memory',Math.round(s.heap/1024)+' KB',s.heap<30000);
      row(diag,'Packets dropped (busy)',s.rxDrop,s.rxDrop>0);row(diag,'Failed sends',s.txFail,s.txFail>0);
      row(diag,'Rejected packets',s.bad+s.auth);row(diag,'Radio restarts',s.reinit,s.reinit>0);
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

void logEvent(const char *fmt, ...) {
  char tmp[56];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(tmp, sizeof tmp, fmt, ap);
  va_end(ap);
  snprintf(logLines[logHead], LOG_WIDTH, "%lus %s", (unsigned long)(millis() / 1000), tmp);
  logHead = (logHead + 1) % LOG_LINES;
  logCount++;
  Serial.printf("[johnify] %s\n", tmp);
}

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

void cleanText(String &s, bool keepNewline) {
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if ((uint8_t)c < 0x20 && !(keepNewline && c == '\n')) s.setCharAt(i, ' ');
  }
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
  uint8_t h[32], buf[48], seed[32];
  size_t cl = strlen(r.code);
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
  if (prefs.getString("rooms", "") != s) {
    if (prefs.putString("rooms", s) == 0 && s.length() > 0) logEvent("Could not save chat list to flash");
  }
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
    logEvent("Chat list full, replacing %.5s...", rooms[slot].code);
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

bool enqueueTx(const Pkt &p, uint8_t len, uint32_t delayMs) {
  for (int i = 0; i < TXQ_SIZE; i++) if (!txq[i].used) {
    txq[i].used = true; txq[i].at = millis() + delayMs; txq[i].len = len; txq[i].tries = 0;
    memcpy(&txq[i].p, &p, len);
    return true;
  }
  return false;
}

void processTx() {
  for (int i = 0; i < TXQ_SIZE; i++) {
    if (!txq[i].used || (int32_t)(millis() - txq[i].at) < 0) continue;
    esp_err_t e = esp_now_send(BCAST, (const uint8_t *)&txq[i].p, txq[i].len);
    if (e == ESP_OK) { txq[i].used = false; statTx++; txFailStreak = 0; blinkLed(); }
    else if (++txq[i].tries >= 5) {
      txq[i].used = false; statTxFail++;
      if (txFailStreak < 255) txFailStreak++;
      logEvent("Radio send failed (error %d)", (int)e);
    }
    else txq[i].at = millis() + 10;
  }
}

#if ESP_ARDUINO_VERSION_MAJOR >= 3
void onRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len)
#else
void onRecv(const uint8_t *mac, const uint8_t *data, int len)
#endif
{
  if (len < PKT_HDR || len > (int)sizeof(Pkt)) { statBad++; return; }
  portENTER_CRITICAL(&rxMux);
  int next = (rxHead + 1) % RXQ_SIZE;
  if (next != rxTail) {
    rxq[rxHead].len = len;
    memcpy(&rxq[rxHead].p, data, len);
    rxHead = next;
  } else {
    statRxDrop++;
  }
  portEXIT_CRITICAL(&rxMux);
}

bool startNow() {
  if (esp_now_init() != ESP_OK) { logEvent("ESP-NOW failed to start"); return false; }
  esp_now_register_recv_cb(onRecv);
  esp_now_peer_info_t peer; memset(&peer, 0, sizeof peer);
  memcpy(peer.peer_addr, BCAST, 6);
  peer.channel = 0;
  peer.ifidx = WIFI_IF_STA;
  peer.encrypt = false;
  if (esp_now_add_peer(&peer) != ESP_OK) { logEvent("ESP-NOW broadcast peer failed"); return false; }
  return true;
}

void handlePacket(const Pkt &p, int len) {
  if (p.magic[0] != 'J' || p.magic[1] != 'F') { statBad++; return; }
  if (p.len > MAX_TEXT || len != PKT_HDR + 12 + p.len + 16) { statBad++; return; }

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
    } else {
      statAuth++;
      if (statAuth % 50 == 1) logEvent("Packet failed its security check (%lu)", (unsigned long)statAuth);
    }
    break;
  }

  if (repeaterOn && p.hops < MAX_HOPS) {
    Pkt c; memcpy(&c, &p, len); c.hops++;
    if (enqueueTx(c, len, 10 + (esp_random() % 50))) statRelay++;
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

void healthCheck() {
  if (millis() - lastHealth < 5000) return;
  lastHealth = millis();
  if (ESP.getFreeHeap() < 20000) {
    if (++lowHeapCount >= 6) { logEvent("Memory very low, restarting"); delay(200); ESP.restart(); }
  } else lowHeapCount = 0;
  if (txFailStreak >= 5) {
    txFailStreak = 0; statReinit++;
    logEvent("Radio keeps failing, restarting it");
    esp_now_deinit();
    delay(50);
    if (!startNow()) { logEvent("Radio restart failed, rebooting"); delay(200); ESP.restart(); }
  }
}

void sendJson(const char *body) {
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", body);
}

void fail(int code, const char *m) {
  server.sendHeader("Cache-Control", "no-store");
  server.send(code, "text/plain", m);
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
  if (!normRoom(server.arg("room"), room)) return fail(400, "bad room");
  ensureRoom(room, true);
  sendJson("{\"ok\":true}");
}

void handleSend() {
  char room[ROOM_LEN + 1];
  if (!normRoom(server.arg("room"), room)) return fail(400, "bad room");
  uint32_t dev;
  if (!parseHex8(server.arg("dev"), dev)) return fail(400, "bad dev");
  String text = server.arg("text"); cleanText(text, true); text.trim();
  if (!text.length()) return fail(400, "empty");
  String name = server.arg("name"); cleanText(name, false); name.trim();
  if (!name.length()) name = "User";

  int ri = ensureRoom(room, true);

  uint8_t pt[12 + MAX_TEXT]; memset(pt, 0, sizeof pt);
  copyUtf8(name, (char *)pt, 12);
  size_t tl = copyUtf8(text, (char *)pt + 12, MAX_TEXT);
  if (tl == 0) return fail(400, "empty");

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
  if (rc) { logEvent("Encryption failed (%d)", rc); return fail(500, "crypto"); }

  uint8_t len = (uint8_t)(PKT_HDR + plen + 16);
  if (!enqueueTx(p, len, 0)) { logEvent("Send queue full"); return fail(503, "radio busy"); }
  enqueueTx(p, len, 30 + (esp_random() % 40));

  seenAdd(pktKey(p.devId, p.msgId));
  char n[13], t[MAX_TEXT + 1];
  memcpy(n, pt, 12); n[12] = 0;
  memcpy(t, pt + 12, tl); t[tl] = 0;
  storeMsg(room, n, t, p.devId, p.msgId);

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
  char sdk[24]; strncpy(sdk, ESP.getSdkVersion(), sizeof sdk - 1); sdk[sizeof sdk - 1] = 0;
  char out[600];
  snprintf(out, sizeof out,
           "{\"ver\":\"%s\",\"chip\":\"%s\",\"sdk\":\"%s\",\"repeater\":%s,\"ssid\":\"%s\",\"ch\":%d,\"hops\":%d,"
           "\"clients\":%d,\"rooms\":%d,\"rx\":%lu,\"tx\":%lu,\"relayed\":%lu,"
           "\"rxDrop\":%lu,\"txFail\":%lu,\"bad\":%lu,\"auth\":%lu,\"reinit\":%lu,"
           "\"heap\":%lu,\"minHeap\":%lu,\"up\":%lu}",
           VERSION, ESP.getChipModel(), sdk, repeaterOn ? "true" : "false", apSsid, WIFI_CHANNEL, MAX_HOPS,
           (int)WiFi.softAPgetStationNum(), n, (unsigned long)statRx, (unsigned long)statTx, (unsigned long)statRelay,
           (unsigned long)statRxDrop, (unsigned long)statTxFail, (unsigned long)statBad, (unsigned long)statAuth,
           (unsigned long)statReinit, (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getMinFreeHeap(),
           (unsigned long)(millis() / 1000));
  sendJson(out);
}

void handleLog() {
  pollBuf[0] = 0;
  Out o = { pollBuf, sizeof pollBuf, 0 };
  outPut(o, "{\"log\":[");
  uint8_t cnt = logCount < LOG_LINES ? (uint8_t)logCount : LOG_LINES;
  for (uint8_t i = 0; i < cnt; i++) {
    uint8_t idx = (logHead + LOG_LINES - cnt + i) % LOG_LINES;
    if (i) outPut(o, ",");
    outJson(o, logLines[idx]);
  }
  outPut(o, "]}");
  sendJson(o.b);
}

void handleRepeater() {
  repeaterOn = server.arg("on") == "1";
  if (prefs.getBool("rep", false) != repeaterOn) prefs.putBool("rep", repeaterOn);
  logEvent("Repeater mode %s", repeaterOn ? "on" : "off");
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
  memset(logLines, 0, sizeof logLines);
  logEvent("Johnify %s starting", VERSION);

  if (!prefs.begin("johnify", false)) logEvent("Storage unavailable, settings will not be saved");
  repeaterOn = prefs.getBool("rep", false);
  loadRooms();

  WiFi.mode(WIFI_AP_STA);
  uint8_t mac[6]; WiFi.macAddress(mac);
  snprintf(apSsid, sizeof apSsid, "%s%02X%02X", AP_SSID_PREFIX, mac[4], mac[5]);
  const char *pw = NULL;
  if (strlen(AP_PASSWORD) >= 8) pw = AP_PASSWORD;
  else if (strlen(AP_PASSWORD) > 0) logEvent("AP_PASSWORD under 8 characters, network left open");
  if (!WiFi.softAP(apSsid, pw, WIFI_CHANNEL, 0, 8)) {
    logEvent("Wi-Fi hotspot failed to start, rebooting");
    delay(2000); ESP.restart();
  }
  WiFi.setSleep(false);
  WiFi.setTxPower(WIFI_POWER_19_5dBm);
  delay(200);

  if (!startNow()) {
    logEvent("Radio failed to start, rebooting");
    delay(2000); ESP.restart();
  }

  dns.start(53, "*", WiFi.softAPIP());
  server.on("/", handleRoot);
  server.on("/api/hello", handleHello);
  server.on("/api/join", handleJoin);
  server.on("/api/send", handleSend);
  server.on("/api/poll", handlePoll);
  server.on("/api/status", handleStatus);
  server.on("/api/log", handleLog);
  server.on("/api/repeater", handleRepeater);
  server.on("/favicon.ico", []() { server.send(204); });
  server.onNotFound(handleNotFound);
  server.begin();

  int saved = 0;
  for (int i = 0; i < MAX_ROOMS; i++) if (rooms[i].used) saved++;
  logEvent("Ready: %s, repeater %s, %d chats saved", apSsid, repeaterOn ? "on" : "off", saved);
  Serial.printf("[johnify] Open http://%s\n", WiFi.softAPIP().toString().c_str());
}

void loop() {
  dns.processNextRequest();
  server.handleClient();
  processRx();
  processTx();
  healthCheck();
#ifdef LED_BUILTIN
  if (ledOffAt && (int32_t)(millis() - ledOffAt) >= 0) { digitalWrite(LED_BUILTIN, HIGH); ledOffAt = 0; }
#endif
  delay(1);
}
