// Tasks
function loadTasks(){
  fetch('/api/tasks').then(function(r){return r.json();})
  .then(function(d){
    var tasks=d.tasks||[],tb=document.getElementById('cap-tb');
    if(!tasks.length){tb.innerHTML='<tr><td colspan="4" class="empty-td">'+T('no_task')+'</td></tr>';return;}
    tb.innerHTML=tasks.map(function(t){
      var n=esc(t.name||'');
      var sched=esc(t.schedule||t.interval||'-');
      return '<tr><td>'+n+'</td><td>'+sched+'</td><td><span class="bk '+(t.enabled?'ok':'of')+'">'+(t.enabled?T('running'):T('stopped'))+'</span></td><td>'+
        '<button class="btn btn-sm '+(t.enabled?'btn-secondary':'btn-primary')+'" onclick="taskToggle(\''+n+'\','+(t.enabled?'false':'true')+')">'+
        (t.enabled?T('disable'):T('enable'))+'</button></td></tr>';
    }).join('');
  }).catch(function(){document.getElementById('cap-tb').innerHTML='<tr><td colspan="4" class="empty-td">'+T('load_fail')+'</td></tr>';});
}
function taskToggle(name,en){
  fetch('/api/tasks/'+encodeURIComponent(name)+'/'+(en?'enable':'disable'),{method:'POST'})
  .then(function(r){return r.json();}).then(function(){loadTasks();}).catch(function(){});
}
