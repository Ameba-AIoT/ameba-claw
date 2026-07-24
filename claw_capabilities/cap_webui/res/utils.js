var L={
zh:{
  nav_status:'系统状态',nav_chat:'实时对话',grp_cfg:'系统配置',grp_mgmt:'系统管理',
  nav_network:'网络配置',nav_llm:'LLM 配置',nav_imbot:'IMBOT 配置',nav_search:'搜索配置',nav_http_request:'HTTP 配置',http_allowlist_label:'HTTP 请求访问白名单',http_allowlist_hint:'每行一条规则，支持通配符 *(例如*.example.com)。留空=拒绝所有；单独 * =放行所有。',
  nav_cap:'任务管理',nav_lua:'LUA 管理',nav_cap_mgr:'CAP 管理',nav_files:'文件管理',nav_memory:'记忆管理',
  cap_grp_title:'CAP 能力组管理',cap_grp_hint:'管理各 CAP 的运行时状态与 LLM 工具可见性。',cap_n_tools:'{n} 个工具',
  cap_col_runtime:'运行时启用',cap_col_llm:'LLM 可见',

  cap_core_tip:'CORE 能力（系统必需），不可禁用',
  cap_layer1:'• Kconfig 关闭：CAP 代码不编译进固件，减少 Flash 占用，重新烧录后生效',
  cap_layer2:'• 运行时启用 关闭：CAP 不初始化，LLM 同时不可见，减少堆内存占用，重启后生效',
  cap_layer3:'• LLM 可见 关闭：LLM 无法调用该工具，CAP 仍在后台运行，立即生效',
  nav_session_mgr:'会话管理',
  session_history:'对话历史',session_current:'(当前)',
  session_del_confirm:'确认删除会话',session_del_ok:'已删除',session_del_fail:'删除失败',
  session_load_fail:'加载失败',session_empty:'暂无会话记录',
  msg_user:'用户',msg_assistant:'助手',
  new_session:'新建对话',clear_ctx:'清除上下文',
  lnk_home:'官方主页',
  ph_disable:'留空禁用',
  llm_hint:'选择大模型提供商，填入 API Key 和模型 ID 即可使用',
  adv_settings:'高级设置',
  chat_ph:'输入消息...',
  ws_connecting:'连接中',
  ws_reconnecting:'重连中',
  refresh:'刷新',restart:'重启设备',save:'保存配置',cancel:'取消',upload:'上传',send:'发送',
  wifi_ssid:'WiFi 名称',wifi_sec:'加密类型',wifi_pw:'密码',
  llm_url:'API 地址',llm_auth:'认证方式',llm_key:'API Key',llm_model:'模型',
  llm_tokens:'最大 Token 数',llm_iter:'最大迭代次数',llm_think:'启用思考模式',llm_stream:'启用流式输出',
  llm_compact:'上下文压缩触发 Token 数',llm_window:'上下文窗口上限 Token 数',
  tg_token:'Telegram Bot Token',fs_id:'Feishu App ID',fs_sec:'Feishu App Secret',
  save_tg:'保存 Telegram',save_fs:'保存飞书',
  qq_app_id:'QQ Bot AppID',qq_sec:'QQ Bot AppSecret (clientSecret)',save_qq:'保存 QQ',
  wx_url:'WeChat iLink URL',wx_app_id:'App ID',save_wx:'保存微信',wx_qr:'生成二维码',
  wx_hint:'请用微信扫描二维码登录',wx_logged:'已登录',wx_pending:'等待扫码',wx_error:'连接错误',wx_idle:'未登录',
  wx_token_lbl:'WeChat Bot Token',save_imbot:'保存配置',
  sch_key:'Tavily API Key',sch_n:'每次返回结果数（1-5）',
  col_name:'名称',col_ver:'版本',col_status:'状态',col_action:'操作',col_size:'大小',col_modified:'修改时间',col_task_schedule:'执行周期',
  go_up:'上级',mkdir:'新建文件夹',
  lua_mod_title:'Lua 模块',lua_mod_locked:'锁定',lua_cat_drv:'驱动模块',lua_cat_dev:'设备模块',lua_cat_sw:'软件模块',
  wifi_saved_h:'WiFi 配置已保存',
  wifi_saved_p:'重启后设备将连接到新的 WiFi 网络。请确保您的设备已连接到目标网络，否则重启后将无法继续访问此配置页面。',
  restart_now:'立即重启',
  saving:'保存中…',saved:'已保存配置',sf:'保存失败: ',rf:'请求错误: ',
  wifi_empty:'WiFi名称不能为空',
  conn:'已连接',noconn:'未连接',running:'运行中',stopped:'未启动',
  cfg:'已配置',nocfg:'未配置',
  s_wifi:'WiFi',s_ap:'SoftAP',s_mode:'工作模式',s_ver:'固件版本',s_heap:'剩余堆内存',s_low:'最低水位',
  enable:'启用',disable:'禁用',edit:'编辑',del:'删除',dl:'下载',
  no_task:'暂无定时任务',no_lua:'暂无 LUA 脚本',empty_dir:'空目录',load_fail:'加载失败',
  files_warn:'不正确地删除或修改必要文件可能导致系统异常',
  region_vfs:'用户区域',region_rolfs:'只读区域',
  edit_file:'编辑文件',view_file:'查看',file_save_ok:'保存成功',file_not_text:'不是文本文件',file_too_large:'文件过大（最大32KB）',file_save_fail:'保存失败: ',
  mem_agents_title:'代理身份',mem_agents_label:'Agent基础身份定义（AGENTS.md）',mem_soul_title:'灵魂',mem_soul_label:'系统人格定义（SOUL.md）',
  mem_user_title:'用户信息',mem_user_label:'跨会话用户资料（USER.md）',
  mem_id_title:'身份',mem_id_label:'个性身份特征（IDENTITY.md）',
  mem_lt_title:'长期记忆',mem_lt_content:'内容',mem_lt_tags:'标签',mem_lt_source:'来源',mem_lt_time:'时间',
  mem_save_ok:'已保存',mem_save_fail:'保存失败: ',mem_load_fail:'加载失败',
  mem_no_items:'暂无长期记忆',mem_del_confirm:'确认删除该条记忆？',
  mem_content_lbl:'内容',mem_tags_lbl:'标签',
  wifi_connect:'连接',wifi_connecting_req:'正在发起连接请求...',wifi_connecting_to:'正在连接到 {ssid} ...请稍候',
  wifi_prov_tip:'连接期间热点信道会切换，网页可能短暂中断，请保持设备连接此热点，将自动恢复...',
  wifi_waiting:'正在等待连接...',wifi_switching:'信道切换中，请保持连接此热点，等待自动恢复...',
  wifi_timeout:'连接超时，请在路由器后台查看设备 IP，访问 http://&lt;IP&gt;',
  wifi_connected_title:'已连接到局域网',wifi_ip_label:'设备 IP 地址：',
  wifi_open_mgmt:'打开管理页面 ',wifi_access_hint:'请将设备连接到上面配置的局域网并点击上方链接',
  wifi_fail:'连接失败：',wifi_hint:'填写局域网 WiFi 信息后点击连接，成功后页面将显示设备 IP 地址和访问链接。',
  wifi_pw_ph:'留空表示开放网络',
  softap_disconn:'SoftAP 连接已断开，请手动重连热点后刷新页面继续'
},
en:{
  nav_status:'System Status',nav_chat:'Live Chat',grp_cfg:'Configuration',grp_mgmt:'Management',
  nav_network:'Network',nav_llm:'LLM Config',nav_imbot:'IMBOT Config',nav_search:'Search Config',nav_http_request:'HTTP Config',http_allowlist_label:'HTTP Request Allowlist',http_allowlist_hint:'One rule per line, wildcards * supported(e.g: *.example.com). Empty=deny all; * alone=allow all.',
  nav_cap:'Task Manager',nav_lua:'LUA Manager',nav_cap_mgr:'CAP Manager',nav_files:'File Manager',nav_memory:'Memory',
  cap_grp_title:'CAP Group Management',cap_grp_hint:'Manage runtime state and LLM tool visibility for each capability group.',cap_n_tools:'{n} tools',
  cap_col_runtime:'Runtime Enable',cap_col_llm:'LLM Visible',

  cap_core_tip:'CORE capability (required by system) — cannot be disabled',
  cap_layer1:'• Kconfig off: CAP not compiled into firmware, reduces Flash usage, effective after re-flash',
  cap_layer2:'• Runtime Enable off: CAP not initialized, LLM also hidden, reduces heap usage, takes effect on next reboot',
  cap_layer3:'• LLM Visible off: LLM cannot call the tools, CAP still runs in background, immediate effect',
  nav_session_mgr:'Sessions',
  session_history:'History',session_current:'(current)',
  session_del_confirm:'Delete session',session_del_ok:'Deleted',session_del_fail:'Delete failed',
  session_load_fail:'Load failed',session_empty:'No sessions',
  msg_user:'User',msg_assistant:'Assistant',
  new_session:'New Session',clear_ctx:'Clear Context',
  lnk_home:'Official Site',
  ph_disable:'Leave blank to disable',
  llm_hint:'Select a provider and enter API Key and Model ID to get started',
  adv_settings:'Advanced',
  chat_ph:'Type a message...',
  ws_connecting:'Connecting',
  ws_reconnecting:'Reconnecting',
  refresh:'Refresh',restart:'Restart',save:'Save',cancel:'Cancel',upload:'Upload',send:'Send',
  wifi_ssid:'WiFi Name (SSID)',wifi_sec:'Security Type',wifi_pw:'Password',
  llm_url:'API URL',llm_auth:'Auth Method',llm_key:'API Key',llm_model:'Model',
  llm_tokens:'Max Tokens',llm_iter:'Max Iterations',llm_think:'Enable Thinking',llm_stream:'Enable Streaming',
  llm_compact:'Compact Trigger Tokens',llm_window:'Context Window Limit',
  tg_token:'Telegram Bot Token',fs_id:'Feishu App ID',fs_sec:'Feishu App Secret',
  save_tg:'Save Telegram',save_fs:'Save Feishu',
  qq_app_id:'QQ Bot AppID',qq_sec:'QQ Bot AppSecret',save_qq:'Save QQ',
  wx_url:'WeChat iLink URL',wx_app_id:'App ID',save_wx:'Save WeChat',wx_qr:'Generate QR Code',
  wx_hint:'Scan QR Code with WeChat to login',wx_logged:'Logged In',wx_pending:'Awaiting Scan',wx_error:'Connection Error',wx_idle:'Not Logged In',
  wx_token_lbl:'WeChat Bot Token',save_imbot:'Save Config',
  sch_key:'Tavily API Key',sch_n:'Results per Query (1-5)',
  col_name:'Name',col_ver:'Version',col_status:'Status',col_action:'Actions',col_size:'Size',col_modified:'Modified',col_task_schedule:'Schedule',
  go_up:'Up',mkdir:'New Folder',
  lua_mod_title:'Lua Modules',lua_mod_locked:'locked',lua_cat_drv:'Drivers',lua_cat_dev:'Devices',lua_cat_sw:'Software',
  wifi_saved_h:'WiFi Config Saved',
  wifi_saved_p:'Device will connect to the new WiFi after restart. Make sure your device is on the target network or you may lose access.',
  restart_now:'Restart Now',
  saving:'Saving…',saved:'Saved',sf:'Save failed: ',rf:'Request error: ',
  wifi_empty:'WiFi name is required',
  conn:'Connected',noconn:'Disconnected',running:'Running',stopped:'Stopped',
  cfg:'Configured',nocfg:'Not configured',
  s_wifi:'WiFi',s_ap:'SoftAP',s_mode:'Mode',s_ver:'Firmware',s_heap:'Free Heap',s_low:'Min Watermark',
  enable:'Enable',disable:'Disable',edit:'Edit',del:'Delete',dl:'Download',
  no_task:'No scheduled tasks',no_lua:'No LUA scripts',empty_dir:'Empty directory',load_fail:'Load failed',
  files_warn:'Deleting or modifying essential files incorrectly may cause system malfunction',
  region_vfs:'User Storage',region_rolfs:'Read-only',
  edit_file:'Edit File',view_file:'View',file_save_ok:'Saved successfully',file_not_text:'Not a text file',file_too_large:'File too large (max 32KB)',file_save_fail:'Save failed: ',
  mem_agents_title:'Agent Identity',mem_agents_label:'Base agent identity definition (AGENTS.md)',mem_soul_title:'Soul',mem_soul_label:'System persona definition (SOUL.md)',
  mem_user_title:'User Info',mem_user_label:'Cross-session user profile (USER.md)',
  mem_id_title:'Identity',mem_id_label:'Personality identity (IDENTITY.md)',
  mem_lt_title:'Long-term Memory',mem_lt_content:'Content',mem_lt_tags:'Tags',mem_lt_source:'Source',mem_lt_time:'Time',
  mem_save_ok:'Saved',mem_save_fail:'Save failed: ',mem_load_fail:'Load failed',
  mem_no_items:'No long-term memories',mem_del_confirm:'Delete this memory entry?',
  mem_content_lbl:'Content',mem_tags_lbl:'Tags',
  softap_disconn:'SoftAP connection lost. Please reconnect to the hotspot and refresh to continue.',
  wifi_connect:'Connect',wifi_connecting_req:'Initiating connection...',wifi_connecting_to:'Connecting to {ssid} ... please wait',
  wifi_prov_tip:'The AP channel may change briefly during connection. Keep connected to this hotspot and the page will auto-recover.',
  wifi_waiting:'Waiting for connection...',wifi_switching:'Network switching, keep connected to this hotspot...',
  wifi_timeout:'Connection timeout. Find device IP in router admin, then visit http://&lt;IP&gt;',
  wifi_connected_title:'Connected to Network',wifi_ip_label:'Device IP: ',
  wifi_open_mgmt:'Open Admin Page ',wifi_access_hint:'Connect your device to the configured network and click the link above',
  wifi_fail:'Connection failed: ',wifi_hint:'Enter WiFi credentials and click Connect. Device IP will be shown once connected.',
  wifi_pw_ph:'Leave blank for open network'
}
};
var lang='zh';
function T(k){return(L[lang]&&L[lang][k])||L.zh[k]||k;}
function toggleLang(){setLang(lang==='zh'?'en':'zh');}
function setLang(l){
  lang=l;
  var li=document.getElementById('lang-icon');
  if(li){var t=li.querySelector('text');if(t)t.textContent=lang==='zh'?'中':'EN';}
  document.querySelectorAll('.nl[data-k]').forEach(function(e){
    var k=e.getAttribute('data-k');
    if(e.tagName==='OPTION')e.text=T(k)+(e.getAttribute('data-sfx')||''); else e.innerText=T(k);
  });
  document.querySelectorAll('[data-ph]').forEach(function(e){e.placeholder=T(e.getAttribute('data-ph'));});
  var advBtn=document.getElementById('llm-adv-toggle');
  if(advBtn){var advOpen=document.getElementById('llm-adv').style.display!=='none';advBtn.textContent=T('adv_settings')+(advOpen?' ▴':' ▾');}
  if(luaModData && luaModData.length)renderLuaModules();
  if(capGrpData && capGrpData.length)renderCapGroups();
  fetchStatus();
}
var wsConnected=false,lastWifi={wc:false,ap:false,ip:'',apip:''};
function syncConnDot(){var dot=document.getElementById('cdot'),ct=document.getElementById('ctxt');if(!dot)return;if(!wsConnected){dot.className='conn-dot';ct.innerText=T('ws_connecting');return;}if(lastWifi.ap){dot.className='conn-dot warn';ct.innerText='配网中 '+esc(lastWifi.apip);}else if(lastWifi.wc){dot.className='conn-dot ok';ct.innerText=T('conn')+' '+esc(lastWifi.ip);}else{dot.className='conn-dot';ct.innerText=T('noconn');}}
function updateConn(ok){wsConnected=ok;syncConnDot();var b=document.getElementById('ws-stat'),t=document.getElementById('ws-stat-t');if(b)b.className='ws-badge '+(ok?'ok':'con');if(t)t.innerText=ok?T('conn'):T('ws_connecting');var dis=!ok,ci=document.getElementById('ci'),sb=document.getElementById('send-btn');if(ci)ci.disabled=dis;if(sb)sb.disabled=dis;if(ok)fetchStatus();}
var pollConsecFail=0;
var EO='<svg xmlns="http://www.w3.org/2000/svg" width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M1 12s4-8 11-8 11 8 11 8-4 8-11 8-11-8-11-8z"/><circle cx="12" cy="12" r="3"/></svg>';
var EC='<svg xmlns="http://www.w3.org/2000/svg" width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M17.94 17.94A10.07 10.07 0 0 1 12 20c-7 0-11-8-11-8a18.45 18.45 0 0 1 5.06-5.94M9.9 4.24A9.12 9.12 0 0 1 12 4c7 0 11 8 11 8a18.5 18.5 0 0 1-2.16 3.19m-6.72-1.07a3 3 0 1 1-4.24-4.24"/><line x1="1" y1="1" x2="23" y2="23"/></svg>';
document.querySelectorAll('.eye').forEach(function(b){b.innerHTML=EO;});
function tpw(id,btn){var i=document.getElementById(id),s=i.type==='password';i.type=s?'text':'password';btn.innerHTML=s?EC:EO;}
function esc(s){return String(s).replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;').replace(/'/g,'&#39;');}
// Minimal self-contained Markdown renderer (no external deps).
// Input is HTML-escaped first, so generated tags are the only HTML present.
function mdInline(s){
  var codes=[];
  // protect inline code spans from further inline processing
  s=s.replace(/`([^`]+)`/g,function(_,c){codes.push(c);return ' '+(codes.length-1)+' ';});
  // links [text](url)
  s=s.replace(/\[([^\]]+)\]\((https?:[^\s)]+)\)/g,'<a href="$2" target="_blank" rel="noopener">$1</a>');
  // bold then italic
  s=s.replace(/\*\*([^*]+)\*\*/g,'<strong>$1</strong>');
  s=s.replace(/__([^_]+)__/g,'<strong>$1</strong>');
  s=s.replace(/\*([^*\s][^*]*)\*/g,'<em>$1</em>');
  s=s.replace(/(^|[^a-zA-Z0-9_])_([^_\s][^_]*)_/g,'$1<em>$2</em>');
  // restore code spans
  s=s.replace(/ (\d+) /g,function(_,n){return '<code>'+codes[n]+'</code>';});
  return s;
}
function mdToHtml(src){
  var lines=String(src).replace(/\r\n?/g,'\n').split('\n');
  var out=[],para=[],listType=null,i=0;
  function flushPara(){if(para.length){out.push('<p>'+para.map(function(l){return mdInline(esc(l));}).join('<br>')+'</p>');para=[];}}
  function closeList(){if(listType){out.push('</'+listType+'>');listType=null;}}
  function splitRow(r){return r.trim().replace(/^\|/,'').replace(/\|$/,'').split('|').map(function(c){return c.trim();});}
  while(i<lines.length){
    var line=lines[i];
    // fenced code block
    if(/^```/.test(line)){
      flushPara();closeList();
      var code=[];i++;
      while(i<lines.length&&!/^```/.test(lines[i])){code.push(lines[i]);i++;}
      i++; // skip closing fence
      out.push('<pre><code>'+esc(code.join('\n'))+'</code></pre>');
      continue;
    }
    // GFM table: header row + separator row
    if(line.indexOf('|')>=0&&i+1<lines.length&&/^\s*\|?[\s:|-]*-[\s:|-]*\|?\s*$/.test(lines[i+1])){
      flushPara();closeList();
      var th=splitRow(line).map(function(c){return '<th>'+mdInline(esc(c))+'</th>';}).join('');
      i+=2;var trs='';
      while(i<lines.length&&lines[i].indexOf('|')>=0&&lines[i].trim()!==''){
        trs+='<tr>'+splitRow(lines[i]).map(function(c){return '<td>'+mdInline(esc(c))+'</td>';}).join('')+'</tr>';i++;
      }
      out.push('<table><thead><tr>'+th+'</tr></thead><tbody>'+trs+'</tbody></table>');
      continue;
    }
    var h=line.match(/^(#{1,6})\s+(.*)$/);
    if(h){flushPara();closeList();out.push('<h'+h[1].length+'>'+mdInline(esc(h[2]))+'</h'+h[1].length+'>');i++;continue;}
    if(/^\s*([-*_])(\s*\1){2,}\s*$/.test(line)){flushPara();closeList();out.push('<hr>');i++;continue;}
    var bq=line.match(/^>\s?(.*)$/);
    if(bq){flushPara();closeList();out.push('<blockquote>'+mdInline(esc(bq[1]))+'</blockquote>');i++;continue;}
    var ul=line.match(/^\s*[-*+]\s+(.*)$/);
    if(ul){flushPara();if(listType!=='ul'){closeList();out.push('<ul>');listType='ul';}out.push('<li>'+mdInline(esc(ul[1]))+'</li>');i++;continue;}
    var ol=line.match(/^\s*\d+[.)]\s+(.*)$/);
    if(ol){flushPara();if(listType!=='ol'){closeList();out.push('<ol>');listType='ol';}out.push('<li>'+mdInline(esc(ol[1]))+'</li>');i++;continue;}
    if(/^\s*$/.test(line)){flushPara();closeList();i++;continue;}
    para.push(line);i++;
  }
  flushPara();closeList();
  return out.join('');
}
function fmtSz(b){if(b==null||b<0)return '-';if(b<1024)return b+'B';if(b<1048576)return (b/1024).toFixed(1)+'KB';return (b/1048576).toFixed(1)+'MB';}
function fmtTime(t){if(!t)return '-';var d=new Date(t*1000);return d.getFullYear()+'-'+String(d.getMonth()+1).padStart(2,'0')+'-'+String(d.getDate()).padStart(2,'0')+' '+String(d.getHours()).padStart(2,'0')+':'+String(d.getMinutes()).padStart(2,'0');}
function msg(id,ok,txt){var m=document.getElementById(id);m.className=ok?'mok':'mer';m.style.display='block';m.innerText=txt;}
