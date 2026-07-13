// Search
function initSearch(){
  withConfig(function(d){
    var s=d.search||{};
    document.getElementById('sch-key').value=s.api_key||'';
    document.getElementById('sch-n').value=s.max_results||3;
  });
}
function saveSearch(){
  msg('sch-m',true,T('saving'));
  fetch('/setup',{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify({section:'search',
      api_key:document.getElementById('sch-key').value,
      max_results:parseInt(document.getElementById('sch-n').value)||3})})
  .then(function(r){return r.json();})
  .then(function(d){if(d.ok){cfgCache=null;msg('sch-m',true,T('saved'));}else msg('sch-m',false,T('sf')+(d.error||''));})
  .catch(function(e){msg('sch-m',false,T('rf')+e);});
}
