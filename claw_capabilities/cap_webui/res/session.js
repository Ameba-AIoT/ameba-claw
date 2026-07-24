// Session list management
function buildSessionItem(alias, isCurrent, preview) {
  var li = document.createElement('li');
  li.dataset.alias = alias;
  li.dataset.preview = preview || '';
  if (isCurrent) li.classList.add('active');

  var inner = document.createElement('div');
  inner.className = 's-inner';
  inner.addEventListener('click', function () {
    document.querySelectorAll('#session-list li').forEach(function (el) { el.classList.remove('active'); });
    li.classList.add('active');
    switchToAlias(alias);
  });

  var title = document.createElement('div');
  title.className = 's-title';
  title.textContent = preview || '新的对话';

  var sub = document.createElement('div');
  sub.className = 's-sub';
  sub.textContent = alias;

  inner.appendChild(title);
  inner.appendChild(sub);

  var more = document.createElement('span');
  more.className = 's-more';
  more.textContent = '⋯';
  more.addEventListener('click', function (e) {
    e.stopPropagation();
    showSessionMenu(li, alias);
  });

  li.appendChild(inner);
  li.appendChild(more);
  return li;
}

function loadSessionList() {
  fetch('/api/session').then(function (r) { return r.json(); }).then(function (d) {
    var list = document.getElementById('session-list');
    if (!list) return;
    var active = sessionStorage.getItem('claw_alias') || '';
    list.innerHTML = '';
    (d.sessions || []).forEach(function (s) {
      var alias = typeof s === 'object' ? (s.alias || '') : s;
      var preview = typeof s === 'object' ? (s.preview || '') : '';
      list.appendChild(buildSessionItem(alias, alias === active, preview));
    });
  }).catch(function () {});
}

function onSessionDeleted(alias) {
  var item = document.querySelector('#session-list [data-alias="' + alias + '"]');
  if (item) item.parentNode.removeChild(item);
}

function showSessionMenu(li, alias) {
  var existing = document.getElementById('s-menu-popup');
  if (existing) existing.parentNode.removeChild(existing);

  var menu = document.createElement('div');
  menu.id = 's-menu-popup';
  menu.style.cssText = 'position:absolute;background:#fff;border:1px solid #ddd;border-radius:4px;box-shadow:0 2px 8px rgba(0,0,0,.15);z-index:999;min-width:100px;';

  function menuItem(label, cb) {
    var item = document.createElement('div');
    item.textContent = label;
    item.style.cssText = 'padding:8px 14px;cursor:pointer;font-size:13px;';
    item.addEventListener('mouseenter', function () { item.style.background = '#f5f5f5'; });
    item.addEventListener('mouseleave', function () { item.style.background = ''; });
    item.addEventListener('click', function (e) {
      e.stopPropagation();
      menu.parentNode.removeChild(menu);
      cb();
    });
    return item;
  }

  menu.appendChild(menuItem('重命名', function () { startRename(li, alias); }));
  menu.appendChild(menuItem('删除', function () { deleteSession(alias); }));

  document.body.appendChild(menu);
  var rect = li.getBoundingClientRect();
  menu.style.top = (rect.bottom + window.scrollY) + 'px';
  menu.style.left = (rect.left + window.scrollX) + 'px';

  function dismiss(e) {
    if (!menu.contains(e.target)) {
      if (menu.parentNode) menu.parentNode.removeChild(menu);
      document.removeEventListener('click', dismiss);
    }
  }
  setTimeout(function () { document.addEventListener('click', dismiss); }, 0);
}

function startRename(li, alias) {
  var titleEl = li.querySelector('.s-sub');
  var inp = document.createElement('input');
  inp.type = 'text';
  inp.value = alias;
  inp.style.cssText = 'width:140px;font-size:13px;border:1px solid #bbb;border-radius:3px;padding:1px 4px;';
  titleEl.replaceWith(inp);
  inp.focus();
  inp.select();

  function submit() {
    var newAlias = inp.value.trim();
    if (!newAlias || newAlias === alias) { loadSessionList(); return; }
    // rename switches the *current* session's alias; first resume if needed
    var doRename = function () {
      fetch('/api/session', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ action: 'rename', alias: newAlias })
      }).then(function (r) { return r.json(); }).then(function (d) {
        if (d.ok) loadSessionList();
        else loadSessionList();
      }).catch(function () { loadSessionList(); });
    };
    var currentAlias = sessionStorage.getItem('claw_alias') || '';
    if (currentAlias !== alias) {
      fetch('/api/session', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ action: 'resume', alias: alias })
      }).then(function () { doRename(); }).catch(function () { loadSessionList(); });
    } else {
      doRename();
    }
  }

  inp.addEventListener('blur', submit);
  inp.addEventListener('keydown', function (e) {
    if (e.key === 'Enter') { inp.blur(); }
    else if (e.key === 'Escape') { loadSessionList(); }
  });
}

function deleteSession(alias) {
  fetch('/api/session?alias=' + encodeURIComponent(alias), { method: 'DELETE' })
    .then(function (r) { return r.json(); })
    .then(function (d) {
      if (!d.ok) return;
      onSessionDeleted(alias);
      var current = sessionStorage.getItem('claw_alias') || '';
      if (current === alias) {
        sessionStorage.removeItem('claw_alias');
        if (typeof clearChat === 'function') clearChat();
        loadSessionList();
      }
    }).catch(function () {});
}

var btnNewSession = document.getElementById('btn-new-session');
if (btnNewSession) {
  btnNewSession.addEventListener('click', function () {
    fetch('/api/session', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ action: 'new' })
    }).then(function (r) { return r.json(); }).then(function (d) {
      if (!d.ok || !d.alias) return;
      var list = document.getElementById('session-list');
      if (list) list.appendChild(buildSessionItem(d.alias, false));
      sessionStorage.setItem('claw_alias', d.alias);
      if (typeof switchToAlias === 'function') switchToAlias(d.alias);
    }).catch(function () {});
  });
}
