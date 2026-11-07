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

function applyTheme(mode, color) {
  mode = mode || 'system';
  const dark = mode === 'dark' || (mode === 'system' &&
    window.matchMedia('(prefers-color-scheme: dark)').matches);
  document.documentElement.setAttribute('data-theme', dark ? 'dark' : 'light');
  if (color) document.documentElement.style.setProperty('--accent', color);
  else document.documentElement.style.removeProperty('--accent');
  try {
    localStorage.setItem('rt_theme', mode);
    localStorage.setItem('rt_theme_color', color || '');
  } catch (e) {}
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
        '<button data-mode="system">跟随系统</button>' +
      '</div>' +
      '<div class="th-swatches">' + colors.map(c =>
        '<i data-color="' + c + '" style="background:' + c + '"></i>').join('') + '</div>' +
      '<label class="th-custom">自定义 <input type="color" id="th-color"></label>' +
    '</div>';
  top.insertBefore(box, ub || null);

  const menu = box.querySelector('#thememenu');
  const profile = (Auth.profile) || {};
  box.querySelectorAll('.th-modes button').forEach(b => {
    if (b.dataset.mode === (profile.theme || 'system')) b.classList.add('active');
    b.onclick = async () => {
      const theme = b.dataset.mode;
      await saveTheme({ theme }, theme, profile.theme_color);
      box.querySelectorAll('.th-modes button').forEach(x => x.classList.remove('active'));
      b.classList.add('active');
    };
  });
  box.querySelectorAll('.th-swatches i').forEach(el => {
    el.onclick = async () => {
      await saveTheme({ theme_color: el.dataset.color },
                      profile.theme || 'system', el.dataset.color);
    };
  });
  const cp = box.querySelector('#th-color');
  if (cp) {
    cp.value = profile.theme_color || '#3b82f6';
    cp.onchange = async () => {
      await saveTheme({ theme_color: cp.value },
                      profile.theme || 'system', cp.value);
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
          ? ['#/overview', '#/agents', '#/clients', '#/streams', '#/accounts', '#/acl', '#/files', '#/audit']
          : ['#/overview', '#/agents', '#/clients', '#/streams', '#/certs', '#/acl', '#/accounts',
             '#/files', '#/audit', '#/settings'];
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
      '<span class="spacer"></span><span>共 ' + arr.length + ' 台 · 5s 自动刷新</span></div>' +
      UI.table(['devid', '状态', '远端地址', '最近活跃', '会话数', '流量 ↓/↑', '操作'], rows) + '</div>';
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

// ---------------------------------------------------------------- Pages.设备详情
const AgentDetailPage = Object.assign(new Page(), {
  refreshInterval() { return 0; },   // 由「每 5s 刷新」勾选框控制
  enter(el, params) {
    this.devid = decodeURIComponent(params[0] || '');
    this.cpuHist = [];
    this.memHist = [];
    this.charts = [];
    this.auto = Number((window.SETTINGS || {})['ui.refresh_seconds'] || 0) > 0;
    const p = Page.prototype.enter.call(this, el, params);
    if (this.auto && !this.timer) this.timer = setInterval(() => this.refresh().catch(() => {}), 5000);
    return p;
  },
  leave() {
    if (this.timer) { clearInterval(this.timer); this.timer = null; }
  },
  spark(id, data, color) {
    const c = document.getElementById(id);
    if (!c || !window.Chart) return;
    this.charts.push(new Chart(c.getContext('2d'), {
      type: 'line',
      data: { labels: data.map((_, i) => i),
        datasets: [{ data: data, borderColor: color, backgroundColor: color + '26',
          fill: true, pointRadius: 0, borderWidth: 2, tension: .3 }] },
      options: { animation: false, responsive: true,
        scales: { x: { display: false }, y: { display: false, min: 0, max: 100 } },
        plugins: { legend: { display: false }, tooltip: { enabled: false } } }
    }));
  },
  diskColor(p) { return p >= 85 ? cssVar('--bad', '#e5534b') : p >= 70 ? cssVar('--warn', '#f5a623') : cssVar('--accent', '#4a9eff'); },
  async refresh() {
    const d = await Api.get('/api/v1/agents/' + encodeURIComponent(this.devid));
    let si = null, siTs = 0;
    try {
      const r = await Api.get('/api/v1/agents/' + encodeURIComponent(this.devid) + '/sysinfo?refresh=1');
      si = r.sysinfo; siTs = r.ts;
    } catch (e) {}

    const rows = (d.streams || []).map(s => [
      UI.esc(s.a_user) + ':' + s.a_sid, s.c_sid,
      s.opened ? '<span class="pill ok">OPEN</span>' : '<span class="pill info">OPENING</span>',
      UI.ago(s.opened_at_ms),
    ]);

    let html = '<div class="panel" style="margin-bottom:16px"><div class="panel-head"><h2>' + UI.esc(d.devid) + '</h2>' +
      '<span class="pill ok">在线</span><span class="spacer"></span>' +
      (si ? '<label class="si-auto"><input type="checkbox" id="si-auto"' + (this.auto ? ' checked' : '') +
            '> 每 5s 刷新</label> <button id="si-export">导出 JSON</button>' : '') + '</div>' + UI.kv({
        '远端地址': UI.esc(d.remote_addr),
        '接入时间': new Date(d.connected_at_ms).toLocaleString(),
        '最近活跃': UI.ago(d.last_seen_ms) + ' 前',
        '流量 ↓/↑': UI.bytes(d.bytes_in) + ' / ' + UI.bytes(d.bytes_out),
      }) + '</div>';

    if (si) {
      this.charts.forEach(c => { try { c.destroy(); } catch (e) {} });
      this.charts = [];
      const kb = n => UI.bytes((Number(n) || 0) * 1024);
      const total = Number(si.mem_total_kb) || 0;
      const avail = Number(si.mem_available_kb) || 0;
      const memPct = total > 0 ? Math.round((total - avail) * 100 / total) : 0;
      const cpu = si.cpu || { usage: 0, cores: [] };
      this.memHist.push(memPct); if (this.memHist.length > 60) this.memHist.shift();
      this.cpuHist.push(cpu.usage || 0); if (this.cpuHist.length > 60) this.cpuHist.shift();

      const mounts = si.mounts || [];
      const maxTotal = mounts.reduce((m, x) => Math.max(m, Number(x.total_kb) || 0), 1);
      const labels = mounts.map(m => m.mount);
      const usedData = mounts.map(m => (Number(m.total_kb) || 0) - (Number(m.free_kb) || 0));
      const freeData = mounts.map(m => Number(m.free_kb) || 0);
      const usedColors = mounts.map(m => {
        const t = Number(m.total_kb) || 0;
        return this.diskColor(t > 0 ? Math.round((t - (Number(m.free_kb) || 0)) * 100 / t) : 0);
      });
      const diskLegend = mounts.map(m => {
        const t = Number(m.total_kb) || 0, f = Number(m.free_kb) || 0;
        const p = t > 0 ? Math.round((t - f) * 100 / t) : 0;
        return '<div class="si-disk-row"><span class="nm">' + UI.esc(m.mount) + '</span>' +
          '<span class="val">' + kb(t - f) + ' / ' + kb(t) + '</span>' +
          '<span class="pct" style="color:' + this.diskColor(p) + '">' + p + '%</span></div>';
      }).join('');

      html += '<div class="si-grid">' +
        '<div class="panel"><div class="panel-head"><h2>内存</h2>' +
        '<span class="spacer"></span><span class="si-num">' + memPct + '%</span></div>' +
        '<div class="si-body"><canvas id="si-mem" height="56"></canvas>' +
        '<div class="si-legend">已用 ' + kb(total - avail) + ' / ' + kb(total) +
        ' · Swap ' + kb(si.swap_total_kb) + '</div></div></div>' +
        '<div class="panel"><div class="panel-head"><h2>CPU</h2>' +
        '<span class="spacer"></span><span class="si-num">' + (cpu.usage || 0).toFixed(0) + '%</span></div>' +
        '<div class="si-body"><canvas id="si-cpu" height="56"></canvas>' +
        '<div class="si-legend">load ' + (si.load1 || 0).toFixed(2) + ' / ' +
        (si.load5 || 0).toFixed(2) + ' / ' + (si.load15 || 0).toFixed(2) +
        ' · ' + (cpu.cores || []).length + ' 核</div></div></div></div>';

      html += '<div class="panel" style="margin-bottom:16px"><div class="panel-head"><h2>存储</h2>' +
        '<span class="spacer"></span><span style="color:var(--muted);font-size:12px">条长 = 分区总量 / 最大分区总量</span></div>' +
        '<div class="si-body"><canvas id="si-disk" height="100"></canvas>' + diskLegend + '</div></div>';

      const certText = si.mtls
        ? UI.kv({ '证书主体': UI.esc(si.cert_subject || ''), '签发者': UI.esc(si.cert_issuer || ''),
                  '序列号': UI.esc(String(si.cert_serial || '')), '状态': si.cert_valid ? '有效' : '已过期' })
        : '<div style="padding:12px;color:var(--muted)">未启用 mTLS</div>';
      const caps = si.caps || {};
      const chips = [];
      (caps.ports || []).forEach(p => chips.push('TCP ' + p));
      if (caps.shell) chips.push('Shell');
      if (caps.sysinfo) chips.push('SYSINFO');
      chips.push(caps.mtls ? 'mTLS' : 'mTLS 未启用');
      const capHtml = chips.map(c => '<span class="chip">' + UI.esc(c) + '</span>').join(' ');
      const runtimeTable = '<table class="si-table">' +
        '<tr><th>agent 版本</th><td>' + UI.esc(si.version || '') + '</td></tr>' +
        '<tr><th>能力</th><td>' + capHtml +
        (canManage() ? ' <button class="caps-edit" id="caps-edit">编辑</button>' : '') + '</td></tr>' +
        '<tr><th>启动参数</th><td><code style="font-size:12px">' + UI.esc(si.args || '') + '</code></td></tr>' +
        '</table>';
      const sysRows = [
        ['架构', si.machine || '—'],
        ['内核', si.kernel || '—'],
        ['CPU 型号', si.cpu_model || '—'],
        ['核心数', ((cpu.cores || []).length) + ' 核'],
        ['板卡型号', si.board || '—'],
        ['采集时间', new Date(siTs).toLocaleString()],
      ];
      const sysTable = '<table class="si-table">' + sysRows.map(r =>
        '<tr><th>' + UI.esc(r[0]) + '</th><td>' + UI.esc(r[1]) + '</td></tr>').join('') + '</table>';
      const nics = (si.nics || []).map(n => [UI.esc(n.name), UI.esc(n.ip), UI.esc(n.netmask),
        n.up ? '<span class="pill ok">up</span>' : '<span class="pill bad">down</span>']);
      html += '<div class="panel" style="margin-bottom:16px"><div class="panel-head"><h2>网卡</h2></div>' +
        UI.table(['接口', 'IP', '掩码', '状态'], nics) + '</div>' +
        '<div class="panel" style="margin-bottom:16px"><div class="panel-head"><h2>系统</h2></div>' + sysTable + '</div>' +
        '<div class="panel" style="margin-bottom:16px"><div class="panel-head"><h2>运行时</h2></div>' + runtimeTable + '</div>' +
        '<div class="panel" style="margin-bottom:16px"><div class="panel-head"><h2>证书</h2></div>' + certText + '</div>';
    }

    html += '<div class="panel"><div class="panel-head"><h2>当前会话</h2></div>' +
      UI.table(['A 用户:sid', 'c_sid', '状态', '开启于'], rows) + '</div>';
    this.el.innerHTML = html;

    if (si) {
      const self = this;
      this.spark('si-mem', this.memHist, cssVar('--accent', '#4a9eff'));
      this.spark('si-cpu', this.cpuHist, cssVar('--warn', '#f5a623'));
      const c = document.getElementById('si-disk');
      if (c && window.Chart) {
        const labels = (si.mounts || []).map(m => m.mount);
        const used = (si.mounts || []).map(m => (Number(m.total_kb) || 0) - (Number(m.free_kb) || 0));
        const free = (si.mounts || []).map(m => Number(m.free_kb) || 0);
        const maxT = (si.mounts || []).reduce((a, m) => Math.max(a, Number(m.total_kb) || 0), 1);
        const colors = (si.mounts || []).map(m => {
          const t = Number(m.total_kb) || 0, f = Number(m.free_kb) || 0;
          return self.diskColor(t > 0 ? Math.round((t - f) * 100 / t) : 0);
        });
        this.charts.push(new Chart(c.getContext('2d'), {
          type: 'bar',
          data: { labels: labels, datasets: [
            { label: '已用', data: used, backgroundColor: colors, minBarLength: 2, stack: 's' },
            { label: '空闲', data: free, backgroundColor: cssVar('--border', '#2b3038'), stack: 's' },
          ] },
          options: { indexAxis: 'y', animation: false, responsive: true,
            scales: { x: { stacked: true, max: maxT, display: false },
                      y: { stacked: true, ticks: { color: cssVar('--chart-axis', '#8b929c') }, grid: { display: false } } },
            plugins: { legend: { display: false } } }
        }));
      }
      const ce = document.getElementById('caps-edit');
      if (ce) ce.onclick = () => {
        const caps = si.caps || {};
        const shellAllowed = roleOf() === 'admin';
        const ov = document.createElement('div');
        ov.className = 'modal-mask';
        ov.innerHTML = '<div class="modal"><h3>编辑能力</h3>' +
          '<label class="fld">转发端口（逗号分隔，1–65535，最多 32 个）' +
          '<input id="caps-ports" value="' + UI.esc((caps.ports || []).join(', ')) + '"></label>' +
          '<label class="fld"><input type="checkbox" id="caps-shell"' + (caps.shell ? ' checked' : '') +
          (shellAllowed ? '' : ' disabled') + '> 允许 Shell' +
          (shellAllowed ? '' : '（仅 admin 可改）') + '</label>' +
          '<div class="modal-actions"><button id="caps-cancel">取消</button>' +
          '<button class="primary" id="caps-save">保存并下发</button></div>' +
          '<div id="caps-msg" class="modal-msg"></div></div>';
        document.body.appendChild(ov);
        const close = () => ov.remove();
        document.getElementById('caps-cancel').onclick = close;
        document.getElementById('caps-save').onclick = async () => {
          const msg = document.getElementById('caps-msg');
          const raw = document.getElementById('caps-ports').value;
          const ports = raw.split(',').map(x => x.trim()).filter(Boolean).map(x => Number(x));
          if (ports.some(x => !Number.isInteger(x) || x <= 0 || x > 65535)) {
            msg.textContent = '端口必须是 1–65535 的整数'; return;
          }
          const body = { ports: ports };
          if (shellAllowed) body.shell = document.getElementById('caps-shell').checked;
          msg.textContent = '下发中…';
          try {
            await Api.put('/api/v1/agents/' + encodeURIComponent(this.devid) + '/caps', body);
            close();
            UI.toast('能力已更新');
            this.refresh();
          } catch (e) { msg.textContent = '失败：' + e.message; }
        };
      };
      const cb = document.getElementById('si-auto');
      if (cb) cb.onchange = () => {
        this.auto = cb.checked;
        if (this.auto) {
          this.timer = setInterval(() => this.refresh().catch(() => {}), 5000);
          this.refresh().catch(() => {});
        } else if (this.timer) {
          clearInterval(this.timer);
          this.timer = null;
        }
      };
      const btn = document.getElementById('si-export');
      if (btn) btn.onclick = () => {
        const blob = new Blob([JSON.stringify(si, null, 2)], { type: 'application/json' });
        const a = document.createElement('a');
        a.href = URL.createObjectURL(blob);
        a.download = self.devid + '-sysinfo.json';
        a.click();
        URL.revokeObjectURL(a.href);
      };
    }
  },
});

// ---------------------------------------------------------------- Pages.客户端列表
const ClientsPage = Object.assign(new Page(), {
  async refresh() {
    const arr = await Api.get('/api/v1/clients');
    const rows = arr.map(c => [
      '<b>' + UI.esc(c.user) + '</b>', UI.esc(c.remote_addr),
      UI.ago(c.last_seen_ms) + ' 前活跃', c.streams,
      UI.bytes(c.bytes_in) + ' / ' + UI.bytes(c.bytes_out),
      '<button class="shell" data-user="' + UI.esc(c.user) + '">Shell</button>',
    ]);
    this.el.innerHTML = '<div class="panel"><div class="panel-head"><h2>客户端（A 侧）</h2>' +
      '<span class="spacer"></span><span>共 ' + arr.length + ' 个</span></div>' +
      UI.table(['用户', '远端地址', '最近活跃', '会话数', '流量 ↓/↑', '操作'], rows) + '</div>';
    document.querySelectorAll('#page button.web').forEach(btn => {
      btn.onclick = (e) => {
        e.stopPropagation();
        const port = prompt('设备 Web 端口（需在 agent --allow 列表）', '80');
        if (!port) return;
        window.open('/device/' + encodeURIComponent(btn.dataset.devid) + '/' + encodeURIComponent(port) + '/', '_blank');
      };
    });
    document.querySelectorAll('#page button.shell').forEach(btn => {
      btn.onclick = async () => {
        const user = btn.dataset.user;
        try {
          // A 侧按需 agent：devid = a:<user>，relay 会通知该 client 在本机拉起 agent
          Router.go('#/terminal/' + encodeURIComponent('a:' + user));
        } catch (e) { UI.toast('打开失败：' + e.message, true); }
      };
    });
  },
});

// ---------------------------------------------------------------- Pages.会话列表
const StreamsPage = Object.assign(new Page(), {
  async refresh() {
    const arr = await Api.get('/api/v1/streams');
    const pill = s => s === 'OPEN' ? '<span class="pill ok">OPEN</span>'
      : s === 'OPENING' ? '<span class="pill info">OPENING</span>'
      : '<span class="pill warn">HALF_CLOSED</span>';
    const rows = arr.map(s => [
      UI.esc(s.a_user) + ':' + s.a_sid,
      UI.esc(s.c_devid) + ':' + s.c_sid,
      pill(s.state), UI.ago(s.opened_at_ms),
      '<button class="kick" data-sid="' + s.a_sid + '">踢掉</button>',
    ]);
    this.el.innerHTML = '<div class="panel"><div class="panel-head"><h2>会话</h2>' +
      '<span class="spacer"></span><span>共 ' + arr.length + ' 条</span></div>' +
      UI.table(['A 用户:a_sid', 'C 设备:c_sid', '状态', '开启于', '操作'], rows) + '</div>';
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
      btn.onclick = async () => {
        if (!confirm('踢掉会话 a_sid=' + btn.dataset.sid + '？（双侧 RESET）')) return;
        try {
          await Api.del('/api/v1/streams/' + btn.dataset.sid);
          UI.toast('已踢掉 ' + btn.dataset.sid);
          this.refresh();
        } catch (err) { UI.toast('操作失败：' + err.message, true); }
      };
    });
  },
});

// ---------------------------------------------------------------- Pages.证书
const CertsPage = Object.assign(new Page(), {
  // 页面有输入框（SAN/CSR），自动重渲染会清空正在输入的内容
  refreshInterval() { return 0; },
  async refresh() {
    const arr = await Api.get('/api/v1/certs');
    const pill = s => s === 'valid' ? '<span class="pill ok">有效</span>'
      : s === 'expiring' ? '<span class="pill warn">即将到期</span>'
      : s === 'expired' ? '<span class="pill bad">已过期</span>'
      : '<span class="pill bad">已吊销</span>';
    const rows = arr.map(c => [
      '<b>' + UI.esc(c.san) + '</b>', UI.esc(c.role),
      '0x' + Number(c.serial).toString(16).toUpperCase(),
      c.expire_ms ? new Date(c.expire_ms).toLocaleDateString() : '—',
      pill(c.status),
      c.status === 'revoked' ? '' : '<button class="revoke" data-san="' + UI.esc(c.san) + '">吊销</button>',
    ]);
    this.el.innerHTML =
      '<div class="panel" style="margin-bottom:16px"><div class="panel-head"><h2>签发新证书</h2>' +
      '<span class="spacer"></span><button id="certs-refresh">刷新列表</button></div>' +
      '<div style="padding:16px;display:flex;gap:10px;align-items:center;flex-wrap:wrap">' +
      'SAN <input id="issue-san" placeholder="PILE-000200" size="18">' +
      '类型 <select id="issue-role"><option value="device">设备</option><option value="user">用户</option></select>' +
      '有效期(天) <input id="issue-days" type="number" value="30" min="1" max="3650" style="width:80px">' +
      '<button class="primary" id="issue-btn">签发</button>' +
      '<span id="issue-result" style="color:var(--muted)"></span></div></div>' +
      '<div class="panel" style="margin-bottom:16px"><div class="panel-head"><h2>CSR 签名（私钥不出设备）</h2></div>' +
      '<div style="padding:16px"><textarea id="csr-input" rows="6" style="width:100%;font-family:monospace" placeholder="粘贴 CSR PEM（设备侧生成，SAN 或 CN 为身份）"></textarea>' +
      '<div style="margin-top:8px;display:flex;gap:10px;align-items:center">有效期(天) <input id="csr-days" type="number" value="30" style="width:80px">' +
      '<button id="csr-btn">签名</button><span id="csr-result" style="color:var(--muted)"></span></div></div></div>' +
      '<div class="panel"><div class="panel-head"><h2>证书台账</h2>' +
      '<span class="spacer"></span><span>共 ' + arr.length + ' 张</span></div>' +
      UI.table(['SAN(身份)', '类型', '序列号', '到期日', '状态', '操作'], rows) + '</div>';

    const cr = document.getElementById('certs-refresh');
    if (cr) cr.onclick = () => this.refresh();
    document.getElementById('issue-btn').onclick = async () => {
      const san = document.getElementById('issue-san').value.trim();
      const role = document.getElementById('issue-role').value;
      const days = parseInt(document.getElementById('issue-days').value, 10) || 30;
      if (!san) { UI.toast('请填写 SAN', true); return; }
      try {
        const r = await Api.post('/api/v1/certs/issue', { san, role, days });
        document.getElementById('issue-result').innerHTML =
          '已签发 ' + UI.esc(r.san) + '：' +
          '<a id="dl-crt" href="#">下载 crt</a> · <a id="dl-key" href="#">下载 key（仅此一次）</a>';
        const dl = (id, name, text) => {
          const url = URL.createObjectURL(new Blob([text], { type: 'application/x-pem-file' }));
          const a = document.getElementById(id);
          a.href = url; a.download = name;
        };
        dl('dl-crt', r.san + '.crt', r.cert);
        dl('dl-key', r.san + '.key', r.key);
        UI.toast('已签发 ' + r.san);
        this.refresh();
      } catch (e) { UI.toast('签发失败：' + e.message, true); }
    };
    document.querySelectorAll('#page button.revoke').forEach(btn => {
      btn.onclick = async () => {
        if (!confirm('吊销 ' + btn.dataset.san + '？立即生效：在线连接被踢、后续 AUTH 被拒')) return;
        try {
          await Api.post('/api/v1/certs/revoke', { san: btn.dataset.san });
          UI.toast('已吊销 ' + btn.dataset.san);
          this.refresh();
        } catch (e) { UI.toast('吊销失败：' + e.message, true); }
      };
    });
    document.getElementById('csr-btn').onclick = async () => {
      const csr = document.getElementById('csr-input').value;
      const days = parseInt(document.getElementById('csr-days').value, 10) || 30;
      if (!csr.trim()) { UI.toast('请粘贴 CSR', true); return; }
      try {
        const r = await Api.post('/api/v1/certs/sign', { csr, days });
        document.getElementById('csr-result').innerHTML =
          '已签发 ' + UI.esc(r.san) + '：<a id="dl-csr-crt" href="#">下载 crt</a>';
        const url = URL.createObjectURL(new Blob([r.cert], { type: 'application/x-pem-file' }));
        const a = document.getElementById('dl-csr-crt');
        a.href = url;
        a.download = r.san + '.crt';
        UI.toast('已签发 ' + r.san);
        this.refresh();
      } catch (e) { UI.toast('CSR 签名失败：' + e.message, true); }
    };
  },
});

// ---------------------------------------------------------------- Pages.ACL
const AclPage = Object.assign(new Page(), {
  // textarea 正在编辑时不能被 5s 刷新覆盖
  refreshInterval() { return 0; },
  async refresh() {
    const d = await Api.get('/api/v1/acl/file');
    this.el.innerHTML =
      '<div class="panel" style="margin-bottom:16px"><div class="panel-head"><h2>ACL 文件（' + UI.esc(d.path || '未配置') + '）</h2>' +
      '<span class="spacer"></span>' +
      '<button id="acl-reload">重新加载</button> <button class="primary" id="acl-save">保存并热加载</button></div>' +
      '<div style="padding:16px"><textarea id="acl-content" rows="12" style="width:100%;font-family:monospace" ' +
      'placeholder="user=devid[,devid|*]，每行一条，# 注释">' + UI.esc(d.content) + '</textarea>' +
      '<div style="color:var(--muted);margin-top:8px">保存 = 写文件 + 热加载，只影响新 OPEN，不动已建立会话；解析失败保留旧表。</div></div></div>' +
      '<div class="panel"><div class="panel-head"><h2>命令行条目（只读）</h2></div>' +
      UI.table(['条目'], (d.cli || []).map(x => [UI.esc(x)])) + '</div>';
    document.getElementById('acl-save').onclick = async () => {
      try {
        const r = await Api.put('/api/v1/acl/file', { content: document.getElementById('acl-content').value });
        UI.toast('已保存，' + r.entries + ' 条授权生效');
      } catch (e) { UI.toast('保存失败：' + e.message, true); }
    };
    document.getElementById('acl-reload').onclick = async () => {
      try {
        await Api.post('/api/v1/acl/reload');
        UI.toast('已重新加载');
      } catch (e) { UI.toast('重载失败：' + e.message, true); }
    };
  },
});

// ---------------------------------------------------------------- Pages.审计
const AuditPage = Object.assign(new Page(), {
  async refresh() {
    const arr = await Api.get('/api/v1/audit');
    this.el.innerHTML = '<div class="panel"><div class="panel-head"><h2>审计日志</h2>' +
      '<span class="spacer"></span><span>最近 ' + arr.length + ' 条（新到旧）</span></div>' +
      UI.table(['时间 / 角色 / 操作 / 详情'], arr.map(x => ['<code>' + UI.esc(x) + '</code>'])) + '</div>';
  },
});

// ---------------------------------------------------------------- Pages.文件
const FilesPage = Object.assign(new Page(), {
  // 不做 5s 自动刷新：重渲染会重建 <input type=file>，用户选好的文件会丢
  refreshInterval() { return 0; },
  enter(el, params, hash) {
    const q = new URLSearchParams((hash || '').split('?')[1] || '');
    this.sort = q.get('sort') || 'name';
    this.order = q.get('order') || 'asc';
    return Page.prototype.enter.call(this, el, params, hash);
  },
  sortLink(field, label) {
    // 首次点大小/时间按降序（大的、新的在前），名称按升序
    let order = (field === 'size' || field === 'mtime') ? 'desc' : 'asc';
    if (this.sort === field) order = this.order === 'asc' ? 'desc' : 'asc';
    const cur = this.sort === field ? (this.order === 'asc' ? ' ▲' : ' ▼') : '';
    return '<a href="#/files?sort=' + field + '&order=' + order +
      '" style="color:inherit;text-decoration:none">' + label + cur + '</a>';
  },
  async refresh() {
    const arr = await Api.get('/api/v1/files?sort=' + this.sort + '&order=' + this.order);
    const rows = arr.map(f => [
      '<b>' + UI.esc(f.name) + '</b>', UI.bytes(f.size),
      new Date(f.mtime_ms).toLocaleString(),
      '<button class="dl" data-name="' + UI.esc(f.name) + '">下载</button> ' +
      (canManage() ? '<button class="del" data-name="' + UI.esc(f.name) + '">删除</button>' : ''),
    ]);
    this.el.innerHTML =
      '<div class="panel" style="margin-bottom:16px"><div class="panel-head"><h2>上传文件</h2>' +
      '<span class="spacer"></span><button id="files-refresh">刷新列表</button></div>' +
      '<div style="padding:16px;display:flex;gap:10px;align-items:center;flex-wrap:wrap">' +
      '<input type="file" id="upload-input"> <button class="primary" id="upload-btn">上传</button>' +
      '<progress id="upload-progress" max="100" value="0" style="width:220px;display:none"></progress>' +
      '<span id="upload-status" style="color:var(--muted)"></span>' +
      '<span style="color:var(--muted)">单文件上限 32 MiB</span></div></div>' +
      '<div class="panel"><div class="panel-head"><h2>文件列表</h2><span class="spacer"></span>' +
      '<span>共 ' + arr.length + ' 个</span></div>' +
      UI.table([this.sortLink('name', '名称'), this.sortLink('size', '大小'),
                this.sortLink('mtime', '修改时间'), '操作'], rows) + '</div>';

    document.getElementById('files-refresh').onclick = () => this.refresh();
    const MAX_UPLOAD = 32 * 1024 * 1024;
    document.getElementById('upload-btn').onclick = () => {
      const btn = document.getElementById('upload-btn');
      const fi = document.getElementById('upload-input').files[0];
      const status = document.getElementById('upload-status');
      const bar = document.getElementById('upload-progress');
      if (!fi) { UI.toast('请选择文件', true); return; }
      // 客户端先挡：不要等传完 32MiB 才报错
      if (fi.size > MAX_UPLOAD) {
        status.textContent = '文件 ' + UI.bytes(fi.size) + ' 超过 32 MiB 上限';
        UI.toast('文件太大（' + UI.bytes(fi.size) + '），上限 32 MiB', true);
        return;
      }
      const xhr = new XMLHttpRequest();
      xhr.open('POST', '/api/v1/files/' + encodeURIComponent(fi.name));
      xhr.setRequestHeader('X-Requested-With', 'fetch');
      bar.style.display = '';
      bar.value = 0;
      status.textContent = '上传中…';
      btn.disabled = true;
      xhr.upload.onprogress = (e) => {
        if (!e.lengthComputable) return;
        bar.value = Math.floor(e.loaded * 100 / e.total);
        status.textContent = '上传中… ' + UI.bytes(e.loaded) + ' / ' + UI.bytes(e.total);
      };
      xhr.onload = () => {
        btn.disabled = false;
        bar.style.display = 'none';
        if (xhr.status === 200) {
          status.textContent = '已上传 ' + fi.name + '（' + UI.bytes(fi.size) + '）';
          this.refresh();
        } else {
          status.textContent = '';
          UI.toast('上传失败：HTTP ' + xhr.status + ' ' + xhr.responseText, true);
        }
      };
      xhr.onerror = () => {
        btn.disabled = false;
        bar.style.display = 'none';
        status.textContent = '';
        UI.toast('上传失败：网络错误', true);
      };
      xhr.send(fi);
    };
    document.querySelectorAll('#page button.dl').forEach(btn => {
      btn.onclick = async () => {
        try {
          const headers = {};
          const r = await fetch('/api/v1/files/' + encodeURIComponent(btn.dataset.name), { headers, cache: 'no-store' });
          if (!r.ok) throw new Error('HTTP ' + r.status);
          const blob = await r.blob();
          const url = URL.createObjectURL(blob);
          const a = document.createElement('a');
          a.href = url;
          a.download = btn.dataset.name;
          a.click();
          URL.revokeObjectURL(url);
        } catch (e) { UI.toast('下载失败：' + e.message, true); }
      };
    });
    document.querySelectorAll('#page button.del').forEach(btn => {
      btn.onclick = async () => {
        if (!confirm('删除 ' + btn.dataset.name + '？')) return;
        try {
          await Api.del('/api/v1/files/' + encodeURIComponent(btn.dataset.name));
          UI.toast('已删除 ' + btn.dataset.name);
          this.refresh();
        } catch (e) { UI.toast('删除失败：' + e.message, true); }
      };
    });
  },
});

// ---------------------------------------------------------------- Pages.终端（xterm.js + WebSocket）
// 终端会话管理器：切页面不断开，支持多个 tab
const TermMgr = {
  sessions: new Map(),
  seq: 0,
  open(devid) {
    const id = devid + '#' + (++this.seq);
    const box = document.createElement('div');
    box.style.cssText = 'height:100%;width:100%';
    const term = new Terminal({ cursorBlink: true, fontSize: parseInt((Auth.profile && Auth.profile.term_font) || 14, 10) });
    const fit = new FitAddon.FitAddon();
    term.loadAddon(fit);
    const proto = location.protocol === 'https:' ? 'wss' : 'ws';
    const ws = new WebSocket(proto + '://' + location.host + '/ws/shell?devid=' + encodeURIComponent(devid));
    const s = { id, devid, term, fit, ws, box, opened: false, alive: true };
    ws.onmessage = e => term.write(e.data);
    ws.onclose = () => { s.alive = false; try { term.write('\r\n[连接已关闭]\r\n'); } catch (e) {} };
    term.onData(d => { if (ws.readyState === 1) ws.send(d); });
    term.onResize(({ cols, rows }) => {
      if (ws.readyState === 1) ws.send(JSON.stringify({ type: 'resize', cols, rows }));
    });
    this.sessions.set(id, s);
    return s;
  },
  aliveOf(devid) {
    let found = null;
    for (const s of this.sessions.values()) if (s.devid === devid && s.alive) found = s;
    return found;
  },
  close(id) {
    const s = this.sessions.get(id);
    if (!s) return;
    s.alive = false;
    try { s.ws.close(); } catch (e) {}
    try { s.term.dispose(); } catch (e) {}
    s.box.remove();
    this.sessions.delete(id);
  },
};

const TerminalPage = Object.assign(new Page(), {
  enter(el, params) {
    this.el = el;
    const devid = decodeURIComponent((params[0] || '').split('?')[0]);
    if (devid) {
      let target = TermMgr.aliveOf(devid);
      if (!target) target = TermMgr.open(devid);
      this.active = target.id;
    }
    this.render();
    this._rs = () => this.fitActive();
    window.addEventListener('resize', this._rs);
  },
  fitActive() {
    const s = TermMgr.sessions.get(this.active);
    if (!s) return;
    try { s.fit.fit(); } catch (e) {}
  },
  render() {
    const ids = [...TermMgr.sessions.keys()];
    if (!ids.length) { Router.go('#/agents'); return; }
    if (!TermMgr.sessions.has(this.active)) this.active = ids[ids.length - 1];
    const cur = TermMgr.sessions.get(this.active);
    const tabs = ids.map(id => {
      const s = TermMgr.sessions.get(id);
      return '<button class="term-tab' + (id === this.active ? ' active' : '') + '" data-id="' + id + '">' +
        UI.esc(s.devid) + (s.alive ? '' : ' ✕') + '</button>';
    }).join('');
    this.el.innerHTML = '<div class="panel"><div class="panel-head"><h2>终端</h2>' +
      '<span class="spacer"></span><span class="term-tabs">' + tabs + '</span> ' +
      '<button id="term-new">新开</button> <button id="term-close">关闭当前</button></div>' +
      '<div id="term-host" style="height:calc(100vh - 220px);background:#000;padding:8px"></div></div>';
    const host = document.getElementById('term-host');
    if (!cur.opened) { cur.term.open(cur.box); cur.opened = true; }
    host.appendChild(cur.box);
    document.querySelectorAll('#page button.term-tab').forEach(btn => {
      btn.onclick = () => { this.active = btn.dataset.id; this.render(); };
    });
    document.getElementById('term-new').onclick = () => {
      const d = prompt('设备 devid', cur.devid);
      if (!d) return;
      this.active = TermMgr.open(d.trim()).id;
      this.render();
    };
    document.getElementById('term-close').onclick = () => {
      TermMgr.close(this.active);
      this.active = null;
      this.render();
    };
    setTimeout(() => this.fitActive(), 0);
  },
  leave() {
    if (this._rs) { window.removeEventListener('resize', this._rs); this._rs = null; }
    const s = TermMgr.sessions.get(this.active);
    if (s && s.box.parentNode) s.box.parentNode.removeChild(s.box);   // 仅摘 DOM，会话保留
  },
});


// ---------------------------------------------------------------- Pages.系统配置
const SettingsPage = Object.assign(new Page(), {
  refreshInterval() { return 0; },
  async refresh() {
    const st = await Api.get('/api/v1/settings');
    const num = (k, label, lo, hi, unit) =>
      '<label class="cfg-row"><span>' + label + '</span><input type="number" data-k="' + k +
      '" value="' + UI.esc(st[k] || '') + '" min="' + lo + '" max="' + hi + '"><em>' + unit + '</em></label>';
    this.el.innerHTML =
      '<div class="panel" style="margin-bottom:16px"><div class="panel-head"><h2>文件</h2></div><div class="cfg-body">' +
        num('file.max_upload_mb', '单文件上传上限', 1, 512, 'MiB') +
        num('file.max_download_mb', '单文件下载上限', 1, 512, 'MiB') + '</div></div>' +
      '<div class="panel" style="margin-bottom:16px"><div class="panel-head"><h2>代理</h2></div><div class="cfg-body">' +
        num('proxy.max_concurrent', '并发上限', 1, 256, '条') +
        num('proxy.sysinfo_wait_ms', 'SYSINFO 等待', 500, 10000, 'ms') +
        num('proxy.config_wait_ms', 'CONFIG_ACK 等待', 500, 10000, 'ms') + '</div></div>' +
      '<div class="panel" style="margin-bottom:16px"><div class="panel-head"><h2>账号安全</h2></div><div class="cfg-body">' +
        num('auth.session_hours', '会话有效期', 1, 168, '小时') +
        num('auth.password_min', '密码最小长度', 6, 32, '位') +
        num('auth.lock_fails', '登录失败锁定', 1, 20, '次') +
        num('auth.lock_minutes', '锁定时长', 1, 120, '分钟') + '</div></div>' +
      '<div class="panel" style="margin-bottom:16px"><div class="panel-head"><h2>终端</h2></div><div class="cfg-body">' +
        num('term.max_sessions', '单账号最大终端数', 1, 32, '个') + '</div></div>' +
      '<div class="panel" style="margin-bottom:16px"><div class="panel-head"><h2>审计</h2></div><div class="cfg-body">' +
        num('audit.retention_days', '审计保留', 7, 365, '天') + '</div></div>' +
      '<div class="panel"><div class="panel-head"><h2>展示</h2></div><div class="cfg-body">' +
        '<label class="cfg-row"><span>站点标题</span><input type="text" data-k="ui.site_title" value="' +
        UI.esc(st['ui.site_title'] || 'rtunnel') + '"></label>' +
        num('ui.refresh_seconds', '详情默认刷新', 0, 300, '秒（0=关闭）') +
        '</div><div class="cfg-actions"><button class="primary" id="cfg-save">保存</button></div></div>';
    document.getElementById('cfg-save').onclick = async () => {
      const body = {};
      document.querySelectorAll('#page .cfg-body [data-k]').forEach(inp => { body[inp.dataset.k] = inp.value; });
      try {
        await Api.put('/api/v1/settings', body);
        UI.toast('已保存，立即生效');
        this.refresh();
      } catch (e) { UI.toast('保存失败：' + e.message, true); }
    };
  },
  enter(el) { this.el = el; return Page.prototype.enter.call(this, el, []); },
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
    applyTheme(Auth.profile.theme, Auth.profile.theme_color);
    UI.renderUser();
    themeMenu();
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
      try { await Api.post('/api/v1/me/logout-all', {}); location.reload(); }
      catch (e) { UI.toast('失败：' + e.message, true); }
    };
  },
  enter(el) { this.el = el; return Page.prototype.enter.call(this, el, []); },
});

// ---------------------------------------------------------------- 启动
Router.register(/^#\/overview$/, OverviewPage, '概览');
Router.register(/^#\/agents$/, AgentsPage, '设备 C');
Router.register(/^#\/agents\/([^/]+)$/, AgentDetailPage, '设备详情');
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
    this.el.innerHTML =
      '<div class="panel" style="margin-bottom:16px"><div class="panel-head"><h2>创建账号</h2></div>' +
      '<div style="padding:16px;display:flex;gap:10px;align-items:center;flex-wrap:wrap">' +
      '<input id="nu-name" placeholder="用户名" size="14"> ' +
      '<input id="nu-pass" type="password" placeholder="密码（≥8位）" size="16"> ' +
      '<select id="nu-role">' + roleOpts + '</select> ' +
      '<button class="primary" id="nu-btn">创建</button><span id="nu-result" style="color:var(--muted)"></span></div></div>' +
      '<div class="panel"><div class="panel-head"><h2>账号列表</h2><span class="spacer"></span>' +
      '<span>共 ' + arr.length + ' 个</span></div>' +
      UI.table(['用户名', '角色', '状态', '创建时间', '操作'], rows) + '</div>';

    document.getElementById('nu-btn').onclick = async () => {
      const username = document.getElementById('nu-name').value.trim();
      const password = document.getElementById('nu-pass').value;
      const role = document.getElementById('nu-role').value;
      if (!username) { UI.toast('用户名不能为空', true); return; }
      if (password.length < 8) { UI.toast('密码至少 8 位', true); return; }
      try {
        await Api.post('/api/v1/users', { username, password, role });
        UI.toast('已创建 ' + username);
        this.refresh();
      } catch (e) { UI.toast('创建失败：' + e.message, true); }
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
      const t = window.SETTINGS['ui.site_title'];
      if (t) document.title = t;
    } catch (e) {}
    const menu = document.getElementById('usermenu');
    if (menu && !document.getElementById('menu-profile')) {
      menu.insertAdjacentHTML('afterbegin', '<a href="#/profile" id="menu-profile">个人设置</a>');
    }
    Auth.profile = Auth.profile || {};
    applyTheme(Auth.profile.theme, Auth.profile.theme_color);
    UI.renderUser();
    themeMenu();
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
      location.reload();
    };
  }
  if (!location.hash) {
    const want = (Auth.profile && Auth.profile.default_page) === 'agents' ? '#/agents' : '#/overview';
    location.hash = want;
  }
  Router.start();
}

Router.register(/^#\/clients$/, ClientsPage, '客户端 A');
Router.register(/^#\/streams$/, StreamsPage, '会话');
Router.register(/^#\/certs$/, CertsPage, '证书');
Router.register(/^#\/acl$/, AclPage, 'ACL');
Router.register(/^#\/accounts$/, AccountsPage, '账号');
Router.register(/^#\/audit$/, AuditPage, '审计');
Router.register(/^#\/files(?:\?.*)?$/, FilesPage, '文件');
Router.register(/^#\/terminal\/([^/]+)$/, TerminalPage, '终端');
Router.register(/^#\/settings$/, SettingsPage, '设置');
Router.register(/^#\/profile$/, ProfilePage, '个人设置');
boot();
