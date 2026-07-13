// Status
function fetchStatus(){
  fetch('/status').then(function(r){return r.json();})
  .then(function(d){
    var wc=d.wifi&&d.wifi.connected,wcfg=d.wifi&&d.wifi.configured,ap=d.softap&&d.softap.running;
    lastWifi={wc:wc,ap:ap,ip:(d.wifi&&d.wifi.ip)||'',apip:(d.softap&&d.softap.ip)||''};
    syncConnDot();
    var h='';
    h+='<div class=si><div class=sl>'+T('s_wifi')+'</div><div class=sv><span class="bk '+(wcfg?'ok':'wn')+'">'+(wcfg?T('cfg'):T('nocfg'))+'</span></div>'+(wc?'<div class=ss>'+esc(d.wifi.ssid||'')+' '+esc(d.wifi.ip||'')+'</div>':'')+'</div>';
    h+='<div class=si><div class=sl>'+T('s_ap')+'</div><div class=sv><span class="bk '+(ap?'ok':'of')+'">'+(ap?T('running'):T('stopped'))+'</span></div>'+(ap?'<div class=ss>'+esc(d.softap.ssid||'')+' '+esc(d.softap.ip||'')+'</div>':'')+'</div>';
    h+='<div class=si><div class=sl>'+T('s_mode')+'</div><div class=sv>'+esc(d.mode||'-')+'</div></div>';
    h+='<div class=si><div class=sl>'+T('s_ver')+'</div><div class=sv>'+esc(d.version||'-')+'</div></div>';
    if(d.heap){h+='<div class=si><div class=sl>'+T('s_heap')+'</div><div class=sv>'+Math.round(d.heap.free_bytes/1024)+'KB</div><div class=ss>'+T('s_low')+' '+Math.round(d.heap.min_ever_bytes/1024)+'KB</div></div>';}
    document.getElementById('sg').innerHTML=h;
  });
}
var _savedTab=localStorage.getItem('claw_tab');
if(_savedTab&&document.getElementById('p-'+_savedTab))show(_savedTab);
fetchStatus();
// Pre-warm config cache so first tab switch is instant
fetch('/api/config').then(function(r){return r.json();}).then(function(d){cfgCache=d;});

// Restart
function performRestart(){
  var m=document.getElementById('rst-m');show('status');
  m.style.display='block';m.className='mok';m.innerText=T('saving');
  fetch('/api/system/restart',{method:'POST'}).then(function(r){return r.json();})
  .then(function(d){
    if(!d.ok){m.className='mer';m.innerText=T('sf')+(d.error||'');return;}
    var n=0,iv=setInterval(function(){
      n++;m.innerText='重启中… ('+n+'s)';
      if(n>=30){clearInterval(iv);m.innerText='重启超时，请手动刷新';return;}
      fetch('/status',{cache:'no-store'}).then(function(r){if(r.ok){clearInterval(iv);location.reload();}}).catch(function(){});
    },1000);
  }).catch(function(e){m.className='mer';m.innerText=T('rf')+e;});
}
function doRestart(){if(!confirm(T('restart')+'?'))return;performRestart();}
