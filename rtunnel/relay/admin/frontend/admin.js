'use strict';
/* rtunnel admin 前端：侧边栏 + hash 路由，无框架单文件。
 * 模块划分：Api（fetch 封装）/ UI（渲染组件）/ Router（hash 路由 + 页面生命周期）/
 *           Pages.*（概览/设备/设备详情/客户端/会话，各带 enter/refresh/leave）。
 */

// ---------------------------------------------------------------- Api
const Api = {
  async req(path, opt = {}) {
    const headers = Object.assign({ 'X-Requested-With': 'fetch' }, opt.headers || {});
    const r = await fetch(path, Object.assign({ cache: 'no-store' }, opt, { headers }));
    if (r.status === 401) {
      // 账号体系下不再弹 token；回到登录页
      if (Auth.me) location.reload();
      throw new Error('unauthorized');
    }
    if (!r.ok) {
      let msg = 'HTTP ' + r.status;
      try { const j = await r.json(); if (j && j.error) msg = j.error; } catch (e) {}
      throw new Error(msg);
    }
    return r.json();
  },
  get(p) { return this.req(p); },
  post(p, body) {
    return this.req(p, { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body || {}) });
  },
  put(p, body) {
    return this.req(p, { method: 'PUT', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body || {}) });
  },
  del(p) { return this.req(p, { method: 'DELETE' }); },
};

// ---------------------------------------------------------------- UI 组件
function cssVar(name, def) {
  const v = getComputedStyle(document.documentElement).getPropertyValue(name).trim();
  return v || def;
}

function updateThemeBtn() {
  const b = document.getElementById('themebtn');
  if (!b) return;
  const p = (typeof Auth !== 'undefined' && Auth.profile) || {};
  const mode = p.theme || 'system';
  const color = p.theme_color;
  if (mode === 'color' && color) {
    b.innerHTML = '<i class="th-dot" style="background:' + color + '"></i>';
    b.title = '主题：自定义色';
    return;
  }
  const sun = '<svg viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><circle cx="12" cy="12" r="4"/><path d="M12 2v2M12 20v2M2 12h2M20 12h2M4.9 4.9l1.4 1.4M17.7 17.7l1.4 1.4M19.1 4.9l-1.4 1.4M6.3 17.7l-1.4 1.4"/></svg>';
  const moon = '<svg viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor" stroke-width="2" stroke-linejoin="round"><path d="M21 12.8A9 9 0 1 1 11.2 3a7 7 0 0 0 9.8 9.8z"/></svg>';
  b.innerHTML = mode === 'dark' ? moon : sun;
  b.title = mode === 'dark' ? '主题：深色' : '主题：浅色';
}

function tagTextWidth(ctx, text) {
  ctx.save();
  ctx.font = '10px -apple-system, "Segoe UI", sans-serif';
  const w = ctx.measureText(text).width + 10;
  ctx.restore();
  return w;
}

function drawTagPill(ctx, x, y, text, bg, fg) {
  const w = tagTextWidth(ctx, text);
  ctx.save();
  ctx.fillStyle = bg;
  if (ctx.roundRect) {
    ctx.beginPath(); ctx.roundRect(x, y - 8, w, 16, 3); ctx.fill();
  } else {
    ctx.fillRect(x, y - 8, w, 16);
  }
  ctx.font = '10px -apple-system, "Segoe UI", sans-serif';
  ctx.fillStyle = fg;
  ctx.textAlign = 'center';
  ctx.fillText(text, x + w / 2, y);
  ctx.restore();
  return w;
}

const _assetCache = {};
function loadScriptOnce(src) {
  if (_assetCache[src]) return _assetCache[src];
  _assetCache[src] = new Promise((res, rej) => {
    const el = document.createElement('script');
    el.src = src;
    el.onload = () => res();
    el.onerror = () => { delete _assetCache[src]; rej(new Error('load failed: ' + src)); };
    document.head.appendChild(el);
  });
  return _assetCache[src];
}

function loadCssOnce(href) {
  if (_assetCache[href]) return _assetCache[href];
  _assetCache[href] = new Promise((res, rej) => {
    const el = document.createElement('link');
    el.rel = 'stylesheet';
    el.href = href;
    el.onload = () => res();
    el.onerror = () => { delete _assetCache[href]; rej(new Error('load failed: ' + href)); };
    document.head.appendChild(el);
  });
  return _assetCache[href];
}

function fmtKbPair(usedKb, totalKb) {
  const k = Number(totalKb) || 0;
  let unit = 'K', div = 1;
  if (k >= 100 * 1024) { unit = 'G'; div = 1024 * 1024; }
  else if (k >= 1024) { unit = 'M'; div = 1024; }
  const f = v => {
    const n = (Number(v) || 0) / div;
    return Math.abs(n) >= 10 ? n.toFixed(1) : n.toFixed(2);
  };
  return { used: f(usedKb), total: f(totalKb), unit: unit };
}

function applyTheme(mode, color) {
  mode = (mode === 'dark' || mode === 'color') ? mode : 'light';
  document.documentElement.setAttribute('data-theme', mode);
  if (mode === 'color' && color) {
    document.documentElement.style.setProperty('--accent', color);
  } else {
    document.documentElement.style.removeProperty('--accent');
  }
  try {
    localStorage.setItem('rt_theme', mode);
    localStorage.setItem('rt_theme_color', color || '');
  } catch (e) {}
  updateThemeBtn();
}

function applySiteTitle() {
  const t = (window.SETTINGS || {})['ui.site_title'];
  if (t) {
    document.title = t;
    const logo = document.querySelector('#sidebar .logo');
    if (logo && logo.firstChild && logo.firstChild.nodeType === 3) {
      logo.firstChild.nodeValue = t;
    }
  }
}

function themeMenu() {
  const old = document.getElementById('themebox');
  if (old) old.remove();
  const top = document.getElementById('topbar');
  const ub = document.getElementById('userbox');
  const box = document.createElement('span');
  box.id = 'themebox';
  const colors = ['#3b82f6', '#57ab5a', '#f5a623', '#e5534b', '#b083f0', '#39c5cf', '#d4a72c'];
  box.innerHTML = '<button id="themebtn" title="主题">🎨</button>' +
    '<div id="thememenu">' +
      '<div class="th-modes">' +
        '<button data-mode="light">浅色</button>' +
        '<button data-mode="dark">深色</button>' +
        '<button data-mode="color">纯色</button>' +
      '</div>' +
      '<div class="th-swatches">' + colors.map(c =>
        '<i data-color="' + c + '" style="background:' + c + '"></i>').join('') + '</div>' +
      '<label class="th-custom">自定义 <input type="color" id="th-color"></label>' +
    '</div>';
  top.insertBefore(box, ub || null);

  const menu = box.querySelector('#thememenu');
  const profile = (Auth.profile) || {};
  box.querySelectorAll('.th-modes button').forEach(b => {
    if (b.dataset.mode === (profile.theme || 'light')) b.classList.add('active');
    b.onclick = async () => {
      const theme = b.dataset.mode;
      await saveTheme({ theme: theme === 'color' ? 'color' : theme },
                      theme, theme === 'color' ? (profile.theme_color || cssVar('--accent', '#3b82f6')) : '');
      box.querySelectorAll('.th-modes button').forEach(x => x.classList.remove('active'));
      b.classList.add('active');
    };
  });
  box.querySelectorAll('.th-swatches i').forEach(el => {
    el.onclick = async () => {
      await saveTheme({ theme: 'color', theme_color: el.dataset.color },
                      'color', el.dataset.color);
      box.querySelectorAll('.th-modes button').forEach(x => x.classList.toggle('active', x.dataset.mode === 'color'));
    };
  });
  const cp = box.querySelector('#th-color');
  if (cp) {
    cp.value = profile.theme_color || '#3b82f6';
    cp.onchange = async () => {
      await saveTheme({ theme: 'color', theme_color: cp.value }, 'color', cp.value);
      box.querySelectorAll('.th-modes button').forEach(x => x.classList.toggle('active', x.dataset.mode === 'color'));
    };
  }
  document.getElementById('themebtn').onclick = (e) => {
    e.stopPropagation();
    menu.style.display = menu.style.display === 'block' ? 'none' : 'block';
  };
  document.addEventListener('click', (e) => {
    if (!box.contains(e.target)) menu.style.display = 'none';
  });
}

function uploadLimitMb() {
  const mb = parseInt((window.SETTINGS || {})['file.max_upload_mb'] || '32', 10);
  return mb > 0 ? mb : 32;
}

async function saveTheme(patch, mode, color) {
  try {
    Auth.profile = Object.assign(Auth.profile || {}, patch);
    await Api.put('/api/v1/me/profile', patch);
  } catch (e) { UI.toast('主题保存失败：' + e.message, true); }
  applyTheme(mode, color);
  if (Router.current) Router.dispatch();
}

const ROLE_NAMES = { admin: '超级管理员', manager: '管理员', operator: '操作员', viewer: '浏览者' };

function avatarColor(name) {
    const palette = ['#4a9eff', '#57ab5a', '#f5a623', '#e5534b', '#b083f0', '#39c5cf', '#d4a72c', '#8b949e'];
    let h = 0;
    for (const ch of String(name || '?')) h = (h * 31 + ch.charCodeAt(0)) >>> 0;
    return palette[h % palette.length];
}

const UI = {
  esc(s) {
    return String(s == null ? '' : s).replace(/[&<>"']/g,
      c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
  },
  renderUser() {
    const me = (typeof Auth !== 'undefined' && Auth.me) || {};
    const name = me.display_name || me.username || '?';
    const color = me.avatar_color || avatarColor(me.username);
    const btn = document.getElementById('userbtn');
    if (btn) {
      btn.innerHTML = '<span class="avatar" style="background:' + color + '">' +
        UI.esc(String(name).slice(0, 1).toUpperCase()) + '</span><span class="uname">' +
        UI.esc(name) + '</span><span class="caret">▾</span>';
    }
    const info = document.getElementById('userinfo');
    if (info) info.textContent = name + ' · ' + (ROLE_NAMES[me.role] || me.role || '');
  },
  bytes(n) {
    n = Number(n) || 0;
    const u = ['B', 'K', 'M', 'G', 'T'];
    let i = 0;
    while (n >= 1024 && i < u.length - 1) { n /= 1024; i++; }
    return n.toFixed(i ? 1 : 0) + u[i];
  },
  ago(ms) {
    if (!ms) return '—';
    const s = Math.max(0, Math.floor((Date.now() - ms) / 1000));
    if (s < 60) return s + 's';
    if (s < 3600) return Math.floor(s / 60) + 'm';
    if (s < 86400) return Math.floor(s / 3600) + 'h';
    return Math.floor(s / 86400) + 'd';
  },
  duration(ms) {
    const s = Math.floor((Number(ms) || 0) / 1000);
    const d = Math.floor(s / 86400), h = Math.floor(s % 86400 / 3600), m = Math.floor(s % 3600 / 60);
    return (d ? d + 'd ' : '') + String(h).padStart(2, '0') + ':' + String(m).padStart(2, '0') + ':' + String(s % 60).padStart(2, '0');
  },
  panel(title, body, opts) {
    opts = opts || {};
    return '<div class="panel ' + (opts.cls || '') + '">' +
      '<div class="panel-head"><h2>' + title + '</h2>' +
      (opts.left || '') + '<span class="spacer"></span>' + (opts.right || '') +
      '</div>' + body + '</div>';
  },
  grid(rows) {
    return '<div class="sys-grid">' + rows.map(r =>
      '<div class="sys-item"><span class="k">' + r[0] + '</span>' +
      '<span class="v" title="' + r[1] + '">' + r[1] + '</span></div>').join('') + '</div>';
  },
  detailTable(cols, rows, opts) {
    opts = opts || {};
    const th = c => '<th>' + (String(c).charAt(0) === '<' ? c : UI.esc(c)) + '</th>';
    let h = '<div class="detail-table-shell"><table class="detail-table"><thead><tr>' +
      cols.map(th).join('') + '</tr></thead><tbody>';
    if (!rows.length) h += '<tr><td class="empty" colspan="' + cols.length + '">' + UI.esc(opts.empty || '暂无数据') + '</td></tr>';
    rows.forEach(r => { h += '<tr>' + r.map(c => '<td>' + c + '</td>').join('') + '</tr>'; });
    return h + '</tbody></table></div>';
  },
  kvTable(rows) {
    return '<div class="detail-table-shell"><table class="detail-table detail-kv"><tbody>' +
      rows.map(r => '<tr><th>' + r[0] + '</th><td>' + r[1] + '</td></tr>').join('') +
      '</tbody></table></div>';
  },
  table(cols, rows, onrow) {
    // 表头允许传 HTML（如排序链接）；以 '<' 开头的原样输出，其余转义
    const th = c => '<th>' + (String(c).charAt(0) === '<' ? c : UI.esc(c)) + '</th>';
    let h = '<table><thead><tr>' + cols.map(th).join('') + '</tr></thead><tbody>';
    if (!rows.length) {
      h += '<tr><td colspan="' + cols.length + '" class="empty">暂无数据</td></tr>';
    }
    rows.forEach((r, i) => {
      h += '<tr data-i="' + i + '">' + r.map(c => '<td>' + c + '</td>').join('') + '</tr>';
    });
    h += '</tbody></table>';
    if (onrow) {
      setTimeout(() => {
        document.querySelectorAll('#page tbody tr').forEach(tr => {
          tr.onclick = () => onrow(rows[tr.dataset.i]);
        });
      });
    }
    return h;
  },
  kv(obj) {
    let h = '<dl class="kv">';
    for (const k in obj) h += '<div><dt>' + UI.esc(k) + '</dt><dd>' + obj[k] + '</dd></div>';
    return h + '</dl>';
  },
  toast(msg, err) {
    const box = document.getElementById('toast');
    const el = document.createElement('div');
    el.className = 'item' + (err ? ' err' : '');
    el.textContent = msg;
    box.appendChild(el);
    setTimeout(() => el.remove(), 4000);
  },
};

// ---------------------------------------------------------------- Router
const Router = {
  routes: [],
  current: null,
  nav: [
    { hash: '#/overview', title: '概览', icon: '▣' },
    { hash: '#/agents', title: '设备 C', icon: '▤' },
    { hash: '#/clients', title: '客户端 A', icon: '▥' },
    { hash: '#/streams', title: '会话', icon: '⇄' },
    { hash: '#/certs', title: '证书', icon: '⚿' },
    { hash: '#/acl', title: 'ACL', icon: '☰' },
    { hash: '#/accounts', title: '账号', icon: '☺' },
    { hash: '#/files', title: '文件', icon: '⬇' },
    { hash: '#/tasks', title: '任务', icon: '⧉' },
    { hash: '#/audit', title: '审计', icon: '≣' },
    { hash: '#/settings', title: '设置', icon: '⚙' },
  ],
  register(re, page, title) { this.routes.push({ re, page, title }); },
  go(h) { location.hash = h; },
  renderNav(activeHash) {
    const role = (Auth.me && Auth.me.role) || 'viewer';
    const allow = role === 'viewer'
      ? ['#/overview', '#/agents']
      : role === 'operator'
        ? ['#/overview', '#/agents', '#/streams', '#/files']
        : role === 'manager'
          ? ['#/overview', '#/agents', '#/clients', '#/streams', '#/accounts', '#/acl', '#/files', '#/audit', '#/tasks']
          : ['#/overview', '#/agents', '#/clients', '#/streams', '#/certs', '#/acl', '#/accounts',
             '#/files', '#/audit', '#/settings', '#/tasks'];
    const navs = allow ? this.nav.filter(n => allow.indexOf(n.hash) >= 0) : this.nav;
    document.getElementById('nav').innerHTML = navs.map(n =>
      '<a href="' + n.hash + '"' + (activeHash.indexOf(n.hash) === 0 ? ' class="active"' : '') + '>' +
      '<span>' + n.icon + '</span><span class="txt">' + n.title + '</span></a>').join('');
  },
  async dispatch() {
    const h = location.hash || '#/overview';
    if (this.current && this.current.page.leave) this.current.page.leave();
    this.current = null;
    for (const r of this.routes) {
      const m = h.match(r.re);
      if (!m) continue;
      this.current = r;
      this.renderNav(h);
      document.getElementById('crumb').textContent = r.title;
      const el = document.getElementById('page');
      el.innerHTML = '<div class="empty">加载中…</div>';
      try {
        await r.page.enter(el, m.slice(1), h);
      } catch (e) {
        el.innerHTML = '<div class="empty">加载失败：' + UI.esc(e.message) + '</div>';
      }
      return;
    }
    this.renderNav(h);
    document.getElementById('crumb').textContent = '404';
    document.getElementById('page').innerHTML =
      '<div class="empty">页面不存在 <a href="#/overview">返回概览</a></div>';
  },
  start() {
    window.addEventListener('hashchange', () => this.dispatch());
    this.dispatch();
  },
};

// 页面基类：5s 轮询，leave 时清理
class Page {
  enter(el, params, hash) {
    this.el = el;
    return this.refresh().then(() => {
      const ms = typeof this.refreshInterval === 'function' ? this.refreshInterval() : 5000;
      if (ms > 0) this.timer = setInterval(() => this.refresh().catch(() => {}), ms);
    });
  }
  leave() { if (this.timer) clearInterval(this.timer); }
}

// ---------------------------------------------------------------- 权限
function roleOf() { return (Auth.me && Auth.me.role) || 'viewer'; }
function canOperate() { return ['admin', 'manager', 'operator'].indexOf(roleOf()) >= 0; }
function canManage() { return ['admin', 'manager'].indexOf(roleOf()) >= 0; }

// ---------------------------------------------------------------- Pages.概览
const OverviewPage = Object.assign(new Page(), {
  async refresh() {
    const o = await Api.get('/api/v1/overview');
    if (!Router.current || Router.current.page !== OverviewPage) return;
    this.el.innerHTML =
      '<div class="cards">' +
      [['在线设备', o.agents], ['在线客户端', o.clients], ['活跃会话', o.streams],
       ['累计下行', UI.bytes(o.bytes_in)], ['累计上行', UI.bytes(o.bytes_out)]]
        .map(c => '<div class="card"><div class="num">' + c[1] + '</div><div class="label">' + c[0] + '</div></div>').join('') +
      '</div>' +
      '<div class="panel">' + UI.kv({
        '版本': UI.esc(o.version),
        '运行时长': UI.duration(o.uptime_ms),
        'mTLS': o.mtls ? '<span class="pill ok">开启</span>' : '<span class="pill warn">关闭</span>',
        'ACL': o.allow_all ? '<span class="pill warn">allow-all</span>' : '<span class="pill info">按表授权</span>',
      }) + '</div>';
    document.getElementById('relay-info').textContent = 'v' + o.version + ' · uptime ' + UI.duration(o.uptime_ms);
    document.getElementById('side-foot').innerHTML =
      '<span class="dot ok"></span>relay 在线 · ' + o.agents + ' 设备 · ' + o.streams + ' 会话';
  },
});

// ---------------------------------------------------------------- Pages.设备列表
const AgentsPage = Object.assign(new Page(), {
  async refresh() {
    const arr = await Api.get('/api/v1/agents');
    const rows = arr.map(a => [
      '<b>' + UI.esc(a.devid) + '</b>',
      '<span class="pill ok">在线</span>',
      UI.esc(a.remote_addr),
      UI.ago(a.last_seen_ms) + ' 前活跃',
      a.streams,
      UI.bytes(a.bytes_in) + ' / ' + UI.bytes(a.bytes_out),
      (canOperate()
        ? '<button class="web" data-devid="' + UI.esc(a.devid) + '">Web</button> ' +
          (a.caps_shell !== 0
            ? '<button class="shell" data-devid="' + UI.esc(a.devid) + '">Shell</button> ' : '') : '') +
      (canManage()
        ? '<button class="bind" data-devid="' + UI.esc(a.devid) + '">绑定</button> ' +
          '<button class="kick" data-devid="' + UI.esc(a.devid) + '">下线</button>' : ''),
    ]);
    this.el.innerHTML = '<div class="panel"><div class="panel-head"><h2>设备（C 侧）</h2>' +
      '<span class="spacer"></span><input id="ag-q" class="tty-search" placeholder="搜索 devid / 地址">' +
      '<span>共 ' + arr.length + ' 台 · 5s 自动刷新</span></div>' +
      UI.table(['devid', '状态', '远端地址', '最近活跃', '会话数', '流量 ↓/↑', '操作'], rows) + '</div>';
    const aq = document.getElementById('ag-q');
    if (aq) aq.oninput = () => {
      const q = aq.value.trim().toLowerCase();
      document.querySelectorAll('#page tbody tr').forEach(tr => {
        tr.style.display = !q || tr.innerText.toLowerCase().indexOf(q) >= 0 ? '' : 'none';
      });
    };
    document.querySelectorAll('#page button.web').forEach(btn => {
      btn.onclick = (e) => {
        e.stopPropagation();
        const port = prompt('设备 Web 端口（需在 agent --allow 列表）', '80');
        if (!port) return;
        window.open('/device/' + encodeURIComponent(btn.dataset.devid) + '/' + encodeURIComponent(port) + '/', '_blank');
      };
    });
    document.querySelectorAll('#page button.shell').forEach(btn => {
      btn.onclick = (e) => {
        e.stopPropagation();
        Router.go('#/terminal/' + encodeURIComponent(btn.dataset.devid));
      };
    });
    document.querySelectorAll('#page tbody tr').forEach((tr, i) => {
      tr.onclick = () => Router.go('#/agents/' + encodeURIComponent(arr[i].devid));
    });
    document.querySelectorAll('#page button.bind').forEach(btn => {
      btn.onclick = async (e) => {
        e.stopPropagation();
        const owner = prompt('绑定到账号（留空 = 解绑）', '');
        if (owner === null) return;
        try {
          await Api.put('/api/v1/devices/' + encodeURIComponent(btn.dataset.devid) + '/owner', { username: owner });
          UI.toast(owner ? ('已绑定到 ' + owner) : '已解绑');
        } catch (err) { UI.toast('绑定失败：' + err.message, true); }
      };
    });
    document.querySelectorAll('#page button.kick').forEach(btn => {
      btn.onclick = async (e) => {
        e.stopPropagation();
        if (!confirm('强制下线 ' + btn.dataset.devid + '？（agent 会退避重连）')) return;
        try {
          await Api.post('/api/v1/agents/' + encodeURIComponent(btn.dataset.devid) + '/kick');
          UI.toast('已下线 ' + btn.dataset.devid);
          this.refresh();
        } catch (err) { UI.toast('操作失败：' + err.message, true); }
      };
    });
  },
});

// ---------------------------------------------------------------- Pages.个人设置
const ProfilePage = Object.assign(new Page(), {
  refreshInterval() { return 0; },
  async refresh() {
    const p = await Api.get('/api/v1/me/profile');
    const colors = ['#4a9eff', '#57ab5a', '#f5a623', '#e5534b', '#b083f0', '#39c5cf', '#d4a72c', '#8b949e'];
    this.el.innerHTML =
      '<div class="panel" style="max-width:560px"><div class="panel-head"><h2>个人设置</h2></div>' +
      '<div class="cfg-body">' +
        '<label class="cfg-row"><span>头像颜色</span><input type="color" id="pf-color" value="' +
          UI.esc(p.avatar_color || colors[0]) + '"><em>首字母 ' + UI.esc(p.username.slice(0, 1).toUpperCase()) + '</em></label>' +
        '<label class="cfg-row"><span>显示名</span><input type="text" id="pf-name" value="' +
          UI.esc(p.display_name || '') + '"></label>' +
        '<label class="cfg-row"><span>默认页面</span><select id="pf-page">' +
          '<option value="overview"' + (p.default_page === 'overview' ? ' selected' : '') + '>概览</option>' +
          '<option value="agents"' + (p.default_page === 'agents' ? ' selected' : '') + '>设备列表</option></select></label>' +
        '<label class="cfg-row"><span>时间格式</span><select id="pf-time">' +
          '<option value="local"' + (p.time_format === 'local' ? ' selected' : '') + '>本地时间</option>' +
          '<option value="relative"' + (p.time_format === 'relative' ? ' selected' : '') + '>相对时间</option></select></label>' +
        '<label class="cfg-row"><span>终端字号</span><input type="number" id="pf-font" value="' +
          UI.esc(p.term_font || '14') + '" min="10" max="24"><em>px</em></label>' +
      '</div><div class="cfg-actions"><button class="primary" id="pf-save">保存</button></div></div>' +

      '<div class="panel" style="max-width:560px;margin-top:16px"><div class="panel-head"><h2>修改密码</h2></div>' +
      '<div class="cfg-body">' +
        '<label class="cfg-row"><span>当前密码</span><input type="password" id="pw-old"></label>' +
        '<label class="cfg-row"><span>新密码</span><input type="password" id="pw-new"></label>' +
      '</div><div class="cfg-actions"><button id="pw-save">修改密码</button></div></div>' +

      '<div class="panel" style="max-width:560px;margin-top:16px"><div class="panel-head"><h2>会话</h2></div>' +
      '<div class="cfg-body"><div style="color:var(--muted);font-size:12px">吊销本账号全部登录会话（含当前）</div></div>' +
      '<div class="cfg-actions"><button class="danger" id="sess-drop">登出所有会话</button></div></div>';

    document.getElementById('pf-save').onclick = async () => {
      const body = {
        avatar_color: document.getElementById('pf-color').value,
        display_name: document.getElementById('pf-name').value,
        default_page: document.getElementById('pf-page').value,
        time_format: document.getElementById('pf-time').value,
        term_font: document.getElementById('pf-font').value,
      };
      try {
        Auth.profile = await Api.put('/api/v1/me/profile', body);
        Auth.me.display_name = Auth.profile.display_name;
        Auth.me.avatar_color = Auth.profile.avatar_color;
        Auth.profile = Auth.profile || {};
        UI.renderUser();
        UI.toast('已保存');
      } catch (e) { UI.toast('保存失败：' + e.message, true); }
    };
    document.getElementById('pw-save').onclick = async () => {
      try {
        await Api.put('/api/v1/users/' + Auth.me.id + '/password', {
          old_password: document.getElementById('pw-old').value,
          new_password: document.getElementById('pw-new').value,
        });
        UI.toast('密码已修改');
        document.getElementById('pw-old').value = '';
        document.getElementById('pw-new').value = '';
      } catch (e) { UI.toast('修改失败：' + e.message, true); }
    };
    document.getElementById('sess-drop').onclick = async () => {
      if (!confirm('确定登出所有会话？')) return;
      try {
        await Api.post('/api/v1/me/logout-all', {});
        try { localStorage.removeItem('rt_theme'); localStorage.removeItem('rt_theme_color'); } catch (e) {}
        location.reload();
      }
      catch (e) { UI.toast('失败：' + e.message, true); }
    };
  },
  enter(el) { this.el = el; return Page.prototype.enter.call(this, el, []); },
});


// ---------------------------------------------------------------- 启动
Router.register(/^#\/overview$/, OverviewPage, '概览');
Router.register(/^#\/agents$/, AgentsPage, '设备 C');
// ---------------------------------------------------------------- Pages.账号
const AccountsPage = Object.assign(new Page(), {
  refreshInterval() { return 0; },
  async refresh() {
    const me = Auth.me || {};
    const arr = await Api.get('/api/v1/users');
    const canM = (u) => u.role !== 'admin' && u.username !== me.username &&
      (me.role === 'admin' || (me.role === 'manager' && (u.role === 'operator' || u.role === 'viewer')));
    const rows = arr.map(u => {
      let ops = '<button class="pw" data-id="' + u.id + '" data-name="' + UI.esc(u.username) + '">改密</button> ';
      if (canM(u)) {
        ops += '<button class="role" data-id="' + u.id + '" data-name="' + UI.esc(u.username) + '">改角色</button> ';
        ops += '<button class="st" data-id="' + u.id + '" data-st="' + (u.status === 'active' ? 'disabled' : 'active') + '">' +
               (u.status === 'active' ? '禁用' : '启用') + '</button> ';
        ops += '<button class="del" data-id="' + u.id + '" data-name="' + UI.esc(u.username) + '">删除</button>';
      }
      return ['<b>' + UI.esc(u.username) + '</b>', UI.esc(u.role), UI.esc(u.status),
              new Date(u.created_ms).toLocaleString(), ops];
    });
    const roleOpts = me.role === 'admin'
      ? '<option value="viewer">浏览者</option><option value="operator">操作员</option><option value="manager">管理员</option>'
      : '<option value="viewer">浏览者</option><option value="operator">操作员</option>';
    const roleBadge = r => '<span class="pill ' + (r === 'admin' ? 'warn' : r === 'manager' ? 'info' : '') + '">' +
      ({admin:'管理员',manager:'管理员',operator:'操作员',viewer:'浏览者'}[r] || r) + '</span>';
    const rows2 = arr.map(u => [
      '<b>' + UI.esc(u.username) + '</b>', roleBadge(u.role),
      u.status === 'active' ? '<span class="pill ok">启用</span>' : '<span class="pill bad">禁用</span>',
      new Date(u.created_ms).toLocaleString(),
      (() => {
        let ops = '<button class="pw" data-id="' + u.id + '" data-name="' + UI.esc(u.username) + '">改密</button> ';
        if (canM(u)) {
          ops += '<button class="role" data-id="' + u.id + '">改角色</button> ';
          ops += '<button class="st" data-id="' + u.id + '" data-st="' + (u.status === 'active' ? 'disabled' : 'active') + '">' +
                 (u.status === 'active' ? '禁用' : '启用') + '</button> ';
          ops += '<button class="del" data-id="' + u.id + '" data-name="' + UI.esc(u.username) + '">删除</button>';
        }
        return ops;
      })(),
    ]);
    this.el.innerHTML =
      '<div class="panel"><div class="panel-head"><h2>账号列表</h2><span class="spacer"></span>' +
      '<input id="au-q" class="tty-search" placeholder="搜索用户名">' +
      '<span>共 ' + arr.length + ' 个</span> ' +
      '<button class="primary" id="nu-open">+ 新建账号</button></div>' +
      UI.table(['用户名', '角色', '状态', '创建时间', '操作'], rows2) + '</div>';

    const aq = document.getElementById('au-q');
    if (aq) aq.oninput = () => {
      const q = aq.value.trim().toLowerCase();
      document.querySelectorAll('#page tbody tr').forEach(tr => {
        tr.style.display = !q || tr.innerText.toLowerCase().indexOf(q) >= 0 ? '' : 'none';
      });
    };
    document.getElementById('nu-open').onclick = () => {
      const mask = document.createElement('div');
      mask.className = 'tty-mask';
      mask.innerHTML = '<div class="tty-modal"><h2>新建账号</h2>' +
        '<label>用户名<input id="nu-name" placeholder="用户名"></label>' +
        '<label>密码<input id="nu-pass" type="password" placeholder="至少 8 位"></label>' +
        '<label>角色<select id="nu-role">' + roleOpts + '</select></label>' +
        '<div class="tty-btns"><button id="nu-cancel">取消</button>' +
        '<button class="primary" id="nu-btn">创建</button></div></div>';
      document.body.appendChild(mask);
      const close = () => mask.remove();
      mask.querySelector('#nu-cancel').onclick = close;
      mask.onclick = e => { if (e.target === mask) close(); };
      mask.querySelector('#nu-btn').onclick = async () => {
        const username = mask.querySelector('#nu-name').value.trim();
        const password = mask.querySelector('#nu-pass').value;
        const role = mask.querySelector('#nu-role').value;
        if (!username) { UI.toast('用户名不能为空', true); return; }
        if (password.length < 8) { UI.toast('密码至少 8 位', true); return; }
        try {
          await Api.post('/api/v1/users', { username, password, role });
          UI.toast('已创建 ' + username);
          close();
          this.refresh();
        } catch (e) { UI.toast('创建失败：' + e.message, true); }
      };
    };
    document.querySelectorAll('#page button.pw').forEach(btn => {
      btn.onclick = async () => {
        const p = prompt('新密码（' + btn.dataset.name + '，≥8位）');
        if (!p) return;
        try { await Api.put('/api/v1/users/' + btn.dataset.id + '/password', { new_password: p }); UI.toast('已修改'); }
        catch (e) { UI.toast('失败：' + e.message, true); }
      };
    });
    document.querySelectorAll('#page button.role').forEach(btn => {
      btn.onclick = async () => {
        const r = prompt('新角色（viewer/operator/manager）');
        if (!r) return;
        try { await Api.put('/api/v1/users/' + btn.dataset.id + '/role', { role: r }); this.refresh(); }
        catch (e) { UI.toast('失败：' + e.message, true); }
      };
    });
    document.querySelectorAll('#page button.st').forEach(btn => {
      btn.onclick = async () => {
        try { await Api.put('/api/v1/users/' + btn.dataset.id + '/status', { status: btn.dataset.st }); this.refresh(); }
        catch (e) { UI.toast('失败：' + e.message, true); }
      };
    });
    document.querySelectorAll('#page button.del').forEach(btn => {
      btn.onclick = async () => {
        if (!confirm('删除账号 ' + btn.dataset.name + '？（设备会自动解绑）')) return;
        try { await Api.del('/api/v1/users/' + btn.dataset.id); this.refresh(); }
        catch (e) { UI.toast('失败：' + e.message, true); }
      };
    });
  },
});

// ---------------------------------------------------------------- 认证引导
const Auth = { me: null };

function authForm(kind) {
  const nav = document.getElementById('nav');
  if (nav) nav.innerHTML = '';
  const el = document.getElementById('page');
  const isSetup = kind === 'setup';
  el.innerHTML = '<div class="panel" style="max-width:420px;margin:60px auto"><div class="panel-head"><h2>' +
    (isSetup ? '初始化超级管理员' : '登录') + '</h2></div><div style="padding:20px;display:flex;flex-direction:column;gap:10px">' +
    (isSetup ? '<input id="af-token" placeholder="部署 token" type="password">' : '') +
    '<input id="af-user" placeholder="用户名" autocomplete="username">' +
    '<input id="af-pass" placeholder="密码" type="password" autocomplete="current-password">' +
    '<button class="primary" id="af-btn">' + (isSetup ? '创建并登录' : '登录') + '</button>' +
    '<span id="af-msg" style="color:var(--muted)"></span></div></div>';
  document.getElementById('af-btn').onclick = async () => {
    const msg = document.getElementById('af-msg');
    const body = { username: document.getElementById('af-user').value.trim(),
                   password: document.getElementById('af-pass').value };
    try {
      const call = async (path, data) => {
        const r = await fetch(path, { method: 'POST', headers: { 'Content-Type': 'application/json' },
                                      body: JSON.stringify(data), cache: 'no-store' });
        if (!r.ok) throw new Error((await r.json()).error || ('HTTP ' + r.status));
      };
      if (isSetup) {
        body.token = document.getElementById('af-token').value.trim();
        await call('/api/v1/setup', body);
      }
      await call('/api/v1/login', body);
      location.reload();
    } catch (e) { msg.textContent = e.message; }
  };
}

async function boot() {
  try {
    const st = await (await fetch('/api/v1/setup/status', { cache: 'no-store' })).json();
    if (st.initialized === false) return authForm('setup');
  } catch (e) { return authForm('login'); }
  try {
    Auth.me = await Api.get('/api/v1/me');
  } catch (e) {
    return authForm('login');
  }
  // 顶栏当前账号 + 登出菜单
  const ub = document.getElementById('userbox');
  const btn = document.getElementById('userbtn');
  if (ub && btn) {
    ub.style.display = '';
    try {
      Auth.profile = await Api.get('/api/v1/me/profile');
      Auth.me.display_name = Auth.profile.display_name;
      Auth.me.avatar_color = Auth.profile.avatar_color;
    } catch (e) {}
    try {
      window.SETTINGS = await Api.get('/api/v1/settings');
      applySiteTitle();
    } catch (e) {}
    const menu = document.getElementById('usermenu');
    if (menu && !document.getElementById('menu-profile')) {
      menu.insertAdjacentHTML('afterbegin', '<a href="#/profile" id="menu-profile">个人设置</a>');
    }
    Auth.profile = Auth.profile || {};
    applyTheme(Auth.profile.theme, Auth.profile.theme_color);
    Auth.profile = Auth.profile || {};
    applyTheme(Auth.profile.theme, Auth.profile.theme_color);
    UI.renderUser();
    themeMenu();
    updateThemeBtn();
    try {
      const o = await Api.get('/api/v1/overview');
      const sf = document.getElementById('side-foot');
      if (sf) sf.innerHTML = '<span class="dot ok"></span>relay 在线 · ' +
        o.agents + ' 设备 · ' + o.streams + ' 会话';
    } catch (e) {}
    btn.onclick = (e) => {
      e.stopPropagation();
      if (menu) menu.style.display = menu.style.display === 'none' ? '' : 'none';
    };
    document.addEventListener('click', () => {
      if (menu) menu.style.display = 'none';
    });
    const lo = document.getElementById('logout');
    if (lo) lo.onclick = async () => {
      try { await fetch('/api/v1/logout', { method: 'POST', cache: 'no-store' }); } catch (e) {}
      try { localStorage.removeItem('rt_theme'); localStorage.removeItem('rt_theme_color'); } catch (e) {}
      location.reload();
    };
  }
  if (!location.hash) {
    const want = (Auth.profile && Auth.profile.default_page) === 'agents' ? '#/agents' : '#/overview';
    location.hash = want;
  }
  Router.start();
}

Router.register(/^#\/accounts$/, AccountsPage, '账号');
Router.register(/^#\/profile$/, ProfilePage, '个人设置');
window.addEventListener('load', boot);
