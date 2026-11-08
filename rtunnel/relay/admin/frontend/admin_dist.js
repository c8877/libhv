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
      '<div class="panel" style="margin-bottom:16px"><div class="panel-head"><h2>分发文件：' +
        UI.esc(this.file) + '</h2><span class="spacer"></span>' +
        '<span style="color:var(--muted);font-size:12px">已选 ' + this.sel.size +
        ' / 在线 ' + onlineIds.length + ' / 共 ' + total + '</span></div>' +
      '<div class="cfg-body">' +
        '<div style="display:flex;gap:10px;flex-wrap:wrap;align-items:center">' +
          '<input type="text" id="dist-q" placeholder="搜索设备 / 归属" value="' + UI.esc(this.q) + '" style="width:220px">' +
          '<input type="text" id="dist-dir" placeholder="目标子目录（可选）" style="width:220px">' +
        '</div>' +
        '<div style="display:flex;gap:10px;flex-wrap:wrap;align-items:center;margin-top:10px">' +
          '<label style="font-size:12px;color:var(--muted)">开始时间 <input type="datetime-local" id="dist-start" value="' +
            toLocalInput(this.startMs) + '"></label>' +
          '<label style="font-size:12px;color:var(--muted)">结束时间 <input type="datetime-local" id="dist-end" value="' +
            toLocalInput(this.endMs) + '"></label>' +
          '<label style="font-size:12px;color:var(--muted)">最大重试 <input type="number" id="dist-retry" min="0" max="10" value="' +
            this.maxRetries + '" style="width:70px"></label>' +
        '</div>' +
      '</div>' +
      '<div style="padding:0 16px 16px;overflow:auto"><table class="tbl"><thead><tr><th style="width:36px"><input type="checkbox" id="dist-all"' +
        (allChecked ? ' checked' : '') + '></th>' + head('devid', '设备') + head('online', '状态') +
        head('owner', '归属') + head('remote_addr', '远端地址') + '</tr></thead><tbody>' +
        view.map(a => '<tr' + (a.online ? '' : ' style="opacity:.55"') + '><td><input type="checkbox" class="dist-pick" data-dev="' +
          UI.esc(a.devid) + '"' + (this.sel.has(a.devid) ? ' checked' : '') +
          (a.online ? '' : ' disabled') + '></td><td>' + UI.esc(a.devid) + '</td><td>' +
          (a.online ? '<span class="pill ok">在线</span>' : '<span class="pill bad">离线</span>') +
          '</td><td>' + UI.esc(a.owner || '—') + '</td><td>' + UI.esc(a.remote_addr || '—') + '</td></tr>').join('') +
        (view.length ? '' : '<tr><td colspan="5" class="empty">没有匹配的设备</td></tr>') +
      '</tbody></table>' +
      '<div style="display:flex;align-items:center;gap:8px;margin-top:12px">' +
        '<button id="dist-prev"' + (this.page <= 1 ? ' disabled' : '') + '>上一页</button>' +
        '<span style="color:var(--muted);font-size:12px">' + this.page + ' / ' + pages + '</span>' +
        '<button id="dist-next"' + (this.page >= pages ? ' disabled' : '') + '>下一页</button>' +
        '<span class="spacer" style="flex:1"></span>' +
        '<a href="#/files"><button>取消</button></a> ' +
        '<button class="primary" id="dist-submit">创建分发任务（' + this.sel.size + '）</button>' +
      '</div></div></div>';

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
    document.getElementById('dist-prev').onclick = () => { this.page--; this.refresh(); };
    document.getElementById('dist-next').onclick = () => { this.page++; this.refresh(); };
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
const STATUS_LABEL = {
  scheduled: '等待开始', queued: '排队中', running: '传输中',
  success: '成功', failed: '失败', timeout: '超时', canceled: '已取消', conflict: '冲突',
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
    const tabs = [['all', '全部'], ['active', '进行中'], ['done', '已完成'],
                  ['failed_group', '失败'], ['conflict', '冲突']]
      .map(([k, n]) => [k, n + ' ' + (tabCount[k] || 0)]);

    const rows = items.map(t => {
      const size = Number(t.file_size) || 0;
      const sent = Number(t.sent) || 0;
      const pct = size ? Math.min(100, Math.floor(sent * 100 / size)) : 0;
      const st = STATUS_LABEL[t.status] || t.status;
      const cls = t.status === 'success' ? 'ok' : (t.status === 'failed' || t.status === 'timeout') ? 'bad'
        : (t.status === 'canceled' ? 'info' : 'warn');
      let ops = '<a href="#/tasks/' + t.id + '">详情</a> ';
      if (t.status === 'failed') ops += '<button class="tr" data-id="' + t.id + '">重试</button> ';
      if (t.status === 'scheduled' || t.status === 'queued') ops += '<button class="tc" data-id="' + t.id + '">取消</button> ';
      if (t.status === 'timeout' && t.extend_count === 0) ops += '<button class="te" data-id="' + t.id + '">延长</button> ';
      if (t.status === 'conflict') ops += '<button class="tv" data-id="' + t.id + '">校验</button> ' +
        '<button class="tf" data-id="' + t.id + '">强制重发</button> ';
      return '<tr><td>' + t.id + '</td><td>' + UI.esc(t.file_name) + '</td><td>' + UI.esc(t.devid) +
        '</td><td>' + UI.esc(t.target_path || '') + '</td>' +
        '<td>' + (t.status === 'running' ? UI.bytes(sent) + ' / ' + UI.bytes(size) + ' ' + pct + '%' : UI.bytes(size)) + '</td>' +
        '<td><span class="pill ' + cls + '" title="' + UI.esc(t.error || '') + '">' + st + '</span></td>' +
        '<td>' + t.retry_count + ' / ' + t.max_retries + '</td>' +
        '<td>' + new Date(t.start_at_ms).toLocaleString() + '</td>' +
        '<td>' + new Date(t.deadline_at_ms).toLocaleString() + '</td>' +
        '<td>' + ops + '</td></tr>';
    }).join('');

    this.el.innerHTML =
      '<div class="panel"><div class="panel-head"><h2>任务</h2><span class="spacer"></span>' +
        tabs.map(([k, n]) => '<button class="tab' + (this.tab === k ? ' active' : '') +
          '" data-tab="' + k + '">' + n + '</button>').join(' ') + '</div>' +
      '<div class="cfg-body" style="display:flex;gap:10px;align-items:center;flex-wrap:wrap">' +
        '<input type="text" id="task-q" placeholder="搜索设备 / 文件" value="' + UI.esc(this.q) + '" style="width:220px">' +
        (this.batch ? '<span class="pill info">本批：' + UI.esc(this.batch) + ' ×</span>' : '') +
        '<span class="spacer" style="flex:1"></span>' +
        '<button id="task-refresh">刷新</button></div>' +
      '<div style="padding:0 16px 16px;overflow:auto"><table class="tbl"><thead><tr>' +
        '<th>ID</th><th>文件</th><th>设备</th><th>目标路径</th><th>进度/大小</th>' +
        '<th>状态</th><th>重试</th><th>开始</th><th>截止</th><th>操作</th></tr></thead><tbody>' +
        (rows || '<tr><td colspan="10" class="empty">暂无任务</td></tr>') +
      '</tbody></table>' +
      '<div style="display:flex;align-items:center;gap:8px;margin-top:12px">' +
        '<button id="task-prev"' + (this.page <= 1 ? ' disabled' : '') + '>上一页</button>' +
        '<span style="color:var(--muted);font-size:12px">' + this.page + ' / ' + pages +
        '（共 ' + r.total + '）</span>' +
        '<button id="task-next"' + (this.page >= pages ? ' disabled' : '') + '>下一页</button>' +
      '</div></div></div>';

    document.querySelectorAll('#page .tab').forEach(b => {
      b.onclick = () => { this.tab = b.dataset.tab; this.page = 1; this.refresh(); };
    });
    const q = document.getElementById('task-q');
    q.oninput = () => {
      clearTimeout(this._qt);
      this._qt = setTimeout(() => { this.q = q.value.trim(); this.page = 1; this.refresh(); }, 200);
    };
    document.getElementById('task-refresh').onclick = () => this.refresh();
    document.getElementById('task-prev').onclick = () => { this.page--; this.refresh(); };
    document.getElementById('task-next').onclick = () => { this.page++; this.refresh(); };
    if (this.batch) {
      const chip = this.el.querySelector('.pill.info');
      if (chip) chip.onclick = () => { this.batch = ''; this.page = 1; this.refresh(); };
    }
    document.querySelectorAll('#page .tr').forEach(b => {
      b.onclick = async () => {
        try { await Api.post('/api/v1/distributions/' + b.dataset.id + '/retry', {}); this.refresh(); }
        catch (e) { UI.toast('重试失败：' + e.message, true); }
      };
    });
    document.querySelectorAll('#page .tc').forEach(b => {
      b.onclick = async () => {
        try { await Api.post('/api/v1/distributions/' + b.dataset.id + '/cancel', {}); this.refresh(); }
        catch (e) { UI.toast('取消失败：' + e.message, true); }
      };
    });
    document.querySelectorAll('#page .tv, #page .tf').forEach(b => {
      b.onclick = async () => {
        const api = b.classList.contains('tv') ? 'verify' : 'force';
        try {
          const r = await Api.post('/api/v1/distributions/' + b.dataset.id + '/' + api, {});
          UI.toast(r.same ? '设备文件一致，已标记成功' : '设备文件不一致，已重新排队');
          this.refresh();
        } catch (e) { UI.toast('操作失败：' + e.message, true); }
      };
    });
    document.querySelectorAll('#page .te').forEach(b => {
      b.onclick = async () => {
        const cur = new Date().getTime() + 24 * 3600 * 1000;
        const v = prompt('新的截止时间（本地时间，最多 +24h）', toLocalInput(cur));
        if (!v) return;
        const t = new Date(v).getTime();
        if (!t) { UI.toast('时间格式不正确', true); return; }
        try { await Api.post('/api/v1/distributions/' + b.dataset.id + '/extend', { deadline_at_ms: t }); this.refresh(); }
        catch (e) { UI.toast('延长失败：' + e.message, true); }
      };
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
    const st = STATUS_LABEL[t.status] || t.status;
    const size = Number(t.file_size) || 0;
    const sent = Number(t.sent) || 0;
    const pct = size ? Math.min(100, Math.floor(sent * 100 / size)) : 0;
    const cls = t.status === 'success' ? 'ok' : (t.status === 'failed' || t.status === 'timeout') ? 'bad'
      : (t.status === 'canceled' ? 'info' : 'warn');
    const meta = [
      ['设备', UI.esc(t.devid)],
      ['目标路径', UI.esc(t.target_path || '—')],
      ['重试', t.retry_count + ' / ' + t.max_retries],
      ['开始时间', new Date(t.start_at_ms).toLocaleString()],
      ['截止时间', new Date(t.deadline_at_ms).toLocaleString()],
      ['创建时间', new Date(t.created_ms).toLocaleString()],
      ['操作人', UI.esc(t.actor || '—')],
      ['批次', UI.esc(t.batch_id || '—')],
    ];
    this.el.innerHTML =
      '<div class="panel" style="max-width:900px"><div class="panel-head"><h2>任务 #' + t.id + '</h2>' +
        '<span class="pill ' + cls + '" style="margin-left:8px">' + UI.esc(st) + '</span>' +
        '<span class="spacer"></span><a href="#/tasks"><button>返回列表</button></a></div>' +
      '<div class="task-hero">' +
        '<div class="task-file"><b>' + UI.esc(t.file_name) + '</b>' +
          '<div class="task-sub">' + UI.bytes(size) + ' · ' +
          (t.file_sha ? '<code title="' + UI.esc(t.file_sha) + '">' + UI.esc(t.file_sha.slice(0, 16)) + '…</code> ' +
            '<button id="td-copy" data-hash="' + UI.esc(t.file_sha) + '">复制哈希</button>' : '—') +
          '</div></div>' +
        '<div class="task-progress"><progress max="100" value="' + pct + '"></progress>' +
          '<div class="task-pct">' + pct + '% · ' + UI.bytes(sent) + ' / ' + UI.bytes(size) + '</div></div>' +
      '</div>' +
      '<div class="kv-grid">' + meta.map(m =>
        '<div class="kv-item"><span class="k">' + m[0] + '</span><span class="v" title="' + m[1] + '">' + m[1] + '</span></div>').join('') +
      '</div>' +
      (t.error ? '<div class="task-error"><b>错误</b> ' + UI.esc(t.error) + '</div>' : '') +
      '<div class="cfg-actions" style="display:flex;justify-content:flex-end;gap:8px;padding:12px 16px">' +
        (t.status === 'failed' ? '<button class="primary" id="td-retry">重试</button> ' : '') +
        (t.status === 'scheduled' || t.status === 'queued' ? '<button id="td-cancel">取消</button> ' : '') +
        (t.status === 'timeout' && t.extend_count === 0 ? '<button id="td-extend">延长截止时间</button>' : '') +
        (t.status === 'conflict' ? '<button id="td-verify">校验设备</button> ' +
          '<button id="td-force">强制重发</button>' : '') +
      '</div></div>';
    const cp = document.getElementById('td-copy');
    if (cp) cp.onclick = async () => {
      try { await navigator.clipboard.writeText(cp.dataset.hash); UI.toast('哈希已复制'); }
      catch (e) { UI.toast('复制失败', true); }
    };
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
      const v = prompt('新的截止时间（本地时间，最多 +24h）',
        toLocalInput(Date.now() + 24 * 3600 * 1000));
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
