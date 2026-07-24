var capGrpData=[];
var capGrpExp={};
function loadCapGroups(){
  fetch('/api/cap/groups').then(function(r){return r.json();})
  .then(function(d){capGrpData=d||[];renderCapGroups();})
  .catch(function(){document.getElementById('cap-grp-list').innerHTML='<div style="color:#94a3b8;font-size:13px;padding:8px 0">'+T('load_fail')+'</div>';});
}
function renderCapGroups(){
  var el=document.getElementById('cap-grp-list');
  if(!capGrpData.length){el.innerHTML='';return;}
  var h='<div class="cap-col-hdr">'+
    '<div style="flex:1"></div>'+
    '<div class="cap-col-cell"><span class="cap-col-lbl">'+T('cap_col_runtime')+'</span></div>'+
    '<div class="cap-col-cell"><span class="cap-col-lbl">'+T('cap_col_llm')+'</span></div>'+
    '</div>';
  capGrpData.forEach(function(g){
    var gid=g.group_id||'';
    var rtOn=!!g.runtime_enabled;
    var llmVis=!!g.llm_visible;
    var isCore=!!g.is_core;
    var tools=g.tools||[];
    var n=tools.length;
    var exp=!!capGrpExp[gid];
    var plugin=g.plugin_name?'<span class="cap-plugin">'+esc(g.plugin_name)+'</span>':'';
    var chev='<svg class="cap-chev'+(exp?' open':'')+'" width="11" height="11" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round"><polyline points="6 9 12 15 18 9"/></svg>';
    var cnt=T('cap_n_tools').replace('{n}',n);
    var toolsHtml='';
    if(n){
      toolsHtml='<div class="cap-tools" id="cap-t-'+esc(gid)+'" style="display:'+(exp?'block':'none')+'">';
      tools.forEach(function(t){toolsHtml+='<div class="cap-tool">'+esc(t)+'</div>';});
      toolsHtml+='</div>';
    }
    var rtDisabled=isCore;
    var rtTitle=isCore?esc(T('cap_core_tip')):'';
    var rtSw='<div class="cap-col-cell"><label class="sw'+(rtDisabled?' sw-disabled':'')+'"'+(rtTitle?' title="'+rtTitle+'"':'')+'>'+
      '<input type="checkbox"'+(rtOn?' checked':'')+(rtDisabled?' disabled':'')+
      ' onchange="capRtToggle(\''+esc(gid)+'\',this.checked)"><span class="sw-sl"></span></label></div>';
    var llmDisabled=isCore||!rtOn;
    var llmSw='<div class="cap-col-cell"><label class="sw'+(llmDisabled?' sw-disabled':'')+'">'+
      '<input type="checkbox"'+((llmVis&&rtOn)?' checked':'')+(llmDisabled?' disabled':'')+
      ' onchange="capLlmToggle(\''+esc(gid)+'\',this.checked)"><span class="sw-sl"></span></label></div>';
    h+='<div class="sw-row cap-row">'+
       '<div class="cap-row-hd" onclick="capToggleExp(\''+esc(gid)+'\')">'+
         '<div class="cap-row-title"><span class="sw-id">'+esc(gid)+'</span>'+plugin+'</div>'+
         '<div class="cap-row-meta">'+chev+'<span class="cap-cnt">'+cnt+'</span></div>'+
       '</div>'+
       rtSw+llmSw+
       toolsHtml+
    '</div>';
  });
  el.innerHTML=h;
}
function capToggleExp(gid){
  capGrpExp[gid]=!capGrpExp[gid];
  var el=document.getElementById('cap-t-'+gid);
  if(el)el.style.display=capGrpExp[gid]?'block':'none';
  var row=el?el.parentElement:null;
  var chev=row?row.querySelector('.cap-chev'):null;
  if(chev)chev.classList.toggle('open',capGrpExp[gid]);
}
function capRtToggle(gid,enabled){
  for(var i=0;i<capGrpData.length;i++){
    if(capGrpData[i].group_id===gid){
      capGrpData[i].runtime_enabled=enabled;
      if(!enabled)capGrpData[i].llm_visible=false;
      break;
    }
  }
  renderCapGroups();
}
function capLlmToggle(gid,visible){
  for(var i=0;i<capGrpData.length;i++){if(capGrpData[i].group_id===gid){capGrpData[i].llm_visible=visible;break;}}
}
function saveCapAll(){
  var hidden=[];
  var rtDisabled=[];
  capGrpData.forEach(function(g){
    if(!g.runtime_enabled)rtDisabled.push(g.group_id);
    if(!g.llm_visible||!g.runtime_enabled)hidden.push(g.group_id);
  });
  msg('cap-grp-m',true,T('saving'));
  fetch('/api/cap/groups/runtime',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({disabled:rtDisabled})})
  .then(function(r){return r.json();})
  .then(function(d){
    if(!d.ok){msg('cap-grp-m',false,T('sf')+(d.error||''));return null;}
    return fetch('/api/cap/groups/visibility',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({hidden:hidden})})
      .then(function(r){return r.json();});
  })
  .then(function(d){
    if(!d)return;
    if(d.ok)msg('cap-grp-m',true,T('saved'));
    else msg('cap-grp-m',false,T('sf')+(d.error||''));
  })
  .catch(function(e){msg('cap-grp-m',false,T('rf')+e);});
}
