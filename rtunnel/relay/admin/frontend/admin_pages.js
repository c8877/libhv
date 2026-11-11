// 运维页面：客户端 A、会话、证书、文件、审计、系统设置
// 列表/文件/设置等页面（admin.js 之后加载）
const ClientsPage = Object.assign(new Page(), {
  async refresh() {
    const arr = await Api.get('/api/v1/clients');
    const rows = arr.map(c => [
      '<b>' + UI.esc(c.user) + '</b>', UI.esc(c.remote_addr),
      UI.ago(c.last_seen_ms) + ' 前活跃', c.streams,
      UI.bytes(c.bytes_in) + ' / ' + UI.bytes(c.bytes_out),
      '<button class="shell" data-act="shell" data-user="' + UI.esc(c.user) + '">Shell</button>',
    ]);
    this.el.innerHTML = UI.listPage({
      title: '客户端（A 侧）', summary: '共 ' + arr.length + ' 个',
      columns: ['用户', '远端地址', '最近活跃', '会话数', '流量 ↓/↑', '操作'], rows,
    });
    UI.actions(this.el, {
      shell: (btn) => {
        // A 侧按需 agent：devid = a:<user>，relay 会通知该 client 在本机拉起 agent
        Router.go('#/terminal/' + encodeURIComponent('a:' + btn.dataset.user));
      },
    });
  },
});

// ---------------------------------------------------------------- Pages.会话列表
const StreamsPage = Object.assign(new Page(), {
  async refresh() {
    const arr = await Api.get('/api/v1/streams');
    const STREAM_STATUS = {
      OPEN: ['OPEN', 'ok'], OPENING: ['OPENING', 'info'],
      HALF_CLOSED: ['HALF_CLOSED', 'warn'], default: ['HALF_CLOSED', 'warn'],
    };
    const rows = arr.map(s => [
      UI.esc(s.a_user) + ':' + s.a_sid,
      UI.esc(s.c_devid) + ':' + s.c_sid,
      UI.status(s.state, STREAM_STATUS), UI.ago(s.opened_at_ms),
      '<button class="kick" data-act="kick" data-sid="' + s.a_sid + '">踢掉</button>',
    ]);
    this.el.innerHTML = UI.listPage({
      title: '会话', summary: '共 ' + arr.length + ' 条',
      search: { id: 'st-q', placeholder: '搜索用户 / 设备' },
      columns: ['A 用户:a_sid', 'C 设备:c_sid', '状态', '开启于', '操作'], rows,
    });
    UI.bindSearch(document.getElementById('st-q'));
    UI.actions(this.el, {
      kick: async (btn) => {
        if (!await UI.confirm({ title: '踢掉会话', message: 'a_sid=' + btn.dataset.sid + ' 将被双侧 RESET。', danger: true })) return;
        try {
          await Api.del('/api/v1/streams/' + btn.dataset.sid);
          UI.toast('已踢掉 ' + btn.dataset.sid);
          this.refresh();
        } catch (err) { UI.toast('操作失败：' + err.message, true); }
      },
    });
  },
});

// ---------------------------------------------------------------- Pages.证书
const CertsPage = Object.assign(new Page(), {
  // 页面有输入框（SAN/CSR），自动重渲染会清空正在输入的内容
  refreshInterval() { return 0; },
  async refresh() {
    const arr = await Api.get('/api/v1/certs');
    const CERT_STATUS = {
      valid: ['有效', 'ok'], expiring: ['即将到期', 'warn'],
      expired: ['已过期', 'bad'], revoked: ['已吊销', 'bad'], default: ['已吊销', 'bad'],
    };
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
        exp, UI.status(c.status, CERT_STATUS),
        c.status === 'revoked' ? '' : '<button class="revoke" data-act="revoke" data-san="' + UI.esc(c.san) + '">吊销</button>',
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
    UI.actions(this.el, {
      revoke: async (btn) => {
        if (!await UI.confirm({ title: '吊销证书', message: '吊销 ' + btn.dataset.san + '？在线连接会被踢，后续 AUTH 被拒。', danger: true })) return;
        try {
          await Api.post('/api/v1/certs/revoke', { san: btn.dataset.san });
          UI.toast('已吊销 ' + btn.dataset.san);
          this.refresh();
        } catch (e) { UI.toast('吊销失败：' + e.message, true); }
      },
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
        (f.sha256 ? '<button class="cp" data-act="copy" data-hash="' + UI.esc(f.sha256) + '">复制</button>' : ''),
      UI.time(f.mtime_ms),
      '<button class="dl" data-act="download" data-name="' + UI.esc(f.name) + '">下载</button> ' +
      (canManage() ? '<button class="push" data-act="push" data-name="' + UI.esc(f.name) + '">分发</button> ' : '') +
      (canManage() ? '<button class="del" data-act="del" data-name="' + UI.esc(f.name) + '">删除</button>' : ''),
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
    UI.actions(this.el, {
      copy: (btn) => UI.copy(btn.dataset.hash, '哈希已复制'),
      push: (btn) => Router.go('#/distribute?file=' + encodeURIComponent(btn.dataset.name)),
      download: async (btn) => {
        try {
          const r = await fetch('/api/v1/files/' + encodeURIComponent(btn.dataset.name), { cache: 'no-store' });
          if (!r.ok) throw new Error('HTTP ' + r.status);
          UI.download(btn.dataset.name, await r.blob());
        } catch (e) { UI.toast('下载失败：' + e.message, true); }
      },
      del: async (btn) => {
        if (!await UI.confirm({ title: '删除文件', message: '删除 ' + btn.dataset.name + '？', danger: true })) return;
        try {
          await Api.del('/api/v1/files/' + encodeURIComponent(btn.dataset.name));
          UI.toast('已删除 ' + btn.dataset.name);
          this.refresh();
        } catch (e) { UI.toast('删除失败：' + e.message, true); }
      },
    });
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

Router.register(/^#\/clients$/, ClientsPage, '客户端 A');
Router.register(/^#\/streams$/, StreamsPage, '会话');
Router.register(/^#\/certs$/, CertsPage, '证书');
Router.register(/^#\/audit$/, AuditPage, '审计');
Router.register(/^#\/files(?:\?.*)?$/, FilesPage, '文件');
Router.register(/^#\/settings$/, SettingsPage, '设置');
