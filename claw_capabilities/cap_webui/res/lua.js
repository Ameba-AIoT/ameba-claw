// LUA driver modules
var luaModData=[];
var luaMask=0x7FF;
function loadLuaModules(){
  fetch('/api/lua/modules').then(function(r){return r.json();})
  .then(function(d){
    luaModData=d.modules||[];
    luaMask=typeof d.mask==='number'?d.mask:0x7FF;
    renderLuaModules();
  })
  .catch(function(){document.getElementById('lua-mod-list').innerHTML='<div style="color:#94a3b8;font-size:13px;padding:8px 0">'+T('load_fail')+'</div>';});
}
function renderLuaModules(){
  if(!luaModData.length){document.getElementById('lua-mod-list').innerHTML='';return;}
  var h='';
  luaModData.forEach(function(m){
    var bit=m.bit;
    var en=m.locked||!!(luaMask&(1<<bit));
    var desc=lang==='zh'?(m.desc_zh||m.desc_en||''):(m.desc_en||m.desc_zh||'');
    var lockTag=m.locked?'<span style="font-size:10px;color:#94a3b8;margin-left:6px;font-family:sans-serif">'+T('lua_mod_locked')+'</span>':'';
    h+='<div class="sw-row"><div class="sw-info"><div class="sw-id">'+esc(m.id)+lockTag+'</div>'+(desc?'<div class="sw-desc">'+esc(desc)+'</div>':'')+'</div>'+
      '<label class="sw"><input type="checkbox"'+(en?' checked':'')+(m.locked?' disabled':'')+
      (m.locked?'':' onchange="luaModToggle('+bit+',this.checked)"')+'><span class="sw-sl" style="'+(m.locked?'opacity:.55;cursor:default':'')+'"></span></label></div>';
  });
  document.getElementById('lua-mod-list').innerHTML=h;
}
function luaModToggle(bit,en){if(en)luaMask|=(1<<bit);else luaMask&=~(1<<bit);}
function saveLuaModules(){
  msg('lua-mod-m',true,T('saving'));
  fetch('/api/lua/modules',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({mask:luaMask})})
  .then(function(r){return r.json();})
  .then(function(d){if(d.ok)msg('lua-mod-m',true,T('saved'));else msg('lua-mod-m',false,T('sf')+(d.error||''));})
  .catch(function(e){msg('lua-mod-m',false,T('rf')+e);});
}
