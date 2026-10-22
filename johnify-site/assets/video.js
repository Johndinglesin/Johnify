(()=>{const $=s=>document.querySelector(s),rm=matchMedia('(prefers-reduced-motion:reduce)').matches;
const cv=$('#vid'),main=cv.getContext('2d'),oa=document.createElement('canvas'),ob=document.createElement('canvas'),ga=oa.getContext('2d'),gb=ob.getContext('2d'),VW=640,VH=480,SANS='"IBM Plex Sans",system-ui,sans-serif',MONO='"IBM Plex Mono",ui-monospace,monospace';
const SC=[
{d:6,t:'No infrastructure',x:'There is no router, cell tower or internet. Each board runs its own Wi-Fi hotspot, named Johnify plus four characters from its MAC address, and serves the chat app itself. Boards talk to each other directly over ESP-NOW, a connectionless 2.4 GHz protocol built into the ESP32.'},
{d:7,t:'Phone to board',x:'You type on your phone. The app sends the chat code, your device ID, your name and the text to the board over plain HTTP. A plain http page has no browser crypto, which is why encryption happens on the board.'},
{d:7,t:'Chat code to key',x:'The 10-character chat code is stretched into a 32-byte key with SHA-256 and 5,000 more rounds that mix the code back in. A second hash of the key gives an 8-byte group ID, so boards can recognise a chat without the key ever going on the air.'},
{d:7,t:'Seal the message',x:'The 12-byte name and the text are encrypted with AES-256-GCM and a fresh random nonce. The group ID, message ID and device ID are authenticated but left readable, so changing any of them breaks decryption. This example totals 69 of 250 bytes.'},
{d:7,t:'Send it twice',x:'Broadcast frames are not acknowledged, so the board sends every packet twice, 30 to 70 ms apart. Each receiver remembers the last 256 device and message IDs, so the second copy is recognised and dropped.'},
{d:8,t:'Relay when out of range',x:'Radio range is the real limit. A board in repeater mode re-broadcasts what it hears with the hop count raised by one, after a random 10 to 60 ms wait, up to 4 hops. It never needs the chat code, so it cannot read what it carries.'},
{d:6,t:'Open the message',x:'A board that knows the chat code matches the group ID, checks the GCM tag, decrypts and stores the message. Phones poll their own board every 1.5 seconds, and the message appears on screen.'}];
let g=main,st=[],tot=0;SC.forEach(s=>{st.push(tot);tot+=s.d});
$('#chs').innerHTML=SC.map((s,i)=>`<li><button data-i="${i}"><span>${i+1}</span>${s.t}</button></li>`).join('');
$('#desc').innerHTML=SC.map((s,i)=>`<div><div class="m sn">Step ${i+1} of ${SC.length}</div><h3>${s.t}</h3><p>${s.x}</p></div>`).join('');
let vt=0,playing=false,sc=1,c={},cur=-1,lt=0,started=false;
const ease=x=>x<0?0:x>1?1:x*x*(3-2*x),sg=(p,a,b)=>ease((p-a)/(b-a));
function T(s,x,y,z,k,al='center',mono=0,mw=0,w=500){g.textAlign=al;g.fillStyle=k||c.ink;const f=()=>g.font=`${w} ${z}px ${mono?MONO:SANS}`;f();if(mw)while(g.measureText(s).width>mw&&z>11){z-=.5;f()}g.fillText(s,x,y)}
function R(x,y,w,h,f,s,r=6,lw=2){g.lineWidth=lw;g.beginPath();g.roundRect(x,y,w,h,r);if(f){g.fillStyle=f;g.fill()}if(s){g.strokeStyle=s;g.stroke()}}
function L(a,b,x,y,k,lw=2,d=[]){g.strokeStyle=k||c.ink;g.lineWidth=lw;g.setLineDash(d);g.lineCap='round';g.beginPath();g.moveTo(a,b);g.lineTo(x,y);g.stroke();g.setLineDash([])}
function ring(x,y,p,Rm){if(p<=0||p>=1)return;g.strokeStyle=c.acc;g.lineWidth=3;for(let i=0;i<3;i++){const r=Rm*p-i*16;if(r>0){g.globalAlpha=(1-p)*(1-i*.28);g.beginPath();g.arc(x,y,r,0,7);g.stroke()}}g.globalAlpha=1}
function waves(x,y,d){g.strokeStyle=c.acc;g.lineWidth=2.5;for(let i=0;i<3;i++){g.globalAlpha=.2+.8*Math.max(0,Math.sin(vt*5-i*1.1));g.beginPath();g.arc(x,y,8+i*8,d>0?-.8:Math.PI-.8,d>0?.8:Math.PI+.8);g.stroke()}g.globalAlpha=1}
function phone(x,y,a,fn){g.globalAlpha=a;R(x,y,64,120,c.bg,c.ink,10);R(x+6,y+14,52,90,c.alt,null,3);if(fn)fn(x+6,y+14);g.globalAlpha=1}
function board(x,y,a,lab,fl=0,mono=0){g.globalAlpha=a;R(x,y,120,70,c.pcb,c.ink,5);R(x+40,y+16,40,38,'#0b1613',null,3);g.fillStyle=Math.sin(vt*7)>0?'#ff6a4a':'#6b2d25';g.beginPath();g.arc(x+12,y+12,4,0,7);g.fill();
 g.strokeStyle=c.ink;g.lineWidth=2.5;g.beginPath();g.moveTo(x+120,y+18);g.lineTo(x+126,y+18);g.lineTo(x+130,y+8);g.lineTo(x+136,y+26);g.lineTo(x+140,y+14);g.stroke();
 if(lab)T(lab,x+60,y+98,18,c.ink,'center',mono,150);if(fl>0){g.globalAlpha=fl;R(x-6,y-6,132,82,null,c.acc,8,3)}g.globalAlpha=1}
const PA=[14,190],BA=[118,215],BB=[402,215],PB=[562,190],fin='71b8c0de9a3f12e4b6c7d8e9f0a1b2c3d4e5f60718293a4b5c6d7e8f9a0b1c2d';
const S=[
(p)=>{[['Router',130],['Cell tower',320],['Internet',510]].forEach(([n,x],i)=>{g.globalAlpha=sg(p,.04+i*.07,.2+i*.07);g.strokeStyle=c.ink;g.lineWidth=2.5;
 if(i==0){R(x-36,66,72,26,c.alt,c.ink,4);L(x-24,66,x-28,40,c.ink);L(x+24,66,x+28,40,c.ink)}
 if(i==1){g.beginPath();g.moveTo(x,36);g.lineTo(x-22,96);g.lineTo(x+22,96);g.closePath();g.stroke();L(x-12,72,x+12,72,c.ink)}
 if(i==2)R(x-36,58,72,38,c.alt,c.ink,19);
 T(n,x,134,20,c.mute,'center',0,150);const q=sg(p,.4+i*.1,.52+i*.1);g.globalAlpha=1;L(x-44,26,x-44+88*q,26+88*q,c.bad,5);L(x+44,26,x+44-88*q,26+88*q,c.bad,5)});
 const a=sg(p,.2,.4);phone(...PA,a);phone(...PB,a);board(...BA,a,'Johnify-9F3A',0,1);board(...BB,a,'Johnify-C07E',0,1);
 if(a>.9){waves(84,250,1);waves(556,250,-1);L(242,250,398,250,c.mute,2,[4,8]);for(let k=0;k<3;k++){const u=(vt*.45+k/3)%1;g.fillStyle=c.acc;g.beginPath();g.arc(242+156*u,250,5,0,7);g.fill()}T('ESP-NOW',320,234,20,c.acc);T('Phones only talk to their own board',320,410,20,c.mute,'center',0,600)}},
(p)=>{const P=[40,190],B=[250,215],n=sg(p,0,.35),snt=p>.42;phone(...P,1,(sx,sy)=>{R(sx+4,sy+68,44,14,c.bg,c.line,4,1.5);if(!snt)R(sx+7,sy+72,38*n,6,c.ink,null,2);else R(sx+10,sy+20,38,16,c.acc,null,6)});
 phone(562,190,.3);board(402,215,.3,'Board B');board(...B,1,'Board A');L(110,250,246,250,c.mute,2,[4,8]);T('HTTP POST',178,232,20,c.mute,'center',0,130);
 const u=sg(p,.45,.72);if(u>0&&u<1){R(112+u*100,238,34,24,c.acc,null,5)}
 const a=sg(p,.74,.92);if(a>0){g.globalAlpha=a;R(185,56,250,120,c.bg,c.bad,8);T('room  K7M2QX9TBA',200,92,19,c.ink,'left',1,222);T('name  Sam',200,122,19,c.ink,'left',1);T('text  meet at 6',200,152,19,c.ink,'left',1);L(310,176,310,212,c.bad,2);T('Plain text on the board',310,350,20,c.bad,'center',0,300);g.globalAlpha=1}},
(p)=>{const code='K7M2QX9TBA',pr=sg(p,.2,.85),n=Math.floor(pr*5000);T('chat code',320,26,18,c.mute);
 [...code].forEach((ch,i)=>{g.globalAlpha=sg(p,i*.015,.1+i*.015);R(102+i*44,40,40,48,c.bg,c.ink,5);T(ch,122+i*44,74,24,c.ink,'center',1);g.globalAlpha=1});
 g.globalAlpha=sg(p,.12,.2);L(320,92,320,116,c.ink);R(210,118,220,54,c.alt,c.ink,6);T('SHA-256 x 5,000',320,152,22,c.ink,'center',0,200,600);g.globalAlpha=1;
 T(String(n).replace(/\B(?=(\d{3})+$)/g,','),320,226,34,c.acc,'center',1,300);R(120,244,400,14,c.alt,c.line,7,1);g.fillStyle=c.acc;g.beginPath();g.roundRect(120,244,Math.max(0,400*pr),14,7);g.fill();
 T('key, 32 bytes',320,300,18,c.mute);const scr=Array.from({length:64},(_,i)=>(i<pr*64?fin:Math.floor(Math.random()*16).toString(16))[i<pr*64?i%fin.length:0]||'0').join('');
 T(scr.slice(0,32),320,328,18,pr>.99?c.acc:c.ink,'center',1,500);T(scr.slice(32),320,352,18,pr>.99?c.acc:c.ink,'center',1,500);
 g.globalAlpha=sg(p,.88,1);T('group ID',320,396,18,c.mute);T(fin.slice(0,16),320,424,24,c.acc,'center',1,400);g.globalAlpha=1},
(p)=>{g.globalAlpha=sg(p,0,.15);R(210,14,220,36,c.bg,c.ink,6);T('Sam + meet at 6',320,38,18,c.ink,'center',0,200);R(20,78,110,30,c.acc,null,6);T('key',75,98,18,c.accfg);R(20,114,110,30,'#b58a2f',null,6);T('nonce',75,134,18,'#fff');R(510,96,110,30,c.alt,c.ink,6);T('header',565,116,18,c.ink);g.globalAlpha=1;
 g.globalAlpha=sg(p,.15,.3);L(320,50,320,78,c.ink);L(130,93,200,100,c.ink);L(130,129,200,122,c.ink);L(510,111,440,111,c.ink);R(200,78,240,64,c.alt,c.ink,8);T('AES-256-GCM',320,118,24,c.ink,'center',0,220,600);g.globalAlpha=1;
 const u=600/69,K=[c.ink,c.ink,c.ink,c.ink,c.ink,'#b58a2f',c.ink,c.acc,c.bad],Z=[2,1,8,4,4,12,1,21,16];let x=20;
 g.globalAlpha=sg(p,.34,.46);L(320,142,320,236,c.mute,2,[4,6]);g.globalAlpha=1;
 Z.forEach((z,i)=>{const q=sg(p,.46+i*.05,.52+i*.05);g.globalAlpha=q;R(x,250,z*u-1.5,40,K[i],null,2);x+=z*u});g.globalAlpha=sg(p,.9,1);
 L(20,302,298,302,c.mute,2);L(300,302,481,302,c.mute,2);L(483,302,620,302,c.mute,2);T('header, readable',159,330,19,c.mute,'center',0,260);T('ciphertext',390,330,19,c.mute,'center',0,170);T('tag',551,330,19,c.mute,'center',0,130);T('69 of 250 bytes',320,396,28,c.ink,'center',0,500,600);g.globalAlpha=1},
(p)=>{const A=[30,200],B=[490,200],ca=[90,235],r1=(p-.1)/.4,r2=(p-.32)/.4,h1=.1+.4*.885,h2=.32+.4*.885,f=t=>t>0&&t<.1?1-t/.1:0;
 ring(...ca,r1,520);ring(...ca,r2,520);board(...A,1,'Board A');board(...B,1,'Board B',Math.max(f(p-h1),f(p-h2)));
 T('Sent twice, 30 to 70 ms apart',320,34,20,c.ink,'center',0,600);L(290,52,290,78,c.ink,3);L(350,52,350,78,c.ink,3);L(290,65,350,65,c.mute,1.5);
 if(p>h1){R(380,330,250,38,c.bg,c.acc,6);T('First copy: kept',505,355,18,c.acc,'center',0,230)}
 if(p>h2){R(380,380,250,38,c.bg,c.bad,6);T('Second copy: dropped',505,405,18,c.bad,'center',0,230)}},
(p)=>{const A=[10,200],Rp=[260,200],C=[510,200],h1=.08+.3*.833,h2=.58+.3*.833,f=t=>t>0&&t<.1?1-t/.1:0;
 ring(70,235,(p-.08)/.3,300);ring(320,235,(p-.58)/.3,300);board(...A,1,'Board A');board(...Rp,1,'Repeater',f(p-h1));board(...C,1,'Board C',f(p-h2));
 if(p>h1){R(220,90,200,60,c.bg,c.ink,6);const h=p>.5?1:0;T('hops '+h,320,129,28,h?c.acc:c.ink,'center',1,180)}
 if(p>.3&&p<.58)T('Out of range',570,170,20,c.bad,'center',0,120);if(p>=.58&&p<h2)T('Out of range',570,170,20,c.bad,'center',0,120,400);if(p>h2)T('Delivered',570,170,20,c.acc,'center',0,120);
 if(p>.4&&p<.58)T('Waits 10 to 60 ms',320,340,19,c.bad,'center',0,240);if(p>.6)T('Cannot read it',320,340,19,c.mute,'center',0,240)},
(p,s)=>{const rows=['Group ID matches a chat','GCM tag verifies','Decrypt and store'];
 rows.forEach((r,i)=>{const a=sg(p,.1+i*.17,.2+i*.17);g.globalAlpha=Math.max(.15,a);R(150,40+i*60,340,44,c.bg,c.ink,6);T(r,170,68+i*60,19,c.ink,'left',0,270);if(a>.95){g.strokeStyle=c.acc;g.lineWidth=3;g.beginPath();g.moveTo(458,62+i*60);g.lineTo(464,69+i*60);g.lineTo(474,55+i*60);g.stroke()}g.globalAlpha=1});
 board(20,300,1,'Board B');phone(540,275,1,(sx,sy)=>{if(p>.78)R(sx+8,sy+24,38,16,c.acc,null,6)});L(146,335,534,335,c.mute,2,[4,8]);T('GET /api/poll every 1.5 s',340,318,19,c.mute,'center',0,370);
 const u=(s%1.5)/.75;if(u<1){g.fillStyle=c.acc;g.beginPath();g.arc(534-u*388,335,6,0,7);g.fill()}
 g.globalAlpha=sg(p,.74,.9);T('Sam: meet at 6',320,440,28,c.ink,'center',0,560,600);T('Shown on the other phone',320,468,18,c.mute);g.globalAlpha=1}];
function paint(ctx,i,p,s){g=ctx;g.setTransform(sc,0,0,sc,0,0);g.globalAlpha=1;g.fillStyle=c.bg;g.fillRect(0,0,VW,VH);g.save();S[i](p,s);g.restore();g.globalAlpha=1}
function frame(ts){const dt=Math.min(.1,(ts-lt)/1000||0);lt=ts;if(playing){vt+=dt;if(vt>=tot)vt=0}
 const cs=getComputedStyle(document.documentElement),v=n=>cs.getPropertyValue(n).trim();c={ink:v('--ink'),bg:v('--bg'),alt:v('--alt'),mute:v('--mute'),line:v('--line'),acc:v('--acc'),accfg:v('--accfg'),bad:v('--bad'),pcb:v('--pcb')};
 let i=st.length-1;while(vt<st[i])i--;const s=vt-st[i],p=s/SC[i].d;
 const X=.6,left=SC[i].d-s,n=SC.length;
 if(left<X){const t=ease(1-left/X);paint(ga,i,p,s);paint(gb,(i+1)%n,0,0);main.setTransform(1,0,0,1,0,0);main.fillStyle=c.bg;main.globalAlpha=1;main.fillRect(0,0,cv.width,cv.height);main.globalAlpha=1-t;main.drawImage(oa,0,0);main.globalAlpha=t;main.drawImage(ob,0,0);main.globalAlpha=1}else paint(main,i,p,s);
 const di=left<X/2?(i+1)%n:i;
 if(di!==cur){cur=di;document.querySelectorAll('#chs li').forEach((l,j)=>l.classList.toggle('on',j===di));document.querySelectorAll('#desc>div').forEach((l,j)=>l.classList.toggle('on',j===di))}
 $('#bar').style.width=(vt/tot*100)+'%';$('#tm').textContent=`${Math.floor(vt/60)}:${String(Math.floor(vt%60)).padStart(2,'0')}`;requestAnimationFrame(frame)}
const setP=b=>{playing=b;$('#pp').textContent=b?'Pause':'Play'};
$('#pp').onclick=()=>setP(!playing);cv.onclick=()=>setP(!playing);
$('#chs').onclick=e=>{const b=e.target.closest('button');if(b){vt=st[+b.dataset.i]+.01;setP(true)}};
$('#pb').onclick=e=>{const r=e.currentTarget.getBoundingClientRect();vt=Math.min(tot-.05,(e.clientX-r.left)/r.width*tot)};
const rsv=()=>{const d=Math.min(devicePixelRatio||1,2);cv.width=Math.round(cv.clientWidth*d);cv.height=Math.round(cv.width*.75);oa.width=ob.width=cv.width;oa.height=ob.height=cv.height;sc=cv.width/VW};
addEventListener('resize',rsv);rsv();cv.seek=t=>{vt=t};setP(false);
if(!rm)new IntersectionObserver(es=>{if(es[0].isIntersecting&&!started){started=true;setP(true)}},{threshold:.5}).observe(cv);
requestAnimationFrame(frame)})();
