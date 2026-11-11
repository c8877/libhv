// 接入账号（A 侧身份）与 Token 页面
const ACC_STATUS = { active: ['启用', 'ok'], disabled: ['停用', 'info'] };
const TOK_STATUS = { active: ['启用', 'ok'], disabled: ['停用', 'info'], revoked: ['已吊销', 'bad'] };
const TOK_PERIOD = { total: '总量', daily: '每日', monthly: '每月' };

function accGB(bytes) {
    const n = Number(bytes) || 0;
    if (!n) return '不限';
    const gb = n / (1024 * 1024 * 1024);
    return (gb >= 10 ? Math.round(gb) : gb.toFixed(1)) + ' GB';
}

function tokExpiry(ms) {
    if (!ms) return '永久';
    if (Date.now() >= ms) return '已过期';
    return UI.time(ms);
}

const AccessAccountsPage = Object.assign(new Page(), {
  // 有展开行与输入框，自动刷新会打断操作
  refreshInterval() { return 0; },

  expandId: 0,
  tokens: {},

  async refresh() {
    const q = this.q || '';
    const arr = await Api.get('/api/v1/client-accounts' + (q ? '?q=' + encodeURIComponent(q) : ''));
    this.accounts = {};
    for (const a of arr) this.accounts[a.id] = a;
    if (this.expandId) await this.loadTokens(this.expandId);
    const head = '<tr><th>用户名</th><th>状态</th><th>Token</th><th>最近使用</th><th>操作</th></tr>';
    let rows = '';
    for (const a of arr) {
      const open = this.expandId === a.id;
      rows += '<tr class="acc-row" data-id="' + a.id + '">' +
        '<td><b>' + UI.esc(a.username) + '</b>' +
          (a.display_name ? '<div class="muted-sm">' + UI.esc(a.display_name) + '</div>' : '') + '</td>' +
        '<td>' + UI.status(a.status, ACC_STATUS) + '</td>' +
        '<td>' + (a.token_count || 0) + '</td>' +
        '<td class="muted">' + (a.last_used_ms ? UI.time(a.last_used_ms) : '—') + '</td>' +
        '<td class="acc-ops">' +
          '<button data-act="effective" data-id="' + a.id + '">有效权限</button> ' +
          (canManage() ? '<button data-act="toggle" data-id="' + a.id + '" data-status="' +
            (a.status === 'active' ? 'disabled' : 'active') + '">' +
            (a.status === 'active' ? '停用' : '启用') + '</button> ' : '') +
          (roleOf() === 'admin' ? '<button class="danger" data-act="del-acc" data-id="' + a.id +
            '" data-name="' + UI.esc(a.username) + '">删除</button>' : '') +
        '</td></tr>';
      if (open) {
        rows += '<tr class="perm-expand"><td colspan="5">' + this.tokenPanel(a) + '</td></tr>';
      }
    }
    if (!rows) rows = '<tr><td colspan="5" class="empty">暂无接入账号</td></tr>';

    this.el.innerHTML = UI.panel('接入账号',
      '<div class="acc-toolbar">' +
        '<input id="acc-q" placeholder="搜索用户名 / 展示名" value="' + UI.esc(q) + '">' +
        '<span class="spacer"></span>' +
        (canManage() ? '<button class="primary" id="acc-new">+ 新建账号</button>' : '') +
      '</div>' +
      '<div class="perm-table-wrap"><table class="detail-table acc-table"><thead>' + head +
      '</thead><tbody>' + rows + '</tbody></table></div>');

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
      tr.onclick = async () => {
        const id = Number(tr.dataset.id);
        this.expandId = this.expandId === id ? 0 : id;
        if (this.expandId) await this.loadTokens(this.expandId);
        this.refresh();
      };
    });

    UI.actions(this.el, {
      effective: (btn) => this.openEffective(btn.dataset.id),
      toggle: async (btn) => {
        const a = (this.accounts || {})[btn.dataset.id] || {};
        try {
          await Api.put('/api/v1/client-accounts/' + btn.dataset.id, {
            display_name: a.display_name || '', note: a.note || '', status: btn.dataset.status,
          });
          UI.toast(btn.dataset.status === 'active' ? '已启用' : '已停用，在线连接已断开');
          this.refresh();
        } catch (e) { UI.toast('操作失败：' + e.message, true); }
      },
      'del-acc': async (btn) => {
        if (!await UI.confirm({
          title: '删除接入账号',
          message: '删除 ' + btn.dataset.name + '？其 Token、分组成员关系会一并移除。',
          danger: true,
        })) return;
        try {
          await Api.del('/api/v1/client-accounts/' + btn.dataset.id);
          UI.toast('已删除');
          this.expandId = 0;
          this.refresh();
        } catch (e) { UI.toast('删除失败：' + e.message, true); }
      },
      'new-token': (btn) => this.openTokenForm(Number(btn.dataset.id)),
      'tok-toggle': async (btn) => {
        try {
          await Api.put('/api/v1/client-tokens/' + btn.dataset.id, { status: btn.dataset.status });
          UI.toast(btn.dataset.status === 'active' ? '已启用' : '已停用');
          this.refresh();
        } catch (e) { UI.toast('操作失败：' + e.message, true); }
      },
      'tok-rotate': async (btn) => {
        if (!await UI.confirm({
          title: '轮换 Token',
          message: '将生成新 Token 并立即吊销旧 Token，使用旧 Token 的连接会被断开。',
          danger: true,
        })) return;
        try {
          const r = await Api.post('/api/v1/client-tokens/' + btn.dataset.id + '/rotate', {});
          this.showSecret('Token 已轮换', r.token);
          this.refresh();
        } catch (e) { UI.toast('轮换失败：' + e.message, true); }
      },
      'tok-del': async (btn) => {
        if (!await UI.confirm({ title: '吊销 Token', message: '吊销后不可恢复，用量记录保留。', danger: true })) return;
        try {
          await Api.del('/api/v1/client-tokens/' + btn.dataset.id);
          UI.toast('已吊销');
          this.refresh();
        } catch (e) { UI.toast('吊销失败：' + e.message, true); }
      },
    });
  },

  tokenPanel(account) {
    const list = this.tokens[account.id];
    const rows = (list || []).map(t => {
      const used = Number(t.used_bytes) || 0;
      const quota = Number(t.quota_bytes) || 0;
      const pct = quota ? Math.min(100, Math.floor(used * 100 / quota)) : 0;
      const usage = quota
        ? UI.bytes(used) + ' / ' + accGB(quota) + '（' + pct + '%）'
        : UI.bytes(used) + ' / 不限';
      const period = TOK_PERIOD[t.quota_period] || '';
      const action = t.quota_action === 'block' ? '超限只拒绝新连接' : '超限断开连接';
      const usageCell = usage + (period ? ' · ' + period : '');
      const rate = t.rate_per_min ? t.rate_per_min + ' 次/分' + (t.burst ? '，突发 ' + t.burst : '') : '不限';
      const sess = t.max_sessions ? t.max_sessions + ' 个' : '不限';
      return '<tr>' +
        '<td>' + UI.esc(t.name || '—') + '</td>' +
        '<td class="mono">' + UI.esc(t.token_prefix) + '</td>' +
        '<td>' + UI.status(t.status, TOK_STATUS) + '</td>' +
        '<td>' + UI.esc(tokExpiry(t.expires_at_ms)) + '</td>' +
        '<td title="' + UI.esc(action) + '">' + UI.esc(usageCell) + '</td>' +
        '<td>' + rate + '</td>' +
        '<td>' + sess + '</td>' +
        '<td class="muted">' + (t.last_used_ms ? UI.time(t.last_used_ms) : '—') + '</td>' +
        '<td class="acc-ops">' +
          (canManage() && t.status !== 'revoked'
            ? '<button data-act="tok-toggle" data-id="' + t.id + '" data-status="' +
              (t.status === 'active' ? 'disabled' : 'active') + '">' +
              (t.status === 'active' ? '停用' : '启用') + '</button> ' +
              '<button data-act="tok-rotate" data-id="' + t.id + '">轮换</button> ' +
              '<button class="danger" data-act="tok-del" data-id="' + t.id + '">吊销</button>'
            : '') +
        '</td></tr>';
    }).join('');
    return '<div class="perm-form acc-token-panel">' +
      '<div class="acc-token-head"><h3>Token</h3><span class="spacer"></span>' +
        (canManage() ? '<button class="primary" data-act="new-token" data-id="' + account.id +
          '">+ 新建 Token</button>' : '') +
      '</div>' +
      '<div class="detail-table-shell"><table class="detail-table acc-token-table"><thead><tr>' +
      '<th>名称</th><th>前缀</th><th>状态</th><th>到期</th><th>流量</th><th>频率</th>' +
      '<th>并发</th><th>最近使用</th><th>操作</th></tr></thead><tbody>' +
      (rows || '<tr><td colspan="9" class="empty">暂无 Token</td></tr>') +
      '</tbody></table></div></div>';
  },

  async loadTokens(id) {
    try { this.tokens[id] = await Api.get('/api/v1/client-accounts/' + id + '/tokens'); }
    catch (e) { this.tokens[id] = []; }
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

  openTokenForm(accountId) {
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
      const gb = Number(m.el.querySelector('#at-quota').value) || 0;
      try {
        const r = await Api.post('/api/v1/client-accounts/' + accountId + '/tokens', {
          name: m.el.querySelector('#at-name').value.trim(),
          expires_in_days: days,
          quota_gb: gb,
          quota_period: m.el.querySelector('#at-period').value,
          quota_action: m.el.querySelector('#at-action').value,
          rate_per_min: Number(m.el.querySelector('#at-rate').value) || 0,
          burst: Number(m.el.querySelector('#at-burst').value) || 0,
          max_sessions: Number(m.el.querySelector('#at-sessions').value) || 0,
        });
        m.close();
        this.showSecret('Token 已创建', r.token);
        this.expandId = accountId;
        this.refresh();
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

  async openEffective(id) {
    let d;
    try { d = await Api.get('/api/v1/client-accounts/' + id + '/effective'); }
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

  async enter(el) {
    Page.prototype.enter.call(this, el);
    this.expandId = 0;
    this.tokens = {};
    await this.refresh();
  },
});

Router.register(/^#\/access$/, AccessAccountsPage, '接入账号');
