// Session Manager
function loadSessionMgr(){
  var el=document.getElementById('sm-list');
  if(!el)return;
  el.innerHTML='<div style="color:#94a3b8;padding:12px">'+T('ws_connecting')+'...</div>';
  fetch('/api/sessions')
    .then(function(r){if(!r.ok)throw new Error(r.statusText);return r.json();})
    .then(function(data){
      var channels=data.channels||[];
      if(!channels.length){
        el.innerHTML='<div class="card"><p class="nl" data-k="session_empty">'+T('session_empty')+'</p></div>';
        return;
      }
      renderSessionMgr(channels);
    })
    .catch(function(){
      el.innerHTML='<div class="card"><p style="color:#ef4444">'+T('session_load_fail')+'</p></div>';
    });
}
function renderSessionMgr(channels){
  var el=document.getElementById('sm-list');
  if(!el)return;
  if(!channels.length){el.innerHTML='<div class="card"><p class="nl" data-k="session_empty">'+T('session_empty')+'</p></div>';return;}
  var html='';
  for(var i=0;i<channels.length;i++){
    var ch=channels[i];
    var chatKey=ch.chat_key||'';
    var current=ch.current||'';
    var sessions=ch.sessions||[];
    html+='<div class="card" style="margin-bottom:16px">';
    html+='<h2 style="margin:0 0 10px">'+esc(chatKey)+'</h2>';
    html+='<table class="tbl"><thead><tr><th>Alias</th><th></th><th class="nl" data-k="col_action">操作</th></tr></thead><tbody>';
    for(var j=0;j<sessions.length;j++){
      var alias=sessions[j];
      var isCur=(alias===current);
      var sessionId=chatKey+':'+alias;
      html+='<tr>';
      html+='<td>'+esc(alias)+'</td>';
      html+='<td>'+(isCur?'<span style="color:#3b82f6;font-size:12px" class="nl" data-k="session_current">'+T('session_current')+'</span>':'')+'</td>';
      html+='<td style="white-space:nowrap">'
        +'<button class="btn btn-secondary btn-sm nl" data-k="session_history" '
        +'onclick="smViewHistory(\''+esc(sessionId)+'\')">'+T('session_history')+'</button> '
        +'<button class="btn btn-danger btn-sm nl" data-k="del" '
        +'onclick="smDelete(\''+esc(chatKey)+'\',\''+esc(alias)+'\')">'+T('del')+'</button>'
        +'</td>';
      html+='</tr>';
    }
    html+='</tbody></table></div>';
  }
  el.innerHTML=html;
}
function smViewHistory(sessionId){
  var mb=document.getElementById('sm-modal-body');
  var mt=document.getElementById('sm-modal-title');
  if(mt)mt.textContent=T('session_history')+': '+sessionId;
  if(mb)mb.innerHTML='<div style="color:#94a3b8">'+T('ws_connecting')+'...</div>';
  smOpenModal();
  fetch('/api/session/history?session_id='+encodeURIComponent(sessionId))
    .then(function(r){if(!r.ok)throw new Error(r.statusText);return r.json();})
    .then(function(d){
      var turns=d.turns||[];
      if(!turns.length){if(mb)mb.innerHTML='<p style="color:#94a3b8" class="nl" data-k="session_empty">'+T('session_empty')+'</p>';return;}
      var h='';
      for(var i=0;i<turns.length;i++){
        var t=turns[i];
        var isUser=(t.role==='user');
        h+='<div style="margin-bottom:8px;padding-bottom:8px;border-bottom:1px solid #e2e8f0">';
        h+='<div><b>'+(isUser?T('msg_user'):T('msg_assistant'))+':</b> '+esc(t.text||'')+'</div>';
        h+='</div>';
      }
      if(mb)mb.innerHTML=h;
    })
    .catch(function(){if(mb)mb.innerHTML='<p style="color:#ef4444">'+T('session_load_fail')+'</p>';});
}
function smDelete(chatKey,alias){
  if(!confirm(T('session_del_confirm')+' '+alias+'?'))return;
  var sep=chatKey.indexOf(':');
  var channel=sep>=0?chatKey.slice(0,sep):chatKey;
  var chatId=sep>=0?chatKey.slice(sep+1):'local';
  var url='/api/session?alias='+encodeURIComponent(alias)
         +'&channel='+encodeURIComponent(channel)
         +'&chat_id='+encodeURIComponent(chatId);
  fetch(url,{method:'DELETE'})
    .then(function(r){if(!r.ok)throw new Error(r.statusText);return r.json();})
    .then(function(d){
      if(d.ok)loadSessionMgr();
      else alert(T('session_del_fail'));
    })
    .catch(function(){alert(T('session_del_fail'));});
}
function smOpenModal(){document.getElementById('sm-modal').style.display='flex';}
function smCloseModal(){document.getElementById('sm-modal').style.display='none';}
