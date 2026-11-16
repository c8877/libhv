/* rtunnel 文件管理组件（本地 Web 与 relay-admin 共用）
 *
 *   const fm = RtunnelFs.createManager({ wsUrl, token, title });
 *   fm.open();
 *
 * 协议：文本帧 JSON 控制消息，二进制帧 16 字节头 + 数据。
 */
(function (global) {
    'use strict';

    var BIN_HEADER = 16;

    function fmtSize(n) {
        if (n === null || n === undefined) return '-';
        if (n < 1024) return n + ' B';
        var u = ['KB', 'MB', 'GB', 'TB'];
        var v = n / 1024, i = 0;
        while (v >= 1024 && i < u.length - 1) { v /= 1024; i++; }
        return (v >= 100 ? v.toFixed(0) : v >= 10 ? v.toFixed(1) : v.toFixed(2)) + ' ' + u[i];
    }

    function fmtTime(ts) {
        if (!ts) return '-';
        var d = new Date(ts * 1000);
        function p(x) { return x < 10 ? '0' + x : '' + x; }
        return d.getFullYear() + '-' + p(d.getMonth() + 1) + '-' + p(d.getDate()) + ' ' + p(d.getHours()) + ':' + p(d.getMinutes());
    }

    function ext(name) {
        var i = name.lastIndexOf('.');
        return i < 0 ? '' : name.slice(i + 1).toLowerCase();
    }

    var TEXT_EXT = ['txt', 'log', 'conf', 'cfg', 'ini', 'json', 'xml', 'yaml', 'yml', 'sh', 'c', 'h', 'cpp', 'hpp',
        'py', 'js', 'css', 'html', 'md', 'csv', 'service', 'env', 'toml', 'lua', 'go', 'rs', 'java'];
    var IMG_EXT = ['png', 'jpg', 'jpeg', 'gif', 'webp', 'bmp', 'svg', 'ico'];

    function isText(name) { return TEXT_EXT.indexOf(ext(name)) >= 0; }
    function isImage(name) { return IMG_EXT.indexOf(ext(name)) >= 0; }

    function langOf(name) {
        var e = ext(name);
        if (e === 'sh' || e === 'bash' || e === 'zsh' || e === 'ksh' || e === 'ash') return 'shell';
        if (e === 'json') return 'json';
        return 'text';
    }

    function jsonKind(v) {
        if (v === null) return 'null';
        if (Array.isArray(v)) return 'array';
        return typeof v;
    }

    // 懒展开的 JSON 树：只在展开时创建子节点，大文件不会卡住
    function buildJsonTree(data, opts) {
        opts = opts || {};
        var budget = opts.budget || 5000;
        var expandDepth = opts.expandDepth === undefined ? 2 : opts.expandDepth;
        var used = 0;

        function summary(type, val) {
            if (type === 'array') return '[' + val.length + ' 项]';
            return '{' + Object.keys(val).length + ' 项}';
        }

        function makeNode(key, val, depth, isIndex) {
            used++;
            var node = document.createElement('div');
            node.className = 'jnode';
            var row = document.createElement('div');
            row.className = 'jr';
            var caret = document.createElement('span');
            caret.className = 'jc';
            var body = document.createElement('span');
            body.className = 'jt';
            row.appendChild(caret);
            row.appendChild(body);
            node.appendChild(row);

            var type = jsonKind(val);
            var container = type === 'object' || type === 'array';
            var empty = container && (type === 'array' ? val.length === 0 : Object.keys(val).length === 0);

            if (key !== null) {
                var k = document.createElement('span');
                k.className = isIndex ? 'ji' : 'jk';
                k.textContent = isIndex ? key : JSON.stringify(key);
                body.appendChild(k);
                body.appendChild(document.createTextNode(': '));
            }

            if (container) {
                var sum = document.createElement('span');
                sum.className = 'jm';
                sum.textContent = empty ? (type === 'array' ? '[]' : '{}') : summary(type, val);
                body.appendChild(sum);
            }
            else {
                var v = document.createElement('span');
                if (type === 'string') { v.className = 'js'; v.textContent = JSON.stringify(val); }
                else if (type === 'number') { v.className = 'jn'; v.textContent = String(val); }
                else if (type === 'boolean') { v.className = 'jb'; v.textContent = String(val); }
                else { v.className = 'jz'; v.textContent = 'null'; }
                body.appendChild(v);
            }

            if (!container || empty) return node;

            var wrap = document.createElement('div');
            wrap.className = 'jchild';
            wrap.hidden = true;
            node.appendChild(wrap);

            var built = false, open = false;
            function build() {
                if (built) return;
                built = true;
                var keys = type === 'array' ? null : Object.keys(val);
                var n = type === 'array' ? val.length : keys.length;
                for (var i = 0; i < n; i++) {
                    if (used >= budget) {
                        var more = document.createElement('div');
                        more.className = 'jr';
                        more.innerHTML = '<span class="jc"></span><span class="jt jm">… 已达显示上限，剩余 ' + (n - i) + ' 项未显示</span>';
                        wrap.appendChild(more);
                        break;
                    }
                    if (type === 'array') wrap.appendChild(makeNode(i, val[i], depth + 1, true));
                    else wrap.appendChild(makeNode(keys[i], val[keys[i]], depth + 1, false));
                }
            }
            function toggle(force) {
                var want = force === undefined ? !open : !!force;
                if (want === open) return;
                open = want;
                if (open) build();
                wrap.hidden = !open;
                caret.textContent = open ? '▼' : '▶';
            }
            caret.addEventListener('click', function (e) { e.stopPropagation(); toggle(); });
            row.addEventListener('click', function (e) {
                if (e.target.closest('.jc')) return;
                toggle();
            });
            toggle(depth < expandDepth);
            return node;
        }

        var root = document.createElement('div');
        root.appendChild(makeNode(null, data, 0, false));
        return root;
    }

    var CSS = `
:host{all:initial}
*{box-sizing:border-box}
.overlay{position:fixed;inset:0;background:rgba(15,18,24,.62);display:flex;align-items:center;justify-content:center;
  font:14px/1.45 -apple-system,BlinkMacSystemFont,"Segoe UI","PingFang SC","Microsoft YaHei",sans-serif;
  color:var(--fg);z-index:2147483000;padding:18px}
.overlay[hidden]{display:none}
.overlay.inline{position:static;inset:auto;display:block;background:transparent;padding:0;z-index:auto}
.overlay.inline[hidden]{display:none}
.overlay.inline .win{width:100%;height:min(560px,70vh);box-shadow:none}
.overlay.inline header{display:none}
.win{width:min(1080px,100%);height:min(760px,100%);background:var(--bg);border:1px solid var(--line);border-radius:14px;
  display:flex;flex-direction:column;overflow:hidden;box-shadow:0 24px 70px rgba(0,0,0,.45)}
header{display:flex;align-items:center;gap:10px;padding:12px 16px;border-bottom:1px solid var(--line);background:var(--panel)}
header .t{font-weight:650;font-size:15px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
header .sub{color:var(--muted);font-size:12px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
header .grow{flex:1;min-width:0}
.dot{display:inline-block;width:8px;height:8px;border-radius:50%;background:#9aa4b2;margin-right:6px;vertical-align:1px}
.dot.on{background:#22c55e;box-shadow:0 0 0 3px rgba(34,197,94,.16)}
.dot.off{background:#ef4444}
.x{border:0;background:transparent;color:var(--muted);font-size:22px;line-height:1;cursor:pointer;padding:2px 8px;border-radius:8px}
.x:hover{background:var(--hover);color:var(--fg)}
.bar{display:flex;align-items:center;gap:8px;padding:8px 12px;border-bottom:1px solid var(--line);background:var(--panel);flex-wrap:wrap}
.bar.tools{background:var(--bg)}
button{border:1px solid var(--line);background:var(--btn);color:var(--fg);border-radius:8px;padding:5px 10px;font-size:13px;cursor:pointer;white-space:nowrap}
button:hover:not(:disabled){background:var(--hover)}
button:disabled{opacity:.45;cursor:not-allowed}
button.primary{background:var(--accent);border-color:var(--accent);color:#fff}
button.danger{color:#ef4444;border-color:rgba(239,68,68,.4)}
.path{flex:1;min-width:160px;border:1px solid var(--line);background:var(--input);color:var(--fg);border-radius:8px;padding:5px 9px;font:12px ui-monospace,SFMono-Regular,Menlo,monospace}
.crumb{flex:1;min-width:120px;display:flex;align-items:center;gap:1px;border:1px solid var(--line);background:var(--input);border-radius:8px;padding:3px 6px;overflow-x:auto;white-space:nowrap;scrollbar-width:none}
.crumb::-webkit-scrollbar{display:none}
.crumb .seg{border:0;background:transparent;color:var(--accent);font:12px ui-monospace,SFMono-Regular,Menlo,monospace;padding:2px 5px;border-radius:5px;cursor:pointer;max-width:220px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.crumb .seg:hover{background:var(--hover);text-decoration:underline}
.crumb .seg.cur{color:var(--fg);font-weight:600;cursor:default}
.crumb .seg.cur:hover{background:transparent;text-decoration:none}
.crumb .sep{color:var(--muted);font-size:11px;padding:0 1px}
.search{border:1px solid var(--line);background:var(--input);color:var(--fg);border-radius:8px;padding:5px 9px;font-size:13px;width:170px}
label.chk{display:flex;align-items:center;gap:5px;color:var(--muted);font-size:12px;white-space:nowrap}
.meta{margin-left:auto;color:var(--muted);font-size:12px;white-space:nowrap}
.tablewrap{flex:1;overflow:auto;background:var(--bg)}
table{width:100%;border-collapse:collapse;font-size:13px}
thead th{position:sticky;top:0;background:var(--panel);border-bottom:1px solid var(--line);text-align:left;padding:7px 8px;
  color:var(--muted);font-weight:600;font-size:12px;white-space:nowrap;cursor:pointer;z-index:1}
thead th.c{cursor:default;width:30px}
tbody td{border-bottom:1px solid var(--line-soft);padding:6px 8px;vertical-align:middle}
tbody tr:hover{background:var(--hover)}
tbody tr.sel{background:var(--sel)}
tbody tr[data-type=dir]{cursor:pointer}
.name{display:flex;align-items:center;gap:8px;min-width:0}
.name .nm{white-space:nowrap;overflow:hidden;text-overflow:ellipsis;max-width:420px}
.badge{font:10px/1 ui-monospace,monospace;border-radius:5px;padding:3px 4px;background:var(--badge);color:var(--muted);flex:none}
.badge.dir{background:rgba(59,130,246,.16);color:#3b82f6}
.badge.img{background:rgba(168,85,247,.16);color:#a855f7}
.badge.lnk{background:rgba(245,158,11,.18);color:#f59e0b}
.ops{white-space:nowrap;text-align:right}
.ops button{padding:2px 7px;font-size:12px;margin-left:4px}
.formrow td{padding:0;background:var(--panel);border-bottom:1px solid var(--line)}
.iform{display:flex;align-items:center;gap:8px;flex-wrap:wrap;padding:9px 12px 10px 34px}
.iform .lb{color:var(--muted);font-size:12px;white-space:nowrap}
.iform input{flex:1;min-width:180px;max-width:560px;border:1px solid var(--line);background:var(--input);color:var(--fg);
  border-radius:8px;padding:6px 9px;font:12.5px ui-monospace,SFMono-Regular,Menlo,monospace;outline:none}
.iform input:focus{border-color:var(--accent)}
tr.editing td{background:var(--sel)}
.rowmenu{position:fixed;pointer-events:auto;z-index:2147483200;min-width:132px;background:var(--panel);border:1px solid var(--line);border-radius:9px;
  box-shadow:0 10px 30px var(--shadow);padding:4px}
.rowmenu[hidden]{display:none}
.rowmenu button{display:block;width:100%;text-align:left;border:0;background:transparent;color:var(--fg);
  padding:6px 10px;border-radius:6px;font-size:13px;white-space:nowrap;margin:0}
.rowmenu button:hover{background:var(--hover)}
.rowmenu button.danger{color:#ef4444}
.rowmenu .sep{height:1px;background:var(--line);margin:4px 2px}
.empty{padding:48px 16px;text-align:center;color:var(--muted)}
footer{display:flex;align-items:center;gap:12px;padding:8px 14px;border-top:1px solid var(--line);background:var(--panel);font-size:12px;color:var(--muted);min-height:40px}
.summary{white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.up{display:none;align-items:center;gap:8px;margin-left:auto;min-width:0}
.up.show{display:flex}
.up .nm{max-width:260px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis;color:var(--fg)}
.upbar{width:160px;height:6px;border-radius:4px;background:var(--line);overflow:hidden}
.upbar i{display:block;height:100%;width:0;background:var(--accent);transition:width .12s}
.modal{position:fixed;inset:0;background:rgba(15,18,24,.62);display:flex;align-items:center;justify-content:center;z-index:2147483001;padding:18px;pointer-events:auto}
.modal[hidden]{display:none}
.mbox{width:min(820px,100%);height:min(620px,100%);background:var(--bg);border:1px solid var(--line);border-radius:14px;display:flex;flex-direction:column;overflow:hidden}
.mhead{display:flex;align-items:center;gap:10px;padding:10px 14px;border-bottom:1px solid var(--line);background:var(--panel)}
.mhead .t{font-weight:600;flex:1;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
textarea{flex:1;width:100%;border:0;outline:0;resize:none;padding:12px 14px;background:var(--input);color:var(--fg);
  font:12.5px/1.55 ui-monospace,SFMono-Regular,Menlo,monospace;white-space:pre;tab-size:4}
.mfoot{display:flex;justify-content:flex-end;gap:8px;padding:9px 12px;border-top:1px solid var(--line);background:var(--panel)}
.pv{flex:1;display:flex;align-items:center;justify-content:center;overflow:auto;background:var(--input);padding:12px}
.pv img{max-width:100%;max-height:100%;object-fit:contain}
.ed{flex:1;min-height:0;display:flex;overflow:hidden}
.ed textarea{flex:1;min-height:0}
.ed .cm-editor{flex:1;min-width:0;height:100%}
.jv{flex:1;overflow:auto;background:var(--input);padding:10px 12px;
  font:12.5px/1.7 ui-monospace,SFMono-Regular,Menlo,monospace}
.jv .jnode{min-width:0}
.jv .jr{display:flex;align-items:flex-start;gap:4px}
.jv .jc{width:14px;flex:none;text-align:center;color:var(--muted);cursor:pointer;user-select:none}
.jv .jt{min-width:0;white-space:pre-wrap;word-break:break-word}
.jv .jk{color:#e45649}
.jv .ji{color:var(--muted)}
.jv .js{color:#50a14f}
.jv .jn,.jv .jb{color:#986801}
.jv .jz{color:var(--muted)}
.jv .jm{color:var(--muted)}
.jv .jchild{padding-left:14px;margin-left:6px;border-left:1px solid var(--line-soft)}
.jv .jerr{color:#ef4444;white-space:pre-wrap}
.toast{position:fixed;left:50%;bottom:28px;transform:translateX(-50%);background:#111827;color:#fff;padding:8px 14px;border-radius:9px;
  font-size:13px;z-index:2147483002;opacity:0;transition:opacity .15s;pointer-events:none;max-width:80vw}
.toast.show{opacity:1}
.toast.err{background:#b91c1c}
@media (max-width:720px){
  .overlay{padding:0}
  .win{width:100%;height:100%;border-radius:0;border:0}
  .bar{gap:6px;padding:7px 8px}
  .bar button{padding:5px 8px;font-size:12px}
  .path{min-width:100px}
  .search{width:120px}
  .meta{display:none}
  th:nth-child(3),td:nth-child(3),th:nth-child(4),td:nth-child(4){display:none}
  .name .nm{max-width:150px}
  .iform{padding:8px 8px 9px 10px}
  .iform input{min-width:120px}
}
:host{--bg:#f7f8fa;--panel:#ffffff;--input:#ffffff;--fg:#1f2937;--muted:#6b7280;--line:#e5e7eb;--line-soft:#f0f1f4;
  --hover:#f3f4f6;--sel:#eef4ff;--btn:#ffffff;--badge:#eef0f4;--accent:#2563eb;--shadow:rgba(0,0,0,.08)}
@media (prefers-color-scheme:dark){
  :host{--bg:#151922;--panel:#1b202b;--input:#12161e;--fg:#e5e7eb;--muted:#9ca3af;--line:#2b3240;--line-soft:#222836;
    --hover:#222a38;--sel:#1e2b45;--btn:#222937;--badge:#2a3140;--accent:#3b82f6}
  .jv .jk{color:#e06c75}
  .jv .js{color:#98c379}
  .jv .jn,.jv .jb{color:#d19a66}
}`;

    var HTML = `
<div class="overlay" hidden>
  <div class="win">
    <header>
      <div class="grow">
        <div class="t" id="title">文件管理</div>
        <div class="sub" id="rootLine">—</div>
      </div>
      <button class="x" id="close" title="关闭">×</button>
    </header>
    <div class="bar">
      <div class="crumb" id="crumb"></div>
      <input class="path" id="path" spellcheck="false" autocomplete="off" hidden>
      <button id="editPath" title="输入路径">编辑</button>
      <button id="refresh">刷新</button>
      <label class="chk"><input type="checkbox" id="hidden"> 隐藏文件</label>
    </div>
    <div class="bar tools">
      <button id="mkdir">+ 新建文件夹</button>
      <button id="touch">+ 新建文件</button>
      <button class="primary" id="upload">上传</button>
      <button class="danger" id="del">删除</button>
      <input class="search" id="search" placeholder="搜索当前目录">
      <span class="meta" id="meta"></span>
    </div>
    <div class="tablewrap">
      <table>
        <thead><tr>
          <th class="c"><input type="checkbox" id="all"></th>
          <th data-sort="name">名称</th>
          <th data-sort="size">大小</th>
          <th data-sort="mtime">修改时间</th>
          <th style="text-align:right">操作</th>
        </tr></thead>
        <tbody id="rows"></tbody>
      </table>
      <div class="empty" id="empty" hidden>空目录</div>
    </div>
    <footer>
      <div class="summary" id="summary">—</div>
      <div class="up" id="upbox">
        <span class="nm" id="upname"></span>
        <div class="upbar"><i id="upbar"></i></div>
        <span id="uppct">0%</span>
        <button id="upcancel">取消</button>
      </div>
    </footer>
  </div>
</div>
<div class="modal" id="editor" hidden>
  <div class="mbox">
    <div class="mhead"><span class="t" id="edTitle">编辑</span>
      <button id="edTree" hidden>树形</button><button id="edClose">关闭</button></div>
    <div class="ed" id="edHost"><textarea id="edText" spellcheck="false"></textarea></div>
    <div class="mfoot"><button id="edCancel">取消</button><button class="primary" id="edSave">保存</button></div>
  </div>
</div>
<div class="modal" id="preview" hidden>
  <div class="mbox">
    <div class="mhead"><span class="t" id="pvTitle">预览</span><button id="pvClose">关闭</button></div>
    <div class="pv" id="pvBody"></div>
  </div>
</div>
<div class="modal" id="jsonview" hidden>
  <div class="mbox">
    <div class="mhead"><span class="t" id="jvTitle">查看</span>
      <button id="jvExpand">展开全部</button><button id="jvCollapse">收起</button>
      <button id="jvSource">源码</button><button id="jvClose">关闭</button></div>
    <div class="jv" id="jvBody"></div>
  </div>
</div>
<div class="rowmenu" id="rowmenu" hidden></div>
<div class="toast" id="toast"></div>
<input type="file" id="file" multiple hidden>
`;

    function Manager(opts) {
        opts = opts || {};
        this.opts = opts;
        this.wsUrl = opts.wsUrl || ((location.protocol === 'https:' ? 'wss://' : 'ws://') + location.host + '/ws/fs');
        this.token = opts.token || sessionStorage.getItem('rtfs.token:' + this.wsUrl) || '';
        this.title = opts.title || '文件管理';
        this.subtitle = opts.subtitle || '';
        this.readonly = !!opts.readonly;
        this.inline = !!opts.inline;
        this.container = opts.container || null;
        this.rw = false;   // 以 welcome 为准
        this.onClose = opts.onClose || null;
        this.onSpace = opts.onSpace || null;

        this.ws = null;
        this.connSeq = 0;
        this.connected = false;
        this.welcome = null;
        this.waiters = [];
        this.binWaiters = [];
        this.binBuf = {};
        this.seq = 1;
        this.cwd = '/';
        this.entries = [];
        this.total = 0;
        this.sel = new Set();
        this.dirSizes = new Map();     // 勾选目录后递归统计的结果
        this.dirPending = new Map();
        this.dirSizeUnsupported = false;
        this.sortKey = 'name';
        this.sortDir = 1;
        this.filter = '';
        this.hidden = false;
        this.visible = false;
        this.uploading = false;
        this.uploadCancel = false;
        this.editing = null;
        this.editorInst = null;
        this.editorToken = 0;
        this._editorLib = null;
        this.jvPath = '';
        this.jvText = '';
        this.jvData = null;
        this.previewUrl = null;
        this.reconnectTimer = null;
        this.closedByUser = false;
        this.destroyed = false;

        this._build();
        this._bind();
    }

    Manager.prototype._build = function () {
        var host = document.createElement('div');
        if (this.inline) {
            host.style.cssText = 'display:block';
            (this.container || document.body).appendChild(host);
        } else {
            host.style.cssText = 'position:fixed;inset:0;pointer-events:none;z-index:2147483000';
            document.body.appendChild(host);
        }
        this.host = host;
        this.sh = host.attachShadow({ mode: 'open' });
        var st = document.createElement('style');
        st.textContent = CSS;
        this.sh.appendChild(st);
        var wrap = document.createElement('div');
        wrap.innerHTML = HTML;
        while (wrap.firstChild) this.sh.appendChild(wrap.firstChild);
        var q = this.q = {};
        var ids = ['close', 'title', 'rootLine', 'crumb', 'path', 'editPath', 'refresh', 'hidden', 'rowmenu',
            'mkdir', 'touch', 'upload', 'del', 'search', 'meta', 'all', 'rows', 'empty', 'summary',
            'upbox', 'upname', 'upbar', 'uppct', 'upcancel', 'editor', 'edTitle', 'edText', 'edHost', 'edTree',
            'edClose', 'edCancel', 'edSave', 'preview', 'pvTitle', 'pvBody', 'pvClose',
            'jsonview', 'jvTitle', 'jvBody', 'jvClose', 'jvSource', 'jvExpand', 'jvCollapse', 'toast', 'file'];
        for (var i = 0; i < ids.length; i++) q[ids[i]] = this.sh.getElementById(ids[i]);
        q.title.textContent = this.title;
        this.overlay = this.sh.querySelector('.overlay');
        if (this.inline) this.overlay.classList.add('inline');
        this.overlay.style.pointerEvents = 'auto';
        this.renderCrumb();
    };

    Manager.prototype._bind = function () {
        var self = this;
        function on(id, ev, fn) { self.q[id].addEventListener(ev, fn); }
        on('close', 'click', function () { self.close(); });
        on('refresh', 'click', function () { self.load(self.cwd); });
        on('path', 'keydown', function (e) {
            if (e.key === 'Enter') self.gotoPath(self.q.path.value);
            else if (e.key === 'Escape') self.endPathEdit();
        });
        on('editPath', 'click', function () {
            if (self.pathEditing) self.endPathEdit();
            else self.startPathEdit();
        });
        on('crumb', 'click', function (e) {
            var seg = e.target.closest('button[data-path]');
            if (!seg) return;
            var dest = seg.getAttribute('data-path');
            if (dest !== self.cwd) self.load(dest);
        });
        on('hidden', 'change', function () { self.hidden = self.q.hidden.checked; self.load(self.cwd); });
        on('mkdir', 'click', function () { self.mkdir(); });
        on('touch', 'click', function () { self.touch(); });
        on('upload', 'click', function () { self.q.file.click(); });
        on('file', 'change', function () { self.uploadFiles(self.q.file.files, self.cwd); self.q.file.value = ''; });
        on('del', 'click', function () { self.removeSelected(); });
        on('all', 'change', function () { self.selectAll(self.q.all.checked); });
        on('search', 'input', function () { self.filter = self.q.search.value.toLowerCase(); self.render(); });
        on('upcancel', 'click', function () { self.uploadCancel = true; });
        on('edClose', 'click', function () { self.closeEditor(); });
        on('edCancel', 'click', function () { self.closeEditor(); });
        on('edSave', 'click', function () { self.saveEditor(); });
        on('edTree', 'click', function () {
            var ed = self.editing;
            if (!ed) return;
            var text = self.editorInst ? self.editorInst.getValue() : self.q.edText.value;
            self.closeEditor();
            self.openJsonView(ed.path, text);
        });
        on('jvClose', 'click', function () { self.closeJsonView(); });
        on('jvExpand', 'click', function () { self.renderJsonView(64, 20000); });
        on('jvCollapse', 'click', function () { self.renderJsonView(1, 5000); });
        on('jvSource', 'click', function () {
            var path = self.jvPath, text = self.jvText;
            self.closeJsonView();
            if (path) self.openEditor(path, text);
        });
        on('pvClose', 'click', function () { self.closePreview(); });
        this.sh.querySelectorAll('th[data-sort]').forEach(function (th) {
            th.addEventListener('click', function () {
                var k = th.getAttribute('data-sort');
                if (self.sortKey === k) self.sortDir = -self.sortDir;
                else { self.sortKey = k; self.sortDir = 1; }
                self.render();
            });
        });
        this.q.rows.addEventListener('click', function (e) {
            var tr = e.target.closest('tr');
            if (!tr || tr.dataset.name === undefined) return;
            if (e.target.matches('input[type=checkbox]')) {
                self.toggleSel(tr.dataset.name, e.target.checked);
                return;
            }
            if (e.target.closest('button')) return;
            // 目录名称单击直接进入（不再保留「打开」按钮）
            if (tr.dataset.type === 'dir' && e.target.closest('.nm')) {
                self._navAt = Date.now();
                self.openEntry(tr.dataset.name, 'dir');
            }
        });
        this.q.rows.addEventListener('dblclick', function (e) {
            if (e.target.closest('button')) return;
            var tr = e.target.closest('tr');
            if (!tr || tr.dataset.name === undefined) return;
            // 单击目录名已经触发跳转，避免双击在换目录后误开新行
            if (tr.dataset.type === 'dir' && Date.now() - (self._navAt || 0) < 500) return;
            // 文件：文本进编辑器、图片进预览、其他直接下载
            self.openEntry(tr.dataset.name, tr.dataset.type);
        });
        this.q.rows.addEventListener('click', function (e) {
            var mb = e.target.closest('button[data-menu]');
            if (!mb) return;
            e.stopPropagation();
            var tr = mb.closest('tr');
            if (!tr) return;
            self.showRowMenu(mb, tr.dataset.name, tr.dataset.type);
        });
        this.q.rows.addEventListener('click', function (e) {
            var b = e.target.closest('button[data-op]');
            if (!b) return;
            var tr = b.closest('tr');
            if (!tr) return;
            self.runAction(tr.dataset.name, tr.dataset.type, b.getAttribute('data-op'));
        });
        this.q.rowmenu.addEventListener('click', function (e) {
            e.stopPropagation();
            var b = e.target.closest('button[data-op]');
            if (!b) return;
            var m = self.q.rowmenu;
            var name = m.dataset.name, type = m.dataset.type, op = b.getAttribute('data-op');
            self.closeRowMenu();
            self.runAction(name, type, op);
        });
        this._docClick = function (e) {
            if (!self.q.rowmenu || self.q.rowmenu.hidden) return;
            var path = e.composedPath ? e.composedPath() : [];
            if (path.indexOf(self.q.rowmenu) < 0) self.closeRowMenu();
        };
        document.addEventListener('click', this._docClick);
        this._closeMenu = function () {
            // 点击时浏览器可能先把按钮滚进视野，忽略刚打开瞬间的滚动
            if (Date.now() - (self._menuAt || 0) > 250) self.closeRowMenu();
        };
        window.addEventListener('scroll', this._closeMenu, true);
        window.addEventListener('resize', this._closeMenu);
        if (global.ResizeObserver) {
            this._lastW = 0;
            this._ro = new ResizeObserver(function () {
                var w = self.q.rows && self.q.rows.parentElement ? self.q.rows.parentElement.clientWidth : 0;
                if (w && w !== self._lastW) {
                    self._lastW = w;
                    if (self.entries.length || self.visible) self.render();
                }
            });
            this._ro.observe(this.overlay.querySelector('.win'));
        }
        // 拖拽上传
        this.overlay.addEventListener('dragover', function (e) { e.preventDefault(); });
        this.overlay.addEventListener('drop', function (e) {
            e.preventDefault();
            if (self.readonly || !self.rw || !self.connected) return;
            if (e.dataTransfer && e.dataTransfer.files && e.dataTransfer.files.length) {
                var dest = self.cwd;
                var tr = e.target && e.target.closest ? e.target.closest('tr') : null;
                if (tr && tr.dataset && tr.dataset.type === 'dir') dest = join(self.cwd, tr.dataset.name);
                self.uploadFiles(e.dataTransfer.files, dest);
            }
        });
        this._keyHandler = function (e) {
            if (!self.visible) return;
            var path = e.composedPath ? e.composedPath() : [e.target];
            var t = path && path[0];
            if (t && (t.tagName === 'INPUT' || t.tagName === 'TEXTAREA' || (t.closest && t.closest('.cm-editor')))) return;
            if (e.key === 'Escape') {
                if (!self.q.rowmenu.hidden) self.closeRowMenu();
                else if (!self.q.jsonview.hidden) self.closeJsonView();
                else if (!self.q.editor.hidden) self.closeEditor();
                else if (!self.q.preview.hidden) self.closePreview();
                else self.close();
                e.preventDefault();
            }
            else if (e.key === 'Delete') { self.removeSelected(); e.preventDefault(); }
            else if (e.key === 'F2' && self.sel.size === 1) {
                var n = Array.from(self.sel)[0];
                self.rename(n, join(self.cwd, n)); e.preventDefault();
            }
            else if (e.key === 'Backspace' && self.cwd !== '/') { self.load(parentOf(self.cwd)); e.preventDefault(); }
        };
        document.addEventListener('keydown', this._keyHandler);
    };

    function join(dir, name) { return (dir === '/' ? '' : dir) + '/' + name; }
    function parentOf(p) {
        var i = p.lastIndexOf('/');
        return i <= 0 ? '/' : p.slice(0, i);
    }

    Manager.prototype.id = function () { return this.seq++; };

    Manager.prototype.toast = function (msg, isErr) {
        var t = this.q.toast;
        t.textContent = msg;
        t.className = 'toast show' + (isErr ? ' err' : '');
        clearTimeout(this._toastTimer);
        var self = this;
        this._toastTimer = setTimeout(function () { t.className = 'toast'; }, isErr ? 4200 : 2200);
    };

    Manager.prototype.open = function () {
        if (this.destroyed) return;
        if (this.visible && this.connected) {
            this.overlay.hidden = false;
            return;
        }
        this.visible = true;
        this.closedByUser = false;
        this.overlay.hidden = false;
        if (this.inline && this.overlay.scrollIntoView) this.overlay.scrollIntoView({ block: 'nearest' });
        var self = this;
        this.connect().then(function () {
            return self.load(self.cwd || '/');
        }).catch(function (e) { self.setConn(false, e.message || '连接失败'); });
    };

    Manager.prototype.close = function () {
        this.visible = false;
        this.closedByUser = true;
        this.overlay.hidden = true;
        this.closeRowMenu();
        this.uploadCancel = true;
        this.closeEditor();
        this.closePreview();
        if (this.reconnectTimer) { clearTimeout(this.reconnectTimer); this.reconnectTimer = null; }
        this._closeWs();
        if (this.onClose) this.onClose();
    };

    Manager.prototype.destroy = function () {
        this.close();
        this.destroyed = true;
        this.closeInlineForm();
        this.destroyEditorInst();
        if (this._keyHandler) document.removeEventListener('keydown', this._keyHandler);
        if (this._docClick) document.removeEventListener('click', this._docClick);
        if (this._closeMenu) {
            window.removeEventListener('scroll', this._closeMenu, true);
            window.removeEventListener('resize', this._closeMenu);
        }
        if (this._ro) this._ro.disconnect();
        if (this.host && this.host.parentNode) this.host.parentNode.removeChild(this.host);
    };

    Manager.prototype._closeWs = function () {
        this.connSeq++;
        this.connected = false;
        var ws = this.ws;
        this.ws = null;
        if (ws) {
            try { ws.onopen = ws.onmessage = ws.onclose = ws.onerror = null; ws.close(); } catch (e) {}
        }
    };

    Manager.prototype.setConn = function (on, text) {
        this.connState = on;
        this.connText = text || '';
    };

    Manager.prototype.connect = function () {
        var self = this;
        if (this.ws && this.connected) return Promise.resolve(this.welcome);
        this._closeWs();
        var seq = this.connSeq;
        this.setConn(false, '连接中');
        return new Promise(function (resolve, reject) {
            var ws;
            try { ws = new WebSocket(self.wsUrl); }
            catch (e) { reject(new Error('WebSocket 地址无效')); return; }
            self.ws = ws;
            ws.binaryType = 'arraybuffer';
            var timer = setTimeout(function () {
                if (seq === self.connSeq) { try { ws.close(); } catch (e) {} reject(new Error('连接超时')); }
            }, 12000);
            ws.onopen = function () {
                if (seq !== self.connSeq) return;
                self._hello().then(function (w) {
                    clearTimeout(timer);
                    self.connected = true;
                    self.setConn(true, '已连接');
                    self.welcome = w;
                    self.applyWelcome();
                    resolve(w);
                }).catch(function (e) {
                    clearTimeout(timer);
                    try { ws.close(); } catch (x) {}
                    reject(e);
                });
            };
            ws.onmessage = function (ev) {
                if (seq !== self.connSeq) return;
                self._onMessage(ev.data);
            };
            ws.onclose = function () {
                if (seq !== self.connSeq) return;
                clearTimeout(timer);
                var was = self.connected;
                self.connected = false;
                self.setConn(false, '连接断开');
                reject(new Error('连接断开'));
                self._failAll(new Error('连接断开'));
                if (was && !self.closedByUser && self.visible) self._scheduleReconnect();
            };
            ws.onerror = function () {};
        });
    };

    Manager.prototype._scheduleReconnect = function () {
        var self = this;
        this._reconnectDelay = Math.min((this._reconnectDelay || 800) * 1.7, 8000);
        if (this.reconnectTimer) clearTimeout(this.reconnectTimer);
        this.reconnectTimer = setTimeout(function () {
            self.reconnectTimer = null;
            if (!self.closedByUser && self.visible) {
                self.connect().then(function () { self.load(self.cwd); }).catch(function () {});
            }
        }, this._reconnectDelay);
    };

    Manager.prototype._hello = function () {
        var self = this;
        var send = { op: 'hello', id: this.id(), v: 1, rw: !this.readonly };
        if (this.token) send.token = this.token;
        return this.request(send, 'welcome').catch(function (e) {
            if (e.code === 2 && self.visible) {
                var t = global.prompt('该设备已启用文件访问 Token，请输入：', self.token || '');
                if (t) {
                    self.token = t;
                    sessionStorage.setItem('rtfs.token:' + self.wsUrl, t);
                    return self._hello();
                }
            }
            throw e;
        });
    };

    Manager.prototype.applyWelcome = function () {
        var w = this.welcome || {};
        var rw = !!w.rw;
        this.q.rootLine.textContent = '根目录: ' + (w.root || '/') + ' · 可用 ' + fmtSize((w.free_kb || 0) * 1024) +
            ' / ' + fmtSize((w.total_kb || 0) * 1024);
        this.rw = rw;
        var ro = this.readonly || !rw;
        this.q.mkdir.disabled = ro;
        this.q.touch.disabled = ro;
        this.q.upload.disabled = ro;
        this.q.del.disabled = ro;
        this.q.upcancel.disabled = false;
        if (ro && w.root) this.q.rootLine.textContent += ' · 只读';
    };

    // ------------------------------------------------------------ WS 收发

    Manager.prototype.request = function (obj, expectOp) {
        var self = this;
        if (!this.ws || this.ws.readyState !== 1) return Promise.reject(new Error('连接已断开，请稍后重试'));
        return new Promise(function (resolve, reject) {
            var w = { id: obj.id, op: expectOp || '', resolve: resolve, reject: reject };
            self.waiters.push(w);
            try { self.ws.send(JSON.stringify(obj)); }
            catch (e) { self._dropWaiter(w); reject(e); return; }
            setTimeout(function () {
                if (self._dropWaiter(w)) reject(new Error('请求超时'));
            }, 20000);
        });
    };

    Manager.prototype._dropWaiter = function (w) {
        var i = this.waiters.indexOf(w);
        if (i < 0) return false;
        this.waiters.splice(i, 1);
        return true;
    };

    Manager.prototype.waitMsg = function (id, op) {
        var self = this;
        return new Promise(function (resolve, reject) {
            var w = { id: id, op: op, resolve: resolve, reject: reject };
            self.waiters.push(w);
            setTimeout(function () {
                if (self._dropWaiter(w)) reject(new Error('等待 ' + op + ' 超时'));
            }, 30000);
        });
    };

    Manager.prototype._failAll = function (err) {
        var ws = this.waiters.splice(0, this.waiters.length);
        for (var i = 0; i < ws.length; i++) ws[i].reject(err);
        var bs = this.binWaiters.splice(0, this.binWaiters.length);
        for (var j = 0; j < bs.length; j++) bs[j].reject(err);
        this.binBuf = {};
    };

    Manager.prototype._onMessage = function (data) {
        if (typeof data !== 'string') {
            this._onBinary(new Uint8Array(data));
            return;
        }
        var j;
        try { j = JSON.parse(data); } catch (e) { return; }
        if (j.op === 'error') {
            var msg = j.message || '操作失败';
            var err = new Error(msg);
            err.code = j.code;
            var hit = false;
            for (var i = 0; i < this.waiters.length; i++) {
                var w = this.waiters[i];
                if (w.id === j.id) {
                    this.waiters.splice(i, 1);
                    w.reject(err);
                    hit = true;
                    break;
                }
            }
            if (!hit) this.toast(msg, true);
            var bs = this.binWaiters;
            for (var n = bs.length - 1; n >= 0; n--) {
                if (bs[n].id === j.id) {
                    var bw = bs.splice(n, 1)[0];
                    bw.reject(err);
                }
            }
            if (this.binBuf[j.id]) delete this.binBuf[j.id];
            return;
        }
        if (j.op === 'pong') return;
        for (var k = 0; k < this.waiters.length; k++) {
            var x = this.waiters[k];
            if (x.id === j.id && (!x.op || x.op === j.op)) {
                this.waiters.splice(k, 1);
                x.resolve(j);
                return;
            }
        }
    };

    Manager.prototype._onBinary = function (u8) {
        if (u8.length < BIN_HEADER) return;
        var dv = new DataView(u8.buffer, u8.byteOffset, u8.byteLength);
        var id = dv.getUint32(0);
        var kind = dv.getUint8(4);
        var flags = dv.getUint8(5);
        var off = Number(dv.getBigUint64 ? dv.getBigUint64(8) : 0);
        var payload = u8.slice(BIN_HEADER);
        var evt = { kind: kind, flags: flags, offset: off, data: payload };
        for (var i = 0; i < this.binWaiters.length; i++) {
            if (this.binWaiters[i].id === id) {
                var w = this.binWaiters.splice(i, 1)[0];
                w.resolve(evt);
                return;
            }
        }
        (this.binBuf[id] = this.binBuf[id] || []).push(evt);
    };

    Manager.prototype.nextBinary = function (id) {
        var self = this;
        var q = this.binBuf[id];
        if (q && q.length) return Promise.resolve(q.shift());
        return new Promise(function (resolve, reject) {
            var w = { id: id, resolve: resolve, reject: reject };
            self.binWaiters.push(w);
            setTimeout(function () {
                var i = self.binWaiters.indexOf(w);
                if (i >= 0) { self.binWaiters.splice(i, 1); reject(new Error('等待文件数据超时')); }
            }, 30000);
        });
    };

    Manager.prototype.sendBinary = function (id, kind, off, bytes) {
        if (!this.ws || this.ws.readyState !== 1) throw new Error('连接已断开，请稍后重试');
        var buf = new ArrayBuffer(BIN_HEADER + bytes.length);
        var dv = new DataView(buf);
        dv.setUint32(0, id);
        dv.setUint8(4, kind);
        dv.setUint8(5, 0);
        dv.setUint16(6, 0);
        dv.setUint32(8, Math.floor(off / 0x100000000));
        dv.setUint32(12, off >>> 0);
        new Uint8Array(buf, BIN_HEADER).set(bytes);
        this.ws.send(buf);
    };

    // ------------------------------------------------------------ 目录

    Manager.prototype.startPathEdit = function () {
        this.pathEditing = true;
        this.q.crumb.hidden = true;
        this.q.path.hidden = false;
        this.q.editPath.textContent = '取消';
        this.q.path.value = this.cwd;
        this.q.path.focus();
        this.q.path.select();
    };

    Manager.prototype.endPathEdit = function () {
        this.pathEditing = false;
        this.q.path.hidden = true;
        this.q.editPath.textContent = '编辑';
        this.q.crumb.hidden = false;
    };

    Manager.prototype.gotoPath = function (value) {
        var self = this;
        var dest = String(value || '').trim() || '/';
        return this.load(dest).then(function () { self.endPathEdit(); });
    };

    // 面包屑：每级目录一个可点击节点，直接跳到该层
    Manager.prototype.renderCrumb = function () {
        var c = this.q.crumb;
        while (c.firstChild) c.removeChild(c.firstChild);
        var parts = this.cwd === '/' ? [] : this.cwd.split('/').filter(Boolean);
        var root = document.createElement('button');
        root.type = 'button';
        root.className = 'seg' + (parts.length ? '' : ' cur');
        root.setAttribute('data-path', '/');
        root.textContent = '根目录';
        root.title = '/';
        c.appendChild(root);
        var acc = '';
        for (var i = 0; i < parts.length; i++) {
            acc += '/' + parts[i];
            var sep = document.createElement('span');
            sep.className = 'sep';
            sep.textContent = '›';
            c.appendChild(sep);
            var b = document.createElement('button');
            b.type = 'button';
            b.className = 'seg' + (i === parts.length - 1 ? ' cur' : '');
            b.setAttribute('data-path', acc);
            b.textContent = parts[i];
            b.title = acc;
            c.appendChild(b);
        }
        c.scrollLeft = c.scrollWidth;
    };

    Manager.prototype.load = function (path) {
        var self = this;
        var id = this.id();
        return this.request({ op: 'list', id: id, path: path, hidden: this.hidden, limit: 500 }, 'list')
            .then(function (r) {
                self.cwd = r.path || path;
                self.entries = r.entries || [];
                self.total = self.entries.length;
                self.sel.clear();
                self.dirSizes.clear();
                self.dirPending.clear();
                self.q.path.value = self.cwd;
                self.renderCrumb();
                self.render();
                if (r.free_kb !== undefined) {
                    self.q.rootLine.textContent = '根目录: ' + (r.root || '/') + ' · 可用 ' + fmtSize(r.free_kb * 1024) +
                        ' / ' + fmtSize(r.total_kb * 1024) + (r.rw ? '' : ' · 只读');
                    self.space = { root: r.root || '/', free_kb: r.free_kb, total_kb: r.total_kb };
                    if (self.onSpace) self.onSpace(self.space);
                }
            })
            .catch(function (e) { if (self.visible) self.toast(e.message, true); });
    };

    Manager.prototype.render = function () {
        var self = this;
        this.closeInlineForm();
        var rows = this.q.rows;
        while (rows.firstChild) rows.removeChild(rows.firstChild);
        // 操作列可用宽度 → 最多平铺几个按钮，剩下的进 ⋯
        var wrapW = rows.parentElement ? rows.parentElement.clientWidth : 0;
        var avail = wrapW - 480;
        var cum = [0, 44, 90, 138, 186, 238, 290];
        this._opSlots = 0;
        for (var ci = 0; ci < cum.length; ci++) if (avail >= cum[ci]) this._opSlots = ci;
        var list = this.entries.slice();
        var f = this.filter;
        if (f) list = list.filter(function (e) { return e.name.toLowerCase().indexOf(f) >= 0; });
        list.sort(function (a, b) {
            var k = self.sortKey;
            if (k === 'size') {
                // 目录大小来自勾选后的递归统计；未统计的目录排在最后
                var ax = self.sizeForSort(a), bx = self.sizeForSort(b);
                if (ax === null && bx === null) return String(a.name).localeCompare(String(b.name), 'zh-CN');
                if (ax === null) return 1;
                if (bx === null) return -1;
                if (ax !== bx) return self.sortDir * (ax - bx);
                return String(a.name).localeCompare(String(b.name), 'zh-CN');
            }
            var d = (a.type === 'dir' ? 0 : 1) - (b.type === 'dir' ? 0 : 1);
            if (d) return d;
            var x = a[k], y = b[k];
            if (k === 'name') return self.sortDir * String(x).localeCompare(String(y), 'zh-CN');
            return self.sortDir * ((x || 0) - (y || 0));
        });
        var totalSize = 0;
        for (var i = 0; i < list.length; i++) {
            var e = list[i];
            if (e.type === 'file') totalSize += e.size || 0;
            var tr = document.createElement('tr');
            tr.dataset.name = e.name;
            tr.dataset.type = e.type;
            if (this.sel.has(e.name)) tr.className = 'sel';

            var td0 = document.createElement('td');
            var cb = document.createElement('input');
            cb.type = 'checkbox';
            cb.checked = this.sel.has(e.name);
            td0.appendChild(cb);
            tr.appendChild(td0);

            var td1 = document.createElement('td');
            var nm = document.createElement('div');
            nm.className = 'name';
            var badge = document.createElement('span');
            badge.className = 'badge ' + (e.type === 'dir' ? 'dir' : e.type === 'link' ? 'lnk' : isImage(e.name) ? 'img' : '');
            badge.textContent = e.type === 'dir' ? 'DIR' : e.type === 'link' ? 'LNK' : isImage(e.name) ? 'IMG' : 'FILE';
            var span = document.createElement('span');
            span.className = 'nm';
            span.textContent = e.name + (e.is_link && e.target ? ' → ' + e.target : '');
            span.title = e.name;
            nm.appendChild(badge); nm.appendChild(span);
            td1.appendChild(nm);
            tr.appendChild(td1);

            var td2 = document.createElement('td');
            if (e.type === 'dir') {
                var ds = this.dirSizes.get(e.name);
                if (ds) {
                    td2.textContent = fmtSize(ds.bytes) + (ds.truncated ? '+' : '');
                    td2.title = '包含子目录';
                }
                else if (this.dirPending.get(e.name)) td2.textContent = '统计中…';
                else td2.textContent = '-';
            }
            else td2.textContent = fmtSize(e.size);
            tr.appendChild(td2);
            var td3 = document.createElement('td');
            td3.textContent = fmtTime(e.mtime);
            tr.appendChild(td3);

            var td4 = document.createElement('td');
            td4.className = 'ops';
            var acts = this.rowActions(e.type, e.name);
            var slots = Math.min(this._opSlots, acts.length);
            for (var ai = 0; ai < slots; ai++) {
                var ab = document.createElement('button');
                ab.setAttribute('data-op', acts[ai].op);
                ab.textContent = acts[ai].label;
                if (acts[ai].danger) ab.className = 'danger';
                td4.appendChild(ab);
            }
            if (slots < acts.length) {
                var mb = document.createElement('button');
                mb.setAttribute('data-menu', '1');
                mb.textContent = '⋯';
                mb.title = '更多操作';
                td4.appendChild(mb);
            }
            tr.appendChild(td4);
            rows.appendChild(tr);
        }
        this.q.empty.hidden = list.length > 0;
        var selCount = this.sel.size, selSize = 0, sizePending = false;
        for (var s of this.sel) {
            var en = this.entries.find(function (x) { return x.name === s; });
            if (!en) continue;
            if (en.type === 'file') selSize += en.size || 0;
            else if (en.type === 'dir') {
                var dsz = this.dirSizes.get(s);
                if (dsz) selSize += dsz.bytes;
                else if (this.dirPending.get(s)) sizePending = true;
            }
        }
        this.q.summary.textContent = list.length + ' 项 · ' + fmtSize(totalSize) +
            (selCount ? ' · 已选 ' + selCount + ' 项 (' + fmtSize(selSize) + (sizePending ? '，统计中…' : '') + ')' : '');
        this.q.all.checked = list.length > 0 && this.sel.size === this.entries.length;
        this.q.meta.textContent = (this.filter ? '筛选 ' + list.length + ' / ' + this.entries.length : '共 ' + this.entries.length + ' 项');
    };

    Manager.prototype.rowActions = function (type, name) {
        var items = [{ op: 'dl', label: '下载' }];
        var w = !this.readonly && this.rw;
        if (w && type === 'file' && isImage(name)) items.push({ op: 'pv', label: '预览' });
        if (w) {
            items.push({ op: 'rename', label: '重命名' });
            items.push({ op: 'move', label: '移动' });
            items.push({ op: 'del', label: '删除', danger: true });
        }
        return items;
    };

    Manager.prototype.runAction = function (name, type, op) {
        var path = join(this.cwd, name);
        if (op === 'dl') {
            if (type === 'dir') this.downloadArchive(path);
            else this.downloadToFile(path);
        }
        else if (op === 'pv') this.openPreview(path);
        else if (op === 'rename') this.rename(name, path);
        else if (op === 'move') this.move(name, path);
        else if (op === 'del') this.remove(name, type === 'dir', path);
    };

    Manager.prototype.closeRowMenu = function () {
        if (!this.q.rowmenu) return;
        this.q.rowmenu.hidden = true;
        this.q.rowmenu.dataset.name = '';
        this.q.rowmenu.dataset.type = '';
    };

    Manager.prototype.showRowMenu = function (btn, name, type) {
        var self = this;
        var acts = this.rowActions(type, name);
        var rest = acts.slice(Math.min(this._opSlots, acts.length));
        if (!rest.length) return;
        var menu = this.q.rowmenu;
        while (menu.firstChild) menu.removeChild(menu.firstChild);
        for (var i = 0; i < rest.length; i++) {
            if (rest[i].op === 'del' && i > 0) {
                var sep = document.createElement('div');
                sep.className = 'sep';
                menu.appendChild(sep);
            }
            var b = document.createElement('button');
            b.setAttribute('data-op', rest[i].op);
            b.textContent = rest[i].label;
            if (rest[i].danger) b.className = 'danger';
            menu.appendChild(b);
        }
        menu.dataset.name = name;
        menu.dataset.type = type;
        this._menuAt = Date.now();
        menu.style.left = '0px';
        menu.style.top = '0px';
        menu.hidden = false;
        var rb = btn.getBoundingClientRect();
        var mw = menu.offsetWidth, mh = menu.offsetHeight;
        var left = Math.min(Math.max(8, rb.right - mw), global.innerWidth - mw - 8);
        var top = rb.bottom + 6;
        if (top + mh > global.innerHeight - 8) top = Math.max(8, rb.top - mh - 6);
        menu.style.left = left + 'px';
        menu.style.top = top + 'px';
    };

    Manager.prototype.sizeForSort = function (e) {
        if (e.type !== 'dir') return e.size || 0;
        var ds = this.dirSizes.get(e.name);
        return ds ? ds.bytes : null;
    };

    Manager.prototype.selectAll = function (on) {
        if (on === undefined) on = this.sel.size !== this.entries.length;
        this.sel.clear();
        if (on) {
            for (var i = 0; i < this.entries.length; i++) {
                var e = this.entries[i];
                this.sel.add(e.name);
                if (e.type === 'dir') this.fetchDirSize(e.name);
            }
        }
        this.render();
    };

    Manager.prototype.toggleSel = function (name, on) {
        if (on) this.sel.add(name); else this.sel.delete(name);
        if (on) {
            var e = this.entries.find(function (x) { return x.name === name; });
            if (e && e.type === 'dir') this.fetchDirSize(name);
        }
        this.render();
    };

    // 勾选目录时按需递归统计，结果缓存在当前目录的 dirSizes 中
    Manager.prototype.fetchDirSize = function (name) {
        var self = this;
        if (this.dirSizeUnsupported) return;
        if (this.dirSizes.has(name) || this.dirPending.has(name)) return;
        this.dirPending.set(name, true);
        var id = this.id();
        this.request({ op: 'size', id: id, path: join(this.cwd, name) }, 'size')
            .then(function (r) {
                self.dirPending.delete(name);
                self.dirSizes.set(name, { bytes: r.bytes || 0, truncated: !!r.truncated });
                self.render();
            })
            .catch(function (e) {
                self.dirPending.delete(name);
                var unsupported = /unknown op/i.test(e.message || '');
                if (unsupported && self.dirSizeUnsupported) return self.render();
                if (unsupported) self.dirSizeUnsupported = true;
                self.toast('统计大小失败：' + e.message, true);
                self.render();
            });
    };

    Manager.prototype.openEntry = function (name, type) {
        if (type === 'dir') { this._navAt = Date.now(); this.load(join(this.cwd, name)); return; }
        if (isImage(name)) this.openPreview(join(this.cwd, name));
        else if (isText(name)) this.openEditor(join(this.cwd, name));
        else this.downloadToFile(join(this.cwd, name));
    };

    Manager.prototype.mkdir = function () {
        var self = this;
        var name = global.prompt('新建文件夹名称：', 'newdir');
        if (!name) return;
        var id = this.id();
        this.request({ op: 'mkdir', id: id, path: join(this.cwd, name) }, 'mkdir')
            .then(function () { self.load(self.cwd); })
            .catch(function (e) { self.toast(e.message, true); });
    };

    Manager.prototype.touch = function () {
        var self = this;
        var name = global.prompt('新建文件名称：', 'new.txt');
        if (!name) return;
        var id = this.id();
        this.request({ op: 'touch', id: id, path: join(this.cwd, name) }, 'touch')
            .then(function () { self.load(self.cwd); })
            .catch(function (e) { self.toast(e.message, true); });
    };

    Manager.prototype.rowByName = function (name) {
        var list = this.q.rows.querySelectorAll('tr');
        for (var i = 0; i < list.length; i++) {
            if (list[i].dataset && list[i].dataset.name === name) return list[i];
        }
        return null;
    };

    // 列表内展开的表单（重命名/移动），不用系统弹窗
    Manager.prototype.inlineForm = function (tr, opts) {
        var self = this;
        this.closeInlineForm();
        if (!tr || !tr.parentNode) return;

        var formTr = document.createElement('tr');
        formTr.className = 'formrow';
        var td = document.createElement('td');
        td.colSpan = 5;
        var box = document.createElement('div');
        box.className = 'iform';

        var lb = document.createElement('span');
        lb.className = 'lb';
        lb.textContent = opts.label;

        var inp = document.createElement('input');
        inp.type = 'text';
        inp.spellcheck = false;
        inp.autocomplete = 'off';
        inp.value = opts.value || '';
        if (opts.placeholder) inp.placeholder = opts.placeholder;

        var ok = document.createElement('button');
        ok.className = 'primary';
        ok.textContent = '确定';
        var no = document.createElement('button');
        no.textContent = '取消';

        box.appendChild(lb);
        box.appendChild(inp);
        box.appendChild(ok);
        box.appendChild(no);
        td.appendChild(box);
        formTr.appendChild(td);
        tr.parentNode.insertBefore(formTr, tr.nextSibling);
        tr.classList.add('editing');
        this._form = { tr: tr, row: formTr };

        var close = function () { self.closeInlineForm(); };
        var busy = function (on) {
            inp.disabled = on;
            ok.disabled = on;
            no.disabled = on;
        };
        var submit = function () {
            var v = inp.value.trim();
            if (!v) { inp.focus(); return; }
            busy(true);
            opts.onOk(v, function (err, msg) {
                if (err) {
                    busy(false);
                    inp.focus();
                    self.toast(msg || (err && err.message) || '操作失败', true);
                    return;
                }
                self.closeInlineForm();
                if (msg) self.toast(msg);
            });
        };

        ok.addEventListener('click', submit);
        no.addEventListener('click', close);
        inp.addEventListener('keydown', function (e) {
            if (e.key === 'Enter') { e.preventDefault(); submit(); }
            else if (e.key === 'Escape') { e.preventDefault(); e.stopPropagation(); close(); }
        });

        setTimeout(function () {
            inp.focus();
            var dot = opts.selectBase ? inp.value.lastIndexOf('.') : -1;
            if (dot > 0) inp.setSelectionRange(0, dot);
            else inp.select();
        }, 0);
    };

    Manager.prototype.closeInlineForm = function () {
        if (!this._form) return;
        if (this._form.tr) this._form.tr.classList.remove('editing');
        if (this._form.row && this._form.row.parentNode) this._form.row.parentNode.removeChild(this._form.row);
        this._form = null;
    };

    Manager.prototype.rename = function (oldName, path) {
        var self = this;
        var tr = this.rowByName(oldName);
        if (!tr) return;
        this.inlineForm(tr, {
            label: '重命名为',
            value: oldName,
            selectBase: true,
            onOk: function (name, done) {
                if (name === oldName) { done(null); return; }
                if (name.indexOf('/') >= 0) { done(new Error('名称不能包含 /')); return; }
                self.request({ op: 'rename', id: self.id(), path: path, name: name }, 'rename')
                    .then(function () {
                        self.load(self.cwd);
                        done(null, '已重命名 ' + oldName + ' → ' + name);
                    })
                    .catch(function (e) { done(e); });
            }
        });
    };

    Manager.prototype.move = function (name, path) {
        var self = this;
        var tr = this.rowByName(name);
        if (!tr) return;
        var cur = parentOf(path);
        this.inlineForm(tr, {
            label: '移动到',
            value: this.cwd,
            placeholder: '目标目录绝对路径',
            onOk: function (dest, done) {
                if (dest.length > 1 && dest.charAt(dest.length - 1) === '/') dest = dest.replace(/\/+$/, '');
                if (dest === cur) { done(null); return; }
                if (dest.charAt(0) !== '/') { done(new Error('请输入绝对路径')); return; }
                self.request({ op: 'move', id: self.id(), path: path, dir: dest }, 'move')
                    .then(function () {
                        self.load(self.cwd);
                        done(null, '已移动 ' + name);
                    })
                    .catch(function (e) { done(e); });
            }
        });
    };

    Manager.prototype.remove = function (name, isDir, path) {
        var self = this;
        if (!global.confirm('删除 ' + name + (isDir ? ' 及其全部内容' : '') + '？')) return;
        var id = this.id();
        this.request({ op: 'delete', id: id, path: path, recursive: !!isDir }, 'delete')
            .then(function () { self.load(self.cwd); })
            .catch(function (e) { self.toast(e.message, true); });
    };

    Manager.prototype.removeSelected = function () {
        var self = this;
        if (this.readonly || !this.rw) return;
        if (!this.sel.size) { this.toast('请先选择文件'); return; }
        var names = Array.from(this.sel);
        if (!global.confirm('删除选中的 ' + names.length + ' 项？目录会连同内容一起删除。')) return;
        var chain = Promise.resolve();
        var failed = 0;
        names.forEach(function (n) {
            chain = chain.then(function () {
                var en = self.entries.find(function (x) { return x.name === n; });
                var id = self.id();
                return self.request({ op: 'delete', id: id, path: join(self.cwd, n), recursive: !!(en && en.type === 'dir') }, 'delete')
                    .catch(function (e) { failed++; self.toast(e.message, true); });
            });
        });
        chain.then(function () { self.load(self.cwd); if (!failed) self.toast('已删除 ' + names.length + ' 项'); });
    };

    // ------------------------------------------------------------ 上传/下载

    Manager.prototype.download = function (path, onProgress) {
        var self = this;
        if (this._dlBusy) return Promise.reject(new Error('已有下载任务进行中'));
        this._dlBusy = true;
        var id = this.id();
        return this.request({ op: 'download_open', id: id, path: path }, 'download_meta')
            .then(function (meta) {
                var chunks = [], got = 0;
                function step() {
                    if (got >= meta.size) return Promise.resolve({ meta: meta, blob: new Blob(chunks) });
                    return self.nextBinary(id).then(function (bin) {
                        chunks.push(bin.data);
                        got += bin.data.length;
                        if (onProgress) onProgress(got, meta.size);
                        self.ws.send(JSON.stringify({ op: 'download_ack', id: id, bytes: bin.data.length }));
                        return step();
                    });
                }
                return step().then(function (r) {
                    if (meta.size === 0) return { meta: meta, blob: new Blob([]) };
                    return r;
                });
            })
            .finally(function () { self._dlBusy = false; });
    };

    Manager.prototype.downloadToFile = function (path) {
        var self = this;
        this.toast('开始下载 ' + path.split('/').pop());
        this.download(path, function (got, total) {
            self.q.summary.textContent = '下载中 ' + Math.floor(got * 100 / total) + '% (' + fmtSize(got) + ' / ' + fmtSize(total) + ')';
        }).then(function (r) {
            var a = document.createElement('a');
            a.href = URL.createObjectURL(r.blob);
            a.download = r.meta.name || 'download';
            document.body.appendChild(a);
            a.click();
            setTimeout(function () { URL.revokeObjectURL(a.href); a.remove(); }, 10000);
            self.toast('已下载 ' + a.download);
        }).catch(function (e) { self.toast(e.message, true); })
          .finally(function () { self.render(); });
    };

    // 目录打包成 tar.gz 边压边传；支持 File System Access API 时直接写入磁盘，否则回退 Blob。
    Manager.prototype.downloadArchive = function (path) {
        var self = this;
        if (this._dlBusy) { this.toast('已有下载任务进行中', true); return; }
        var id = this.id();
        var base = path === '/' ? 'root' : path.split('/').pop();
        var writable = null;
        var picker = null;
        if (global.showSaveFilePicker) {
            try {
                picker = global.showSaveFilePicker({
                    suggestedName: base + '.tar.gz',
                    types: [{ description: 'gzip 压缩包', accept: { 'application/gzip': ['.tar.gz', '.tgz'] } }]
                });
            } catch (e) { picker = null; }
        }
        this._dlBusy = true;
        this.toast('开始打包 ' + base + ' …');
        return Promise.resolve(picker).catch(function (e) {
            if (e && e.name === 'AbortError') return 'abort';
            return null;
        }).then(function (handle) {
            if (handle === 'abort') return;
            return self.request({ op: 'archive_open', id: id, path: path }, 'archive_meta').then(function (meta) {
                var prep = handle ? Promise.resolve(handle.createWritable()).then(function (w) { writable = w; }) : Promise.resolve();
                return prep.then(function () {
                    var chunks = [], got = 0;
                    function ack(n) {
                        try { self.ws.send(JSON.stringify({ op: 'download_ack', id: id, bytes: n })); } catch (e) {}
                    }
                    function finish() {
                        if (writable) return writable.close().then(function () { self.toast('已下载 ' + meta.name); });
                        var blob = new Blob(chunks, { type: 'application/gzip' });
                        var a = document.createElement('a');
                        a.href = URL.createObjectURL(blob);
                        a.download = meta.name || (base + '.tar.gz');
                        document.body.appendChild(a);
                        a.click();
                        setTimeout(function () { URL.revokeObjectURL(a.href); a.remove(); }, 10000);
                        self.toast('已下载 ' + a.download);
                    }
                    function step() {
                        return self.nextBinary(id).then(function (bin) {
                            if (bin.flags & 1) return finish();
                            got += bin.data.length;
                            self.q.summary.textContent = '打包下载中 ' + fmtSize(got);
                            ack(bin.data.length);
                            if (writable) return writable.write(bin.data).then(step);
                            chunks.push(bin.data);
                            return step();
                        });
                    }
                    return step();
                });
            });
        }).catch(function (e) {
            if (e && e.name === 'AbortError') return;
            self.toast('打包下载失败：' + (e.message || e), true);
            if (self.ws && self.connected) {
                try { self.ws.send(JSON.stringify({ op: 'download_cancel', id: id })); } catch (x) {}
            }
            if (writable) { try { writable.abort(); } catch (x) {} }
        }).finally(function () {
            self._dlBusy = false;
            self.render();
        });
    };

    Manager.prototype.uploadFiles = function (files, dir) {
        var self = this;
        if (this.readonly || !this.rw || !files || !files.length) return;
        var arr = Array.from(files);
        var i = 0;
        function next() {
            if (i >= arr.length || self.uploadCancel) {
                self.uploadCancel = false;
                self.q.upbox.className = 'up';
                self.load(self.cwd);
                return;
            }
            var f = arr[i++];
            self.uploadOne(f, dir).then(next).catch(function (e) {
                self.toast(f.name + ': ' + e.message, true);
                next();
            });
        }
        next();
    };

    Manager.prototype.uploadOne = function (file, dir) {
        var self = this;
        this.uploadCancel = false;
        this.q.upbox.className = 'up show';
        this.q.upname.textContent = file.name;
        this.q.upbar.style.width = '0%';
        this.q.uppct.textContent = '0%';
        var id = this.id();
        var uploadId = null, offset = 0;
        var upKey = 'rtfs.up:' + this.wsUrl + ':' + dir + '/' + file.name + ':' + file.size;
        var resume = '';
        try { resume = localStorage.getItem(upKey) || ''; } catch (e) {}
        return this.request({ op: 'upload_open', id: id, dir: dir, name: file.name, size: file.size,
                              overwrite: true, upload_id: resume }, 'upload_ready')
            .then(function (r) {
                uploadId = r.upload_id;
                offset = r.offset || 0;
                try { localStorage.setItem(upKey, uploadId); } catch (e) {}
                var chunk = r.chunk || 30720;
                function step() {
                    if (self.uploadCancel) {
                        return self.request({ op: 'upload_cancel', id: self.id(), upload_id: uploadId }, 'upload_cancelled')
                            .then(function () {
                                try { localStorage.removeItem(upKey); } catch (e) {}
                                throw new Error('已取消');
                            });
                    }
                    if (offset >= file.size) return Promise.resolve();
                    var end = Math.min(offset + chunk, file.size);
                    return file.slice(offset, end).arrayBuffer().then(function (buf) {
                        self.sendBinary(id, 2, offset, new Uint8Array(buf));
                        return self.waitMsg(id, 'upload_ack').then(function (ack) {
                            offset = ack.received;
                            var pct = file.size ? Math.floor(offset * 100 / file.size) : 100;
                            self.q.upbar.style.width = pct + '%';
                            self.q.uppct.textContent = pct + '%';
                            return step();
                        });
                    });
                }
                return step().then(function () {
                    var fid = self.id();
                    return self.request({ op: 'upload_finish', id: fid, upload_id: uploadId, path: join(dir, file.name) }, 'upload_done');
                });
            })
            .then(function () {
                try { localStorage.removeItem(upKey); } catch (e) {}
                self.q.upbox.className = 'up';
                self.toast('已上传 ' + file.name);
            })
            .catch(function (e) {
                self.q.upbox.className = 'up';
                throw e;
            });
    };

    // ------------------------------------------------------------ 编辑/预览

    Manager.prototype.openEditor = function (path, text) {
        var self = this;
        var name = path.split('/').pop();
        if (text !== undefined) { this.showEditor(path, name, text); return; }
        this.toast('读取 ' + name + ' ...');
        this.request({ op: 'stat', id: this.id(), path: path }, 'stat').then(function (st) {
            if (st.entry.size > 1024 * 1024) throw new Error('文件超过 1 MiB，请下载后编辑');
            return self.download(path).then(function (r) {
                return r.blob.text().then(function (t) { self.showEditor(path, r.meta.name, t); });
            });
        }).catch(function (e) { self.toast(e.message, true); });
    };

    Manager.prototype.showEditor = function (path, name, text) {
        this.editing = { path: path, name: name };
        this.q.edTitle.textContent = '编辑 - ' + path;
        this.q.edTree.hidden = ext(name) !== 'json';
        this.q.editor.hidden = false;
        this.mountEditor(text, name);
    };

    Manager.prototype.mountEditor = function (text, name) {
        var self = this;
        this.destroyEditorInst();
        this.q.edText.hidden = false;
        this.q.edText.value = text;
        var tok = this.editorToken;
        this.ensureEditorLib().then(function (factory) {
            if (!factory || tok !== self.editorToken || !self.editing) return;
            var inst = factory(self.q.edHost, {
                value: text,
                language: langOf(name),
                onSave: function () { self.saveEditor(); },
                onEscape: function () { self.closeEditor(); }
            });
            if (tok !== self.editorToken || !self.editing) { try { inst.destroy(); } catch (e) {} return; }
            self.editorInst = inst;
            self.q.edText.hidden = true;
        }).catch(function () { /* 组件不可用时继续用 textarea */ });
    };

    Manager.prototype.destroyEditorInst = function () {
        this.editorToken++;
        if (this.editorInst) {
            try { this.editorInst.destroy(); } catch (e) {}
            this.editorInst = null;
        }
    };

    Manager.prototype.ensureEditorLib = function () {
        var self = this;
        if (this.opts.createEditor) return Promise.resolve(this.opts.createEditor);
        if (global.hvCreateEditor) return Promise.resolve(global.hvCreateEditor);
        if (this._editorLib) return this._editorLib;
        this._editorLib = new Promise(function (resolve, reject) {
            var s = document.createElement('script');
            s.src = self.opts.editorUrl || '/assets/cm-editor.js';
            s.async = true;
            s.onload = function () { resolve(global.hvCreateEditor || null); };
            s.onerror = function () { self._editorLib = null; reject(new Error('编辑器组件加载失败')); };
            document.head.appendChild(s);
        });
        return this._editorLib;
    };

    Manager.prototype.closeEditor = function () {
        this.editing = null;
        this.destroyEditorInst();
        this.q.editor.hidden = true;
        this.q.edText.value = '';
    };

    Manager.prototype.saveEditor = function () {
        var self = this;
        if (!this.editing) return;
        if (this.readonly || !this.rw) { this.toast('只读模式', true); return; }
        var text = this.editorInst ? this.editorInst.getValue() : this.q.edText.value;
        var blob = new Blob([text], { type: 'text/plain' });
        var name = this.editing.name;
        var dir = parentOf(this.editing.path);
        this.q.edSave.disabled = true;
        this.uploadBlob(blob, name, dir).then(function () {
            self.toast('已保存 ' + name);
            self.closeEditor();
            self.load(self.cwd);
        }).catch(function (e) {
            self.toast(e.message, true);
        }).finally(function () { self.q.edSave.disabled = false; });
    };

    Manager.prototype.openJsonView = function (path, text) {
        var self = this;
        var name = path.split('/').pop();
        if (text !== undefined) { this.showJsonView(path, text); return; }
        this.toast('读取 ' + name + ' ...');
        this.request({ op: 'stat', id: this.id(), path: path }, 'stat').then(function (st) {
            if (st.entry.size > 1024 * 1024) throw new Error('文件超过 1 MiB，请下载后查看');
            return self.download(path).then(function (r) {
                return r.blob.text().then(function (t) { self.showJsonView(path, t); });
            });
        }).catch(function (e) { self.toast(e.message, true); });
    };

    Manager.prototype.showJsonView = function (path, text) {
        this.jvPath = path;
        this.jvText = text;
        this.q.jvTitle.textContent = '查看 - ' + path;
        this.renderJsonView(2, 5000);
        this.q.jsonview.hidden = false;
    };

    Manager.prototype.renderJsonView = function (expandDepth, budget) {
        var body = this.q.jvBody;
        while (body.firstChild) body.removeChild(body.firstChild);
        var data;
        try {
            data = JSON.parse(this.jvText);
        }
        catch (e) {
            var err = document.createElement('div');
            err.className = 'jerr';
            err.textContent = 'JSON 解析失败：' + e.message;
            body.appendChild(err);
            var tip = document.createElement('div');
            tip.className = 'jm';
            tip.style.marginTop = '10px';
            tip.textContent = '可点「源码」切换到文本模式查看。';
            body.appendChild(tip);
            this.jvData = null;
            return;
        }
        this.jvData = data;
        body.appendChild(buildJsonTree(data, { expandDepth: expandDepth, budget: budget }));
    };

    Manager.prototype.closeJsonView = function () {
        this.q.jsonview.hidden = true;
        while (this.q.jvBody.firstChild) this.q.jvBody.removeChild(this.q.jvBody.firstChild);
        this.jvPath = '';
        this.jvText = '';
        this.jvData = null;
    };

    Manager.prototype.uploadBlob = function (blob, name, dir) {
        var f;
        try { f = new File([blob], name); } catch (e) {
            f = blob; f.name = name;
        }
        return this.uploadOne(f, dir);
    };

    Manager.prototype.openPreview = function (path) {
        var self = this;
        this.download(path).then(function (r) {
            var type = r.blob.type || '';
            if (!type && isImage(r.meta.name)) {
                var e = ext(r.meta.name);
                type = 'image/' + (e === 'svg' ? 'svg+xml' : e === 'jpg' ? 'jpeg' : e);
            }
            self.closePreview();
            self.previewUrl = URL.createObjectURL(new Blob([r.blob], { type: type }));
            self.q.pvTitle.textContent = '预览 - ' + path;
            self.q.pvBody.innerHTML = '';
            var img = document.createElement('img');
            img.src = self.previewUrl;
            img.alt = r.meta.name;
            self.q.pvBody.appendChild(img);
            self.q.preview.hidden = false;
        }).catch(function (e) { self.toast(e.message, true); });
    };

    Manager.prototype.closePreview = function () {
        if (this.previewUrl) { URL.revokeObjectURL(this.previewUrl); this.previewUrl = null; }
        this.q.preview.hidden = true;
        this.q.pvBody.innerHTML = '';
    };

    global.RtunnelFs = {
        version: 1,
        Manager: Manager,
        createManager: function (opts) { return new Manager(opts); },
        fmtSize: fmtSize,
        fmtTime: fmtTime
    };
})(window);
