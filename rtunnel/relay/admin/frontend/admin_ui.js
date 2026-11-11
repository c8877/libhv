'use strict';
/* 统一渲染组件：表格 / KV / 表单 / 面板 / 格式化 */
const UI = {
  esc(s) {
    return String(s == null ? '' : s).replace(/[&<>"']/g,
      c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
  },
  pill(text, kind) {
    return '<span class="pill' + (kind ? ' ' + kind : '') + '">' + UI.esc(text) + '</span>';
  },
  // map: { value: [label, kind], default: [label, kind] }
  status(value, map, attrs) {
    const m = (map && map[value]) || (map && map.default) || [value, ''];
    return '<span class="pill' + (m[1] ? ' ' + m[1] : '') + '"' + (attrs || '') + '>' +
      UI.esc(m[0]) + '</span>';
  },
  time(ms) {
    if (ms == null || ms === 0 || ms === '') return '—';
    const p = (typeof Auth !== 'undefined' && Auth.profile) || {};
    return p.time_format === 'relative' ? UI.ago(ms) : new Date(ms).toLocaleString();
  },
  download(name, data, mime) {
    const blob = data instanceof Blob ? data
      : new Blob([data], { type: mime || 'application/octet-stream' });
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = url;
    a.download = name;
    a.click();
    setTimeout(() => URL.revokeObjectURL(url), 0);
  },
  async copy(text, okMsg) {
    try {
      if (navigator.clipboard && window.isSecureContext) {
        await navigator.clipboard.writeText(text);
      } else {
        const ta = document.createElement('textarea');
        ta.value = text;
        ta.style.position = 'fixed';
        ta.style.opacity = '0';
        document.body.appendChild(ta);
        ta.select();
        const ok = document.execCommand('copy');
        ta.remove();
        if (!ok) throw new Error('copy failed');
      }
      UI.toast(okMsg || '已复制');
      return true;
    } catch (e) {
      UI.toast('复制失败', true);
      return false;
    }
  },
  renderUser() {
    const me = (typeof Auth !== 'undefined' && Auth.me) || {};
    const name = me.display_name || me.username || '?';
    const color = me.avatar_color || avatarColor(me.username);
    const btn = document.getElementById('userbtn');
    if (btn) {
      btn.innerHTML = '<span class="avatar" style="background:' + color + '">' +
        UI.esc(String(name).slice(0, 1).toUpperCase()) + '</span><span class="uname">' +
        UI.esc(name) + '</span><span class="caret">▾</span>';
    }
    const info = document.getElementById('userinfo');
    if (info) info.textContent = name + ' · ' + (ROLE_NAMES[me.role] || me.role || '');
  },
  // 统一 Tab 组件（attached 样式）
  // opts: { items:[{key,label,count,disabled}], value, onChange(key) }
  // 返回: { el, getValue, setValue, setCount, setItems, destroy }
  tabs(container, opts) {
    if (!container) return null;
    opts = opts || {};
    let items = (opts.items || []).slice();
    let value = opts.value != null ? opts.value : (items[0] && items[0].key);
    const onChange = typeof opts.onChange === 'function' ? opts.onChange : function () {};
    container.classList.add('ui-tabs', 'ui-tabs-attached');
    container.setAttribute('role', 'tablist');

    const activate = (key, fire) => {
      if (!items.some(x => x.key === key)) return;
      value = key;
      container.querySelectorAll('.ui-tab').forEach(b => {
        const on = b.dataset.key === key;
        b.classList.toggle('is-active', on);
        b.setAttribute('aria-selected', on ? 'true' : 'false');
        b.tabIndex = on ? 0 : -1;
      });
      if (fire) onChange(key);
    };
    const bind = () => {
      container.querySelectorAll('.ui-tab').forEach(b => {
        b.onclick = () => activate(b.dataset.key, true);
        b.onkeydown = (e) => {
          const idx = items.findIndex(x => x.key === b.dataset.key);
          let next = -1;
          if (e.key === 'ArrowRight') next = (idx + 1) % items.length;
          else if (e.key === 'ArrowLeft') next = (idx - 1 + items.length) % items.length;
          else if (e.key === 'Home') next = 0;
          else if (e.key === 'End') next = items.length - 1;
          if (next >= 0) {
            e.preventDefault();
            activate(items[next].key, true);
            const el = container.querySelector('.ui-tab[data-key="' + items[next].key + '"]');
            if (el) el.focus();
          }
        };
      });
    };
    const render = () => {
      container.innerHTML = items.map(x =>
        '<button class="ui-tab' + (x.key === value ? ' is-active' : '') + '" role="tab" ' +
        'data-key="' + UI.esc(x.key) + '" aria-selected="' + (x.key === value ? 'true' : 'false') + '" ' +
        'tabindex="' + (x.key === value ? '0' : '-1') + '"' + (x.disabled ? ' disabled' : '') + '>' +
        '<span class="ui-tab-label">' + UI.esc(x.label || x.key) + '</span>' +
        (x.count != null ? '<span class="ui-tab-count">' + Number(x.count) + '</span>' : '') +
        '</button>').join('');
      bind();
    };
    render();
    return {
      el: container,
      getValue: () => value,
      setValue: (key, fire) => activate(key, fire !== false),
      setCount(key, count) {
        const it = items.find(x => x.key === key);
        if (!it) return;
        it.count = count;
        const b = container.querySelector('.ui-tab[data-key="' + key + '"] .ui-tab-count');
        if (b) b.textContent = Number(count);
        else render();
      },
      setItems(next) {
        items = (next || []).slice();
        if (!items.some(x => x.key === value)) value = items[0] && items[0].key;
        render();
      },
      destroy() {
        container.innerHTML = '';
      },
    };
  },
  // 左右穿梭选择器。opts:
  // { title, items, selected:[key], key(x), renderItem(x), renderMissing(key), text(x),
  //   filters:[{id,label,checked,test(x)}], leftTitle, rightTitle,
  //   countText(leftN, selN, total), summary(selN, picked), emptyLeft, emptyRight,
  //   addon:{label,onClick(addItem)}, allowEmpty, emptyMsg, okText, onOk(pickedKeys, close) }
  transfer(opts) {
    opts = opts || {};
    const items = (opts.items || []).slice();
    const keyOf = opts.key || (x => x);
    const renderItem = opts.renderItem || (x => UI.esc(String(x)));
    const renderMissing = opts.renderMissing || (k => UI.esc(String(k)));
    const textOf = opts.text || (x => String(x));
    const filters = opts.filters || [];
    const on = {};
    filters.forEach(f => { on[f.id] = f.checked === true; });
    const selected = new Map();
    (opts.selected || []).forEach(k => {
      const it = items.find(x => keyOf(x) === k);
      selected.set(k, it || { __missing: k });
    });
    const m = UI.modal({
      title: opts.title || '选择',
      body: '<div class="transfer-toolbar"><input id="tp-q" placeholder="' +
        UI.esc(opts.searchPlaceholder || '搜索') + '">' +
        filters.map(f => '<label><input type="checkbox" id="tp-f-' + UI.esc(f.id) + '"' +
          (on[f.id] ? ' checked' : '') + '> ' + UI.esc(f.label) + '</label>').join('') +
        '<span id="tp-count"></span><span class="spacer"></span>' +
        (opts.addon ? '<button id="tp-addon">' + UI.esc(opts.addon.label) + '</button>' : '') + '</div>' +
        '<div class="transfer"><div class="transfer-list"><h4 id="tp-left-title"></h4><div id="tp-left"></div></div>' +
        '<div class="transfer-ops"><button id="tp-add">&gt;</button><button id="tp-addall">&gt;&gt;</button>' +
        '<button id="tp-rem">&lt;</button><button id="tp-clear">&lt;&lt;</button></div>' +
        '<div class="transfer-list"><h4 id="tp-right-title"></h4><div id="tp-right"></div></div></div>' +
        '<div class="transfer-foot"><span id="tp-summary"></span></div>',
      buttons: '<button data-modal-close>取消</button><button class="primary" id="tp-ok">' +
        UI.esc(opts.okText || '确定') + '</button>',
    });
    const modalEl = m.el.querySelector('.tty-modal');
    if (modalEl) modalEl.classList.add('transfer-modal');
    const $ = s => m.el.querySelector(s);
    const row = (it, cls) => {
      const k = it.__missing != null ? it.__missing : keyOf(it);
      return '<label class="transfer-item"><input type="checkbox" class="' + cls +
        '" value="' + UI.esc(k) + '">' +
        (it.__missing != null ? renderMissing(k) : renderItem(it)) + '</label>';
    };
    const leftItems = () => {
      const q = $('#tp-q').value.trim().toLowerCase();
      return items.filter(x => {
        if (selected.has(keyOf(x))) return false;
        if (q && textOf(x).toLowerCase().indexOf(q) < 0) return false;
        return filters.every(f => !on[f.id] || f.test(x));
      });
    };
    const draw = () => {
      const left = leftItems();
      const right = [...selected.values()];
      $('#tp-left').innerHTML = left.length ? left.map(x => row(x, 'tp-pick')).join('')
        : '<div class="empty">' + UI.esc(opts.emptyLeft || '没有可添加的项') + '</div>';
      $('#tp-right').innerHTML = right.length ? right.map(x => row(x, 'tp-rpick')).join('')
        : '<div class="empty">' + UI.esc(opts.emptyRight || '从左侧勾选后点 >') + '</div>';
      $('#tp-count').textContent = opts.countText ? opts.countText(left.length, right.length, items.length)
        : '共 ' + left.length + ' 项';
      $('#tp-left-title').textContent = (opts.leftTitle || '备选') + ' · ' + left.length;
      $('#tp-right-title').textContent = (opts.rightTitle || '已选') + ' · ' + right.length;
      $('#tp-summary').textContent = opts.summary ? opts.summary(right.length, right)
        : '已选 ' + right.length + ' 项';
      $('#tp-add').disabled = left.length === 0;
      $('#tp-addall').disabled = left.length === 0;
      $('#tp-rem').disabled = right.length === 0;
      $('#tp-clear').disabled = right.length === 0;
    };
    const picked = cls => [...m.el.querySelectorAll('.' + cls + ':checked')].map(x => x.value);
    const moveRight = values => {
      const set = new Set(values);
      items.filter(x => set.has(keyOf(x)) && !selected.has(keyOf(x)))
        .forEach(x => selected.set(keyOf(x), x));
      draw();
    };
    const moveLeft = values => { values.forEach(k => selected.delete(k)); draw(); };
    const addItem = it => {
      const k = keyOf(it);
      if (selected.has(k)) return;
      if (!items.some(x => keyOf(x) === k)) items.push(it);
      selected.set(k, it);
      draw();
    };
    $('#tp-add').onclick = () => moveRight(picked('tp-pick'));
    $('#tp-addall').onclick = () => moveRight(leftItems().map(keyOf));
    $('#tp-rem').onclick = () => moveLeft(picked('tp-rpick'));
    $('#tp-clear').onclick = () => { selected.clear(); draw(); };
    $('#tp-q').oninput = draw;
    filters.forEach(f => {
      const el = $('#tp-f-' + f.id);
      if (el) el.onchange = () => { on[f.id] = el.checked; draw(); };
    });
    if (opts.addon) $('#tp-addon').onclick = () => opts.addon.onClick(addItem);
    $('#tp-left').onclick = e => {
      const it = e.target.closest('.transfer-item');
      if (it && e.detail === 2) moveRight([it.querySelector('input').value]);
    };
    $('#tp-right').onclick = e => {
      const it = e.target.closest('.transfer-item');
      if (it && e.detail === 2) moveLeft([it.querySelector('input').value]);
    };
    $('#tp-ok').onclick = () => {
      if (!selected.size && !opts.allowEmpty) {
        UI.toast(opts.emptyMsg || '请先选择', true);
        return;
      }
      const pickedKeys = [...selected.keys()];
      if (opts.onOk) opts.onOk(pickedKeys, m.close);
      else m.close();
    };
    draw();
    return m;
  },
  // 列表行动作：模板写 data-act，页面只注册一次；冒泡到此统一拦截
  actions(root, map) {
    if (!root || !map) return;
    if (!root.__uiActions) {
      root.__uiActions = {};
      root.addEventListener('click', e => {
        const el = e.target.closest('[data-act]');
        if (!el || !root.contains(el)) return;
        const fn = root.__uiActions[el.dataset.act];
        if (!fn) return;
        e.stopPropagation();
        fn(el, e);
      }, true);
    }
    root.__uiActions = map;
  },
  // datetime-local 输入框的值（本地时间，分钟精度）
  localInput(ms) {
    if (!ms) return '';
    const d = new Date(ms);
    const pad = n => String(n).padStart(2, '0');
    return d.getFullYear() + '-' + pad(d.getMonth() + 1) + '-' + pad(d.getDate()) +
      'T' + pad(d.getHours()) + ':' + pad(d.getMinutes());
  },

  // 字节 → GB；0 表示不限
  gb(bytes) {
    const n = Number(bytes) || 0;
    if (!n) return '不限';
    const gb = n / (1024 * 1024 * 1024);
    return (gb >= 10 ? Math.round(gb) : gb.toFixed(1)) + ' GB';
  },

  // 字节 → GB 数值，供编辑框回填
  gbValue(bytes) {
    const n = Number(bytes) || 0;
    if (!n) return 0;
    return Math.round(n / (1024 * 1024 * 1024) * 10) / 10;
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
  panel(title, body, opts) {
    opts = opts || {};
    return '<div class="panel ' + (opts.cls || '') + '">' +
      '<div class="panel-head"><h2>' + title + '</h2>' +
      (opts.left || '') + '<span class="spacer"></span>' + (opts.right || '') +
      '</div>' + body + '</div>';
  },
  grid(rows) {
    return '<div class="sys-grid">' + rows.map(r =>
      '<div class="sys-item"><span class="k">' + r[0] + '</span>' +
      '<span class="v" title="' + r[1] + '">' + r[1] + '</span></div>').join('') + '</div>';
  },
  listPage(opts) {
    opts = opts || {};
    const search = opts.search
      ? '<input id="' + UI.esc(opts.search.id || '') + '" class="ui-search" placeholder="' +
        UI.esc(opts.search.placeholder || '搜索') + '">'
      : '';
    return UI.panel(opts.title, UI.detailTable(opts.columns || [], opts.rows || [], opts.tableOpts),
      { cls: opts.cls || '', left: opts.summary ? '<span class="muted ml10">' +
          opts.summary + '</span>' : '', right: search + (opts.actions || '') });
  },
  bindSearch(input, scope) {
    if (!input) return;
    input.oninput = () => {
      const q = input.value.trim().toLowerCase();
      const rows = (scope || document).querySelectorAll('tbody tr');
      rows.forEach(tr => {
        tr.style.display = !q || tr.innerText.toLowerCase().indexOf(q) >= 0 ? '' : 'none';
      });
    };
  },
  pager(page, pages, total, right) {
    return '<div class="ui-pager">' +
      '<button data-pager="prev"' + (page <= 1 ? ' disabled' : '') + '>上一页</button>' +
      '<span>' + page + ' / ' + pages + (total != null ? '（共 ' + total + '）' : '') + '</span>' +
      '<button data-pager="next"' + (page >= pages ? ' disabled' : '') + '>下一页</button>' +
      '<span class="spacer"></span>' + (right || '') + '</div>';
  },
  bindPager(root, onPrev, onNext) {
    const p = root.querySelector('[data-pager="prev"]');
    const n = root.querySelector('[data-pager="next"]');
    if (p) p.onclick = onPrev;
    if (n) n.onclick = onNext;
  },
  modal(opts) {
    opts = opts || {};
    const mask = document.createElement('div');
    mask.className = 'tty-mask';
    mask.innerHTML = '<div class="tty-modal"><h2>' + (opts.title || '') + '</h2>' + (opts.body || '') +
      '<div class="tty-btns">' + (opts.buttons || '') + '</div></div>';
    document.body.appendChild(mask);
    let closed = false;
    const close = () => {
      if (closed) return;
      closed = true;
      mask.remove();
      if (opts.onClose) opts.onClose();
    };
    mask.querySelectorAll('[data-modal-close]').forEach(b => { b.onclick = close; });
    mask.onclick = e => { if (e.target === mask) close(); };
    return { el: mask, close };
  },
  confirm(opts) {
    opts = opts || {};
    return new Promise(resolve => {
      let done = false;
      const m = UI.modal({
        title: opts.title || '确认',
        body: '<div class="ui-confirm-text">' + (opts.message || '') + '</div>',
        buttons: '<button data-modal-close>取消</button>' +
          '<button class="' + (opts.danger ? 'danger' : 'primary') + '" data-ok>确认</button>',
        onClose: () => { if (!done) { done = true; resolve(false); } },
      });
      const ok = m.el.querySelector('[data-ok]');
      if (ok) ok.onclick = () => { done = true; m.close(); resolve(true); };
    });
  },
  prompt(opts) {
    opts = opts || {};
    return new Promise(resolve => {
      let done = false;
      const m = UI.modal({
        title: opts.title || '输入',
        body: '<label>' + (opts.label || '值') + '<input id="ui-prompt-input" type="' +
          (opts.type || 'text') + '" value="' + UI.esc(opts.value || '') + '" placeholder="' +
          UI.esc(opts.placeholder || '') + '"></label>',
        buttons: '<button data-modal-close>取消</button><button class="primary" data-ok>确定</button>',
        onClose: () => { if (!done) { done = true; resolve(null); } },
      });
      const input = m.el.querySelector('#ui-prompt-input');
      const ok = m.el.querySelector('[data-ok]');
      const submit = () => { done = true; const v = input.value; m.close(); resolve(v); };
      if (ok) ok.onclick = submit;
      if (input) {
        input.focus();
        input.onkeydown = e => { if (e.key === 'Enter') submit(); };
      }
    });
  },
  formValues(root) {
    const out = {};
    (root || document).querySelectorAll('[data-field]').forEach(el => {
      out[el.dataset.field] = el.type === 'checkbox' ? el.checked : el.value;
    });
    return out;
  },
  detailTable(cols, rows, opts) {
    opts = opts || {};
    const th = c => '<th>' + (String(c).charAt(0) === '<' ? c : UI.esc(c)) + '</th>';
    let h = '<div class="detail-table-shell ' + (opts.shellCls || '') + '"><table class="detail-table ' + (opts.cls || '') + '"><thead><tr>' +
      cols.map(th).join('') + '</tr></thead><tbody>';
    if (!rows.length) h += '<tr><td class="empty" colspan="' + cols.length + '">' + UI.esc(opts.empty || '暂无数据') + '</td></tr>';
    rows.forEach((r, i) => {
      // 允许直接插入整行，例如表格行下方的展开表单（管理台账号页在用）
      if (typeof r === 'string') { h += r; return; }
      h += '<tr' + (opts.rowAttrs ? opts.rowAttrs(i) : '') + '>' + r.map(c => '<td>' + c + '</td>').join('') + '</tr>';
    });
    return h + '</tbody></table></div>';
  },
  kvTable(rows, opts) {
    opts = opts || {};
    const head = opts.header ? '<thead><tr>' + opts.header.map(h => '<th>' + h + '</th>').join('') + '</tr></thead>' : '';
    return '<div class="detail-table-shell ' + (opts.shellCls || '') + '">' +
      '<table class="detail-table detail-kv ' + (opts.cls || '') + '">' + head + '<tbody>' +
      rows.map(r => '<tr><th>' + r[0] + '</th><td>' + r[1] + '</td></tr>').join('') +
      '</tbody></table></div>';
  },
  form(opts) {
    opts = opts || {};
    const rows = opts.rows || [];
    let body;
    const cell = r => r.raw ? r.raw : '<label>' + r.label + r.control + '</label>';
    if (opts.layout === 'grid') {
      body = '<div class="ui-form-grid ' + (opts.cols ? 'cols-' + opts.cols : '') + '">' +
        rows.map(cell).join('') + '</div>';
    } else {
      body = '<div class="form-rows">' + rows.map(r =>
        '<label class="form-row"><span class="k">' + r.label + '</span>' +
        '<span class="c">' + r.control + '</span><span class="u">' + (r.unit || '') + '</span></label>').join('') + '</div>';
    }
    return '<div class="ui-form ' + (opts.cls || '') + '"' + (opts.id ? ' id="' + UI.esc(opts.id) + '"' : '') +
      (opts.attrs ? ' ' + opts.attrs : '') + '>' + body +
      '<div class="ui-form-foot">' + (opts.left || '') +
      '<span class="spacer"></span>' + (opts.right || '') + '</div></div>';
  },
  // 列声明表格：单元格由 render(row) 或 cells(row) 生成，适合状态徽标、按钮组等复杂列。
  // opts: cols/rows/empty/cls/shellCls/plain/rowCls/rowAttrs/tdAttrs/cells
  //       head 可整段替换表头（全选框等），expand 在数据行后插入展开表单行。
  dataTable(opts) {
    opts = opts || {};
    const cols = opts.cols || [], rows = opts.rows || [];
    const head = opts.head || cols.map(c => '<th' + (c.thCls ? ' class="' + c.thCls + '"' : '') +
      (c.thAttrs ? ' ' + c.thAttrs : '') + '>' + c.label + '</th>').join('');
    let h = (opts.plain ? '' : '<div class="detail-table-shell ' + (opts.shellCls || '') + '">') +
      '<table class="detail-table ' + (opts.cls || '') + '"><thead><tr>' + head + '</tr></thead><tbody>';
    if (!rows.length) h += '<tr><td class="empty" colspan="' + cols.length + '">' + UI.esc(opts.empty || '暂无数据') + '</td></tr>';
    rows.forEach(r => {
      const rc = opts.rowCls ? opts.rowCls(r) : '';
      const ra = opts.rowAttrs ? opts.rowAttrs(r) : '';
      const cells = opts.cells ? opts.cells(r) : cols.map(c => c.render ? c.render(r) : UI.esc(r[c.key]));
      h += '<tr' + (rc ? ' class="' + rc + '"' : '') + (ra ? ' ' + ra : '') + '>' +
        cells.map((cell, i) => '<td' + (cols[i] && cols[i].cls ? ' class="' + cols[i].cls + '"' : '') +
          (opts.tdAttrs && cols[i] ? opts.tdAttrs(cols[i], r) : '') + '>' + cell + '</td>').join('') + '</tr>';
      if (opts.expand) h += opts.expand(r);
    });
    h += '</tbody></table>';
    return opts.plain ? h : h + '</div>';
  },
  // 单条记录的表格：同一张表在只读与编辑表单之间原地切换。
  // host 承载 <table>，外层 .detail-table-shell 由页面提供。
  // opts: {
  //   data, columns=2,
  //   fields: [{ key, label, span=1, cls, view(data), edit:{type,id,options,value(data),
  //              html(value,data),placeholder,maxlength,rows,step,min,max,
  //              onValue,offValue,onLabel,offLabel} }],
  //   cls, actions, renderActions(editing, data),
  //   hint(values, data) -> string | {text,warn},
  //   confirmSave(values, data) -> Promise<bool>|bool,
  //   save(values, data) -> Promise,
  //   onSaved(values, data), onStateChange(editing, data), dirtyMessage, focus
  //   editing（初始态）
  // }
  // 返回: { el, host, start, save, cancel, leave, update, isEditing, isDirty, values, destroy }
  // 渲染单个字段的编辑控件，editTable 与行内展开表单共用
  fieldControl(field, value, data) {
    const e = field.edit || {};
    const v = value;
    const attrs = ' class="ui-inline" data-ef="' + UI.esc(field.key) + '"' +
      (e.id ? ' id="' + UI.esc(e.id) + '"' : '');
    if (e.type === 'custom') return e.html ? e.html(v, data) : '';
    if (e.type === 'switch') {
      const on = String(v) === String(e.onValue != null ? e.onValue : '1');
      const onLabel = e.onLabel != null ? e.onLabel : '开';
      const offLabel = e.offLabel != null ? e.offLabel : '关';
      return '<label class="ui-switch"' + (e.id ? ' id="' + UI.esc(e.id) + '"' : '') + '>' +
        '<input type="checkbox" data-ef="' + UI.esc(field.key) + '"' + (on ? ' checked' : '') + '>' +
        '<span class="ui-switch-track"><span class="ui-switch-dot"></span></span>' +
        '<span class="ui-switch-text" data-on="' + UI.esc(onLabel) + '" data-off="' + UI.esc(offLabel) + '">' +
        UI.esc(on ? onLabel : offLabel) + '</span></label>';
    }
    if (e.type === 'select') {
      return '<select' + attrs + '>' + (e.options || []).map(op =>
        '<option value="' + UI.esc(op.value) + '"' +
        (String(op.value) === String(v == null ? '' : v) ? ' selected' : '') + '>' +
        UI.esc(op.label != null ? op.label : op.value) + '</option>').join('') + '</select>';
    }
    if (e.type === 'textarea') {
      return '<textarea' + attrs + ' rows="' + (e.rows || 2) + '"' +
        (e.placeholder ? ' placeholder="' + UI.esc(e.placeholder) + '"' : '') +
        (e.maxlength ? ' maxlength="' + e.maxlength + '"' : '') + '>' +
        UI.esc(v == null ? '' : v) + '</textarea>';
    }
    return '<input type="' + UI.esc(e.type || 'text') + '"' + attrs +
      ' value="' + UI.esc(v == null ? '' : v) + '"' +
      (e.placeholder ? ' placeholder="' + UI.esc(e.placeholder) + '"' : '') +
      (e.maxlength ? ' maxlength="' + e.maxlength + '"' : '') +
      (e.step != null ? ' step="' + e.step + '"' : '') +
      (e.min != null ? ' min="' + e.min + '"' : '') +
      (e.max != null ? ' max="' + e.max + '"' : '') + '>';
  },

  // 收集 [data-ef] 控件的值；checkbox 按 onValue/offValue 映射回业务值
  fieldValues(root, fields) {
    const out = {};
    const edits = {};
    (fields || []).forEach(f => { if (f.edit) edits[f.key] = f.edit; });
    root.querySelectorAll('[data-ef]').forEach(el => {
      const key = el.dataset.ef;
      if (el.type === 'checkbox') {
        const e = edits[key] || {};
        out[key] = el.checked
          ? (e.onValue != null ? e.onValue : '1')
          : (e.offValue != null ? e.offValue : '');
      } else {
        out[key] = el.value;
      }
    });
    return out;
  },

  // 开关文案随勾选即时更新；行内表单与 editTable 共用
  bindFields(root, onChange) {
    if (!root) return;
    const fire = e => { if (typeof onChange === 'function') onChange(e); };
    root.addEventListener('input', fire);
    root.addEventListener('change', e => {
      const el = e.target;
      if (el && el.type === 'checkbox' && el.parentElement) {
        const txt = el.parentElement.querySelector('.ui-switch-text');
        if (txt) txt.textContent = el.checked ? txt.dataset.on : txt.dataset.off;
      }
      fire(e);
    });
  },

  // 字段网格：标签在上、控件在下，用于表格行下方的展开表单
  formGrid(fields, data) {
    const d = data || {};
    return '<div class="ui-form-fields">' + (fields || []).map(f => {
      let ctrl;
      if (typeof f.html === 'function') ctrl = f.html(d);
      else if (f.edit) {
        const v = f.edit.value ? f.edit.value(d) : d[f.key];
        ctrl = UI.fieldControl(f, v, d);
      } else {
        ctrl = UI.esc(d[f.key] == null ? '' : d[f.key]);
      }
      return '<div class="ui-form-field' + (Number(f.span) > 1 ? ' ui-form-span2' : '') + '">' +
        '<span class="ui-form-label">' + UI.esc(f.label) + '</span>' + ctrl + '</div>';
    }).join('') + '</div>';
  },

  editTable(host, opts) {
    if (!host) return null;
    const o = opts || {};
    const fields = o.fields || [];
    const columns = Math.max(1, Number(o.columns) || 2);
    const state = { data: o.data || {}, editing: !!o.editing, saving: false, initial: null };
    const editable = fields.some(f => f.edit);

    const valueOf = f => (f.edit && f.edit.value) ? f.edit.value(state.data) : state.data[f.key];
    const esc = v => UI.esc(v == null ? '' : v);

    const control = f => UI.fieldControl(f, valueOf(f), state.data);

    const cell = f => {
      const span = Math.min(columns, Math.max(1, Number(f.span) || 1));
      const v = state.editing && f.edit ? control(f)
        : (f.view ? f.view(state.data) : esc(state.data[f.key] === '' || state.data[f.key] == null
          ? '—' : state.data[f.key]));
      return '<th>' + UI.esc(f.label) + '</th><td' +
        (span > 1 ? ' colspan="' + (span * 2 - 1) + '"' : '') +
        (f.cls ? ' class="' + f.cls + '"' : '') + '>' + v + '</td>';
    };

    const body = () => {
      let html = '', used = 0, row = [];
      const flush = () => {
        if (!row.length) return;
        html += '<tr>' + row.map(cell).join('') + '</tr>';
        row = []; used = 0;
      };
      fields.forEach(f => {
        const span = Math.min(columns, Math.max(1, Number(f.span) || 1));
        if (used + span > columns) flush();
        row.push(f); used += span;
        if (used >= columns) flush();
      });
      flush();
      if (state.editing && typeof o.hint === 'function') {
        html += '<tr class="ui-edit-hint-row"><td colspan="' + (columns * 2) +
          '" class="ui-edit-hint-cell"><span class="ui-edit-hint"></span></td></tr>';
      }
      return html;
    };

    const values = () => UI.fieldValues(host, fields);

    const isDirty = () => state.editing && state.initial !== null &&
      JSON.stringify(values()) !== state.initial;

    const updateHint = () => {
      if (!state.editing || typeof o.hint !== 'function') return;
      const el = host.querySelector('.ui-edit-hint');
      if (!el) return;
      const r = o.hint(values(), state.data);
      let text = '', warn = false;
      if (r && typeof r === 'object') { text = r.text || ''; warn = !!r.warn; }
      else text = r || '';
      el.textContent = text;
      el.classList.toggle('warn', warn);
    };

    let renderActions = () => {};
    if (o.actions) {
      renderActions = () => {
        o.actions.innerHTML = typeof o.renderActions === 'function'
          ? o.renderActions(state.editing, state.data) : '';
        const start = o.actions.querySelector('[data-edit-start]');
        if (start) start.onclick = () => api.start();
        const save = o.actions.querySelector('[data-edit-save]');
        if (save) save.onclick = () => api.save();
        const cancel = o.actions.querySelector('[data-edit-cancel]');
        if (cancel) cancel.onclick = () => api.cancel();
      };
    }

    const render = () => {
      host.innerHTML = '<table class="detail-table ui-edit-table' +
        (o.cls ? ' ' + o.cls : '') + (state.editing ? ' is-editing' : '') +
        '"><tbody>' + body() + '</tbody></table>';
      renderActions();
      if (state.editing) {
        updateHint();
        setTimeout(() => {
          const el = (o.focus && host.querySelector(o.focus)) ||
            host.querySelector('input[data-ef],select[data-ef],textarea[data-ef]');
          if (el) { el.focus(); if (el.select) el.select(); }
        }, 0);
      }
      if (typeof o.onStateChange === 'function') o.onStateChange(state.editing, state.data);
    };

    host.addEventListener('input', e => {
      if (state.editing && e.target.dataset && e.target.dataset.ef != null) updateHint();
    });
    host.addEventListener('change', e => {
      if (!state.editing || !e.target.dataset || e.target.dataset.ef == null) return;
      if (e.target.type === 'checkbox') {
        const txt = e.target.parentElement.querySelector('.ui-switch-text');
        if (txt) txt.textContent = e.target.checked ? txt.dataset.on : txt.dataset.off;
      }
      updateHint();
    });
    host.addEventListener('keydown', e => {
      if (!state.editing) return;
      if (e.key === 'Escape') { e.preventDefault(); api.cancel(); }
      else if (e.key === 'Enter' && e.target.tagName === 'INPUT' && e.target.type !== 'checkbox') {
        e.preventDefault();
        api.save();
      }
    });

    const api = {
      host,
      get el() { return host.querySelector('table'); },
      isEditing: () => state.editing,
      isDirty,
      values,
      start() {
        if (state.editing || state.saving || !editable) return;
        state.editing = true;
        render();
        state.initial = JSON.stringify(values());
      },
      async save() {
        if (!state.editing || state.saving) return;
        state.saving = true;
        const v = values();
        const btn = o.actions && o.actions.querySelector('[data-edit-save]');
        if (typeof o.confirmSave === 'function' && !await o.confirmSave(v, state.data)) {
          state.saving = false;
          return;
        }
        if (btn) btn.disabled = true;
        try {
          if (typeof o.save === 'function') await o.save(v, state.data);
          Object.assign(state.data, v);
          state.editing = false;
          state.initial = null;
          render();
          if (typeof o.onSaved === 'function') o.onSaved(v, state.data);
        } catch (e) {
          if (btn) btn.disabled = false;
          const hint = host.querySelector('.ui-edit-hint');
          if (hint) {
            hint.textContent = '保存失败：' + e.message;
            hint.classList.add('warn');
          } else {
            UI.toast('保存失败：' + e.message, true);
          }
        } finally {
          state.saving = false;
        }
      },
      async cancel() {
        if (!state.editing || state.saving) return;
        if (isDirty() && !await UI.confirm({
          title: '放弃修改',
          message: o.dirtyMessage || '当前表单还有未保存的修改，确定放弃？',
        })) return;
        state.editing = false;
        state.initial = null;
        render();
      },
      // 页面要执行其它操作时调用：有改动先确认，返回是否已退出编辑
      async leave() {
        if (!state.editing) return true;
        if (state.saving) return false;
        if (isDirty() && !await UI.confirm({
          title: '放弃修改',
          message: o.dirtyMessage || '当前表单还有未保存的修改，继续操作会放弃修改。',
        })) return false;
        state.editing = false;
        state.initial = null;
        render();
        return true;
      },
      update(data) {
        state.data = data || {};
        if (!state.editing) render();
      },
      destroy() { host.innerHTML = ''; },
    };

    render();
    return api;
  },
  // 竖排两列明细，用于详情页信息块
  kvList(rows, opts) {
    opts = opts || {};
    return '<div class="kv-list ' + (opts.cls || '') + '">' + rows.map(r =>
      '<div class="kv-row"><span class="k">' + r[0] + '</span>' +
      '<span class="v" title="' + String(r[1]).replace(/<[^>]*>/g, '') + '">' + r[1] + '</span></div>').join('') + '</div>';
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

// 通用「启用 / 停用」状态：账号、接入账号等共用，避免各页重复定义
UI.STATUS_ACTIVE = { active: ['启用', 'ok'], disabled: ['停用', 'info'] };
