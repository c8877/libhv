<template>
  <div>
    <div class="panel">
      <div class="panel-head">
        <h2>{{ devid }}</h2>
        <el-tag type="success" size="small">在线</el-tag>
        <span class="spacer" />
        <el-switch v-model="auto" active-text="自动刷新" @change="onAutoChange" />
        <el-select v-model="period" class="period" @change="onAutoChange">
          <el-option v-for="p in [5, 10, 30]" :key="p" :label="p + 's'" :value="p" />
        </el-select>
        <el-button :loading="loading" @click="loadAll({ notify: true })">刷新</el-button>
        <el-button @click="exportJson">导出 JSON</el-button>
      </div>

      <el-descriptions v-if="detail" :column="descCols" size="small" border>
        <el-descriptions-item label="远端地址">{{ detail.remote_addr || '—' }}</el-descriptions-item>
        <el-descriptions-item label="接入时间">{{ time(detail.connected_at_ms) }}</el-descriptions-item>
        <el-descriptions-item label="最近活跃">{{ ago(detail.last_seen_ms) }} 前</el-descriptions-item>
        <el-descriptions-item label="流量 ↓/↑">
          {{ bytes(detail.bytes_in) }} / {{ bytes(detail.bytes_out) }}
        </el-descriptions-item>
      </el-descriptions>
    </div>

    <div class="grid-2">
      <div class="panel">
        <div class="panel-head">
          <h2>存储</h2>
          <span class="spacer" />
          <span class="muted">条形按最大分区归一</span>
        </div>
        <div v-for="d in disks" :key="d.mount" class="disk">
          <div class="disk-top">
            <span class="mount">{{ d.mount }}</span>
            <span v-if="d.fstype" class="fs">{{ d.fstype }}</span>
            <span class="pct" :class="toneOf(d.pct)">{{ d.pct }}%</span>
          </div>
          <el-progress :percentage="d.pct" :stroke-width="6" :show-text="false" :color="diskColor(d.pct)" />
          <div class="size">{{ d.used }} / {{ d.total }} {{ d.unit }}</div>
        </div>
        <el-empty v-if="sys && !disks.length" description="无挂载点" :image-size="60" />
        <el-skeleton v-else-if="!sys" :rows="3" animated />
      </div>

      <div class="panel">
        <div class="panel-head"><h2>网卡</h2></div>
        <el-table :data="nics" size="small">
          <el-table-column prop="name" label="接口" min-width="90" />
          <el-table-column prop="ip" label="IP" min-width="130" />
          <el-table-column v-if="!isPhone" prop="netmask" label="掩码" min-width="120" />
          <el-table-column label="状态" width="80">
            <template #default="{ row }">
              <el-tag :type="row.up ? 'success' : 'danger'" size="small">{{ row.up ? 'up' : 'down' }}</el-tag>
            </template>
          </el-table-column>
          <template #empty><div class="muted">暂无网卡信息</div></template>
        </el-table>
      </div>
    </div>

    <FileManagerCard :devid="devid" :sys="sys" :can-edit="canEdit" />

    <SvcManagerCard :devid="devid" :sys="sys" :can-edit="canEdit" />

    <div class="panel">
      <div class="panel-head"><h2>系统信息</h2></div>
      <el-descriptions v-if="sys" :column="sysCols" size="small" border>
        <el-descriptions-item label="架构">{{ sys.machine || '—' }}</el-descriptions-item>
        <el-descriptions-item label="内核">{{ sys.kernel || '—' }}</el-descriptions-item>
        <el-descriptions-item label="CPU 型号">{{ sys.cpu_model || '—' }}</el-descriptions-item>
        <el-descriptions-item label="核心数">{{ sys.cpu_count || 0 }} 核</el-descriptions-item>
        <el-descriptions-item label="板卡型号">{{ sys.board || '—' }}</el-descriptions-item>
        <el-descriptions-item label="采集时间">{{ time(sysTs) }}</el-descriptions-item>
      </el-descriptions>
      <el-alert v-else-if="sysErr" :title="sysErr" type="warning" :closable="false" show-icon />
      <el-skeleton v-else :rows="2" animated />
    </div>

    <div class="panel">
      <div class="panel-head"><h2>能力配置</h2></div>
      <div v-if="sys" class="kv-rows">
        <div class="kv-row">
          <span class="caret" />
          <span class="k">agent 版本</span>
          <span class="v mono">{{ sys.version || '—' }}</span>
        </div>
        <div class="kv-row">
          <span class="caret" />
          <span class="k">启动参数</span>
          <span class="v"><code class="args">{{ sys.args || '—' }}</code></span>
        </div>
        <div class="kv-row caps-toggle"
             :class="{ clickable: canOpenCaps, open: capsOpen }"
             :role="canOpenCaps ? 'button' : null"
             :tabindex="canOpenCaps ? 0 : null"
             :aria-expanded="canOpenCaps ? String(capsOpen) : null"
             @click="onCapsToggle"
             @keydown.enter.prevent="onCapsToggle"
             @keydown.space.prevent="onCapsToggle">
          <span class="caret">
            <el-icon v-if="canOpenCaps" :class="{ open: capsOpen }"><ArrowRight /></el-icon>
          </span>
          <span class="k">能力</span>
          <span class="v chips">
            <el-tag v-for="c in capsChips" :key="c" size="small" effect="plain">{{ c }}</el-tag>
            <span v-if="!capsChips.length" class="muted">—</span>
          </span>
          <span v-if="canOpenCaps" class="caps-hint">{{ capsOpen ? '点击收起' : '点击配置' }}</span>
        </div>
        <el-collapse-transition>
          <div v-if="capsOpen && sys" class="caps-edit">
            <div class="caps-runtime">
              <span class="muted">运行时配置</span>
              <code class="mono">{{ (sys.caps && sys.caps.config_file) || '—' }}</code>
              <el-tag v-if="!sys.caps || !sys.caps.config_file" size="small" type="info" effect="plain">agent 未上报</el-tag>
              <el-tag v-else-if="sys.caps.config_loaded" size="small" type="success" effect="plain">已从文件加载</el-tag>
              <el-tag v-else size="small" type="warning" effect="plain">使用命令行参数</el-tag>
              <span class="muted">agent {{ (sys.caps && sys.caps.version) || '—' }}</span>
              <span class="spacer" />
              <el-button link size="small" :loading="capsRefreshing" @click="refreshCaps">刷新运行时</el-button>
            </div>
            <div class="caps-form">
              <div class="caps-grid">
                <div class="vform-section span2">访问控制</div>
                <div class="vform-row span2">
                  <span class="vlabel">转发端口</span>
                  <div class="vcontrol">
                    <el-input v-model="capsForm.ports" size="small" :disabled="!canCaps" placeholder="22, 8080, 8099" />
                  </div>
                </div>
                <div class="vform-row">
                  <span class="vlabel">允许 Shell</span>
                  <div class="vcontrol">
                    <el-checkbox v-model="capsForm.shell" :disabled="!canCaps">允许</el-checkbox>
                  </div>
                </div>
                <div class="vform-row">
                  <span class="vlabel">OTA 升级</span>
                  <div class="vcontrol">
                    <el-switch v-model="capsForm.ota" :disabled="!canOta" />
                    <span class="muted">接收升级包</span>
                  </div>
                </div>

                <div class="vform-section span2">文件与数据</div>
                <div class="vform-row span2">
                  <span class="vlabel">数据目录</span>
                  <div class="vcontrol stacked">
                    <el-input v-model="capsForm.file_dir" size="small" :disabled="!canCaps" placeholder="/opt/data/files" />
                    <div class="vform-note">升级包存放目录；文件根目录留空时也用它</div>
                  </div>
                </div>
                <template v-if="capsForm.fsSupported">
                  <div class="vform-row">
                    <span class="vlabel">启用文件管理</span>
                    <div class="vcontrol">
                      <el-checkbox v-model="capsForm.fs" :disabled="!canCaps">启用</el-checkbox>
                    </div>
                  </div>
                  <div class="vform-row">
                    <span class="vlabel">允许写入</span>
                    <div class="vcontrol">
                      <el-switch v-model="capsForm.fs_rw" :disabled="!canCaps || !capsForm.fs" />
                      <span class="muted">关闭后只读</span>
                    </div>
                  </div>
                  <div class="vform-row span2">
                    <span class="vlabel">文件根目录</span>
                    <div class="vcontrol">
                      <el-input v-model="capsForm.fs_root" size="small" :disabled="!canCaps || !capsForm.fs"
                                placeholder="/opt/data/files" />
                    </div>
                  </div>
                  <div class="vform-row">
                    <span class="vlabel">单文件上限</span>
                    <div class="vcontrol">
                      <el-input-number v-model="capsForm.fs_max_mb" :min="1" :max="10240"
                                       :disabled="!canCaps || !capsForm.fs" size="small" />
                      <span class="vunit">MB</span>
                    </div>
                  </div>
                </template>
                <div v-else class="vform-note muted span2">当前 agent 版本不支持文件管理配置，升级 agent 后可在此开启</div>

                <div class="vform-section span2">服务管理</div>
                <template v-if="capsForm.svcSupported">
                  <div class="vform-row">
                    <span class="vlabel">启用服务管理</span>
                    <div class="vcontrol">
                      <el-checkbox v-model="capsForm.svc" :disabled="!canCaps">启用</el-checkbox>
                      <span class="muted">systemd 状态与日志</span>
                    </div>
                  </div>
                  <div class="vform-row">
                    <span class="vlabel">允许控制</span>
                    <div class="vcontrol">
                      <el-switch v-model="capsForm.svc_rw" :disabled="!canCaps || !capsForm.svc" />
                      <span class="muted">关闭后只读</span>
                    </div>
                  </div>
                </template>
                <div v-else class="vform-note muted span2">当前 agent 版本不支持服务管理配置</div>

                <div class="vform-section span2">串口白名单</div>
                <div class="vform-row span2">
                  <span class="vlabel">允许串口</span>
                  <div class="vcontrol tty-allow">
                    <el-tag v-for="(p, i) in capsForm.allow_tty" :key="p" size="small"
                            :closable="canTty" @close="removeAllowTty(i)">{{ p }}</el-tag>
                    <el-input v-if="allowTtyOpen" v-model="allowTtyInput" size="small" class="allow-input"
                              placeholder="/dev/ttyS2" @keyup.enter="addAllowTty" @blur="addAllowTty" />
                    <el-button v-else size="small" :disabled="!canTty" @click="allowTtyOpen = true">+ 添加</el-button>
                    <div class="vform-note">留空时只允许串口配置里启用的设备</div>
                  </div>
                </div>
              </div>
              <div class="form-actions">
                <span v-if="!canCaps && !canOta && !canTty" class="muted">没有修改能力配置的权限</span>
                <span class="spacer" />
                <el-button size="small" @click="capsOpen = false">取消</el-button>
                <el-button size="small" type="primary" :loading="capsSaving"
                           :disabled="!canCaps && !canOta && !canTty" @click="saveCaps">保存并下发</el-button>
              </div>
            </div>
          </div>
        </el-collapse-transition>
      </div>
      <el-skeleton v-else :rows="3" animated />
    </div>

    <div class="panel">
      <div class="panel-head"><h2>证书</h2></div>
      <el-descriptions v-if="sys && sys.mtls" :column="descCols" size="small" border>
        <el-descriptions-item label="证书主体"><span class="mono">{{ sys.cert_subject || '—' }}</span></el-descriptions-item>
        <el-descriptions-item label="签发者"><span class="mono">{{ sys.cert_issuer || '—' }}</span></el-descriptions-item>
        <el-descriptions-item label="序列号"><span class="mono">{{ sys.cert_serial || '—' }}</span></el-descriptions-item>
        <el-descriptions-item label="状态">
          <el-tag :type="sys.cert_valid ? 'success' : 'danger'" size="small">
            {{ sys.cert_valid ? '有效' : '已过期' }}
          </el-tag>
        </el-descriptions-item>
      </el-descriptions>
      <div v-else-if="sys" class="muted">未启用 mTLS</div>
      <el-skeleton v-else :rows="2" animated />
    </div>

    <div class="panel">
      <div class="panel-head series-head">
        <h2>采样曲线</h2>
        <span class="spacer" />
        <el-switch v-model="sampleAuto" active-text="自动采集" @change="onSampleAuto" />
        <el-button size="small" @click="selectAllSeries">全选</el-button>
        <el-button size="small" @click="clearSeries">清空</el-button>
        <el-button size="small" @click="pullSeries(true)">全量</el-button>
      </div>
      <div class="series-bar">
        <span class="series-label">指标</span>
        <el-popover v-model:visible="seriesPopOpen" placement="bottom-start" :width="250" trigger="click">
          <template #reference>
            <div ref="seriesWrapEl" class="series-select" role="button" tabindex="0"
                 :aria-expanded="String(seriesPopOpen)"
                 @keydown.enter.prevent="seriesPopOpen = !seriesPopOpen"
                 @keydown.space.prevent="seriesPopOpen = !seriesPopOpen">
              <template v-if="selected.length">
                <span v-for="(s, i) in selected" v-show="isPhone || i < seriesVisible" :key="s" class="series-chip">
                  {{ s }}<button class="chip-x" aria-label="移除" @click.stop="removeSeries(s)">×</button>
                </span>
                <span v-if="seriesHidden" class="series-more">+{{ seriesHidden }}<span class="more-caret">▾</span></span>
              </template>
              <span v-else class="series-empty">选择指标</span>
              <div v-if="!isPhone" ref="seriesMeasureEl" class="series-measure" aria-hidden="true">
                <span v-for="s in selected" :key="'m-' + s" class="series-chip">{{ s }}<span class="chip-x">×</span></span>
              </div>
            </div>
          </template>
          <div class="series-pop">
            <div class="series-pop-head">已选 {{ selected.length }} / {{ enabledCfg.length }}</div>
            <el-checkbox-group v-model="selected" class="series-options" @change="onSeriesPick">
              <el-checkbox v-for="c in enabledCfg" :key="c.series" :value="c.series">{{ c.series }}</el-checkbox>
            </el-checkbox-group>
            <div class="series-pop-actions">
              <el-button size="small" text @click="selectAllSeries">全选</el-button>
              <el-button size="small" text @click="clearSeries">清空</el-button>
            </div>
          </div>
        </el-popover>
      </div>
      <div v-if="!enabledCfg.length" class="muted">该设备还没有启用的采样指标，可在下方「采样配置」中添加</div>
      <template v-else>
        <div v-show="hasUnit('percent')" class="chart-box">
          <div class="chart-title">百分比</div>
          <canvas ref="percentEl" />
        </div>
        <div v-show="hasUnit('load')" class="chart-box">
          <div class="chart-title">负载</div>
          <canvas ref="loadEl" />
        </div>
        <div v-show="hasUnit('rate')" class="chart-box">
          <div class="chart-title">速率</div>
          <canvas ref="rateEl" />
        </div>
      </template>
    </div>

    <div class="panel">
      <div class="panel-head">
        <h2>采样配置</h2>
        <span class="muted">{{ seriesCfg.length }} 项 · {{ unsyncedCount }} 项未同步</span>
        <span class="spacer" />
        <el-button v-if="canEdit" size="small" @click="openAddMetric">+ 添加指标</el-button>
      </div>
      <el-table ref="metricTable" class="row-edit-table" :data="seriesCfg" size="small" row-key="series" :show-header="!isPhone" @row-click="onMetricRow">
        <el-table-column type="expand">
          <template #default="{ row }">
            <div class="metric-edit" @click.stop>
              <div class="vform metric-form">
                <div class="vform-row">
                  <span class="vlabel">采样间隔</span>
                  <div class="vcontrol">
                    <el-input-number v-model="row.interval_sec" :min="1" :max="300" :disabled="!canEdit" size="small" />
                    <span class="vunit">秒</span>
                  </div>
                </div>
                <div class="vform-row">
                  <span class="vlabel">保留点数</span>
                  <div class="vcontrol">
                    <el-input-number v-model="row.points" :min="60" :max="86400" :disabled="!canEdit" size="small" />
                    <span class="vunit">点</span>
                  </div>
                </div>
                <div class="vform-row">
                  <span class="vlabel">预计保留</span>
                  <div class="vcontrol"><span class="vreadonly num">{{ dur(row) }}</span></div>
                </div>
                <div class="vform-row">
                  <span class="vlabel">同步状态</span>
                  <div class="vcontrol"><span class="muted">{{ metricHint(row) }}</span></div>
                </div>
                <div class="vform-actions">
                  <span class="spacer" />
                  <el-button v-if="canEdit" size="small" type="danger" plain @click="deleteMetric(row)">删除</el-button>
                  <el-button size="small" @click="cancelMetric">取消</el-button>
                  <el-button v-if="canEdit" size="small" type="primary" @click="saveMetricRow(row)">保存并下发</el-button>
                </div>
              </div>
            </div>
          </template>
        </el-table-column>
        <el-table-column v-if="isPhone" min-width="0">
          <template #default="{ row }">
            <div class="metric-card">
              <div class="metric-card-top">
                <b>{{ row.series }}</b><span class="sub">{{ unitLabel(row.series) }}</span>
                <span class="grow" />
                <el-switch :model-value="row.enabled" :disabled="!canEdit" @click.stop @change="(v) => toggleMetric(row, v)" />
              </div>
              <div class="metric-card-meta metric-card-sub">
                <span class="num">{{ row.interval_sec }} s · {{ row.points }} 点 · {{ dur(row) }}</span>
                <span class="grow" />
                <span class="sync" :class="row.enabled ? (row.synced ? 'ok' : 'warn') : 'off'">
                  {{ row.enabled ? (row.synced ? '已同步' : '未同步') : '禁用' }}
                </span>
              </div>
            </div>
          </template>
        </el-table-column>
        <el-table-column v-if="!isPhone" label="指标" min-width="150">
          <template #default="{ row }">
            <b>{{ row.series }}</b><span class="sub">{{ unitLabel(row.series) }}</span>
          </template>
        </el-table-column>
        <el-table-column v-if="!isPhone" label="间隔" width="90">
          <template #default="{ row }"><span class="num">{{ row.interval_sec }} s</span></template>
        </el-table-column>
        <el-table-column v-if="!isPhone" label="保留" min-width="130">
          <template #default="{ row }">
            <span class="num">{{ row.points }} 点</span><span class="sub">· {{ dur(row) }}</span>
          </template>
        </el-table-column>
        <el-table-column v-if="!isPhone" label="同步" width="110">
          <template #default="{ row }">
            <span class="sync" :class="row.enabled ? (row.synced ? 'ok' : 'warn') : 'off'">
              {{ row.enabled ? (row.synced ? '已同步' : '未同步') : '禁用' }}
            </span>
          </template>
        </el-table-column>
        <el-table-column v-if="!isPhone" label="启用" width="80">
          <template #default="{ row }">
            <el-switch :model-value="row.enabled" :disabled="!canEdit" @click.stop @change="(v) => toggleMetric(row, v)" />
          </template>
        </el-table-column>
        <template #empty><div class="muted">暂无配置</div></template>
      </el-table>
    </div>

    <div class="panel">
      <div class="panel-head">
        <h2>串口 TTY</h2>
        <span class="spacer" />
        <el-input v-model="ttyQuery" size="small" class="tty-search" placeholder="搜索别名 / 路径" clearable />
        <el-button size="small" @click="loadTtys(true)">刷新</el-button>
        <el-button v-if="canEdit" size="small" @click="manualAddTty">手动添加</el-button>
      </div>
      <el-tabs v-model="ttyTab">
        <el-tab-pane :label="'已配置 (' + ttys.length + ')'" name="config">
          <el-table ref="ttyTable" class="row-edit-table" :data="filteredTtys" size="small" row-key="path" :show-header="!isPhone"
                    @row-click="onTtyRow" @expand-change="onTtyExpand">
            <el-table-column type="expand">
              <template #default="{ row }">
                <div v-if="ttyExpandedPath === row.path" class="tty-edit" @click.stop>
                  <div class="vform tty-vform">
                    <div class="tty-grid">
                      <div class="vform-row">
                        <span class="vlabel">别名</span>
                        <div class="vcontrol"><el-input v-model="ttyEditing.alias" size="small" :disabled="!canEdit" placeholder="设备内唯一，可留空" /></div>
                      </div>
                      <div class="vform-row">
                        <span class="vlabel">启用</span>
                        <div class="vcontrol">
                          <el-switch v-model="ttyEditing.enabled" :disabled="!canEdit" />
                          <span class="muted">{{ ttyEditing.enabled ? '启用' : '禁用' }}</span>
                        </div>
                      </div>
                      <div class="vform-row span2">
                        <span class="vlabel">路径</span>
                        <div class="vcontrol path-line">
                          <span class="vreadonly mono">{{ row.path }}</span>
                          <el-button size="small" text @click="copyText(row.path, '路径已复制')">复制路径</el-button>
                          <el-button v-if="canTty" size="small" text @click="takeoverTty(row)">强制接管</el-button>
                        </div>
                      </div>
                    <div class="vform-row">
                      <span class="vlabel">别名</span>
                      <div class="vcontrol"><el-input v-model="ttyEditing.alias" size="small" :disabled="!canEdit" placeholder="设备内唯一，可留空" /></div>
                    </div>
                    <div class="vform-row">
                      <span class="vlabel">启用</span>
                      <div class="vcontrol">
                        <el-switch v-model="ttyEditing.enabled" :disabled="!canEdit" />
                        <span class="muted">{{ ttyEditing.enabled ? '启用' : '禁用' }}</span>
                      </div>
                    </div>
                    <div class="vform-row">
                      <span class="vlabel">波特率</span>
                      <div class="vcontrol">
                        <el-select v-model="ttyEditing.baud" size="small" :disabled="!canEdit">
                          <el-option v-for="b in bauds" :key="b" :label="b" :value="b" />
                        </el-select>
                      </div>
                    </div>
                    <div class="vform-row">
                      <span class="vlabel">数据位</span>
                      <div class="vcontrol">
                        <el-select v-model="ttyEditing.data_bits" size="small" :disabled="!canEdit">
                          <el-option v-for="b in [5, 6, 7, 8]" :key="b" :label="b" :value="b" />
                        </el-select>
                      </div>
                    </div>
                    <div class="vform-row">
                      <span class="vlabel">校验</span>
                      <div class="vcontrol">
                        <el-select v-model="ttyEditing.parity" size="small" :disabled="!canEdit">
                          <el-option label="无 (N)" value="N" />
                          <el-option label="偶 (E)" value="E" />
                          <el-option label="奇 (O)" value="O" />
                        </el-select>
                      </div>
                    </div>
                    <div class="vform-row">
                      <span class="vlabel">停止位</span>
                      <div class="vcontrol">
                        <el-select v-model="ttyEditing.stop_bits" size="small" :disabled="!canEdit">
                          <el-option :label="1" :value="1" />
                          <el-option :label="2" :value="2" />
                        </el-select>
                      </div>
                    </div>
                    </div>
                    <div class="vform-actions">
                      <span class="grow" />
                      <el-button v-if="canEdit" size="small" type="danger" plain @click="deleteTty(row)">删除</el-button>
                      <el-button size="small" @click="collapseTtyRow">取消</el-button>
                      <el-button v-if="canEdit" size="small" type="primary" :loading="ttySaving" @click="saveTtyRow">保存并下发</el-button>
                    </div>
                  </div>
                </div>
              </template>
            </el-table-column>
            <el-table-column v-if="isPhone" min-width="0">
              <template #default="{ row }">
                <div class="tty-card">
                  <div class="tty-card-top">
                    <b>{{ row.alias || '未命名' }}</b>
                    <el-tag :type="row.enabled ? 'success' : 'info'" size="small">{{ row.enabled ? '启用' : '禁用' }}</el-tag>
                    <span class="grow" />
                    <el-button size="small" type="primary" link @click.stop="openTtyConsole(row)">调试</el-button>
                  </div>
                  <div class="path" :title="row.path">{{ row.path }}</div>
                  <div class="tty-card-meta">
                    <span>{{ row.baud }} · {{ row.data_bits }}{{ row.parity }}{{ row.stop_bits }}</span>
                    <span class="num">RX {{ ttyRx(row.path) }}</span>
                    <span class="num">TX {{ ttyTx(row.path) }}</span>
                  </div>
                </div>
              </template>
            </el-table-column>
            <el-table-column v-if="!isPhone" label="设备" min-width="180">
              <template #default="{ row }">
                <b>{{ row.alias || '—' }}</b>
                <div class="path" :title="row.path">{{ row.path }}</div>
              </template>
            </el-table-column>
            <el-table-column v-if="!isPhone" label="状态" width="80">
              <template #default="{ row }">
                <el-tag :type="row.enabled ? 'success' : 'info'" size="small">{{ row.enabled ? '启用' : '禁用' }}</el-tag>
              </template>
            </el-table-column>
            <el-table-column v-if="!isPhone" label="参数" min-width="120">
              <template #default="{ row }">{{ row.baud }} · {{ row.data_bits }}{{ row.parity }}{{ row.stop_bits }}</template>
            </el-table-column>
            <el-table-column v-if="!isPhone" label="RX" width="90">
              <template #default="{ row }"><span class="num">{{ ttyRx(row.path) }}</span></template>
            </el-table-column>
            <el-table-column v-if="!isPhone" label="TX" width="90">
              <template #default="{ row }"><span class="num">{{ ttyTx(row.path) }}</span></template>
            </el-table-column>
            <el-table-column v-if="!isPhone" label="调试" width="70">
              <template #default="{ row }">
                <el-button size="small" type="primary" link @click.stop="openTtyConsole(row)">调试</el-button>
              </template>
            </el-table-column>
            <template #empty><div class="muted">暂无已配置串口</div></template>
          </el-table>
        </el-tab-pane>
        <el-tab-pane :label="'未配置 (' + unconfigured.length + ')'" name="unconfig">
          <el-table ref="ttyAddTable" class="row-edit-table" :data="filteredUnconfigured" size="small" row-key="path" :show-header="!isPhone"
                    @row-click="onTtyAddRow" @expand-change="onTtyAddExpand">
            <el-table-column type="expand">
              <template #default="{ row }">
                <div v-if="ttyAddPath === row.path" class="tty-edit" @click.stop>
                  <div class="vform tty-vform">
                    <div class="tty-grid">
                      <div class="vform-row span2">
                        <span class="vlabel">路径</span>
                        <div class="vcontrol"><span class="vreadonly mono">{{ row.path }}</span></div>
                      </div>
                    <div class="vform-row">
                      <span class="vlabel">别名</span>
                      <div class="vcontrol"><el-input v-model="ttyAddForm.alias" size="small" :disabled="!canEdit" placeholder="设备内唯一，可留空" /></div>
                    </div>
                    <div class="vform-row">
                      <span class="vlabel">启用</span>
                      <div class="vcontrol">
                        <el-switch v-model="ttyAddForm.enabled" :disabled="!canEdit" />
                        <span class="muted">{{ ttyAddForm.enabled ? '启用' : '禁用' }}</span>
                      </div>
                    </div>
                    <div class="vform-row">
                      <span class="vlabel">波特率</span>
                      <div class="vcontrol">
                        <el-select v-model="ttyAddForm.baud" size="small" :disabled="!canEdit">
                          <el-option v-for="b in bauds" :key="b" :label="b" :value="b" />
                        </el-select>
                      </div>
                    </div>
                    <div class="vform-row">
                      <span class="vlabel">数据位</span>
                      <div class="vcontrol">
                        <el-select v-model="ttyAddForm.data_bits" size="small" :disabled="!canEdit">
                          <el-option v-for="b in [5, 6, 7, 8]" :key="b" :label="b" :value="b" />
                        </el-select>
                      </div>
                    </div>
                    <div class="vform-row">
                      <span class="vlabel">校验</span>
                      <div class="vcontrol">
                        <el-select v-model="ttyAddForm.parity" size="small" :disabled="!canEdit">
                          <el-option label="无 (N)" value="N" />
                          <el-option label="偶 (E)" value="E" />
                          <el-option label="奇 (O)" value="O" />
                        </el-select>
                      </div>
                    </div>
                    <div class="vform-row">
                      <span class="vlabel">停止位</span>
                      <div class="vcontrol">
                        <el-select v-model="ttyAddForm.stop_bits" size="small" :disabled="!canEdit">
                          <el-option :label="1" :value="1" />
                          <el-option :label="2" :value="2" />
                        </el-select>
                      </div>
                    </div>
                    </div>
                    <div class="vform-actions">
                      <span class="grow" />
                      <el-button size="small" @click="collapseTtyAddRow">取消</el-button>
                      <el-button size="small" type="primary" :loading="ttySaving" @click="saveTtyAdd">添加</el-button>
                    </div>
                  </div>
                </div>
              </template>
            </el-table-column>
            <el-table-column v-if="isPhone" min-width="0">
              <template #default="{ row }">
                <div class="tty-card">
                  <div class="tty-card-top">
                    <b class="muted">未配置</b>
                    <span class="grow" />
                    <span class="muted">{{ canEdit ? '点击添加' : '—' }}</span>
                  </div>
                  <div class="path" :title="row.path">{{ row.path }}</div>
                  <div class="tty-card-meta">
                    <span class="num">RX {{ ttyRx(row.path) }}</span>
                    <span class="num">TX {{ ttyTx(row.path) }}</span>
                  </div>
                </div>
              </template>
            </el-table-column>
            <el-table-column v-if="!isPhone" label="设备" min-width="180">
              <template #default="{ row }">
                <b class="muted">未配置</b>
                <div class="path" :title="row.path">{{ row.path }}</div>
              </template>
            </el-table-column>
            <el-table-column v-if="!isPhone" label="状态" width="80"><span class="muted">未配置</span></el-table-column>
            <el-table-column v-if="!isPhone" label="RX" width="90">
              <template #default="{ row }"><span class="num">{{ ttyRx(row.path) }}</span></template>
            </el-table-column>
            <el-table-column v-if="!isPhone" label="TX" width="90">
              <template #default="{ row }"><span class="num">{{ ttyTx(row.path) }}</span></template>
            </el-table-column>
            <el-table-column v-if="!isPhone" label="配置" width="90">
              <template #default="{ row }"><span class="muted">{{ canEdit ? '点击添加' : '—' }}</span></template>
            </el-table-column>
            <template #empty><div class="muted">{{ ttyQuery ? '没有匹配的串口' : '没有未配置的串口' }}</div></template>
          </el-table>
        </el-tab-pane>
      </el-tabs>
    </div>

    <div class="panel">
      <div class="panel-head">
        <h2>会话</h2>
        <span class="muted">共 {{ streams.length }} 条</span>
      </div>
      <el-table :data="streams" size="small">
        <el-table-column label="A 用户:sid" min-width="160">
          <template #default="{ row }">{{ row.a_user }}:{{ row.a_sid }}</template>
        </el-table-column>
        <el-table-column v-if="!isPhone" prop="c_sid" label="c_sid" min-width="100" />
        <el-table-column label="状态" width="90">
          <template #default="{ row }">
            <el-tag :type="row.opened && !row.a_closed && !row.c_closed ? 'success' : 'info'" size="small">
              {{ row.opened && !row.a_closed && !row.c_closed ? '活跃' : '关闭' }}
            </el-tag>
          </template>
        </el-table-column>
        <el-table-column v-if="!isPhone" label="开启于" min-width="150">
          <template #default="{ row }">{{ time(row.opened_at_ms) }}</template>
        </el-table-column>
        <template #empty><div class="muted">当前无会话</div></template>
      </el-table>
    </div>

    <el-dialog v-model="addMetricOpen" title="添加采样指标" width="420px">
      <el-checkbox-group v-model="addMetricPick" class="cand-list">
        <el-checkbox v-for="c in addCandidates" :key="c" :value="c">{{ c }}</el-checkbox>
      </el-checkbox-group>
      <div v-if="!addCandidates.length" class="muted">没有可添加的指标</div>
      <template #footer>
        <el-button @click="addMetricOpen = false">取消</el-button>
        <el-button type="primary" :disabled="!addMetricPick.length" @click="addMetrics">添加</el-button>
      </template>
    </el-dialog>

    <el-dialog v-model="ttyDialog" :title="ttyEditing.isNew ? '添加串口' : '编辑串口'" width="480px">
      <div class="tty-form">
        <label>别名<el-input v-model="ttyEditing.alias" placeholder="设备内唯一，可留空" /></label>
        <label>路径<el-input v-model="ttyEditing.path" :disabled="!ttyEditing.isNew" placeholder="/dev/ttyS0" /></label>
        <label>波特率
          <el-select v-model="ttyEditing.baud">
            <el-option v-for="b in bauds" :key="b" :label="b" :value="b" />
          </el-select>
        </label>
        <label>数据位
          <el-select v-model="ttyEditing.data_bits">
            <el-option v-for="b in [5, 6, 7, 8]" :key="b" :label="b" :value="b" />
          </el-select>
        </label>
        <label>校验
          <el-select v-model="ttyEditing.parity">
            <el-option label="无 (N)" value="N" />
            <el-option label="偶 (E)" value="E" />
            <el-option label="奇 (O)" value="O" />
          </el-select>
        </label>
        <label>停止位
          <el-select v-model="ttyEditing.stop_bits">
            <el-option :label="1" :value="1" />
            <el-option :label="2" :value="2" />
          </el-select>
        </label>
        <label class="check"><el-checkbox v-model="ttyEditing.enabled">启用</el-checkbox></label>
      </div>
      <div class="tty-form-extra">
        <el-button v-if="!ttyEditing.isNew" size="small" @click="copyText(ttyEditing.path, '路径已复制')">复制路径</el-button>
        <el-button v-if="!ttyEditing.isNew && canTty" size="small" @click="takeoverTty(ttyEditing)">强制接管</el-button>
      </div>
      <template #footer>
        <el-button @click="ttyDialog = false">取消</el-button>
        <el-button type="primary" :loading="ttySaving" @click="saveTty">保存并下发</el-button>
      </template>
    </el-dialog>
  </div>
</template>

<script setup>
import { computed, nextTick, onBeforeUnmount, onMounted, reactive, ref, watch } from 'vue'
import { useRoute, useRouter } from 'vue-router'
import Chart from 'chart.js/auto'
import { ArrowRight } from '@element-plus/icons-vue'
import { Api } from '@/shared/api.js'
import { can, canManage } from '@/shared/store.js'
import { useBreakpoint } from '@/shared/useBreakpoint.js'
import { ago, bytes, copyText, downloadJson, kbPair, time } from '@/shared/utils.js'
import FileManagerCard from '@/components/FileManagerCard.vue'
import SvcManagerCard from '@/components/SvcManagerCard.vue'

const route = useRoute()
const router = useRouter()
const { isPhone, isTablet } = useBreakpoint()
const devid = computed(() => decodeURIComponent(route.params.devid || ''))
const agentBase = computed(() => '/api/v1/agents/' + encodeURIComponent(devid.value))
const descCols = computed(() => (isPhone.value ? 1 : isTablet.value ? 2 : 4))
const sysCols = computed(() => (isPhone.value ? 1 : 3))
const canEdit = computed(() => canManage())
const canCaps = computed(() => can('device.caps.write'))
const canOta = computed(() => can('device.ota.write'))
const canTty = computed(() => can('device.tty.write'))
const canOpenCaps = computed(() => canCaps.value || canOta.value || canTty.value)

const detail = ref(null)
const sys = ref(null)
const sysTs = ref(0)
const sysErr = ref('')
const loading = ref(false)
const auto = ref(false)
const period = ref(10)
let sysFull = false
let sysRetry = null

const seriesCfg = ref([])
const enabledCfg = computed(() => seriesCfg.value.filter((c) => c.enabled))
const unsyncedCount = computed(() => seriesCfg.value.filter((c) => c.enabled && !c.synced).length)
const selected = ref([])
const seriesWrapEl = ref(null)
const seriesMeasureEl = ref(null)
const seriesVisible = ref(999)
const seriesPopOpen = ref(false)
const seriesHidden = computed(() => (isPhone.value ? 0 : Math.max(0, selected.value.length - seriesVisible.value)))
const cache = reactive({})
const lastTs = reactive({})
const pullAt = reactive({})
const sampleAuto = ref(false)
let sampleTimer = null
const metricTable = ref(null)

const percentEl = ref(null)
const loadEl = ref(null)
const rateEl = ref(null)
const charts = {}

const capsOpen = ref(false)
const capsSaving = ref(false)
const capsForm = reactive({
  ports: '', file_dir: '', shell: false, ota: false, allow_tty: [],
  fsSupported: false, fs: false, fs_root: '', fs_rw: false, fs_max_mb: 512,
  svcSupported: false, svc: false, svc_rw: false,
})
const allowTtyOpen = ref(false)
const allowTtyInput = ref('')
const capsRefreshing = ref(false)

const addMetricOpen = ref(false)
const addMetricPick = ref([])

const ttys = ref([])
const ttyDisc = ref([])
const ttyLoaded = ref(false)
const ttyTab = ref('config')
const ttyQuery = ref('')
const ttyDialog = ref(false)
const ttySaving = ref(false)
const ttyTable = ref(null)
const ttyAddTable = ref(null)
const ttyExpandedPath = ref('')
const ttyAddPath = ref('')
const bauds = [1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600]
const ttyEditing = reactive({
  isNew: true, path: '', alias: '', baud: 115200, data_bits: 8,
  parity: 'N', stop_bits: 1, enabled: true, roles: 'operator,manager,admin',
})
const ttyAddForm = reactive({
  alias: '', baud: 115200, data_bits: 8, parity: 'N', stop_bits: 1, enabled: true,
})

const disks = computed(() => {
  const mounts = (sys.value && sys.value.mounts) || []
  return mounts.map((m) => {
    const total = Number(m.total_kb) || 0
    const free = Number(m.free_kb) || 0
    const used = total - free
    const pair = kbPair(used, total)
    return {
      mount: m.mount,
      fstype: m.fstype || '',
      pct: total > 0 ? Math.round((used * 100) / total) : 0,
      used: pair.used,
      total: pair.total,
      unit: pair.unit,
    }
  })
})
const nics = computed(() => (sys.value && sys.value.nics) || [])
const streams = computed(() => (detail.value && detail.value.streams) || [])
const caps = computed(() => (sys.value && sys.value.caps) || {})
const capsChips = computed(() => {
  const c = caps.value
  const out = []
  ;(c.ports || []).forEach((p) => out.push('TCP ' + p))
  if (c.shell) out.push('Shell')
  if (c.sysinfo) out.push('SYSINFO')
  if (c.ota) out.push('OTA 升级')
  if ((c.allow_tty || []).length) out.push('串口 ' + c.allow_tty.join(' '))
  if (c.fs) out.push('文件管理 ' + (c.fs_root || '') + (c.fs_rw ? ' (读写)' : ' (只读)'))
  if (c.svc) out.push('服务管理' + (c.svc_rw ? ' (可控制)' : ' (只读)'))
  out.push(c.mtls ? 'mTLS' : 'mTLS 未启用')
  return out
})

const ttySerial = computed(() => {
  const map = {}
  ;((sys.value && sys.value.serial) || []).forEach((x) => { map[x.path] = x })
  return map
})
const configuredPaths = computed(() => new Set(ttys.value.map((t) => t.path)))
const unconfigured = computed(() => ttyDisc.value.filter((d) => !configuredPaths.value.has(d.path)))
const ttyQ = computed(() => ttyQuery.value.trim().toLowerCase())
const filteredTtys = computed(() => ttys.value.filter((t) => {
  if (!ttyQ.value) return true
  return (t.alias || '').toLowerCase().includes(ttyQ.value) || (t.path || '').toLowerCase().includes(ttyQ.value)
}))
const filteredUnconfigured = computed(() => unconfigured.value.filter((d) => {
  if (!ttyQ.value) return true
  return (d.path || '').toLowerCase().includes(ttyQ.value)
}))
const addCandidates = computed(() => {
  const existing = new Set(seriesCfg.value.map((c) => c.series))
  const out = ['cpu', 'mem', 'load']
  ;((sys.value && sys.value.nics) || []).forEach((n) => { if (n.name) out.push('net:' + n.name) })
  const paths = new Set()
  ;((sys.value && sys.value.serial) || []).forEach((s) => paths.add(s.path))
  ttys.value.forEach((t) => paths.add(t.path))
  ttyDisc.value.forEach((d) => paths.add(d.path))
  paths.forEach((p) => {
    const b = String(p).split('/').pop()
    if (b && b.startsWith('tty')) out.push('serial:' + b)
  })
  return [...new Set(out)].filter((x) => !existing.has(x))
})

function toneOf(p) {
  return p >= 85 ? 'bad' : p >= 70 ? 'warn' : ''
}
function diskColor(p) {
  return p >= 85 ? '#e5534b' : p >= 70 ? '#f5a623' : '#3b82f6'
}

function unitOf(name) {
  if (name === 'cpu' || name === 'mem') return 'percent'
  if (name === 'load') return 'load'
  return 'rate'
}
function unitLabel(name) {
  return { percent: '百分比', load: '负载', rate: '速率' }[unitOf(name)]
}
function hasUnit(u) {
  return selected.value.some((s) => unitOf(s) === u)
}
function dur(c) {
  const sec = (Number(c.interval_sec) || 1) * (Number(c.points) || 0)
  if (sec >= 3600) return (sec / 3600).toFixed(sec % 3600 ? 1 : 0) + ' h'
  if (sec >= 60) return Math.round(sec / 60) + ' m'
  return sec + ' s'
}
function metricHint(row) {
  if (!row.enabled) return '已禁用，不参与采样'
  return row.synced ? '已下发并确认' : '已下发，等待 agent 确认'
}

function mergeSysinfo(p) {
  if (!p || !sys.value) return
  if (p.swap_free_kb != null) sys.value.swap_free_kb = p.swap_free_kb
  ;(p.mounts || []).forEach((pm) => {
    const m = (sys.value.mounts || []).find((x) => x.mount === pm.mount)
    if (m && pm.free_kb != null) m.free_kb = pm.free_kb
  })
  ;(p.nics || []).forEach((pn) => {
    const n = (sys.value.nics || []).find((x) => x.name === pn.name)
    if (!n) return
    if (pn.ip != null) n.ip = pn.ip
    if (pn.up != null) n.up = pn.up
  })
  if (p.serial) sys.value.serial = p.serial
}

async function loadSysinfo(silent) {
  try {
    const mode = sysFull ? 'inc' : 'full'
    const r = await Api.get(agentBase.value + '/sysinfo?mode=' + mode)
    if (r.mode === 'inc') mergeSysinfo(r.sysinfo)
    else {
      sys.value = r.sysinfo
      sysFull = true
    }
    sysTs.value = r.ts || Date.now()
    sysErr.value = ''
    if (sys.value && !sys.value.cpu_count && !sysRetry) {
      sysRetry = setTimeout(() => { sysRetry = null; loadSysinfo(true) }, 1200)
    }
  } catch (e) {
    if (!silent) sysErr.value = '系统信息获取失败：' + e.message
  }
}

async function loadAll(opts = {}) {
  const { silent = false, notify = false } = opts
  if (!silent) loading.value = true
  try {
    detail.value = await Api.get(agentBase.value)
  } catch (e) {
    if (!silent) ElMessage.error('设备详情加载失败：' + e.message)
  }
  await loadSysinfo(silent)
  await pullSeries(!silent)
  if (!silent) loading.value = false
  if (notify) ElMessage.success('已刷新')
}

async function fetchSeriesCfg() {
  try {
    const list = await Api.get(agentBase.value + '/metrics/series/config')
    seriesCfg.value = Array.isArray(list) ? list : []
  } catch (e) {
    seriesCfg.value = []
  }
}

function pickSeries() {
  const enabled = enabledCfg.value.map((c) => c.series)
  const prefer = enabled.filter((s) => s === 'cpu' || s === 'mem')
  selected.value = prefer.length ? prefer : enabled.slice(0, 1)
}

function syncSeriesSelection() {
  const enabled = enabledCfg.value.map((c) => c.series)
  selected.value = selected.value.filter((s) => enabled.includes(s))
  if (!selected.value.length) pickSeries()
}

function seriesPoints(name) {
  return cache[name] || []
}

function valueArray(name, points) {
  if (unitOf(name) !== 'rate') {
    return points.map((p) => [Number(p.ts), Number(p.v) || 0])
  }
  let prx = 0
  let ptx = 0
  let pts = 0
  const rx = []
  const tx = []
  points.forEach((p) => {
    const t = Number(p.ts) || 0
    const dt = pts ? Math.max(1, t - pts) / 1000 : 1
    rx.push([t, pts ? Math.max(0, (Number(p.rx) - prx) / dt) : 0])
    tx.push([t, pts ? Math.max(0, (Number(p.tx) - ptx) / dt) : 0])
    prx = Number(p.rx) || 0
    ptx = Number(p.tx) || 0
    pts = t
  })
  return { rx, tx }
}

async function pullSeriesNames(names, full) {
  await Promise.all(names.map(async (name) => {
    const since = full ? 0 : (lastTs[name] || 0)
    try {
      const r = await Api.get(agentBase.value +
        '/metrics/series?series=' + encodeURIComponent(name) + '&since_ts=' + since)
      if (r.truncated && since) {
        lastTs[name] = 0
        return pullSeriesNames([name], true)
      }
      if (!since) cache[name] = []
      cache[name] = (cache[name] || []).concat(r.points || [])
      while (cache[name].length > 300) cache[name].shift()
      lastTs[name] = r.newest_ts || since
    } catch (e) { /* 离线或该指标无数据 */ }
  }))
}

async function pullSeries(full) {
  const names = selected.value.filter(Boolean)
  if (!names.length) {
    renderCharts()
    return
  }
  await pullSeriesNames(names, full)
  const now = Date.now()
  names.forEach((n) => { pullAt[n] = now })
  await nextTick()
  renderCharts()
}

async function onSeriesPick() {
  await pullSeries(true)
}

function removeSeries(name) {
  selected.value = selected.value.filter((s) => s !== name)
  pullSeries(true)
}

// 桌面单行铺开：按容器宽度算出能放下几个 chip，剩余收进 +N；手机不限行数
function measureSeries() {
  const n = selected.value.length
  if (isPhone.value || !seriesWrapEl.value || !seriesMeasureEl.value) {
    seriesVisible.value = n
    return
  }
  const chips = Array.from(seriesMeasureEl.value.children)
  if (!chips.length) {
    seriesVisible.value = 0
    return
  }
  const avail = seriesWrapEl.value.clientWidth - 16
  const gap = 8
  const total = chips.reduce((a, c) => a + c.offsetWidth, 0) + gap * (chips.length - 1)
  if (total <= avail) {
    seriesVisible.value = chips.length
    return
  }
  const reserve = 52
  let used = 0
  let fit = 0
  for (const c of chips) {
    const add = (fit ? gap : 0) + c.offsetWidth
    if (used + add > avail - reserve) break
    used += add
    fit++
  }
  seriesVisible.value = Math.max(1, fit)
}

function selectAllSeries() {
  selected.value = enabledCfg.value.map((c) => c.series)
  pullSeries(true)
}
function clearSeries() {
  selected.value = []
  renderCharts()
}

function onAutoChange() {
  stopTimer()
  if (auto.value) timer = setInterval(() => loadAll({ silent: true }), Math.max(5, Number(period.value) || 10) * 1000)
}

function onSampleAuto() {
  stopSample()
  if (sampleAuto.value) sampleTimer = setInterval(sampleTick, 1000)
}

async function sampleTick() {
  if (!sampleAuto.value || document.hidden) return
  const now = Date.now()
  const due = selected.value.filter((name) => {
    const cfg = seriesCfg.value.find((c) => c.series === name)
    if (!cfg) return false
    return now - (pullAt[name] || 0) >= Math.max(1, Number(cfg.interval_sec) || 5) * 1000
  })
  if (!due.length) return
  due.forEach((n) => { pullAt[n] = now })
  await pullSeriesNames(due, false)
  renderCharts()
}

function stopSample() {
  if (sampleTimer) {
    clearInterval(sampleTimer)
    sampleTimer = null
  }
}

const palette = ['#4a9eff', '#f5a623', '#57ab5a', '#b083f0', '#e5534b']

function renderCharts() {
  const groups = { percent: [], load: [], rate: [] }
  selected.value.forEach((s) => groups[unitOf(s)].push(s))
  const els = { percent: percentEl.value, load: loadEl.value, rate: rateEl.value }
  for (const unit of ['percent', 'load', 'rate']) {
    const el = els[unit]
    if (!el) continue
    if (charts[unit]) {
      charts[unit].destroy()
      delete charts[unit]
    }
    if (!groups[unit].length) continue
    const tsSet = new Set()
    const data = {}
    groups[unit].forEach((name) => {
      const v = valueArray(name, seriesPoints(name))
      data[name] = v
      if (Array.isArray(v)) v.forEach((x) => tsSet.add(x[0]))
      else {
        v.rx.forEach((x) => tsSet.add(x[0]))
        v.tx.forEach((x) => tsSet.add(x[0]))
      }
    })
    const labels = [...tsSet].sort((a, b) => a - b)
    const idx = new Map(labels.map((t, i) => [t, i]))
    const put = (arr) => {
      const out = new Array(labels.length).fill(null)
      arr.forEach((x) => {
        const i = idx.get(x[0])
        if (i !== undefined) out[i] = x[1]
      })
      return out
    }
    const datasets = []
    groups[unit].forEach((name, si) => {
      const color = palette[si % palette.length]
      const v = data[name]
      if (Array.isArray(v)) {
        datasets.push({
          label: name, data: put(v), borderColor: color, backgroundColor: color + '1f',
          borderWidth: 2, pointRadius: 0, fill: true, tension: 0.2,
        })
      } else {
        datasets.push({ label: name + ' RX', data: put(v.rx), borderColor: color, borderWidth: 2, pointRadius: 0 })
        datasets.push({
          label: name + ' TX', data: put(v.tx), borderColor: color, borderDash: [4, 3],
          borderWidth: 2, pointRadius: 0,
        })
      }
    })
    charts[unit] = new Chart(el.getContext('2d'), {
      type: 'line',
      data: {
        labels: labels.map((t) => new Date(t).toLocaleTimeString('zh-CN', { hour12: false })),
        datasets,
      },
      options: {
        animation: false,
        responsive: true,
        maintainAspectRatio: false,
        interaction: { mode: 'index', intersect: false },
        plugins: { legend: { display: true, labels: { boxWidth: 10 } } },
        scales: { x: { display: false }, y: { beginAtZero: true, ticks: { maxTicksLimit: 4 } } },
      },
    })
  }
}

function onCapsToggle() {
  if (!canOpenCaps.value) return
  toggleCaps()
}

function addAllowTty() {
  const v = allowTtyInput.value.trim()
  allowTtyOpen.value = false
  allowTtyInput.value = ''
  if (!v) return
  if (v[0] !== '/' || v.includes('..')) {
    ElMessage.error('串口路径必须以 / 开头且不能包含 ..')
    return
  }
  if (!capsForm.allow_tty.includes(v)) capsForm.allow_tty.push(v)
}

function removeAllowTty(i) {
  capsForm.allow_tty.splice(i, 1)
}

function fillCapsFromSys() {
  if (!sys.value) return
  const c = sys.value.caps || {}
  capsForm.ports = (c.ports || []).join(', ')
  capsForm.file_dir = c.file_dir || ''
  capsForm.shell = !!c.shell
  capsForm.ota = !!c.ota
  capsForm.allow_tty = Array.isArray(c.allow_tty) ? c.allow_tty.slice() : []
  allowTtyOpen.value = false
  allowTtyInput.value = ''
  capsForm.fsSupported = Object.prototype.hasOwnProperty.call(c, 'fs')
  capsForm.fs = !!c.fs
  capsForm.fs_root = c.fs_root || c.file_dir || ''
  capsForm.fs_rw = !!c.fs_rw
  capsForm.fs_max_mb = Number(c.fs_max_mb) || 512
  capsForm.svcSupported = Object.prototype.hasOwnProperty.call(c, 'svc')
  capsForm.svc = !!c.svc
  capsForm.svc_rw = !!c.svc_rw
}

function toggleCaps() {
  capsOpen.value = !capsOpen.value
  if (capsOpen.value) fillCapsFromSys()
}

// 能力配置以 agent 的 sysinfo 为准：强制拉全量后重新填充
async function refreshCaps() {
  capsRefreshing.value = true
  try {
    sysFull = false
    await loadSysinfo(true)
    fillCapsFromSys()
  } finally {
    capsRefreshing.value = false
  }
}

async function saveCaps() {
  const raw = canCaps.value ? capsForm.ports.trim() : ''
  const ports = raw ? raw.split(/[,\s]+/).filter(Boolean).map(Number) : []
  if (canCaps.value && ports.some((x) => !Number.isInteger(x) || x <= 0 || x > 65535)) {
    ElMessage.error('端口必须是 1–65535 的整数')
    return
  }
  const body = {}
  if (canCaps.value) {
    body.ports = ports
    const dir = capsForm.file_dir.trim()
    if (dir) body.file_dir = dir
    body.shell = capsForm.shell
  }
  if (canOta.value) body.ota = !!capsForm.ota
  if (canTty.value) body.allow_tty = capsForm.allow_tty.slice()
  if (!canCaps.value) {
    // 只改 OTA / 串口白名单时不动其它能力
  } else if (capsForm.fsSupported) {
    if (capsForm.fs) {
      const root = capsForm.fs_root.trim()
      if (!root || root[0] !== '/' || root.includes('..')) {
        ElMessage.error('文件根目录必须是绝对路径，且不能包含 ..')
        return
      }
      const maxMb = Number(capsForm.fs_max_mb)
      if (!Number.isInteger(maxMb) || maxMb < 1 || maxMb > 10240) {
        ElMessage.error('单文件上限必须是 1–10240 MB 的整数')
        return
      }
      body.fs = true
      body.fs_root = root
      body.fs_rw = !!capsForm.fs_rw
      body.fs_max_mb = maxMb
    } else {
      body.fs = false
    }
  }
  if (canCaps.value && capsForm.svcSupported) {
    body.svc = !!capsForm.svc
    if (capsForm.svc) body.svc_rw = !!capsForm.svc_rw
  }
  if (!Object.keys(body).length) {
    ElMessage.warning('没有可提交的修改')
    return
  }
  capsSaving.value = true
  try {
    const r = await Api.put(agentBase.value + '/caps', body)
    capsOpen.value = false
    const fsMissed = body.fs !== undefined &&
      !(typeof r.fs === 'boolean' && r.fs === body.fs && (!body.fs || r.fs_root === body.fs_root))
    const svcMissed = body.svc !== undefined &&
      !(typeof r.svc === 'boolean' && r.svc === body.svc && (!body.svc || r.svc_rw === body.svc_rw))
    const otaMissed = body.ota !== undefined && (typeof r.ota !== 'boolean' || r.ota !== body.ota)
    const ttyMissed = body.allow_tty !== undefined &&
      !(Array.isArray(r.allow_tty) && JSON.stringify(r.allow_tty) === JSON.stringify(body.allow_tty))
    if (otaMissed) ElMessage.warning('设备未应用 OTA 开关：请确认 agent 已升级到支持该配置的版本')
    else if (ttyMissed) ElMessage.warning('设备未应用串口白名单：请确认 agent 已升级到支持该配置的版本')
    else if (fsMissed) ElMessage.warning('设备未应用文件管理配置：请确认 agent 已升级')
    else if (svcMissed) ElMessage.warning('设备未应用服务管理配置：请确认 agent 已升级')
    else if (r && r.persisted === false) ElMessage.warning('已生效，但未能写入配置文件，重启后会丢失')
    else ElMessage.success('能力已更新')
    // 能力位不在增量 sysinfo 里，强制拉一次全量，卡片/入口立即跟着变
    sysFull = false
    await loadAll({ silent: true })
  } catch (e) {
    ElMessage.error('失败：' + e.message)
  } finally {
    capsSaving.value = false
  }
}

function onMetricRow(row) {
  metricTable.value && metricTable.value.toggleRowExpansion(row)
}

async function saveMetricList(list) {
  try {
    await Api.put(agentBase.value + '/metrics/series/config',
      list.map((c) => ({
        series: c.series,
        enabled: !!c.enabled,
        interval_sec: Number(c.interval_sec) || 5,
        points: Number(c.points) || 720,
      })))
    await fetchSeriesCfg()
    syncSeriesSelection()
    await pullSeries(true)
    ElMessage.success('采样配置已下发')
  } catch (e) {
    ElMessage.error('保存失败：' + e.message)
  }
}

async function saveMetricRow(row) {
  await saveMetricList(seriesCfg.value.map((c) => (c.series === row.series ? row : c)))
}

async function toggleMetric(row, v) {
  row.enabled = v
  await saveMetricList(seriesCfg.value.map((c) => (c.series === row.series ? Object.assign({}, c, { enabled: v }) : c)))
}

async function deleteMetric(row) {
  try {
    await ElMessageBox.confirm('确定删除指标 ' + row.series + '？', '删除指标', {
      type: 'warning', confirmButtonText: '删除', cancelButtonText: '取消',
    })
  } catch (e) { return }
  await saveMetricList(seriesCfg.value.filter((c) => c.series !== row.series))
}

async function cancelMetric() {
  await fetchSeriesCfg()
  syncSeriesSelection()
}

function openAddMetric() {
  addMetricPick.value = []
  addMetricOpen.value = true
}

async function addMetrics() {
  const picked = addMetricPick.value
  if (!picked.length) return
  const list = seriesCfg.value.concat(picked.map((s) => ({ series: s, enabled: true, interval_sec: 5, points: 720 })))
  addMetricOpen.value = false
  await saveMetricList(list)
}

function ttyRx(path) {
  const x = ttySerial.value[path]
  return x ? bytes(x.rx) : '—'
}
function ttyTx(path) {
  const x = ttySerial.value[path]
  return x ? bytes(x.tx) : '—'
}

async function loadTtys(force) {
  if (force) {
    ttyExpandedPath.value = ''
    ttyAddPath.value = ''
  }
  if (ttyLoaded.value && !force) return
  try {
    const list = await Api.get(agentBase.value + '/ttys')
    ttys.value = Array.isArray(list) ? list : []
  } catch (e) {
    ttys.value = []
  }
  if (canEdit.value) {
    try {
      const d = await Api.get(agentBase.value + '/ttys/discovered')
      ttyDisc.value = Array.isArray(d) ? d : []
    } catch (e) {
      ttyDisc.value = []
    }
  } else {
    ttyDisc.value = []
  }
  ttyLoaded.value = true
}

function fillTtyEditing(row) {
  ttyEditing.isNew = false
  ttyEditing.path = row.path || ''
  ttyEditing.alias = row.alias || ''
  ttyEditing.baud = Number(row.baud) || 115200
  ttyEditing.data_bits = Number(row.data_bits) || 8
  ttyEditing.parity = row.parity || 'N'
  ttyEditing.stop_bits = Number(row.stop_bits) || 1
  ttyEditing.enabled = !!row.enabled
  ttyEditing.roles = row.roles || 'operator,manager,admin'
}

function onTtyRow(row) {
  if (ttyTable.value) ttyTable.value.toggleRowExpansion(row)
}

// 同时只展开一行，展开时把该行复制到 ttyEditing，取消不会改到列表数据
function onTtyExpand(row, expanded) {
  const open = (expanded || []).some((r) => r.path === row.path)
  if (!open) {
    if (ttyExpandedPath.value === row.path) ttyExpandedPath.value = ''
    return
  }
  for (const r of expanded || []) {
    if (r.path !== row.path && ttyTable.value) ttyTable.value.toggleRowExpansion(r, false)
  }
  ttyExpandedPath.value = row.path
  fillTtyEditing(row)
}

function collapseTtyRow() {
  const row = filteredTtys.value.find((r) => r.path === ttyExpandedPath.value)
  if (row && ttyTable.value) ttyTable.value.toggleRowExpansion(row, false)
  ttyExpandedPath.value = ''
}

function resetTtyAdd() {
  ttyAddForm.alias = ''
  ttyAddForm.baud = 115200
  ttyAddForm.data_bits = 8
  ttyAddForm.parity = 'N'
  ttyAddForm.stop_bits = 1
  ttyAddForm.enabled = true
}

function onTtyAddRow(row) {
  if (ttyAddTable.value) ttyAddTable.value.toggleRowExpansion(row)
}

function onTtyAddExpand(row, expanded) {
  const open = (expanded || []).some((r) => r.path === row.path)
  if (!open) {
    if (ttyAddPath.value === row.path) ttyAddPath.value = ''
    return
  }
  for (const r of expanded || []) {
    if (r.path !== row.path && ttyAddTable.value) ttyAddTable.value.toggleRowExpansion(r, false)
  }
  ttyAddPath.value = row.path
  resetTtyAdd()
}

function collapseTtyAddRow() {
  const row = filteredUnconfigured.value.find((r) => r.path === ttyAddPath.value)
  if (row && ttyAddTable.value) ttyAddTable.value.toggleRowExpansion(row, false)
  ttyAddPath.value = ''
}

function manualAddTty() {
  collapseTtyRow()
  collapseTtyAddRow()
  ttyEditing.isNew = true
  ttyEditing.path = ''
  ttyEditing.alias = ''
  ttyEditing.baud = 115200
  ttyEditing.data_bits = 8
  ttyEditing.parity = 'N'
  ttyEditing.stop_bits = 1
  ttyEditing.enabled = true
  ttyEditing.roles = 'operator,manager,admin'
  ttyDialog.value = true
}

async function putTty(cfg) {
  const path = String(cfg.path || '').trim()
  const alias = String(cfg.alias || '').trim()
  if (!path || path[0] !== '/') {
    ElMessage.error('路径必须以 / 开头')
    return false
  }
  if (alias && !/^[A-Za-z0-9_-]{1,32}$/.test(alias)) {
    ElMessage.error('别名只能包含字母、数字、下划线和短横线')
    return false
  }
  ttySaving.value = true
  try {
    await Api.put(agentBase.value + '/ttys', {
      path,
      alias,
      enabled: !!cfg.enabled,
      baud: cfg.baud,
      data_bits: cfg.data_bits,
      parity: cfg.parity,
      stop_bits: cfg.stop_bits,
      roles: cfg.roles || 'operator,manager,admin',
    })
    ElMessage.success('串口配置已下发')
    return true
  } catch (e) {
    ElMessage.error('保存失败：' + e.message)
    return false
  } finally {
    ttySaving.value = false
  }
}

async function saveTtyRow() {
  if (!(await putTty(ttyEditing))) return
  collapseTtyRow()
  await loadTtys(true)
}

async function saveTtyAdd() {
  if (!(await putTty(Object.assign({ path: ttyAddPath.value }, ttyAddForm)))) return
  collapseTtyAddRow()
  await loadTtys(true)
}

async function saveTty() {
  if (!(await putTty(ttyEditing))) return
  ttyDialog.value = false
  await loadTtys(true)
}

async function deleteTty(row) {
  try {
    await ElMessageBox.confirm('确定删除 ' + (row.alias || row.path) + ' 的串口配置？', '删除串口配置', {
      type: 'warning', confirmButtonText: '删除', cancelButtonText: '取消',
    })
  } catch (e) { return }
  try {
    await Api.del(agentBase.value + '/ttys?path=' + encodeURIComponent(row.path))
    if (ttyExpandedPath.value === row.path) collapseTtyRow()
    await loadTtys(true)
    ElMessage.success('已删除')
  } catch (e) {
    ElMessage.error('删除失败：' + e.message)
  }
}


async function takeoverTty(row) {
  const name = row.alias || row.path
  try {
    const r = await ElMessageBox.prompt('会断开当前会话，输入别名确认：' + name, '强制接管', {
      confirmButtonText: '接管', cancelButtonText: '取消',
    })
    if (String(r.value || '') !== name) {
      ElMessage.warning('输入不一致，已取消')
      return
    }
  } catch (e) { return }
  router.push({
    path: '/tty/' + encodeURIComponent(devid.value) + '/' + encodeURIComponent(name),
    query: { takeover: '1' },
  })
}

function openTtyConsole(row) {
  const name = row.alias || row.path
  router.push('/tty/' + encodeURIComponent(devid.value) + '/' + encodeURIComponent(name))
}

function exportJson() {
  downloadJson(devid.value + '-detail.json', {
    detail: detail.value,
    sysinfo: sys.value,
    sysinfo_ts: sysTs.value,
    series: seriesCfg.value,
    ttys: ttys.value,
  })
}

let timer = null
function stopTimer() {
  if (timer) {
    clearInterval(timer)
    timer = null
  }
}

async function init() {
  stopTimer()
  stopSample()
  auto.value = false
  sampleAuto.value = false
  detail.value = null
  sys.value = null
  sysErr.value = ''
  sysFull = false
  seriesCfg.value = []
  selected.value = []
  ttys.value = []
  ttyDisc.value = []
  ttyLoaded.value = false
  ttyTab.value = 'config'
  ttyExpandedPath.value = ''
  ttyAddPath.value = ''
  addMetricOpen.value = false
  capsOpen.value = false
  seriesPopOpen.value = false
  if (sysRetry) { clearTimeout(sysRetry); sysRetry = null }
  Object.keys(cache).forEach((k) => delete cache[k])
  Object.keys(lastTs).forEach((k) => delete lastTs[k])
  Object.keys(pullAt).forEach((k) => delete pullAt[k])
  await fetchSeriesCfg()
  pickSeries()
  await loadAll()
  loadTtys()
}

watch(() => route.params.devid, (v) => { if (v) init() })
watch([selected, isPhone, () => enabledCfg.value.length], () => nextTick(measureSeries), { deep: true })

let seriesRO = null
onMounted(() => {
  init()
  if (window.ResizeObserver) {
    seriesRO = new ResizeObserver(() => measureSeries())
    nextTick(() => { if (seriesWrapEl.value) seriesRO.observe(seriesWrapEl.value) })
  }
})
onBeforeUnmount(() => {
  stopTimer()
  stopSample()
  if (seriesRO) { seriesRO.disconnect(); seriesRO = null }
  if (sysRetry) { clearTimeout(sysRetry); sysRetry = null }
  Object.values(charts).forEach((c) => { try { c.destroy() } catch (e) { /* 已销毁 */ } })
})
</script>

<style scoped>
.period { width: 84px; }
:deep(.el-table .cell) { white-space: nowrap; }
.disk { padding: 8px 0; border-bottom: 1px solid var(--el-border-color-lighter); }
.disk:last-child { border-bottom: 0; }
.disk-top { display: flex; align-items: center; gap: 8px; margin-bottom: 6px; }
.disk-top .mount { font-size: 13px; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.fs {
  font-size: 11px;
  color: var(--el-text-color-secondary);
  background: var(--el-fill-color);
  border-radius: 4px;
  padding: 0 6px;
}
.disk-top .pct { margin-left: auto; font-size: 12px; color: var(--el-text-color-secondary); font-variant-numeric: tabular-nums; }
.disk-top .pct.warn { color: #c98a12; }
.disk-top .pct.bad { color: #d9534f; }
.size { margin-top: 3px; font-size: 11px; color: var(--el-text-color-secondary); }
.chart-box { margin-top: 8px; height: 190px; position: relative; }
.chart-title { font-size: 12px; color: var(--el-text-color-secondary); margin-bottom: 4px; }
.series-bar { display: flex; align-items: flex-start; gap: 10px; margin: 0 0 12px; }
.series-label { flex: none; font-size: 12px; line-height: 30px; color: var(--el-text-color-secondary); }
.series-select {
  position: relative;
  flex: 1;
  min-width: 0;
  display: flex;
  flex-wrap: nowrap;
  align-items: center;
  gap: 8px;
  min-height: 30px;
  padding: 2px 8px;
  border: 1px solid var(--el-border-color);
  border-radius: 6px;
  background: var(--el-bg-color);
  cursor: pointer;
  overflow: hidden;
}
.series-select:hover, .series-select:focus-visible { border-color: var(--el-color-primary); outline: none; }
.series-chip {
  flex: none;
  display: inline-flex;
  align-items: center;
  gap: 4px;
  height: 22px;
  padding: 0 4px 0 8px;
  border-radius: 4px;
  background: var(--el-fill-color);
  font-size: 12px;
  line-height: 22px;
  color: var(--el-text-color-regular);
}
.series-chip .chip-x {
  border: 0;
  background: transparent;
  padding: 0 2px;
  font-size: 13px;
  line-height: 1;
  color: var(--el-text-color-secondary);
  cursor: pointer;
}
.series-chip .chip-x:hover { color: var(--el-color-danger); }
.series-more { flex: none; display: inline-flex; align-items: center; gap: 2px; padding: 0 6px; font-size: 12px; color: var(--el-text-color-secondary); }
.series-select:hover .series-more { color: var(--el-color-primary); }
.more-caret { font-size: 9px; line-height: 1; }
.series-empty { font-size: 12px; color: var(--el-text-color-placeholder); }
.series-measure { position: absolute; left: 0; top: 0; display: flex; gap: 8px; width: max-content; visibility: hidden; pointer-events: none; }
.series-pop-head { margin-bottom: 8px; font-size: 12px; color: var(--el-text-color-secondary); }
.series-options { display: flex; flex-direction: column; gap: 4px; max-height: 260px; overflow-y: auto; }
.series-options :deep(.el-checkbox) { height: 24px; margin-right: 0; }
.series-pop-actions { display: flex; justify-content: flex-end; gap: 4px; margin-top: 8px; padding-top: 8px; border-top: 1px solid var(--el-border-color-lighter); }

.kv-rows { border: 1px solid var(--el-border-color-lighter); border-radius: 8px; overflow: hidden; }
.kv-row { display: flex; align-items: flex-start; gap: 12px; padding: 9px 12px; border-bottom: 1px solid var(--el-border-color-lighter); }
.kv-row:last-child { border-bottom: 0; }
.kv-row .caret { flex: none; display: flex; align-items: center; justify-content: center; width: 16px; height: 24px; color: var(--el-text-color-secondary); }
.kv-row .caret .el-icon { font-size: 12px; transition: transform 0.15s ease; }
.kv-row .caret .el-icon.open { transform: rotate(90deg); }
.kv-row .k { flex: none; width: 92px; color: var(--el-text-color-secondary); font-size: 12px; line-height: 24px; }
.kv-row .v { min-width: 0; font-size: 13px; word-break: break-all; }
.kv-row .chips { display: flex; flex-wrap: wrap; gap: 6px; padding-top: 1px; }
.caps-toggle.clickable { cursor: pointer; }
.caps-toggle.clickable:hover { background: var(--el-fill-color-light); }
.caps-toggle.clickable:focus-visible { outline: 2px solid var(--el-color-primary-light-5); outline-offset: -2px; }
.caps-hint { flex: none; margin-left: auto; font-size: 12px; line-height: 24px; color: var(--el-text-color-secondary); }
.args { font-size: 12px; color: var(--el-text-color-regular); white-space: pre-wrap; word-break: break-all; }
.caps-edit {
  padding: 12px 12px 14px;
  background: var(--el-fill-color-lighter);
  border-top: 1px solid var(--el-border-color-lighter);
}
.caps-form { max-width: 100%; }
.caps-runtime { display: flex; align-items: center; gap: 8px; flex-wrap: wrap; margin: 0 0 10px; font-size: 12px; }
.caps-runtime code { padding: 1px 6px; border-radius: 4px; background: var(--el-fill-color-light); }
.caps-runtime .spacer { flex: 1; }
.caps-grid { display: grid; grid-template-columns: repeat(2, minmax(0, 1fr)); gap: 8px 28px; align-items: start; }
.caps-grid .span2 { grid-column: 1 / -1; }
.caps-grid .vform-row + .vform-row { margin-top: 0; }
.caps-grid .vform-section { grid-column: 1 / -1; margin: 8px 0 2px; }
.caps-grid .vform-section:first-child { margin-top: 0; }
.caps-grid .vform-note { margin: 2px 0 0; font-size: 12px; color: var(--el-text-color-secondary); }
.caps-grid .vcontrol.stacked { flex-direction: column; align-items: stretch; gap: 2px; }
.tty-allow { display: flex; flex-wrap: wrap; gap: 6px; align-items: center; }
.tty-allow .allow-input { width: 180px; }
.vform-row { display: flex; align-items: center; gap: 0; }
.vform-row + .vform-row { margin-top: 10px; }
.vform-row .vlabel { flex: none; width: 104px; padding-right: 14px; text-align: right; font-size: 12px; line-height: 24px; color: var(--el-text-color-secondary); }
.vform-row .vcontrol { flex: 1; display: flex; align-items: center; gap: 8px; min-width: 0; }
.vform-row .vcontrol > .el-input { width: 420px; max-width: 100%; }
.caps-grid .vcontrol > .el-input { width: 100%; }
.vform-row .vcontrol > .el-input-number { width: 140px; }
.vform-row .vcontrol > .el-checkbox { margin-right: 0; }
.vunit { flex: none; font-size: 12px; color: var(--el-text-color-secondary); }
.vreadonly { font-size: 13px; }
.vform-section { display: flex; align-items: center; gap: 10px; margin: 16px 0 10px; font-size: 12px; color: var(--el-text-color-secondary); }
.vform-section::before, .vform-section::after { content: ''; flex: 1; height: 1px; background: var(--el-border-color-lighter); }
.vform-note { margin: 12px 0 0 104px; font-size: 12px; }
.vform-actions { display: flex; align-items: center; gap: 8px; margin-top: 16px; padding-top: 12px; border-top: 1px solid var(--el-border-color-lighter); }
.vform-actions .spacer, .form-actions .spacer { flex: 1; }
.form-actions { display: flex; align-items: center; gap: 8px; margin-top: 16px; padding-top: 12px; border-top: 1px solid var(--el-border-color-lighter); }

.sub { margin-left: 6px; font-size: 11px; color: var(--el-text-color-secondary); }
.num { font-variant-numeric: tabular-nums; }
.sync { font-size: 12px; }
.sync::before {
  content: '';
  display: inline-block;
  width: 6px;
  height: 6px;
  border-radius: 50%;
  margin-right: 6px;
  vertical-align: 1px;
  background: var(--el-text-color-placeholder);
}
.sync.ok::before { background: var(--el-color-success); }
.sync.warn::before { background: var(--el-color-warning); }
.sync.off::before { background: var(--el-text-color-placeholder); }
.metric-edit { padding: 6px 12px 12px 42px; }
.metric-form { max-width: 560px; }
.metric-form .vlabel { width: 96px; }
.metric-card { padding: 2px 0; }
.metric-card-top { display: flex; align-items: center; gap: 6px; }
.metric-card-meta { margin-top: 3px; font-size: 12px; color: var(--el-text-color-secondary); }
.metric-card-sub { display: flex; align-items: center; gap: 8px; }
.metric-card-sub .sync { flex: none; }
.grow { flex: 1; }

.tty-edit { padding: 6px 12px 12px 42px; }
.tty-vform { max-width: 100%; }
.tty-grid { display: grid; grid-template-columns: repeat(2, minmax(0, 1fr)); gap: 8px 28px; align-items: start; }
.tty-grid .span2 { grid-column: 1 / -1; }
.tty-grid .vform-row + .vform-row { margin-top: 0; }
.tty-grid .path-line { flex-wrap: wrap; }
.tty-vform .vcontrol > .el-select { width: 200px; max-width: 100%; }
.path-line { flex-wrap: wrap; gap: 2px 8px; }
.path-line .el-button + .el-button { margin-left: 0; }
.tty-card { padding: 2px 0; }
.tty-card-top { display: flex; align-items: center; gap: 8px; }
.tty-card-meta { display: flex; flex-wrap: wrap; gap: 4px 12px; margin-top: 3px; font-size: 12px; color: var(--el-text-color-secondary); }
.vform-actions .el-button + .el-button, .form-actions .el-button + .el-button { margin-left: 0; }

.tty-search { width: 190px; }
.path { font-size: 11px; color: var(--el-text-color-secondary); overflow: hidden; text-overflow: ellipsis; }
.cand-list { display: flex; flex-direction: column; gap: 4px; }
.tty-form { display: grid; grid-template-columns: 1fr 1fr; gap: 12px 16px; }
.tty-form > label { display: flex; flex-direction: column; gap: 5px; font-size: 12px; color: var(--el-text-color-secondary); }
.tty-form > label.check { flex-direction: row; align-items: center; }
.tty-form-extra { display: flex; gap: 8px; margin-top: 12px; }

@media (max-width: 899px) {
  .caps-grid, .tty-grid { grid-template-columns: 1fr; }
  .caps-grid .span2, .caps-grid .vform-section, .tty-grid .span2 { grid-column: auto; }
}
@media (max-width: 599px) {
  .period { width: 76px; }
  .panel-head { flex-wrap: wrap; }
  .tty-search { width: 100%; }
  .tty-form { grid-template-columns: 1fr; }
  .metric-edit, .tty-edit { padding-left: 12px; }
  .caps-hint { display: none; }
  .kv-row { display: grid; grid-template-columns: 16px minmax(0, 1fr); column-gap: 8px; row-gap: 2px; align-items: start; }
  .kv-row .caret { grid-column: 1; grid-row: 1; height: 20px; }
  .kv-row .k { grid-column: 2; grid-row: 1; width: auto; line-height: 20px; }
  .kv-row .v { grid-column: 2; grid-row: 2; }
  .kv-row .chips { gap: 4px; }
  .kv-row .chips :deep(.el-tag) { height: 20px; padding: 0 5px; font-size: 11px; }
  .vform-row { flex-direction: column; align-items: stretch; gap: 5px; }
  .vform-row .vlabel { width: auto; padding-right: 0; text-align: left; }
  .vform-row .vcontrol > .el-input { width: 100%; }
  .vform-note { margin-left: 0; }
  .form-actions, .vform-actions { flex-wrap: wrap; }
  .series-head h2 { flex: 0 0 100%; }
  .series-bar { gap: 8px; }
  .series-label { line-height: 28px; }
  .series-select { flex-wrap: wrap; overflow: visible; padding: 4px 8px; }
  .series-chip { height: 24px; }
  .row-edit-table :deep(.el-table__body colgroup col:first-child),
  .row-edit-table :deep(.el-table__header colgroup col:first-child) { width: 30px !important; }
  .row-edit-table :deep(.el-table__expand-column) { width: 30px !important; padding: 0 !important; vertical-align: top; }
  .row-edit-table :deep(.el-table__expand-column .cell) { display: flex; align-items: flex-start; justify-content: center; width: 30px; padding: 8px 0 0; }
  .row-edit-table :deep(.el-table__expand-icon) { margin-top: 0; }
}
</style>
