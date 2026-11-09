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
    this.el.innerHTML = UI.listPage({
      title: '客户端（A 侧）', summary: '共 ' + arr.length + ' 个',
      columns: ['用户', '远端地址', '最近活跃', '会话数', '流量 ↓/↑', '操作'], rows,
    });
    document.querySelectorAll('#page button.web').forEach(btn => {
      btn.onclick = async (e) => {
        e.stopPropagation();
        const port = await UI.prompt({ title: '设备 Web 端口', label: '端口（需在 agent --allow 列表）', value: '80' });
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
    this.el.innerHTML = UI.listPage({
      title: '会话', summary: '共 ' + arr.length + ' 条',
      search: { id: 'st-q', placeholder: '搜索用户 / 设备' },
      columns: ['A 用户:a_sid', 'C 设备:c_sid', '状态', '开启于', '操作'], rows,
    });
    UI.bindSearch(document.getElementById('st-q'));
    document.querySelectorAll('#page button.bind').forEach(btn => {
      btn.onclick = async (e) => {
        e.stopPropagation();
        const owner = await UI.prompt({ title: '绑定设备', label: '账号（留空 = 解绑）' });
        if (owner === null) return;
        try {
          await Api.put('/api/v1/devices/' + encodeURIComponent(btn.dataset.devid) + '/owner', { username: owner });
          UI.toast(owner ? ('已绑定到 ' + owner) : '已解绑');
        } catch (err) { UI.toast('绑定失败：' + err.message, true); }
      };
    });
    document.querySelectorAll('#page button.kick').forEach(btn => {
      btn.onclick = async () => {
        if (!await UI.confirm({ title: '踢掉会话', message: 'a_sid=' + btn.dataset.sid + ' 将被双侧 RESET。', danger: true })) return;
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
    const roleName = r => r === 'device' ? '设备' : r === 'user' ? '用户' : UI.esc(r || '');
    const rows = arr.map(c => {
      const days = c.expire_ms ? Math.ceil((c.expire_ms - Date.now()) / 86400000) : null;
      const exp = c.expire_ms
        ? new Date(c.expire_ms).toLocaleDateString() +
          (days !== null ? ' <span class="muted">' + (days >= 0 ? '剩 ' + days + ' 天' : '已过期') + '</span>' : '')
        : '—';
      const serial = '0x' + Number(c.serial).toString(16).toUpperCase();
      return [
        '<b title="' + UI.esc(c.san) + '">' + UI.esc(c.san.length > 28 ? c.san.slice(0, 27) + '…' : c.san) + '</b>',
        roleName(c.role),
        '<code title="' + UI.esc(serial) + '">' + UI.esc(serial.length > 14 ? serial.slice(0, 13) + '…' : serial) + '</code>',
        exp, pill(c.status),
        c.status === 'revoked' ? '' : '<button class="revoke" data-san="' + UI.esc(c.san) + '">吊销</button>',
      ];
    });
    this.el.innerHTML =
      UI.panel('签发新证书', UI.form({
        id: 'issue-form', layout: 'grid', cols: 3,
        rows: [
          { label: 'SAN', control: '<input data-field="san" placeholder="PILE-000200">' },
          { label: '类型', control: '<select data-field="role"><option value="device">设备</option><option value="user">用户</option></select>' },
          { label: '有效期(天)', control: '<input data-field="days" type="number" value="30" min="1" max="3650">' },
        ],
        left: '<span id="issue-result" class="muted"></span>',
        right: '<button class="primary" id="issue-btn">签发</button>',
      }), { cls: 'mb16', right: '<button id="certs-refresh">刷新列表</button>' }) +
      UI.panel('CSR 签名（私钥不出设备）', '<div class="ui-pad">' +
        '<textarea id="csr-input" rows="6" class="ui-textarea" placeholder="粘贴 CSR PEM（设备侧生成，SAN 或 CN 为身份）"></textarea>' +
        '<div class="ui-flex ui-mt8">有效期(天) <input id="csr-days" type="number" value="30" class="ui-w80">' +
        '<button id="csr-btn">签名</button><span id="csr-result" class="muted"></span></div></div>', { cls: 'mb16' }) +
      UI.listPage({
        title: '证书台账', summary: '共 ' + arr.length + ' 张',
        search: { id: 'cert-q', placeholder: '搜索 SAN' },
        columns: ['SAN(身份)', '类型', '序列号', '到期日', '状态', '操作'], rows,
      });

    const cr = document.getElementById('certs-refresh');
    if (cr) cr.onclick = () => this.refresh();
    UI.bindSearch(document.getElementById('cert-q'));
    document.getElementById('issue-btn').onclick = async () => {
      const v = UI.formValues(document.getElementById('issue-form'));
      const san = (v.san || '').trim();
      const role = v.role;
      const days = parseInt(v.days, 10) || 30;
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
        if (!await UI.confirm({ title: '吊销证书', message: '吊销 ' + btn.dataset.san + '？在线连接会被踢，后续 AUTH 被拒。', danger: true })) return;
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
    const aclBody = '<div class="ui-pad"><textarea id="acl-content" rows="12" class="ui-textarea" ' +
      'placeholder="user=devid[,devid|*]，每行一条，# 注释">' + UI.esc(d.content) + '</textarea>' +
      '<div class="muted ui-mt8">保存 = 写文件 + 热加载，只影响新 OPEN，不动已建立会话；解析失败保留旧表。</div></div>';
    this.el.innerHTML = UI.panel('ACL 文件（' + UI.esc(d.path || '未配置') + '）', aclBody, {
      cls: 'mb16', right: '<button id="acl-reload">重新加载</button> ' +
        '<button class="primary" id="acl-save">保存并热加载</button>',
    }) + UI.panel('命令行条目（只读）', UI.table(['条目'], (d.cli || []).map(x => [UI.esc(x)])));
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
    const rows = arr.map(x => {
      const m = /^\[([^\]]+)\] role=(\S+) action=(\S+) detail=(.*)$/.exec(x);
      if (!m) return ['<code>' + UI.esc(x) + '</code>', '', '', ''];
      return ['<span class="muted">' + UI.esc(m[1].replace('T', ' ').replace('Z', '')) + '</span>',
        UI.esc(m[2]), '<code>' + UI.esc(m[3]) + '</code>',
        '<span title="' + UI.esc(m[4]) + '">' + UI.esc(m[4].length > 80 ? m[4].slice(0, 79) + '…' : m[4]) + '</span>'];
    });
    this.el.innerHTML = UI.listPage({
      title: '审计日志', summary: '最近 ' + arr.length + ' 条（新到旧）',
      search: { id: 'au-q', placeholder: '搜索操作 / 详情' },
      columns: ['时间', '角色', '操作', '详情'], rows,
    });
    UI.bindSearch(document.getElementById('au-q'));
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
      '" class="ui-tablink">' + label + cur + '</a>';
  },
  async refresh() {
    const arr = await Api.get('/api/v1/files?sort=' + this.sort + '&order=' + this.order);
    const rows = arr.map(f => [
      '<b>' + UI.esc(f.name) + '</b>',
      '<span class="ui-right">' + UI.bytes(f.size) + '</span>',
      '<code title="' + UI.esc(f.sha256 || '') + '">' + UI.esc((f.sha256 || '').slice(0, 8)) + '</code> ' +
        (f.sha256 ? '<button class="cp" data-hash="' + UI.esc(f.sha256) + '">复制</button>' : ''),
      new Date(f.mtime_ms).toLocaleString(),
      '<button class="dl" data-name="' + UI.esc(f.name) + '">下载</button> ' +
      (canManage() ? '<button class="push" data-name="' + UI.esc(f.name) + '">分发</button> ' : '') +
      (canManage() ? '<button class="del" data-name="' + UI.esc(f.name) + '">删除</button>' : ''),
    ]);
    const uploadBody = '<div class="ui-pad ui-flex">' +
      '<input type="file" id="upload-input"> <button class="primary" id="upload-btn">上传</button>' +
      '<progress id="upload-progress" class="ui-w220" max="100" value="0" style="display:none"></progress>' +
      '<span id="upload-status" class="muted"></span>' +
      '<span class="muted">单文件上限 ' + uploadLimitMb() + ' MiB</span></div>';
    this.el.innerHTML = UI.panel('上传文件', uploadBody, {
      cls: 'mb16', right: '<button id="files-refresh">刷新列表</button>',
    }) + UI.listPage({
      title: '文件列表', summary: '共 ' + arr.length + ' 个',
      search: { id: 'fl-q', placeholder: '搜索文件名' },
      columns: [this.sortLink('name', '名称'), this.sortLink('size', '大小'), '哈希',
                this.sortLink('mtime', '修改时间'), '操作'],
      rows,
    });

    document.getElementById('files-refresh').onclick = () => this.refresh();
    UI.bindSearch(document.getElementById('fl-q'));
    document.querySelectorAll('#page button.cp').forEach(btn => {
      btn.onclick = async () => {
        try { await navigator.clipboard.writeText(btn.dataset.hash); UI.toast('哈希已复制'); }
        catch (e) { UI.toast('复制失败', true); }
      };
    });
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
        if (!await UI.confirm({ title: '删除文件', message: '删除 ' + btn.dataset.name + '？', danger: true })) return;
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
  async enter(el, params) {
    this.el = el;
    el.innerHTML = '<div class="panel"><div class="empty">终端组件加载中…</div></div>';
    try {
      await loadCssOnce('/vendor/xterm.css');
      await loadScriptOnce('/vendor/xterm.js');
      await loadScriptOnce('/vendor/xterm-addon-fit.js');
    } catch (e) {
      el.innerHTML = '<div class="panel"><div class="empty">终端组件加载失败</div></div>';
      return;
    }
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
      '<div id="term-host" class="ui-term"></div></div>';
    const host = document.getElementById('term-host');
    if (!cur.opened) { cur.term.open(cur.box); cur.opened = true; }
    host.appendChild(cur.box);
    document.querySelectorAll('#page button.term-tab').forEach(btn => {
      btn.onclick = () => { this.active = btn.dataset.id; this.render(); };
    });
    document.getElementById('term-new').onclick = async () => {
      const d = await UI.prompt({ title: '新建终端', label: '设备 devid', value: cur.devid });
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
// ---------------------------------------------------------------- Pages.权限
const PermissionPage = Object.assign(new Page(), {
  refreshInterval() { return 0; },
  tab: 'client',
  expandKey: null,
  ov: null,
  detail: null,
  async refresh() {
    this.ov = await Api.get('/api/v1/permission/overview');
    if (this.expandKey && this.expandKey !== 'client:new' && this.expandKey !== 'agent:new') {
      const [kind, id] = this.expandKey.split(':');
      const base = kind === 'client' ? 'client-groups' : 'agent-groups';
      try { this.detail = await Api.get('/api/v1/permission/' + base + '/' + id); }
      catch (e) { this.expandKey = null; this.detail = null; }
    } else {
      this.detail = null;
    }
    this.render();
  },
  capText(c) {
    c = c || {};
    const bits = [];
    if (c.shell === '1' || c.shell === true) bits.push('Shell');
    if (c.tcp_ports) bits.push('TCP ' + c.tcp_ports);
    if (c.tty) bits.push('TTY ' + c.tty);
    if (c.file && c.file !== 'none') bits.push('文件 ' + c.file);
    if (c.sysinfo === '1' || c.sysinfo === true) bits.push('SYSINFO');
    if (c.metrics === '1' || c.metrics === true) bits.push('指标');
    return bits.join(' · ') || '无';
  },
  chk(v) { return v === '1' || v === true; },
  chip(text, cls) {
    return '<span class="perm-chip ' + (cls || '') + '" data-v="' + UI.esc(text) + '">' +
      UI.esc(text) + ' <button class="perm-chip-x">×</button></span>';
  },
  aclRow(a, groups) {
    const opts = groups.map(g => '<option value="' + g.id + '"' +
      (Number(a.agent_group_id) === g.id ? ' selected' : '') + '>' + UI.esc(g.name) + '</option>').join('');
    return '<tr class="perm-acl-row">' +
      '<td><select class="pa-agent">' + opts + '</select></td>' +
      '<td><input class="pa-ports" value="' + UI.esc(a.tcp_ports || '') + '" placeholder="22,80,*"></td>' +
      '<td class="center"><input type="checkbox" class="pa-shell"' + (a.shell ? ' checked' : '') + '></td>' +
      '<td><input class="pa-tty" value="' + UI.esc(a.tty || '') + '" placeholder="ttyS*"></td>' +
      '<td><select class="pa-file">' + ['none','ro','rw'].map(v =>
        '<option value="' + v + '"' + ((a.file || 'none') === v ? ' selected' : '') + '>' + v + '</option>').join('') + '</select></td>' +
      '<td class="center"><input type="checkbox" class="pa-sysinfo"' + (a.sysinfo ? ' checked' : '') + '></td>' +
      '<td class="center"><input type="checkbox" class="pa-metrics"' + (a.metrics ? ' checked' : '') + '></td>' +
      '<td class="center"><input type="checkbox" class="pa-config"' + (a.config ? ' checked' : '') + '></td>' +
      '<td class="center"><input type="checkbox" class="pa-web"' + (a.web ? ' checked' : '') + '></td>' +
      '<td><button class="pa-del">移除</button></td></tr>';
  },
  clientForm(d, groups) {
    const users = (d.users || []).map(u => this.chip(u)).join('');
    const acl = (d.acl || []).map(a => this.aclRow(a, groups)).join('');
    return '<div class="perm-form">' +
      '<div class="perm-grid2"><label>名称<input id="pc-name" value="' + UI.esc(d.name || '') + '"></label>' +
      '<label>描述<input id="pc-desc" value="' + UI.esc(d.description || '') + '"></label></div>' +
      '<h3>客户端用户</h3><div class="perm-chips" id="pc-users">' + users + '</div>' +
      '<div class="ui-flex" style="margin-top:8px"><input id="pc-user" placeholder="alice 或 alice,bob">' +
      '<button id="pc-add-user">添加用户</button></div>' +
      '<h3>ACL 配置</h3>' +
      '<div class="perm-acl-wrap"><table class="detail-table perm-acl"><thead><tr>' +
      '<th>设备分组</th><th>TCP 端口</th><th>Shell</th><th>TTY</th><th>文件</th>' +
      '<th>SYSINFO</th><th>指标</th><th>配置</th><th>Web</th><th></th></tr></thead>' +
      '<tbody id="pc-acl">' + (acl || '') + '</tbody></table></div>' +
      '<div class="ui-flex" style="margin-top:8px"><select id="pc-agent">' + groups.map(g =>
        '<option value="' + g.id + '">' + UI.esc(g.name) + '</option>').join('') + '</select>' +
      '<button id="pc-add-acl">添加 ACL</button></div>' +
      '<div class="perm-form-foot"><button class="perm-cancel">取消</button>' +
      '<button class="primary" id="pc-save">保存</button></div></div>';
  },
  agentForm(d) {
    const devices = (d.devices || []).map(x => this.chip(x)).join('');
    const c = d.caps || {};
    return '<div class="perm-form">' +
      '<div class="perm-grid2"><label>名称<input id="pa-name" value="' + UI.esc(d.name || '') + '"></label>' +
      '<label>描述<input id="pa-desc" value="' + UI.esc(d.description || '') + '"></label></div>' +
      '<h3>设备成员 <button id="pa-transfer">+ 添加设备</button></h3>' +
      '<div class="perm-chips" id="pa-devices">' + devices + '</div>' +
      '<h3>能力上限</h3><div class="perm-caps">' +
      '<label><input type="checkbox" id="pa-shell"' + (this.chk(c.shell) ? ' checked' : '') + '> Shell</label>' +
      '<label><input type="checkbox" id="pa-sysinfo"' + (this.chk(c.sysinfo) ? ' checked' : '') + '> SYSINFO</label>' +
      '<label><input type="checkbox" id="pa-metrics"' + (this.chk(c.metrics) ? ' checked' : '') + '> 指标</label>' +
      '<label><input type="checkbox" id="pa-config"' + (this.chk(c.config) ? ' checked' : '') + '> 修改配置</label>' +
      '<label><input type="checkbox" id="pa-web"' + (this.chk(c.web) ? ' checked' : '') + '> Web</label></div>' +
      '<label class="perm-field">TCP 端口<input id="pa-ports" value="' + UI.esc(c.tcp_ports || '') + '"></label>' +
      '<label class="perm-field">TTY<input id="pa-tty" value="' + UI.esc(c.tty || '') + '"></label>' +
      '<label class="perm-field">文件<select id="pa-file">' + ['none','ro','rw'].map(v =>
        '<option value="' + v + '"' + ((c.file || 'none') === v ? ' selected' : '') + '>' + v + '</option>').join('') + '</select></label>' +
      '<div class="perm-form-foot"><button class="perm-cancel">取消</button>' +
      '<button class="primary" id="pa-save">保存</button></div></div>';
  },
  render() {
    const ov = this.ov || {};
    const cgs = ov.client_groups || [];
    const ags = ov.agent_groups || [];
    const isClient = this.tab === 'client';
    const rows = isClient ? cgs.map(g => {
      const open = this.expandKey === 'client:' + g.id;
      return '<tr class="perm-row' + (open ? ' open' : '') + '" data-key="client:' + g.id + '">' +
        '<td><span class="perm-chev">' + (open ? '▾' : '▸') + '</span> <b>' + UI.esc(g.name) + '</b></td>' +
        '<td>' + (g.user_count || 0) + '</td><td>' + (g.acl_count || 0) + '</td>' +
        '<td class="muted">' + UI.ago(g.updated_ms) + '</td>' +
        '<td><button class="danger perm-del" data-kind="client" data-id="' + g.id + '">删除</button></td></tr>' +
        (open ? '<tr class="perm-expand"><td colspan="5">' +
          this.clientForm(this.detail || g, ags) + '</td></tr>' : '');
    }).join('') : ags.map(g => {
      const open = this.expandKey === 'agent:' + g.id;
      return '<tr class="perm-row' + (open ? ' open' : '') + '" data-key="agent:' + g.id + '">' +
        '<td><span class="perm-chev">' + (open ? '▾' : '▸') + '</span> <b>' + UI.esc(g.name) + '</b></td>' +
        '<td>' + (g.device_count || 0) + '</td><td>' + (g.ref_count || 0) + '</td>' +
        '<td class="muted">' + UI.esc(this.capText(g.caps)) + '</td>' +
        '<td><button class="danger perm-del" data-kind="agent" data-id="' + g.id + '">删除</button></td></tr>' +
        (open ? '<tr class="perm-expand"><td colspan="5">' +
          this.agentForm(this.detail || g) + '</td></tr>' : '');
    }).join('');
    let newForm = '';
    if (this.expandKey === 'client:new') newForm = '<tr class="perm-expand"><td colspan="5">' + this.clientForm({}, ags) + '</td></tr>';
    if (this.expandKey === 'agent:new') newForm = '<tr class="perm-expand"><td colspan="5">' + this.agentForm({}) + '</td></tr>';
    const head = isClient
      ? '<tr><th>名称</th><th>用户数</th><th>设备组</th><th>更新</th><th></th></tr>'
      : '<tr><th>名称</th><th>设备数</th><th>被引用</th><th>能力上限</th><th></th></tr>';
    this.el.innerHTML = UI.panel('权限管理',
      '<div class="perm-tabs"><button class="perm-tab' + (isClient ? ' active' : '') + '" data-tab="client">客户端分组</button>' +
      '<button class="perm-tab' + (!isClient ? ' active' : '') + '" data-tab="agent">设备分组</button>' +
      '<span class="spacer"></span>' + (canManage() ? '<button class="primary" id="perm-new">+ 新建分组</button>' : '') + '</div>' +
      '<div class="perm-table-wrap"><table class="detail-table perm-table"><thead>' + head + '</thead><tbody>' +
      (newForm || '') + (rows || '<tr><td colspan="5" class="empty">暂无分组</td></tr>') + '</tbody></table></div>');
    this.bind();
  },
  bind() {
    document.querySelectorAll('.perm-tab').forEach(b => {
      b.onclick = () => { this.tab = b.dataset.tab; this.expandKey = null; this.detail = null; this.render(); };
    });
    const nb = document.getElementById('perm-new');
    if (nb) nb.onclick = () => {
      this.expandKey = this.tab + ':new';
      this.detail = this.tab === 'client' ? { users: [], acl: [] } : { devices: [], caps: {} };
      this.render();
    };
    document.querySelectorAll('.perm-row').forEach(tr => {
      tr.onclick = (e) => {
        if (e.target.closest('button')) return;
        const key = tr.dataset.key;
        this.expandKey = this.expandKey === key ? null : key;
        this.detail = null;
        this.refresh();
      };
    });
    document.querySelectorAll('.perm-del').forEach(b => {
      b.onclick = async (e) => {
        e.stopPropagation();
        if (!await UI.confirm({ title: '删除分组', message: '删除该分组？', danger: true })) return;
        const base = b.dataset.kind === 'client' ? 'client-groups' : 'agent-groups';
        try { await Api.del('/api/v1/permission/' + base + '/' + b.dataset.id); this.expandKey = null; this.refresh(); }
        catch (err) { UI.toast('删除失败：' + err.message, true); }
      };
    });
    const cancel = document.querySelector('.perm-cancel');
    if (cancel) cancel.onclick = () => { this.expandKey = null; this.detail = null; this.refresh(); };
    this.bindChips();
    this.bindTransfer();
    this.bindAcl();
    const cs = document.getElementById('pc-save');
    if (cs) cs.onclick = () => this.saveClient();
    const as = document.getElementById('pa-save');
    if (as) as.onclick = () => this.saveAgent();
  },
  bindChips() {
    const add = (boxId, inputId, btnId) => {
      const box = document.getElementById(boxId), input = document.getElementById(inputId), btn = document.getElementById(btnId);
      if (!box || !input || !btn) return;
      btn.onclick = () => {
        input.value.split(',').map(x => x.trim()).filter(Boolean).forEach(v => {
          if ([...box.querySelectorAll('.perm-chip')].some(c => c.dataset.v === v)) return;
          const d = document.createElement('span');
          d.className = 'perm-chip'; d.dataset.v = v;
          d.innerHTML = UI.esc(v) + ' <button class="perm-chip-x">×</button>';
          box.appendChild(d);
        });
        input.value = '';
      };
    };
    add('pc-users', 'pc-user', 'pc-add-user');
    add('pa-devices', 'pa-device', 'pa-add-dev');
    document.querySelectorAll('.perm-chip-x').forEach(b => {
      b.onclick = () => b.closest('.perm-chip').remove();
    });
  },
  bindTransfer() {
    const b = document.getElementById('pa-transfer');
    if (!b) return;
    b.onclick = () => this.openTransfer();
  },
  async openTransfer() {
    const isNew = this.expandKey === 'agent:new';
    const id = isNew ? 0 : this.expandKey.split(':')[1];
    let data;
    try { data = await Api.get('/api/v1/permission/agent-groups/' + id + '/candidates'); }
    catch (e) { UI.toast('加载设备失败：' + e.message, true); return; }
    const all = data.devices || [];
    const right = new Map();
    const m = UI.modal({
      title: '添加设备到「' + UI.esc((this.detail && this.detail.name) || '当前分组') + '」',
      body: '<div class="transfer-toolbar"><input id="tr-q" placeholder="搜索 devid">' +
        '<label><input type="checkbox" id="tr-online"> 仅在线</label><span id="tr-count"></span></div>' +
        '<div class="transfer"><div class="transfer-list"><h4 id="tr-left-title">可选设备</h4><div id="tr-left"></div></div>' +
        '<div class="transfer-ops"><button id="tr-add">&gt;</button><button id="tr-addall">&gt;&gt;</button>' +
        '<button id="tr-rem">&lt;</button><button id="tr-clear">&lt;&lt;</button></div>' +
        '<div class="transfer-list"><h4 id="tr-right-title">已选设备</h4><div id="tr-right"></div></div></div>' +
        '<div class="transfer-foot"><span id="tr-summary"></span></div>',
      buttons: '<button data-modal-close>取消</button><button class="primary" id="tr-ok">确定添加</button>',
    });
    const modalEl = m.el.querySelector('.tty-modal');
    if (modalEl) modalEl.classList.add('transfer-modal');
    const esc = UI.esc;
    const render = () => {
      const q = m.el.querySelector('#tr-q').value.trim().toLowerCase();
      const only = m.el.querySelector('#tr-online').checked;
      const left = all.filter(d => !right.has(d.devid) && (!only || d.online) &&
        (!q || d.devid.toLowerCase().includes(q)));
      m.el.querySelector('#tr-left').innerHTML = left.length ? left.map(d =>
        '<label class="transfer-item"><input type="checkbox" class="tr-pick" value="' + esc(d.devid) + '">' +
        '<span class="transfer-dev">' + esc(d.devid) + '</span>' +
        '<span class="pill ' + (d.online ? 'ok' : 'info') + '">' + (d.online ? '在线' : '登记') + '</span>' +
        ((d.groups || []).length ? '<span class="muted-sm">' + esc(d.groups.join(',')) + '</span>' : '') +
        '</label>').join('') : '<div class="empty">没有可添加的设备</div>';
      m.el.querySelector('#tr-right').innerHTML = right.size ? [...right.values()].map(d =>
        '<label class="transfer-item"><input type="checkbox" class="tr-rpick" value="' + esc(d.devid) + '">' +
        '<span class="transfer-dev">' + esc(d.devid) + '</span>' +
        '<span class="pill ' + (d.online ? 'ok' : 'info') + '">' + (d.online ? '在线' : '登记') + '</span></label>').join('')
        : '<div class="empty">从左侧勾选设备后点 &gt;</div>';
      m.el.querySelector('#tr-count').textContent = '共 ' + left.length + ' 台';
      m.el.querySelector('#tr-left-title').textContent = '可选设备 · ' + left.length;
      m.el.querySelector('#tr-right-title').textContent = '已选设备 · ' + right.size;
      const off = [...right.values()].filter(d => !d.online).length;
      m.el.querySelector('#tr-summary').textContent = '已选 ' + right.size + ' 台' +
        (off ? '，其中 ' + off + ' 台离线' : '');
      m.el.querySelector('#tr-addall').disabled = left.length === 0;
      m.el.querySelector('#tr-add').disabled = left.length === 0;
      m.el.querySelector('#tr-rem').disabled = right.size === 0;
      m.el.querySelector('#tr-clear').disabled = right.size === 0;
    };
    const picked = cls => [...m.el.querySelectorAll('.' + cls + ':checked')].map(x => x.value);
    const move = (values, toRight) => {
      const set = new Set(values);
      if (toRight) {
        all.filter(d => set.has(d.devid) && !right.has(d.devid)).forEach(d => right.set(d.devid, d));
      } else {
        set.forEach(v => right.delete(v));
      }
      render();
    };
    m.el.querySelector('#tr-add').onclick = () => move(picked('tr-pick'), true);
    m.el.querySelector('#tr-addall').onclick = () => {
      const q = m.el.querySelector('#tr-q').value.trim().toLowerCase();
      const only = m.el.querySelector('#tr-online').checked;
      move(all.filter(d => !right.has(d.devid) && (!only || d.online) &&
        (!q || d.devid.toLowerCase().includes(q))).map(d => d.devid), true);
    };
    m.el.querySelector('#tr-rem').onclick = () => move(picked('tr-rpick'), false);
    m.el.querySelector('#tr-clear').onclick = () => { right.clear(); render(); };
    m.el.querySelector('#tr-q').oninput = render;
    m.el.querySelector('#tr-online').onchange = render;
    m.el.querySelector('#tr-left').onclick = (e) => {
      const item = e.target.closest('.transfer-item');
      if (!item) return;
      if (e.detail === 2) move([item.querySelector('input').value], true);
    };
    m.el.querySelector('#tr-right').onclick = (e) => {
      const item = e.target.closest('.transfer-item');
      if (!item) return;
      if (e.detail === 2) move([item.querySelector('input').value], false);
    };
    m.el.querySelector('#tr-ok').onclick = async () => {
      if (!right.size) { UI.toast('请先选择设备', true); return; }
      const devids = [...right.keys()];
      if (isNew) {
        const box = document.getElementById('pa-devices');
        devids.forEach(v => {
          if ([...box.querySelectorAll('.perm-chip')].some(c => c.dataset.v === v)) return;
          const d = document.createElement('span');
          d.className = 'perm-chip'; d.dataset.v = v;
          d.innerHTML = esc(v) + ' <button class="perm-chip-x">×</button>';
          box.appendChild(d);
        });
        box.querySelectorAll('.perm-chip-x').forEach(b => { b.onclick = () => b.closest('.perm-chip').remove(); });
        m.close();
        return;
      }
      try {
        await Api.post('/api/v1/permission/agent-groups/' + id + '/devices', { devids });
        m.close(); UI.toast('已添加 ' + devids.length + ' 台设备');
        this.detail = null; this.refresh();
      } catch (e) { UI.toast('添加失败：' + e.message, true); }
    };
    render();
  },
  bindAcl() {
    const add = document.getElementById('pc-add-acl');
    if (add) add.onclick = () => {
      const sel = document.getElementById('pc-agent');
      const groups = this.ov.agent_groups || [];
      const g = groups.find(x => String(x.id) === sel.value) || groups[0];
      if (!g) return;
      const tbody = document.getElementById('pc-acl');
      const tmp = document.createElement('tbody');
      tmp.innerHTML = this.aclRow({ agent_group_id: g.id, file: 'none' }, groups);
      const tr = tmp.querySelector('tr');
      tbody.appendChild(tr);
      tr.querySelector('.pa-del').onclick = () => tr.remove();
    };
    document.querySelectorAll('.pa-del').forEach(b => { b.onclick = () => b.closest('tr').remove(); });
  },
  collectClient() {
    const users = [...document.querySelectorAll('#pc-users .perm-chip')].map(x => x.dataset.v);
    const acl = [...document.querySelectorAll('#pc-acl .perm-acl-row')].map(tr => ({
      agent_group_id: Number(tr.querySelector('.pa-agent').value),
      tcp_ports: tr.querySelector('.pa-ports').value.trim(),
      shell: tr.querySelector('.pa-shell').checked,
      tty: tr.querySelector('.pa-tty').value.trim(),
      file: tr.querySelector('.pa-file').value,
      sysinfo: tr.querySelector('.pa-sysinfo').checked,
      metrics: tr.querySelector('.pa-metrics').checked,
      config: tr.querySelector('.pa-config').checked,
      web: tr.querySelector('.pa-web').checked,
    }));
    return { name: document.getElementById('pc-name').value.trim(),
      description: document.getElementById('pc-desc').value.trim(), users, acl };
  },
  async saveClient() {
    const body = this.collectClient();
    if (!body.name) { UI.toast('名称不能为空', true); return; }
    const id = this.expandKey.startsWith('client:') && this.expandKey !== 'client:new' ? this.expandKey.split(':')[1] : '';
    try {
      const r = await Api[id ? 'put' : 'post']('/api/v1/permission/client-groups' + (id ? '/' + id : ''), body);
      this.expandKey = 'client:' + r.id; this.detail = null; UI.toast('已保存'); this.refresh();
    } catch (e) { UI.toast('保存失败：' + e.message, true); }
  },
  collectAgent() {
    const devices = [...document.querySelectorAll('#pa-devices .perm-chip')].map(x => x.dataset.v);
    return { name: document.getElementById('pa-name').value.trim(),
      description: document.getElementById('pa-desc').value.trim(), devices,
      caps: {
        tcp_ports: document.getElementById('pa-ports').value.trim(),
        shell: document.getElementById('pa-shell').checked,
        tty: document.getElementById('pa-tty').value.trim(),
        file: document.getElementById('pa-file').value,
        sysinfo: document.getElementById('pa-sysinfo').checked,
        metrics: document.getElementById('pa-metrics').checked,
        config: document.getElementById('pa-config').checked,
        web: document.getElementById('pa-web').checked,
      } };
  },
  async saveAgent() {
    const body = this.collectAgent();
    if (!body.name) { UI.toast('名称不能为空', true); return; }
    const id = this.expandKey.startsWith('agent:') && this.expandKey !== 'agent:new' ? this.expandKey.split(':')[1] : '';
    try {
      const r = await Api[id ? 'put' : 'post']('/api/v1/permission/agent-groups' + (id ? '/' + id : ''), body);
      this.expandKey = 'agent:' + r.id; this.detail = null; UI.toast('已保存'); this.refresh();
    } catch (e) { UI.toast('保存失败：' + e.message, true); }
  },
  enter(el) { this.el = el; return Page.prototype.enter.call(this, el, []); },
});


// ---------------------------------------------------------------- Pages.分组
const GroupsPage = Object.assign(new Page(), {
  refreshInterval() { return 0; },
  capText(c) {
    const bits = [];
    if (c.shell === '1' || c.shell === 'true') bits.push('Shell');
    if (c.tcp_ports) bits.push('TCP ' + c.tcp_ports);
    if (c.tty) bits.push('TTY ' + c.tty);
    if (c.file && c.file !== 'none') bits.push('文件 ' + c.file);
    if (c.sysinfo === '1' || c.sysinfo === 'true') bits.push('SYSINFO');
    if (c.metrics === '1' || c.metrics === 'true') bits.push('指标');
    return bits.join(' · ') || '无';
  },
  capOn(c, k) { return c[k] === '1' || c[k] === 'true'; },
  async refresh() {
    if (this.editId) return this.renderDetail();
    const arr = await Api.get('/api/v1/groups');
    const rows = arr.map(g => [
      '<b>' + UI.esc(g.name) + '</b>',
      g.device_count,
      g.subject_count,
      '<span class="muted">' + UI.esc(this.capText(g.caps || {})) + '</span>',
      UI.ago(g.updated_ms),
      '<button class="g-edit" data-id="' + g.id + '">编辑</button> ' +
      '<button class="g-del" data-id="' + g.id + '" data-name="' + UI.esc(g.name) + '">删除</button>',
    ]);
    this.el.innerHTML = UI.panel('分组',
      UI.detailTable(['名称', '设备数', '账号数', '能力', '更新时间', '操作'], rows),
      { right: '<button class="primary" id="g-new">+ 新建分组</button>' });
    document.getElementById('g-new').onclick = () => {
      const m = UI.modal({
        title: '新建分组',
        body: '<label>名称<input id="g-new-name" placeholder="例如 产线A"></label>' +
          '<label>描述<input id="g-new-desc" placeholder="可选"></label>',
        buttons: '<button data-modal-close>取消</button><button class="primary" id="g-new-ok">创建</button>',
      });
      m.el.querySelector('#g-new-ok').onclick = async () => {
        const name = m.el.querySelector('#g-new-name').value.trim();
        if (!name) { UI.toast('名称不能为空', true); return; }
        try {
          const r = await Api.post('/api/v1/groups', {
            name,
            description: m.el.querySelector('#g-new-desc').value.trim(),
          });
          m.close();
          this.editId = r.id;
          this.refresh();
        } catch (e) { UI.toast('创建失败：' + e.message, true); }
      };
    };
    this.el.querySelectorAll('.g-edit').forEach(b => {
      b.onclick = (e) => { e.stopPropagation(); this.editId = Number(b.dataset.id); this.refresh(); };
    });
    this.el.querySelectorAll('.g-del').forEach(b => {
      b.onclick = async (e) => {
        e.stopPropagation();
        if (!await UI.confirm({ title: '删除分组', message: '删除 ' + b.dataset.name + '？有关联账号时会拒绝。', danger: true })) return;
        try { await Api.del('/api/v1/groups/' + b.dataset.id); UI.toast('已删除'); this.refresh(); }
        catch (err) { UI.toast('删除失败：' + err.message, true); }
      };
    });
  },
  async renderDetail() {
    const g = await Api.get('/api/v1/groups/' + this.editId);
    const caps = g.caps || {};
    const members = (g.devices || []).map(d =>
      '<tr><td><code>' + UI.esc(d) + '</code></td><td style="text-align:right">' +
      '<button class="g-mdel" data-dev="' + UI.esc(d) + '">移出</button></td></tr>').join('') ||
      '<tr><td colspan="2" class="empty">暂无设备</td></tr>';
    const subjects = (g.subjects || []).map(x =>
      '<span class="group-subject"><b>' + (x.type === 'admin' ? '管理员' : '客户端') + '</b> ' +
      UI.esc(x.id) + ' <button class="g-sdel" data-type="' + x.type + '" data-id="' + UI.esc(x.id) + '">×</button></span>').join(' ') ||
      '<span class="muted">暂无账号</span>';
    const body =
      '<div class="cfg-body group-basic">' +
        '<label>名称 <input id="g-name" value="' + UI.esc(g.name) + '"></label>' +
        '<label>描述 <input id="g-desc" value="' + UI.esc(g.description || '') + '" placeholder="可选"></label>' +
      '</div>' +
      '<div class="group-grid">' +
        '<div class="group-box"><h3>设备成员</h3>' +
          '<div class="ui-flex"><input id="g-dev" placeholder="输入 devid，可逗号分隔"><button id="g-add">添加</button></div>' +
          '<table class="detail-table"><tbody>' + members + '</tbody></table></div>' +
        '<div class="group-box"><h3>能力策略</h3>' +
          '<div class="group-caps">' +
            '<label class="group-cap"><input type="checkbox" id="g-shell"' + (this.capOn(caps,'shell') ? ' checked' : '') + '> Shell</label>' +
            '<label class="group-cap"><input type="checkbox" id="g-sysinfo"' + (this.capOn(caps,'sysinfo') ? ' checked' : '') + '> SYSINFO</label>' +
            '<label class="group-cap"><input type="checkbox" id="g-metrics"' + (this.capOn(caps,'metrics') ? ' checked' : '') + '> 指标</label>' +
            '<label class="group-cap"><input type="checkbox" id="g-config"' + (this.capOn(caps,'config') ? ' checked' : '') + '> 修改配置</label>' +
            '<label class="group-cap"><input type="checkbox" id="g-web"' + (this.capOn(caps,'web') ? ' checked' : '') + '> Web</label>' +
          '</div>' +
          '<label class="group-field">TCP 端口 <input id="g-ports" value="' + UI.esc(caps.tcp_ports || '') + '" placeholder="22,80,8000-8100 或 *"></label>' +
          '<label class="group-field">TTY <input id="g-tty" value="' + UI.esc(caps.tty || '') + '" placeholder="ttyS*,ttyUSB0"></label>' +
          '<label class="group-field">文件 <select id="g-file">' +
            ['none','ro','rw'].map(v => '<option value="' + v + '"' + ((caps.file || 'none') === v ? ' selected' : '') + '>' + v + '</option>').join('') +
          '</select></label></div>' +
      '</div>' +
      '<div class="group-box"><h3>关联账号</h3>' +
        '<div class="group-subjects">' + subjects + '</div>' +
        '<div class="ui-flex" style="margin-top:10px">' +
          '<select id="g-stype"><option value="admin">管理员</option><option value="client">客户端</option></select>' +
          '<input id="g-sid" placeholder="用户名"><button id="g-sadd">加入分组</button>' +
        '</div></div>' +
      '<div class="ui-form-foot"><span class="spacer"></span>' +
        '<button id="g-back">返回</button><button class="primary" id="g-save">保存分组</button></div>';
    this.el.innerHTML = UI.panel('分组：' + UI.esc(g.name), body,
      { right: '<button class="danger" id="g-remove">删除分组</button>' });

    document.getElementById('g-back').onclick = () => { this.editId = null; this.refresh(); };
    document.getElementById('g-remove').onclick = async () => {
      if (!await UI.confirm({ title: '删除分组', message: '删除 ' + g.name + '？', danger: true })) return;
      try { await Api.del('/api/v1/groups/' + g.id); this.editId = null; UI.toast('已删除'); this.refresh(); }
      catch (e) { UI.toast('删除失败：' + e.message, true); }
    };
    document.getElementById('g-save').onclick = async () => {
      const caps = {
        tcp_ports: document.getElementById('g-ports').value.trim(),
        shell: document.getElementById('g-shell').checked ? '1' : '0',
        tty: document.getElementById('g-tty').value.trim(),
        file: document.getElementById('g-file').value,
        sysinfo: document.getElementById('g-sysinfo').checked ? '1' : '0',
        metrics: document.getElementById('g-metrics').checked ? '1' : '0',
        config: document.getElementById('g-config').checked ? '1' : '0',
        web: document.getElementById('g-web').checked ? '1' : '0',
      };
      try {
        await Api.put('/api/v1/groups/' + g.id, {
          name: document.getElementById('g-name').value.trim(),
          description: document.getElementById('g-desc').value.trim(),
          caps,
        });
        UI.toast('已保存');
        this.refresh();
      } catch (e) { UI.toast('保存失败：' + e.message, true); }
    };
    document.getElementById('g-add').onclick = async () => {
      const raw = document.getElementById('g-dev').value.trim();
      const devids = raw.split(',').map(x => x.trim()).filter(Boolean);
      if (!devids.length) return;
      try { await Api.post('/api/v1/groups/' + g.id + '/devices', { devids }); this.refresh(); }
      catch (e) { UI.toast('添加失败：' + e.message, true); }
    };
    this.el.querySelectorAll('.g-mdel').forEach(b => {
      b.onclick = async () => {
        try { await Api.del('/api/v1/groups/' + g.id + '/devices/' + encodeURIComponent(b.dataset.dev)); this.refresh(); }
        catch (e) { UI.toast('移出失败：' + e.message, true); }
      };
    });
    document.getElementById('g-sadd').onclick = async () => {
      const type = document.getElementById('g-stype').value;
      const id = document.getElementById('g-sid').value.trim();
      if (!id) return;
      try { await Api.post('/api/v1/groups/' + g.id + '/subjects', { type, id }); this.refresh(); }
      catch (e) { UI.toast('加入失败：' + e.message, true); }
    };
    this.el.querySelectorAll('.g-sdel').forEach(b => {
      b.onclick = async () => {
        try {
          await Api.del('/api/v1/groups/' + g.id + '/subjects/' + b.dataset.type + '/' + encodeURIComponent(b.dataset.id));
          this.refresh();
        } catch (e) { UI.toast('移出失败：' + e.message, true); }
      };
    });
  },
  enter(el) { this.el = el; return Page.prototype.enter.call(this, el, []); },
});


const SettingsPage = Object.assign(new Page(), {
  refreshInterval() { return 0; },
  async refresh() {
    const st = await Api.get('/api/v1/settings');
    const schema = st._schema || [];
    const groups = [];
    schema.forEach(d => { if (groups.indexOf(d.group) < 0) groups.push(d.group); });
    let html = '';
    groups.forEach(g => {
      const defs = schema.filter(d => d.group === g);
      const tip = d => {
        const range = (d.min === 0 && d.max === 0) ? '' :
          '范围 ' + d.min + '–' + d.max + (d.unit ? ' ' + d.unit : '');
        return UI.esc((d.desc || '') + (range ? ' · ' + range : ''));
      };
      const rows = defs.map(d => [
        '<span class="setting-name">' + UI.esc(d.label) + '</span>' +
        '<span class="tip" tabindex="0" data-tip="' + tip(d) + '">ⓘ</span>',
        '<b>' + UI.esc(st[d.key] || '—') + '</b>' +
        (d.unit ? ' <span class="muted">' + UI.esc(d.unit) + '</span>' : '')]);
      const table = UI.kvTable(rows, { header: ['设置项', '当前值'], cls: 'settings-kv', shellCls: 'settings-kv-shell' });
      const form = this.editGroup === g ? UI.form({
        cls: 'setting-form',
        rows: defs.map(d => {
          const isText = (d.min === 0 && d.max === 0);
          const input = isText
            ? '<input type="text" data-set-input="' + d.key + '" value="' + UI.esc(st[d.key] || '') + '">'
            : '<input type="number" data-set-input="' + d.key + '" value="' + UI.esc(st[d.key] || '') +
              '" min="' + d.min + '" max="' + d.max + '">';
          return { label: UI.esc(d.label), control: input, unit: UI.esc(d.unit || '') };
        }),
        left: '<span class="muted">保存后立即生效</span>',
        right: '<button data-set-cancel="1">取消</button>' +
          '<button class="primary" data-set-save="' + UI.esc(g) + '">保存本组</button>',
      }) : '';
      html += UI.panel(UI.esc(g), table + form, { cls: 'mb16',
        right: '<button data-set-edit="' + UI.esc(g) + '">' + (this.editGroup === g ? '收起' : '编辑') + '</button>' });
    });
    this.el.innerHTML = html;

    this.el.querySelectorAll('[data-set-edit]').forEach(b => {
      b.onclick = () => {
        this.editGroup = this.editGroup === b.dataset.setEdit ? null : b.dataset.setEdit;
        this.refresh();
      };
    });
    this.el.querySelectorAll('[data-set-cancel]').forEach(b => {
      b.onclick = () => { this.editGroup = null; this.refresh(); };
    });
    this.el.querySelectorAll('[data-set-save]').forEach(b => {
      b.onclick = async () => {
        const g = b.dataset.setSave;
        const defs = schema.filter(d => d.group === g);
        const body = {};
        defs.forEach(d => {
          const el = document.querySelector('[data-set-input="' + d.key + '"]');
          if (el) body[d.key] = el.value;
        });
        try {
          await Api.put('/api/v1/settings', body);
          try {
            window.SETTINGS = await Api.get('/api/v1/settings');
            applySiteTitle();
          } catch (e) {}
          this.editGroup = null;
          UI.toast('已保存，立即生效');
          this.refresh();
        } catch (e) { UI.toast('保存失败：' + e.message, true); }
      };
    });
  },
  enter(el) { this.el = el; return Page.prototype.enter.call(this, el, []); },
});


// ---------------------------------------------------------------- 串口调试
const TtyPage = Object.assign(new Page(), {
  refreshInterval() { return 0; },
  enter(el, params) {
    this.el = el;
    this.devid = decodeURIComponent(params[0] || '');
    const raw = params[1] || '';
    this.alias = decodeURIComponent(raw.split('?')[0]);
    this.monitor = /[?&]monitor=1/.test(raw);
    this.takeover = /[?&]takeover=1/.test(raw);
    this.entries = [];
    this.paused = false;
    this.showTs = true;
    this.showHex = true;
    this.showText = true;
    this.maxLines = 2000;
    el.innerHTML = '<div class="panel"><div class="panel-head">' +
      '<h2>串口调试 · ' + UI.esc(this.alias) + '</h2><span class="spacer"></span>' +
      '<span id="tty-state" class="pill">连接中…</span> ' +
      '<button id="tty-close">断开</button>' +
      '<button id="tty-dl" style="display:none">下载抓流</button></div>' +
      (this.monitor ? '<div class="tty-ro">只读监听</div>' : '') +
      '<div class="tty-console" id="tty-log"></div>' +
      '<div class="tty-send">' +
      '<select id="tty-mode"><option value="text">文本</option><option value="hex">HEX</option></select>' +
      '<select id="tty-eol"><option value="">无行尾</option><option value="\r\n">CRLF</option>' +
      '<option value="\n">LF</option><option value="\r">CR</option></select>' +
      '<input id="tty-input" placeholder="输入后 Ctrl+Enter 发送">' +
      '<button id="tty-send">发送</button></div>' +
      '<div class="tty-tools">' +
      '<label><input type="checkbox" id="tty-ts" checked> 时间戳</label>' +
      '<label><input type="checkbox" id="tty-hex" checked> HEX</label>' +
      '<label><input type="checkbox" id="tty-text" checked> 文本</label>' +
      '<label><input type="checkbox" id="tty-pause"> 暂停</label>' +
      '<button id="tty-clear">清空</button><button id="tty-save">保存日志</button>' +
      ' <a href="#/agents/' + encodeURIComponent(this.devid) + '">返回设备</a></div></div>';
    if (this.monitor) {
      ['tty-mode','tty-eol','tty-input','tty-send'].forEach(id => { const e = document.getElementById(id); if (e) e.disabled = true; });
    }
    this.logEl = document.getElementById('tty-log');
    this.inputEl = document.getElementById('tty-input');
    const $ = id => document.getElementById(id);
    $('tty-close').onclick = () => { if (this.ws) this.ws.close(); };
    $('tty-dl').onclick = () => {
      if (this.captureFile) location.href = '/api/v1/ttylogs/' + encodeURIComponent(this.captureFile);
    };
    $('tty-send').onclick = () => this.send();
    this.inputEl.onkeydown = e => { if (e.key === 'Enter' && (e.ctrlKey || e.metaKey)) this.send(); };
    $('tty-clear').onclick = () => { this.entries = []; this.renderLog(); };
    $('tty-save').onclick = () => this.saveLog();
    ['ts','hex','text','pause'].forEach(k => {
      $(('tty-' + k)).onchange = e => {
        this['show' + k[0].toUpperCase() + k.slice(1)] = e.target.checked;
        if (k === 'pause') this.paused = e.target.checked;
        this.renderLog();
      };
    });
    this.connect();
  },
  leave() {
    if (this.ws) { this.ws.onclose = null; this.ws.close(); this.ws = null; }
  },
  connect() {
    const proto = location.protocol === 'https:' ? 'wss:' : 'ws:';
    let url = proto + '//' + location.host + '/ws/tty/' +
      encodeURIComponent(this.devid) + '/' + encodeURIComponent(this.alias);
    if (this.monitor) url += '?monitor=1';
    if (this.takeover) url += '?takeover=1';
    const ws = new WebSocket(url);
    ws.binaryType = 'arraybuffer';
    this.ws = ws;
    ws.onopen = () => { document.getElementById('tty-state').textContent = '已连接'; };
    ws.onclose = () => { const st = document.getElementById('tty-state'); if (st) st.textContent = '已断开'; };
    ws.onerror = () => { const st = document.getElementById('tty-state'); if (st) st.textContent = '连接失败'; };
    ws.onmessage = ev => {
      if (typeof ev.data === 'string') {
        try {
          const j = JSON.parse(ev.data);
          if (j.op === 'error') { this.addEntry('sys', j.msg || 'error', null); }
          if (j.op === 'open_ok' && j.file) {
            this.captureFile = j.file;
            const dl = document.getElementById('tty-dl');
            if (dl) dl.style.display = '';
          }
        } catch (e) {}
        return;
      }
      const bytes = new Uint8Array(ev.data);
      this.addEntry('rx', null, bytes);
    };
  },
  send() {
    if (!this.ws || this.ws.readyState !== 1) return;
    const mode = document.getElementById('tty-mode').value;
    const eol = document.getElementById('tty-eol').value;
    let raw = this.inputEl.value;
    let bytes;
    if (mode === 'hex') {
      const clean = raw.replace(/[^0-9a-fA-F]/g, '');
      const out = new Uint8Array(Math.floor(clean.length / 2));
      for (let i = 0; i < out.length; i++) out[i] = parseInt(clean.substr(i * 2, 2), 16);
      bytes = out;
    } else {
      bytes = new TextEncoder().encode(raw + eol);
    }
    if (!bytes.length) return;
    let bin = '';
    bytes.forEach(b => { bin += String.fromCharCode(b); });
    this.ws.send(JSON.stringify({ op: 'write', data: btoa(bin) }));
    this.addEntry('tx', null, bytes);
    this.inputEl.value = '';
  },
  addEntry(dir, text, bytes) {
    this.entries.push({ ts: Date.now(), dir, text, bytes });
    if (this.entries.length > this.maxLines) this.entries.shift();
    if (!this.paused) this.renderLog();
  },
  fmtHex(bytes) {
    return Array.from(bytes).map(b => b.toString(16).padStart(2, '0')).join(' ');
  },
  fmtText(bytes) {
    if (!bytes) return '';
    return new TextDecoder('utf-8', { fatal: false }).decode(bytes);
  },
  renderLog() {
    if (!this.logEl) return;
    const parts = this.entries.map(e => {
      const t = this.showTs ? '<span class="tty-ts">' + new Date(e.ts).toLocaleTimeString('zh-CN', { hour12: false }) +
        '.' + String(e.ts % 1000).padStart(3, '0') + '</span> ' : '';
      const d = '<span class="tty-' + e.dir + '">' + (e.dir === 'rx' ? '←' : e.dir === 'tx' ? '→' : '!') + '</span> ';
      if (e.dir === 'sys') return '<div class="tty-line">' + t + d + UI.esc(e.text || '') + '</div>';
      let body = '';
      if (this.showHex) body += '<span class="tty-hex">' + this.fmtHex(e.bytes) + '</span> ';
      if (this.showText) body += '<span class="tty-text">' + UI.esc(this.fmtText(e.bytes).replace(/\r/g, '\\r').replace(/\n/g, '\\n')) + '</span>';
      return '<div class="tty-line">' + t + d + body + '</div>';
    });
    this.logEl.innerHTML = parts.join('');
    this.logEl.scrollTop = this.logEl.scrollHeight;
  },
  saveLog() {
    const lines = this.entries.map(e => {
      if (e.dir === 'sys') return '[' + new Date(e.ts).toISOString() + '] SYS ' + (e.text || '');
      return '[' + new Date(e.ts).toISOString() + '] ' + e.dir.toUpperCase() + ' ' + this.fmtHex(e.bytes);
    });
    const blob = new Blob([lines.join('\n')], { type: 'text/plain' });
    const a = document.createElement('a');
    a.href = URL.createObjectURL(blob);
    a.download = this.devid + '-' + this.alias.replace(/[^A-Za-z0-9_.-]/g, '_') + '-tty.log';
    a.click();
    URL.revokeObjectURL(a.href);
  },
});

Router.register(/^#\/tty\/([^/]+)\/(.+)$/, TtyPage, '串口调试');
Router.register(/^#\/agents\/([^/]+)$/, AgentDetailPage, '设备详情');

// 路由注册（页面定义在本文件）
Router.register(/^#\/clients$/, ClientsPage, '客户端 A');
Router.register(/^#\/streams$/, StreamsPage, '会话');
Router.register(/^#\/certs$/, CertsPage, '证书');
Router.register(/^#\/acl$/, AclPage, 'ACL');
Router.register(/^#\/audit$/, AuditPage, '审计');
Router.register(/^#\/files(?:\?.*)?$/, FilesPage, '文件');
Router.register(/^#\/terminal\/([^/]+)$/, TerminalPage, '终端');
Router.register(/^#\/permission$/, PermissionPage, '权限');
Router.register(/^#\/groups$/, GroupsPage, '分组');
Router.register(/^#\/settings$/, SettingsPage, '设置');
