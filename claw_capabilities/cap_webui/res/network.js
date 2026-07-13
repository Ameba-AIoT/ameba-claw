// Network
function initNetwork(){
  withConfig(function(d){
    var w=d.wifi||{};
    var h='<div class=si style="margin-bottom:12px"><div class=sl>'+T('col_status')+'</div><div class=sv><span class="bk '+(w.configured?'ok':'wn')+'">'+(w.configured?T('cfg'):T('nocfg'))+'</span></div>'+(w.configured&&w.ssid?'<div class=ss>'+esc(w.ssid)+(w.security_type?' ['+esc(w.security_type)+']':'')+'</div>':'')+'</div>';
    document.getElementById('wc-st').innerHTML=h;
    if(w.ssid)document.getElementById('wc-s').value=w.ssid;
    if(w.password)document.getElementById('wc-p').value=w.password;
  });
}
function saveWifi(){
  var s=document.getElementById('wc-s').value.trim(),p=document.getElementById('wc-p').value;
  if(!s){msg('wc-m',false,T('wifi_empty'));return;}
  msg('wc-m',true,T('wifi_connecting_req'));
  fetch('/api/wifi/connect',{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify({ssid:s,password:p})})
  .then(function(r){return r.json();})
  .then(function(d){
    if(d.ok&&d.connecting){cfgCache=null;showPollingStatus(s);setTimeout(function(){pollWifiStatus(0);},3000);}
    else if(d.ok&&d.ip&&d.ip!=='0.0.0.0'){cfgCache=null;showConnectedBanner(d.ip);}
    else{msg('wc-m',false,T('wifi_fail')+(d.error||''));}
  })
  .catch(function(){
    cfgCache=null;showPollingStatus(s);setTimeout(function(){pollWifiStatus(0);},3000);
  });
}
function showPollingStatus(ssid){
  pollConsecFail=0;
  var m=document.getElementById('wc-m');
  m.style.display='block';m.className='';
  m.innerHTML='<div style="padding:10px;border:1px solid #91d5ff;border-radius:6px;background:#e6f7ff;">'
    +'<div style="color:#1890ff;font-weight:bold;">'+T('wifi_connecting_to').replace('{ssid}',esc(ssid))+'</div>'
    +'<div id="wc-poll-tip" style="margin-top:6px;color:#888;font-size:12px;">'+T('wifi_prov_tip')+'</div>'
    +'</div>';
}
function pollWifiStatus(attempt){
  if(attempt>60){var m=document.getElementById('wc-m');m.style.display='block';m.className='';m.innerHTML='<div style="padding:10px;">'
    +T('wifi_timeout')+'</div>';return;}
  fetch('/status',{signal:AbortSignal.timeout(3000)})
  .then(function(r){return r.json();})
  .then(function(d){
    pollConsecFail=0;
    if(d.wifi&&d.wifi.connected&&d.wifi.ip&&d.wifi.ip!=='0.0.0.0'){showConnectedBanner(d.wifi.ip);}
    else if(d.wifi&&d.wifi.connect_error&&d.wifi.connect_error!==''){
      msg('wc-m',false,T('wifi_fail')+d.wifi.connect_error);
    }
    else{var t=document.getElementById('wc-poll-tip');if(t)t.textContent=T('wifi_waiting')+' ('+attempt+'/60)';setTimeout(function(){pollWifiStatus(attempt+1);},2000);}
  })
  .catch(function(){
    pollConsecFail++;
    if(pollConsecFail>=3){
      var m=document.getElementById('wc-m');m.style.display='block';m.className='';
      m.innerHTML='<div style="background:#fff7e6;border:1px solid #ffa940;border-radius:6px;padding:12px 16px;color:#d46b08;">'+T('softap_disconn')+'</div>';
      return;
    }
    var t=document.getElementById('wc-poll-tip');if(t)t.textContent=T('wifi_switching')+' ('+attempt+'/60)';
    setTimeout(function(){pollWifiStatus(attempt+1);},2000);
  });
}
function showConnectedBanner(ip){
  var url='http://'+ip;
  var m=document.getElementById('wc-m');
  m.style.display='block';
  m.className='';
  m.innerHTML='<div style="background:#e6f7e6;border:1px solid #52c41a;border-radius:6px;padding:12px 16px;">'
    +'<div style="font-weight:bold;color:#389e0d;font-size:15px;">'+T('wifi_connected_title')+'</div>'
    +'<div style="margin:6px 0;color:#333;">'+T('wifi_ip_label')+'<b>'+ip+'</b></div>'
    +'<a href="'+url+'" style="display:inline-block;margin-top:4px;padding:6px 16px;background:#1890ff;color:#fff;border-radius:4px;text-decoration:none;font-size:14px;">'+T('wifi_open_mgmt')+url+'</a>'
    +'<div style="margin-top:8px;color:#888;font-size:12px;">'+T('wifi_access_hint')+'</div>'
    +'</div>';
}
