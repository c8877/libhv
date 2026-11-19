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
*[hidden]{display:none!important}
.overlay{position:fixed;inset:0;background:rgba(15,18,24,.62);display:flex;align-items:center;justify-content:center;
  font:14px/1.45 -apple-system,BlinkMacSystemFont,"Segoe UI","PingFang SC","Microsoft YaHei",sans-serif;
  color:var(--fg);z-index:2147483000;padding:18px}
.overlay[hidden]{display:none}
.overlay.inline{position:static;inset:auto;display:block;background:transparent;padding:0;z-index:auto}
.overlay.inline[hidden]{display:none}
.overlay.inline .win{width:100%;height:min(560px,70vh);box-shadow:none}
.overlay.inline header{display:none}
.win:fullscreen{width:100vw;height:100vh;border:0;border-radius:0}
.overlay.inline .win:fullscreen{width:100vw;height:100vh}
.win:-webkit-full-screen{width:100vw;height:100vh;border:0;border-radius:0}
.overlay.inline .win:-webkit-full-screen{width:100vw;height:100vh}
.win{width:min(1080px,100%);height:min(760px,100%);background:var(--bg);border:1px solid var(--line);border-radius:14px;
  display:flex;flex-direction:column;overflow:hidden;box-shadow:0 24px 70px rgba(0,0,0,.45)}
header{display:flex;align-items:center;gap:10px;padding:12px 16px;border-bottom:1px solid var(--line);background:var(--panel)}
header .t{font-weight:650;font-size:15px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
header .sub{color:var(--muted);font-size:12px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
header .grow{flex:1;min-width:0}
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
.note{display:block;min-width:0;max-width:100%;padding-left:10px;margin-left:2px;border-left:1px solid var(--line);
  line-height:20px;color:var(--muted);font-size:12px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.note.warn{color:#c98a12}
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
.ibtn{display:inline-flex;align-items:center;justify-content:center;width:28px;height:28px;padding:0;border:0;border-radius:7px;
  background:transparent;color:var(--fg);flex:none;cursor:pointer}
.ibtn:hover:not(:disabled){background:var(--hover)}
.ibtn.primary{background:var(--accent);color:#fff}
.ibtn.primary:hover:not(:disabled){background:var(--accent);filter:brightness(1.07)}
.ibtn.on{color:var(--accent)}
.ibtn.danger{color:#ef4444}
.ibtn svg{width:16px;height:16px;stroke:currentColor;fill:none;stroke-width:1.6;stroke-linecap:round;stroke-linejoin:round}
.ibtn[hidden]{display:none}
.bar .spacer{flex:1;min-width:6px}
.tico{display:inline-flex;flex:none;color:var(--muted)}
.tico svg{width:15px;height:15px;stroke:currentColor;fill:none;stroke-width:1.6;stroke-linecap:round;stroke-linejoin:round}
.tico.dir{color:#e0a33a}
.tico.img{color:#a855f7}
.tico.lnk{color:#f59e0b}
.name .tw{flex:none;width:16px;height:16px;border:0;background:transparent;padding:0;text-align:center;color:var(--muted);
  cursor:pointer;user-select:none;font-size:11px;line-height:1;display:inline-flex;align-items:center;justify-content:center}
.name .tw:hover{background:transparent;color:var(--fg)}
.name .tw::before{content:'';width:0;height:0;border-left:4px solid currentColor;border-top:3.5px solid transparent;
  border-bottom:3.5px solid transparent;transition:transform .12s;transform-origin:35% 50%}
.name .tw.open::before{transform:rotate(90deg)}
.name .tw.ph{visibility:hidden}
.name .ind{flex:none;height:1px}
.name .iming{display:none;margin-left:auto;padding-left:8px;color:var(--muted);font-size:11px;white-space:nowrap}
tbody td.dim{color:var(--muted);font-size:12px;white-space:nowrap}
table.tree{table-layout:fixed}
table.tree td:nth-child(1),table.tree th:nth-child(1){width:32px}
table.tree td:nth-child(3),table.tree th:nth-child(3){width:96px}
table.tree td:nth-child(4),table.tree th:nth-child(4){width:136px}
table.tree td:nth-child(5),table.tree th:nth-child(5){width:52px}
table.tree .name{min-width:0}
table.tree .name .nm{flex:1;min-width:0;max-width:none}
table.tree td{overflow:hidden}
tr.tree-note td{color:var(--muted);font-size:12px;font-style:italic}
tr.tree-note .note{padding:2px 0}
.formrow td{padding:0;background:var(--panel);border-bottom:1px solid var(--line)}
.iform{display:flex;align-items:center;gap:8px;flex-wrap:wrap;padding:9px 12px 10px 34px}
.iform .lb{color:var(--muted);font-size:12px;white-space:nowrap}
.iform input:not([type=checkbox]):not([type=radio]){flex:1;min-width:180px;max-width:560px;border:1px solid var(--line);
  background:var(--input);color:var(--fg);border-radius:8px;padding:6px 9px;
  font:12.5px ui-monospace,SFMono-Regular,Menlo,monospace;outline:none}
.iform input:not([type=checkbox]):not([type=radio]):focus{border-color:var(--accent)}
.iform input[type=checkbox],.iform input[type=radio]{flex:none;width:auto;min-width:0;margin:0;accent-color:var(--accent)}
.iform .sp{flex:1}
.iform select{border:1px solid var(--line);background:var(--input);color:var(--fg);border-radius:8px;padding:6px 8px;
  font:12.5px ui-monospace,SFMono-Regular,Menlo,monospace;max-width:280px}
.iform .row{display:flex;align-items:center;gap:8px;flex-wrap:wrap;width:100%}
.iform .row .lb{min-width:64px}
.iform .warn{color:#f59e0b;font-size:12px}
.iform .hint,.iform .err{font-size:12px}
.iform .hint{color:var(--muted)}
.iform .err{color:#ef4444}
.iform .prog{flex:1;min-width:100px;max-width:300px;height:6px;border-radius:4px;background:var(--line);overflow:hidden}
.iform .prog i{display:block;height:100%;width:0;background:var(--accent);transition:width .12s}
.ifile{display:flex;flex-direction:column;align-items:stretch;gap:8px;padding:10px 12px 11px 34px}
.ifile .top{display:flex;align-items:center;gap:8px;flex-wrap:wrap;width:100%;min-width:0}
.ifile .kind{flex:none;font-size:11px;line-height:18px;padding:0 6px;border-radius:6px;
  border:1px solid var(--line);background:var(--input);color:var(--muted)}
.ifile .nm{font:600 13px ui-monospace,SFMono-Regular,Menlo,monospace;color:var(--fg);word-break:break-all}
.ifile .top .sp{flex:1}
.ifile .grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(190px,1fr));gap:6px 20px;width:100%}
.ifile .cell{display:flex;gap:8px;font-size:12.5px;min-width:0}
.ifile .cell.wide{grid-column:1/-1}
.ifile .cell .k{flex:none;min-width:56px;color:var(--muted)}
.ifile .cell .v{min-width:0;color:var(--fg);word-break:break-all}
.ifile .acts{display:flex;gap:8px;flex-wrap:wrap;width:100%}
.iconf{display:flex;align-items:center;gap:8px;flex-wrap:wrap;padding:9px 12px 10px 34px}
.iconf .tx{font-size:13px;color:var(--fg)}
.iconf .tx b{font-weight:600}
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
.dest{margin-left:auto;min-width:0;white-space:nowrap;overflow:hidden;text-overflow:ellipsis;color:var(--muted)}
.dest:empty{display:none}
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
.mfoot{display:flex;justify-content:flex-end;gap:8px;padding:9px 12px;border-top:1px solid var(--line);background:var(--panel)}
.pv{flex:1;display:flex;align-items:center;justify-content:center;overflow:auto;background:var(--input);padding:12px}
.pv img{max-width:100%;max-height:100%;object-fit:contain}
.ed{flex:1;min-height:0;display:flex;overflow:hidden}
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
  .bar{gap:4px;padding:6px 8px}
  .bar .ibtn{width:30px;height:30px}
  .path{min-width:100px}
  .search{width:110px;flex:0 1 150px;min-width:90px}
  .meta{display:none}
  .tools .spacer{display:none}
  .overlay.inline #fullscreen{display:none}
  th:nth-child(3),td:nth-child(3),th:nth-child(4),td:nth-child(4){display:none}
  .name .nm{max-width:150px}
  table.tree .name .nm{max-width:none}
  table.tree td:nth-child(5),table.tree th:nth-child(5){width:44px}
  table.tree td:nth-child(1),table.tree th:nth-child(1){width:28px}
  .name .iming{display:inline;padding-left:6px;font-size:10.5px}
  .name .iming .itm{display:none}
  tbody td{padding:7px 6px}
  .iform{padding:8px 8px 9px 10px}
  .iform input{min-width:120px}
  .ifile{padding:9px 8px 10px 10px}
  .ifile .grid{grid-template-columns:1fr}
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


    var ICONS = {
        mkdir: '<svg viewBox="0 0 24 24"><path d="M3 7a2 2 0 0 1 2-2h4l2 2h8a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z"/><path d="M12 11v6M9 14h6"/></svg>',
        touch: '<svg viewBox="0 0 24 24"><path d="M14 3H7a2 2 0 0 0-2 2v14a2 2 0 0 0 2 2h10a2 2 0 0 0 2-2V8z"/><path d="M14 3v5h5"/><path d="M12 12v5M9.5 14.5h5"/></svg>',
        upload: '<svg viewBox="0 0 24 24"><path d="M12 16V4"/><path d="m7 9 5-5 5 5"/><path d="M20 16v3a2 2 0 0 1-2 2H6a2 2 0 0 1-2-2v-3"/></svg>',
        pack: '<svg viewBox="0 0 24 24"><path d="m12 3 8 4.5v9L12 21l-8-4.5v-9z"/><path d="M4 7.5l8 4.5 8-4.5M12 12v9"/></svg>',
        del: '<svg viewBox="0 0 24 24"><path d="M3 6h18"/><path d="M8 6V4h8v2"/><path d="M6 6l1 14h10l1-14"/><path d="M10 10v6M14 10v6"/></svg>',
        refresh: '<svg viewBox="0 0 24 24"><path d="M20 11a8 8 0 1 0-2.3 5.7"/><path d="M20 4v7h-7"/></svg>',
        pencil: '<svg viewBox="0 0 24 24"><path d="M4 20h4L19 9l-4-4L4 16z"/><path d="m13 5 4 4"/></svg>',
        tree: '<svg viewBox="0 0 24 24"><path d="M5 4v16"/><path d="M5 8h5M10 8v4h5M10 12v6h5"/></svg>',
        list: '<svg viewBox="0 0 24 24"><path d="M4 6h2M4 12h2M4 18h2"/><path d="M10 6h10M10 12h10M10 18h10"/></svg>',
        expandAll: '<svg viewBox="0 0 24 24"><path d="m7 6 5 5 5-5"/><path d="m7 13 5 5 5-5"/></svg>',
        collapseAll: '<svg viewBox="0 0 24 24"><path d="m7 18 5-5 5 5"/><path d="m7 11 5-5 5 5"/></svg>',
        fullscreen: '<svg viewBox="0 0 24 24"><path d="M8 3H5a2 2 0 0 0-2 2v3"/><path d="M16 3h3a2 2 0 0 1 2 2v3"/><path d="M8 21H5a2 2 0 0 1-2-2v-3"/><path d="M16 21h3a2 2 0 0 0 2-2v-3"/></svg>',
        exitFullscreen: '<svg viewBox="0 0 24 24"><path d="M8 3v3a2 2 0 0 1-2 2H3"/><path d="M21 8h-3a2 2 0 0 1-2-2V3"/><path d="M3 16h3a2 2 0 0 1 2 2v3"/><path d="M16 21v-3a2 2 0 0 1 2-2h3"/></svg>',
        eye: '<svg viewBox="0 0 24 24"><path d="M2 12s3.5-6 10-6 10 6 10 6-3.5 6-10 6S2 12 2 12z"/><circle cx="12" cy="12" r="2.6"/></svg>',
        folder: '<svg viewBox="0 0 24 24"><path d="M3 7a2 2 0 0 1 2-2h4l2 2h8a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z"/></svg>',
        file: '<svg viewBox="0 0 24 24"><path d="M14 3H7a2 2 0 0 0-2 2v14a2 2 0 0 0 2 2h10a2 2 0 0 0 2-2V8z"/><path d="M14 3v5h5"/></svg>',
        image: '<svg viewBox="0 0 24 24"><rect x="3" y="4" width="18" height="16" rx="2"/><circle cx="9" cy="9" r="1.6"/><path d="m4 17 5-5 4 4 2-2 5 4"/></svg>',
        link: '<svg viewBox="0 0 24 24"><path d="M10 13a5 5 0 0 0 7 0l2-2a5 5 0 0 0-7-7l-1 1"/><path d="M14 11a5 5 0 0 0-7 0l-2 2a5 5 0 0 0 7 7l1-1"/></svg>'
    };

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
      <button class="ibtn" id="editPath" title="输入路径" aria-label="输入路径">${ICONS.pencil}</button>
      <button class="ibtn" id="refresh" title="刷新" aria-label="刷新">${ICONS.refresh}</button>
      <button class="ibtn" id="viewToggle" title="切换为列表视图" aria-label="切换视图">${ICONS.list}</button>
      <button class="ibtn" id="expandAll" title="全部展开" aria-label="全部展开" hidden>${ICONS.expandAll}</button>
      <button class="ibtn" id="collapseAll" title="全部折叠" aria-label="全部折叠" hidden>${ICONS.collapseAll}</button>
    </div>
    <div class="bar tools">
      <button class="ibtn" id="mkdir" title="新建文件夹" aria-label="新建文件夹">${ICONS.mkdir}</button>
      <button class="ibtn" id="touch" title="新建文件" aria-label="新建文件">${ICONS.touch}</button>
      <button class="ibtn primary" id="upload" title="上传到当前目录" aria-label="上传">${ICONS.upload}</button>
      <button class="ibtn" id="pack" title="打包下载" aria-label="打包下载" hidden>${ICONS.pack}</button>
      <button class="ibtn danger" id="del" title="删除选中" aria-label="删除选中">${ICONS.del}</button>
      <span class="note" id="note" hidden></span>
      <span class="spacer"></span>
      <input class="search" id="search" placeholder="搜索当前目录">
      <button class="ibtn" id="hiddenBtn" title="显示隐藏文件" aria-label="显示隐藏文件">${ICONS.eye}</button>
      <button class="ibtn" id="fullscreen" title="全屏" aria-label="全屏" hidden>${ICONS.fullscreen}</button>
      <input type="checkbox" id="hidden" hidden>
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
      <div class="dest" id="dest"></div>
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
    <div class="ed" id="edHost"></div>
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
        // 默认走设备 WS 协议；传 backend 时改走自定义传输（如文件仓库 HTTP）
        this.backend = opts.backend || null;
        this.caps = { mkdir: true, touch: true, rename: true, move: true, pack: false, upload: true,
                      del: true, edit: true, rowEdit: false, flat: false, tree: false, hidden: true,
                      dirSize: true, archive: true };
        this.menuExtras = opts.menuExtras || null;
        this.onMenuAction = opts.onMenuAction || null;
        this.wsUrl = opts.wsUrl || ((location.protocol === 'https:' ? 'wss://' : 'ws://') + location.host + '/ws/fs');
        this.token = opts.token || sessionStorage.getItem('rtfs.token:' + this.wsUrl) || '';
        this.title = opts.title || '文件管理';
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
        this.sel = new Set();          // 选中项的绝对路径
        this.selAll = new Set();       // 整棵子树都选中的目录（含尚未加载的子项）
        this.dirSizes = new Map();     // path -> 递归统计结果
        this.dirPending = new Map();
        this.sortKey = 'name';
        this.sortDir = 1;
        this.filter = '';
        this.hidden = false;
        // 树形为默认视图；文件仓库（flat）强制列表
        this.viewMode = opts.viewMode === 'list' ? 'list' : 'tree';
        if (!opts.viewMode) {
            try {
                var savedView = global.localStorage.getItem('rtfs.view');
                if (savedView === 'list' || savedView === 'tree') this.viewMode = savedView;
            } catch (e) { /* 隐私模式下不可用 */ }
        }
        this.expanded = new Set();     // 已展开目录的绝对路径
        this.kids = new Map();         // path -> 子项
        this.loadingKids = new Set();
        this._entryByPath = new Map();
        this._rowItems = [];
        this.visible = false;
        this.uploading = false;
        this.uploadCancel = false;
        this.editing = null;
        this.editorInst = null;
        this.editorToken = 0;
        this._editorLib = null;
        this.jvPath = '';
        this.jvText = '';
        this.previewUrl = null;
        this.reconnectTimer = null;
        this.closedByUser = false;
        this.destroyed = false;

        this._build();
        this._bind();
    }

    Manager.prototype.isTree = function () {
        return this.viewMode === 'tree' && !this.caps.flat && (!this.backend || !!this.caps.tree);
    };

    Manager.prototype.applyViewButton = function () {
        if (!this.q || !this.q.viewToggle) return;
        var tree = this.isTree();
        this.q.viewToggle.innerHTML = tree ? ICONS.list : ICONS.tree;
        this.q.viewToggle.title = tree ? '切换为列表视图' : '切换为树形视图';
        this.q.viewToggle.setAttribute('aria-label', this.q.viewToggle.title);
        this.syncTreeTools();
    };

    // 树形专用的「全部展开/折叠」只在当前确实是树形视图时出现
    Manager.prototype.syncTreeTools = function () {
        if (!this.q || !this.q.expandAll) return;
        var show = this.isTree();
        this.q.expandAll.hidden = !show;
        this.q.collapseAll.hidden = !show;
    };

    Manager.prototype.isFullscreen = function () {
        var d = document;
        var el = d.fullscreenElement || d.webkitFullscreenElement || d.msFullscreenElement;
        return !!(el && this.q && el === this.q.win);
    };

    Manager.prototype.syncFullscreenBtn = function () {
        var q = this.q;
        if (!q || !q.fullscreen || !q.win) return;
        var supported = !!(q.win.requestFullscreen || q.win.webkitRequestFullscreen || q.win.msRequestFullscreen);
        q.fullscreen.hidden = !supported;
        var on = this.isFullscreen();
        q.fullscreen.innerHTML = on ? ICONS.exitFullscreen : ICONS.fullscreen;
        var t = on ? '退出全屏' : '全屏';
        q.fullscreen.title = t;
        q.fullscreen.setAttribute('aria-label', t);
        q.fullscreen.classList.toggle('on', on);
    };

    Manager.prototype.toggleFullscreen = function () {
        var self = this, d = document, el = this.q && this.q.win;
        if (!el) return;
        if (this.isFullscreen()) {
            var exit = d.exitFullscreen || d.webkitExitFullscreen || d.msExitFullscreen;
            if (!exit) return;
            try {
                var p = exit.call(d);
                if (p && p.catch) p.catch(function () {});
            } catch (e) { /* ignore */ }
            return;
        }
        var req = el.requestFullscreen || el.webkitRequestFullscreen || el.msRequestFullscreen;
        if (!req) { this.toast('当前浏览器不支持全屏', true); return; }
        try {
            var pr = req.call(el);
            if (pr && pr.catch) pr.catch(function () { self.toast('无法进入全屏', true); });
        } catch (e) {
            this.toast('无法进入全屏', true);
        }
    };

    // 展开当前目录（或指定目录）下的整棵子树；目录过多时按上限截断
    Manager.prototype.expandAll = function () {
        this.expandSubtree(null);
    };

    Manager.prototype.expandSubtree = function (root) {
        var self = this;
        if (!this.isTree() || this._treeBusy) return;
        this._treeBusy = true;
        var seq = this._treeSeq = (this._treeSeq || 0) + 1;
        var MAX_DIRS = 800;
        var dirs = 0, truncated = false;

        function walk(path) {
            if (seq !== self._treeSeq) return Promise.resolve();
            if (dirs >= MAX_DIRS) { truncated = true; return Promise.resolve(); }
            dirs++;
            self.expanded.add(path);
            if (dirs % 10 === 0) self.render();
            var load = self.kids.has(path) ? Promise.resolve() : self.fetchKids(path, true).catch(function () {});
            return load.then(function () {
                if (seq !== self._treeSeq) return;
                var kids = self.kids.get(path) || [];
                var chain = Promise.resolve();
                for (var i = 0; i < kids.length; i++) {
                    if (kids[i].type !== 'dir') continue;
                    (function (p) {
                        chain = chain.then(function () { return walk(p); });
                    })(join(path, kids[i].name));
                }
                return chain;
            });
        }

        var chain = Promise.resolve();
        if (root) {
            chain = walk(root);
        }
        else {
            for (var i = 0; i < this.entries.length; i++) {
                if (this.entries[i].type !== 'dir') continue;
                (function (p) {
                    chain = chain.then(function () { return walk(p); });
                })(join(this.cwd, this.entries[i].name));
            }
        }
        chain.then(function () {
            if (seq !== self._treeSeq) return;
            self._treeBusy = false;
            self.render();
            if (truncated) self.toast('目录过多，已展开 ' + MAX_DIRS + ' 个');
        }).catch(function () {
            if (seq !== self._treeSeq) return;
            self._treeBusy = false;
            self.render();
        });
    };

    Manager.prototype.collapseAll = function () {
        if (!this.isTree()) return;
        this._treeSeq = (this._treeSeq || 0) + 1;
        this._treeBusy = false;
        this.expanded.clear();
        this.render();
    };

    Manager.prototype.collapseSubtree = function (path) {
        var self = this;
        this._treeSeq = (this._treeSeq || 0) + 1;
        this._treeBusy = false;
        (function close(p) {
            var kids = self.kids.get(p) || [];
            for (var i = 0; i < kids.length; i++) {
                if (kids[i].type === 'dir') close(join(p, kids[i].name));
            }
            self.expanded.delete(p);
        })(path);
        this.render();
    };

    Manager.prototype.setView = function (mode) {
        this.viewMode = mode === 'list' ? 'list' : 'tree';
        try { global.localStorage.setItem('rtfs.view', this.viewMode); } catch (e) { /* ignore */ }
        this.applyViewButton();
        this.render();
    };

    Manager.prototype.syncHiddenBtn = function () {
        if (!this.q || !this.q.hiddenBtn) return;
        this.q.hiddenBtn.classList.toggle('on', !!this.hidden);
        var t = this.hidden ? '不显示隐藏文件' : '显示隐藏文件';
        this.q.hiddenBtn.title = t;
        this.q.hiddenBtn.setAttribute('aria-label', t);
    };

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
        if (this.opts.height) this.sh.querySelector('.win').style.height = this.opts.height;
        if (this.opts.searchPlaceholder) this.sh.getElementById('search').placeholder = this.opts.searchPlaceholder;
        var q = this.q = {};
        var ids = ['close', 'title', 'rootLine', 'crumb', 'path', 'editPath', 'refresh', 'hidden', 'rowmenu',
            'mkdir', 'touch', 'upload', 'pack', 'del', 'note', 'search', 'meta', 'all', 'rows', 'empty', 'summary', 'dest', 'viewToggle',
            'expandAll', 'collapseAll', 'fullscreen', 'hiddenBtn',
            'upbox', 'upname', 'upbar', 'uppct', 'upcancel', 'editor', 'edTitle', 'edHost', 'edTree',
            'edClose', 'edCancel', 'edSave', 'preview', 'pvTitle', 'pvBody', 'pvClose',
            'jsonview', 'jvTitle', 'jvBody', 'jvClose', 'jvSource', 'jvExpand', 'jvCollapse', 'toast', 'file'];
        for (var i = 0; i < ids.length; i++) q[ids[i]] = this.sh.getElementById(ids[i]);
        q.win = this.sh.querySelector('.win');
        q.title.textContent = this.title;
        this.overlay = this.sh.querySelector('.overlay');
        if (this.inline) this.overlay.classList.add('inline');
        this.overlay.style.pointerEvents = 'auto';
        this.renderCrumb();
        this.applyViewButton();
        this.syncHiddenBtn();
        this.syncFullscreenBtn();
        this.setNote(this.opts.note || '', this.opts.noteWarn);
    };

    // 工具栏上的补充说明（根目录、容量、只读等）；空串时整段隐藏
    Manager.prototype.setNote = function (text, warn) {
        if (!this.q || !this.q.note) return;
        var s = String(text || '');
        this.q.note.textContent = s;
        this.q.note.hidden = !s;
        this.q.note.classList.toggle('warn', !!warn && !!s);
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
        on('hidden', 'change', function () { self.hidden = self.q.hidden.checked; self.syncHiddenBtn(); self.load(self.cwd); });
        on('hiddenBtn', 'click', function () {
            self.q.hidden.checked = !self.q.hidden.checked;
            self.hidden = self.q.hidden.checked;
            self.syncHiddenBtn();
            self.load(self.cwd);
        });
        on('mkdir', 'click', function () { self.mkdir(); });
        on('touch', 'click', function () { self.touch(); });
        on('upload', 'click', function () { self.q.file.click(); });
        on('file', 'change', function () { self.uploadFiles(self.q.file.files, self.targetDir()); self.q.file.value = ''; });
        on('viewToggle', 'click', function () { self.setView(self.isTree() ? 'list' : 'tree'); });
        on('expandAll', 'click', function () { self.expandAll(); });
        on('collapseAll', 'click', function () { self.collapseAll(); });
        on('fullscreen', 'click', function () { self.toggleFullscreen(); });
        on('pack', 'click', function () { self.archiveSelected(); });
        on('del', 'click', function () { self.removeSelected(); });
        on('all', 'change', function () { self.selectAll(self.q.all.checked); });
        on('search', 'input', function () { self.setFilter(self.q.search.value); });
        on('upcancel', 'click', function () { self.uploadCancel = true; });
        on('edClose', 'click', function () { self.closeEditor(); });
        on('edCancel', 'click', function () { self.closeEditor(); });
        on('edSave', 'click', function () { self.saveEditor(); });
        on('edTree', 'click', function () {
            var ed = self.editing;
            if (!ed) return;
            var text = self.editorInst ? self.editorInst.getValue() : ed.text;
            self.closeEditor();
            self.showJsonView(ed.path, text);
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
            if (!tr || tr.dataset.path === undefined) return;
            if (Date.now() - (self._lpAt || 0) < 700) return;
            if (e.target.matches('input[type=checkbox]')) {
                self.toggleSel(tr.dataset.path, e.target.checked);
                return;
            }
            if (e.target.closest('button')) return;
            if (tr.dataset.type !== 'dir') return;
            if (self.isTree()) {
                self.toggleExpand(tr.dataset.path);
            } else if (e.target.closest('.nm')) {
                self._navAt = Date.now();
                self.openEntry(tr.dataset.path, 'dir');
            }
        });
        this.q.rows.addEventListener('dblclick', function (e) {
            if (e.target.closest('button')) return;
            var tr = e.target.closest('tr');
            if (!tr || tr.dataset.path === undefined) return;
            if (tr.dataset.type === 'dir') {
                // 单击已经切换展开状态时，双击仍进入该目录作为根
                self.openEntry(tr.dataset.path, 'dir');
                return;
            }
            // 双击文件展开详情卡片，打开仍走卡片按钮或右键菜单
            self.showInfo(tr.dataset.path);
        });
        this.q.rows.addEventListener('contextmenu', function (e) {
            var tr = e.target.closest('tr');
            if (!tr || tr.dataset.path === undefined) return;
            e.preventDefault();
            e.stopPropagation();
            self.showRowMenu({ x: e.clientX, y: e.clientY }, tr.dataset.path, tr.dataset.type, true);
        });
        // 触屏长按 500ms 走同一套菜单；移动超过 10px 视为滚动
        this.q.rows.addEventListener('touchstart', function (e) {
            var tr = e.target.closest('tr');
            if (!tr || tr.dataset.path === undefined || e.touches.length !== 1) return;
            var t = e.touches[0];
            var sx = t.clientX, sy = t.clientY, moved = false;
            var timer = setTimeout(function () {
                if (moved) return;
                self._lpAt = Date.now();
                self.showRowMenu({ x: sx, y: sy }, tr.dataset.path, tr.dataset.type, true);
                if (navigator.vibrate) { try { navigator.vibrate(10); } catch (err) {} }
            }, 500);
            function move(ev) {
                var p = ev.touches[0];
                if (Math.abs(p.clientX - sx) > 10 || Math.abs(p.clientY - sy) > 10) {
                    moved = true;
                    clearTimeout(timer);
                }
            }
            function end() {
                clearTimeout(timer);
                tr.removeEventListener('touchmove', move);
                tr.removeEventListener('touchend', end);
                tr.removeEventListener('touchcancel', end);
            }
            tr.addEventListener('touchmove', move, { passive: true });
            tr.addEventListener('touchend', end);
            tr.addEventListener('touchcancel', end);
        }, { passive: true });
        this.q.rows.addEventListener('click', function (e) {
            var caret = e.target.closest('button[data-act=expand]');
            if (!caret) return;
            e.stopPropagation();
            var ctr = caret.closest('tr');
            if (ctr) self.toggleExpand(ctr.dataset.path);
        });
        this.q.rows.addEventListener('click', function (e) {
            var mb = e.target.closest('button[data-menu]');
            if (!mb) return;
            e.stopPropagation();
            var tr = mb.closest('tr');
            if (!tr) return;
            // 树形行只有 ⋯，直接给完整菜单
            self.showRowMenu(mb, tr.dataset.path, tr.dataset.type, self.isTree());
        });
        this.q.rows.addEventListener('click', function (e) {
            var b = e.target.closest('button[data-op]');
            if (!b) return;
            var tr = b.closest('tr');
            if (!tr) return;
            self.runAction(tr.dataset.path, tr.dataset.type, b.getAttribute('data-op'));
        });
        this.q.rowmenu.addEventListener('click', function (e) {
            e.stopPropagation();
            var b = e.target.closest('button[data-op]');
            if (!b) return;
            var m = self.q.rowmenu;
            var path = m.dataset.path, type = m.dataset.type, op = b.getAttribute('data-op');
            self.closeRowMenu();
            self.runAction(path, type, op);
        });
        this._docClick = function (e) {
            if (!self.q.rowmenu || self.q.rowmenu.hidden) return;
            var path = e.composedPath();
            if (path.indexOf(self.q.rowmenu) < 0) self.closeRowMenu();
        };
        document.addEventListener('click', this._docClick);
        this._closeMenu = function () {
            // 点击时浏览器可能先把按钮滚进视野，忽略刚打开瞬间的滚动
            if (Date.now() - (self._menuAt || 0) > 250) self.closeRowMenu();
        };
        window.addEventListener('scroll', this._closeMenu, true);
        window.addEventListener('resize', this._closeMenu);
        this._fsHandler = function () { self.syncFullscreenBtn(); };
        document.addEventListener('fullscreenchange', this._fsHandler);
        document.addEventListener('webkitfullscreenchange', this._fsHandler);
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
                if (tr && tr.dataset && tr.dataset.type === 'dir') dest = tr.dataset.path;
                self.uploadFiles(e.dataTransfer.files, dest);
            }
        });
        this._keyHandler = function (e) {
            if (!self.visible) return;
            var path = e.composedPath();
            var t = path && path[0];
            if (t && (t.tagName === 'INPUT' || (t.closest && t.closest('.cm-editor')))) return;
            if (e.key === 'Escape') {
                // Hex/尾部查看器盖在文件管理器上，Esc 只关查看器，不关管理器
                if (global.RtunnelFileView && global.RtunnelFileView.isOpen()) return;
                if (!self.q.rowmenu.hidden) self.closeRowMenu();
                else if (!self.q.jsonview.hidden) self.closeJsonView();
                else if (!self.q.editor.hidden) self.closeEditor();
                else if (!self.q.preview.hidden) self.closePreview();
                else if (self._form && self._infoPath) self.closeInlineForm();
                // 全屏时 Esc 交给浏览器退出全屏，不关整个文件管理器
                else if (self.isFullscreen()) return;
                // 内嵌在页面里时 Esc 不应该收起整个文件管理器（页面有自己的开关）
                else if (!self.inline) self.close();
                e.preventDefault();
            }
            else if (e.key === 'Delete') { self.removeSelected(); e.preventDefault(); }
            else if (e.key === 'F2') {
                var roots = self.selectedRoots();
                if (roots.length === 1) { self.rename(roots[0]); e.preventDefault(); }
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
    function baseName(p) {
        var i = p.lastIndexOf('/');
        return i < 0 ? p : p.slice(i + 1);
    }
    function saveBlob(blob, name) {
        var a = document.createElement('a');
        a.href = URL.createObjectURL(blob);
        a.download = name || 'download';
        document.body.appendChild(a);
        a.click();
        setTimeout(function () { URL.revokeObjectURL(a.href); a.remove(); }, 10000);
        return a.download;
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
        }).catch(function (e) { self.toast(e.message || '连接失败', true); });
    };

    Manager.prototype.close = function () {
        if (this.isFullscreen()) {
            var d = document;
            var exit = d.exitFullscreen || d.webkitExitFullscreen || d.msExitFullscreen;
            if (exit) { try { exit.call(d); } catch (e) { /* ignore */ } }
        }
        this.visible = false;
        this.closedByUser = true;
        this.overlay.hidden = true;
        this.closeRowMenu();
        this.uploadCancel = true;
        this.closeEditor();
        this.closePreview();
        if (this.reconnectTimer) { clearTimeout(this.reconnectTimer); this.reconnectTimer = null; }
        this._closeWs();
        if (this.backend && this.backend.close) { try { this.backend.close(); } catch (e) {} }
        if (this.onClose) this.onClose();
    };

    Manager.prototype.destroy = function () {
        this.close();
        this.destroyed = true;
        this.closeInlineForm();
        if (this._keyHandler) document.removeEventListener('keydown', this._keyHandler);
        if (this._docClick) document.removeEventListener('click', this._docClick);
        if (this._closeMenu) {
            window.removeEventListener('scroll', this._closeMenu, true);
            window.removeEventListener('resize', this._closeMenu);
        }
        if (this._fsHandler) {
            document.removeEventListener('fullscreenchange', this._fsHandler);
            document.removeEventListener('webkitfullscreenchange', this._fsHandler);
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

    Manager.prototype.connect = function () {
        var self = this;
        if (this.backend) {
            if (this.connected) return Promise.resolve(this.welcome);
            this.connSeq++;
            return this.backend.connect().then(function (w) {
                self.connected = true;
                self.welcome = w || {};
                self.applyCaps(self.welcome.caps);
                self.applySpace(self.welcome);
                return self.welcome;
            });
        }
        if (this.ws && this.connected) return Promise.resolve(this.welcome);
        this._closeWs();
        var seq = this.connSeq;
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
                    self.welcome = w;
                    self.applySpace(w);
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

    // welcome 与 list 都带空间信息：统一刷新根目录行和写权限
    Manager.prototype.applySpace = function (w) {
        var ro = this.readonly || !w.rw;
        this.rw = !!w.rw;
        if (w.label) {
            this.q.rootLine.textContent = w.label + (ro ? ' · 只读' : '');
        }
        else {
            this.q.rootLine.textContent = '根目录: ' + (w.root || '/') + ' · 可用 ' + fmtSize(w.free_kb * 1024) +
                ' / ' + fmtSize(w.total_kb * 1024) + (ro ? ' · 只读' : '');
        }
        this.q.mkdir.disabled = ro;
        this.q.touch.disabled = ro;
        this.q.upload.disabled = ro;
        this.q.del.disabled = ro;
        this.space = { root: w.root || '/', free_kb: w.free_kb, total_kb: w.total_kb };
        if (this.onSpace) this.onSpace(this.space);
    };

    // 后端能力：仓库没有目录/重命名/移动，隐藏对应入口
    Manager.prototype.applyCaps = function (caps) {
        if (!caps) return;
        var self = this;
        for (var k in caps) if (Object.prototype.hasOwnProperty.call(caps, k)) this.caps[k] = caps[k];
        var c = this.caps, q = this.q;
        q.mkdir.hidden = !c.mkdir;
        q.touch.hidden = !c.touch;
        if (c.flat) {
            // 仓库只有一层：固定列表视图，去掉路径栏，刷新并到工具栏
            this.viewMode = 'list';
            q.viewToggle.hidden = true;
            var bar1 = q.crumb.parentNode;
            var tools = q.mkdir.parentNode;
            if (q.refresh.parentNode !== tools) tools.insertBefore(q.refresh, q.mkdir);
            if (q.editPath.parentNode === bar1) bar1.hidden = true;
        }
        q.pack.hidden = !c.pack;
        q.upload.hidden = !c.upload;
        q.del.hidden = !c.del;
        q.hiddenBtn.hidden = c.hidden === false;
        if (this.backend && !c.tree) {
            // 后端只有一层：不提供视图切换
            q.viewToggle.hidden = true;
        } else {
            q.viewToggle.hidden = false;
            this.applyViewButton();
        }
        this.syncTreeTools();
    };

    // ------------------------------------------------------------ 后端封装

    Manager.prototype.listDir = function (path) {
        if (this.backend) return this.backend.list(path, this.hidden);
        return this.request({ op: 'list', id: this.id(), path: path, hidden: this.hidden, limit: 500 }, 'list');
    };

    Manager.prototype.statEntry = function (path) {
        if (this.backend) {
            return this.backend.stat(path).then(function (e) { return { entry: e }; });
        }
        return this.request({ op: 'stat', id: this.id(), path: path }, 'stat');
    };

    Manager.prototype.dirSize = function (path) {
        if (this.backend) return this.backend.size(path);
        return this.request({ op: 'size', id: this.id(), path: path }, 'size');
    };

    Manager.prototype.mkPath = function (path) {
        if (this.backend) return this.backend.mkdir(path);
        return this.request({ op: 'mkdir', id: this.id(), path: path }, 'mkdir');
    };

    Manager.prototype.touchPath = function (path) {
        if (this.backend) return this.backend.touch(path);
        return this.request({ op: 'touch', id: this.id(), path: path }, 'touch');
    };

    Manager.prototype.renamePath = function (path, name) {
        if (this.backend) return this.backend.rename(path, name);
        return this.request({ op: 'rename', id: this.id(), path: path, name: name }, 'rename');
    };

    Manager.prototype.movePath = function (path, dir) {
        if (this.backend) return this.backend.move(path, dir);
        return this.request({ op: 'move', id: this.id(), path: path, dir: dir }, 'move');
    };

    Manager.prototype.deletePath = function (path, recursive) {
        if (this.backend) return this.backend.remove(path, recursive);
        return this.request({ op: 'delete', id: this.id(), path: path, recursive: !!recursive }, 'delete');
    };

    Manager.prototype.putText = function (path, text) {
        if (this.backend && this.backend.writeText) return this.backend.writeText(path, text);
        return this.uploadBlob(new Blob([text], { type: 'text/plain' }), path.split('/').pop(), parentOf(path));
    };

    // 列表内展开的确认条（删除）
    Manager.prototype.confirmRow = function (anchor, opts) {
        var self = this;
        this.closeInlineForm();
        var formTr = document.createElement('tr');
        formTr.className = 'formrow';
        var td = document.createElement('td');
        td.colSpan = 5;
        var box = document.createElement('div');
        box.className = 'iconf';
        var tx = document.createElement('span');
        tx.className = 'tx';
        tx.appendChild(document.createTextNode(opts.text || ''));
        if (opts.name) {
            var b = document.createElement('b');
            b.textContent = opts.name;
            tx.appendChild(b);
        }
        if (opts.tail) tx.appendChild(document.createTextNode(opts.tail));
        var ok = document.createElement('button');
        ok.className = opts.danger === false ? 'primary' : 'danger';
        ok.textContent = opts.ok || '删除';
        var no = document.createElement('button');
        no.textContent = '取消';
        box.appendChild(tx); box.appendChild(ok); box.appendChild(no);
        td.appendChild(box);
        formTr.appendChild(td);
        var row = anchor && anchor.parentNode ? anchor : null;
        if (row) {
            row.parentNode.insertBefore(formTr, row.nextSibling);
            row.classList.add('editing');
        }
        else {
            this.q.rows.insertBefore(formTr, this.q.rows.firstChild);
        }
        this._form = { tr: row, row: formTr };
        var close = function () { self.closeInlineForm(); };
        no.addEventListener('click', close);
        ok.addEventListener('click', function () {
            ok.disabled = true; no.disabled = true;
            opts.onOk(close);
        });
        setTimeout(function () { ok.focus(); }, 0);
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
        var off = dv.getUint32(8) * 4294967296 + dv.getUint32(12);
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
        // 换目录/刷新时中止尚未完成的全部展开
        this._treeSeq = (this._treeSeq || 0) + 1;
        this._treeBusy = false;
        return this.listDir(path)
            .then(function (r) {
                var next = r.path || path;
                var sameRoot = next === self.cwd;
                self.cwd = next;
                self.entries = r.entries || [];
                self.sel.clear();
                self.selAll.clear();
                self.dirSizes.clear();
                self.dirPending.clear();
                if (!sameRoot) {
                    // 换根后旧展开路径不再有效，搜索结果也属于旧根
                    self.expanded.clear();
                    self.kids.clear();
                    self.loadingKids.clear();
                    self.filter = '';
                    self._searchHits = null;
                    self._searching = false;
                    self._searchSeq = (self._searchSeq || 0) + 1;
                    self.q.search.value = '';
                }
                self.q.path.value = self.cwd;
                self.renderCrumb();
                self.applySpace(r);
                self.render();
                if (self.opts.onList) self.opts.onList(r);
                if (sameRoot && self.isTree() && self.expanded.size) self.reloadExpanded();
                // 刷新后搜索条件仍有效：重新拉一次结果
                if (sameRoot && self.filter && self.isTree() && self.backend && self.backend.search) {
                    self.setFilter(self.filter);
                }
            })
            .catch(function (e) { if (self.visible) self.toast(e.message, true); });
    };

    // ------------------------------------------------------------ 树形视图

    Manager.prototype.fetchKids = function (path, quiet) {
        var self = this;
        if (this.loadingKids.has(path)) return Promise.resolve();
        this.loadingKids.add(path);
        if (!quiet) this.render();
        return this.listDir(path).then(function (r) {
            self.loadingKids.delete(path);
            var es = r.entries || [];
            self.kids.set(path, es);
            // 父目录是整棵选中的，加载出来的子项同步选中
            if (self.selAll.has(path)) {
                for (var i = 0; i < es.length; i++) self.markSubtree(join(path, es[i].name), true);
            }
            if (!quiet) self.render();
        }).catch(function (e) {
            self.loadingKids.delete(path);
            self.expanded.delete(path);
            self.kids.delete(path);
            if (!quiet) {
                self.toast('打开目录失败：' + (e.message || e), true);
                self.render();
            }
            throw e;
        });
    };

    Manager.prototype.toggleExpand = function (path) {
        if (this.expanded.has(path)) {
            this.expanded.delete(path);
            this.render();
            return;
        }
        this.expanded.add(path);
        if (this.kids.has(path) || this.loadingKids.has(path)) { this.render(); return; }
        this.fetchKids(path, false);
    };

    // 刷新后重新读取所有已展开目录；父目录先于子目录
    Manager.prototype.reloadExpanded = function () {
        var self = this;
        var paths = Array.from(this.expanded).sort(function (a, b) {
            return a.split('/').length - b.split('/').length || a.localeCompare(b);
        });
        var chain = Promise.resolve();
        paths.forEach(function (p) {
            chain = chain.then(function () { return self.fetchKids(p, true); }).catch(function () {});
        });
        chain.then(function () { if (self.visible) self.render(); });
    };

    Manager.prototype.findEntry = function (path) {
        var p = this.cwd;
        if (path === p) return null;
        var prefix = p === '/' ? '/' : p + '/';
        if (path.indexOf(prefix) !== 0) return null;
        var parts = path.slice(prefix.length).split('/');
        var entries = this.entries;
        var e = null;
        for (var i = 0; i < parts.length; i++) {
            e = null;
            for (var j = 0; j < entries.length; j++) {
                if (entries[j].name === parts[i]) { e = entries[j]; break; }
            }
            if (!e) return null;
            if (i < parts.length - 1) {
                p = join(p, parts[i]);
                entries = this.kids.get(p) || [];
            }
        }
        return e;
    };

    Manager.prototype.entryOf = function (path) {
        return this._entryByPath.get(path) || this.findEntry(path);
    };

    Manager.prototype.targetDir = function () {
        var roots = this.selectedRoots();
        if (roots.length === 1) {
            var p = roots[0];
            var e = this.entryOf(p);
            if (e && e.type === 'dir') return p;
            if (e) return parentOf(p);
        }
        return this.cwd;
    };

    Manager.prototype.sizeText = function (e, path) {
        if (e.type === 'dir') {
            var ds = this.dirSizes.get(path);
            if (ds) return fmtSize(ds.bytes) + (ds.truncated ? '+' : '');
            if (this.dirPending.get(path)) return '统计中…';
            return '—';
        }
        return fmtSize(e.size);
    };

    Manager.prototype.sortEntries = function (list, parent) {
        var self = this;
        list.sort(function (a, b) {
            var k = self.sortKey;
            if (k === 'size') {
                // 目录大小来自勾选后的递归统计；未统计的目录排在最后
                var ax = self.sizeForSort(a, join(parent, a.name)), bx = self.sizeForSort(b, join(parent, b.name));
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
        return list;
    };

    // 搜索：后端支持递归搜索时（文件仓库）走服务端，否则只过滤已加载节点
    Manager.prototype.setFilter = function (value) {
        var self = this;
        this.filter = String(value || '').trim().toLowerCase();
        this._searchSeq = (this._searchSeq || 0) + 1;
        var seq = this._searchSeq;
        var remote = this.isTree() && this.backend && this.backend.search && this.filter;
        this._searchHits = null;
        this._searching = !!remote;
        if (!remote) { this.render(); return; }
        this.render();
        this.backend.search(this.cwd, this.filter).then(function (list) {
            if (seq !== self._searchSeq) return;
            self._searching = false;
            self._searchHits = list || [];
            self.render();
        }).catch(function (e) {
            if (seq !== self._searchSeq) return;
            self._searching = false;
            self._searchHits = [];
            self.toast('搜索失败：' + (e.message || e), true);
            self.render();
        });
    };

    // 拍平当前树：搜索时只保留命中的节点及其祖先，并自动展开祖先
    Manager.prototype.treeNodes = function () {
        var self = this, f = this.filter;
        if (f && this._searchHits) {
            // 服务端递归搜索结果：路径已完整，直接平铺展示
            return this._searchHits.map(function (e) {
                var p = '/' + String(e.name || '').replace(/^\/+/, '');
                return { e: e, parent: '/', path: p, depth: 0, flat: true };
            });
        }
        var out = [];
        function matchBelow(entries, parent) {
            for (var i = 0; i < entries.length; i++) {
                var e = entries[i], p = join(parent, e.name);
                if (!f || e.name.toLowerCase().indexOf(f) >= 0) return true;
                if (e.type === 'dir' && self.kids.has(p) && matchBelow(self.kids.get(p), p)) return true;
            }
            return false;
        }
        function walk(entries, parent, depth) {
            var list = self.sortEntries(entries.slice(), parent);
            for (var i = 0; i < list.length; i++) {
                var e = list[i], p = join(parent, e.name);
                var selfHit = !f || e.name.toLowerCase().indexOf(f) >= 0;
                var kids = e.type === 'dir' ? self.kids.get(p) : null;
                var below = f && kids ? matchBelow(kids, p) : false;
                if (f && !selfHit && !below) continue;
                out.push({ e: e, parent: parent, path: p, depth: depth });
                if (e.type !== 'dir') continue;
                if (!(self.expanded.has(p) || (f && below))) continue;
                if (self.loadingKids.has(p)) { out.push({ loading: true, parent: p, path: p, depth: depth + 1 }); continue; }
                if (!kids) continue;
                if (!kids.length) { out.push({ emptyDir: true, parent: p, path: p, depth: depth + 1 }); continue; }
                walk(kids, p, depth + 1);
            }
        }
        walk(this.entries, this.cwd, 0);
        return out;
    };

    Manager.prototype.noteRow = function (text, depth) {
        var tr = document.createElement('tr');
        tr.className = 'tree-note';
        var td = document.createElement('td');
        td.colSpan = 5;
        var d = document.createElement('div');
        d.className = 'note';
        d.style.paddingLeft = (14 + depth * 16) + 'px';
        d.textContent = text;
        td.appendChild(d);
        tr.appendChild(td);
        return tr;
    };

    Manager.prototype.buildRow = function (e, parent, depth, tree, flat) {
        var path = join(parent, e.name);
        var tr = document.createElement('tr');
        tr.dataset.path = path;
        tr.dataset.name = e.name;
        tr.dataset.type = e.type;
        tr.dataset.dir = parent;
        if (this.sel.has(path)) tr.className = 'sel';

        var td0 = document.createElement('td');
        var cb = document.createElement('input');
        cb.type = 'checkbox';
        cb.checked = this.sel.has(path);
        if (!cb.checked && this._partial && this._partial.has(path)) cb.indeterminate = true;
        td0.appendChild(cb);
        tr.appendChild(td0);

        var td1 = document.createElement('td');
        var nm = document.createElement('div');
        nm.className = 'name';
        if (tree && depth > 0) {
            var ind = document.createElement('span');
            ind.className = 'ind';
            ind.style.width = (depth * 16) + 'px';
            nm.appendChild(ind);
        }
        if (e.type === 'dir') {
            var tw = document.createElement('button');
            tw.type = 'button';
            var live = tree && !flat;
            tw.className = 'tw' + (live && this.expanded.has(path) ? ' open' : '') + (live ? '' : ' ph');
            tw.setAttribute('data-act', 'expand');
            tw.title = live ? (this.expanded.has(path) ? '收起' : '展开') : '';
            if (!live) tw.tabIndex = -1;
            nm.appendChild(tw);
        } else {
            // 文件也占一个箭头槽，列表视图下图标与目录对齐
            var tw2 = document.createElement('span');
            tw2.className = 'tw ph';
            nm.appendChild(tw2);
        }
        var ic = document.createElement('span');
        ic.className = 'tico ' + (e.type === 'dir' ? 'dir' : e.type === 'link' ? 'lnk' : isImage(e.name) ? 'img' : '');
        ic.innerHTML = e.type === 'dir' ? ICONS.folder : e.type === 'link' ? ICONS.link
            : isImage(e.name) ? ICONS.image : ICONS.file;
        nm.appendChild(ic);
        var span = document.createElement('span');
        span.className = 'nm';
        span.textContent = e.name + (e.is_link && e.target ? ' → ' + e.target : '');
        span.title = e.name;
        nm.appendChild(span);
        var im = document.createElement('span');
        im.className = 'iming';
        var isz = document.createElement('span');
        isz.textContent = this.sizeText(e, path);
        var itm = document.createElement('span');
        itm.className = 'itm';
        itm.textContent = ' · ' + fmtTime(e.mtime);
        im.appendChild(isz);
        im.appendChild(itm);
        nm.appendChild(im);
        td1.appendChild(nm);
        tr.appendChild(td1);

        var td2 = document.createElement('td');
        td2.className = 'dim';
        td2.textContent = this.sizeText(e, path);
        tr.appendChild(td2);

        var td3 = document.createElement('td');
        td3.className = 'dim';
        td3.textContent = fmtTime(e.mtime);
        tr.appendChild(td3);

        var td4 = document.createElement('td');
        td4.className = 'ops';
        if (tree || flat) {
            var mb = document.createElement('button');
            mb.setAttribute('data-menu', '1');
            mb.textContent = '⋯';
            mb.title = '更多操作';
            td4.appendChild(mb);
        } else {
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
                var mb2 = document.createElement('button');
                mb2.setAttribute('data-menu', '1');
                mb2.textContent = '⋯';
                mb2.title = '更多操作';
                td4.appendChild(mb2);
            }
        }
        tr.appendChild(td4);
        return tr;
    };

    Manager.prototype.render = function () {
        var self = this;
        // 排序/搜索/勾选/窗口变化都会重建行，详情卡片跟着行一起重建
        var keepInfo = this._infoPath;
        this.closeInlineForm();
        var rows = this.q.rows;
        while (rows.firstChild) rows.removeChild(rows.firstChild);
        this._entryByPath = new Map();
        this._rowItems = [];
        var tree = this.isTree();
        var table = this.sh.querySelector('table');
        if (table) table.classList.toggle('tree', tree);
        // 操作列可用宽度 → 最多平铺几个按钮，剩下的进 ⋯
        var wrapW = rows.parentElement ? rows.parentElement.clientWidth : 0;
        var avail = wrapW >= 560 ? wrapW - 480 : 0;
        var cum = [0, 44, 90, 138, 186, 238, 290];
        this._opSlots = 0;
        if (!tree) {
            for (var ci = 0; ci < cum.length; ci++) if (avail >= cum[ci]) this._opSlots = ci;
            // 窄屏隐藏了大小/时间列，仍有空间放一个主操作
            if (wrapW > 0 && wrapW < 560) this._opSlots = 1;
        }
        var count = 0;
        // 有后代被选中、但自身未选中的目录 → 半选状态
        this._partial = this.partialDirs();
        if (tree) {
            var nodes = this.treeNodes();
            for (var i = 0; i < nodes.length; i++) {
                var n = nodes[i];
                if (n.loading) { rows.appendChild(this.noteRow('加载中…', n.depth)); continue; }
                if (n.emptyDir) { rows.appendChild(this.noteRow('空目录', n.depth)); continue; }
                this._entryByPath.set(n.path, n.e);
                this._rowItems.push(n.path);
                count++;
                rows.appendChild(this.buildRow(n.e, n.parent, n.depth, true, !!n.flat));
            }
        } else {
            var list = this.entries.slice();
            var f = this.filter;
            if (f) list = list.filter(function (e) { return e.name.toLowerCase().indexOf(f) >= 0; });
            this.sortEntries(list, this.cwd);
            count = list.length;
            for (var j = 0; j < list.length; j++) {
                var e = list[j], p = join(this.cwd, e.name);
                this._entryByPath.set(p, e);
                this._rowItems.push(p);
                rows.appendChild(this.buildRow(e, this.cwd, 0, false));
            }
        }
        this.q.empty.hidden = count > 0 || this._searching;
        this.q.empty.textContent = this._searching ? '搜索中…' : this.filter ? '没有匹配项' : '空目录';

        // 树形下汇总所有可见文件；列表下等同于当前目录
        var totalSize = 0;
        for (var ti = 0; ti < this._rowItems.length; ti++) {
            var te = this._entryByPath.get(this._rowItems[ti]);
            if (te && te.type === 'file') totalSize += te.size || 0;
        }
        var selCount = this.sel.size, selSize = 0, sizePending = false, selHasDir = false;
        this.sel.forEach(function (p) {
            var en = self._entryByPath.get(p) || self.findEntry(p);
            if (!en) return;
            if (en.type === 'file') selSize += en.size || 0;
            else if (en.type === 'dir') {
                selHasDir = true;
                var dsz = self.dirSizes.get(p);
                if (dsz) selSize += dsz.bytes;
                else if (self.dirPending.get(p)) sizePending = true;
            }
        });
        // 目录不参与体积统计，只显示选中数量，避免出现「0 B」
        this.q.summary.textContent = count + ' 项 · ' + fmtSize(totalSize) +
            (selCount ? ' · 已选 ' + selCount + ' 项' +
                (selHasDir ? '' : ' (' + fmtSize(selSize) + (sizePending ? '，统计中…' : '') + ')') : '');
        var allOn = this._rowItems.length > 0;
        var anyOn = false;
        for (var ri = 0; ri < this._rowItems.length; ri++) {
            if (this.sel.has(this._rowItems[ri])) anyOn = true;
            else allOn = false;
        }
        this.q.all.checked = allOn;
        this.q.all.indeterminate = anyOn && !allOn;
        this.q.del.disabled = this.readonly || !this.rw || !this.caps.del || !selCount;
        this.q.pack.disabled = !selCount;
        // 条目总数在底部 footer 已有，工具条只在筛选/搜索时补充计数
        this.q.meta.textContent = this._searchHits
            ? '搜索 ' + count + ' 项'
            : (this.filter ? '筛选 ' + count + ' / ' + this.entries.length : '');
        var dest = (!this.readonly && this.rw && !this.caps.flat) ? this.targetDir() : '';
        this.q.dest.textContent = dest ? '上传到 ' + dest : '';
        this.q.upload.title = '上传到 ' + (dest || this.cwd);
        this.q.upload.setAttribute('aria-label', this.q.upload.title);
        if (keepInfo) {
            var ke = this._entryByPath.get(keepInfo);
            if (ke) this.showInfo(keepInfo);
        }
    };

    // 右键 / ⋯ 的完整菜单（顺序按使用频率，与行内按钮不同）
    Manager.prototype.rowMenuActions = function (type, name) {
        var items = [];
        if (this.menuExtras) {
            var extra = this.menuExtras(type, name) || [];
            if (extra.length) {
                items = items.concat(extra);
                items.push({ sep: true });
            }
        }
        items.push({ op: 'open', label: '打开' });
        if (type === 'dir' && this.isTree()) {
            items.push({ op: 'expandAll', label: '全部展开' });
            items.push({ op: 'collapseAll', label: '全部折叠' });
        }
        if (type === 'file') {
            items.push({ op: 'edit', label: '编辑（文本）' });
            items.push({ op: 'hex', label: '查看 Hex' });
            items.push({ op: 'tail', label: '查看尾部' });
        }
        items.push({ op: 'info', label: '属性' });
        items.push({ sep: true });
        items.push({ op: 'dl', label: type === 'dir' ? '打包下载' : '下载' });
        if (!this.readonly && this.rw) {
            if (this.caps.rename) items.push({ op: 'rename', label: '重命名' });
            if (this.caps.move) items.push({ op: 'move', label: '移动' });
            if (this.caps.del) {
                items.push({ sep: true });
                items.push({ op: 'del', label: '删除', danger: true });
            }
        }
        return items;
    };

    Manager.prototype.rowActions = function (type, name) {
        var items = [{ op: 'dl', label: '下载' }];
        var w = !this.readonly && this.rw;
        if (w && type === 'file' && isImage(name)) items.push({ op: 'pv', label: '预览' });
        if (w) {
            if (this.caps.rename) items.push({ op: 'rename', label: '重命名' });
            if (this.caps.move) items.push({ op: 'move', label: '移动' });
            if (this.caps.del) items.push({ op: 'del', label: '删除', danger: true });
        }
        if (this.caps.rowEdit && type === 'file') items.push({ op: 'edit', label: '编辑' });
        return items;
    };

    Manager.prototype.runAction = function (path, type, op) {
        var name = baseName(path);
        if (this.onMenuAction && this.onMenuAction(op, name, type, path)) return;
        if (op === 'open') this.openEntry(path, type);
        else if (op === 'expandAll') this.expandSubtree(path);
        else if (op === 'collapseAll') this.collapseSubtree(path);
        else if (op === 'edit') this.openEditor(path);
        else if (op === 'hex') this.openHexView(name, path);
        else if (op === 'tail') this.openTailView(name, path);
        else if (op === 'info') this.showInfo(path);
        else if (op === 'dl') {
            if (type === 'dir') this.downloadArchive(path);
            else this.downloadToFile(path);
        }
        else if (op === 'pv') this.openPreview(path);
        else if (op === 'rename') this.rename(path);
        else if (op === 'move') this.move(path);
        else if (op === 'del') this.remove(path, type === 'dir');
    };

    Manager.prototype.closeRowMenu = function () {
        if (!this.q.rowmenu) return;
        this.q.rowmenu.hidden = true;
        this.q.rowmenu.dataset.path = '';
        this.q.rowmenu.dataset.type = '';
    };

    Manager.prototype.showRowMenu = function (anchor, path, type, full) {
        var name = baseName(path);
        var acts;
        if (full) acts = this.rowMenuActions(type, name);
        else {
            var all = this.rowActions(type, name);
            acts = all.slice(Math.min(this._opSlots, all.length));
        }
        if (!acts.length) return;
        var menu = this.q.rowmenu;
        while (menu.firstChild) menu.removeChild(menu.firstChild);
        var pendingSep = false, shown = 0;
        for (var i = 0; i < acts.length; i++) {
            if (acts[i].sep) { pendingSep = shown > 0; continue; }
            if (pendingSep) {
                var sep = document.createElement('div');
                sep.className = 'sep';
                menu.appendChild(sep);
                pendingSep = false;
            }
            var b = document.createElement('button');
            b.setAttribute('data-op', acts[i].op);
            b.textContent = acts[i].label;
            if (acts[i].danger) b.className = 'danger';
            menu.appendChild(b);
            shown++;
        }
        menu.dataset.path = path;
        menu.dataset.type = type;
        this._menuAt = Date.now();
        menu.style.left = '0px';
        menu.style.top = '0px';
        menu.hidden = false;
        var mw = menu.offsetWidth, mh = menu.offsetHeight;
        var left, top;
        if (anchor && anchor.getBoundingClientRect) {
            var rb = anchor.getBoundingClientRect();
            left = Math.min(Math.max(8, rb.right - mw), global.innerWidth - mw - 8);
            top = rb.bottom + 6;
            if (top + mh > global.innerHeight - 8) top = Math.max(8, rb.top - mh - 6);
        }
        else {
            left = Math.min(Math.max(8, anchor.x), global.innerWidth - mw - 8);
            top = Math.min(Math.max(8, anchor.y), global.innerHeight - mh - 8);
        }
        menu.style.left = left + 'px';
        menu.style.top = top + 'px';
    };

    Manager.prototype.sizeForSort = function (e, path) {
        if (e.type !== 'dir') return e.size || 0;
        var ds = this.dirSizes.get(path);
        return ds ? ds.bytes : null;
    };

    // 递归标记/取消目录及其已加载的子项；selAll 同时记录「整棵子树选中」，
    // 这样之后展开才加载出来的子项也会自动选中。
    Manager.prototype.markSubtree = function (path, on) {
        var e = this.entryOf(path);
        var isDir = !!(e && e.type === 'dir');
        if (on) {
            this.sel.add(path);
            if (isDir) this.selAll.add(path);
        }
        else {
            this.sel.delete(path);
            this.selAll.delete(path);
        }
        if (!isDir) return;
        var kids = this.kids.get(path) || [];
        for (var i = 0; i < kids.length; i++) this.markSubtree(join(path, kids[i].name), on);
    };

    // 取消某个子项后，祖先都变成部分选中（不进入 sel，界面显示 indeterminate）
    Manager.prototype.unmarkAncestors = function (path) {
        var a = parentOf(path);
        while (a && a.length >= this.cwd.length) {
            this.sel.delete(a);
            this.selAll.delete(a);
            if (a === this.cwd) break;
            a = parentOf(a);
        }
    };

    // 选中集合的顶层路径：祖先已选中时忽略后代，避免删除/移动重复处理
    Manager.prototype.selectedRoots = function () {
        var self = this, out = [];
        this.sel.forEach(function (p) {
            var a = parentOf(p);
            while (a && a.length >= self.cwd.length) {
                if (self.sel.has(a)) return;
                if (a === self.cwd) break;
                a = parentOf(a);
            }
            out.push(p);
        });
        return out;
    };

    // 当前选中集合的祖先目录里，自身没选中但下面有选中项的目录
    Manager.prototype.partialDirs = function () {
        var out = new Set();
        var cwd = this.cwd;
        var self = this;
        this.sel.forEach(function (p) {
            var a = parentOf(p);
            while (a && a.length > cwd.length) {
                if (!self.sel.has(a)) out.add(a);
                a = parentOf(a);
            }
        });
        return out;
    };

    Manager.prototype.selectAll = function (on) {
        this.sel.clear();
        this.selAll.clear();
        if (on) {
            var tree = this.isTree();
            for (var i = 0; i < this._rowItems.length; i++) {
                var p = this._rowItems[i];
                if (tree) this.markSubtree(p, true);
                else this.sel.add(p);
                var e = this._entryByPath.get(p);
                if (e && e.type === 'dir') this.fetchDirSize(p);
            }
        }
        this.render();
    };

    Manager.prototype.toggleSel = function (path, on) {
        var e = this.entryOf(path);
        if (this.isTree() && e && e.type === 'dir') {
            // 树形视图勾选目录：递归选中/取消整棵子树
            this.markSubtree(path, on);
            if (!on) this.unmarkAncestors(path);
            if (on) this.fetchDirSize(path);
        }
        else {
            if (on) this.sel.add(path);
            else {
                this.sel.delete(path);
                this.unmarkAncestors(path);
            }
        }
        this.render();
    };

    // 勾选目录时按需递归统计，结果按绝对路径缓存
    Manager.prototype.fetchDirSize = function (path) {
        var self = this;
        // 后端没有递归统计能力时不做请求，目录大小保持「—」
        if (this.caps.dirSize === false) return;
        if (this.dirSizes.has(path) || this.dirPending.has(path)) return;
        this.dirPending.set(path, true);
        this.dirSize(path)
            .then(function (r) {
                self.dirPending.delete(path);
                self.dirSizes.set(path, { bytes: r.bytes || 0, truncated: !!r.truncated });
                self.render();
            })
            .catch(function (e) {
                self.dirPending.delete(path);
                self.toast('统计大小失败：' + (e.message || e), true);
                self.render();
            });
    };

    Manager.prototype.openEntry = function (path, type) {
        if (type === 'dir') { this._navAt = Date.now(); this.load(path); return; }
        var name = baseName(path);
        if (isImage(name)) this.openPreview(path);
        else if (isText(name)) this.openEditor(path);
        else this.downloadToFile(path);
    };

    // 文件/目录详情卡片：双击文件或菜单「属性」在行内展开
    Manager.prototype.showInfo = function (path) {
        var self = this;
        var tr = this.rowByPath(path);
        if (!tr) return;
        if (this._infoPath === path && this._form) { this.closeInlineForm(); return; }
        var e = this._entryByPath.get(path) || this.findEntry(path);
        var name = baseName(path);
        if (!e) e = { name: name, type: 'file', size: 0, mtime: 0 };
        var kind = e.type === 'dir' ? '目录' : e.type === 'link' ? '链接'
            : isImage(name) ? '图片' : isText(name) ? '文本文件' : '文件';
        var size = this.sizeText(e, path);
        if (e.type === 'dir' && !this.dirSizes.has(path)) size = '—';
        var mtime = e.mtime ? fmtTime(e.mtime) : '—';

        this.inlinePanel(path, function (api) {
            api.box.className = 'iform ifile';

            var top = document.createElement('div');
            top.className = 'top';
            var kindEl = document.createElement('span');
            kindEl.className = 'kind';
            kindEl.textContent = kind;
            var nmEl = document.createElement('b');
            nmEl.className = 'nm';
            nmEl.textContent = name;
            var sp = document.createElement('span');
            sp.className = 'sp';
            var closeBtn = document.createElement('button');
            closeBtn.textContent = '关闭';
            closeBtn.addEventListener('click', function () { api.close(); });
            top.appendChild(kindEl); top.appendChild(nmEl); top.appendChild(sp); top.appendChild(closeBtn);

            function cell(k, v, wide) {
                var d = document.createElement('div');
                d.className = 'cell' + (wide ? ' wide' : '');
                var a = document.createElement('span');
                a.className = 'k';
                a.textContent = k;
                var b = document.createElement('span');
                b.className = 'v';
                b.textContent = v;
                d.appendChild(a); d.appendChild(b);
                return d;
            }
            var grid = document.createElement('div');
            grid.className = 'grid';
            grid.appendChild(cell('大小', size));
            grid.appendChild(cell('修改时间', mtime));
            grid.appendChild(cell('路径', path, true));

            var acts = document.createElement('div');
            acts.className = 'acts';
            function act(label, primary, fn) {
                var b = document.createElement('button');
                b.textContent = label;
                if (primary) b.className = 'primary';
                b.addEventListener('click', fn);
                acts.appendChild(b);
            }
            var writable = !self.readonly && self.rw;
            act(e.type === 'dir' ? '打开' : (isImage(name) ? '预览' : '打开'), true, function () {
                self.openEntry(path, e.type);
            });
            if (e.type === 'file' && isText(name) && writable && self.caps.edit) {
                act('编辑（文本）', false, function () { self.openEditor(path); });
            }
            if (e.type === 'file') {
                act('查看 Hex', false, function () { self.openHexView(name, path); });
                act('查看尾部', false, function () { self.openTailView(name, path); });
            }
            act(e.type === 'dir' ? '打包下载' : '下载', false, function () {
                if (e.type === 'dir') self.downloadArchive(path);
                else self.downloadToFile(path);
            });

            api.box.appendChild(top);
            api.box.appendChild(grid);
            api.box.appendChild(acts);
        });
        this._infoPath = path;
    };

    Manager.prototype.createEntry = function (op, label, defName) {
        var self = this;
        var name = global.prompt(label, defName);
        if (!name) return;
        if (name.indexOf('/') >= 0 || name === '.' || name === '..') {
            this.toast('名称不能包含 /');
            return;
        }
        // 选中目录时新建到该目录，否则建在当前目录
        var dir = this.targetDir();
        var call = op === 'mkdir' ? this.mkPath(join(dir, name)) : this.touchPath(join(dir, name));
        call.then(function () {
            if (dir !== self.cwd) self.expanded.add(dir);
            self.load(self.cwd);
        })
            .catch(function (e) { self.toast(e.message, true); });
    };

    Manager.prototype.mkdir = function () {
        this.createEntry('mkdir', '新建文件夹名称：', 'newdir');
    };

    Manager.prototype.touch = function () {
        this.createEntry('touch', '新建文件名称：', 'new.txt');
    };

    Manager.prototype.rowByPath = function (path) {
        var list = this.q.rows.querySelectorAll('tr');
        for (var i = 0; i < list.length; i++) {
            if (list[i].dataset && list[i].dataset.path === path) return list[i];
        }
        return null;
    };

    // 兼容仓库等只有一层的宿主：按文件名查找
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

    // 宿主编排的行内面板（如部署表单），沿用列表内展开的样式
    Manager.prototype.inlinePanel = function (nameOrPath, build) {
        var self = this;
        var tr = this.rowByPath(nameOrPath) || this.rowByName(nameOrPath);
        if (!tr) return null;
        this.closeInlineForm();
        var formTr = document.createElement('tr');
        formTr.className = 'formrow';
        var td = document.createElement('td');
        td.colSpan = 5;
        var box = document.createElement('div');
        box.className = 'iform';
        td.appendChild(box);
        formTr.appendChild(td);
        tr.parentNode.insertBefore(formTr, tr.nextSibling);
        tr.classList.add('editing');
        this._form = { tr: tr, row: formTr };
        var api = {
            box: box,
            close: function () { self.closeInlineForm(); },
            toast: function (m, isErr) { self.toast(m, isErr); }
        };
        if (build) build(api);
        return api;
    };

    Manager.prototype.closeInlineForm = function () {
        if (!this._form) return;
        if (this._form.tr) this._form.tr.classList.remove('editing');
        if (this._form.row && this._form.row.parentNode) this._form.row.parentNode.removeChild(this._form.row);
        this._form = null;
        this._infoPath = '';
    };

    Manager.prototype.rename = function (path) {
        var self = this;
        var oldName = baseName(path);
        var tr = this.rowByPath(path);
        if (!tr) return;
        this.inlineForm(tr, {
            label: '重命名为',
            value: oldName,
            selectBase: true,
            onOk: function (name, done) {
                if (name === oldName) { done(null); return; }
                if (name.indexOf('/') >= 0) { done(new Error('名称不能包含 /')); return; }
                self.renamePath(path, name)
                    .then(function () {
                        self.load(self.cwd);
                        done(null, '已重命名 ' + oldName + ' → ' + name);
                    })
                    .catch(function (e) { done(e); });
            }
        });
    };

    Manager.prototype.move = function (path) {
        var self = this;
        var name = baseName(path);
        var tr = this.rowByPath(path);
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
                self.movePath(path, dest)
                    .then(function () {
                        if (dest !== self.cwd) self.expanded.add(dest);
                        self.load(self.cwd);
                        done(null, '已移动 ' + name);
                    })
                    .catch(function (e) { done(e); });
            }
        });
    };

    Manager.prototype.remove = function (path, isDir) {
        var self = this;
        var name = baseName(path);
        this.confirmRow(this.rowByPath(path), {
            text: '删除 ', name: name, tail: isDir ? ' 及其全部内容？' : '？', ok: '删除',
            onOk: function (close) {
                self.deletePath(path, !!isDir).then(function () {
                    close();
                    self.load(self.cwd);
                    self.toast('已删除 ' + name);
                }).catch(function (e) { close(); self.toast(e.message, true); });
            }
        });
    };

    Manager.prototype.removeSelected = function () {
        var self = this;
        if (this.readonly || !this.rw || !this.caps.del) return;
        if (!this.sel.size) { this.toast('请先选择文件'); return; }
        var paths = this.selectedRoots();
        var hasDir = paths.some(function (p) {
            var en = self._entryByPath.get(p) || self.findEntry(p);
            return !!(en && en.type === 'dir');
        });
        this.confirmRow(null, {
            text: '删除已选中的 ' + paths.length + ' 项？', tail: hasDir ? '目录会连同内容一起删除。' : '', ok: '删除',
            onOk: function (close) {
                close();
                var chain = Promise.resolve();
                var failed = 0;
                paths.forEach(function (p) {
                    chain = chain.then(function () {
                        var en = self._entryByPath.get(p) || self.findEntry(p);
                        return self.deletePath(p, !!(en && en.type === 'dir'))
                            .catch(function (e) { failed++; self.toast(e.message, true); });
                    });
                });
                chain.then(function () {
                    self.load(self.cwd);
                    if (!failed) self.toast('已删除 ' + paths.length + ' 项');
                });
            }
        });
    };

    // ------------------------------------------------------------ 上传/下载

    Manager.prototype.download = function (path, onProgress, opts) {
        var self = this;
        if (this.backend) {
            if (this._dlBusy) return Promise.reject(new Error('已有下载任务进行中'));
            this._dlBusy = true;
            return this.backend.download(path, onProgress, opts).finally(function () { self._dlBusy = false; });
        }
        if (this._dlBusy) return Promise.reject(new Error('已有下载任务进行中'));
        this._dlBusy = true;
        var id = this.id();
        var req = { op: 'download_open', id: id, path: path };
        if (opts && (opts.offset || opts.limit)) {
            req.offset = opts.offset || 0;
            if (opts.limit) req.limit = opts.limit;
        }
        return this.request(req, 'download_meta')
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
                return step();
            })
            .finally(function () { self._dlBusy = false; });
    };

    Manager.prototype.downloadToFile = function (path) {
        var self = this;
        this.toast('开始下载 ' + path.split('/').pop());
        this.download(path, function (got, total) {
            self.q.summary.textContent = '下载中 ' + Math.floor(got * 100 / total) + '% (' + fmtSize(got) + ' / ' + fmtSize(total) + ')';
        }).then(function (r) {
            self.toast('已下载 ' + saveBlob(r.blob, r.meta.name));
        }).catch(function (e) { self.toast(e.message, true); })
          .finally(function () { self.render(); });
    };

    // 目录打包成 tar.gz 边压边传；支持 File System Access API 时直接写入磁盘，否则回退 Blob。
    Manager.prototype.downloadArchive = function (path) {
        var self = this;
        // 仓库后端暂不支持目录打包，逐文件下载或勾选文件打包
        if (this.backend) { this.toast('目录暂不支持打包，请勾选文件后打包', true); return; }
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
                        self.toast('已下载 ' + saveBlob(blob, meta.name || (base + '.tar.gz')));
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

    // 勾选后打包（仅后端支持时提供按钮）
    Manager.prototype.archiveSelected = function () {
        var self = this;
        if (!this.backend || !this.backend.archive) return;
        var dirs = 0;
        var paths = Array.from(this.sel).filter(function (p) {
            var en = self.entryOf(p);
            if (en && en.type === 'dir') { dirs++; return false; }
            return true;
        });
        if (!paths.length) { this.toast(dirs ? '目录暂不支持打包' : '请先选择文件'); return; }
        if (dirs) this.toast('已跳过 ' + dirs + ' 个目录');
        var base = paths.length === 1 ? baseName(paths[0]).replace(/\.[^.]+$/, '') : ('repo-' + paths.length);
        this.toast('正在打包 ' + paths.length + ' 项…');
        this.backend.archive(paths, function (done, total) {
            self.q.summary.textContent = '打包中 ' + fmtSize(done) + (total ? ' / ' + fmtSize(total) : '');
        }).then(function (r) {
            saveBlob(r.blob, r.name || (base + '.tar.gz'));
            self.toast('已打包 ' + paths.length + ' 项');
            self.render();
        }).catch(function (e) {
            self.toast('打包失败：' + (e.message || e), true);
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
                if (dir && dir !== self.cwd) self.expanded.add(dir);
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
        if (this.backend) {
            this.uploadCancel = false;
            this.q.upbox.className = 'up show';
            this.q.upname.textContent = file.name;
            this.q.upbar.style.width = '0%';
            this.q.uppct.textContent = '0%';
            return this.backend.upload(file, dir, function (sent, total) {
                var pct = total ? Math.floor(sent * 100 / total) : 100;
                self.q.upbar.style.width = pct + '%';
                self.q.uppct.textContent = pct + '%';
            }, function () { return self.uploadCancel; }).then(function () {
                self.q.upbox.className = 'up';
                self.toast('已上传 ' + file.name);
            }).catch(function (e) {
                self.q.upbox.className = 'up';
                throw e;
            });
        }
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
                var win = r.inflight || 1;      // 旧 agent 不带该字段，退化为串行
                var next = offset;
                return new Promise(function (resolve, reject) {
                    var inflight = 0, failed = false;
                    function fail(e) {
                        if (failed) return;
                        failed = true;
                        reject(e);
                    }
                    function pump() {
                        if (failed) return;
                        if (self.uploadCancel) {
                            failed = true;
                            self.request({ op: 'upload_cancel', id: self.id(), upload_id: uploadId }, 'upload_cancelled')
                                .then(function () {
                                    try { localStorage.removeItem(upKey); } catch (e) {}
                                    reject(new Error('已取消'));
                                }).catch(reject);
                            return;
                        }
                        while (inflight < win && next < file.size) {
                            (function (start, end) {
                                next = end;
                                inflight++;
                                file.slice(start, end).arrayBuffer().then(function (buf) {
                                    self.sendBinary(id, 2, start, new Uint8Array(buf));
                                    return self.waitMsg(id, 'upload_ack');
                                }).then(function (ack) {
                                    inflight--;
                                    offset = ack.received;
                                    var pct = file.size ? Math.floor(offset * 100 / file.size) : 100;
                                    self.q.upbar.style.width = pct + '%';
                                    self.q.uppct.textContent = pct + '%';
                                    pump();
                                }).catch(fail);
                            })(next, Math.min(next + chunk, file.size));
                        }
                        if (inflight === 0 && next >= file.size) resolve();
                    }
                    pump();
                }).then(function () {
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

    Manager.prototype.statPath = function (path) {
        return this.statEntry(path);
    };

    // 只读一段文件（Hex / 尾部查看用），返回 Uint8Array
    Manager.prototype.readRange = function (path, offset, len) {
        if (this.backend) return this.backend.readRange(path, offset, len);
        return this.download(path, null, { offset: offset, limit: len }).then(function (r) {
            return r.blob.arrayBuffer().then(function (buf) { return new Uint8Array(buf); });
        });
    };

    Manager.prototype.ensureViewLib = function () {
        var self = this;
        if (global.RtunnelFileView) return Promise.resolve(global.RtunnelFileView);
        if (this._viewLib) return this._viewLib;
        this._viewLib = new Promise(function (resolve, reject) {
            var s = document.createElement('script');
            s.src = self.opts.viewUrl || '/assets/file-view.js';
            s.async = true;
            s.onload = function () {
                if (global.RtunnelFileView) resolve(global.RtunnelFileView);
                else { self._viewLib = null; reject(new Error('查看器组件不可用')); }
            };
            s.onerror = function () { self._viewLib = null; reject(new Error('查看器组件加载失败')); };
            document.head.appendChild(s);
        });
        return this._viewLib;
    };

    Manager.prototype.openHexView = function (name, path) {
        var self = this;
        this.ensureViewLib().then(function (lib) {
            return self.statPath(path).then(function (st) {
                lib.openHex({
                    name: name + ' · Hex',
                    size: st.entry.size,
                    read: function (off, len) { return self.readRange(path, off, len); }
                });
            });
        }).catch(function (e) { self.toast(e.message || '查看失败', true); });
    };

    Manager.prototype.openTailView = function (name, path) {
        var self = this;
        this.ensureViewLib().then(function (lib) {
            return self.statPath(path).then(function (st) {
                lib.openTail({
                    name: name + ' · 尾部',
                    size: st.entry.size,
                    read: function (off, len) { return self.readRange(path, off, len); },
                    stat: function () {
                        return self.statPath(path).then(function (r) { return r.entry.size; });
                    }
                });
            });
        }).catch(function (e) { self.toast(e.message || '查看失败', true); });
    };

    // 读取文本文件（限制 1 MiB），返回 { name, text }
    Manager.prototype.readText = function (path) {
        var self = this;
        return this.statEntry(path).then(function (st) {
            if (st.entry.size > 1024 * 1024) throw new Error('文件超过 1 MiB，请下载后查看');
            return self.download(path).then(function (r) {
                return r.blob.text().then(function (t) { return { name: r.meta.name, text: t }; });
            });
        });
    };

    Manager.prototype.openEditor = function (path, text) {
        var self = this;
        var name = path.split('/').pop();
        if (text !== undefined) { this.showEditor(path, name, text); return; }
        this.toast('读取 ' + name + ' ...');
        this.readText(path)
            .then(function (r) { self.showEditor(path, r.name, r.text); })
            .catch(function (e) { self.toast(e.message, true); });
    };

    Manager.prototype.showEditor = function (path, name, text) {
        this.editing = { path: path, name: name, text: text };
        this.q.edTitle.textContent = '编辑 - ' + path;
        this.q.edTree.hidden = ext(name) !== 'json';
        this.q.editor.hidden = false;
        this.mountEditor();
    };

    Manager.prototype.mountEditor = function () {
        var self = this;
        var ed = this.editing;
        if (!ed) return;
        this.destroyEditorInst();
        var tok = this.editorToken;
        this.ensureEditorLib().then(function (factory) {
            if (tok !== self.editorToken || !self.editing) return;
            self.editorInst = factory(self.q.edHost, {
                value: ed.text,
                language: langOf(ed.name),
                onSave: function () { self.saveEditor(); },
                onEscape: function () { self.closeEditor(); }
            });
        }).catch(function (e) {
            self.toast(e.message || '编辑器组件加载失败', true);
            self.closeEditor();
        });
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
        if (global.hvCreateEditor) return Promise.resolve(global.hvCreateEditor);
        if (this._editorLib) return this._editorLib;
        this._editorLib = new Promise(function (resolve, reject) {
            var s = document.createElement('script');
            s.src = self.opts.editorUrl || '/assets/cm-editor.js';
            s.async = true;
            s.onload = function () {
                if (global.hvCreateEditor) resolve(global.hvCreateEditor);
                else { self._editorLib = null; reject(new Error('编辑器组件不可用')); }
            };
            s.onerror = function () { self._editorLib = null; reject(new Error('编辑器组件加载失败')); };
            document.head.appendChild(s);
        });
        return this._editorLib;
    };

    Manager.prototype.closeEditor = function () {
        this.editing = null;
        this.destroyEditorInst();
        this.q.editor.hidden = true;
    };

    Manager.prototype.saveEditor = function () {
        var self = this;
        if (!this.editing) return;
        if (this.readonly || !this.rw) { this.toast('只读模式', true); return; }
        var text = this.editorInst ? this.editorInst.getValue() : this.editing.text;
        var name = this.editing.name;
        var path = this.editing.path;
        this.q.edSave.disabled = true;
        this.putText(path, text).then(function () {
            self.toast('已保存 ' + name);
            self.closeEditor();
            self.load(self.cwd);
        }).catch(function (e) {
            self.toast(e.message, true);
        }).finally(function () { self.q.edSave.disabled = false; });
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
            return;
        }
        body.appendChild(buildJsonTree(data, { expandDepth: expandDepth, budget: budget }));
    };

    Manager.prototype.closeJsonView = function () {
        this.q.jsonview.hidden = true;
        while (this.q.jvBody.firstChild) this.q.jvBody.removeChild(this.q.jvBody.firstChild);
        this.jvPath = '';
        this.jvText = '';
    };

    Manager.prototype.uploadBlob = function (blob, name, dir) {
        return this.uploadOne(new File([blob], name), dir);
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
        createManager: function (opts) { return new Manager(opts); }
    };
})(window);
