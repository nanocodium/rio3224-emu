// Live front/rear panel view over HTTP (--panel-http PORT): a single-page UI (served from this header) shows the two
// OLED panels, the +48V/SIG/PEAK LEDs, the status LEDs, the engine meters and lets the user press DEC / INC / the
// encoder, turn the encoder, flip the +48V MASTER switch, fail a PSU or pull the power.  Everything is polled from the
// emulator thread (non-blocking sockets), so no locking is needed; the machine supplies the state and executes actions.
#pragma once
#include "../platform.h"
#include <functional>
#include <map>
#include <string>
#include <vector>

class PanelServer {
public:
    std::function<std::string()> stateJson;                                   // GET /state
    std::function<std::string()> oledPgm;                                     // GET /oled.pgm (binary PGM, both panels)
    std::function<void(const std::string& path, const std::map<std::string, std::string>& q)> action;   // /press /rotate /master /psu /poweroff
    std::function<bool(const std::string& path, const std::map<std::string, std::string>& q, std::string& type, std::string& body)> extraRoute;   // other pages (e.g. /dante)

    bool open(uint16_t port, std::string& err) {
        platformInit();
        listenFd = (int)socket(AF_INET, SOCK_STREAM, 0);
        if (listenFd < 0) { err = sockError(); return false; }
        int one = 1; setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof one);
        sockaddr_in sa{}; sa.sin_family = AF_INET; sa.sin_port = htons(port); sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(listenFd, (sockaddr*)&sa, sizeof sa) < 0 || listen(listenFd, 8) < 0) { err = std::string("port ") + std::to_string(port) + ": " + sockError(); sockClose(listenFd); listenFd = -1; return false; }
        sockNonBlocking(listenFd); this->port = port; return true;
    }
    ~PanelServer() { if (listenFd >= 0) sockClose(listenFd); for (auto& c : conns) sockClose(c.fd); }
    uint16_t port = 0;

    void poll() {
        if (listenFd < 0) return;
        for (int k = 0; k < 8; k++) { int c = (int)accept(listenFd, nullptr, nullptr); if (c < 0) break; sockNonBlocking(c); conns.push_back({ c, "" }); }
        for (size_t i = 0; i < conns.size();) {
            Conn& c = conns[i]; char buf[2048];
            ssize_t r = recv(c.fd, buf, sizeof buf, 0);
            if (r > 0) c.req.append(buf, (size_t)r);
            bool done = false;
            if (c.req.find("\r\n\r\n") != std::string::npos) { respond(c); done = true; }
            else if (r == 0 || (r < 0 && !sockWouldBlock()) || c.req.size() > 65536) done = true;
            if (done) { sockClose(c.fd); conns.erase(conns.begin() + i); } else i++;
        }
    }

private:
    struct Conn { int fd; std::string req; };
    int listenFd = -1; std::vector<Conn> conns;

    static void sendAll(int fd, const std::string& s) { size_t off = 0; int spins = 0; while (off < s.size() && spins < 2000) { ssize_t w = send(fd, s.data() + off, (int)(s.size() - off), MSG_NOSIGNAL); if (w > 0) off += (size_t)w; else if (w < 0 && sockWouldBlock()) { spins++; } else break; } }
    static std::string http(const std::string& type, const std::string& body, int code = 200) {
        return "HTTP/1.1 " + std::to_string(code) + (code == 200 ? " OK" : " Not Found") + "\r\nContent-Type: " + type + "\r\nContent-Length: " + std::to_string(body.size()) + "\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n" + body;
    }
    void respond(Conn& c) {
        size_t sp = c.req.find(' '), sp2 = c.req.find(' ', sp + 1); if (sp == std::string::npos || sp2 == std::string::npos) { sendAll(c.fd, http("text/plain", "bad request", 404)); return; }
        std::string url = c.req.substr(sp + 1, sp2 - sp - 1), path = url, qs; size_t q = url.find('?'); if (q != std::string::npos) { path = url.substr(0, q); qs = url.substr(q + 1); }
        std::map<std::string, std::string> query; std::string cur;
        auto put = [&](const std::string& kv) { size_t e = kv.find('='); if (e == std::string::npos) query[kv] = ""; else query[kv.substr(0, e)] = kv.substr(e + 1); };
        for (char ch : qs) { if (ch == '&') { put(cur); cur.clear(); } else cur += ch; } if (!cur.empty()) put(cur);
        if (path == "/") sendAll(c.fd, http("text/html; charset=utf-8", page()));
        else if (path == "/state") sendAll(c.fd, http("application/json", stateJson ? stateJson() : "{}"));
        else if (path == "/oled.pgm") sendAll(c.fd, http("application/octet-stream", oledPgm ? oledPgm() : ""));
        else if (action && (path == "/press" || path == "/rotate" || path == "/master" || path == "/psu" || path == "/poweroff")) { action(path, query); sendAll(c.fd, http("application/json", "{\"ok\":true}")); }
        else { std::string type, body; if (extraRoute && extraRoute(path, query, type, body)) sendAll(c.fd, http(type, body)); else sendAll(c.fd, http("text/plain", "not found", 404)); }
    }

    static std::string page() {
        return R"HTML(<!doctype html><html><head><meta charset="utf-8"><title>Rio3224-D2 panel</title>
<style>
body{background:#1b1d20;color:#ddd;font-family:system-ui,sans-serif;margin:0;padding:16px}
.unit{background:#2a2d31;border-radius:10px;padding:14px;max-width:1240px;box-shadow:0 2px 12px #000}
canvas{background:#000;border:2px solid #111;border-radius:4px;image-rendering:pixelated;width:100%;max-width:1200px;height:auto}
.row{display:flex;gap:18px;align-items:flex-start;flex-wrap:wrap;margin-top:12px}
.leds{display:grid;grid-template-columns:auto repeat(32,16px);gap:3px 2px;align-items:center;font-size:11px}
.led{width:12px;height:12px;border-radius:50%;background:#333;display:inline-block;border:1px solid #111}
.led.on48{background:#f33;box-shadow:0 0 6px #f33}.led.onsig{background:#3f3;box-shadow:0 0 6px #3f3}.led.onpk{background:#f33;box-shadow:0 0 6px #f33}
.led.onst{background:#3f3;box-shadow:0 0 6px #3f3}
button{background:#444;color:#eee;border:1px solid #666;border-radius:6px;padding:8px 14px;font-size:14px;cursor:pointer}
button:active{background:#777}
.knob{width:70px;height:70px;border-radius:50%;background:radial-gradient(#666,#222);border:2px solid #888;display:inline-block;text-align:center;line-height:70px;cursor:ns-resize;user-select:none}
.meters{display:flex;gap:2px;align-items:flex-end;height:90px;background:#111;padding:4px;border-radius:4px}
.bar{width:9px;background:#3a3;transition:height .08s}
.bar.hot{background:#c33}
small{color:#999}
#status{margin-top:8px;font-size:13px;color:#bbb}
.rear{background:#333;border-radius:8px;padding:10px;margin-top:12px}
label{margin-right:14px}
</style></head><body>
<div class="unit">
 <div><b>Rio3224-D2</b> <small id="title">front panel (live)</small> <small style="float:right"><a href="/dante" style="color:#9cf">Dante diagnostics</a></small></div>
 <canvas id="oled" width="1200" height="96"></canvas>
 <div class="row">
  <div>
   <div><button onclick="press(3)">&#9664; DEC</button> <button onclick="press(4)">INC &#9654;</button> <button onclick="both()">DEC+INC (next menu)</button> <button onclick="hold()">hold both 2 s (back)</button></div>
   <div style="margin-top:10px"><span class="knob" id="knob" title="wheel = turn, click = push">push</span>
     <button onclick="rot(-1)">&#8630; -1</button> <button onclick="rot(1)">+1 &#8631;</button> <button onclick="rot(-10)">-10</button> <button onclick="rot(10)">+10</button></div>
  </div>
  <div class="leds" id="leds"></div>
 </div>
 <div class="row"><div><small>input meters (engine, dBFS)</small><div class="meters" id="min"></div></div><div><small>outputs 1-16 / AES 1-8</small><div class="meters" id="mout"></div></div></div>
 <div class="rear"><b>rear</b>&nbsp; <label><input type="checkbox" id="m48" onchange="g('/master?on='+(this.checked?1:0))"> +48V MASTER</label>
   <label><input type="checkbox" id="psuA" checked onchange="g('/psu?a='+(this.checked?1:0))"> PSU A</label>
   <label><input type="checkbox" id="psuB" checked onchange="g('/psu?b='+(this.checked?1:0))"> PSU B</label>
   <button onclick="if(confirm('Cut the mains? The firmware saves its settings and the unit powers down.'))g('/poweroff')">POWER OFF</button></div>
 <div id="status"></div>
</div>
<script>
const g=u=>fetch(u).catch(()=>{});
function press(sw,ms){return g('/press?sw='+sw+'&ms='+(ms||150));}
function both(){g('/press?sw=3&ms=150');g('/press?sw=4&ms=150');}
function hold(){g('/press?sw=3&ms=2200');g('/press?sw=4&ms=2200');}
function rot(n){g('/rotate?steps='+n);}
const knob=document.getElementById('knob');knob.onclick=()=>press(5);knob.onwheel=e=>{e.preventDefault();rot(e.deltaY<0?1:-1);};
const names=['+48V','SIG','PEAK'];const ledsDiv=document.getElementById('leds');
function buildLeds(){let h='';for(let r=0;r<3;r++){h+='<span>'+names[r]+'</span>';for(let i=0;i<32;i++)h+='<span class="led" id="l'+r+'_'+i+'"></span>';}
 h+='<span>SIG out</span>';for(let i=0;i<16;i++)h+='<span class="led" id="lo_'+i+'"></span>';for(let i=0;i<16;i++)h+='<span></span>';
 h+='<span>SIG AES</span>';for(let i=0;i<8;i++)h+='<span class="led" id="la_'+i+'"></span>';for(let i=0;i<24;i++)h+='<span></span>';
 h+='<span>status</span>';['PRI','ACT','SEC','SYNC'].forEach((n,i)=>{h+='<span class="led" id="ls_'+i+'" title="'+n+'"></span>'});for(let i=0;i<28;i++)h+='<span></span>';ledsDiv.innerHTML=h;}
buildLeds();
function bars(id,vals,n){const d=document.getElementById(id);if(d.children.length!==n){d.innerHTML='';for(let i=0;i<n;i++){const b=document.createElement('div');b.className='bar';d.appendChild(b);}}
 for(let i=0;i<n;i++){const v=vals[i]??-200;const h=Math.max(0,Math.min(1,(v+60)/60));d.children[i].style.height=(h*82+1)+'px';d.children[i].className='bar'+(v>-3?' hot':'');}}
async function drawOled(){try{const r=await fetch('/oled.pgm');const b=new Uint8Array(await r.arrayBuffer());let p=0,tok=[];while(tok.length<4&&p<b.length){let s='';while(p<b.length&&b[p]>32){s+=String.fromCharCode(b[p++]);}p++;if(s)tok.push(s);}
 const w=+tok[1],h=+tok[2];const c=document.getElementById('oled');const ctx=c.getContext('2d');const img=ctx.createImageData(w,h);for(let i=0;i<w*h;i++){const v=b[p+i];img.data[i*4]=v*0.85;img.data[i*4+1]=v;img.data[i*4+2]=v*0.9;img.data[i*4+3]=255;}
 const off=document.createElement('canvas');off.width=w;off.height=h;off.getContext('2d').putImageData(img,0,0);ctx.imageSmoothingEnabled=false;ctx.clearRect(0,0,c.width,c.height);ctx.drawImage(off,0,0,c.width,c.height);}catch(e){}}
async function tick(){try{const s=await (await fetch('/state')).json();
 for(let i=0;i<32;i++){document.getElementById('l0_'+i).className='led'+(s.leds.p48[i]===1?' on48':'');document.getElementById('l1_'+i).className='led'+(s.leds.sig[i]===1?' onsig':'');document.getElementById('l2_'+i).className='led'+(s.leds.peak[i]===1?' onpk':'');}
 for(let i=0;i<16;i++)document.getElementById('lo_'+i).className='led'+(s.leds.sigOut[i]===1?' onsig':'');for(let i=0;i<8;i++)document.getElementById('la_'+i).className='led'+(s.leds.sigAes[i]===1?' onsig':'');
 for(let i=0;i<4;i++)document.getElementById('ls_'+i).className='led'+(s.leds.status[i]===1?' onst':'');
 bars('min',s.meters.in,32);bars('mout',s.meters.out,24);
 document.getElementById('m48').checked=!!s.rear.master48;document.getElementById('psuA').checked=!s.rear.psuAFail;document.getElementById('psuB').checked=!s.rear.psuBFail;
 document.getElementById('status').textContent='t = '+(s.ms/1000).toFixed(1)+' s | '+(s.realtime?'real time':'free running')+' | Dante module '+(s.danteRunning?'up':'down')+' | word clock '+(s.clockLocked?'locked':'unlocked')+' | fs '+s.fs+' Hz | audio '+(s.audio?'on':'off')+(s.powerLost?' | POWER LOST':'');
 }catch(e){}}
setInterval(tick,120);setInterval(drawOled,150);tick();drawOled();
</script></body></html>)HTML";
    }
};
