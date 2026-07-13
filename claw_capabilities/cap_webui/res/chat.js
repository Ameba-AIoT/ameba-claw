// Chat
var cfgCache=null;
function withConfig(cb){
  if(cfgCache){cb(cfgCache);return;}
  fetch('/api/config').then(function(r){return r.json();})
  .then(function(d){cfgCache=d;cb(d);}).catch(function(){cb({});});
}
var lastId=0;
var currentAlias=sessionStorage.getItem('claw_alias')||'';
function addBubble(role,text){
  var box=document.getElementById('cb'),d=document.createElement('div');
  d.className='msg m'+role.charAt(0);
  var inner=role==='assistant'?mdToHtml(text):esc(text);
  d.innerHTML='<div class=row><div class=bbl>'+inner+'</div></div>';
  box.appendChild(d);box.scrollTop=box.scrollHeight;
}
function clearChat(){document.getElementById('cb').innerHTML='';lastId=0;}
function switchToAlias(alias){
  sessionStorage.setItem('claw_alias',alias);
  currentAlias=alias;
  if(chatWs&&chatWs.readyState===1)
    chatWs.send(JSON.stringify({type:'sync',alias:alias}));
  clearChat();
}
function autoResizeCi(){var t=document.getElementById('ci');t.style.height='auto';t.style.height=Math.min(t.scrollHeight,120)+'px';}
function doSend(){
  if(!chatWs||chatWs.readyState!==1)return;
  var inp=document.getElementById('ci'),tx=inp.value.trim();
  if(!tx)return;inp.value='';inp.style.height='';addBubble('user',tx);
  chatWs.send(JSON.stringify({text:tx,alias:currentAlias}));
}
// WebSocket realtime chat (port 80, /ws/chat)
var chatWs=null,wsRetry=0;
function renderMsg(m,isSnapshot){
  if(!m||m.id===undefined)return;
  if(m.id<=lastId)return;
  lastId=m.id;
  if(m.role==='assistant'){addBubble('assistant',m.text);}
  else if(m.role==='user'&&isSnapshot){addBubble('user',m.text);}
}
function chatWsConnect(){
  updateConn(false);
  var proto=(location.protocol==='https:')?'wss://':'ws://';
  var ws;
  try{ws=new WebSocket(proto+location.host+'/ws/chat');}
  catch(e){scheduleReconnect();return;}
  chatWs=ws;
  ws.onopen=function(){
    wsRetry=0;updateConn(true);
    chatWs.send(JSON.stringify({type:'sync',alias:currentAlias}));
  };
  ws.onmessage=function(ev){
    var d;try{d=JSON.parse(ev.data);}catch(e){return;}
    if(Array.isArray(d))return;
    if(d.type==='snapshot'){
      if(d.alias===currentAlias||!currentAlias){
        currentAlias=d.alias||currentAlias;
        clearChat();
        (d.messages||[]).forEach(function(m){renderMsg(m,true);});
      }
      return;
    }
    if(d.type==='session_deleted'){
      if(typeof onSessionDeleted==='function')onSessionDeleted(d.alias);
      return;
    }
    if(d.type==='session_snapshot'){
      if(Array.isArray(d.sessions)&&typeof buildSessionItem==='function'){
        var sl=document.getElementById('session-list');
        if(sl){
          sl.innerHTML='';
          d.sessions.forEach(function(s){
            var a=typeof s==='object'?(s.alias||''):s;
            var p=typeof s==='object'?(s.preview||''):'';
            sl.appendChild(buildSessionItem(a,a===d.current,p));
          });
        }
      }
      return;
    }
    if(d.alias&&d.alias!==currentAlias)return;
    renderMsg(d,false);
    if(d.role==='assistant'&&typeof loadSessionList==='function'){
      var item=document.querySelector('#session-list [data-alias="'+currentAlias+'"]');
      if(item&&!item.dataset.preview)loadSessionList();
    }
  };
  ws.onclose=function(){chatWs=null;updateConn(false);scheduleReconnect();};
  ws.onerror=function(){try{ws.close();}catch(e){}};
}
function scheduleReconnect(){
  wsRetry++;
  var delay=Math.min(1000*wsRetry,8000);
  setTimeout(chatWsConnect,delay);
}
var btnClearCtx=document.getElementById('btn-clear-ctx');
if(btnClearCtx)btnClearCtx.addEventListener('click',function(){
  clearChat();
  fetch('/api/session',{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify({action:'clear'})});
});
setLang(lang);
chatWsConnect();
