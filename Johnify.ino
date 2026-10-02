

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
uint32_t statRx = 0, statTx = 0, statRelay = 0;   // Counters shown on the Repeater page
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
<title>Johnify</title>
<style>
:root{
  --bg:#e9e3d4;--sf:#f6f2e8;--ink:#1f1d19;--mu:#6f685a;--ln:#cdc5b1;
  --ac:#c4421f;--actx:#fff8ee;--ok:#3f6b3a;--mine:#1f1d19;--minetx:#f6f2e8;--sh:#1f1d19;
  --serif:"Iowan Old Style","Palatino Linotype",Palatino,Georgia,serif;
  --mono:ui-monospace,"SF Mono",Menlo,Consolas,monospace}
@media(prefers-color-scheme:dark){:root{
  --bg:#171510;--sf:#221f19;--ink:#ece5d4;--mu:#9a917d;--ln:#3a352b;
  --ac:#e2693f;--actx:#1a1510;--ok:#8db36b;--mine:#ece5d4;--minetx:#171510;--sh:#000}}
*{box-sizing:border-box}
html,body{margin:0;height:100%}
body{display:flex;flex-direction:column;height:100dvh;background:var(--bg);color:var(--ink);
  font:16px/1.45 system-ui,-apple-system,"Segoe UI",Roboto,sans-serif}
button,input,textarea{font:inherit;color:inherit}

header{display:flex;align-items:baseline;justify-content:space-between;
  padding:12px 14px 6px;padding-top:max(12px,env(safe-area-inset-top))}
.mark{font:700 27px/1 var(--serif);letter-spacing:-.5px}
.link{display:flex;align-items:center;gap:6px;font:11px var(--mono);
  text-transform:uppercase;letter-spacing:.8px;color:var(--mu)}
.sq{width:8px;height:8px;background:var(--mu)}.sq.on{background:var(--ok)}.sq.off{background:var(--ac)}

#bar{display:flex;gap:8px;padding:8px 14px 12px;border-bottom:1.5px solid var(--ink)}
.btn{flex:1;border:1.5px solid var(--ink);background:var(--sf);border-radius:3px;
  padding:9px 6px;font:600 13px var(--mono);letter-spacing:.2px;cursor:pointer;
  box-shadow:2px 2px 0 var(--sh);white-space:nowrap}
.btn:active{transform:translate(2px,2px);box-shadow:none}
.btn.pri,.btn.on{background:var(--ac);color:var(--actx);border-color:var(--ac)}
.btn.sm{flex:none;padding:7px 10px;font-size:12px}
.btn.big{width:100%;padding:13px;font-size:15px}

#view{flex:1;min-height:0;display:flex;flex-direction:column;overflow-y:auto;padding:6px 14px 12px}

.empty{margin:auto 0;padding:24px 0}
.empty h2{font:700 24px/1.15 var(--serif);margin:0 0 10px}
.empty p{margin:0 0 8px;color:var(--mu);max-width:34ch}
.empty ol{margin:14px 0 0;padding-left:20px;max-width:34ch}.empty li{margin-bottom:6px}

.row{flex:none;display:flex;align-items:center;gap:12px;padding:13px 2px;
  border-bottom:1px solid var(--ln);cursor:pointer}
.grow{flex:1;min-width:0}
.code{font:700 17px var(--mono);letter-spacing:1px}
.prev{color:var(--mu);font-size:14px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.badge{font:700 12px var(--mono);background:var(--ac);color:var(--actx);padding:2px 7px;border-radius:2px}

.chead{flex:none;display:flex;align-items:center;gap:8px;padding:8px 0 10px;
  border-bottom:1px solid var(--ln);margin-bottom:8px}
.cmid{flex:1;background:none;border:0;display:flex;flex-direction:column;align-items:center;cursor:pointer;padding:0}
.ccode{font:700 17px var(--mono);letter-spacing:1px}
.csub{font:10px var(--mono);text-transform:uppercase;letter-spacing:.7px;color:var(--mu)}

.msgs{flex:1;min-height:0;overflow-y:auto;display:flex;flex-direction:column;gap:7px;padding:2px 0}
.hint{margin:auto;color:var(--mu);font-family:var(--serif);font-style:italic}
.b{max-width:82%;padding:8px 11px;border-radius:4px;border:1px solid var(--ln);background:var(--sf);word-break:break-word}
.b.me{align-self:flex-end;background:var(--mine);color:var(--minetx);border-color:var(--mine)}
.b.them{align-self:flex-start}
.who{font:700 11px var(--mono);text-transform:uppercase;letter-spacing:.6px;color:var(--ac);margin-bottom:2px}
.tx{white-space:pre-wrap}
.tm{font:10px var(--mono);opacity:.6;text-align:right;margin-top:2px}

.comp{flex:none;display:flex;gap:8px;padding-top:10px;padding-bottom:env(safe-area-inset-bottom)}
textarea,.inp{flex:1;width:100%;background:var(--sf);border:1.5px solid var(--ink);border-radius:3px;padding:10px 12px;resize:none;outline:none}
textarea:focus,.inp:focus{border-color:var(--ac)}
.comp .btn{flex:none;padding:0 18px}

.card{flex:none;background:var(--sf);border:1.5px solid var(--ink);border-radius:3px;padding:14px;margin:10px 0 0}
.card h3{font:700 19px var(--serif);margin:0 0 6px}
.sub{color:var(--mu);font-size:14px;margin:0 0 12px}
.st{display:flex;justify-content:space-between;gap:12px;padding:7px 0;border-top:1px solid var(--ln);font-size:14px}
.st span{color:var(--mu)}.st b{font:600 13px var(--mono);text-align:right;word-break:break-all}

#modal{position:fixed;inset:0;background:rgba(20,18,14,.55);display:flex;align-items:center;justify-content:center;padding:20px}
#modal[hidden]{display:none}
.box{background:var(--bg);border:1.5px solid var(--ink);border-radius:3px;box-shadow:4px 4px 0 var(--sh);padding:18px;width:100%;max-width:340px}
.box h3{font:700 20px var(--serif);margin:0 0 4px}
.box .inp{font:700 20px var(--mono);letter-spacing:2px;text-align:center;text-transform:uppercase;margin:12px 0}
.acts{display:flex;gap:8px}.acts .btn{padding:11px}

#toast{position:fixed;left:50%;bottom:92px;transform:translateX(-50%);background:var(--ink);color:var(--bg);
  padding:9px 14px;border-radius:3px;font:13px var(--mono);opacity:0;pointer-events:none;transition:opacity .2s;max-width:90%;text-align:center}
#toast.on{opacity:1}
</style></head><body>
<header>
 <span class="mark">Johnify</span>
 <span class="link"><span id="sq" class="sq"></span><span id="lt">Connecting</span></span>
</header>
<nav id="bar">
 <button id="bCreate" class="btn pri">Create chat</button>
 <button id="bJoin" class="btn">Join</button>
 <button id="bMenu" class="btn">Repeater</button>
</nav>
<main id="view"></main>
<div id="modal" hidden><div class="box">
 <h3>Join a chat</h3>
 <div class="sub" style="margin:0">Enter the 10-character code you were given.</div>
 <input id="jc" class="inp" maxlength="10" placeholder="CODE" autocapitalize="characters" autocomplete="off" spellcheck="false">
 <div class="acts"><button id="jx" class="btn">Cancel</button><button id="jg" class="btn pri">Join</button></div>
</div></div>
<div id="toast"></div>
<script>
// ---- small helpers -------------------------------------------------------
const $=s=>document.querySelector(s);
const el=(t,c,x)=>{const e=document.createElement(t);if(c)e.className=c;if(x!=null)e.textContent=x;return e};
const bytes=s=>new TextEncoder().encode(s).length;
const fmtUp=s=>Math.floor(s/3600)+'h '+Math.floor(s%3600/60)+'m';

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
function setLink(ok){$('#sq').className='sq '+(ok?'on':'off');$('#lt').textContent=ok?'Linked':'No link'}

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
  box.value='';
  try{
    if(!S.dev)await hello();
    await api('/api/send',{room:cur,dev:S.dev,name:S.name||('User-'+S.dev.slice(0,4)),text:t},true);
    busy=false;poll()}
  catch(e){box.value=t;toast('Did not send. Check your Wi-Fi.')}}

// ---- screens -------------------------------------------------------------
function goList(){view='list';cur=null;render()}
function openRoom(c){cur=c;view='chat';S.unread[c]=0;save();render()}

function render(){
  $('#bMenu').classList.toggle('on',view==='repeater');
  clearInterval(stT);
  const v=$('#view');v.innerHTML='';
  if(view==='list')drawList(v);else if(view==='chat')drawChat(v);else drawRep(v)}

function drawList(v){
  if(!S.rooms.length){
    const c=el('div','empty');
    c.append(el('h2',0,'No chats yet.'),
      el('p',0,'Messages travel between Johnify boards by radio. No internet needed.'));
    const ol=el('ol');
    ['Press Create chat to get a 10-character code.','Give the code to the other person.','They press Join and type it in.']
      .forEach(x=>ol.append(el('li',0,x)));
    c.append(ol);v.append(c);return}
  for(const code of S.rooms.slice().reverse()){
    const L=S.msgs[code]||[],last=L[L.length-1],r=el('div','row'),a=el('div','grow');
    a.append(el('div','code',code),el('div','prev',last?(last.mine?'You: ':'')+last.text:'No messages yet'));
    r.append(a);
    if(S.unread[code])r.append(el('span','badge',S.unread[code]));
    r.onclick=()=>openRoom(code);v.append(r)}}

function drawChat(v){
  const head=el('div','chead');
  const bk=el('button','btn sm','\u2039 Chats');bk.onclick=goList;
  const mid=el('button','cmid');mid.append(el('span','ccode',cur),el('span','csub','encrypted \u00b7 tap to copy'));
  mid.onclick=()=>copy(cur);
  const lv=el('button','btn sm','Leave');
  lv.onclick=()=>{if(confirm('Remove this chat from this phone?')){
    S.rooms=S.rooms.filter(x=>x!==cur);delete S.msgs[cur];delete S.unread[cur];save();goList()}};
  head.append(bk,mid,lv);

  const m=el('div','msgs');m.id='msgs';
  const f=el('div','comp'),t=el('textarea');t.id='txt';t.rows=1;t.placeholder='Write a message';
  t.onkeydown=e=>{if(e.key==='Enter'&&!e.shiftKey){e.preventDefault();send()}};
  const s=el('button','btn pri','Send');s.onclick=send;
  f.append(t,s);v.append(head,m,f);drawMsgs(true)}

function drawMsgs(force){
  const m=$('#msgs');if(!m)return;
  const stick=force||m.scrollHeight-m.scrollTop-m.clientHeight<90;   // keep scrolling only if already at the bottom
  m.innerHTML='';
  const L=S.msgs[cur]||[];
  if(!L.length)m.append(el('div','hint','Nothing here yet.'));
  for(const x of L){
    const b=el('div','b '+(x.mine?'me':'them'));
    if(!x.mine)b.append(el('div','who',x.name||'?'));
    b.append(el('div','tx',x.text),
      el('div','tm',new Date(x.t).toLocaleTimeString([],{hour:'2-digit',minute:'2-digit'})));
    m.append(b)}
  if(stick)m.scrollTop=m.scrollHeight}

function drawRep(v){
  const c1=el('div','card');
  c1.append(el('h3',0,'Repeater'),
    el('p','sub','When this is on, the board re-sends every message it hears from other boards, for any chat. Use it to bridge two boards that are too far apart to hear each other. A repeater cannot read the messages.'));
  let on=false;
  const tg=el('button','btn big');
  const paint=()=>{tg.textContent='Repeater is '+(on?'ON':'OFF');tg.classList.toggle('pri',on)};
  tg.onclick=async()=>{
    try{await api('/api/repeater',{on:on?0:1},true);on=!on;paint();toast(on?'Repeater on':'Repeater off')}
    catch(e){toast('Could not change the setting')}};
  paint();
  const st=el('div');st.style.marginTop='12px';
  c1.append(tg,st);

  const c2=el('div','card');
  c2.append(el('h3',0,'Your name'));
  const inp=el('input','inp');inp.style.cssText='font:16px system-ui;letter-spacing:0;text-transform:none;text-align:left;margin:0 0 10px';
  inp.placeholder='Shown next to your messages (12 characters)';inp.maxLength=12;inp.value=S.name;
  inp.oninput=()=>{S.name=inp.value.trim();save()};
  const id=el('div','st');id.style.borderTop='0';id.append(el('span',0,'Device ID'),el('b',0,S.dev||'-'));
  c2.append(inp,id);

  const back=el('button','btn sm','\u2039 Chats');back.style.alignSelf='flex-start';back.onclick=goList;
  v.append(back,c1,c2);

  const update=async()=>{
    try{
      const s=await api('/api/status');on=!!s.repeater;paint();st.innerHTML='';
      [['Network',s.ssid],['Phones connected',s.clients],['Chats stored',s.rooms],
       ['Packets heard',s.rx],['Packets sent',s.tx],['Relayed',s.relayed],['Uptime',fmtUp(s.up)]]
        .forEach(([k,x])=>{const d=el('div','st');d.append(el('span',0,k),el('b',0,String(x)));st.append(d)})
    }catch(e){}};
  update();stT=setInterval(update,2500)}

// ---- buttons and start-up ------------------------------------------------
$('#bMenu').onclick=()=>{if(view==='repeater')goList();else{view='repeater';render()}};
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

// /api/status : numbers shown on the Repeater page.
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
