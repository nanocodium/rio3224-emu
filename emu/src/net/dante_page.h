// Dante diagnostics page served by the panel HTTP server at /dante (data from /dante.json and /dante.log).
#pragma once

static const char* kDantePageHtml = R"HTML(<!doctype html><html><head><meta charset="utf-8"><title>Rio3224-D2 Dante diagnostics</title>
<style>
body{background:#1b1d20;color:#ddd;font-family:system-ui,sans-serif;margin:0;padding:16px}
.box{background:#2a2d31;border-radius:10px;padding:14px;margin-bottom:12px;box-shadow:0 2px 12px #000}
h2{margin:0 0 8px 0;font-size:16px;color:#9cf}
table{border-collapse:collapse;font-size:13px}td,th{padding:2px 10px 2px 0;text-align:left;vertical-align:top}th{color:#999;font-weight:normal}
.ok{color:#6f6}.bad{color:#f66}.warn{color:#fc6}
pre{background:#111;padding:8px;border-radius:6px;font-size:12px;max-height:340px;overflow:auto;white-space:pre-wrap}
small{color:#999}a{color:#9cf}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(360px,1fr));gap:12px}
</style></head><body>
<div class="box"><b>Rio3224-D2</b> <small>Controller-interoperable virtual Dante endpoint - live diagnostics</small> <small style="float:right"><a href="/">front panel</a></small>
<div id="head" style="margin-top:6px;font-size:13px"></div></div>
<div class="grid">
<div class="box"><h2>Identity &amp; network</h2><table id="ident"></table></div>
<div class="box"><h2>Clock (PTPv1)</h2><table id="clock"></table></div>
<div class="box"><h2>Discovery &amp; controller</h2><table id="disc"></table></div>
<div class="box"><h2>Other devices on the link</h2><table id="peers"></table></div>
</div>
<div class="grid">
<div class="box"><h2>Receive channels (subscriptions)</h2><table id="rx"></table></div>
<div class="box"><h2>Transmit channels</h2><div id="tx" style="font-size:13px"></div><h2 style="margin-top:12px">Transmit flows (to other receivers)</h2><table id="txf"></table><h2 style="margin-top:12px">Receive flows</h2><table id="rxf"></table></div>
</div>
<div class="box"><h2>Counters</h2><table id="cnt"></table></div>
<div class="box"><h2>Event log <small>(newest last)</small></h2><pre id="log"></pre></div>
<script>
function row(k,v,c){return '<tr><th>'+k+'</th><td class="'+(c||'')+'">'+v+'</td></tr>';}
function esc(s){return String(s??'').replace(/[&<>]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;'}[c]));}
async function tick(){try{const s=await (await fetch('/dante.json')).json();
 document.getElementById('head').innerHTML=(s.running?'<span class="ok">running</span>':'<span class="bad">stopped</span>')+' &nbsp; device <b>'+esc(s.name)+'</b> at '+esc(s.ip)+' &nbsp; '+s.fs+' Hz '+s.bits+' bit &nbsp; latency '+s.latencyMs+' ms &nbsp; '+(s.redundant?'REDUNDANT':'DAISY CHAIN (switched)')+' &nbsp; primary link '+(s.linkPrimary?'<span class="ok">up</span>':'<span class="bad">down</span>')+' &nbsp; secondary '+(s.ip2?(s.linkSecondary?'<span class="ok">up</span>':'<span class="bad">down</span>'):'<span class="warn">no interface</span>');
 let h='';h+=row('device name',esc(s.name));h+=row('factory name',esc(s.factoryName));h+=row('manufacturer / model',esc(s.manufacturer)+' / '+esc(s.model)+' <small>'+esc(s.modelId)+'</small>');h+=row('device id / MAC',s.deviceId+' / '+s.mac);
 h+=row('IPv4',esc(s.ip)+(s.ip2?' + '+esc(s.ip2)+' (secondary)':''));h+=row('ports',('ARC '+s.ports.arc+', CMC '+s.ports.cmc+', info '+s.ports.info+', flows '+s.ports.flows)+(s.standardPorts?'':' <span class="warn">(non-standard: the standard ports belong to another program on this host; advertised through DNS-SD)</span>'));
 h+=row('packet trace',s.trace?esc(s.trace):'off');document.getElementById('ident').innerHTML=h;
 const c=s.clock;h='';h+=row('PTP sockets',c.open?'<span class="ok">open (319/320, 224.0.1.129)</span>':'<span class="bad">not open</span>');h+=row('state','<b>'+c.state+'</b>'+(c.state==='master'?' (this device is the grandmaster)':c.state==='slave'?' (following '+c.master+')':''),c.state==='master'||c.locked?'ok':'warn');
 h+=row('locked',c.locked?'yes':'no',c.locked?'ok':'bad');h+=row('grandmaster',c.master+(c.preferred?' (preferred master)':''));h+=row('offset / path delay',c.offsetUs.toFixed(1)+' us / '+c.pathDelayUs.toFixed(1)+' us');h+=row('frequency correction',c.freqPpb.toFixed(0)+' ppb');
 h+=row('other masters heard',c.foreignMasters);h+=row('Sync rx / tx',c.syncRx+' / '+c.syncTx);h+=row('Delay_Req tx / Delay_Resp rx',c.delayReqTx+' / '+c.delayRespRx);h+=row('clock steps',c.steps);document.getElementById('clock').innerHTML=h;
 const d=s.discovery,k=s.controller;h='';h+=row('mDNS records published',d.records);h+=row('mDNS queries seen / answered',d.queriesSeen+' / '+d.answered);h+=row('last query answered',d.lastQuery?esc(d.lastQuery)+' <small>'+(d.lastQueryAgeMs/1000).toFixed(1)+' s ago</small>':'-');
 h+=row('controller requests (ARC/CMC/info)',k.queries,k.queries?'ok':'warn');h+=row('last controller request',k.last?esc(k.last)+' <small>'+(k.lastAgeMs/1000).toFixed(1)+' s ago</small>':'none yet');document.getElementById('disc').innerHTML=h;
 h='<tr><th>name</th><th>address</th><th>heard</th></tr>';for(const p of s.peers)h+='<tr><td>'+esc(p.name)+'</td><td>'+esc(p.ip)+'</td><td>'+(p.ageMs/1000).toFixed(0)+' s ago</td></tr>';if(!s.peers.length)h+='<tr><td colspan=3><small>none heard yet (devices announce every few minutes; a browse query goes out every 30 s)</small></td></tr>';document.getElementById('peers').innerHTML=h;
 h='<tr><th>#</th><th>label</th><th>subscribed to</th><th>status</th></tr>';s.rx.forEach((r,i)=>{const cls=r.status==='receiving'?'ok':r.status==='none'?'':r.status==='in progress'||r.status==='unresolved'?'warn':'bad';h+='<tr><td>'+(i+1)+'</td><td>'+esc(r.label)+'</td><td>'+esc(r.sub)+'</td><td class="'+cls+'">'+r.status+'</td></tr>';});document.getElementById('rx').innerHTML=h;
 document.getElementById('tx').innerHTML=s.tx.map((t,i)=>(i+1)+': '+esc(t)).join(' &nbsp;|&nbsp; ');
 h='<tr><th>#</th><th>receiver</th><th>destination</th><th>TX channels</th><th>fpp/bits</th><th>packets</th><th>keepalives</th></tr>';for(const f of s.txFlows)h+='<tr><td>'+f.id+'</td><td>'+esc(f.to)+'</td><td>'+esc(f.dst)+'</td><td>'+esc(f.channels)+'</td><td>'+f.fpp+'/'+f.bits+'</td><td>'+f.packets+'</td><td>'+f.keepalives+'</td></tr>';if(!s.txFlows.length)h+='<tr><td colspan=7><small>none</small></td></tr>';document.getElementById('txf').innerHTML=h;
 h='<tr><th>#</th><th>transmitter</th><th>control</th><th>our port</th><th>TX ids</th><th>state</th><th>packets</th><th>late</th><th>last audio</th></tr>';for(const f of s.rxFlows)h+='<tr><td>'+f.id+'</td><td>'+esc(f.from)+'</td><td>'+esc(f.src)+'</td><td>'+f.port+'</td><td>'+esc(f.txChannels)+'</td><td class="'+(f.established?'ok':'warn')+'">'+(f.established?'established':'requesting')+'</td><td>'+f.packets+'</td><td>'+f.late+'</td><td>'+(f.audioAgeMs<0?'-':(f.audioAgeMs/1000).toFixed(1)+' s ago')+'</td></tr>';if(!s.rxFlows.length)h+='<tr><td colspan=9><small>none</small></td></tr>';document.getElementById('rxf').innerHTML=h;
 const n=s.counters;h='';h+=row('audio packets sent / received',n.txPackets+' / '+n.rxPackets);h+=row('late packets',n.rxLate,n.rxLate?'warn':'');h+=row('media clock re-anchors',n.resyncs,n.resyncs>1?'warn':'');h+=row('protocol errors / unknown requests',n.errors,n.errors?'warn':'');h+=row('engine blocks tx / rx',n.txBlocks+' / '+n.rxBlocks);h+=row('mDNS packets tx / rx',n.mdnsTx+' / '+n.mdnsRx);document.getElementById('cnt').innerHTML=h;
 const lg=await (await fetch('/dante.log')).text();const pre=document.getElementById('log');const atEnd=pre.scrollTop+pre.clientHeight>=pre.scrollHeight-20;pre.textContent=lg;if(atEnd)pre.scrollTop=pre.scrollHeight;
}catch(e){document.getElementById('head').innerHTML='<span class="bad">endpoint not running (start the emulator with --dante-ctl IFADDR)</span>';}}
setInterval(tick,1000);tick();
</script></body></html>)HTML";
