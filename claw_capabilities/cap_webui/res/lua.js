// LUA driver modules
var luaModData=[];
var luaDisabledSet={};
function loadLuaModules(){
  fetch('/api/lua/modules').then(function(r){return r.json();})
  .then(function(d){
    luaModData=(d.modules||[]).filter(function(m){return m.chip_ok;});
    luaDisabledSet={};
    luaModData.forEach(function(m){if(!m.enabled)luaDisabledSet[m.id]=true;});
    renderLuaModules();
  })
  .catch(function(){document.getElementById('lua-mod-list').innerHTML='<div style="color:#94a3b8;font-size:13px;padding:8px 0">'+T('load_fail')+'</div>';});
}
function renderLuaModules(){
  if(!luaModData.length){document.getElementById('lua-mod-list').innerHTML='';return;}
  var hw=luaModData.filter(function(m){return m.category==='drv';});
  var periph=luaModData.filter(function(m){return m.category==='dev';});
  var sw=luaModData.filter(function(m){return m.category==='sw';});
  var h='';
  function renderGroup(label,mods){
    if(!mods.length)return;
    h+='<div style="font-size:12px;font-weight:600;color:#94a3b8;margin:10px 0 2px">'+label+'</div>';
    mods.forEach(function(m){
      var en=m.locked||!luaDisabledSet[m.id];
      var lockTag=m.locked?'<span style="font-size:10px;color:#94a3b8;margin-left:6px;font-family:sans-serif">'+T('lua_mod_locked')+'</span>':'';
      h+='<div class="sw-row"><div class="sw-info"><div class="sw-id">'+esc(m.id)+lockTag+'</div></div>'+
        '<label class="sw"><input type="checkbox"'+(en?' checked':'')+(m.locked?' disabled':'')+
        (m.locked?'':' onchange="luaModToggle(\''+esc(m.id)+'\',this.checked)"')+'><span class="sw-sl" style="'+(m.locked?'opacity:.55;cursor:default':'')+'"></span></label></div>';
    });
  }
  renderGroup(T('lua_cat_drv'),hw);
  renderGroup(T('lua_cat_dev'),periph);
  renderGroup(T('lua_cat_sw'),sw);
  document.getElementById('lua-mod-list').innerHTML=h;
}
function luaModToggle(id,en){if(en)delete luaDisabledSet[id];else luaDisabledSet[id]=true;}
function saveLuaModules(){
  var disabled=Object.keys(luaDisabledSet).join(',');
  msg('lua-mod-m',true,T('saving'));
  fetch('/api/lua/modules',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({disabled:disabled})})
  .then(function(r){return r.json();})
  .then(function(d){if(d.ok)msg('lua-mod-m',true,T('saved'));else msg('lua-mod-m',false,T('sf')+(d.error||''));})
  .catch(function(e){msg('lua-mod-m',false,T('rf')+e);});
}
