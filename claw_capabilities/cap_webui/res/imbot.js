// IMBOT
var wxPollIv=null;
function drawWxQr(url){
  try{
    var qr=qrcode(0,'M');qr.addData(url,'Byte');qr.make();
    var canvas=document.getElementById('wx-qr-canvas');
    var ctx=canvas.getContext('2d');
    var n=qr.getModuleCount(),cell=Math.floor(200/n),off=Math.floor((200-n*cell)/2);
    ctx.fillStyle='#fff';ctx.fillRect(0,0,200,200);
    ctx.fillStyle='#000';
    for(var r=0;r<n;r++)for(var c=0;c<n;c++)if(qr.isDark(r,c))ctx.fillRect(off+c*cell,off+r*cell,cell,cell);
    document.getElementById('wx-qr-area').style.display='block';
  }catch(e){console.error('QR:',e);}
}
function toggleIm(id){
  var body=document.getElementById(id+'-body');
  var arrow=document.getElementById(id+'-arrow');
  var open=body.style.display!=='none';
  body.style.display=open?'none':'block';
  arrow.classList.toggle('open',!open);
}
function initImbot(){
  withConfig(function(d){
    document.getElementById('tg-tok').value=(d.telegram&&d.telegram.bot_token)||'';
    document.getElementById('fs-id').value=(d.feishu&&d.feishu.app_id)||'';
    document.getElementById('fs-sec').value=(d.feishu&&d.feishu.app_secret)||'';
    document.getElementById('wx-url').value=(d.wechat&&d.wechat.base_url)||'';
    var fsConf=!!(d.feishu&&d.feishu.app_id);
    var fsB=document.getElementById('fs-grp-badge');
    if(fsB){fsB.className='bk '+(fsConf?'ok':'of');fsB.innerText=T(fsConf?'cfg':'nocfg');}
    var tgConf=!!(d.telegram&&d.telegram.bot_token);
    var tgB=document.getElementById('tg-grp-badge');
    if(tgB){tgB.className='bk '+(tgConf?'ok':'of');tgB.innerText=T(tgConf?'cfg':'nocfg');}
    document.getElementById('qq-id').value=(d.qq&&d.qq.app_id)||'';
    document.getElementById('qq-sec').value=(d.qq&&d.qq.app_secret)||'';
    var qqConf=!!(d.qq&&d.qq.app_id);
    var qqB=document.getElementById('qq-grp-badge');
    if(qqB){qqB.className='bk '+(qqConf?'ok':'of');qqB.innerText=T(qqConf?'cfg':'nocfg');}
  });
  fetch('/api/wechat/status').then(function(r){return r.json();}).then(renderWxStatus).catch(function(){});
  fetch('/api/wechat/token').then(function(r){return r.json();}).then(function(d){
    if(d.ok&&d.token)document.getElementById('wx-token').value=d.token;
  }).catch(function(){});
}
function renderWxStatus(d){
  var st=document.getElementById('wx-st');
  var qa=document.getElementById('wx-qr-area');
  var gb=document.getElementById('wx-grp-badge');
  if(!d||!d.state){return;}
  if(d.state==='polling'){
    st.innerHTML='<span class="bk ok">'+T('wx_logged')+'</span>';
    if(gb){gb.className='bk ok';gb.innerText=T('wx_logged');}
    qa.style.display='none';
    if(wxPollIv){clearInterval(wxPollIv);wxPollIv=null;}
    fetch('/api/wechat/token').then(function(r){return r.json();}).then(function(td){
      if(td.ok&&td.token)document.getElementById('wx-token').value=td.token;
    }).catch(function(){});
  } else if(d.state==='qr_pending'){
    st.innerHTML='<span class="bk wn">'+T('wx_pending')+'</span>';
    if(gb){gb.className='bk wn';gb.innerText=T('wx_pending');}
    if(d.qr_url){drawWxQr(d.qr_url);}
    if(!wxPollIv)wxPollIv=setInterval(function(){
      fetch('/api/wechat/status').then(function(r){return r.json();}).then(renderWxStatus).catch(function(){});
    },2000);
  } else if(d.state==='error'){
    st.innerHTML='<span class="bk er">'+T('wx_error')+'</span>';
    if(gb){gb.className='bk er';gb.innerText=T('wx_error');}
    qa.style.display='none';
    if(wxPollIv){clearInterval(wxPollIv);wxPollIv=null;}
  } else {
    st.innerHTML='<span class="bk of">'+T('wx_idle')+'</span>';
    if(gb){gb.className='bk of';gb.innerText=T('wx_idle');}
    qa.style.display='none';
  }
}
function genWxQr(){
  msg('wx-m',true,T('saving'));
  fetch('/api/wechat/qrcode').then(function(r){return r.json();})
  .then(function(d){
    document.getElementById('wx-m').style.display='none';
    if(d.ok){
      drawWxQr(d.qr_url);
      renderWxStatus({state:'qr_pending',qr_url:d.qr_url});
    } else if(d.state==='polling'){
      renderWxStatus({state:'polling'});
    } else {
      msg('wx-m',false,T('sf')+(d.error||''));
    }
  }).catch(function(e){msg('wx-m',false,T('rf')+e);});
}
function saveWechat(){
  msg('wx-m',true,T('saving'));
  fetch('/setup',{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify({section:'wechat',base_url:document.getElementById('wx-url').value,bot_token:document.getElementById('wx-token').value})})
  .then(function(r){return r.json();})
  .then(function(d){if(d.ok){cfgCache=null;msg('wx-m',true,T('saved'));}else msg('wx-m',false,T('sf')+(d.error||''));})
  .catch(function(e){msg('wx-m',false,T('rf')+e);});
}
function saveFeishu(){
  msg('fs-m',true,T('saving'));
  var appId=document.getElementById('fs-id').value;
  fetch('/setup',{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify({section:'feishu',app_id:appId,app_secret:document.getElementById('fs-sec').value})})
  .then(function(r){return r.json();})
  .then(function(d){
    if(d.ok){
      cfgCache=null;msg('fs-m',true,T('saved'));
      var fsB=document.getElementById('fs-grp-badge');
      if(fsB){fsB.className='bk '+(appId?'ok':'of');fsB.innerText=T(appId?'cfg':'nocfg');}
    }else msg('fs-m',false,T('sf')+(d.error||''));
  }).catch(function(e){msg('fs-m',false,T('rf')+e);});
}
function saveTelegram(){
  msg('tg-m',true,T('saving'));
  var tok=document.getElementById('tg-tok').value;
  fetch('/setup',{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify({section:'telegram',bot_token:tok})})
  .then(function(r){return r.json();})
  .then(function(d){
    if(d.ok){
      cfgCache=null;msg('tg-m',true,T('saved'));
      var tgB=document.getElementById('tg-grp-badge');
      if(tgB){tgB.className='bk '+(tok?'ok':'of');tgB.innerText=T(tok?'cfg':'nocfg');}
    }else msg('tg-m',false,T('sf')+(d.error||''));
  }).catch(function(e){msg('tg-m',false,T('rf')+e);});
}
function saveQQ(){
  msg('qq-m',true,T('saving'));
  var id=document.getElementById('qq-id').value;
  var sec=document.getElementById('qq-sec').value;
  fetch('/setup',{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify({section:'qq',app_id:id,app_secret:sec})})
  .then(function(r){return r.json();})
  .then(function(d){
    if(d.ok){
      cfgCache=null;msg('qq-m',true,T('saved'));
      var qqB=document.getElementById('qq-grp-badge');
      if(qqB){qqB.className='bk '+(id?'ok':'of');qqB.innerText=T(id?'cfg':'nocfg');}
    }else msg('qq-m',false,T('sf')+(d.error||''));
  }).catch(function(e){msg('qq-m',false,T('rf')+e);});
}
