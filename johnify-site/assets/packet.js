(()=>{const $=s=>document.querySelector(s),rm=matchMedia('(prefers-reduced-motion:reduce)').matches;
const F=[['magic',0,2,0],['hops',2,1,0],['gid',3,8,0],['msgId',11,4,0],['devId',15,4,0],['nonce',19,12,1],['len',31,1,0],['ciphertext (name + text)',32,'12 + len',2],['tag','44 + len',16,3]];
const rnd=s=>{let h=2166136261;for(const c of s){h^=c.charCodeAt(0);h=Math.imul(h,16777619)}return()=>{h^=h<<13;h^=h>>>17;h^=h<<5;return(h>>>0)&255}};
const hx=a=>a.map(b=>b.toString(16).padStart(2,'0')).join(' ');
$('#tb').innerHTML=F.map((f,i)=>`<tr data-i="${i}"><td data-l="Offset">${f[1]}</td><td data-l="Size">${f[2]}</td><td data-l="Field">${f[0]}</td></tr>`).join('');
let fl;function render(sc){const v=$('#msg').value,len=new TextEncoder().encode(v).length,r=rnd(v+(sc?Math.random():'k')),g=n=>Array.from({length:n},r);
const d=[[0x4a,0x46],[0],g(8),g(4),g(4),g(12),[len],g(12+len),g(16)];
$('#pk').innerHTML=d.map((a,i)=>`<div class="k${F[i][3]}" style="flex-grow:${a.length}" title="${F[i][0]}"></div>`).join('');
$('#hex').innerHTML=d.map((a,i)=>`<em class="k${F[i][3]}" data-i="${i}">${hx(a)}</em>`).join('');
$('#tot').innerHTML=`<b>${60+len}</b> of 250 bytes (60 + ${len} bytes of text)`}
$('#msg').addEventListener('input',()=>{if(rm)return render(0);let k=0;clearInterval(fl);fl=setInterval(()=>{render(1);if(++k>6){clearInterval(fl);render(0)}},45)});render(0);
const hl=e=>{const t=e.target.closest('[data-i]');document.querySelectorAll('.on').forEach(x=>x.classList.remove('on'));if(t)document.querySelectorAll(`[data-i="${t.dataset.i}"]`).forEach(x=>x.classList.add('on'))};
document.addEventListener('mouseover',hl);document.addEventListener('click',hl)})();
