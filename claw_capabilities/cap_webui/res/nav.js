// Sidebar toggle — unified for desktop and mobile
var sbColl=false;
function toggleSb(){
  var isMob=window.innerWidth<=680;
  if(isMob){
    var sb=document.getElementById('sidebar'),bd=document.getElementById('sb-bd');
    var o=sb.classList.toggle('open');bd.classList.toggle('show',o);
  }else{
    sbColl=!sbColl;
    document.getElementById('sidebar').classList.toggle('collapsed',sbColl);
  }
}
function closeMob(){
  if(window.innerWidth<=680){
    document.getElementById('sidebar').classList.remove('open');
    document.getElementById('sb-bd').classList.remove('show');
  }
}

// Navigation
function show(p){
  document.querySelectorAll('.panel').forEach(function(e){e.classList.remove('active');});
  document.querySelectorAll('.nav-item').forEach(function(e){e.classList.remove('active');});
  document.getElementById('p-'+p).classList.add('active');
  var ni=document.getElementById('n-'+p);if(ni)ni.classList.add('active');
  localStorage.setItem('claw_tab',p);
  closeMob();
  if(p==='chat')loadSessionList();
  if(p==='network')initNetwork();
  if(p==='llm')initLLM();
  if(p==='imbot')initImbot();
  if(p==='search')initSearch();
  if(p==='http_request')initHttpRequest();
  if(p==='cap')loadTasks();
  if(p==='lua')loadLuaModules();
  if(p==='cap_mgr')loadCapGroups();
  if(p==='files')initFiles();
  if(p==='memory')loadMemory();
  if(p==='session_mgr')loadSessionMgr();
}
