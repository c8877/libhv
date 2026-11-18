/* rtunnel 只读查看器：Hex / 文件尾部（本地 Web 与 relay-admin 共用）
 *
 * 组件只负责渲染，数据由调用方通过回调提供：
 *   RtunnelFileView.openHex({
 *       name: 'agent.bin', size: 4096,
 *       read: function (offset, len) { return Promise<Uint8Array>; }
 *   });
 *   RtunnelFileView.openTail({
 *       name: 'agent.log', size: 12345, lines: 200,
 *       read: function (offset, len) { ... },
 *       stat: function () { return Promise<number>; }   // 可选，提供后支持「跟随」
 *   });
 */
(function (global) {
    'use strict';

    var HEX_PAGE = 4096;
    var TAIL_SPAN = 64 * 1024;

    var CSS = [
        ':host{all:initial}',
        '*{box-sizing:border-box}',
        '.overlay{position:fixed;inset:0;z-index:2147483100;background:rgba(15,23,42,.48);',
        'display:flex;align-items:center;justify-content:center;padding:24px}',
        '.win{width:min(1000px,96vw);max-height:86vh;display:flex;flex-direction:column;',
        'background:var(--panel,#fff);color:var(--fg,#1f2937);border:1px solid var(--line,#e2e8f0);',
        'border-radius:14px;box-shadow:0 24px 64px rgba(15,23,42,.28);overflow:hidden}',
        '.head{display:flex;align-items:center;gap:10px;padding:12px 16px;border-bottom:1px solid var(--line,#e2e8f0)}',
        '.head b{font-size:13.5px;font-weight:650;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}',
        '.head .sp{flex:1}',
        '.head .sz{color:var(--muted,#64748b);font-size:12px;font-variant-numeric:tabular-nums}',
        '.x{border:0;background:transparent;color:var(--muted,#64748b);font-size:20px;line-height:1;',
        'cursor:pointer;padding:2px 8px;border-radius:8px}',
        '.x:hover{background:rgba(100,116,139,.12)}',
        '.bar{display:flex;align-items:center;gap:8px;padding:8px 16px;border-bottom:1px solid var(--line,#e2e8f0);',
        'flex-wrap:wrap;font-size:12px}',
        '.bar label{display:flex;align-items:center;gap:6px;color:var(--muted,#64748b)}',
        '.bar input[type=text]{width:130px;padding:5px 8px;border:1px solid var(--line,#cbd5e1);border-radius:8px;',
        'background:var(--input,#fff);color:inherit;font-family:var(--mono,ui-monospace,Menlo,Consolas,monospace);font-size:12px}',
        '.bar select{padding:5px 8px;border:1px solid var(--line,#cbd5e1);border-radius:8px;background:var(--input,#fff);color:inherit;font-size:12px}',
        '.bar button{padding:5px 10px;border:1px solid var(--line,#cbd5e1);border-radius:8px;background:var(--input,#fff);',
        'color:inherit;font-size:12px;cursor:pointer}',
        '.bar button:hover:not(:disabled){border-color:#3b82f6;color:#2563eb}',
        '.bar button:disabled{opacity:.45;cursor:default}',
        '.bar .sp{flex:1}',
        '.bar .muted{color:var(--muted,#64748b)}',
        '.body{flex:1;min-height:0;overflow:auto;padding:10px 16px 14px}',
        'pre{margin:0;font-family:var(--mono,ui-monospace,Menlo,Consolas,monospace);font-size:12px;line-height:1.55}',
        '.hex .off{color:#94a3b8}',
        '.hex .bytes{color:inherit}',
        '.hex .asc{color:#0ea5e9}',
        '.hex .gap{color:#cbd5e1}',
        '.msg{color:var(--muted,#64748b);font-size:12.5px;padding:8px 0}',
        '.err{color:#dc2626;font-size:12.5px;padding:8px 0;white-space:pre-wrap}',
        '@media (prefers-color-scheme:dark){',
        '.win{--panel:#161a20;--fg:#e6edf3;--line:#2b333e;--muted:#9aa4b2;--input:#1b2028}',
        '.x:hover{background:rgba(148,163,184,.16)}',
        '.hex .off{color:#6b7684}.hex .asc{color:#67d5f5}.hex .gap{color:#3b4450}',
        '.err{color:#f87171}}',
        '@media (max-width:600px){',
        '.overlay{padding:0}.win{width:100vw;max-height:100vh;height:100vh;border-radius:0}',
        '.bar input[type=text]{width:104px}}'
    ].join('');

    function h(tag, cls, text) {
        var el = document.createElement(tag);
        if (cls) el.className = cls;
        if (text !== undefined && text !== null) el.textContent = text;
        return el;
    }

    function fmtSize(n) {
        if (n === null || n === undefined) return '-';
        if (n < 1024) return n + ' B';
        var u = ['KB', 'MB', 'GB', 'TB'], v = n / 1024, i = 0;
        while (v >= 1024 && i < u.length - 1) { v /= 1024; i++; }
        return (v >= 100 ? v.toFixed(0) : v >= 10 ? v.toFixed(1) : v.toFixed(2)) + ' ' + u[i];
    }

    function pad(n, w) { return ('00000000' + n.toString(16)).slice(-w).toUpperCase(); }
    function two(n) { return n < 16 ? '0' + n.toString(16).toUpperCase() : n.toString(16).toUpperCase(); }
    function printable(b) { return b >= 32 && b < 127 ? String.fromCharCode(b) : '.'; }

    var active = null;

    function shell(title, sizeText) {
        if (active) active.close();
        var host = document.createElement('div');
        document.body.appendChild(host);
        var sh = host.attachShadow({ mode: 'open' });
        var st = document.createElement('style');
        st.textContent = CSS;
        sh.appendChild(st);

        var overlay = h('div', 'overlay');
        var win = h('div', 'win');
        var head = h('div', 'head');
        var name = h('b', '', title);
        var sz = h('span', 'sz', sizeText || '');
        var x = h('button', 'x', '\u00d7');
        x.title = '关闭';
        head.appendChild(name);
        head.appendChild(h('span', 'sp'));
        head.appendChild(sz);
        head.appendChild(x);
        var bar = h('div', 'bar');
        var body = h('div', 'body');
        win.appendChild(head);
        win.appendChild(bar);
        win.appendChild(body);
        overlay.appendChild(win);
        sh.appendChild(overlay);

        var onKey = function (e) { if (e.key === 'Escape' && ctrl.close) ctrl.close(); };
        document.addEventListener('keydown', onKey);
        overlay.addEventListener('mousedown', function (e) { if (e.target === overlay && ctrl.close) ctrl.close(); });

        var ctrl = {
            host: host, sh: sh, overlay: overlay, win: win, head: head, bar: bar, body: body,
            title: name, size: sz,
            close: null
        };
        x.addEventListener('click', function () { ctrl.close(); });
        ctrl._onKey = onKey;
        active = ctrl;
        return ctrl;
    }

    function teardown(ctrl, onClose) {
        if (ctrl.closed) return;
        ctrl.closed = true;
        document.removeEventListener('keydown', ctrl._onKey);
        if (ctrl.timer) clearInterval(ctrl.timer);
        ctrl.host.parentNode && ctrl.host.parentNode.removeChild(ctrl.host);
        if (active === ctrl) active = null;
        if (onClose) onClose();
    }

    function message(ctrl, text, isErr) {
        while (ctrl.body.firstChild) ctrl.body.removeChild(ctrl.body.firstChild);
        ctrl.body.appendChild(h('div', isErr ? 'err' : 'msg', text));
    }

    // ---------------------------------------------------------------- Hex

    function openHex(opts) {
        opts = opts || {};
        var size = Number(opts.size) || 0;
        var ctrl = shell(opts.name || 'Hex 查看', fmtSize(size));
        ctrl.close = function () { teardown(ctrl, opts.onClose); };

        var offset = 0;
        var page = Math.max(1, Math.ceil(size / HEX_PAGE));

        var offIn = document.createElement('input');
        offIn.type = 'text';
        offIn.value = '0x00000000';
        offIn.title = '输入偏移（0x 十六进制或十进制）';
        var go = h('button', '', '跳转');
        var prev = h('button', '', '上一页');
        var next = h('button', '', '下一页');
        var info = h('span', 'muted', '');
        ctrl.bar.appendChild(h('label', '', '偏移'));
        ctrl.bar.appendChild(offIn);
        ctrl.bar.appendChild(go);
        ctrl.bar.appendChild(prev);
        ctrl.bar.appendChild(next);
        ctrl.bar.appendChild(h('span', 'sp'));
        ctrl.bar.appendChild(info);

        var pre = document.createElement('pre');
        pre.className = 'hex';
        ctrl.body.appendChild(pre);

        function render() {
            prev.disabled = offset <= 0;
            next.disabled = offset + HEX_PAGE >= size;
            offIn.value = '0x' + pad(offset, 8);
            info.textContent = '第 ' + (Math.floor(offset / HEX_PAGE) + 1) + ' / ' + page + ' 页 · 每页 ' +
                fmtSize(HEX_PAGE);
        }

        function load() {
            if (size === 0) { message(ctrl, '空文件'); render(); return; }
            var len = Math.min(HEX_PAGE, size - offset);
            // 上一次失败时 message() 清空了 body，这里把 pre 重新挂回去
            if (pre.parentNode !== ctrl.body) {
                while (ctrl.body.firstChild) ctrl.body.removeChild(ctrl.body.firstChild);
                ctrl.body.appendChild(pre);
            }
            pre.textContent = '读取中…';
            Promise.resolve(opts.read(offset, len)).then(function (bytes) {
                if (ctrl.closed) return;
                var arr = bytes instanceof Uint8Array ? bytes : new Uint8Array(bytes);
                var out = [];
                for (var i = 0; i < arr.length; i += 16) {
                    var n = Math.min(16, arr.length - i);
                    var hex = '', asc = '';
                    for (var j = 0; j < 16; j++) {
                        if (j < n) {
                            hex += two(arr[i + j]) + ' ';
                            asc += printable(arr[i + j]);
                        }
                        else {
                            hex += '   ';
                        }
                        if (j === 7) hex += ' ';
                    }
                    out.push(pad(offset + i, 8) + '  ' + hex + ' |' + asc + '|');
                }
                pre.textContent = out.join('\n');
                render();
            }).catch(function (e) {
                if (ctrl.closed) return;
                message(ctrl, '读取失败：' + (e.message || e), true);
                render();
            });
        }

        function jump(v) {
            var n;
            if (typeof v === 'string' && /^0x/i.test(v.trim())) n = parseInt(v.trim().slice(2), 16);
            else n = parseInt(v, 10);
            if (!isFinite(n) || n < 0) n = 0;
            if (n > size - 1) n = Math.max(0, size - 1);
            offset = Math.floor(n / HEX_PAGE) * HEX_PAGE;
            load();
        }

        go.addEventListener('click', function () { jump(offIn.value); });
        offIn.addEventListener('keydown', function (e) { if (e.key === 'Enter') jump(offIn.value); });
        prev.addEventListener('click', function () {
            if (offset > 0) { offset = Math.max(0, offset - HEX_PAGE); load(); }
        });
        next.addEventListener('click', function () {
            if (offset + HEX_PAGE < size) { offset += HEX_PAGE; load(); }
        });

        render();
        load();
        return ctrl;
    }

    // ---------------------------------------------------------------- Tail

    function openTail(opts) {
        opts = opts || {};
        var size = Number(opts.size) || 0;
        var lines = Number(opts.lines) || 200;
        var ctrl = shell(opts.name || '尾部查看', fmtSize(size));
        ctrl.close = function () { teardown(ctrl, opts.onClose); };

        var sel = document.createElement('select');
        [100, 200, 500, 1000].forEach(function (n) {
            var o = document.createElement('option');
            o.value = String(n);
            o.textContent = n + ' 行';
            if (n === lines) o.selected = true;
            sel.appendChild(o);
        });
        var follow = document.createElement('label');
        var cb = document.createElement('input');
        cb.type = 'checkbox';
        follow.appendChild(cb);
        follow.appendChild(document.createTextNode('跟随 (2s)'));
        var refresh = h('button', '', '刷新');
        var info = h('span', 'muted', '');
        ctrl.bar.appendChild(sel);
        if (opts.stat) ctrl.bar.appendChild(follow);
        ctrl.bar.appendChild(refresh);
        ctrl.bar.appendChild(h('span', 'sp'));
        ctrl.bar.appendChild(info);

        var pre = document.createElement('pre');
        pre.className = 'tail';
        ctrl.body.appendChild(pre);

        var lastSize = -1;
        var loading = false;

        function currentSize() {
            if (!opts.stat) return Promise.resolve(size);
            return Promise.resolve(opts.stat()).then(function (n) {
                size = Number(n) || 0;
                ctrl.size.textContent = fmtSize(size);
                return size;
            });
        }

        function load(quiet) {
            if (loading) return Promise.resolve();
            loading = true;
            return currentSize().then(function (total) {
                var start = Math.max(0, total - TAIL_SPAN);
                var len = total - start;
                if (len <= 0) {
                    pre.textContent = '';
                    info.textContent = '空文件';
                    return null;
                }
                return Promise.resolve(opts.read(start, len)).then(function (bytes) {
                    if (ctrl.closed) return;
                    if (pre.parentNode !== ctrl.body) {
                        while (ctrl.body.firstChild) ctrl.body.removeChild(ctrl.body.firstChild);
                        ctrl.body.appendChild(pre);
                    }
                    var arr = bytes instanceof Uint8Array ? bytes : new Uint8Array(bytes);
                    var text = new TextDecoder('utf-8', { fatal: false }).decode(arr);
                    var all = text.split('\n');
                    if (all.length && all[all.length - 1] === '') all.pop();
                    var shown = all.slice(Math.max(0, all.length - lines));
                    var clipped = start > 0 ? '（仅显示最后 ' + fmtSize(len) + '）' : '';
                    pre.textContent = shown.join('\n');
                    ctrl.body.scrollTop = ctrl.body.scrollHeight;
                    info.textContent = '最后 ' + shown.length + ' 行 · ' + fmtSize(len) + clipped +
                        (quiet ? '' : ' · ' + new Date().toLocaleTimeString());
                });
            }).catch(function (e) {
                if (ctrl.closed) return;
                message(ctrl, '读取失败：' + (e.message || e), true);
            }).finally(function () { loading = false; });
        }

        function startFollow() {
            if (ctrl.timer) clearInterval(ctrl.timer);
            ctrl.timer = setInterval(function () {
                currentSize().then(function (total) {
                    if (ctrl.closed || !cb.checked) return;
                    if (total !== lastSize) { lastSize = total; load(true); }
                }).catch(function () {});
            }, 2000);
        }

        cb.addEventListener('change', function () {
            if (cb.checked) { startFollow(); load(true); }
            else if (ctrl.timer) { clearInterval(ctrl.timer); ctrl.timer = null; }
        });
        sel.addEventListener('change', function () { lines = Number(sel.value) || 200; load(); });
        refresh.addEventListener('click', function () { load(); });

        load();
        return ctrl;
    }

    global.RtunnelFileView = {
        openHex: openHex,
        openTail: openTail,
        isOpen: function () { return !!active; },
        close: function () { if (active) active.close(); }
    };
})(typeof window !== 'undefined' ? window : this);
