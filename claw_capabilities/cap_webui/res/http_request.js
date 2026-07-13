// HTTP Request config
function initHttpRequest(){
  withConfig(function(d){
    var h=d.http_request||{};
    document.getElementById('http-allowlist').value=h.allowlist||'';
  });
}
function saveHttpRequest(){
  msg('http-m',true,T('saving'));
  var allowlist=document.getElementById('http-allowlist').value;
  fetch('/setup',{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify({section:'http_request',allowlist:allowlist})})
  .then(function(r){return r.json();})
  .then(function(d){if(d.ok){cfgCache=null;msg('http-m',true,T('saved'));}else msg('http-m',false,T('sf')+(d.error||''));})
  .catch(function(e){msg('http-m',false,T('rf')+e);});
}
