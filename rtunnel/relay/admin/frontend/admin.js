'use strict';
/* rtunnel admin 前端：侧边栏 + hash 路由，无框架单文件。
 * 模块划分：Api（fetch 封装）/ UI（渲染组件）/ Router（hash 路由 + 页面生命周期）/
 *           Pages.*（概览/设备/设备详情/客户端/会话，各带 enter/refresh/leave）。
 */

// ---------------------------------------------------------------- Api
const Api = {
  token: localStorage.getItem('rt_admin_token') || '',
  async req(path, opt = {}) {
    const headers = Object.assign({ 'X-Requested-With': 'fetch' }, opt.headers || {});
    if (this.token) headers['Authorization'] = 'Bearer ' + this.token;
    const r = await fetch(path, Object.assign({ cache: 'no-store' }, opt, { headers }));
    if (r.status === 401) {
      const t = prompt('需要 admin token（Bearer）：');
      if (t) { this.token = t; localStorage.setItem('rt_admin_token', t); return this.req(path, opt); }
      throw new Error('unauthorized');
    }
    if (!r.ok) throw new Error('HTTP ' + r.status + ' ' + path);
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
const UI = {
  esc(s) {
    return String(s == null ? '' : s).replace(/[&<>"']/g,
      c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
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
    if (!rows.length) return '<div class="empty">暂无数据</div>';
    let h = '<table><thead><tr>' + cols.map(c => '<th>' + UI.esc(c) + '</th>').join('') + '</tr></thead><tbody>';
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
    { hash: '#/files', title: '文件', icon: '⬇' },
    { hash: '#/audit', title: '审计', icon: '≣' },
  ],
  register(re, page, title) { this.routes.push({ re, page, title }); },
  go(h) { location.hash = h; },
  renderNav(activeHash) {
    document.getElementById('nav').innerHTML = this.nav.map(n =>
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
      '<button class="shell" data-devid="' + UI.esc(a.devid) + '">Shell</button> ' +
      '<button class="kick" data-devid="' + UI.esc(a.devid) + '">下线</button>',
    ]);
    this.el.innerHTML = '<div class="panel"><div class="panel-head"><h2>设备（C 侧）</h2>' +
      '<span class="spacer"></span><span>共 ' + arr.length + ' 台 · 5s 自动刷新</span></div>' +
      UI.table(['devid', '状态', '远端地址', '最近活跃', '会话数', '流量 ↓/↑', '操作'], rows) + '</div>';
    document.querySelectorAll('#page button.shell').forEach(btn => {
      btn.onclick = (e) => {
        e.stopPropagation();
        Router.go('#/terminal/' + encodeURIComponent(btn.dataset.devid) + '?direct=1');
      };
    });
    document.querySelectorAll('#page tbody tr').forEach((tr, i) => {
      tr.onclick = () => Router.go('#/agents/' + encodeURIComponent(arr[i].devid));
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
  async refresh() {
    const d = await Api.get('/api/v1/agents/' + encodeURIComponent(this.devid));
    const rows = (d.streams || []).map(s => [
      UI.esc(s.a_user) + ':' + s.a_sid, s.c_sid,
      s.opened ? '<span class="pill ok">OPEN</span>' : '<span class="pill info">OPENING</span>',
      UI.ago(s.opened_at_ms),
    ]);
    this.el.innerHTML =
      '<div class="panel" style="margin-bottom:16px"><div class="panel-head"><h2>' + UI.esc(d.devid) + '</h2>' +
      '<span class="pill ok">在线</span></div>' + UI.kv({
        '远端地址': UI.esc(d.remote_addr),
        '接入时间': new Date(d.connected_at_ms).toLocaleString(),
        '最近活跃': UI.ago(d.last_seen_ms) + ' 前',
        '流量 ↓/↑': UI.bytes(d.bytes_in) + ' / ' + UI.bytes(d.bytes_out),
      }) + '</div>' +
      '<div class="panel"><div class="panel-head"><h2>当前会话</h2></div>' +
      UI.table(['A 用户:sid', 'c_sid', '状态', '开启于'], rows) + '</div>';
  },
  enter(el, params) {
    this.devid = decodeURIComponent(params[0] || '');
    return Page.prototype.enter.call(this, el, params);
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
    let order = 'asc';
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
      '<button class="del" data-name="' + UI.esc(f.name) + '">删除</button>',
    ]);
    this.el.innerHTML =
      '<div class="panel" style="margin-bottom:16px"><div class="panel-head"><h2>上传文件</h2>' +
      '<span class="spacer"></span><button id="files-refresh">刷新列表</button></div>' +
      '<div style="padding:16px;display:flex;gap:10px;align-items:center;flex-wrap:wrap">' +
      '<input type="file" id="upload-input"> <button class="primary" id="upload-btn">上传</button>' +
      '<span id="upload-status" style="color:var(--muted)"></span></div></div>' +
      '<div class="panel"><div class="panel-head"><h2>文件列表</h2><span class="spacer"></span>' +
      '<span>共 ' + arr.length + ' 个</span></div>' +
      UI.table([this.sortLink('name', '名称'), this.sortLink('size', '大小'),
                this.sortLink('mtime', '修改时间'), '操作'], rows) + '</div>';

    document.getElementById('files-refresh').onclick = () => this.refresh();
    document.getElementById('upload-btn').onclick = async () => {
      const fi = document.getElementById('upload-input').files[0];
      if (!fi) { UI.toast('请选择文件', true); return; }
      const status = document.getElementById('upload-status');
      status.textContent = '上传中…';
      try {
        const headers = { 'X-Requested-With': 'fetch' };
        if (Api.token) headers['Authorization'] = 'Bearer ' + Api.token;
        const r = await fetch('/api/v1/files/' + encodeURIComponent(fi.name),
          { method: 'POST', headers, body: fi, cache: 'no-store' });
        if (!r.ok) throw new Error('HTTP ' + r.status + ' ' + (await r.text()));
        status.textContent = '已上传 ' + fi.name + '（' + UI.bytes(fi.size) + '）';
        this.refresh();
      } catch (e) { status.textContent = ''; UI.toast('上传失败：' + e.message, true); }
    };
    document.querySelectorAll('#page button.dl').forEach(btn => {
      btn.onclick = async () => {
        try {
          const headers = {};
          if (Api.token) headers['Authorization'] = 'Bearer ' + Api.token;
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
const TerminalPage = Object.assign(new Page(), {
  enter(el, params) {
    // 路由捕获组可能带查询串（?direct=1），devid 必须只保留设备名
    this.devid = decodeURIComponent((params[0] || '').split('?')[0]);
    el.innerHTML = '<div class="panel"><div class="panel-head"><h2>终端：' + UI.esc(this.devid) + '</h2>' +
      '<span class="spacer"></span><span style="color:var(--muted);font-size:12px">免密 shell（agent OPEN_SHELL，设备侧直接起 shell）</span>' +
      '<button id="term-close">关闭</button></div>' +
      '<div id="term" style="height:calc(100vh - 220px);background:#000;padding:8px"></div></div>';

    this.term = new Terminal({ cursorBlink: true, fontSize: 14 });
    this.fit = new FitAddon.FitAddon();
    this.term.loadAddon(this.fit);
    this.term.open(document.getElementById('term'));
    this.fit.fit();

    const proto = location.protocol === 'https:' ? 'wss' : 'ws';
    const q = new URLSearchParams((location.hash.split('?')[1] || ''));
    // direct 未指定 user 时后端回退到配置的默认 shell_user
    const wsUser = q.get('user') || '';
    this.ws = new WebSocket(proto + '://' + location.host + '/ws/shell?devid=' +
      encodeURIComponent(this.devid) + '&user=' + encodeURIComponent(wsUser) + '&token=' + encodeURIComponent(Api.token));
    this.ws.onmessage = (e) => this.term.write(e.data);
    this.ws.onclose = () => this.term.write('\r\n[连接已关闭]\r\n');
    this.term.onData(d => { if (this.ws.readyState === 1) this.ws.send(d); });
    this.term.onResize(({ cols, rows }) => {
      if (this.ws.readyState === 1) this.ws.send(JSON.stringify({ type: 'resize', cols, rows }));
    });
    this._rs = () => this.fit.fit();
    window.addEventListener('resize', this._rs);
    document.getElementById('term-close').onclick = () => {
      this.ws.close();
      Router.go('#/agents');
    };
  },
  leave() {
    if (this.ws) this.ws.close();
    if (this._rs) window.removeEventListener('resize', this._rs);
    if (this.term) this.term.dispose();
    if (this.timer) clearInterval(this.timer);
  },
});

// ---------------------------------------------------------------- 启动
Router.register(/^#\/overview$/, OverviewPage, '概览');
Router.register(/^#\/agents$/, AgentsPage, '设备 C');
Router.register(/^#\/agents\/([^/]+)$/, AgentDetailPage, '设备详情');
Router.register(/^#\/clients$/, ClientsPage, '客户端 A');
Router.register(/^#\/streams$/, StreamsPage, '会话');
Router.register(/^#\/certs$/, CertsPage, '证书');
Router.register(/^#\/acl$/, AclPage, 'ACL');
Router.register(/^#\/audit$/, AuditPage, '审计');
Router.register(/^#\/files(?:\?.*)?$/, FilesPage, '文件');
Router.register(/^#\/terminal\/([^/]+)$/, TerminalPage, '终端');
if (!location.hash) location.hash = '#/overview';
Router.start();
