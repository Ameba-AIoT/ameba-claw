// Memory management
var ltItems=[];
async function loadMemory(){
  var names=['agents','soul','identity','user'];
  /* 1. List vfs:/ once to know which profile files actually exist */
  var existing=null;
  try{
    var lr=await fetch('/api/files?path=vfs:/');
    if(lr.ok){
      var ld=await lr.json();
      existing={};
      var entries=ld.entries||[];
      for(var j=0;j<entries.length;j++){if(entries[j].name)existing[entries[j].name.toLowerCase()]=entries[j].path;}
    }
  }catch(e){}
  /* 2. Read each profile file serially — at most 1 concurrent request */
  for(var i=0;i<names.length;i++){
    var name=names[i];
    var filename=name.toUpperCase()+'.md';
    var el=document.getElementById('mem-'+name);
    if(existing&&!existing[filename.toLowerCase()]){if(el)el.value='';continue;}
    var path=(existing&&existing[filename.toLowerCase()])||('vfs:/'+filename);
    try{
      var resp=await fetch('/api/files/content?path='+encodeURIComponent(path));
      if(el)el.value=resp.ok?await resp.text():'';
    }catch(e){if(el)el.value='';}
  }
  /* 3. Long-term store */
  loadLongTermItems();
}
function saveMemoryFile(name){
  var mid='mem-'+name+'-m';
  var el=document.getElementById('mem-'+name);
  if(!el)return;
  msg(mid,true,T('saving'));
  fetch('/api/files/content?path=/'+name.toUpperCase()+'.md',{method:'PUT',body:el.value})
    .then(function(r){return r.json();})
    .then(function(d){if(d.ok)msg(mid,true,T('mem_save_ok'));else msg(mid,false,T('mem_save_fail')+(d.error||''));})
    .catch(function(e){msg(mid,false,T('rf')+e);});
}
function loadLongTermItems(){
  var tb=document.getElementById('mem-lt-tb');
  fetch('/api/files/content?path=/memory/long_term_store.json')
    .then(function(r){if(!r.ok)throw new Error(r.statusText);return r.text();})
    .then(function(t){
      try{ltItems=JSON.parse(t)||[];}catch(e){ltItems=[];}
      renderLongTermItems();
    })
    .catch(function(){
      ltItems=[];
      if(tb)tb.innerHTML='<tr><td colspan="5" class="empty-td">'+T('mem_load_fail')+'</td></tr>';
    });
}
function renderLongTermItems(){
  var tb=document.getElementById('mem-lt-tb');
  if(!tb)return;
  if(!ltItems.length){tb.innerHTML='<tr><td colspan="5" class="empty-td">'+T('mem_no_items')+'</td></tr>';return;}
  var h='';
  for(var i=0;i<ltItems.length;i++){
    var it=ltItems[i];
    h+='<tr id="lt-row-'+it.id+'">'
      +'<td style="max-width:280px;word-break:break-all">'+esc(it.content||'')+'</td>'
      +'<td>'+esc(it.tags||'')+'</td>'
      +'<td>'+esc(it.source||'')+'</td>'
      +'<td style="white-space:nowrap">'+fmtTime(it.created_at)+'</td>'
      +'<td style="white-space:nowrap">'
        +'<button class="btn btn-secondary btn-sm" onclick="ltEditStart('+it.id+')">'+T('edit')+'</button> '
        +'<button class="btn btn-danger btn-sm" onclick="ltDelete('+it.id+')">'+T('del')+'</button>'
      +'</td>'
      +'</tr>';
  }
  tb.innerHTML=h;
}
function ltEditStart(id){
  var item=null;
  for(var i=0;i<ltItems.length;i++){if(ltItems[i].id===id){item=ltItems[i];break;}}
  if(!item)return;
  var row=document.getElementById('lt-row-'+id);
  if(!row)return;
  row.innerHTML=
    '<td colspan="3"><div class="fm" style="margin:0">'
      +'<label style="font-size:11px;color:#64748b;margin:2px 0">'+T('mem_content_lbl')+'</label>'
      +'<textarea id="lt-ec-'+id+'" style="min-height:60px;margin-bottom:6px"></textarea>'
      +'<label style="font-size:11px;color:#64748b;margin:2px 0">'+T('mem_tags_lbl')+'</label>'
      +'<input id="lt-et-'+id+'" type="text">'
    +'</div></td>'
    +'<td>'+esc(item.source||'')+'</td>'
    +'<td style="white-space:nowrap">'
      +'<button class="btn btn-primary btn-sm" onclick="ltEditSave('+id+')">'+T('save')+'</button> '
      +'<button class="btn btn-secondary btn-sm" onclick="renderLongTermItems()">'+T('cancel')+'</button>'
    +'</td>';
  var ec=document.getElementById('lt-ec-'+id);
  var et=document.getElementById('lt-et-'+id);
  if(ec)ec.value=item.content||'';
  if(et)et.value=item.tags||'';
}
function ltEditSave(id){
  var ec=document.getElementById('lt-ec-'+id);
  var et=document.getElementById('lt-et-'+id);
  if(!ec)return;
  for(var i=0;i<ltItems.length;i++){
    if(ltItems[i].id===id){
      ltItems[i].content=ec.value;
      if(et)ltItems[i].tags=et.value;
      break;
    }
  }
  saveLongTermStore(renderLongTermItems);
}
function ltDelete(id){
  if(!confirm(T('mem_del_confirm')))return;
  var newItems=[];
  for(var i=0;i<ltItems.length;i++){if(ltItems[i].id!==id)newItems.push(ltItems[i]);}
  ltItems=newItems;
  saveLongTermStore(renderLongTermItems);
}
function saveLongTermStore(onDone){
  var mid='mem-lt-m';
  msg(mid,true,T('saving'));
  fetch('/api/files/content?path=/memory/long_term_store.json',{method:'PUT',body:JSON.stringify(ltItems)})
    .then(function(r){return r.json();})
    .then(function(d){
      if(d.ok){msg(mid,true,T('mem_save_ok'));if(onDone)onDone();}
      else msg(mid,false,T('mem_save_fail')+(d.error||''));
    })
    .catch(function(e){msg(mid,false,T('rf')+e);});
}
