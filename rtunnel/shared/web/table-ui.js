/* rtunnel 表格通用行为（本地页、共享组件、管理台共用）
 *
 * 只负责三件事：排序状态的默认方向/切换、行比较、表头箭头。
 * 渲染仍由各页面决定（本地页拼 HTML，组件用 DOM，管理台用 el-table）。
 *
 *   var state = { key: 'cpu', dir: -1 };
 *   RtunnelTable.toggle(state, 'name', 'text');   // 首次点文本列从升序开始
 *   RtunnelTable.arrow(state, 'name');            // ▲ / ▼ / ''
 *   var rows = RtunnelTable.sortRows(rows, state, cols);
 */
(function (global) {
    'use strict';

    var ASC = '▲';
    var DESC = '▼';

    // 数值列首次点击降序，文本列首次点击升序
    function defaultDir(type) {
        return type === 'num' ? -1 : 1;
    }

    function toggle(state, key, type) {
        var dir = state.dir || defaultDir(type);
        if (state.key === key) dir = dir === 1 ? -1 : 1;
        else dir = defaultDir(type);
        state.key = key;
        state.dir = dir;
        return dir;
    }

    function arrow(state, key) {
        if (!state || state.key !== key) return '';
        return (state.dir || 1) > 0 ? ASC : DESC;
    }

    function valueOf(row, col) {
        if (col.value) return col.value(row);
        return row ? row[col.key] : undefined;
    }

    function compare(a, b, col) {
        var x = valueOf(a, col);
        var y = valueOf(b, col);
        if (col.type === 'num') {
            x = Number(x);
            y = Number(y);
            if (!isFinite(x)) x = 0;
            if (!isFinite(y)) y = 0;
            return x - y;
        }
        return String(x == null ? '' : x).localeCompare(String(y == null ? '' : y), 'zh-CN');
    }

    function sortRows(rows, state, cols) {
        var col = null;
        for (var i = 0; i < cols.length; i++) {
            if (cols[i].key === state.key) { col = cols[i]; break; }
        }
        if (!col) return rows;
        var dir = state.dir || defaultDir(col.type);
        return rows.slice().sort(function (a, b) { return dir * compare(a, b, col); });
    }

    // 生成 DOM 表头，返回 { tr, update(state) }；列定义见文件头注释
    function createHead(cols, opts) {
        opts = opts || {};
        var tr = document.createElement('tr');
        var arrows = {};
        cols.forEach(function (c) {
            var th = document.createElement('th');
            if (c.cls) th.className = c.cls;
            th.appendChild(document.createTextNode(c.label));
            if (c.key) {
                th.classList.add('sortable');
                th.title = '点击排序';
                var arw = document.createElement('span');
                arw.className = 'arw';
                th.appendChild(arw);
                arrows[c.key] = arw;
                th.addEventListener('click', function () {
                    if (opts.onSort) opts.onSort(c.key, c.type);
                });
            }
            tr.appendChild(th);
        });
        return {
            tr: tr,
            update: function (state) {
                for (var k in arrows) arrows[k].textContent = arrow(state, k);
            }
        };
    }

    // 字符串表格用：返回 <tr>...</tr>
    function headHTML(cols, state) {
        return '<tr>' + cols.map(function (c) {
            var cls = c.cls ? c.cls + ' ' : '';
            if (!c.key) return '<th' + (cls ? ' class="' + c.cls + '"' : '') + '>' + c.label + '</th>';
            return '<th class="' + cls + 'sortable" data-sort="' + c.key + '">' +
                c.label + '<span class="arw">' + arrow(state, c.key) + '</span></th>';
        }).join('') + '</tr>';
    }

    global.RtunnelTable = {
        defaultDir: defaultDir,
        toggle: toggle,
        arrow: arrow,
        sortRows: sortRows,
        createHead: createHead,
        headHTML: headHTML
    };
})(window);
