// 列表/文件/设置等页面（admin.js 之后加载）
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
      '<code title="' + UI.esc(f.sha256 || '') + '">' + UI.esc((f.sha256 || '').slice(0, 8)) + '</code>',
      new Date(f.mtime_ms).toLocaleString(),
      '<button class="dl" data-name="' + UI.esc(f.name) + '">下载</button> ' +
      (canManage() ? '<button class="push" data-name="' + UI.esc(f.name) + '">分发</button> ' : '') +
      (canManage() ? '<button class="del" data-name="' + UI.esc(f.name) + '">删除</button>' : ''),
    ]);
    this.el.innerHTML =
      '<div class="panel" style="margin-bottom:16px"><div class="panel-head"><h2>上传文件</h2>' +
      '<span class="spacer"></span><button id="files-refresh">刷新列表</button></div>' +
      '<div style="padding:16px;display:flex;gap:10px;align-items:center;flex-wrap:wrap">' +
      '<input type="file" id="upload-input"> <button class="primary" id="upload-btn">上传</button>' +
      '<progress id="upload-progress" max="100" value="0" style="width:220px;display:none"></progress>' +
      '<span id="upload-status" style="color:var(--muted)"></span>' +
      '<span style="color:var(--muted)">单文件上限 ' + uploadLimitMb() + ' MiB</span></div></div>' +
      '<div class="panel"><div class="panel-head"><h2>文件列表</h2><span class="spacer"></span>' +
      '<span>共 ' + arr.length + ' 个</span></div>' +
      UI.table([this.sortLink('name', '名称'), this.sortLink('size', '大小'), '哈希',
                this.sortLink('mtime', '修改时间'), '操作'], rows) + '</div>';

    document.getElementById('files-refresh').onclick = () => this.refresh();
    const MAX_UPLOAD = uploadLimitMb() * 1024 * 1024;
    const CHUNK = 8 * 1024 * 1024;
    document.getElementById('upload-btn').onclick = async () => {
      const btn = document.getElementById('upload-btn');
      const fi = document.getElementById('upload-input').files[0];
      const status = document.getElementById('upload-status');
      const bar = document.getElementById('upload-progress');
      if (!fi) { UI.toast('请选择文件', true); return; }
      if (fi.size > MAX_UPLOAD) {
        status.textContent = '文件 ' + UI.bytes(fi.size) + ' 超过 ' + uploadLimitMb() + ' MiB 上限';
        UI.toast('文件太大（' + UI.bytes(fi.size) + '），上限 ' + uploadLimitMb() + ' MiB', true);
        return;
      }
      const send = (url, blob, onprog) => new Promise((res, rej) => {
        const xhr = new XMLHttpRequest();
        xhr.open('POST', url);
        xhr.setRequestHeader('X-Requested-With', 'fetch');
        xhr.upload.onprogress = (e) => { if (e.lengthComputable && onprog) onprog(e.loaded); };
        xhr.onload = () => xhr.status === 200 ? res(xhr.responseText)
          : rej(new Error('HTTP ' + xhr.status + ' ' + xhr.responseText));
        xhr.onerror = () => rej(new Error('网络错误'));
        xhr.send(blob);
      });
      bar.style.display = '';
      bar.value = 0;
      status.textContent = '上传中…';
      btn.disabled = true;
      try {
        const url = '/api/v1/files/' + encodeURIComponent(fi.name);
        const uid = Math.random().toString(36).slice(2) + Date.now().toString(36);
        if (fi.size <= CHUNK) {
          await send(url, fi, (loaded) => {
            bar.value = Math.floor(loaded * 100 / fi.size);
            status.textContent = '上传中… ' + UI.bytes(loaded) + ' / ' + UI.bytes(fi.size);
          });
        } else {
          let off = 0, tries = 0;
          while (off < fi.size) {
            const chunk = fi.slice(off, off + CHUNK);
            try {
              await send(url + '?append=1&upload=' + uid + '&offset=' + off, chunk, (loaded) => {
                const cur = off + loaded;
                bar.value = Math.floor(cur * 100 / fi.size);
                status.textContent = '上传中… ' + UI.bytes(cur) + ' / ' + UI.bytes(fi.size);
              });
              off += chunk.size;
              tries = 0;
            } catch (e) {
              if (++tries > 2) throw e;
              try {
                const st = await Api.get('/api/v1/uploads/' + encodeURIComponent(fi.name) +
                  '?upload=' + uid);
                off = st.received || off;
                status.textContent = '重试中… 已传 ' + UI.bytes(off) + ' / ' + UI.bytes(fi.size);
              } catch (e2) { throw e; }
            }
          }
          await send(url + '?finalize=1&upload=' + uid, new Blob([]));
        }
        status.textContent = '已上传 ' + fi.name + '（' + UI.bytes(fi.size) + '）';
        this.refresh();
      } catch (e) {
        status.textContent = '';
        UI.toast('上传失败：' + e.message, true);
      } finally {
        btn.disabled = false;
        bar.style.display = 'none';
      }
    };
    document.querySelectorAll('#page button.push').forEach(btn => {
      btn.onclick = () => Router.go('#/distribute?file=' + encodeURIComponent(btn.dataset.name));
    });
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
    const schema = st._schema || [];
    const groups = [];
    schema.forEach(d => { if (groups.indexOf(d.group) < 0) groups.push(d.group); });

    let html = '';
    groups.forEach(g => {
      const rows = schema.filter(d => d.group === g).map(d => {
        const isText = (d.min === 0 && d.max === 0);
        const input = isText
          ? '<input type="text" data-k="' + d.key + '" value="' + UI.esc(st[d.key] || '') + '">'
          : '<input type="number" data-k="' + d.key + '" value="' + UI.esc(st[d.key] || '') +
            '" min="' + d.min + '" max="' + d.max + '">';
        return '<label class="cfg-row"><span>' + UI.esc(d.label) + '</span>' + input +
          (d.unit ? '<em>' + UI.esc(d.unit) + '</em>' : '') + '</label>';
      }).join('');
      html += '<div class="panel" style="margin-bottom:16px"><div class="panel-head"><h2>' +
        UI.esc(g) + '</h2></div><div class="cfg-body">' + rows + '</div></div>';
    });
    html += '<div class="panel"><div class="cfg-actions" style="padding-top:16px">' +
      '<button class="primary" id="cfg-save">保存</button></div></div>';
    this.el.innerHTML = html;

    document.getElementById('cfg-save').onclick = async () => {
      const body = {};
      document.querySelectorAll('#page .cfg-body [data-k]').forEach(inp => { body[inp.dataset.k] = inp.value; });
      try {
        await Api.put('/api/v1/settings', body);
        try {
          window.SETTINGS = await Api.get('/api/v1/settings');
          applySiteTitle();
        } catch (e) {}
        UI.toast('已保存，立即生效');
        this.refresh();
      } catch (e) { UI.toast('保存失败：' + e.message, true); }
    };
  },
  enter(el) { this.el = el; return Page.prototype.enter.call(this, el, []); },
});


Router.register(/^#\/agents\/([^/]+)$/, AgentDetailPage, '设备详情');

// 路由注册（页面定义在本文件）
Router.register(/^#\/clients$/, ClientsPage, '客户端 A');
Router.register(/^#\/streams$/, StreamsPage, '会话');
Router.register(/^#\/certs$/, CertsPage, '证书');
Router.register(/^#\/acl$/, AclPage, 'ACL');
Router.register(/^#\/audit$/, AuditPage, '审计');
Router.register(/^#\/files(?:\?.*)?$/, FilesPage, '文件');
Router.register(/^#\/terminal\/([^/]+)$/, TerminalPage, '终端');
Router.register(/^#\/settings$/, SettingsPage, '设置');
