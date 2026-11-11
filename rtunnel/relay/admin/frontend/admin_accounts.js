// 接入账号（A 侧身份）列表与详情
const TOK_STATUS = { active: ['启用', 'ok'], disabled: ['停用', 'info'], revoked: ['已吊销', 'bad'] };
const TOK_PERIOD = { total: '总量', daily: '每日', monthly: '每月' };

function tokExpiry(ms) {
    if (!ms) return '永久';
    if (Date.now() >= ms) return '已过期';
    return UI.time(ms);
}

// ---------------------------------------------------------------- 列表
const AccessAccountsPage = Object.assign(new Page(), {
  // 搜索框输入时不能被自动刷新打断
  refreshInterval() { return 0; },

  async refresh() {
    const q = this.q || '';
    let arr;
    try { arr = await Api.get('/api/v1/client-accounts' + (q ? '?q=' + encodeURIComponent(q) : '')); }
    catch (e) {
      this.el.innerHTML = UI.panel('接入账号', '<div class="empty">加载失败：' + UI.esc(e.message) + '</div>');
      return;
    }
    const table = UI.dataTable({
      cls: 'acc-table',
      plain: true,
      cols: [
        { label: '用户名', render: a => '<b>' + UI.esc(a.username) + '</b>' +
            (a.display_name ? '<div class="muted-sm">' + UI.esc(a.display_name) + '</div>' : '') },
        { label: '状态', render: a => UI.status(a.status, UI.STATUS_ACTIVE) },
        { label: 'Token', render: a => a.token_count || 0 },
        { label: '最近使用', cls: 'muted',
          render: a => a.last_used_ms ? UI.time(a.last_used_ms) : '—' },
        { label: '操作', cls: 'acc-ops',
          render: a => '<a class="acc-detail" href="#/access/' + a.id + '">详情 ›</a>' },
      ],
      rows: arr,
      empty: '暂无接入账号',
      mobile: ['title', 'pair', 'pair', 'full', 'ops'],
      rowCls: () => 'acc-row',
      rowAttrs: a => 'data-id="' + a.id + '"',
    });

    this.el.innerHTML = UI.panel('接入账号',
      '<div class="acc-toolbar">' +
        '<input id="acc-q" placeholder="搜索用户名 / 展示名" value="' + UI.esc(q) + '">' +
        '<span class="spacer"></span>' +
        (canManage() ? '<button class="primary" id="acc-new">+ 新建账号</button>' : '') +
      '</div><div class="perm-table-wrap">' + table + '</div>');

    const qel = document.getElementById('acc-q');
    if (qel) {
      qel.oninput = () => {
        clearTimeout(this._qt);
        this._qt = setTimeout(() => { this.q = qel.value.trim(); this.refresh(); }, 250);
      };
    }
    const nb = document.getElementById('acc-new');
    if (nb) nb.onclick = () => this.openAccountForm();

    this.el.querySelectorAll('.acc-row').forEach(tr => {
      tr.onclick = () => Router.go('#/access/' + tr.dataset.id);
    });
  },

  openAccountForm() {
    const m = UI.modal({
      title: '新建接入账号',
      body: '<div class="perm-grid2">' +
        '<label>用户名<input id="aa-user" data-field="username" placeholder="与客户端 --user 一致" maxlength="64"></label>' +
        '<label>展示名<input id="aa-display" data-field="display_name" placeholder="可选" maxlength="128"></label>' +
        '<label class="acc-span2">备注<input id="aa-note" data-field="note" placeholder="可选" maxlength="256"></label>' +
        '</div><div class="muted-sm">账号创建后在「权限」页加入客户端分组才会获得设备权限。</div>',
      buttons: '<button data-modal-close>取消</button><button class="primary" id="aa-ok">创建</button>',
    });
    m.el.querySelector('#aa-ok').onclick = async () => {
      const v = UI.formValues(m.el);
      const username = String(v.username || '').trim();
      if (!username) { UI.toast('用户名必填', true); return; }
      try {
        await Api.post('/api/v1/client-accounts', {
          username,
          display_name: String(v.display_name || '').trim(),
          note: String(v.note || '').trim(),
        });
        UI.toast('已创建 ' + username);
        m.close();
        this.refresh();
      } catch (e) { UI.toast('创建失败：' + e.message, true); }
    };
  },

  async enter(el, params) {
    this.q = this.q || '';
    return Page.prototype.enter.call(this, el, params);
  },
});

// ---------------------------------------------------------------- 详情
const AccessDetailPage = Object.assign(new Page(), {
  refreshInterval() { return this.gone ? 0 : 5000; },

  async enter(el, params) {
    this.id = Number((params && params[0]) || 0);
    this.account = null;
    this.tokens = [];
    this.eff = null;
    this.filter = 'all';
    this.editId = 0;
    this.editDirty = false;
    this.kvEditor = null;          // 账号信息表编辑器（UI.editTable）
    this.gone = false;
    this._tabs = null;
    return Page.prototype.enter.call(this, el, params);
  },

  leave() {
    if (this._tabs) { this._tabs.destroy(); this._tabs = null; }
    this.kvEditor = null;
    Page.prototype.leave.call(this);
  },

  async refresh() {
    // 行内表单 / 账号信息编辑期间暂停刷新，避免输入被覆盖
    if (this.editId || (this.kvEditor && this.kvEditor.isEditing())) return;
    let a;
    try { a = await Api.get('/api/v1/client-accounts/' + this.id); }
    catch (e) {
      if (e.status === 404) {
        this.gone = true;
        this.el.innerHTML = UI.panel('接入账号',
          '<div class="empty">账号不存在或已删除<br><a href="#/access">返回接入账号</a></div>');
        return;
      }
      throw e;
    }
    if (this.gone) this.gone = false;
    this.account = a;
    const [tokens, eff] = await Promise.all([
      Api.get('/api/v1/client-accounts/' + this.id + '/tokens').catch(() => []),
      Api.get('/api/v1/client-accounts/' + this.id + '/effective').catch(() => null),
    ]);
    this.tokens = Array.isArray(tokens) ? tokens : [];
    this.eff = eff;
    this.render();
  },

  render() {
    const a = this.account;
    if (!a) return;
    const canM = canManage();
    const isAdmin = roleOf() === 'admin';
    const banner = a.status === 'disabled'
      ? '<div class="acc-banner">账号已停用，名下 ' + (a.token_count || 0) +
        ' 个 Token 均无法接入；重新启用后立即恢复。</div>'
      : '';
    const danger = isAdmin
      ? '<div class="acc-danger" id="acc-danger">' +
        '<span class="muted-sm">删除会同时移除 Token 与分组成员关系。</span>' +
        '<span class="spacer"></span><button class="danger" data-act="acc-del">删除账号</button></div>'
      : '';
    const card1 = UI.panel('账号信息',
      banner +
      '<div class="detail-table-shell"><div id="acc-kv-host"></div></div>' + danger,
      { cls: 'mb16', right: '<span id="acc-info-actions"></span>' });

    const counts = { all: this.tokens.length, active: 0, disabled: 0, revoked: 0 };
    for (const t of this.tokens) if (counts[t.status] != null) counts[t.status]++;
    const table = UI.dataTable({
      cls: 'acc-token-table',
      shellCls: 'acc-token-shell',
      cols: [
        { label: '名称 / 前缀', mobile: 'title', short: '名称' },
        { label: '状态 / 到期', mobile: 'pair', short: '状态' },
        { label: '流量', mobile: 'pair', short: '流量' },
        { label: '频率 · 并发', mobile: 'pair', short: '频率' },
        { label: '最近使用', mobile: 'pair', short: '最近', cls: 'muted' },
        { label: '操作', mobile: 'ops', short: '操作', cls: 'acc-ops' },
      ],
      rows: this.filteredTokens(),
      empty: '暂无 Token',
      rowCls: t => 'tok-row' + (this.editId === t.id ? ' is-editing' : ''),
      rowAttrs: t => 'data-id="' + t.id + '"',
      tdAttrs: c => c.short ? ' data-label="' + c.short + '"' : '',
      expand: t => this.editId === t.id ? this.editRow(t) : '',
      cells: t => this.tokenCells(t),
    });
    const card2 = UI.panel('Token',
      '<div id="tok-tabs"></div>' + table,
      { right: canM ? '<button class="primary" data-act="tok-new">+ 新建 Token</button>' : '' });

    this.el.innerHTML = '<div class="acc-back"><a href="#/access">← 接入账号</a></div>' + card1 + card2;

    this.mountAccountEditor();

    if (this._tabs) { this._tabs.destroy(); this._tabs = null; }
    this._tabs = UI.tabs(document.getElementById('tok-tabs'), {
      items: [
        { key: 'all', label: '全部', count: counts.all },
        { key: 'active', label: '启用', count: counts.active },
        { key: 'disabled', label: '停用', count: counts.disabled },
        { key: 'revoked', label: '已吊销', count: counts.revoked },
      ],
      value: this.filter,
      onChange: key => this.setFilter(key),
    });

    UI.actions(this.el, {
      'acc-edit': () => this.startAccountEdit(),
      'acc-effective': () => this.openEffective(),
      'acc-toggle': () => this.toggleAccount(),
      'acc-del': () => this.confirmDelete(),
      'tok-new': () => this.openTokenForm(),
      'tok-edit': btn => this.openEdit(Number(btn.dataset.id)),
      'tok-toggle': btn => this.toggleToken(btn),
      'tok-rotate': btn => this.rotateToken(btn),
      'tok-del': btn => this.revokeToken(btn),
      'tok-save': () => this.saveEdit(),
      'tok-cancel': () => this.cancelEdit(),
    });

    if (this.editId) this.bindEditForm();
  },

  // 账号信息字段：只读渲染与编辑控件在同一处声明
  accountFields() {
    const dash = '<span class="muted">—</span>';
    return [
      { key: 'username', label: '用户名',
        view: d => '<span class="mono">' + UI.esc(d.username) + '</span>' },
      { key: 'status', label: '状态',
        view: d => UI.status(d.status, UI.STATUS_ACTIVE),
        edit: { type: 'switch', id: 'ae-status', onValue: 'active', offValue: 'disabled',
          onLabel: '启用', offLabel: '停用' } },
      { key: 'id', label: '账号 ID', view: d => d.id },
      { key: 'display_name', label: '展示名',
        view: d => d.display_name ? UI.esc(d.display_name) : dash,
        edit: { type: 'text', id: 'ae-display', placeholder: '可空', maxlength: 128 } },
      { key: 'note', label: '备注', span: 2, cls: 'acc-wrap',
        view: d => d.note ? UI.esc(d.note) : dash,
        edit: { type: 'textarea', id: 'ae-note', rows: 2, maxlength: 256, placeholder: '可空' } },
      { key: 'created_by', label: '创建人',
        view: d => d.created_by ? UI.esc(d.created_by) : dash },
      { key: 'created_ms', label: '创建时间',
        view: d => d.created_ms ? UI.time(d.created_ms) : dash },
      { key: 'updated_ms', label: '更新时间',
        view: d => d.updated_ms ? UI.time(d.updated_ms) : dash },
      { key: 'last_used', label: '最近使用',
        view: d => (d.last_used_ms ? UI.time(d.last_used_ms) : '—') +
          (d.last_used_ip ? ' · <span class="mono">' + UI.esc(d.last_used_ip) + '</span>' : '') },
      { key: 'token_count', label: 'Token', span: 2,
        view: d => (d.token_count || 0) + ' 个（启用 ' + (d.token_active || 0) +
          ' / 停用 ' + (d.token_disabled || 0) + ' / 已吊销 ' + (d.token_revoked || 0) + '）' },
      { key: 'perm', label: '权限摘要', span: 2, cls: 'acc-wrap', view: () => this.permSummary() },
    ];
  },

  mountAccountEditor() {
    const host = document.getElementById('acc-kv-host');
    if (!host) return;
    const canM = canManage();
    this.kvEditor = UI.editTable(host, {
      data: this.account,
      cls: 'acc-kv',
      fields: this.accountFields(),
      actions: document.getElementById('acc-info-actions'),
      renderActions: (editing, a) => editing
        ? '<button data-edit-cancel>取消</button> <button class="primary" data-edit-save>保存</button>'
        : (canM ? '<button data-act="acc-edit">编辑资料</button> ' : '') +
          '<button data-act="acc-effective">有效权限</button>' +
          (canM ? ' <button data-act="acc-toggle">' + (a.status === 'active' ? '停用' : '启用') + '</button>' : ''),
      hint: (v, d) => {
        const msgs = [];
        let warn = false;
        if (v.status !== d.status && v.status === 'disabled') {
          warn = true;
          msgs.push('保存后账号立即停用，名下 ' + (d.token_count || 0) +
            ' 个 Token 全部失效，在线连接被断开');
        }
        if (!msgs.length) msgs.push('用户名不可修改；保存后立即生效');
        return { text: msgs.join('；') + '。', warn };
      },
      confirmSave: async (v, d) => {
        if (v.status !== d.status && v.status === 'disabled') {
          return UI.confirm({ title: '停用账号',
            message: '停用后其全部 Token 立即失效，在线连接会被断开。', danger: true });
        }
        return true;
      },
      save: async v => {
        await Api.put('/api/v1/client-accounts/' + this.account.id, {
          display_name: v.display_name.trim(),
          note: v.note.trim(),
          status: v.status,
        });
      },
      onSaved: () => { UI.toast('已保存'); this.refresh(); },
      onStateChange: editing => {
        const d = document.getElementById('acc-danger');
        if (d) d.style.display = editing ? 'none' : '';
      },
      dirtyMessage: '账号信息表单还未保存，继续操作会放弃修改。',
      focus: '#ae-display',
    });
  },

  // 账号表单与 Token 表单互斥：先收起 Token 表单，再进入账号编辑
  async startAccountEdit() {
    if (this.editId) {
      if (this.editDirty && !await UI.confirm({
        title: '放弃修改', message: '另一个 Token 的表单还未保存，切换会放弃修改。',
      })) return;
      this.editId = 0;
      this.editDirty = false;
      await this.refresh();
    }
    if (this.kvEditor) this.kvEditor.start();
  },

  async leaveAccount() {
    if (!this.kvEditor) return true;
    return this.kvEditor.leave();
  },

  permSummary() {
    const e = this.eff;
    if (!e) return '<span class="muted">未加入客户端分组</span>';
    const groups = (e.groups || []).length;
    if (!groups) return '<span class="muted">未加入客户端分组</span>';
    const caps = e.caps || {};
    return UI.esc('分组 ' + groups + ' · 设备 ' + (e.devices || []).length + ' 台 · TCP ' +
      (caps.tcp_ports || '无') + ' · 文件 ' + (caps.file || 'none') + ' · Shell ' +
      (caps.shell === '1' ? '允许' : '禁止'));
  },

  filteredTokens() {
    if (this.filter === 'all') return this.tokens;
    return this.tokens.filter(t => t.status === this.filter);
  },

  tokenCells(t) {
    const editing = this.editId === t.id;
    const used = Number(t.used_bytes) || 0;
    const quota = Number(t.quota_bytes) || 0;
    const pct = quota ? Math.min(100, Math.floor(used * 100 / quota)) : 0;
    const barCls = quota && used >= quota ? 'bad' : (pct >= 80 ? 'warn' : '');
    const quotaText = quota
      ? UI.bytes(used) + ' / ' + UI.gb(quota) + ' · ' + (TOK_PERIOD[t.quota_period] || '') + ' · ' + pct + '%'
      : UI.bytes(used) + ' / 不限';
    const rateText = t.rate_per_min ? t.rate_per_min + '/分' : '频率不限';
    const burstText = t.rate_per_min ? '突发 ' + (t.burst || t.rate_per_min) : '无频率限制';
    const sessText = t.max_sessions ? '并发 ' + t.max_sessions : '并发不限';
    const ops = t.status === 'revoked'
      ? '<span class="muted-sm">—</span>'
      : canManage()
        ? '<button data-act="tok-edit" data-id="' + t.id + '">' + (editing ? '收起' : '编辑') + '</button> ' +
          '<button data-act="tok-toggle" data-id="' + t.id + '" data-status="' +
            (t.status === 'active' ? 'disabled' : 'active') + '">' +
            (t.status === 'active' ? '停用' : '启用') + '</button> ' +
          '<button data-act="tok-rotate" data-id="' + t.id + '">轮换</button> ' +
          '<button class="danger" data-act="tok-del" data-id="' + t.id + '">吊销</button>'
        : '';
    return [
      '<div class="tok-cell"><b>' + UI.esc(t.name || '未命名') + '</b>' +
        '<div class="mono muted-sm">' + UI.esc(t.token_prefix) + '…</div></div>',
      '<div class="tok-cell">' + UI.status(t.status, TOK_STATUS) +
        '<div class="muted-sm">' + UI.esc(tokExpiry(t.expires_at_ms)) + '</div></div>',
      '<div class="tok-cell">' +
        (quota ? '<progress class="acc-progress ' + barCls + '" max="100" value="' + pct + '"></progress>' : '') +
        '<div class="muted-sm">' + UI.esc(quotaText) + '</div></div>',
      '<div class="tok-cell">' + rateText + ' · ' + sessText +
        '<div class="muted-sm">' + burstText + '</div></div>',
      '<div class="tok-cell">' +
        (t.last_used_ms ? UI.time(t.last_used_ms) : '—') +
        (t.last_used_ip ? '<div class="muted-sm mono">' + UI.esc(t.last_used_ip) + '</div>' : '') +
        '</div>',
      '<div class="tok-cell">' + ops + '</div>',
    ];
  },

  editRow(t) {
    const quotaGb = UI.gbValue(t.quota_bytes);
    const opts = (map, cur) => Object.keys(map).map(k =>
      '<option value="' + k + '"' + (cur === k ? ' selected' : '') + '>' + map[k] + '</option>').join('');
    return '<tr class="tok-edit-row"><td colspan="6">' +
      '<div class="tok-form" data-id="' + t.id + '" data-used="' + (Number(t.used_bytes) || 0) +
        '" data-period="' + UI.esc(t.quota_period) + '">' +
      '<label>名称<input id="te-name" maxlength="64" value="' + UI.esc(t.name || '') + '"></label>' +
      '<label>到期<input id="te-expire" type="datetime-local" value="' +
        UI.esc(UI.localInput(t.expires_at_ms)) + '"></label>' +
      '<label>流量配额<span class="tok-unit"><input id="te-quota" type="number" min="0" step="0.1" value="' +
        quotaGb + '"><span>GB</span></span></label>' +
      '<label>周期<select id="te-period">' + opts(TOK_PERIOD, t.quota_period) + '</select></label>' +
      '<label>超限动作<select id="te-action">' +
        '<option value="cut"' + (t.quota_action !== 'block' ? ' selected' : '') + '>断开连接</option>' +
        '<option value="block"' + (t.quota_action === 'block' ? ' selected' : '') + '>只拒绝新连接</option>' +
        '</select></label>' +
      '<label>频率（次/分）<input id="te-rate" type="number" min="0" step="1" value="' +
        (t.rate_per_min || 0) + '"></label>' +
      '<label>突发<input id="te-burst" type="number" min="0" step="1" value="' +
        (t.burst || 0) + '"></label>' +
      '<label>并发会话<input id="te-sessions" type="number" min="0" step="1" value="' +
        (t.max_sessions || 0) + '"></label>' +
      '<div class="tok-form-foot"><span id="te-hint" class="tok-hint"></span>' +
        '<span class="spacer"></span><button data-act="tok-cancel">取消</button>' +
        '<button class="primary" data-act="tok-save">保存并生效</button></div>' +
      '</div></td></tr>';
  },

  bindEditForm() {
    const form = this.el.querySelector('.tok-form');
    if (!form) return;
    const mark = () => { this.editDirty = true; this.updateEditHint(); };
    form.querySelectorAll('input,select').forEach(el => {
      el.addEventListener('input', mark);
      el.addEventListener('change', mark);
    });
    form.addEventListener('keydown', e => {
      if (e.key === 'Escape') { e.preventDefault(); this.cancelEdit(); }
      else if (e.key === 'Enter' && e.target.tagName === 'INPUT') {
        e.preventDefault();
        this.saveEdit();
      }
    });
    setTimeout(() => {
      const n = form.querySelector('#te-name');
      if (n) { n.focus(); n.select(); }
    }, 0);
    this.updateEditHint();
  },

  updateEditHint() {
    const form = this.el.querySelector('.tok-form');
    if (!form) return;
    const hint = form.querySelector('#te-hint');
    if (!hint) return;
    const used = Number(form.dataset.used) || 0;
    const quota = Math.round((Number(form.querySelector('#te-quota').value) || 0) * 1024 * 1024 * 1024);
    const period = form.querySelector('#te-period').value;
    const action = form.querySelector('#te-action').value;
    const exp = form.querySelector('#te-expire').value;
    const msgs = [];
    let warn = false;
    if (quota > 0 && quota <= used) {
      warn = true;
      msgs.push('新配额低于已用流量，保存后' + (action === 'cut' ? '会立即断开在线连接' : '会拒绝新连接'));
    }
    if (period !== form.dataset.period) {
      warn = true;
      msgs.push('修改周期会清零本周期已用流量');
    }
    if (exp) {
      const t = new Date(exp).getTime();
      if (t && t <= Date.now()) {
        warn = true;
        msgs.push('到期时间早于当前时间，保存后立即失效');
      }
    }
    if (!msgs.length) msgs.push('0 表示不限；修改周期会清零本周期已用流量');
    hint.textContent = msgs.join('；') + '。';
    hint.classList.toggle('warn', warn);
  },

  async setFilter(key) {
    if (key === this.filter) return;
    if (!await this.leaveAccount()) {
      if (this._tabs) this._tabs.setValue(this.filter, false);
      return;
    }
    if (this.editId && this.editDirty &&
        !await UI.confirm({ title: '放弃修改', message: '当前表单还有未保存的修改，切换会放弃。' })) {
      if (this._tabs) this._tabs.setValue(this.filter, false);
      return;
    }
    this.editId = 0;
    this.editDirty = false;
    this.filter = key;
    this.render();
  },

  async openEdit(id) {
    if (!await this.leaveAccount()) return;
    if (this.editId === id) { await this.cancelEdit(); return; }
    if (this.editId && this.editDirty &&
        !await UI.confirm({ title: '放弃修改', message: '另一个 Token 的表单还未保存，切换会放弃修改。' })) return;
    this.editId = id;
    this.editDirty = false;
    this.render();
  },

  async cancelEdit() {
    if (this.editDirty &&
        !await UI.confirm({ title: '放弃修改', message: '表单还有未保存的修改，确定放弃？' })) return;
    this.editId = 0;
    this.editDirty = false;
    await this.refresh();
  },

  async saveEdit() {
    const form = this.el.querySelector('.tok-form');
    if (!form) return;
    const id = Number(form.dataset.id);
    const expire = form.querySelector('#te-expire').value;
    const body = {
      name: form.querySelector('#te-name').value.trim(),
      expires_at_ms: expire ? new Date(expire).getTime() : 0,
      quota_gb: Number(form.querySelector('#te-quota').value) || 0,
      quota_period: form.querySelector('#te-period').value,
      quota_action: form.querySelector('#te-action').value,
      rate_per_min: Number(form.querySelector('#te-rate').value) || 0,
      burst: Number(form.querySelector('#te-burst').value) || 0,
      max_sessions: Number(form.querySelector('#te-sessions').value) || 0,
    };
    const btn = form.querySelector('[data-act="tok-save"]');
    if (btn) btn.disabled = true;
    try {
      await Api.put('/api/v1/client-tokens/' + id, body);
      this.editId = 0;
      this.editDirty = false;
      UI.toast('已保存，立即生效');
      await this.refresh();
    } catch (e) {
      if (btn) btn.disabled = false;
      UI.toast('保存失败：' + e.message, true);
    }
  },

  async toggleToken(btn) {
    if (!await this.leaveAccount()) return;
    try {
      await Api.put('/api/v1/client-tokens/' + btn.dataset.id, { status: btn.dataset.status });
      UI.toast(btn.dataset.status === 'active' ? '已启用' : '已停用');
      this.editId = 0;
      this.editDirty = false;
      await this.refresh();
    } catch (e) { UI.toast('操作失败：' + e.message, true); }
  },

  async rotateToken(btn) {
    if (!await this.leaveAccount()) return;
    if (!await UI.confirm({
      title: '轮换 Token',
      message: '将生成新 Token 并立即吊销旧 Token，使用旧 Token 的连接会被断开。',
      danger: true,
    })) return;
    try {
      const r = await Api.post('/api/v1/client-tokens/' + btn.dataset.id + '/rotate', {});
      this.editId = 0;
      this.editDirty = false;
      this.showSecret('Token 已轮换', r.token);
      await this.refresh();
    } catch (e) { UI.toast('轮换失败：' + e.message, true); }
  },

  async revokeToken(btn) {
    if (!await this.leaveAccount()) return;
    if (!await UI.confirm({ title: '吊销 Token', message: '吊销后不可恢复，用量记录保留。', danger: true })) return;
    try {
      await Api.del('/api/v1/client-tokens/' + btn.dataset.id);
      UI.toast('已吊销');
      this.editId = 0;
      this.editDirty = false;
      await this.refresh();
    } catch (e) { UI.toast('吊销失败：' + e.message, true); }
  },

  async toggleAccount() {
    const a = this.account;
    const next = a.status === 'active' ? 'disabled' : 'active';
    if (next === 'disabled' && !await UI.confirm({
      title: '停用账号',
      message: '停用后其全部 Token 立即失效，在线连接会被断开。',
      danger: true,
    })) return;
    try {
      await Api.put('/api/v1/client-accounts/' + a.id, {
        display_name: a.display_name || '', note: a.note || '', status: next,
      });
      UI.toast(next === 'active' ? '已启用' : '已停用，在线连接已断开');
      await this.refresh();
    } catch (e) { UI.toast('操作失败：' + e.message, true); }
  },

  confirmDelete() {
    const a = this.account;
    const m = UI.modal({
      title: '删除接入账号',
      body: '<div>将删除 <b>' + UI.esc(a.username) + '</b>：</div>' +
        '<ul class="acc-del-list"><li>' + (a.token_count || 0) + ' 个 Token</li>' +
        '<li>' + (a.group_count || 0) + ' 个分组成员关系</li></ul>' +
        '<div class="muted-sm">此操作不可恢复。请输入用户名确认：</div>' +
        '<input id="ad-name" class="acc-del-input" autocomplete="off" placeholder="' +
        UI.esc(a.username) + '">',
      buttons: '<button data-modal-close>取消</button>' +
        '<button class="danger" id="ad-ok" disabled>删除</button>',
    });
    const input = m.el.querySelector('#ad-name');
    const ok = m.el.querySelector('#ad-ok');
    input.oninput = () => { ok.disabled = input.value.trim() !== a.username; };
    input.onkeydown = e => { if (e.key === 'Enter' && !ok.disabled) ok.click(); };
    ok.onclick = async () => {
      if (input.value.trim() !== a.username) return;
      ok.disabled = true;
      try {
        await Api.del('/api/v1/client-accounts/' + a.id);
        m.close();
        UI.toast('已删除 ' + a.username);
        Router.go('#/access');
      } catch (e) {
        ok.disabled = false;
        UI.toast('删除失败：' + e.message, true);
      }
    };
    setTimeout(() => input.focus(), 0);
  },

  async openTokenForm() {
    if (!await this.leaveAccount()) return;
    const accountId = this.id;
    const m = UI.modal({
      title: '新建 Token',
      body: '<div class="perm-grid2">' +
        '<label class="acc-span2">名称<input id="at-name" data-field="name" placeholder="例如 构建机 / 巡检" maxlength="64"></label>' +
        '<label>有效期<select id="at-expire" data-field="expires_in_days">' +
          '<option value="0">永久</option><option value="7">7 天</option>' +
          '<option value="30" selected>30 天</option><option value="90">90 天</option>' +
          '<option value="365">1 年</option></select></label>' +
        '<label>流量配额<input id="at-quota" data-field="quota_gb" type="number" min="0" step="1" value="0" placeholder="GB，0 = 不限"></label>' +
        '<label>周期<select id="at-period" data-field="quota_period">' +
          '<option value="monthly" selected>每月</option><option value="daily">每日</option>' +
          '<option value="total">总量</option></select></label>' +
        '<label>超限动作<select id="at-action" data-field="quota_action">' +
          '<option value="cut" selected>断开连接</option>' +
          '<option value="block">只拒绝新连接</option></select></label>' +
        '<label>频率（次/分）<input id="at-rate" data-field="rate_per_min" type="number" min="0" value="0" placeholder="0 = 不限"></label>' +
        '<label>突发<input id="at-burst" data-field="burst" type="number" min="0" value="0" placeholder="0 = 取频率值"></label>' +
        '<label>并发会话<input id="at-sessions" data-field="max_sessions" type="number" min="0" value="0" placeholder="0 = 不限"></label>' +
        '</div><div class="muted-sm">Token 权限等于账号的客户端分组权限，这里只能进一步收窄。</div>',
      buttons: '<button data-modal-close>取消</button><button class="primary" id="at-ok">创建</button>',
    });
    m.el.querySelector('#at-ok').onclick = async () => {
      const v = UI.formValues(m.el);
      try {
        const r = await Api.post('/api/v1/client-accounts/' + accountId + '/tokens', {
          name: String(v.name || '').trim(),
          expires_in_days: Number(v.expires_in_days) || 0,
          quota_gb: Number(v.quota_gb) || 0,
          quota_period: v.quota_period,
          quota_action: v.quota_action,
          rate_per_min: Number(v.rate_per_min) || 0,
          burst: Number(v.burst) || 0,
          max_sessions: Number(v.max_sessions) || 0,
        });
        m.close();
        this.showSecret('Token 已创建', r.token);
        await this.refresh();
      } catch (e) { UI.toast('创建失败：' + e.message, true); }
    };
  },

  showSecret(title, token) {
    const m = UI.modal({
      title,
      body: '<div class="muted-sm">只显示这一次，关闭后无法再次查看；丢失只能轮换。</div>' +
        '<div class="acc-secret" id="as-value">' + UI.esc(token) + '</div>' +
        '<div class="muted-sm">客户端可直接用 Token 登录：</div>' +
        '<div class="acc-secret acc-cmd">rtunnel-client --token ' + UI.esc(token) +
        ' --socks5 127.0.0.1:1080</div>',
      buttons: '<button id="as-copy">复制</button><button class="primary" data-modal-close>我已保存</button>',
    });
    m.el.querySelector('#as-copy').onclick = () => UI.copy(token, '已复制');
  },

  async openEffective() {
    let d;
    try { d = await Api.get('/api/v1/client-accounts/' + this.id + '/effective'); }
    catch (e) { UI.toast('查询失败：' + e.message, true); return; }
    const caps = d.caps || {};
    const devices = (d.devices || []).map(x => UI.pill(x, 'info')).join(' ') || '<span class="muted">无</span>';
    const groups = (d.groups || []).map(x => UI.pill(x, '')).join(' ') || '<span class="muted">未加入分组</span>';
    const capRows = [
      ['TCP 端口', caps.tcp_ports || '无'],
      ['Shell', caps.shell === '1' ? '允许' : '禁止'],
      ['TTY', caps.tty || '无'],
      ['文件', caps.file || 'none'],
      ['系统信息', caps.sysinfo === '1' ? '允许' : '禁止'],
      ['指标', caps.metrics === '1' ? '允许' : '禁止'],
      ['配置', caps.config === '1' ? '允许' : '禁止'],
      ['Web', caps.web === '1' ? '允许' : '禁止'],
    ];
    const capHtml = '<div class="acc-caps">' + capRows.map(r =>
      '<div class="acc-cap"><span class="k">' + r[0] + '</span><span class="v">' + UI.esc(r[1]) + '</span></div>'
    ).join('') + '</div>';
    UI.modal({
      title: '有效权限 · ' + UI.esc(d.client || ''),
      body: '<div class="acc-eff">' +
        '<div><span class="muted-sm">所属客户端分组</span><div>' + groups + '</div></div>' +
        '<div><span class="muted-sm">可访问设备 · ' + (d.devices || []).length + '</span><div>' + devices + '</div></div>' +
        '<div><span class="muted-sm">有效能力</span>' + capHtml + '</div>' +
        '<div class="muted-sm">结果已叠加设备分组上限与字典停用；设备侧还会按 --allow 再校验一次。</div></div>',
      buttons: '<button data-modal-close>关闭</button>',
    });
  },
});

Router.register(/^#\/access$/, AccessAccountsPage, '接入账号');
Router.register(/^#\/access\/(\d+)$/, AccessDetailPage, '账号详情');
