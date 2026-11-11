// 接入账号（A 侧身份）列表与详情
const ACC_STATUS = { active: ['启用', 'ok'], disabled: ['停用', 'info'] };
const TOK_STATUS = { active: ['启用', 'ok'], disabled: ['停用', 'info'], revoked: ['已吊销', 'bad'] };
const TOK_PERIOD = { total: '总量', daily: '每日', monthly: '每月' };

function accGB(bytes) {
    const n = Number(bytes) || 0;
    if (!n) return '不限';
    const gb = n / (1024 * 1024 * 1024);
    return (gb >= 10 ? Math.round(gb) : gb.toFixed(1)) + ' GB';
}

function accGBInput(bytes) {
    const n = Number(bytes) || 0;
    if (!n) return 0;
    return Math.round(n / (1024 * 1024 * 1024) * 10) / 10;
}

function tokExpiry(ms) {
    if (!ms) return '永久';
    if (Date.now() >= ms) return '已过期';
    return UI.time(ms);
}

// 仅供 datetime-local 输入框使用
function tokLocalInput(ms) {
    if (!ms) return '';
    const d = new Date(ms);
    const pad = n => String(n).padStart(2, '0');
    return d.getFullYear() + '-' + pad(d.getMonth() + 1) + '-' + pad(d.getDate()) +
      'T' + pad(d.getHours()) + ':' + pad(d.getMinutes());
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
    const head = '<tr><th>用户名</th><th>状态</th><th>Token</th><th>最近使用</th><th>操作</th></tr>';
    const rows = arr.map(a =>
      '<tr class="acc-row" data-id="' + a.id + '">' +
        '<td><b>' + UI.esc(a.username) + '</b>' +
          (a.display_name ? '<div class="muted-sm">' + UI.esc(a.display_name) + '</div>' : '') + '</td>' +
        '<td>' + UI.status(a.status, ACC_STATUS) + '</td>' +
        '<td>' + (a.token_count || 0) + '</td>' +
        '<td class="muted">' + (a.last_used_ms ? UI.time(a.last_used_ms) : '—') + '</td>' +
        '<td class="acc-ops"><a class="acc-detail" href="#/access/' + a.id + '">详情 ›</a></td>' +
      '</tr>').join('');

    this.el.innerHTML = UI.panel('接入账号',
      '<div class="acc-toolbar">' +
        '<input id="acc-q" placeholder="搜索用户名 / 展示名" value="' + UI.esc(q) + '">' +
        '<span class="spacer"></span>' +
        (canManage() ? '<button class="primary" id="acc-new">+ 新建账号</button>' : '') +
      '</div>' +
      '<div class="perm-table-wrap"><table class="detail-table acc-table"><thead>' + head +
      '</thead><tbody>' + (rows || '<tr><td colspan="5" class="empty">暂无接入账号</td></tr>') +
      '</tbody></table></div>');

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
        '<label>用户名<input id="aa-user" placeholder="与客户端 --user 一致" maxlength="64"></label>' +
        '<label>展示名<input id="aa-display" placeholder="可选" maxlength="128"></label>' +
        '<label class="acc-span2">备注<input id="aa-note" placeholder="可选" maxlength="256"></label>' +
        '</div><div class="muted-sm">账号创建后在「权限」页加入客户端分组才会获得设备权限。</div>',
      buttons: '<button data-modal-close>取消</button><button class="primary" id="aa-ok">创建</button>',
    });
    m.el.querySelector('#aa-ok').onclick = async () => {
      const username = m.el.querySelector('#aa-user').value.trim();
      if (!username) { UI.toast('用户名必填', true); return; }
      try {
        await Api.post('/api/v1/client-accounts', {
          username,
          display_name: m.el.querySelector('#aa-display').value.trim(),
          note: m.el.querySelector('#aa-note').value.trim(),
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
    this.editingAccount = false;   // 账号信息表转为表单编辑
    this.accountDirty = false;
    this.gone = false;
    this._tabs = null;
    return Page.prototype.enter.call(this, el, params);
  },

  leave() {
    if (this._tabs) { this._tabs.destroy(); this._tabs = null; }
    this.editingAccount = false;
    this.accountDirty = false;
    Page.prototype.leave.call(this);
  },

  async refresh() {
    // 行内表单 / 账号信息编辑期间暂停刷新，避免输入被覆盖
    if (this.editId || this.editingAccount) return;
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
    const danger = isAdmin && !this.editingAccount
      ? '<div class="acc-danger"><span class="muted-sm">删除会同时移除 Token 与分组成员关系。</span>' +
        '<span class="spacer"></span><button class="danger" data-act="acc-del">删除账号</button></div>'
      : '';
    const cardRight = this.editingAccount
      ? '<button data-act="acc-cancel">取消</button> <button class="primary" data-act="acc-save">保存</button>'
      : (canM ? '<button data-act="acc-edit">编辑资料</button> ' : '') +
        '<button data-act="acc-effective">有效权限</button>' +
        (canM ? ' <button data-act="acc-toggle">' + (a.status === 'active' ? '停用' : '启用') + '</button>' : '');
    const card1 = UI.panel('账号信息',
      banner +
      '<div class="detail-table-shell"><table class="detail-table acc-kv' +
        (this.editingAccount ? ' is-editing' : '') + '"><tbody>' +
        this.accountKv() + '</tbody></table></div>' + danger,
      { cls: 'mb16', right: cardRight });

    const counts = { all: this.tokens.length, active: 0, disabled: 0, revoked: 0 };
    for (const t of this.tokens) if (counts[t.status] != null) counts[t.status]++;
    const rows = this.filteredTokens().map(t => this.tokenRow(t)).join('');
    const card2 = UI.panel('Token',
      '<div id="tok-tabs"></div>' +
      '<div class="detail-table-shell acc-token-shell"><table class="detail-table acc-token-table"><thead><tr>' +
      '<th>名称 / 前缀</th><th>状态 / 到期</th><th>流量</th><th>频率 · 并发</th>' +
      '<th>最近使用</th><th>操作</th></tr></thead><tbody>' +
      (rows || '<tr><td colspan="6" class="empty">暂无 Token</td></tr>') +
      '</tbody></table></div>',
      { right: canM ? '<button class="primary" data-act="tok-new">+ 新建 Token</button>' : '' });

    this.el.innerHTML = '<div class="acc-back"><a href="#/access">← 接入账号</a></div>' + card1 + card2;

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
      'acc-save': () => this.saveAccountEdit(),
      'acc-cancel': () => this.cancelAccountEdit(),
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
    if (this.editingAccount) this.bindAccountForm();
  },

  accountKv() {
    if (this.editingAccount) return this.accountKvEdit();
    const a = this.account;
    const last = a.last_used_ms ? UI.time(a.last_used_ms) : '—';
    const ip = a.last_used_ip ? ' · <span class="mono">' + UI.esc(a.last_used_ip) + '</span>' : '';
    const dash = '<span class="muted">—</span>';
    return '' +
      '<tr><th>用户名</th><td class="mono">' + UI.esc(a.username) + '</td>' +
        '<th>状态</th><td>' + UI.status(a.status, ACC_STATUS) + '</td></tr>' +
      '<tr><th>账号 ID</th><td>' + a.id + '</td>' +
        '<th>展示名</th><td>' + (a.display_name ? UI.esc(a.display_name) : dash) + '</td></tr>' +
      '<tr><th>备注</th><td colspan="3" class="acc-wrap">' + (a.note ? UI.esc(a.note) : dash) + '</td></tr>' +
      '<tr><th>创建人</th><td>' + (a.created_by ? UI.esc(a.created_by) : dash) + '</td>' +
        '<th>创建时间</th><td>' + (a.created_ms ? UI.time(a.created_ms) : dash) + '</td></tr>' +
      '<tr><th>更新时间</th><td>' + (a.updated_ms ? UI.time(a.updated_ms) : dash) + '</td>' +
        '<th>最近使用</th><td>' + last + ip + '</td></tr>' +
      '<tr><th>Token</th><td colspan="3">' + (a.token_count || 0) + ' 个（启用 ' +
        (a.token_active || 0) + ' / 停用 ' + (a.token_disabled || 0) +
        ' / 已吊销 ' + (a.token_revoked || 0) + '）</td></tr>' +
      '<tr><th>权限摘要</th><td colspan="3" class="acc-wrap">' + this.permSummary() + '</td></tr>';
  },

  // 编辑态：同一张表，可改字段变成控件，只读字段保持文本
  accountKvEdit() {
    const a = this.account;
    const last = a.last_used_ms ? UI.time(a.last_used_ms) : '—';
    const ip = a.last_used_ip ? ' · <span class="mono">' + UI.esc(a.last_used_ip) + '</span>' : '';
    const dash = '<span class="muted">—</span>';
    return '' +
      '<tr><th>用户名</th><td class="mono">' + UI.esc(a.username) + '</td>' +
        '<th>状态</th><td><select id="ae-status" class="acc-inline">' +
          '<option value="active"' + (a.status === 'active' ? ' selected' : '') + '>启用</option>' +
          '<option value="disabled"' + (a.status === 'disabled' ? ' selected' : '') + '>停用</option>' +
        '</select></td></tr>' +
      '<tr><th>账号 ID</th><td>' + a.id + '</td>' +
        '<th>展示名</th><td><input id="ae-display" class="acc-inline" maxlength="128" value="' +
          UI.esc(a.display_name || '') + '" placeholder="可空"></td></tr>' +
      '<tr><th>备注</th><td colspan="3"><textarea id="ae-note" class="acc-inline" rows="2" ' +
        'maxlength="256" placeholder="可空">' + UI.esc(a.note || '') + '</textarea></td></tr>' +
      '<tr><th>创建人</th><td>' + (a.created_by ? UI.esc(a.created_by) : dash) + '</td>' +
        '<th>创建时间</th><td>' + (a.created_ms ? UI.time(a.created_ms) : dash) + '</td></tr>' +
      '<tr><th>更新时间</th><td>' + (a.updated_ms ? UI.time(a.updated_ms) : dash) + '</td>' +
        '<th>最近使用</th><td>' + last + ip + '</td></tr>' +
      '<tr><th>Token</th><td colspan="3">' + (a.token_count || 0) + ' 个（启用 ' +
        (a.token_active || 0) + ' / 停用 ' + (a.token_disabled || 0) +
        ' / 已吊销 ' + (a.token_revoked || 0) + '）</td></tr>' +
      '<tr><th>权限摘要</th><td colspan="3" class="acc-wrap">' + this.permSummary() + '</td></tr>' +
      '<tr><td colspan="4" class="acc-edit-cell"><div class="acc-edit-foot">' +
        '<span id="ae-hint" class="tok-hint"></span></div></td></tr>';
  },

  startAccountEdit() {
    if (!this.account) return;
    this.editId = 0;
    this.editDirty = false;
    this.editingAccount = true;
    this.accountDirty = false;
    this.render();
  },

  async cancelAccountEdit() {
    if (!(await this.leaveAccountEdit())) return;
    await this.refresh();
  },

  // 账号编辑期间做其它操作：先收起表单，有改动先确认
  async leaveAccountEdit() {
    if (!this.editingAccount) return true;
    if (this.accountDirty && !await UI.confirm({
      title: '放弃修改', message: '账号信息表单还未保存，继续操作会放弃修改。',
    })) return false;
    this.editingAccount = false;
    this.accountDirty = false;
    return true;
  },

  bindAccountForm() {
    const form = this.el.querySelector('.acc-kv.is-editing');
    if (!form) return;
    const mark = () => { this.accountDirty = true; this.updateAccountHint(); };
    form.querySelectorAll('input,select,textarea').forEach(el => {
      el.addEventListener('input', mark);
      el.addEventListener('change', mark);
    });
    form.addEventListener('keydown', e => {
      if (e.key === 'Escape') { e.preventDefault(); this.cancelAccountEdit(); }
      else if (e.key === 'Enter' && e.target.tagName === 'INPUT') {
        e.preventDefault();
        this.saveAccountEdit();
      }
    });
    setTimeout(() => {
      const el = form.querySelector('#ae-display');
      if (el) { el.focus(); el.select(); }
    }, 0);
    this.updateAccountHint();
  },

  updateAccountHint() {
    const hint = this.el.querySelector('#ae-hint');
    if (!hint || !this.account) return;
    const status = this.el.querySelector('#ae-status').value;
    const msgs = [];
    let warn = false;
    if (status !== this.account.status && status === 'disabled') {
      warn = true;
      msgs.push('保存后账号立即停用，名下 ' + (this.account.token_count || 0) +
        ' 个 Token 全部失效，在线连接被断开');
    }
    if (!msgs.length) msgs.push('用户名不可修改；保存后立即生效');
    hint.textContent = msgs.join('；') + '。';
    hint.classList.toggle('warn', warn);
  },

  async saveAccountEdit() {
    const a = this.account;
    const q = sel => this.el.querySelector(sel);
    const display = q('#ae-display');
    const note = q('#ae-note');
    const status = q('#ae-status');
    if (!display || !note || !status) return;
    if (status.value !== a.status && status.value === 'disabled' && !await UI.confirm({
      title: '停用账号',
      message: '停用后其全部 Token 立即失效，在线连接会被断开。',
      danger: true,
    })) return;
    const btn = q('[data-act="acc-save"]');
    if (btn) btn.disabled = true;
    try {
      await Api.put('/api/v1/client-accounts/' + a.id, {
        display_name: display.value.trim(),
        note: note.value.trim(),
        status: status.value,
      });
      this.editingAccount = false;
      this.accountDirty = false;
      UI.toast('已保存');
      await this.refresh();
    } catch (e) {
      if (btn) btn.disabled = false;
      const hint = q('#ae-hint');
      if (hint) { hint.textContent = '保存失败：' + e.message; hint.classList.add('warn'); }
    }
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

  tokenRow(t) {
    const editing = this.editId === t.id;
    const used = Number(t.used_bytes) || 0;
    const quota = Number(t.quota_bytes) || 0;
    const pct = quota ? Math.min(100, Math.floor(used * 100 / quota)) : 0;
    const barCls = quota && used >= quota ? 'bad' : (pct >= 80 ? 'warn' : '');
    const quotaText = quota
      ? UI.bytes(used) + ' / ' + accGB(quota) + ' · ' + (TOK_PERIOD[t.quota_period] || '') + ' · ' + pct + '%'
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
    let html = '<tr class="tok-row' + (editing ? ' is-editing' : '') + '" data-id="' + t.id + '">' +
      '<td data-label="名称"><div class="tok-cell"><b>' + UI.esc(t.name || '未命名') + '</b>' +
        '<div class="mono muted-sm">' + UI.esc(t.token_prefix) + '…</div></div></td>' +
      '<td data-label="状态"><div class="tok-cell">' + UI.status(t.status, TOK_STATUS) +
        '<div class="muted-sm">' + UI.esc(tokExpiry(t.expires_at_ms)) + '</div></div></td>' +
      '<td data-label="流量"><div class="tok-cell">' +
        (quota ? '<progress class="acc-progress ' + barCls + '" max="100" value="' + pct + '"></progress>' : '') +
        '<div class="muted-sm">' + UI.esc(quotaText) + '</div></div></td>' +
      '<td data-label="频率"><div class="tok-cell">' + rateText + ' · ' + sessText +
        '<div class="muted-sm">' + burstText + '</div></div></td>' +
      '<td data-label="最近" class="muted"><div class="tok-cell">' +
        (t.last_used_ms ? UI.time(t.last_used_ms) : '—') +
        (t.last_used_ip ? '<div class="muted-sm mono">' + UI.esc(t.last_used_ip) + '</div>' : '') +
        '</div></td>' +
      '<td data-label="操作" class="acc-ops"><div class="tok-cell">' + ops + '</div></td></tr>';
    if (editing) html += this.editRow(t);
    return html;
  },

  editRow(t) {
    const quotaGb = accGBInput(t.quota_bytes);
    const opts = (map, cur) => Object.keys(map).map(k =>
      '<option value="' + k + '"' + (cur === k ? ' selected' : '') + '>' + map[k] + '</option>').join('');
    return '<tr class="tok-edit-row"><td colspan="6">' +
      '<div class="tok-form" data-id="' + t.id + '" data-used="' + (Number(t.used_bytes) || 0) +
        '" data-period="' + UI.esc(t.quota_period) + '">' +
      '<label>名称<input id="te-name" maxlength="64" value="' + UI.esc(t.name || '') + '"></label>' +
      '<label>到期<input id="te-expire" type="datetime-local" value="' +
        UI.esc(tokLocalInput(t.expires_at_ms)) + '"></label>' +
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
    if (this.editingAccount && !await this.leaveAccountEdit()) {
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
    if (!await this.leaveAccountEdit()) return;
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
    if (!await this.leaveAccountEdit()) return;
    try {
      await Api.put('/api/v1/client-tokens/' + btn.dataset.id, { status: btn.dataset.status });
      UI.toast(btn.dataset.status === 'active' ? '已启用' : '已停用');
      this.editId = 0;
      this.editDirty = false;
      await this.refresh();
    } catch (e) { UI.toast('操作失败：' + e.message, true); }
  },

  async rotateToken(btn) {
    if (!await this.leaveAccountEdit()) return;
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
    if (!await this.leaveAccountEdit()) return;
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
    if (!await this.leaveAccountEdit()) return;
    const accountId = this.id;
    const m = UI.modal({
      title: '新建 Token',
      body: '<div class="perm-grid2">' +
        '<label class="acc-span2">名称<input id="at-name" placeholder="例如 构建机 / 巡检" maxlength="64"></label>' +
        '<label>有效期<select id="at-expire">' +
          '<option value="0">永久</option><option value="7">7 天</option>' +
          '<option value="30" selected>30 天</option><option value="90">90 天</option>' +
          '<option value="365">1 年</option></select></label>' +
        '<label>流量配额<input id="at-quota" type="number" min="0" step="1" value="0" placeholder="GB，0 = 不限"></label>' +
        '<label>周期<select id="at-period">' +
          '<option value="monthly" selected>每月</option><option value="daily">每日</option>' +
          '<option value="total">总量</option></select></label>' +
        '<label>超限动作<select id="at-action">' +
          '<option value="cut" selected>断开连接</option>' +
          '<option value="block">只拒绝新连接</option></select></label>' +
        '<label>频率（次/分）<input id="at-rate" type="number" min="0" value="0" placeholder="0 = 不限"></label>' +
        '<label>突发<input id="at-burst" type="number" min="0" value="0" placeholder="0 = 取频率值"></label>' +
        '<label>并发会话<input id="at-sessions" type="number" min="0" value="0" placeholder="0 = 不限"></label>' +
        '</div><div class="muted-sm">Token 权限等于账号的客户端分组权限，这里只能进一步收窄。</div>',
      buttons: '<button data-modal-close>取消</button><button class="primary" id="at-ok">创建</button>',
    });
    m.el.querySelector('#at-ok').onclick = async () => {
      const days = Number(m.el.querySelector('#at-expire').value) || 0;
      try {
        const r = await Api.post('/api/v1/client-accounts/' + accountId + '/tokens', {
          name: m.el.querySelector('#at-name').value.trim(),
          expires_in_days: days,
          quota_gb: Number(m.el.querySelector('#at-quota').value) || 0,
          quota_period: m.el.querySelector('#at-period').value,
          quota_action: m.el.querySelector('#at-action').value,
          rate_per_min: Number(m.el.querySelector('#at-rate').value) || 0,
          burst: Number(m.el.querySelector('#at-burst').value) || 0,
          max_sessions: Number(m.el.querySelector('#at-sessions').value) || 0,
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
