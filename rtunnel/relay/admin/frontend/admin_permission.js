// 权限页：客户端/设备分组、设备上限、端口/TTY 字典、有效权限、旧 ACL 导入导出
// ---------------------------------------------------------------- Pages.权限
const DictManager = {
  async open(kind) {
    const state = { kind, q: '', sel: new Set(), list: [], editId: 0, editInitial: null };
    let tabs;
    const m = UI.modal({
      title: '字典管理',
      body: '<div id="dict-tabs"></div>' +
        '<div class="dict-toolbar"><input id="dict-q" placeholder="搜索">' +
        '<span class="spacer"></span><button id="dict-batch">+ 批量新增</button></div>' +
        '<div id="dict-body"></div>',
      buttons: '<button data-modal-close>关闭</button><button class="danger" id="dict-del">删除选中</button>',
      onClose: () => { if (tabs) tabs.destroy(); },
    });
    m.el.querySelector('.tty-modal').classList.add('dict-modal');
    const render = () => {
      const q = state.q.toLowerCase();
      const rows = state.list.filter(d => !q || d.value.toLowerCase().includes(q) || (d.name || '').toLowerCase().includes(q));
      m.el.querySelector('#dict-body').innerHTML = UI.dataTable({
        cls: 'dict-table',
        plain: true,
        head: '<th><input type="checkbox" id="dict-all"></th>' +
          '<th>' + (state.kind === 'ports' ? '端口/范围' : 'TTY 路径/模式') + '</th>' +
          '<th>名称</th><th>引用</th><th>状态</th><th>操作</th>',
        cols: Array(6).fill({}),
        rows: rows,
        empty: '暂无字典项',
        rowCls: d => state.editId === d.id ? 'is-editing' : '',
        cells: d => {
          const editing = state.editId === d.id;
          return [
            '<input type="checkbox" class="dict-pick" value="' + d.id + '"' +
              (state.sel.has(d.id) ? ' checked' : '') + '>',
            '<code>' + UI.esc(d.value) + '</code>',
            UI.esc(d.name || ''),
            String(d.refs || 0),
            d.enabled ? UI.pill('启用', 'ok') : UI.pill('停用', 'info'),
            '<button class="dict-edit" data-id="' + d.id + '">' + (editing ? '收起' : '编辑') + '</button> ' +
              '<button class="dict-del" data-id="' + d.id + '">删除</button>',
          ];
        },
        expand: d => state.editId === d.id ? DictManager.dictEditRow(state, d) : '',
      });
      m.el.querySelector('#dict-all').checked = rows.length > 0 && rows.every(d => state.sel.has(d.id));
      m.el.querySelector('#dict-all').onchange = e => {
        rows.forEach(d => e.target.checked ? state.sel.add(d.id) : state.sel.delete(d.id));
        render();
      };
      m.el.querySelectorAll('.dict-pick').forEach(c => c.onchange = () => {
        const id = Number(c.value); c.checked ? state.sel.add(id) : state.sel.delete(id);
        m.el.querySelector('#dict-del').disabled = state.sel.size === 0;
      });
      m.el.querySelectorAll('.dict-edit').forEach(b => b.onclick = async () => {
        const id = Number(b.dataset.id);
        if (state.editId === id) { await closeEdit(); return; }
        if (!await closeEdit()) return;
        state.editId = id;
        state.editInitial = null;
        render();
      });
      m.el.querySelectorAll('.dict-del').forEach(b => b.onclick = () => {
        const d = state.list.find(x => x.id === Number(b.dataset.id));
        if (d) DictManager.deleteDictItems(state, [d.id], load);
      });
      m.el.querySelector('#dict-del').disabled = state.sel.size === 0;

      const form = m.el.querySelector('.dict-edit-row');
      if (!form) return;
      state.editInitial = JSON.stringify(DictManager.dictValues(form));
      UI.bindFields(form);
      const nameInput = form.querySelector('#de-name');
      setTimeout(() => { if (nameInput) { nameInput.focus(); nameInput.select(); } }, 0);
      form.addEventListener('keydown', e => {
        if (e.key === 'Escape') { e.preventDefault(); closeEdit(); }
        else if (e.key === 'Enter' && e.target.tagName === 'INPUT') { e.preventDefault(); saveEdit(); }
      });
      form.querySelector('[data-act="dict-save"]').onclick = () => saveEdit();
      form.querySelector('[data-act="dict-cancel"]').onclick = () => closeEdit();
    };

    // 收起行内表单；有改动先确认，返回是否已收起
    const closeEdit = async (silent) => {
      const form = m.el.querySelector('.dict-edit-row');
      if (!form) { state.editId = 0; return true; }
      const dirty = JSON.stringify(DictManager.dictValues(form)) !== state.editInitial;
      if (!silent && dirty && !await UI.confirm({
        title: '放弃修改', message: '字典项还有未保存的修改，确定放弃？',
      })) return false;
      state.editId = 0;
      state.editInitial = null;
      render();
      return true;
    };

    const saveEdit = async () => {
      const form = m.el.querySelector('.dict-edit-row');
      if (!form) return;
      const v = DictManager.dictValues(form);
      const btn = form.querySelector('[data-act="dict-save"]');
      const hint = form.querySelector('#de-hint');
      if (btn) btn.disabled = true;
      try {
        await Api.put('/api/v1/permission/dict/' + state.kind + '/' + state.editId, {
          name: String(v.name || '').trim(),
          description: String(v.description || '').trim(),
          enabled: !!v.enabled,
        });
        state.editId = 0;
        state.editInitial = null;
        UI.toast('已保存');
        await load();
      } catch (e) {
        if (btn) btn.disabled = false;
        if (hint) { hint.textContent = '保存失败：' + e.message; hint.classList.add('warn'); }
      }
    };
    const load = async () => {
      try { state.list = await Api.get('/api/v1/permission/dict/' + state.kind); }
      catch (e) { UI.toast('加载失败：' + e.message, true); return; }
      state.sel.clear();
      if (state.editId && !state.list.some(d => d.id === state.editId)) state.editId = 0;
      render();
    };
    tabs = UI.tabs(m.el.querySelector('#dict-tabs'), {
      items: [{ key: 'ports', label: '端口字典' }, { key: 'ttys', label: 'TTY 字典' }],
      value: state.kind,
      onChange: key => { state.kind = key; state.sel.clear(); state.editId = 0; state.editInitial = null; load(); },
    });
    m.el.querySelector('#dict-q').oninput = e => { state.q = e.target.value.trim(); render(); };
    m.el.querySelector('#dict-batch').onclick = () => DictManager.openDictBatch(state, load);
    m.el.querySelector('#dict-del').onclick = () => DictManager.deleteDictItems(state, [...state.sel], load);
    await load();
  },
  async openDictBatch(state, onDone) {
    const isPort = state.kind === 'ports';
    const m = UI.modal({
      title: isPort ? '批量新增端口字典' : '批量新增 TTY 字典',
      body: '<label>' + (isPort ? '端口/范围，逗号或换行分隔' : 'TTY 路径/模式，逗号或换行分隔') +
        '<textarea id="db-values" rows="4" style="width:100%;font-family:monospace" placeholder="' +
        (isPort ? '22,80,443,8000-8100' : 'ttyS0,ttyS1,ttyUSB*,ttyACM*') + '"></textarea></label>' +
        '<label>名称前缀（可选）<input id="db-prefix" placeholder="例如 产线"></label>' +
        '<div class="dict-parse" id="db-parse"></div>',
      buttons: '<button data-modal-close>取消</button><button class="primary" id="db-ok">批量添加</button>',
    });
    const parse = () => {
      const raw = m.el.querySelector('#db-values').value;
      const seen = new Set();
      const items = raw.split(/[,，\s\n]+/).map(x => x.trim()).filter(Boolean).filter(x => {
        if (seen.has(x)) return false; seen.add(x); return true;
      });
      const bad = [];
      const preview = items.map(v => {
        let ok = true;
        if (isPort) {
          const mm = /^(\d+)(?:-(\d+))?$/.exec(v);
          if (!mm) ok = false;
          else {
            const a = Number(mm[1]), b = mm[2] ? Number(mm[2]) : a;
            if (a < 1 || b > 65535 || a > b) ok = false;
          }
        } else if (!/^[A-Za-z0-9_./?*-]+$/.test(v)) ok = false;
        if (!ok) bad.push(v);
        return '<span class="dict-token ' + (ok ? 'ok' : 'bad') + '">' + UI.esc(v) + (ok ? ' ✓' : ' ✗') + '</span>';
      }).join(' ');
      m.el.querySelector('#db-parse').innerHTML = preview || '<span class="muted">输入后显示解析结果</span>';
      m.el.querySelector('#db-ok').disabled = items.length === 0 || bad.length > 0;
      return items;
    };
    m.el.querySelector('#db-values').oninput = parse;
    m.el.querySelector('#db-values').focus();
    m.el.querySelector('#db-ok').onclick = async () => {
      const values = parse();
      try {
        const r = await Api.post('/api/v1/permission/dict/' + state.kind + '/batch', {
          values, name_prefix: m.el.querySelector('#db-prefix').value.trim(),
        });
        m.close();
        UI.toast('已添加 ' + (r.added || []).length + ' 项' +
          ((r.existing || []).length ? '，跳过已存在 ' + r.existing.length + ' 项' : ''));
        onDone();
      } catch (e) {
        UI.toast('添加失败：' + e.message, true);
      }
    };
    parse();
  },
  dictFields() {
    return [
      { key: 'name', label: '名称', edit: { type: 'text', id: 'de-name', maxlength: 64, placeholder: '可选' } },
      { key: 'description', label: '描述', edit: { type: 'text', id: 'de-desc', maxlength: 128, placeholder: '可选' } },
      { key: 'enabled', label: '状态', edit: { type: 'switch', id: 'de-enabled',
        onValue: true, offValue: false, onLabel: '启用', offLabel: '停用' } },
    ];
  },
  // 表格行下方的编辑表单：值只读，名称/描述/状态可改
  dictEditRow(state, d) {
    const fields = [{
      key: 'value', label: state.kind === 'ports' ? '端口/范围' : 'TTY',
      html: () => '<input class="ui-inline" value="' + UI.esc(d.value) + '" disabled>',
    }].concat(DictManager.dictFields());
    return '<tr class="ui-row-edit dict-edit-row"><td colspan="6"><div class="ui-row-edit-inner">' +
      UI.formGrid(fields, d) +
      '<div class="ui-row-edit-foot">' +
        '<span class="tok-hint" id="de-hint">停用后，已授权的端口/TTY 立即失效。</span>' +
        '<span class="spacer"></span>' +
        '<button data-act="dict-cancel">取消</button>' +
        '<button class="primary" data-act="dict-save">保存</button>' +
      '</div></div></td></tr>';
  },
  dictValues(root) {
    return UI.fieldValues(root, DictManager.dictFields());
  },
  async deleteDictItems(state, ids, onDone) {
    if (!ids.length) return;
    if (ids.length === 1) {
      const d = state.list.find(x => x.id === ids[0]);
      if (d && d.refs > 0) {
        if (await UI.confirm({ title: '无法删除', message: '该项被 ' + d.refs + ' 处权限引用，只能停用。', danger: true })) {
          try {
            await Api.put('/api/v1/permission/dict/' + state.kind + '/' + d.id, { name: d.name, description: d.description, enabled: false });
            onDone();
          } catch (e) { UI.toast('停用失败：' + e.message, true); }
        }
        return;
      }
    }
    if (!await UI.confirm({ title: '删除字典项', message: '删除选中的 ' + ids.length + ' 项？被引用项会自动跳过。', danger: true })) return;
    try {
      const r = await Api.post('/api/v1/permission/dict/' + state.kind + '/batch-delete', { ids });
      UI.toast('已删除 ' + (r.deleted || []).length + ' 项' +
        ((r.skipped || []).length ? '，跳过 ' + r.skipped.length + ' 项' : ''));
      onDone();
    } catch (e) { UI.toast('删除失败：' + e.message, true); }
  },
};

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
  capChips(v) {
    return String(v || '').split(',').map(x => x.trim()).filter(Boolean)
      .map(x => '<span class="perm-chip sm">' + UI.esc(x) + '</span>').join('');
  },
  aclRow(a, groups) {
    const opts = groups.map(g => '<option value="' + g.id + '"' +
      (Number(a.agent_group_id) === g.id ? ' selected' : '') + '>' + UI.esc(g.name) + '</option>').join('');
    return '<tr class="perm-acl-row">' +
      '<td><select class="pa-agent">' + opts + '</select></td>' +
      '<td><div class="perm-cap-cell"><span class="pa-ports-text">' + UI.esc(a.tcp_ports || '—') + '</span>' +
        '<button class="pa-ports-edit">编辑</button><input type="hidden" class="pa-ports" value="' + UI.esc(a.tcp_ports || '') + '"></div></td>' +
      '<td class="center"><input type="checkbox" class="pa-shell"' + (a.shell ? ' checked' : '') + '></td>' +
      '<td><div class="perm-cap-cell"><span class="pa-tty-text">' + UI.esc(a.tty || '—') + '</span>' +
        '<button class="pa-tty-edit">编辑</button><input type="hidden" class="pa-tty" value="' + UI.esc(a.tty || '') + '"></div></td>' +
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
      '<div class="ui-flex" style="margin-top:8px"><input id="pc-user" list="pc-user-list" placeholder="alice 或 alice,bob">' +
      '<datalist id="pc-user-list"></datalist>' +
      '<button id="pc-add-user">添加用户</button></div>' +
      '<h3>ACL 配置</h3>' +
      '<div class="perm-acl-wrap"><table class="detail-table perm-acl"><thead><tr>' +
      '<th>设备分组</th><th>TCP 端口</th><th>Shell</th><th>TTY</th><th>文件</th>' +
      '<th>SYSINFO</th><th>指标</th><th>配置</th><th>Web</th><th></th></tr></thead>' +
      '<tbody id="pc-acl">' + (acl || '') + '</tbody></table></div>' +
      '<div class="perm-acl-hint">左右滑动查看 ACL 全部列</div>' +
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
      '<div class="perm-field"><span>TCP 端口 <button id="pa-ports-edit">编辑</button></span>' +
        '<div class="perm-chips" id="pa-ports-view">' + this.capChips(c.tcp_ports) + '</div><input type="hidden" id="pa-ports" value="' + UI.esc(c.tcp_ports || '') + '"></div>' +
      '<div class="perm-field"><span>TTY <button id="pa-tty-edit">编辑</button></span>' +
        '<div class="perm-chips" id="pa-tty-view">' + this.capChips(c.tty) + '</div><input type="hidden" id="pa-tty" value="' + UI.esc(c.tty || '') + '"></div>' +
      '<label class="perm-field">文件<select id="pa-file">' + ['none','ro','rw'].map(v =>
        '<option value="' + v + '"' + ((c.file || 'none') === v ? ' selected' : '') + '>' + v + '</option>').join('') + '</select></label>' +
      '<h3>被客户端分组引用</h3><div class="perm-chips">' +
        ((d.referenced_by || []).length
          ? d.referenced_by.map(x => '<span class="perm-chip sm">' + UI.esc(x) + '</span>').join('')
          : '<span class="muted-sm">未被引用</span>') + '</div>' +
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
        '<td data-m="title"><span class="perm-chev">' + (open ? '▾' : '▸') + '</span> <b>' + UI.esc(g.name) + '</b></td>' +
        '<td data-m="pair">' + (g.user_count || 0) + '</td><td data-m="pair">' + (g.acl_count || 0) + '</td>' +
        '<td class="muted" data-m="full"><div class="perm-sum">' + UI.esc(this.capText(g.caps)) + '</div></td>' +
        '<td class="muted" data-m="pair">' + UI.ago(g.updated_ms) + '</td>' +
        '<td data-m="ops"><button class="danger perm-del" data-kind="client" data-id="' + g.id + '">删除</button></td></tr>' +
        (open ? '<tr class="perm-expand"><td colspan="6">' +
          this.clientForm(this.detail || g, ags) + '</td></tr>' : '');
    }).join('') : ags.map(g => {
      const open = this.expandKey === 'agent:' + g.id;
      return '<tr class="perm-row' + (open ? ' open' : '') + '" data-key="agent:' + g.id + '">' +
        '<td data-m="title"><span class="perm-chev">' + (open ? '▾' : '▸') + '</span> <b>' + UI.esc(g.name) + '</b></td>' +
        '<td data-m="pair">' + (g.device_count || 0) + '</td><td data-m="pair">' + (g.ref_count || 0) + '</td>' +
        '<td class="muted" data-m="full"><div class="perm-sum">' + UI.esc(this.capText(g.caps)) + '</div></td>' +
        '<td class="muted" data-m="pair">' + UI.ago(g.updated_ms) + '</td>' +
        '<td data-m="ops"><button class="danger perm-del" data-kind="agent" data-id="' + g.id + '">删除</button></td></tr>' +
        (open ? '<tr class="perm-expand"><td colspan="6">' +
          this.agentForm(this.detail || g) + '</td></tr>' : '');
    }).join('');
    let newForm = '';
    if (this.expandKey === 'client:new') newForm = '<tr class="perm-expand"><td colspan="6">' + this.clientForm({}, ags) + '</td></tr>';
    if (this.expandKey === 'agent:new') newForm = '<tr class="perm-expand"><td colspan="6">' + this.agentForm({}) + '</td></tr>';
    const head = isClient
      ? '<tr><th>名称</th><th>用户数</th><th>设备组</th><th>能力摘要</th><th>更新</th><th></th></tr>'
      : '<tr><th>名称</th><th>设备数</th><th>被引用</th><th>能力上限</th><th>更新</th><th></th></tr>';
    if (this._tabs) { this._tabs.destroy(); this._tabs = null; }
    this.el.innerHTML = UI.panel('权限管理',
      '<div id="perm-tabs"></div>' +
      '<div class="perm-table-wrap"><table class="detail-table perm-table"><thead>' + head + '</thead><tbody>' +
      (newForm || '') + (rows || '<tr><td colspan="6" class="empty">暂无分组</td></tr>') + '</tbody></table></div>',
      { right: '<button id="perm-effective">有效权限</button> ' +
        (canManage() ? '<button id="perm-legacy">旧 ACL</button> <button id="perm-dict">字典管理</button> ' +
          '<button class="primary" id="perm-new">+ 新建分组</button>' : '') });
    this._tabs = UI.tabs(document.getElementById('perm-tabs'), {
      items: [
        { key: 'client', label: '客户端分组', count: cgs.length },
        { key: 'agent', label: '设备分组', count: ags.length },
      ],
      value: this.tab,
      onChange: (key) => {
        this.tab = key; this.expandKey = null; this.detail = null; this.render();
      },
    });
    this.bind();
  },
  bind() {
    const db = document.getElementById('perm-dict');
    if (db) db.onclick = () => DictManager.open('ports');
    const eb = document.getElementById('perm-effective');
    if (eb) eb.onclick = () => this.openEffective();
    const lb = document.getElementById('perm-legacy');
    if (lb) lb.onclick = () => this.openLegacyAcl();
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
    this.bindCapEditors();
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
          d.querySelector('.perm-chip-x').onclick = () => d.remove();
          box.appendChild(d);
        });
        input.value = '';
      };
    };
    add('pc-users', 'pc-user', 'pc-add-user');
    // 用户候选来自「接入账号」，避免拼错用户名导致授权落空
    Api.get('/api/v1/client-accounts').then(list => {
      const dl = document.getElementById('pc-user-list');
      if (!dl || !Array.isArray(list)) return;
      dl.innerHTML = list.map(a => '<option value="' + UI.esc(a.username) + '"></option>').join('');
    }).catch(() => {});
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
    const esc = UI.esc;
    UI.transfer({
      title: '添加设备到「' + esc((this.detail && this.detail.name) || '当前分组') + '」',
      items: data.devices || [],
      key: d => d.devid,
      renderItem: d => '<span class="transfer-dev">' + esc(d.devid) + '</span>' +
        UI.pill(d.online ? '在线' : '登记', d.online ? 'ok' : 'info') +
        ((d.groups || []).length ? '<span class="muted-sm">' + esc(d.groups.join(',')) + '</span>' : ''),
      text: d => d.devid,
      filters: [{ id: 'online', label: '仅在线', test: d => d.online }],
      leftTitle: '可选设备', rightTitle: '已选设备',
      countText: n => '共 ' + n + ' 台',
      summary: (n, picked) => {
        const off = picked.filter(d => !d.online).length;
        return '已选 ' + n + ' 台' + (off ? '，其中 ' + off + ' 台离线' : '');
      },
      emptyLeft: '没有可添加的设备',
      emptyRight: '勾选设备后点 >',
      emptyMsg: '请先选择设备',
      okText: '确定添加',
      onOk: async (devids, close) => {
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
          close();
          return;
        }
        try {
          await Api.post('/api/v1/permission/agent-groups/' + id + '/devices', { devids });
          close(); UI.toast('已添加 ' + devids.length + ' 台设备');
          this.detail = null; this.refresh();
        } catch (e) { UI.toast('添加失败：' + e.message, true); }
      },
    });
  },
  bindCapEditors() {
    document.querySelectorAll('.pa-ports-edit').forEach(b => {
      b.onclick = (e) => {
        e.stopPropagation();
        const tr = b.closest('tr');
        this.openCapTransfer('ports', tr.querySelector('.pa-ports').value, v => {
          tr.querySelector('.pa-ports').value = v;
          tr.querySelector('.pa-ports-text').textContent = v || '—';
        });
      };
    });
    document.querySelectorAll('.pa-tty-edit').forEach(b => {
      b.onclick = (e) => {
        e.stopPropagation();
        const tr = b.closest('tr');
        this.openCapTransfer('ttys', tr.querySelector('.pa-tty').value, v => {
          tr.querySelector('.pa-tty').value = v;
          tr.querySelector('.pa-tty-text').textContent = v || '—';
        });
      };
    });
    const pe = document.getElementById('pa-ports-edit');
    if (pe) pe.onclick = () => {
      const input = document.getElementById('pa-ports');
      this.openCapTransfer('ports', input.value, v => {
        input.value = v;
        document.getElementById('pa-ports-view').innerHTML = this.capChips(v);
      });
    };
    const te = document.getElementById('pa-tty-edit');
    if (te) te.onclick = () => {
      const input = document.getElementById('pa-tty');
      this.openCapTransfer('ttys', input.value, v => {
        input.value = v;
        document.getElementById('pa-tty-view').innerHTML = this.capChips(v);
      });
    };
  },
  async openDictAdd(kind, onAdded) {
    const isPort = kind === 'ports';
    const m = UI.modal({
      title: isPort ? '新增端口字典' : '新增 TTY 字典',
      body: '<label>' + (isPort ? '端口/范围' : '路径/模式') +
        '<input id="dict-val" placeholder="' + (isPort ? '8080 或 8000-8100' : 'ttyUSB*') + '"></label>' +
        '<label>名称<input id="dict-name" placeholder="可选"></label>' +
        '<label>描述<input id="dict-desc" placeholder="可选"></label>',
      buttons: '<button data-modal-close>取消</button><button class="primary" id="dict-ok">保存并加入</button>',
    });
    m.el.querySelector('#dict-ok').onclick = async () => {
      const value = m.el.querySelector('#dict-val').value.trim();
      if (!value) { UI.toast('请输入值', true); return; }
      try {
        const name = m.el.querySelector('#dict-name').value.trim();
        const description = m.el.querySelector('#dict-desc').value.trim();
        const r = await Api.post('/api/v1/permission/dict/' + kind, { value, name, description });
        m.close();
        onAdded({ id: r.id, value, name: name || value, enabled: true });
      } catch (e) { UI.toast('保存失败：' + e.message, true); }
    };
    const val = m.el.querySelector('#dict-val');
    val.focus();
    val.onkeydown = e => { if (e.key === 'Enter') m.el.querySelector('#dict-ok').click(); };
  },
  async openCapTransfer(kind, current, onSave) {
    const isPort = kind === 'ports';
    let list;
    try { list = await Api.get('/api/v1/permission/dict/' + kind); }
    catch (e) { UI.toast('加载字典失败：' + e.message, true); return; }
    list = (list || []).slice();
    const values = String(current || '').split(',').map(x => x.trim()).filter(Boolean);
    for (const v of values) {
      if (list.some(x => x.value === v)) continue;
      try {
        const r = await Api.post('/api/v1/permission/dict/' + kind, { value: v, name: v });
        list.push({ id: r.id, value: v, name: v, enabled: true });
      } catch (e) { /* 保留为未入库，仍可展示与回填 */ }
    }
    UI.transfer({
      title: isPort ? '选择 TCP 端口' : '选择 TTY',
      items: list,
      selected: values,
      key: d => d.value,
      renderItem: d => '<span class="transfer-dev">' + UI.esc(d.value) + '</span>' +
        '<span class="muted-sm">' + UI.esc(d.name || '') + '</span>',
      renderMissing: v => '<span class="transfer-dev">' + UI.esc(v) + '</span>' +
        '<span class="muted-sm">未入库</span>',
      text: d => d.value + ' ' + (d.name || ''),
      filters: [{ id: 'enabled', label: '仅启用', checked: true, test: d => d.enabled !== false }],
      leftTitle: isPort ? '备选端口' : '备选 TTY',
      rightTitle: '已选',
      countText: () => '字典 ' + list.length + ' 项',
      summary: n => '已选 ' + n + ' 项',
      emptyLeft: '没有可用字典项',
      emptyRight: '从左侧选择',
      allowEmpty: true,
      addon: {
        label: '+ 新增字典项',
        onClick: add => this.openDictAdd(kind, d => { list.push(d); add(d); }),
      },
      onOk: (picked, close) => { close(); onSave(picked.join(',')); },
    });
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
      this.bindCapEditors();
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
  async openEffective() {
    const m = UI.modal({
      title: '有效权限预览',
      body: '<div class="ui-flex"><input id="pe-client" placeholder="客户端用户名，例如 alice">' +
        '<button class="primary" id="pe-check">查询</button></div>' +
        '<div id="pe-body" class="pe-body"><div class="empty">输入用户名后查询</div></div>',
      buttons: '<button data-modal-close>关闭</button>',
    });
    m.el.querySelector('.tty-modal').classList.add('effective-modal');
    const input = m.el.querySelector('#pe-client');
    const body = m.el.querySelector('#pe-body');
    const run = async () => {
      const u = input.value.trim();
      if (!u) { input.focus(); return; }
      body.innerHTML = '<div class="empty">查询中…</div>';
      let d;
      try { d = await Api.get('/api/v1/permission/effective?client=' + encodeURIComponent(u)); }
      catch (e) { body.innerHTML = '<div class="empty">查询失败：' + UI.esc(e.message) + '</div>'; return; }
      const chip = x => '<span class="perm-chip sm">' + UI.esc(x) + '</span>';
      const groups = (d.groups || []).map(chip).join('');
      const devs = (d.devices || []).map(chip).join('');
      const caps = this.capText(d.caps);
      const capHtml = caps === '无' ? '<span class="muted-sm">无</span>' : caps.split(' · ').map(chip).join('');
      body.innerHTML =
        '<h3>所属客户端分组</h3><div class="perm-chips">' +
          (groups || '<span class="muted-sm">未加入客户端分组，按旧 ACL / allow-all 策略</span>') + '</div>' +
        '<h3>可访问设备 · ' + (d.devices || []).length + '</h3><div class="perm-chips pe-scroll">' +
          (devs || '<span class="muted-sm">无</span>') + '</div>' +
        '<h3>有效能力</h3><div class="perm-chips">' + capHtml + '</div>' +
        '<p class="muted-sm pe-note">已按设备分组能力上限和字典停用过滤；设备侧还会按启动参数再校验。</p>';
    };
    m.el.querySelector('#pe-check').onclick = run;
    input.onkeydown = e => { if (e.key === 'Enter') run(); };
    input.focus();
  },
  async openLegacyAcl() {
    const isAdmin = roleOf() === 'admin';
    let d = { content: '', path: '', cli: [] };
    try { d = await Api.get('/api/v1/acl/file'); }
    catch (e) { /* 未配置 ACL 文件时仍可导出 */ }
    const cli = (d.cli || []).join('\n');
    let importText = d.content || '';
    if (cli) importText += (importText && importText[importText.length - 1] !== '\n' ? '\n' : '') + cli;
    let tabs;
    const m = UI.modal({
      title: '旧 ACL（' + (d.path || '未配置文件') + '）',
      body: '<div id="legacy-tabs"></div>' +
        '<div id="legacy-import">' +
        '<p class="muted-sm ui-mt8">每条 <code>user=devid[,devid|*]</code> 生成一个客户端分组和一个设备分组，' +
        '能力默认全开；同名分组会跳过。</p>' +
        '<textarea id="ia-content" rows="11" class="ui-textarea ui-mt8" placeholder="alice=dev1,dev2">' +
        UI.esc(importText) + '</textarea>' +
        '<div id="ia-result" class="muted-sm ui-mt8"></div></div>' +
        '<div id="legacy-export" hidden>' +
        '<p class="muted-sm ui-mt8">从当前分组生成旧 ACL 文本（只读）。只有能力全开的 ACL 能导出，' +
        '受限条目会在文末注释里列出。</p>' +
        '<textarea id="ea-content" rows="11" class="ui-textarea ui-mt8" readonly ' +
        'placeholder="点「生成导出」"></textarea>' +
        '<div id="ea-result" class="muted-sm ui-mt8"></div></div>',
      buttons: '<button data-modal-close>关闭</button>' +
        (isAdmin ? '<button class="primary" id="ia-ok">导入</button>' : '') +
        '<button id="ea-run">生成导出</button><button id="ea-copy">复制</button>',
      onClose: () => { if (tabs) tabs.destroy(); },
    });
    m.el.querySelector('.tty-modal').classList.add('acl-modal');
    const imp = m.el.querySelector('#ia-content');
    const exp = m.el.querySelector('#ea-content');
    const iaRes = m.el.querySelector('#ia-result');
    const eaRes = m.el.querySelector('#ea-result');
    const iaBtn = m.el.querySelector('#ia-ok');
    const eaRun = m.el.querySelector('#ea-run');
    const eaCopy = m.el.querySelector('#ea-copy');
    const show = (key) => {
      m.el.querySelector('#legacy-import').hidden = key !== 'import';
      m.el.querySelector('#legacy-export').hidden = key !== 'export';
      if (iaBtn) iaBtn.hidden = key !== 'import';
      eaRun.hidden = key !== 'export';
      eaCopy.hidden = key !== 'export';
    };
    const items = [];
    if (isAdmin) items.push({ key: 'import', label: '导入' });
    items.push({ key: 'export', label: '导出' });
    tabs = UI.tabs(m.el.querySelector('#legacy-tabs'), {
      items, value: items[0].key, onChange: show,
    });
    show(items[0].key);
    if (iaBtn) {
      iaBtn.onclick = async () => {
        iaRes.textContent = '导入中…';
        try {
          const r = await Api.post('/api/v1/permission/import-acl', { content: imp.value });
          const skipped = (r.skipped || []).map(x => x.user + '（' + x.reason + '）').join('、');
          iaRes.textContent = '已导入 ' + r.imported + ' 个客户端分组，关联 ' + r.devices + ' 台设备' +
            (skipped ? '；跳过：' + skipped : '');
          if (r.imported) setTimeout(() => { m.close(); this.refresh(); }, 600);
        } catch (e) { iaRes.textContent = '导入失败：' + e.message; }
      };
    }
    eaRun.onclick = async () => {
      eaRes.textContent = '生成中…';
      try {
        const r = await Api.get('/api/v1/permission/export-acl');
        exp.value = r.content || '';
        eaRes.textContent = '已导出 ' + r.clients + ' 个客户端、' + r.devices + ' 条授权' +
          (r.restricted ? '；跳过 ' + r.restricted + ' 条受限 ACL' : '');
      } catch (e) { eaRes.textContent = '导出失败：' + e.message; }
    };
    eaCopy.onclick = async () => {
      if (!exp.value) { eaRes.textContent = '先点「生成导出」'; return; }
      if (await UI.copy(exp.value, '已复制到剪贴板')) eaRes.textContent = '已复制到剪贴板';
    };
  },
  enter(el) { this.el = el; return Page.prototype.enter.call(this, el, []); },
});

Router.register(/^#\/permission$/, PermissionPage, '权限');
