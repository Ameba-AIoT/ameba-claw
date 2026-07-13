// Files
var curPath='vfs:/';
var _filesRO=false;
var _filesInited=false;
var _regions=null;  /* populated by initFiles(); used by fpParse to validate prefixes */

/* Parse "rolfs:/skills/main.lua" → {region:"rolfs",path:"/skills/main.lua"}.
 * Only recognises prefixes present in _regions (once loaded) to avoid treating
 * single-letter strings like "c:" as a valid region. */
function fpParse(full){
  var m=full.match(/^([a-z0-9]+):(.*)$/);
  if(m){
    var valid=!_regions||_regions.some(function(r){return r.prefix===m[1];});
    if(valid)return {region:m[1],path:m[2]||'/'};
  }
  return {region:'vfs',path:full};
}
/* Build full path from region + path */
function fpJoin(region,path){return region+':'+path;}
/* Parent of path component only (no region) */
function fpParent(path){
  var p=path.replace(/\/+$/,'');
  var idx=p.lastIndexOf('/');
  return idx<=0?'/':p.substring(0,idx);
}

function initFiles(){
  if(_filesInited){loadFiles(curPath);return;}
  _filesInited=true;
  fetch('/api/files/regions').then(function(r){return r.json();})
  .then(function(regions){
    _regions=regions;
    var sel=document.getElementById('region-sel');
    sel.innerHTML='';
    regions.forEach(function(r){
      var opt=document.createElement('option');
      opt.value=r.prefix;
      opt.className='nl';
      opt.setAttribute('data-k','region_'+r.prefix);
      opt.setAttribute('data-sfx','('+r.prefix+')');
      opt.text=T('region_'+r.prefix)+'('+r.prefix+')';
      sel.appendChild(opt);
    });
    loadFiles('vfs:/');
  }).catch(function(){loadFiles('vfs:/');});
}

function onRegionChange(region){
  loadFiles(fpJoin(region,'/'));
}

function loadFiles(path){
  curPath=path||'vfs:/';
  /* Reset RO state immediately so stale true doesn't block edits during navigation */
  _filesRO=false;
  _applyROState(false);
  var parsed=fpParse(curPath);
  /* Sync dropdown to current region */
  var sel=document.getElementById('region-sel');
  if(sel)sel.value=parsed.region;
  /* Show path part (without scheme) in the path box */
  document.getElementById('fpath').innerText=parsed.path||'/';
  fetch('/api/files?path='+encodeURIComponent(curPath)).then(function(r){return r.json();})
  .then(function(d){
    _filesRO=d.readonly||false;
    _applyROState(_filesRO);
    var entries=d.entries||[],tb=document.getElementById('files-tb');
    if(!entries.length){tb.innerHTML='<tr><td colspan="4" class="empty-td">'+T('empty_dir')+'</td></tr>';return;}
    tb.innerHTML=entries.map(function(f){
      var isDir=f.type==='dir',n=esc(f.name||''),fp=esc(f.path||'');
      var nameCell=isDir?
        '<a href="#" style="color:#4a90e2;font-weight:600;text-decoration:none" onclick="loadFiles(\''+fp+'\');return false">'+n+'/</a>':
        n;
      var dlName=isDir?n+'.zip':n;
      var acts='<button class="btn btn-secondary btn-sm" onclick="dlEntry(\''+fp+'\',\''+dlName+'\')">'+T('dl')+'</button> ';
      if(!isDir){
        if(_filesRO)
          acts+='<button class="btn btn-secondary btn-sm" onclick="filesEdit(\''+fp+'\')">'+T('view_file')+'</button> ';
        else
          acts+='<button class="btn btn-secondary btn-sm" onclick="filesEdit(\''+fp+'\')">'+T('edit')+'</button> ';
      } else {
        acts+='<button class="btn btn-secondary btn-sm" style="visibility:hidden" disabled>'+T('edit')+'</button> ';
      }
      if(!_filesRO)
        acts+='<button class="btn btn-danger btn-sm" onclick="filesDel(\''+fp+'\')">'+T('del')+'</button>';
      return '<tr><td>'+nameCell+'</td><td>'+fmtSz(f.size)+'</td><td>'+fmtTime(f.mtime)+'</td><td>'+acts+'</td></tr>';
    }).join('');
  }).catch(function(){document.getElementById('files-tb').innerHTML='<tr><td colspan="4" class="empty-td">'+T('load_fail')+'</td></tr>';});
}

function _applyROState(ro){
  var btnUp=document.getElementById('btn-upload');
  var btnMk=document.getElementById('btn-mkdir');
  if(btnUp)btnUp.style.display=ro?'none':'';
  if(btnMk)btnMk.style.display=ro?'none':'';
}

function filesUp(){
  var parsed=fpParse(curPath);
  var parent=fpParent(parsed.path);
  loadFiles(fpJoin(parsed.region,parent));
}
function doMkdir(){
  var name=prompt('文件夹名称:');
  if(!name||!name.trim())return;
  name=name.trim();
  var parsed=fpParse(curPath);
  var sep=parsed.path.endsWith('/')?'':'/';
  var full=fpJoin(parsed.region,parsed.path+sep+name);
  fetch('/api/files/mkdir?path='+encodeURIComponent(full),{method:'POST'})
  .then(function(r){return r.json();})
  .then(function(d){if(d.ok)loadFiles(curPath);else alert('创建失败: '+(d.error||''));})
  .catch(function(e){alert('请求失败: '+e);});
}
function doFilesUpload(inp){
  if(!inp.files||!inp.files[0])return;
  var fd=new FormData();fd.append('file',inp.files[0]);fd.append('path',curPath);
  fetch('/api/files/upload',{method:'POST',body:fd})
  .then(function(r){return r.json();}).then(function(d){if(d.ok)loadFiles(curPath);else alert(T('sf')+(d.error||''));})
  .catch(function(e){alert(T('rf')+e);});inp.value='';
}
function dlEntry(path,name){var a=document.createElement('a');a.href='/api/files/download?path='+encodeURIComponent(path);a.download=name;document.body.appendChild(a);a.click();document.body.removeChild(a);}
function filesDel(path){
  if(!confirm(T('del')+' '+path+'?'))return;
  fetch('/api/files?path='+encodeURIComponent(path),{method:'DELETE'})
  .then(function(r){return r.json();}).then(function(){loadFiles(curPath);}).catch(function(){});
}
var _editPath='';
function filesEdit(path){
  _editPath=path;
  var ro=_filesRO;
  var titleEl=document.getElementById('edit-title');
  var area=document.getElementById('edit-area');
  var msg=document.getElementById('edit-msg');
  var saveBtn=document.getElementById('btn-edit-save');
  titleEl.innerText=T(ro?'view_file':'edit_file')+': '+fpParse(path).path;
  area.value='';
  area.readOnly=ro;
  area.style.background=ro?'#f8faff':'';
  if(saveBtn)saveBtn.style.display=ro?'none':'';
  msg.className='modal-msg';
  msg.style.display='none';
  document.getElementById('edit-modal').classList.add('show');
  fetch('/api/files/content?path='+encodeURIComponent(path))
  .then(function(r){
    if(!r.ok)return r.json().then(function(d){throw new Error((d&&d.error)||r.statusText);});
    return r.text();
  })
  .then(function(t){area.value=t;})
  .catch(function(e){
    msg.className='modal-msg err';
    msg.style.display='block';
    msg.innerText=T('sf')+e.message;
  });
}
function closeEditModal(){
  document.getElementById('edit-modal').classList.remove('show');
  _editPath='';
}
function filesSaveEdit(){
  var area=document.getElementById('edit-area');
  var msg=document.getElementById('edit-msg');
  msg.className='modal-msg';
  msg.style.display='none';
  fetch('/api/files/content?path='+encodeURIComponent(_editPath),{method:'PUT',body:area.value})
  .then(function(r){return r.json();})
  .then(function(d){
    if(d.ok){
      msg.className='modal-msg ok';
      msg.style.display='block';
      msg.innerText=T('file_save_ok');
    }else{
      msg.className='modal-msg err';
      msg.style.display='block';
      msg.innerText=T('file_save_fail')+(d.error||'');
    }
  })
  .catch(function(e){
    msg.className='modal-msg err';
    msg.style.display='block';
    msg.innerText=T('rf')+e.message;
  });
}
