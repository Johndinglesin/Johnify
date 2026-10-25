#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <stdarg.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_system.h>
#include <mbedtls/gcm.h>
#include <mbedtls/sha256.h>
#include <freertos/semphr.h>

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
#define CON_LINES      80           // Developer console lines kept in memory while Developer mode is on.

#define VERSION   "1.4.0"
#define LOG_LINES 16
#define LOG_WIDTH 72
#define CON_WIDTH 160
#define PKT_SLOTS 12
#define DEV_LEASE_MS 8000

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

SemaphoreHandle_t stateMx = NULL;
struct Guard {
  Guard() { xSemaphoreTake(stateMx, portMAX_DELAY); }
  ~Guard() { xSemaphoreGive(stateMx); }
};

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
bool     restartPending = false;
uint32_t restartAt = 0;
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
char conBuf[9000];

enum { C_SYS, C_HTTP, C_RX, C_TX, C_CRYPTO, C_CHAT, C_WIFI };
const char *const CAT_NAMES[] = {"SYS", "HTTP", "RX", "TX", "CRYPTO", "CHAT", "WIFI"};

struct ConLine { uint32_t seq; uint32_t ms; uint8_t cat; int8_t pk; char text[CON_WIDTH]; };
struct PktRec { uint32_t seq; uint8_t len; uint8_t data[250]; };
struct ClientRec { uint32_t ip; uint32_t last; uint32_t count; };

ConLine   conLines[CON_LINES];
PktRec    pktRecs[PKT_SLOTS];
ClientRec clients[8];
uint32_t  logMs[LOG_LINES];
uint32_t  conSeq = 1;
uint8_t   pktHead = 0;
bool      devOn = false;
bool      devPolls = false;
uint32_t  devUntil = 0;
uint32_t  busyUs = 0;
uint8_t   cpuPct = 0;
uint32_t  lastSample = 0;
uint8_t   staMac[10][6];
int8_t    staRssi[10];
int       staN = 0;
int       lastStatus = 0;
size_t    lastBytes = 0;
portMUX_TYPE conMux = portMUX_INITIALIZER_UNLOCKED;

const char INDEX_HTML[] PROGMEM = R"HTML(<!DOCTYPE html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<meta name="color-scheme" content="light dark">
<meta name="theme-color" content="#12402f">
<title>Johnify</title>
<style>
:root{--paper:#efe9da;--card:#f9f5ea;--ink:#15130e;--mute:#6a6454;--rule:#d6cdb8;
  --pri:#12402f;--pritx:#f6f1e2;--acc:#e8431a;--acctx:#15130e;--acct:#b8330f;
  --serif:"Iowan Old Style","Palatino Linotype",Palatino,Georgia,serif;--sans:system-ui,-apple-system,"Segoe UI",sans-serif}
@media(prefers-color-scheme:dark){:root{--paper:#0f110d;--card:#171a14;--ink:#efe9d9;--mute:#9b9584;--rule:#2c2f25;
  --pri:#1d5c45;--pritx:#f4efdf;--acc:#ff5b2e;--acctx:#15130e;--acct:#ff7d55}}
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent}
html,body{margin:0;height:100%}
body{display:flex;flex-direction:column;height:100dvh;background:var(--paper);color:var(--ink);font:500 16px/1.45 var(--sans)}
button,input,textarea{font:inherit;color:inherit}
svg{display:block}
:focus-visible{outline:3px solid var(--acc);outline-offset:2px}
header{flex:none;display:flex;align-items:center;gap:8px;padding:10px 14px;padding-top:max(10px,env(safe-area-inset-top));border-bottom:2px solid var(--ink)}
.mark{flex:1;min-width:0;display:flex;align-items:center;gap:8px}
.word{font:400 30px/1 var(--serif);letter-spacing:-.01em}
.dot{width:8px;height:8px;border-radius:50%;background:var(--mute)}
.dot.on{background:#2e8b57}.dot.off{background:var(--acc)}
.btn{border:2px solid var(--ink);background:transparent;border-radius:8px;padding:9px 12px;font:700 13px/1 var(--sans);cursor:pointer;white-space:nowrap}
.btn:active{transform:translateY(1px)}
.btn.pri{background:var(--pri);border-color:var(--pri);color:var(--pritx)}
.btn.warn{background:var(--acc);border-color:var(--acc);color:var(--acctx)}
.btn.wide{width:100%;padding:14px;font-size:14px}
.icon{flex:none;width:38px;height:38px;border:2px solid var(--ink);border-radius:8px;background:none;display:grid;place-items:center;cursor:pointer}
.icon.on{background:var(--ink);color:var(--paper)}
@media(max-width:380px){.word{font-size:28px}.btn{padding:9px 9px;font-size:12px}.icon{width:34px;height:34px}}
#warn{flex:none;padding:8px 14px;background:var(--acc);color:var(--acctx);font:700 13px/1.35 var(--sans)}
#warn[hidden]{display:none}
#view{flex:1;min-height:0;display:flex;flex-direction:column}
.scr{flex:1;min-height:0;display:flex;flex-direction:column}
.list{flex:1;overflow-y:auto;display:flex;flex-direction:column;padding:16px 14px 24px}
.lhead{display:flex;align-items:flex-end;justify-content:space-between;border-bottom:2px solid var(--ink);padding-bottom:6px}
.lhead h1{margin:0;font:400 50px/.9 var(--serif);letter-spacing:-.02em}
.lhead .n{font:800 13px var(--sans);letter-spacing:.1em;color:var(--acct)}
.row{display:grid;grid-template-columns:46px 1fr auto;gap:8px;align-items:center;padding:14px 0;border-bottom:1px solid var(--rule);cursor:pointer}
.row:active{background:var(--card)}
.idx{font:400 32px/1 var(--serif);color:var(--acct)}
.rt{min-width:0}
.rcode{font:800 17px var(--sans);letter-spacing:.1em}
.rprev{color:var(--mute);font-size:14px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.row.un .rprev{color:var(--ink);font-weight:700}
.rside{text-align:right;min-width:44px}
.rtime{font:600 12px var(--sans);color:var(--mute)}
.badge{display:inline-block;margin-top:4px;min-width:22px;padding:2px 7px;border-radius:4px;background:var(--acc);color:var(--acctx);font:800 12px var(--sans);text-align:center}
.empty{margin-top:auto;padding-top:20px}
.empty h1{margin:0 0 14px;font:400 clamp(48px,16vw,76px)/.9 var(--serif);letter-spacing:-.03em}
.lead{margin:0 0 24px;color:var(--mute);max-width:30ch}
.steps{list-style:none;margin:0;padding:0;border-top:2px solid var(--ink)}
.steps li{display:grid;grid-template-columns:54px 1fr;align-items:baseline;padding:12px 0;border-bottom:1px solid var(--rule)}
.steps b{font:400 40px/1 var(--serif);color:var(--acct)}
.steps span{font-weight:700}
.chead{flex:none;display:flex;align-items:center;gap:8px;padding:8px 12px 8px 6px;background:var(--pri);color:var(--pritx)}
.chead .icon{border-color:transparent;color:var(--pritx)}
.cmid{flex:1;min-width:0;display:flex;flex-direction:column;align-items:flex-start;background:none;border:0;padding:0;text-align:left;cursor:pointer}
.clab{font:800 10px var(--sans);letter-spacing:.16em;text-transform:uppercase;opacity:.75}
.ccode{font:800 19px/1.1 var(--sans);letter-spacing:.12em}
.leave{background:none;border:0;padding:6px 4px;font:700 13px var(--sans);text-decoration:underline;text-underline-offset:3px;cursor:pointer}
.encl{flex:none;display:flex;align-items:center;gap:6px;padding:6px 14px;border-bottom:1px solid var(--rule);font:700 11px var(--sans);letter-spacing:.04em;color:var(--mute)}
.msgs{flex:1;min-height:0;overflow-y:auto;display:flex;flex-direction:column;gap:8px;padding:14px}
.hint{margin:auto;color:var(--mute);font:400 24px var(--serif)}
.b{max-width:82%;padding:9px 12px 7px;border-radius:12px;word-break:break-word}
.b.g{margin-top:-5px}
.b.me{align-self:flex-end;background:var(--pri);color:var(--pritx);border-bottom-right-radius:3px}
.b.them{align-self:flex-start;background:var(--card);border:1.5px solid var(--ink);border-bottom-left-radius:3px}
.who{margin-bottom:2px;font:800 11px var(--sans);letter-spacing:.08em;text-transform:uppercase}
.tx{white-space:pre-wrap}
.tm{margin-top:2px;font:600 11px var(--sans);opacity:.6;text-align:right}
.comp{flex:none;display:flex;align-items:flex-end;gap:8px;padding:10px 12px;padding-bottom:max(10px,env(safe-area-inset-bottom));border-top:2px solid var(--ink)}
.inbox{flex:1;position:relative}
textarea{display:block;width:100%;min-height:44px;max-height:120px;resize:none;border:2px solid var(--ink);border-radius:8px;background:var(--card);padding:10px 12px;font:500 16px/1.35 var(--sans)}
.cnt{position:absolute;right:2px;top:-21px;font:800 11px var(--sans);color:var(--mute)}
.cnt.over{color:var(--acct)}
.send{flex:none;width:48px;height:44px;border:2px solid var(--ink);border-radius:8px;background:var(--acc);color:var(--acctx);display:grid;place-items:center;cursor:pointer}
.send:disabled{opacity:.5}
.page{flex:1;overflow-y:auto;padding:16px 14px 30px}
.ptitle{margin:0 0 14px;padding-bottom:6px;border-bottom:2px solid var(--ink);font:400 50px/.9 var(--serif);letter-spacing:-.02em}
.bento{display:grid;grid-template-columns:1.35fr 1fr;gap:10px}
.tile{min-width:0;padding:14px;border:2px solid var(--ink);border-radius:10px;background:var(--card)}
.tile.dark{background:var(--pri);border-color:var(--pri);color:var(--pritx)}
.tile.wide{grid-column:1/-1}
.tile.tall{grid-row:span 2;display:flex;flex-direction:column;justify-content:space-between}
.tile h3{margin:0 0 6px;font:400 28px/1 var(--serif)}
.tile p{margin:0 0 12px;font-size:14px;opacity:.85}
.tl{font:800 10px var(--sans);letter-spacing:.14em;text-transform:uppercase;opacity:.7}
.num{font:400 38px/1 var(--serif);letter-spacing:-.02em;overflow-wrap:anywhere}
.tall .num{font-size:58px}
.kv{display:flex;justify-content:space-between;gap:12px;padding:8px 0;border-top:1px solid var(--rule);font-size:14px}
.kv span{color:var(--mute)}
.kv b{font-weight:700;text-align:right;word-break:break-all}
.kv b.warnv{color:var(--acct)}
.rrow{display:flex;align-items:flex-start;justify-content:space-between;gap:14px}
.switch{position:relative;flex:none;width:60px;height:34px}
.switch input{position:absolute;inset:0;width:100%;height:100%;margin:0;opacity:0;cursor:pointer;z-index:1}
.knob{position:absolute;inset:0;border:2px solid currentColor;border-radius:8px}
.knob:after{content:"";position:absolute;top:4px;left:4px;width:22px;height:22px;border-radius:4px;background:currentColor;transition:transform .15s}
.switch input:checked+.knob{background:var(--acc);border-color:var(--acc);color:var(--acctx)}
.switch input:checked+.knob:after{transform:translateX(26px)}
.field{width:100%;margin:2px 0 12px;padding:6px 0;border:0;border-bottom:2px solid var(--ink);border-radius:0;background:none;font:700 18px var(--sans);outline:none}
.rep{width:100%;height:170px;margin-top:12px;font:500 12px/1.4 ui-monospace,Menlo,Consolas,monospace}
@media(max-width:380px){.tile .num{font-size:34px}}
.bar{height:10px;margin-top:10px;border:2px solid currentColor;border-radius:5px;overflow:hidden}
.bar i{display:block;height:100%;width:0;background:currentColor;transition:width .3s}
.bar.hot i{background:var(--acc)}
.sub2{margin-top:8px;font-size:13px;opacity:.8}
.trow{display:flex;align-items:center;justify-content:space-between;gap:12px;padding:8px 0;border-top:1px solid var(--rule);font-size:14px}
.tools{display:flex;flex-wrap:wrap;gap:6px;margin-top:10px}
.chip{padding:6px 10px;border:2px solid var(--ink);border-radius:6px;background:none;font:800 10px var(--sans);letter-spacing:.08em;cursor:pointer}
.chip.off{opacity:.4}
.leave.on{font-weight:800}
.con{height:46dvh;min-height:260px;margin-top:10px;padding:8px 10px;overflow-y:auto;border-radius:8px;background:#12140f;color:#e8e3d3;font:500 11.5px/1.5 ui-monospace,"SF Mono",Menlo,Consolas,monospace}
.cl{padding:1px 0;white-space:pre-wrap;word-break:break-word}
.ct{margin-right:6px;opacity:.45}
.cc{display:inline-block;min-width:6ch;margin-right:6px;font-weight:800}
.c-SYS .cc{color:#9b9584}.c-HTTP .cc{color:#e8e3d3}.c-RX .cc{color:#6cc09a}.c-TX .cc{color:#ff8a63}.c-CRYPTO .cc{color:#e0c060}.c-CHAT .cc{color:#7fb0d8}.c-WIFI .cc{color:#c9a574}
.chexb{margin-left:6px;padding:0 6px;border:1px solid #6b6556;border-radius:4px;background:none;color:#e8e3d3;font:inherit;font-size:10px;cursor:pointer}
.chexv{margin:2px 0 4px 6px;padding:6px;border-left:2px solid #6b6556;color:#b8b29f;word-break:break-all}
.con.hide-SYS .c-SYS,.con.hide-HTTP .c-HTTP,.con.hide-RX .c-RX,.con.hide-TX .c-TX,.con.hide-CRYPTO .c-CRYPTO,.con.hide-CHAT .c-CHAT,.con.hide-WIFI .c-WIFI{display:none}
#modal,#cmodal{position:fixed;inset:0;display:flex;align-items:center;justify-content:center;padding:20px;background:rgba(10,9,6,.6)}
#modal[hidden],#cmodal[hidden]{display:none}
.box{width:100%;max-width:340px;padding:20px;border:2px solid var(--ink);border-radius:10px;background:var(--paper)}
.box h3{margin:0 0 4px;font:400 36px/1 var(--serif)}
.box p{margin:0;color:var(--mute);font-size:14px}
.box input{width:100%;margin:14px 0;padding:12px;border:2px solid var(--ink);border-radius:8px;background:var(--card);text-align:center;font:800 20px var(--sans);letter-spacing:.14em;text-transform:uppercase;outline:none}
.acts{display:flex;gap:8px}.acts .btn{flex:1;padding:13px}
#toast{position:fixed;left:50%;bottom:96px;transform:translateX(-50%);max-width:90%;padding:10px 16px;border-radius:6px;background:var(--ink);color:var(--paper);font:700 13px/1.35 var(--sans);text-align:center;opacity:0;pointer-events:none;transition:opacity .2s}
#toast.on{opacity:1}
.boot .rise{animation:rise .6s cubic-bezier(.2,.7,.2,1) both;animation-delay:calc(var(--i,0) * 60ms)}
@keyframes rise{from{opacity:0;transform:translateY(14px)}}
@media(prefers-reduced-motion:reduce){.boot .rise{animation:none}}
</style></head><body class="boot">
<header>
 <div class="mark"><span class="word">Johnify</span><span id="dot" class="dot"></span></div>
 <button id="bCreate" class="btn pri">Create chat</button>
 <button id="bJoin" class="btn">Join</button>
 <button id="bSet" class="icon" aria-label="Settings"></button>
</header>
<div id="warn" hidden role="alert"></div>
<main id="view"></main>
<div id="modal" hidden><div class="box" role="dialog" aria-modal="true" aria-labelledby="mt">
 <h3 id="mt">Join a chat</h3>
 <p>Type the 10-character code you were given.</p>
 <input id="jc" maxlength="12" placeholder="CODE" autocapitalize="characters" autocomplete="off" spellcheck="false" aria-label="Chat code">
 <div class="acts"><button id="jx" class="btn">Cancel</button><button id="jg" class="btn pri">Join</button></div>
</div></div>
<div id="cmodal" hidden><div class="box" role="alertdialog" aria-modal="true" aria-labelledby="ct" aria-describedby="cp">
 <h3 id="ct"></h3>
 <p id="cp"></p>
 <div class="acts" style="margin-top:16px"><button id="cn" class="btn">Cancel</button><button id="cy" class="btn warn"></button></div>
</div></div>
<div id="toast" role="status"></div>
<script>
const $=s=>document.querySelector(s);
const el=(t,c,x)=>{const e=document.createElement(t);if(c)e.className=c;if(x!=null)e.textContent=x;return e};
const bytes=s=>new TextEncoder().encode(s).length;
const fmtUp=s=>{s=Math.max(0,s|0);const d=Math.floor(s/86400),h=Math.floor(s%86400/3600),m=Math.floor(s%3600/60);return(d?d+'d ':'')+h+'h '+m+'m'};
const fmtTime=t=>{try{return new Date(t).toLocaleTimeString([],{hour:'numeric',minute:'2-digit'})}catch(e){return''}};
const fmtCode=c=>c.slice(0,5)+' '+c.slice(5);
const ROOMRE=/^[A-Z0-9]{10}$/;
const ICON={
  back:'<path d="M15 5l-7 7 7 7"/>',
  lock:'<rect x="5" y="11" width="14" height="9" rx="2"/><path d="M8 11V8a4 4 0 0 1 8 0v3"/>',
  up:'<path d="M12 19V5M6 11l6-6 6 6"/>',
  sliders:'<path d="M4 7h9M17 7h3M4 17h3M11 17h9"/><circle cx="15" cy="7" r="2"/><circle cx="9" cy="17" r="2"/>'};
const svg=(n,s)=>'<svg viewBox="0 0 24 24" width="'+s+'" height="'+s+'" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">'+ICON[n]+'</svg>';
const DARK=matchMedia('(prefers-color-scheme:dark)');
const PAL=['#b8330f','#2f6b52','#8a6a12','#3c5a7a','#6b4a2b'],PALD=['#ff7d55','#6cc09a','#e0c060','#7fb0d8','#d0a070'];
const colorOf=s=>{let h=0;for(const c of s)h=(h*31+c.charCodeAt(0))>>>0;return(DARK.matches?PALD:PAL)[h%5]};

let S={};
try{S=JSON.parse(localStorage.getItem('jf')||'{}')}catch(e){S={}}
(function(){
  if(!S||typeof S!=='object'||Array.isArray(S))S={};
  S.rooms=(Array.isArray(S.rooms)?S.rooms:[]).filter(c=>typeof c==='string'&&ROOMRE.test(c)).slice(0,12);
  if(!S.msgs||typeof S.msgs!=='object')S.msgs={};
  for(const k of Object.keys(S.msgs))if(!S.rooms.includes(k)||!Array.isArray(S.msgs[k]))delete S.msgs[k];
  if(!S.unread||typeof S.unread!=='object')S.unread={};
  S.since=Number.isInteger(S.since)&&S.since>=0?S.since:0;
  for(const k of['name','boot','dev','ssid'])if(typeof S[k]!=='string')S[k]='';
})();
let storeOK=true;
const save=()=>{try{localStorage.setItem('jf',JSON.stringify(S))}catch(e){
  if(storeOK){storeOK=false;setTimeout(()=>toast('This browser cannot save chats. History is lost when you close the page.'),0)}}};

let cur=null,view='list',busy=false,again=false,fails=0,sending=false,stT=null;
const MSG={'bad room':'That chat code is not valid.','bad dev':'Your phone ID is missing. Reload the page.','empty':'Type a message first.',
  'crypto':'The board could not encrypt that message.','radio busy':'The radio is busy. Try again in a moment.'};
const why=e=>e&&e.net?(e.message==='timeout'?'The board is not answering.':'Cannot reach the board. Check your Wi-Fi.'):((e&&MSG[e.message])||'Board error: '+((e&&e.message)||'unknown'));

function toast(t){const e=$('#toast');e.textContent=t;e.classList.add('on');clearTimeout(toast.t);toast.t=setTimeout(()=>e.classList.remove('on'),3000)}
function ask(title,text,okLabel){
  return new Promise(res=>{
    const m=$('#cmodal'),y=$('#cy'),n=$('#cn');
    $('#ct').textContent=title;$('#cp').textContent=text;y.textContent=okLabel;m.hidden=false;
    const done=v=>{m.hidden=true;y.onclick=n.onclick=m.onclick=null;res(v)};
    y.onclick=()=>done(true);n.onclick=()=>done(false);m.onclick=e=>{if(e.target===m)done(false)};
    n.focus()})}
function setLink(ok){
  $('#dot').className='dot '+(ok?'on':'off');$('#dot').title=ok?'Connected to the board':'Not connected';
  if(ok){fails=0;$('#warn').hidden=true;return}
  if(++fails>=2){const w=$('#warn');w.textContent='No connection to the board. Join the Wi-Fi network '+(S.ssid||'Johnify-XXXX')+' and keep this page open.';w.hidden=false}}

async function api(p,o,post){
  const q=new URLSearchParams(o||{}),c=new AbortController(),t=setTimeout(()=>c.abort(),5000);
  try{
    const r=post?await fetch(p,{method:'POST',body:q,signal:c.signal}):await fetch(p+'?'+q,{signal:c.signal});
    if(!r.ok){const e=new Error((await r.text()).trim().slice(0,80)||'HTTP '+r.status);e.board=true;throw e}
    try{return await r.json()}catch(e){const n=new Error('offline');n.net=true;throw n}
  }catch(e){
    if(e.board||e.net)throw e;
    const n=new Error(e.name==='AbortError'?'timeout':'offline');n.net=true;throw n;
  }finally{clearTimeout(t)}}

async function hello(){const h=await api('/api/hello',{dev:/^[0-9a-f]{8}$/.test(S.dev)?S.dev:''});S.dev=h.dev;S.ssid=h.ssid||S.ssid;save();return h}
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
  if(!Array.isArray(ms))return;
  let changed=false;
  for(const m of ms){
    if(!m||typeof m.r!=='string'||typeof m.d!=='string'||typeof m.i!=='string'||!S.rooms.includes(m.r))continue;
    const L=S.msgs[m.r]=S.msgs[m.r]||[],id=m.d+m.i;
    if(L.some(x=>x.id===id))continue;
    L.push({id,dev:m.d,name:String(m.n||''),text:String(m.x||''),t:Date.now()-(Number(m.a)||0)*1000,mine:m.d===S.dev});
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
    if(typeof j.boot!=='string'||!Number.isInteger(j.next))throw Object.assign(new Error('offline'),{net:true});
    S.boot=j.boot;S.since=j.next;ingest(j.msgs);save();setLink(true)}
  catch(e){setLink(false)}
  busy=false;
  if(again){again=false;poll()}}
async function tick(){if(!document.hidden)await poll();setTimeout(tick,fails>=3?4000:1500)}

async function send(){
  const box=$('#txt');if(!box||!cur||sending)return;
  const t=box.value.trim();if(!t)return;
  if(bytes(t)>190){toast('Too long. The limit is 190 bytes.');return}
  sending=true;const sb=$('#sendb');if(sb)sb.disabled=true;
  box.value='';box.style.height='auto';count();
  try{
    if(!S.dev)await hello();
    await api('/api/send',{room:cur,dev:S.dev,name:S.name||('User-'+S.dev.slice(0,4)),text:t},true);
    poll()}
  catch(e){box.value=t;count();toast(why(e))}
  sending=false;if(sb)sb.disabled=false}
function count(){
  const b=$('#txt'),c=$('#cnt');if(!b||!c)return;
  const left=190-bytes(b.value);
  c.hidden=left>40;c.textContent=left>=0?left+' left':(-left)+' too long';c.className='cnt'+(left<0?' over':'')}

async function leaveRoom(code){
  S.rooms=S.rooms.filter(x=>x!==code);delete S.msgs[code];delete S.unread[code];save();
  if(view==='chat'&&cur===code)nav('list');
  for(let n=0;busy&&n<20;n++)await new Promise(r=>setTimeout(r,50));
  try{await api('/api/leave',{room:code},true);toast('You left the chat.')}
  catch(e){toast('You left the chat on this phone, but the board could not be reached.')}}
async function resetBoard(){
  if(!await ask('Reset this board?','This erases every chat, message and setting on this board and on this phone, then restarts the board. It cannot be undone.','Reset board'))return;
  try{await api('/api/reset',{confirm:'RESET'},true)}catch(e){toast(why(e));return}
  S.rooms=[];S.msgs={};S.unread={};S.since=0;S.boot='';S.dev='';S.name='';save();
  toast('Board reset. It restarts in a few seconds.');nav('list')}
function nav(v,c){
  if(v===view&&c===cur)return;
  try{history.pushState({v,c:c||null},'')}catch(e){}
  view=v;cur=c||null;if(v==='chat'){S.unread[c]=0;save()}render()}
function back(){if(history.state&&history.state.v&&history.state.v!=='list')history.back();else nav('list')}
addEventListener('popstate',e=>{
  const s=e.state||{v:'list'};view=s.v||'list';cur=s.c||null;
  if(view==='chat'&&!S.rooms.includes(cur)){view='list';cur=null}
  render()});
try{history.replaceState({v:'list',c:null},'')}catch(e){}

function render(){
  stopDev();
  $('#bSet').classList.toggle('on',view==='settings');
  clearInterval(stT);
  const v=$('#view');v.innerHTML='';
  if(view==='chat'&&!S.rooms.includes(cur)){view='list';cur=null}
  if(view==='list')drawList(v);else if(view==='chat')drawChat(v);else drawSettings(v)}
function backBtn(){const b=el('button','icon');b.innerHTML=svg('back',22);b.setAttribute('aria-label','Back');b.onclick=back;return b}
const rise=(e,i)=>{e.classList.add('rise');e.style.setProperty('--i',i);return e};

function drawList(v){
  const scr=el('div','scr'),list=el('div','list');
  if(!S.rooms.length){
    const e=el('div','empty');
    e.append(rise(el('h1',0,'No chats yet.'),0),rise(el('p','lead','Johnify sends messages from board to board by radio. No internet, no router.'),1));
    const ol=el('ol','steps');
    ['Press Create chat to get a 10-character code.','Give the code to the other person.','They press Join and type it in.']
      .forEach((x,i)=>{const li=el('li');li.append(el('b',0,String(i+1)),el('span',0,x));ol.append(rise(li,i+2))});
    e.append(ol);list.append(e);scr.append(list);v.append(scr);return}
  const h=el('div','lhead');h.append(el('h1',0,'Chats'),el('span','n',String(S.rooms.length).padStart(2,'0')));
  list.append(rise(h,0));
  S.rooms.slice().reverse().forEach((code,i)=>{
    const L=S.msgs[code]||[],last=L[L.length-1],r=el('div','row'+(S.unread[code]?' un':'')),mid=el('div','rt'),side=el('div','rside');
    r.setAttribute('role','button');r.tabIndex=0;
    mid.append(el('div','rcode',fmtCode(code)),el('div','rprev',last?(last.mine?'You: ':'')+last.text:'No messages yet'));
    if(last)side.append(el('div','rtime',fmtTime(last.t)));
    if(S.unread[code])side.append(el('span','badge',S.unread[code]));
    r.append(el('div','idx',String(i+1).padStart(2,'0')),mid,side);
    r.onclick=()=>nav('chat',code);
    r.onkeydown=e=>{if(e.key==='Enter'||e.key===' '){e.preventDefault();nav('chat',code)}};
    list.append(rise(r,i+1))});
  scr.append(list);v.append(scr)}

function drawChat(v){
  const scr=el('div','scr'),head=el('div','chead'),mid=el('button','cmid');
  mid.append(el('span','clab','Chat code'),el('span','ccode',fmtCode(cur)));mid.onclick=()=>copy(cur);mid.setAttribute('aria-label','Copy chat code');
  const lv=el('button','leave','Leave');
  lv.onclick=async()=>{const code=cur;
    if(await ask('Leave this chat?','It is removed from this phone, its saved messages are deleted, and the board stops keeping it. You can join again later with the code '+fmtCode(code)+'.','Leave'))leaveRoom(code)};
  head.append(backBtn(),mid,lv);
  const enc=el('div','encl');enc.innerHTML=svg('lock',12)+'<span>END-TO-END ENCRYPTED. TAP THE CODE TO COPY IT.</span>';
  const m=el('div','msgs');m.id='msgs';
  const f=el('div','comp'),ib=el('div','inbox'),t=el('textarea'),cn=el('span','cnt');
  t.id='txt';t.rows=1;t.placeholder='Write a message';t.setAttribute('aria-label','Message');cn.id='cnt';cn.hidden=true;
  t.oninput=()=>{t.style.height='auto';t.style.height=Math.min(t.scrollHeight,120)+'px';count()};
  t.onkeydown=e=>{if(e.key==='Enter'&&!e.shiftKey){e.preventDefault();send()}};
  const s=el('button','send');s.id='sendb';s.innerHTML=svg('up',22);s.setAttribute('aria-label','Send');s.onclick=send;
  ib.append(cn,t);f.append(ib,s);scr.append(head,enc,m,f);v.append(scr);drawMsgs(true)}

function drawMsgs(force){
  const m=$('#msgs');if(!m)return;
  const stick=force||m.scrollHeight-m.scrollTop-m.clientHeight<90;
  m.innerHTML='';
  const L=S.msgs[cur]||[];
  if(!L.length)m.append(el('div','hint','Nothing here yet.'));
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

const fmtB=n=>n==null?'-':n>=1048576?(n/1048576).toFixed(1)+' MB':n>=1024?Math.round(n/1024)+' KB':n+' B';
const CATS=['SYS','HTTP','RX','TX','CRYPTO','CHAT','WIFI'];
const CON={boot:'',last:0,lines:[],hidden:{},paused:false,polls:false};
const DEV={on:false,grid:null,btn:null,t1:null,t2:null};
let conBox=null,conBusy=false;
const kvl=(k,x)=>{const d=el('div','kv');d.append(el('span',0,k),el('b',0,String(x)));return d};
const kvRows=(host,list)=>{[...host.querySelectorAll('.kv')].forEach(x=>x.remove());for(const[k,x,bad]of list){const d=kvl(k,x);if(bad)d.lastChild.className='warnv';host.append(d)}};
const hexRows=h=>{const o=[];for(let i=0;i<h.length;i+=32)o.push(h.slice(i,i+32).replace(/(..)/g,'$1 ').trim());return o.join('\n')};
const conText=l=>'['+(l.t/1000).toFixed(3)+'] '+l.c+' '+l.x+(l.h?'\n'+hexRows(l.h):'');
function conLine(l){
  const d=el('div','cl c-'+l.c);
  d.append(el('span','ct',(l.t/1000).toFixed(3)),el('span','cc',l.c),el('span','cx',l.x));
  if(l.h){const b=el('button','chexb','hex'),p=el('div','chexv');p.hidden=true;p.textContent=hexRows(l.h);b.onclick=()=>{p.hidden=!p.hidden};d.append(b,p)}
  return d}
function conPush(l){
  CON.lines.push(l);if(CON.lines.length>500)CON.lines.shift();
  if(!conBox)return;
  const stick=conBox.scrollHeight-conBox.scrollTop-conBox.clientHeight<40;
  conBox.append(conLine(l));
  while(conBox.children.length>400)conBox.removeChild(conBox.firstChild);
  if(stick)conBox.scrollTop=conBox.scrollHeight}
async function conPoll(){
  if(conBusy||!DEV.on)return;conBusy=true;
  try{
    for(let n=0;n<5&&DEV.on;n++){
      const j=await api('/api/console',{since:CON.last,boot:CON.boot});
      if(!DEV.on)break;
      if(!j.on){await api('/api/dev',{on:1},true);break}
      if(CON.boot&&j.boot!==CON.boot){CON.lines=[];if(conBox)conBox.innerHTML='';conPush({s:0,t:0,c:'SYS',x:'The board restarted. Console cleared.'})}
      CON.boot=j.boot;
      if(j.lost>0)conPush({s:0,t:0,c:'SYS',x:j.lost+' older lines were dropped because the board only keeps the most recent ones.'});
      for(const l of j.lines)conPush(l);
      if(Number.isInteger(j.next))CON.last=j.next;
      if(!j.more)break}
  }catch(e){}
  conBusy=false}
function mkSw(label,onchg){
  const r=el('div','trow'),s=el('label','switch'),c=el('input');c.type='checkbox';c.setAttribute('aria-label',label);
  s.append(c,el('span','knob'));c.onchange=()=>onchg(c.checked,c);r.append(el('span',0,label),s);return[r,c]}

function stopDev(){
  if(!DEV.on)return;
  DEV.on=false;clearInterval(DEV.t1);clearInterval(DEV.t2);
  if(DEV.grid)DEV.grid.querySelectorAll('.devt').forEach(x=>x.remove());
  if(DEV.btn){DEV.btn.textContent='Developer';DEV.btn.classList.remove('on')}
  conBox=null;CON.lines=[];CON.last=0;CON.boot='';CON.polls=false;CON.paused=false;
  DEV.grid=DEV.btn=null;
  api('/api/dev',{on:0},true).catch(()=>{})}

async function startDev(grid,btn){
  try{await api('/api/dev',{on:1},true)}catch(e){toast(why(e));return}
  DEV.on=true;DEV.grid=grid;DEV.btn=btn;btn.textContent='Developer on';btn.classList.add('on');
  CON.lines=[];CON.last=0;CON.boot='';CON.polls=false;CON.paused=false;
  const T=(cls,label)=>{const t=el('div','tile devt '+cls);t.append(el('div','tl',label));return t};
  const tTemp=T('','Chip temperature'),nTemp=el('div','num','-');tTemp.append(nTemp);
  const tCpu=T('','Johnify CPU use'),nCpu=el('div','num','-'),bCpu=el('div','bar'),fCpu=el('i');bCpu.append(fCpu);tCpu.append(nCpu,bCpu);
  const tRam=T('wide','Memory'),nRam=el('div','num','-'),bRam=el('div','bar'),fRam=el('i'),sRam=el('div','sub2');bRam.append(fRam);tRam.append(nRam,bRam,sRam);
  const tSto=T('wide','Storage'),tChip=T('wide','Chip'),tDev=T('wide','Connected devices'),tCon=T('wide','Live console');
  tCon.append(el('div','sub2','Records requests, packets, encryption and Wi-Fi activity while this section is open. It never records chat codes, keys or message text.'));
  const[rowPolls,cbPolls]=mkSw('Include polling requests',async(on,c)=>{try{await api('/api/dev',{polls:on?1:0},true);CON.polls=on}catch(e){c.checked=!on;toast(why(e))}});
  const tools=el('div','tools'),bP=el('button','chip','PAUSE'),bC=el('button','chip','CLEAR'),bY=el('button','chip','COPY');
  bP.onclick=()=>{CON.paused=!CON.paused;bP.textContent=CON.paused?'RESUME':'PAUSE';bP.classList.toggle('off',CON.paused)};
  bC.onclick=async()=>{CON.lines=[];if(conBox)conBox.innerHTML='';try{await api('/api/dev',{clear:1},true)}catch(e){toast(why(e))}};
  bY.onclick=()=>{const t=CON.lines.map(conText).join('\n');let ok=false;try{const a=document.createElement('textarea');a.value=t;document.body.appendChild(a);a.select();ok=document.execCommand('copy');a.remove()}catch(e){}toast(ok?'Console copied':'Could not copy automatically.')};
  tools.append(bP,bC,bY);
  const filt=el('div','tools');
  CATS.forEach(c=>{const b=el('button','chip',c);b.onclick=()=>{CON.hidden[c]=!CON.hidden[c];b.classList.toggle('off',!!CON.hidden[c]);if(conBox)conBox.classList.toggle('hide-'+c,!!CON.hidden[c])};if(CON.hidden[c])b.classList.add('off');filt.append(b)});
  conBox=el('div','con');conBox.setAttribute('role','log');CATS.forEach(c=>{if(CON.hidden[c])conBox.classList.add('hide-'+c)});
  tCon.append(rowPolls,tools,filt,conBox);
  [tTemp,tCpu,tRam,tSto,tChip,tDev,tCon].forEach(t=>grid.append(t));
  if(tTemp.scrollIntoView)tTemp.scrollIntoView({block:'start',behavior:'auto'});
  const perf=async()=>{
    if(!DEV.on||document.hidden)return;
    try{
      const p=await api('/api/perf');
      if(!DEV.on||p.on===false)return;
      nTemp.textContent=p.temp==null?'n/a':p.temp.toFixed(1)+'\u00b0C';
      nCpu.textContent=p.cpu+'%';fCpu.style.width=Math.min(100,p.cpu)+'%';bCpu.className='bar'+(p.cpu>80?' hot':'');
      const used=p.heapTotal-p.heapFree,pct=p.heapTotal?Math.round(used*100/p.heapTotal):0;
      nRam.textContent=pct+'% used';fRam.style.width=pct+'%';bRam.className='bar'+(pct>85?' hot':'');
      sRam.textContent=fmtB(p.heapFree)+' free of '+fmtB(p.heapTotal)+(p.heapMax!=null?' \u00b7 largest free block '+fmtB(p.heapMax):'')+(p.psramTotal?' \u00b7 PSRAM '+fmtB(p.psramFree)+' free of '+fmtB(p.psramTotal):'');
      const sr=[['Flash chip',fmtB(p.flash)]];
      if(p.sketch!=null)sr.push(['Program size',fmtB(p.sketch)],['Free for updates',fmtB(p.sketchFree)]);
      if(p.fsTotal!=null)sr.push(['Files used',fmtB(p.fsTotal-p.fsFree)+' of '+fmtB(p.fsTotal)],['Files free',fmtB(p.fsFree)]);
      kvRows(tSto,sr);
      kvRows(tChip,[['Chip',p.chip+(p.cores?' ('+p.cores+' cores)':'')],['Clock',p.mhz+' MHz'],['Last restart',p.reset],['Uptime',fmtUp(p.up)]]);
      const dr=[['Wi-Fi devices',p.sta.length]];
      p.sta.forEach(s=>dr.push([s.mac,s.rssi==null?'':s.rssi+' dBm']));
      p.clients.forEach(c=>dr.push([c.ip,c.n+' requests, '+c.age+'s ago']));
      kvRows(tDev,dr);
    }catch(e){}};
  perf();conPoll();
  DEV.t1=setInterval(perf,2000);
  DEV.t2=setInterval(()=>{if(!CON.paused&&!document.hidden)conPoll()},1000)}

function drawSettings(v){
  const scr=el('div','scr'),head=el('div','chead'),ttl=el('div','cmid');
  ttl.append(el('span','clab','Johnify'),el('span','ccode','Settings'));ttl.style.cursor='default';ttl.lastChild.style.letterSpacing='.04em';
  const dv=el('button','leave','Developer');
  head.append(backBtn(),ttl,dv);
  const page=el('div','page'),grid=el('div','bento');
  page.append(rise(el('h1','ptitle','Settings'),0),grid);

  const rep=el('div','tile dark wide'),rr=el('div','rrow'),rl=el('div');
  rl.append(el('h3',0,'Repeater'),el('p',0,'Re-sends every message this board hears from other boards, for any chat, to bridge boards too far apart to hear each other. A repeater cannot read what it passes on.'));
  const sw=el('label','switch'),cb=el('input');cb.type='checkbox';cb.setAttribute('aria-label','Repeater mode');sw.append(cb,el('span','knob'));
  cb.onchange=async()=>{try{await api('/api/repeater',{on:cb.checked?1:0},true);toast(cb.checked?'Repeater on':'Repeater off')}
    catch(e){cb.checked=!cb.checked;toast(why(e))}};
  rr.append(rl,sw);rep.append(rr);

  const stat=(cls,label)=>{const t=el('div','tile '+cls),n=el('div','num','-');t.append(el('div','tl',label),n);return[t,n]};
  const[tHeard,nHeard]=stat('tall','Packets heard'),[tSent,nSent]=stat('','Sent'),[tRel,nRel]=stat('','Relayed');

  const prof=el('div','tile wide');
  const nm=el('input','field');nm.placeholder='Your name';nm.maxLength=12;nm.value=S.name;nm.setAttribute('aria-label','Display name');
  nm.oninput=()=>{S.name=nm.value.trim();save()};
  const kvl=(k,x)=>{const d=el('div','kv');d.append(el('span',0,k),el('b',0,String(x)));return d};
  prof.append(el('div','tl','Profile'),nm,kvl('Device ID',S.dev||'-'));

  const net=el('div','tile wide'),health=el('div','tile wide');
  net.append(el('h3',0,'This board'));health.append(el('h3',0,'Health'));

  const rst=el('div','tile wide'),rb=el('button','btn wide warn','Reset board');
  rst.append(el('h3',0,'Start over'),el('p',0,'Erases every chat, message and setting on this board and on this phone, then restarts the board so it behaves like freshly uploaded firmware. Other phones keep their own copies.'),rb);
  rb.onclick=resetBoard;
  const bug=el('div','tile wide'),bt=el('button','btn wide','Copy bug report'),box=el('textarea','rep');
  box.readOnly=true;box.hidden=true;bt.onclick=()=>report(box);
  bug.append(bt,box);

  [rep,tHeard,tSent,tRel,prof,net,health,bug,rst].forEach((t,i)=>grid.append(rise(t,i+1)));
  dv.onclick=()=>{if(DEV.on)stopDev();else startDev(grid,dv)};
  scr.append(head,page);v.append(scr);

  const rows=(host,list)=>{[...host.querySelectorAll('.kv')].forEach(x=>x.remove());
    for(const[k,x,bad]of list){const d=kvl(k,x);if(bad)d.lastChild.className='warnv';host.append(d)}};
  const update=async()=>{
    try{
      const s=await api('/api/status');cb.checked=!!s.repeater;
      nHeard.textContent=s.rx;nSent.textContent=s.tx;nRel.textContent=s.relayed;
      rows(net,[['Wi-Fi network',s.ssid],['Phones connected',s.clients],['Chats stored',s.rooms],['Channel',s.ch],['Uptime',fmtUp(s.up)]]);
      rows(health,[['Firmware',s.ver],['Free memory',Math.round(s.heap/1024)+' KB',s.heap<30000],['Packets dropped (busy)',s.rxDrop,s.rxDrop>0],
        ['Failed sends',s.txFail,s.txFail>0],['Rejected packets',s.bad+s.auth],['Radio restarts',s.reinit,s.reinit>0]]);
    }catch(e){}};
  update();stT=setInterval(update,2500)}

$('#bSet').innerHTML=svg('sliders',22);
$('#bSet').onclick=()=>{if(view==='settings')back();else nav('settings')};
$('#bCreate').onclick=async()=>{const c=await joinRoom(gen());if(c){nav('chat',c);toast('Chat created. Share the code.')}};
const openJoin=()=>{$('#modal').hidden=false;$('#jc').value='';$('#jc').focus()},closeJoin=()=>{$('#modal').hidden=true};
$('#bJoin').onclick=openJoin;
$('#jx').onclick=closeJoin;
$('#modal').onclick=e=>{if(e.target===$('#modal'))closeJoin()};
$('#jg').onclick=async()=>{const c=await joinRoom($('#jc').value);if(c){closeJoin();nav('chat',c)}};
$('#jc').onkeydown=e=>{if(e.key==='Enter')$('#jg').click();if(e.key==='Escape')closeJoin()};
addEventListener('keydown',e=>{if(e.key==='Escape'&&!$('#cmodal').hidden)$('#cn').click()});
addEventListener('pagehide',()=>{if(DEV.on){try{fetch('/api/dev',{method:'POST',body:new URLSearchParams({on:0}),keepalive:true})}catch(e){}}});
document.addEventListener('visibilitychange',()=>{if(!document.hidden)poll()});
let errShown=false;
addEventListener('error',()=>{if(!errShown){errShown=true;toast('Something went wrong. Reload the page if it keeps happening.')}});
addEventListener('unhandledrejection',()=>{if(!errShown){errShown=true;toast('Something went wrong. Reload the page if it keeps happening.')}});

render();setTimeout(()=>document.body.classList.remove('boot'),1400);
hello().catch(()=>{});tick();
</script></body></html>)HTML";

void conStore(uint8_t cat, const char *text, uint32_t ms, const uint8_t *data, size_t dlen) {
  portENTER_CRITICAL(&conMux);
  uint32_t seq = conSeq++;
  ConLine &l = conLines[(seq - 1) % CON_LINES];
  l.seq = seq; l.ms = ms; l.cat = cat; l.pk = -1;
  strncpy(l.text, text, CON_WIDTH - 1); l.text[CON_WIDTH - 1] = 0;
  if (data && dlen) {
    uint8_t slot = pktHead;
    pktHead = (pktHead + 1) % PKT_SLOTS;
    pktRecs[slot].seq = seq; pktRecs[slot].len = (uint8_t)dlen; memcpy(pktRecs[slot].data, data, dlen);
    l.pk = (int8_t)slot;
  }
  portEXIT_CRITICAL(&conMux);
}

void conf(uint8_t cat, const char *fmt, ...) {
  if (!devOn) return;
  char tmp[CON_WIDTH];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(tmp, sizeof tmp, fmt, ap);
  va_end(ap);
  conStore(cat, tmp, millis(), NULL, 0);
}

void confPkt(uint8_t cat, const uint8_t *data, size_t len, const char *fmt, ...) {
  if (!devOn) return;
  char tmp[CON_WIDTH];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(tmp, sizeof tmp, fmt, ap);
  va_end(ap);
  conStore(cat, tmp, millis(), data, len);
}

void gidTag(const uint8_t *gid, char *out) { snprintf(out, 7, "%02x%02x%02x", gid[0], gid[1], gid[2]); }

void clearConsole() {
  portENTER_CRITICAL(&conMux);
  memset(conLines, 0, sizeof conLines);
  memset(pktRecs, 0, sizeof pktRecs);
  portEXIT_CRITICAL(&conMux);
}

void logEvent(const char *fmt, ...) {
  char tmp[56];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(tmp, sizeof tmp, fmt, ap);
  va_end(ap);
  uint32_t now = millis();
  logMs[logHead] = now;
  snprintf(logLines[logHead], LOG_WIDTH, "%lus %s", (unsigned long)(now / 1000), tmp);
  logHead = (logHead + 1) % LOG_LINES;
  logCount++;
  Serial.printf("[johnify] %s\n", tmp);
  if (devOn) conStore(C_SYS, tmp, now, NULL, 0);
}

void devStop() {
  devOn = false;
  devPolls = false;
  clearConsole();
  memset(clients, 0, sizeof clients);
  staN = 0; busyUs = 0; cpuPct = 0;
}

void devStart() {
  if (!devOn) {
    clearConsole();
    memset(clients, 0, sizeof clients);
    busyUs = 0; cpuPct = 0; staN = 0; lastSample = millis();
    devOn = true;
    uint8_t n = logCount < LOG_LINES ? (uint8_t)logCount : LOG_LINES;
    for (uint8_t i = 0; i < n; i++) {
      uint8_t idx = (logHead + LOG_LINES - n + i) % LOG_LINES;
      const char *sp = strchr(logLines[idx], ' ');
      conStore(C_SYS, sp ? sp + 1 : logLines[idx], logMs[idx], NULL, 0);
    }
  }
  devUntil = millis() + DEV_LEASE_MS;
}

void devCheck() {
  if (devOn && (int32_t)(millis() - devUntil) >= 0) devStop();
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
  uint32_t t0 = devOn ? millis() : 0;
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
  if (devOn) {
    char tg[7];
    gidTag(r.gid, tg);
    conf(C_CRYPTO, "derive key for chat %s took %lu ms (5000 rounds)", tg, (unsigned long)(millis() - t0));
  }
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
    char tg[7];
    gidTag(rooms[slot].gid, tg);
    logEvent("Chat list full, replacing chat %s", tg);
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
    if (e == ESP_OK) {
      txq[i].used = false; statTx++; txFailStreak = 0; blinkLed();
      if (devOn) {
        char tg[7];
        gidTag(txq[i].p.gid, tg);
        confPkt(C_TX, (const uint8_t *)&txq[i].p, txq[i].len, "TX chat %s msg %08x dev %08x hops %u len %u",
                tg, (unsigned)txq[i].p.msgId, (unsigned)txq[i].p.devId, (unsigned)txq[i].p.hops, (unsigned)txq[i].p.len);
      }
    }
    else if (++txq[i].tries >= 5) {
      txq[i].used = false; statTxFail++;
      if (txFailStreak < 255) txFailStreak++;
      logEvent("Radio send failed (error %d)", (int)e);
    }
    else {
      txq[i].at = millis() + 10;
      conf(C_TX, "TX busy (error %d), retry %u of 5", (int)e, (unsigned)txq[i].tries);
    }
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
  const uint8_t *raw = (const uint8_t *)&p;
  if (p.magic[0] != 'J' || p.magic[1] != 'F') {
    statBad++;
    if (devOn) confPkt(C_RX, raw, len, "RX rejected: not a Johnify packet (%d bytes)", len);
    return;
  }
  if (p.len > MAX_TEXT || len != PKT_HDR + 12 + p.len + 16) {
    statBad++;
    if (devOn) confPkt(C_RX, raw, len, "RX rejected: wrong length (%d bytes, text field %u)", len, (unsigned)p.len);
    return;
  }

  uint64_t key = pktKey(p.devId, p.msgId);
  char tag[7] = "";
  if (devOn) gidTag(p.gid, tag);
  if (seenHas(key)) {
    if (devOn) confPkt(C_RX, raw, len, "RX duplicate ignored  chat %s msg %08x dev %08x hops %u", tag, (unsigned)p.msgId, (unsigned)p.devId, (unsigned)p.hops);
    return;
  }
  seenAdd(key);
  statRx++; blinkLed();

  const char *result = "chat not known on this board";
  size_t plen = 12 + p.len;
  for (int i = 0; i < MAX_ROOMS; i++) {
    if (!rooms[i].used || memcmp(rooms[i].gid, p.gid, 8) != 0) continue;
    uint8_t aad[16]; makeAad(p, aad);
    uint8_t pt[12 + MAX_TEXT];
    uint32_t t0 = devOn ? micros() : 0;
    mbedtls_gcm_context g; mbedtls_gcm_init(&g);
    int rc = mbedtls_gcm_setkey(&g, MBEDTLS_CIPHER_ID_AES, rooms[i].key, 256);
    if (!rc) rc = mbedtls_gcm_auth_decrypt(&g, plen, p.nonce, 12, aad, 16, p.body + plen, 16, p.body, pt);
    mbedtls_gcm_free(&g);
    if (devOn) conf(C_CRYPTO, "open %s chat %s msg %08x %u B in %lu us", rc == 0 ? "OK" : "FAILED", tag, (unsigned)p.msgId, (unsigned)plen, (unsigned long)(micros() - t0));
    if (rc == 0) {
      char name[13]; memcpy(name, pt, 12); name[12] = 0;
      char text[MAX_TEXT + 1]; memcpy(text, pt + 12, p.len); text[p.len] = 0;
      storeMsg(rooms[i].code, name, text, p.devId, p.msgId);
      result = "decrypted and stored";
      if (devOn) conf(C_CHAT, "message stored for chat %s (%u bytes of text)", tag, (unsigned)p.len);
    } else {
      statAuth++;
      result = "security check FAILED, dropped";
      if (statAuth % 50 == 1) logEvent("Packet failed its security check (%lu)", (unsigned long)statAuth);
    }
    break;
  }

  bool relayed = false;
  if (repeaterOn && p.hops < MAX_HOPS) {
    Pkt c; memcpy(&c, &p, len); c.hops++;
    relayed = enqueueTx(c, len, 10 + (esp_random() % 50));
    if (relayed) statRelay++;
  }
  if (devOn) confPkt(C_RX, raw, len, "RX chat %s msg %08x dev %08x hops %u len %u: %s%s", tag, (unsigned)p.msgId, (unsigned)p.devId, (unsigned)p.hops, (unsigned)p.len, result, relayed ? ", relay queued" : "");
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
  lastStatus = 200; lastBytes = strlen(body);
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", body);
}

void fail(int code, const char *m) {
  lastStatus = code; lastBytes = strlen(m);
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
  int ri = ensureRoom(room, true);
  if (devOn) {
    char tg[7];
    gidTag(rooms[ri].gid, tg);
    conf(C_CHAT, "phone joined chat %s", tg);
  }
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
  uint32_t t0 = devOn ? micros() : 0;
  mbedtls_gcm_context g; mbedtls_gcm_init(&g);
  int rc = mbedtls_gcm_setkey(&g, MBEDTLS_CIPHER_ID_AES, rooms[ri].key, 256);
  if (!rc) rc = mbedtls_gcm_crypt_and_tag(&g, MBEDTLS_GCM_ENCRYPT, plen, p.nonce, 12, aad, 16, pt, p.body, 16, p.body + plen);
  mbedtls_gcm_free(&g);
  if (rc) { logEvent("Encryption failed (%d)", rc); return fail(500, "crypto"); }
  if (devOn) {
    char tg[7];
    gidTag(p.gid, tg);
    conf(C_CRYPTO, "seal chat %s msg %08x %u B text -> %u B packet in %lu us", tg, (unsigned)p.msgId, (unsigned)tl, (unsigned)(PKT_HDR + plen + 16), (unsigned long)(micros() - t0));
  }

  uint8_t len = (uint8_t)(PKT_HDR + plen + 16);
  if (!enqueueTx(p, len, 0)) { logEvent("Send queue full"); return fail(503, "radio busy"); }
  enqueueTx(p, len, 30 + (esp_random() % 40));
  if (devOn) {
    char tg[7];
    gidTag(p.gid, tg);
    conf(C_CHAT, "message from a phone queued for the radio, sent twice: chat %s msg %08x (%u bytes of text)", tg, (unsigned)p.msgId, (unsigned)tl);
  }

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

void handleLeave() {
  char room[ROOM_LEN + 1];
  if (!normRoom(server.arg("room"), room)) return fail(400, "bad room");
  int i = findRoom(room);
  if (i >= 0) {
    char tg[7];
    gidTag(rooms[i].gid, tg);
    memset(&rooms[i], 0, sizeof rooms[i]);
    saveRooms();
    logEvent("Left chat %s", tg);
    conf(C_CHAT, "phone left chat %s, board forgot it", tg);
  }
  for (int k = 0; k < MAX_MSGS; k++) if (strcmp(msgs[k].room, room) == 0) msgs[k].room[0] = 0;
  sendJson("{\"ok\":true}");
}

void handleReset() {
  if (server.method() != HTTP_POST) return fail(405, "post only");
  if (server.arg("confirm") != String("RESET")) return fail(400, "confirm");
  devStop();
  memset(rooms, 0, sizeof rooms);
  memset(msgs, 0, sizeof msgs);
  memset(seenKeys, 0, sizeof seenKeys);
  memset(txq, 0, sizeof txq);
  memset(logLines, 0, sizeof logLines);
  portENTER_CRITICAL(&rxMux);
  rxHead = 0; rxTail = 0;
  portEXIT_CRITICAL(&rxMux);
  seenPos = 0; nextSeq = 1; logHead = 0; logCount = 0;
  statRx = statTx = statRelay = statTxFail = statBad = statAuth = statReinit = 0;
  statRxDrop = 0; txFailStreak = 0; lowHeapCount = 0;
  repeaterOn = false;
  prefs.clear();
  restartPending = true;
  restartAt = millis() + 800;
  sendJson("{\"ok\":true}");
}

void outHex(Out &o, const uint8_t *d, size_t n) {
  static const char H[] = "0123456789abcdef";
  if (o.n + n * 2 + 1 > o.cap) return;
  for (size_t i = 0; i < n; i++) { o.b[o.n++] = H[d[i] >> 4]; o.b[o.n++] = H[d[i] & 15]; }
  o.b[o.n] = 0;
}

const char *resetName() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "Power on";
    case ESP_RST_SW: return "Software restart";
    case ESP_RST_PANIC: return "Crash";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT: return "Watchdog";
    case ESP_RST_BROWNOUT: return "Brownout (low voltage)";
    case ESP_RST_DEEPSLEEP: return "Deep sleep wake";
    case ESP_RST_EXT: return "Reset button";
    default: return "Other";
  }
}

void sampler() {
  uint32_t now = millis();
  if (now - lastSample < 1000) return;
  uint32_t win = (now - lastSample) * 1000UL;
  uint32_t pct = (uint32_t)(((uint64_t)busyUs * 100ULL) / win);
  cpuPct = pct > 100 ? 100 : (uint8_t)pct;
  busyUs = 0; lastSample = now;

  wifi_sta_list_t list;
  memset(&list, 0, sizeof list);
  if (esp_wifi_ap_get_sta_list(&list) != ESP_OK) return;
  uint8_t cur[10][6]; int8_t rs[10]; int n = 0;
  memset(cur, 0, sizeof cur); memset(rs, 0, sizeof rs);
  for (int i = 0; i < list.num && n < 10; i++) { memcpy(cur[n], list.sta[i].mac, 6); rs[n] = list.sta[i].rssi; n++; }
  for (int i = 0; i < n; i++) {
    bool known = false;
    for (int k = 0; k < staN; k++) if (memcmp(cur[i], staMac[k], 6) == 0) known = true;
    if (!known) conf(C_WIFI, "device connected %02x:%02x:%02x:xx:xx:xx signal %d dBm", cur[i][0], cur[i][1], cur[i][2], (int)rs[i]);
  }
  for (int k = 0; k < staN; k++) {
    bool still = false;
    for (int i = 0; i < n; i++) if (memcmp(cur[i], staMac[k], 6) == 0) still = true;
    if (!still) conf(C_WIFI, "device disconnected %02x:%02x:%02x:xx:xx:xx", staMac[k][0], staMac[k][1], staMac[k][2]);
  }
  memcpy(staMac, cur, sizeof cur); memcpy(staRssi, rs, sizeof rs); staN = n;
}

void noteClient() {
  uint32_t k = (uint32_t)server.client().remoteIP();
  int slot = -1, oldest = 0;
  for (int i = 0; i < 8; i++) {
    if (clients[i].ip == k) { slot = i; break; }
    if (clients[i].last < clients[oldest].last) oldest = i;
  }
  if (slot < 0) { slot = oldest; clients[slot].ip = k; clients[slot].count = 0; }
  clients[slot].last = millis() | 1;
  clients[slot].count++;
}

bool showArg(const String &n) {
  return n == "dev" || n == "since" || n == "boot" || n == "on" || n == "polls" || n == "clear" || n == "confirm";
}

void logRequest(uint32_t us) {
  char args[72]; args[0] = 0; size_t n = 0;
  for (int i = 0; i < server.args() && n < sizeof args - 26; i++) {
    String name = server.argName(i), val = server.arg(i);
    int w = showArg(name)
      ? snprintf(args + n, sizeof args - n, "%s%s=%.12s", n ? "&" : "", name.c_str(), val.c_str())
      : snprintf(args + n, sizeof args - n, "%s%s=<%uB>", n ? "&" : "", name.c_str(), (unsigned)val.length());
    if (w < 0) break;
    n += (size_t)w;
    if (n >= sizeof args) { n = sizeof args - 1; break; }
  }
  IPAddress ip = server.client().remoteIP();
  conf(C_HTTP, "%u.%u.%u.%u %s %s%s%s -> %d %uB %lu.%lu ms", ip[0], ip[1], ip[2], ip[3],
       server.method() == HTTP_POST ? "POST" : "GET", server.uri().c_str(), n ? "?" : "", args,
       lastStatus, (unsigned)lastBytes, (unsigned long)(us / 1000), (unsigned long)((us % 1000) / 100));
}

void routed(void (*fn)(), uint8_t mode) {
  Guard g;
  if (!devOn) { fn(); return; }
  if (mode != 2) noteClient();
  lastStatus = 0; lastBytes = 0;
  uint32_t t0 = micros();
  fn();
  uint32_t dt = micros() - t0;
  busyUs += dt;
  if (mode == 0 || (mode == 1 && devPolls)) logRequest(dt);
}

void routedFree(void (*fn)()) {
  if (!devOn) { fn(); return; }
  noteClient();
  lastStatus = 0; lastBytes = 0;
  uint32_t t0 = micros();
  fn();
  logRequest(micros() - t0);
}

void handleDev() {
  if (server.method() != HTTP_POST) return fail(405, "post only");
  if (server.hasArg("on")) {
    if (server.arg("on") == "1") devStart(); else devStop();
  }
  if (devOn) {
    devUntil = millis() + DEV_LEASE_MS;
    if (server.hasArg("polls")) devPolls = server.arg("polls") == "1";
    if (server.arg("clear") == "1") clearConsole();
  }
  sendJson(devOn ? (devPolls ? "{\"on\":true,\"polls\":true}" : "{\"on\":true,\"polls\":false}") : "{\"on\":false,\"polls\":false}");
}

void handleConsole() {
  char bootHex[9]; snprintf(bootHex, sizeof bootHex, "%08x", (unsigned)bootId);
  uint32_t since = (uint32_t)server.arg("since").toInt();
  if (server.arg("boot") != String(bootHex)) since = 0;
  conBuf[0] = 0;
  Out o = { conBuf, sizeof conBuf, 0 };
  if (!devOn) {
    outF(o, "{\"on\":false,\"polls\":false,\"boot\":\"%s\",\"lost\":0,\"lines\":[],\"next\":%lu,\"more\":false}", bootHex, (unsigned long)since);
    sendJson(o.b);
    return;
  }
  devUntil = millis() + DEV_LEASE_MS;
  portENTER_CRITICAL(&conMux);
  uint32_t newest = conSeq - 1;
  portEXIT_CRITICAL(&conMux);
  uint32_t oldest = newest > CON_LINES ? newest - CON_LINES + 1 : 1;
  if (since > newest) since = 0;
  uint32_t first = since + 1, lost = 0;
  if (first < oldest) { lost = oldest - first; first = oldest; }
  uint32_t last = first - 1;
  int count = 0;
  outF(o, "{\"on\":true,\"polls\":%s,\"boot\":\"%s\",\"lost\":%lu,\"lines\":[", devPolls ? "true" : "false", bootHex, (unsigned long)lost);
  for (uint32_t seq = first; seq <= newest; seq++) {
    if (count >= 40 || o.cap - o.n < 1100) break;
    ConLine l; PktRec pr; bool hasPk = false, ok = false;
    portENTER_CRITICAL(&conMux);
    ConLine &src = conLines[(seq - 1) % CON_LINES];
    if (src.seq == seq) {
      l = src; ok = true;
      if (l.pk >= 0 && pktRecs[l.pk].seq == seq) { pr = pktRecs[l.pk]; hasPk = true; }
    }
    portEXIT_CRITICAL(&conMux);
    last = seq;
    if (!ok) continue;
    if (count) outPut(o, ",");
    outF(o, "{\"s\":%lu,\"t\":%lu,\"c\":\"%s\",\"x\":", (unsigned long)seq, (unsigned long)l.ms, CAT_NAMES[l.cat]);
    outJson(o, l.text);
    if (hasPk) { outPut(o, ",\"h\":\""); outHex(o, pr.data, pr.len); outPut(o, "\""); }
    outPut(o, "}");
    count++;
  }
  outF(o, "],\"next\":%lu,\"more\":%s}", (unsigned long)last, last < newest ? "true" : "false");
  sendJson(o.b);
}

void handlePerf() {
  if (!devOn) { sendJson("{\"on\":false}"); return; }
  devUntil = millis() + DEV_LEASE_MS;
  pollBuf[0] = 0;
  Out o = { pollBuf, sizeof pollBuf, 0 };
  const char *chip = ESP.getChipModel();
  float t = temperatureRead();
  outPut(o, "{\"on\":true,\"temp\":");
  if (strcmp(chip, "ESP32") != 0 && t > -40.0f && t < 125.0f) outF(o, "%.1f", (double)t); else outPut(o, "null");
  outF(o, ",\"cpu\":%u,\"mhz\":%u,\"cores\":%u,\"chip\":\"%s\",\"heapTotal\":%lu,\"heapFree\":%lu,\"heapMax\":%lu",
       (unsigned)cpuPct, (unsigned)ESP.getCpuFreqMHz(), (unsigned)ESP.getChipCores(), chip,
       (unsigned long)ESP.getHeapSize(), (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getMaxAllocHeap());
  if (ESP.getPsramSize()) outF(o, ",\"psramTotal\":%lu,\"psramFree\":%lu", (unsigned long)ESP.getPsramSize(), (unsigned long)ESP.getFreePsram());
  else outPut(o, ",\"psramTotal\":null,\"psramFree\":null");
  outF(o, ",\"flash\":%lu,\"sketch\":%lu,\"sketchFree\":%lu,\"up\":%lu,\"reset\":\"%s\",\"sta\":[",
       (unsigned long)ESP.getFlashChipSize(), (unsigned long)ESP.getSketchSize(), (unsigned long)ESP.getFreeSketchSpace(),
       (unsigned long)(millis() / 1000), resetName());
  for (int i = 0; i < staN; i++) {
    if (i) outPut(o, ",");
    outF(o, "{\"mac\":\"%02x:%02x:%02x:xx:xx:xx\",\"rssi\":%d}", staMac[i][0], staMac[i][1], staMac[i][2], (int)staRssi[i]);
  }
  outPut(o, "],\"clients\":[");
  int c = 0;
  for (int i = 0; i < 8; i++) {
    if (!clients[i].ip) continue;
    uint32_t k = clients[i].ip;
    if (c++) outPut(o, ",");
    outF(o, "{\"ip\":\"%u.%u.%u.%u\",\"age\":%lu,\"n\":%lu}", (unsigned)(k & 255), (unsigned)((k >> 8) & 255), (unsigned)((k >> 16) & 255),
         (unsigned)(k >> 24), (unsigned long)((millis() - clients[i].last) / 1000), (unsigned long)clients[i].count);
  }
  outPut(o, "]}");
  sendJson(o.b);
}

void handleRoot() {
  lastStatus = 200; lastBytes = strlen_P(INDEX_HTML);
  server.sendHeader("Cache-Control", "no-cache");
  server.send_P(200, "text/html", INDEX_HTML);
}

void webTask(void *arg) {
  for (;;) {
    dns.processNextRequest();
    server.handleClient();
    delay(2);
  }
}

void handleNotFound() {
  IPAddress ip = WiFi.softAPIP();
  char loc[40];
  snprintf(loc, sizeof loc, "http://%u.%u.%u.%u/", ip[0], ip[1], ip[2], ip[3]);
  lastStatus = 302; lastBytes = 0;
  server.sendHeader("Location", loc, true);
  server.send(302, "text/plain", "");
}

void setup() {
  Serial.begin(115200);
  stateMx = xSemaphoreCreateMutex();
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
  server.on("/", []() { routedFree(handleRoot); });
  server.on("/api/hello", []() { routed(handleHello, 0); });
  server.on("/api/join", []() { routed(handleJoin, 0); });
  server.on("/api/send", []() { routed(handleSend, 0); });
  server.on("/api/poll", []() { routed(handlePoll, 1); });
  server.on("/api/status", []() { routed(handleStatus, 1); });
  server.on("/api/log", []() { routed(handleLog, 1); });
  server.on("/api/repeater", []() { routed(handleRepeater, 0); });
  server.on("/api/leave", []() { routed(handleLeave, 0); });
  server.on("/api/reset", []() { routed(handleReset, 0); });
  server.on("/api/dev", []() { routed(handleDev, 0); });
  server.on("/api/console", []() { routed(handleConsole, 2); });
  server.on("/api/perf", []() { routed(handlePerf, 1); });
  server.on("/favicon.ico", []() { routedFree([]() { lastStatus = 204; lastBytes = 0; server.send(204); }); });
  server.onNotFound([]() { routedFree(handleNotFound); });
  server.begin();

  int saved = 0;
  for (int i = 0; i < MAX_ROOMS; i++) if (rooms[i].used) saved++;
  logEvent("Ready: %s, repeater %s, %d chats saved", apSsid, repeaterOn ? "on" : "off", saved);
  logEvent("Open http://%s", WiFi.softAPIP().toString().c_str());
  if (xTaskCreate(webTask, "johnify-web", 10240, NULL, 1, NULL) != pdPASS) {
    logEvent("Web server task failed to start, rebooting");
    delay(2000); ESP.restart();
  }
}

void loop() {
  {
    Guard g;
    uint32_t t0 = devOn ? micros() : 0;
    processRx();
    processTx();
    healthCheck();
    if (devOn) { sampler(); busyUs += micros() - t0; }
    devCheck();
    if (restartPending && (int32_t)(millis() - restartAt) >= 0) ESP.restart();
#ifdef LED_BUILTIN
    if (ledOffAt && (int32_t)(millis() - ledOffAt) >= 0) { digitalWrite(LED_BUILTIN, HIGH); ledOffAt = 0; }
#endif
  }
  delay(1);
}
