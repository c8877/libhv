// 分发任务相关页面（admin.js 之后加载，共享其全局工具）
// ---------------------------------------------------------------- Pages.分发
function toLocalInput(ms) {
  const d = new Date(ms);
  const pad = n => String(n).padStart(2, '0');
  return d.getFullYear() + '-' + pad(d.getMonth() + 1) + '-' + pad(d.getDate()) +
    'T' + pad(d.getHours()) + ':' + pad(d.getMinutes());
}

const DistributePage = Object.assign(new Page(), {
  refreshInterval() { return 0; },
  enter(el, params, hash) {
    this.el = el;
    this.file = '';
    const m = (hash || '').match(/[?&]file=([^&]+)/);
    if (m) this.file = decodeURIComponent(m[1]);
    this.q = '';
    this.page = 1;
    this.per = 10;
    this.sortKey = 'devid';
    this.sortAsc = true;
    this.sel = new Set();
    const now = Date.now();
    if (!this.startMs) {
      this.startMs = now;
      this.endMs = now + 24 * 3600 * 1000;
    }
    this.maxRetries = 3;
    return this.refresh();
  },
  async refresh() {
    if (!this.file) {
      this.el.innerHTML = '<div class="empty">缺少文件参数，<a href="#/files">返回文件列表</a></div>';
      return;
    }
    const focused = document.activeElement && document.activeElement.id === 'dist-q';
    const caret = focused ? document.activeElement.selectionStart : 0;
    const [agents, devs] = await Promise.all([
      Api.get('/api/v1/agents'),
      Api.get('/api/v1/devices').catch(() => []),
    ]);
    const online = new Set(agents.map(a => a.devid));
    const map = new Map();
    agents.forEach(a => map.set(a.devid, { devid: a.devid, online: true, remote_addr: a.remote_addr || '', owner: '' }));
    (devs || []).forEach(d => {
      if (!map.has(d.devid)) map.set(d.devid, { devid: d.devid, online: false, remote_addr: '', owner: d.owner || '' });
      else map.get(d.devid).owner = d.owner || '';
    });
    let rows = Array.from(map.values());
    if (this.q) {
      const q = this.q.toLowerCase();
      rows = rows.filter(a => a.devid.toLowerCase().includes(q) || (a.owner || '').toLowerCase().includes(q));
    }
    const dir = this.sortAsc ? 1 : -1;
    rows.sort((a, b) => {
      const x = String(a[this.sortKey] == null ? '' : a[this.sortKey]);
      const y = String(b[this.sortKey] == null ? '' : b[this.sortKey]);
      return (x < y ? -1 : x > y ? 1 : 0) * dir;
    });
    const total = rows.length;
    const pages = Math.max(1, Math.ceil(total / this.per));
    if (this.page > pages) this.page = pages;
    const view = rows.slice((this.page - 1) * this.per, this.page * this.per);
    const onlineIds = rows.filter(a => a.online).map(a => a.devid);
    const allChecked = onlineIds.length > 0 && onlineIds.every(d => this.sel.has(d));

    const head = (k, label) => '<th class="sortable" data-k="' + k + '">' + label +
      (this.sortKey === k ? (this.sortAsc ? ' ▲' : ' ▼') : '') + '</th>';

    this.el.innerHTML =
      '<div class="panel mb16"><div class="panel-head"><h2>分发文件：' +
        UI.esc(this.file) + '</h2><span class="spacer"></span>' +
        '<span class="muted-sm">已选 ' + this.sel.size +
        ' / 在线 ' + onlineIds.length + ' / 共 ' + total + '</span></div>' +
      '<div class="cfg-body">' +
        '<div class="ui-flex">' +
          '<input type="text" id="dist-q" placeholder="搜索设备 / 归属" value="' + UI.esc(this.q) + '" class="ui-w220">' +
          '<input type="text" id="dist-dir" placeholder="目标子目录（可选）" class="ui-w220">' +
        '</div>' +
        '<div class="ui-flex ui-mt8">' +
          '<label class="muted-sm">开始时间 <input type="datetime-local" id="dist-start" value="' +
            toLocalInput(this.startMs) + '"></label>' +
          '<label class="muted-sm">结束时间 <input type="datetime-local" id="dist-end" value="' +
            toLocalInput(this.endMs) + '"></label>' +
          '<label class="muted-sm">最大重试 <input type="number" id="dist-retry" min="0" max="10" value="' +
            this.maxRetries + '" class="ui-w70"></label>' +
        '</div>' +
      '</div>' +
      '<div class="ui-pad-x"><div class="detail-table-shell"><table class="detail-table"><thead><tr><th class="ui-w36"><input type="checkbox" id="dist-all"' +
        (allChecked ? ' checked' : '') + '></th>' + head('devid', '设备') + head('online', '状态') +
        head('owner', '归属') + head('remote_addr', '远端地址') + '</tr></thead><tbody>' +
        view.map(a => '<tr' + (a.online ? '' : ' class="offline-row"') + '><td><input type="checkbox" class="dist-pick" data-dev="' +
          UI.esc(a.devid) + '"' + (this.sel.has(a.devid) ? ' checked' : '') +
          (a.online ? '' : ' disabled') + '></td><td>' + UI.esc(a.devid) + '</td><td>' +
          (a.online ? '<span class="pill ok">在线</span>' : '<span class="pill bad">离线</span>') +
          '</td><td>' + UI.esc(a.owner || '—') + '</td><td>' + UI.esc(a.remote_addr || '—') + '</td></tr>').join('') +
        (view.length ? '' : '<tr><td colspan="5" class="empty">没有匹配的设备</td></tr>') +
      '</tbody></table></div>' +
      UI.pager(this.page, pages, null,
        '<a href="#/files"><button>取消</button></a> ' +
        '<button class="primary" id="dist-submit">创建分发任务（' + this.sel.size + '）</button>') +
      '</div></div>';

    const q = document.getElementById('dist-q');
    q.oninput = () => {
      clearTimeout(this._qt);
      this._qt = setTimeout(() => {
        this.q = q.value.trim();
        this.page = 1;
        this.refresh();
      }, 200);
    };
    if (focused) { q.focus(); q.setSelectionRange(caret, caret); }
    const ds = document.getElementById('dist-start');
    const de = document.getElementById('dist-end');
    if (ds) ds.onchange = () => { this.startMs = new Date(ds.value).getTime() || this.startMs; };
    if (de) de.onchange = () => { this.endMs = new Date(de.value).getTime() || this.endMs; };
    document.querySelectorAll('#page th.sortable').forEach(th => {
      th.onclick = () => {
        const k = th.dataset.k;
        if (this.sortKey === k) this.sortAsc = !this.sortAsc;
        else { this.sortKey = k; this.sortAsc = true; }
        this.refresh();
      };
    });
    document.getElementById('dist-all').onchange = (e) => {
      onlineIds.forEach(d => e.target.checked ? this.sel.add(d) : this.sel.delete(d));
      this.refresh();
    };
    document.querySelectorAll('#page .dist-pick').forEach(cb => {
      cb.onchange = () => {
        if (cb.checked) this.sel.add(cb.dataset.dev); else this.sel.delete(cb.dataset.dev);
        this.refresh();
      };
    });
    UI.bindPager(this.el, () => { this.page--; this.refresh(); }, () => { this.page++; this.refresh(); });
    document.getElementById('dist-submit').onclick = async () => {
      if (!this.sel.size) { UI.toast('请选择设备', true); return; }
      const start = new Date(document.getElementById('dist-start').value).getTime();
      const end = new Date(document.getElementById('dist-end').value).getTime();
      const retry = parseInt(document.getElementById('dist-retry').value || '3', 10);
      if (!start || !end || end <= start) { UI.toast('时间范围不合法', true); return; }
      if (end - start > 30 * 86400 * 1000) { UI.toast('时间窗口不能超过 30 天', true); return; }
      const dir = document.getElementById('dist-dir').value.trim();
      try {
        const r = await Api.post('/api/v1/distributions', {
          file: this.file, dir: dir, devids: Array.from(this.sel),
          start_at_ms: start, deadline_at_ms: end, max_retries: retry,
        });
        if (r.skipped_offline) UI.toast('部分设备离线已跳过：' + r.skipped_offline, true);
        UI.toast('已创建 ' + r.tasks.length + ' 条任务');
        Router.go('#/tasks?batch=' + encodeURIComponent(r.batch_id));
      } catch (e) { UI.toast('创建失败：' + e.message, true); }
    };
  },
});

// ---------------------------------------------------------------- Pages.任务
const TASK_STATUS = {
  scheduled: ['等待开始', 'warn'], queued: ['排队中', 'warn'], running: ['传输中', 'warn'],
  success: ['成功', 'ok'], failed: ['失败', 'bad'], timeout: ['超时', 'bad'],
  canceled: ['已取消', 'info'], conflict: ['冲突', 'warn'], default: ['未知', 'warn'],
};

const TasksPage = Object.assign(new Page(), {
  refreshInterval() { return 2000; },
  enter(el, params, hash) {
    this.el = el;
    this.tab = 'all';
    this.q = '';
    this.page = 1;
    this.per = 20;
    this.batch = '';
    const m = (hash || '').match(/[?&]batch=([^&]+)/);
    if (m) this.batch = decodeURIComponent(m[1]);
    return this.refresh();
  },
  async refresh() {
    if (document.hidden) return;
    if (document.activeElement && document.activeElement.id === 'task-q') return;
    let url = '/api/v1/distributions?page=' + this.page + '&per_page=' + this.per;
    if (this.tab !== 'all') url += '&status=' + this.tab;
    if (this.q) url += '&q=' + encodeURIComponent(this.q);
    if (this.batch) url += '&batch=' + encodeURIComponent(this.batch);
    const [r, sum] = await Promise.all([
      Api.get(url),
      Api.get('/api/v1/distributions/summary').catch(() => ({ all: 0, active: 0, done: 0, failed: 0 })),
    ]);
    const items = r.items || [];
    const pages = Math.max(1, Math.ceil(r.total / this.per));
    const tabCount = { all: sum.all, active: sum.active, done: sum.done, failed_group: sum.failed, conflict: sum.conflict };
    const tabItems = [['all', '全部'], ['active', '进行中'], ['done', '已完成'],
                      ['failed_group', '失败'], ['conflict', '冲突']]
      .map(([k, n]) => ({ key: k, label: n, count: tabCount[k] || 0 }));

    const rows = items.map(t => {
      const size = Number(t.file_size) || 0;
      const sent = Number(t.sent) || 0;
      const pct = size ? Math.min(100, Math.floor(sent * 100 / size)) : 0;
      let ops = '<a href="#/tasks/' + t.id + '">详情</a> ';
      if (t.status === 'failed') ops += '<button class="tr" data-act="retry" data-id="' + t.id + '">重试</button> ';
      if (t.status === 'scheduled' || t.status === 'queued') ops += '<button class="tc" data-act="cancel" data-id="' + t.id + '">取消</button> ';
      if (t.status === 'timeout' && t.extend_count === 0) ops += '<button class="te" data-act="extend" data-id="' + t.id + '">延长</button> ';
      if (t.status === 'conflict') ops += '<button class="tv" data-act="check" data-api="verify" data-id="' + t.id + '">校验</button> ' +
        '<button class="tf" data-act="check" data-api="force" data-id="' + t.id + '">强制重发</button> ';
      return '<tr><td>' + t.id + '</td><td>' + UI.esc(t.file_name) + '</td><td>' + UI.esc(t.devid) +
        '</td><td>' + UI.esc(t.target_path || '') + '</td>' +
        '<td>' + (t.status === 'running' ? UI.bytes(sent) + ' / ' + UI.bytes(size) + ' ' + pct + '%' : UI.bytes(size)) + '</td>' +
        '<td>' + UI.status(t.status, TASK_STATUS, ' title="' + UI.esc(t.error || '') + '"') + '</td>' +
        '<td>' + t.retry_count + ' / ' + t.max_retries + '</td>' +
        '<td>' + UI.time(t.start_at_ms) + '</td>' +
        '<td>' + UI.time(t.deadline_at_ms) + '</td>' +
        '<td>' + ops + '</td></tr>';
    }).join('');

    if (this._tabs) { this._tabs.destroy(); this._tabs = null; }
    this.el.innerHTML =
      '<div class="panel"><div class="panel-head"><h2>任务</h2><span class="spacer"></span>' +
        '<input type="text" id="task-q" placeholder="搜索设备 / 文件" value="' + UI.esc(this.q) + '" class="ui-w220">' +
        (this.batch ? '<span class="pill info">本批：' + UI.esc(this.batch) + ' ×</span>' : '') +
        '<button id="task-refresh">刷新</button></div>' +
      '<div id="task-tabs"></div>' +
      '<div class="ui-tabpanel"><div class="detail-table-shell"><table class="detail-table"><thead><tr>' +
        '<th>ID</th><th>文件</th><th>设备</th><th>目标路径</th><th>进度/大小</th>' +
        '<th>状态</th><th>重试</th><th>开始</th><th>截止</th><th>操作</th></tr></thead><tbody>' +
        (rows || '<tr><td colspan="10" class="empty">暂无任务</td></tr>') +
      '</tbody></table></div>' +
      UI.pager(this.page, pages, r.total) + '</div></div>';

    this._tabs = UI.tabs(document.getElementById('task-tabs'), {
      items: tabItems, value: this.tab,
      onChange: (key) => { this.tab = key; this.page = 1; this.refresh(); },
    });
    const q = document.getElementById('task-q');
    q.oninput = () => {
      clearTimeout(this._qt);
      this._qt = setTimeout(() => { this.q = q.value.trim(); this.page = 1; this.refresh(); }, 200);
    };
    document.getElementById('task-refresh').onclick = () => this.refresh();
    UI.bindPager(this.el, () => { this.page--; this.refresh(); }, () => { this.page++; this.refresh(); });
    if (this.batch) {
      const chip = this.el.querySelector('.pill.info');
      if (chip) chip.onclick = () => { this.batch = ''; this.page = 1; this.refresh(); };
    }
    UI.actions(this.el, {
      retry: async (btn) => {
        try { await Api.post('/api/v1/distributions/' + btn.dataset.id + '/retry', {}); this.refresh(); }
        catch (e) { UI.toast('重试失败：' + e.message, true); }
      },
      cancel: async (btn) => {
        try { await Api.post('/api/v1/distributions/' + btn.dataset.id + '/cancel', {}); this.refresh(); }
        catch (e) { UI.toast('取消失败：' + e.message, true); }
      },
      extend: async (btn) => {
        const cur = new Date().getTime() + 24 * 3600 * 1000;
        const v = await UI.prompt({ title: '延长截止时间', label: '新的截止时间（本地时间，最多 +24h）',
          type: 'datetime-local', value: toLocalInput(cur) });
        if (!v) return;
        const t = new Date(v).getTime();
        if (!t) { UI.toast('时间格式不正确', true); return; }
        try { await Api.post('/api/v1/distributions/' + btn.dataset.id + '/extend', { deadline_at_ms: t }); this.refresh(); }
        catch (e) { UI.toast('延长失败：' + e.message, true); }
      },
      check: async (btn) => {
        try {
          const r = await Api.post('/api/v1/distributions/' + btn.dataset.id + '/' + btn.dataset.api, {});
          UI.toast(r.same ? '设备文件一致，已标记成功' : '设备文件不一致，已重新排队');
          this.refresh();
        } catch (e) { UI.toast('操作失败：' + e.message, true); }
      },
    });
  },
});


// ---------------------------------------------------------------- Pages.任务详情
const TaskDetailPage = Object.assign(new Page(), {
  refreshInterval() { return 3000; },
  enter(el, params) {
    this.el = el;
    this.id = params[0];
    return this.refresh();
  },
  async refresh() {
    const t = await Api.get('/api/v1/distributions/' + this.id);
    const size = Number(t.file_size) || 0;
    const sent = Number(t.sent) || 0;
    const pct = size ? Math.min(100, Math.floor(sent * 100 / size)) : 0;
    const trans = [
      ['设备', UI.esc(t.devid)],
      ['目标路径', UI.esc(t.target_path || '—')],
      ['重试', t.retry_count + ' / ' + t.max_retries],
      ['批次', UI.esc(t.batch_id || '—')],
    ];
    const times = [
      ['创建时间', UI.time(t.created_ms)],
      ['开始时间', UI.time(t.start_at_ms)],
      ['截止时间', UI.time(t.deadline_at_ms)],
      ['操作人', UI.esc(t.actor || '—')],
    ];
    const kv = list => '<div class="kv-list">' + list.map(m =>
      '<div class="kv-row"><span class="k">' + m[0] + '</span>' +
      '<span class="v" title="' + m[1] + '">' + m[1] + '</span></div>').join('') + '</div>';
    this.el.innerHTML =
      '<div class="task-page">' +
        '<div class="panel task-head"><div class="task-title-wrap">' +
          '<h2>任务 #' + t.id + '</h2>' + UI.status(t.status, TASK_STATUS) + '</div>' +
          '<span class="spacer"></span><div class="task-actions">' +
          (t.status === 'failed' ? '<button class="primary" id="td-retry">重试</button>' : '') +
          (t.status === 'scheduled' || t.status === 'queued' ? '<button id="td-cancel">取消</button>' : '') +
          (t.status === 'timeout' && t.extend_count === 0 ? '<button id="td-extend">延长截止</button>' : '') +
          (t.status === 'conflict' ? '<button id="td-verify">校验设备</button>' +
            '<button id="td-force">强制重发</button>' : '') +
          '<a href="#/tasks"><button>返回列表</button></a></div></div>' +
        '<div class="panel task-overview">' +
          '<div class="task-file-name">' + UI.esc(t.file_name) + '</div>' +
          '<div class="task-sub">' + UI.bytes(size) +
            (t.file_sha ? ' · <code title="' + UI.esc(t.file_sha) + '">' + UI.esc(t.file_sha) + '</code> ' +
              '<button id="td-copy" data-hash="' + UI.esc(t.file_sha) + '">复制哈希</button>' : '') +
          '</div>' +
          '<div class="task-bar"><span style="width:' + pct + '%"></span></div>' +
          '<div class="task-stats"><span>' + pct + '%</span><span>' +
            UI.bytes(sent) + ' / ' + UI.bytes(size) + '</span></div>' +
        '</div>' +
        '<div class="task-cols">' +
          UI.panel('传输', kv(trans)) + UI.panel('时间与操作', kv(times)) +
        '</div>' +
        (t.error ? '<div class="panel task-error"><b>错误</b><div>' + UI.esc(t.error) + '</div></div>' : '') +
      '</div>';
    const cp = document.getElementById('td-copy');
    if (cp) cp.onclick = () => UI.copy(cp.dataset.hash, '哈希已复制');
    const rt = document.getElementById('td-retry');
    if (rt) rt.onclick = async () => {
      try { await Api.post('/api/v1/distributions/' + this.id + '/retry', {}); this.refresh(); }
      catch (e) { UI.toast('重试失败：' + e.message, true); }
    };
    const cc = document.getElementById('td-cancel');
    if (cc) cc.onclick = async () => {
      try { await Api.post('/api/v1/distributions/' + this.id + '/cancel', {}); this.refresh(); }
      catch (e) { UI.toast('取消失败：' + e.message, true); }
    };
    const tv = document.getElementById('td-verify');
    const tf = document.getElementById('td-force');
    const doCheck = async (api) => {
      try {
        const r = await Api.post('/api/v1/distributions/' + this.id + '/' + api, {});
        UI.toast(r.same ? '设备文件一致，已标记成功' : '设备文件不一致，已重新排队');
        this.refresh();
      } catch (e) { UI.toast('操作失败：' + e.message, true); }
    };
    if (tv) tv.onclick = () => doCheck('verify');
    if (tf) tf.onclick = () => doCheck('force');
    const ex = document.getElementById('td-extend');
    if (ex) ex.onclick = async () => {
      const v = await UI.prompt({ title: '延长截止时间', label: '新的截止时间（本地时间，最多 +24h）',
        type: 'datetime-local', value: toLocalInput(Date.now() + 24 * 3600 * 1000) });
      if (!v) return;
      try { await Api.post('/api/v1/distributions/' + this.id + '/extend',
        { deadline_at_ms: new Date(v).getTime() }); this.refresh(); }
      catch (e) { UI.toast('延长失败：' + e.message, true); }
    };
  },
});


Router.register(/^#\/distribute(?:\?.*)?$/, DistributePage, '分发文件');
Router.register(/^#\/tasks(?:\?.*)?$/, TasksPage, '任务');
Router.register(/^#\/tasks\/(\d+)$/, TaskDetailPage, '任务详情');
