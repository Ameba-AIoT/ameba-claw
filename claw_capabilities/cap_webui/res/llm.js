// LLM
var LLM_PROVIDERS={
  deepseek:{url:'https://api.deepseek.com/v1/',model:'deepseek-v4-flash',backend:0},
  bailian:{url:'https://dashscope.aliyuncs.com/compatible-mode/v1/',model:'qwen-plus',backend:0},
  openai:{url:'https://api.openai.com/v1/',model:'gpt-5.5',backend:0},
  anthropic:{url:'https://api.anthropic.com/v1/',model:'claude-sonnet-4-6',backend:1}};
function selectLLMProvider(p){
  ['deepseek','bailian','openai','anthropic','custom'].forEach(function(k){
    var b=document.getElementById('llm-prov-'+k);
    if(b)b.className='btn btn-sm '+(k===p?'btn-primary':'btn-secondary');
  });
  var pr=LLM_PROVIDERS[p];
  if(pr){
    document.getElementById('llm-url').value=pr.url;
    document.getElementById('llm-model').value=pr.model;
    var s=document.getElementById('llm-auth');
    for(var i=0;i<s.options.length;i++){if(parseInt(s.options[i].value)===pr.backend){s.selectedIndex=i;break;}}
    document.getElementById('llm-adv').style.display='none';
    document.getElementById('llm-adv-toggle').textContent=T('adv_settings')+' ▾';
  }else{
    document.getElementById('llm-adv').style.display='block';
    document.getElementById('llm-adv-toggle').textContent=T('adv_settings')+' ▴';
  }
}
function toggleLLMAdv(){
  var adv=document.getElementById('llm-adv');
  var show=adv.style.display==='none';
  adv.style.display=show?'block':'none';
  document.getElementById('llm-adv-toggle').textContent=show?T('adv_settings')+' ▴':T('adv_settings')+' ▾';
}
function initLLM(){
  withConfig(function(d){
    var l=d.llm||{};
    document.getElementById('llm-url').value=l.api_url||'';
    document.getElementById('llm-key').value=l.api_key||'';
    document.getElementById('llm-model').value=l.model||'';
    document.getElementById('llm-tok').value=l.max_tokens||16384;
    document.getElementById('llm-iter').value=l.max_iterations||50;
    document.getElementById('llm-think').checked=!!l.thinking;
    document.getElementById('llm-stream').checked=l.stream!==false;
    document.getElementById('llm-compact').value=l.compact_tokens||110000;
    document.getElementById('llm-window').value=l.window_tokens||128000;
    var bk=typeof l.backend==='number'?l.backend:0;
    var s=document.getElementById('llm-auth');
    for(var i=0;i<s.options.length;i++){if(parseInt(s.options[i].value)===bk){s.selectedIndex=i;break;}}
    var url=l.api_url||'';
    var detected=null;
    for(var pk in LLM_PROVIDERS){if(LLM_PROVIDERS[pk].url===url){detected=pk;break;}}
    ['deepseek','bailian','openai','anthropic','custom'].forEach(function(k){
      var b=document.getElementById('llm-prov-'+k);
      if(b)b.className='btn btn-sm '+(k===detected?'btn-primary':'btn-secondary');
    });
    if(!detected&&url){
      document.getElementById('llm-adv').style.display='block';
      document.getElementById('llm-adv-toggle').textContent=T('adv_settings')+' ▴';
      var cb=document.getElementById('llm-prov-custom');
      if(cb)cb.className='btn btn-sm btn-primary';
    }
  });
}
function saveLLM(){
  msg('llm-m',true,T('saving'));
  fetch('/setup',{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify({section:'llm',api_url:document.getElementById('llm-url').value,
      api_key:document.getElementById('llm-key').value,
      model:document.getElementById('llm-model').value||'deepseek-v4-flash',
      max_tokens:parseInt(document.getElementById('llm-tok').value)||16384,
      max_iterations:parseInt(document.getElementById('llm-iter').value)||50,
      thinking:document.getElementById('llm-think').checked,
      stream:document.getElementById('llm-stream').checked,
      compact_tokens:parseInt(document.getElementById('llm-compact').value)||110000,
      window_tokens:parseInt(document.getElementById('llm-window').value)||128000,
      backend:parseInt(document.getElementById('llm-auth').value)})})
  .then(function(r){return r.json();})
  .then(function(d){if(d.ok){cfgCache=null;msg('llm-m',true,T('saved'));}else msg('llm-m',false,T('sf')+(d.error||''));})
  .catch(function(e){msg('llm-m',false,T('rf')+e);});
}
