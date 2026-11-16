// 分发/OTA 任务共用的状态文案、可操作判断与进度
import { ElMessageBox } from 'element-plus'

const TASK_STATUS = {
  scheduled: ['等待开始', 'warning'],
  queued: ['排队中', 'warning'],
  running: ['传输中', 'warning'],
  verifying: ['版本校验', 'warning'],
  installing: ['安装中', 'warning'],
  confirming: ['等待重启', 'warning'],
  success: ['成功', 'success'],
  failed: ['失败', 'danger'],
  timeout: ['超时', 'danger'],
  canceled: ['已取消', 'info'],
  conflict: ['冲突', 'warning'],
  unsupported: ['不支持 OTA', 'info'],
}

const UNKNOWN = ['未知', 'warning']

export function taskStatusText(s) {
  return (TASK_STATUS[s] || UNKNOWN)[0]
}

export function taskStatusType(s) {
  return (TASK_STATUS[s] || UNKNOWN)[1]
}

export function taskCanCancel(status) {
  return ['scheduled', 'queued', 'running'].includes(status)
}

export function taskCanRetry(status, kind) {
  if (status === 'failed') return true
  return kind === 'ota' && ['timeout', 'conflict', 'unsupported', 'canceled'].includes(status)
}

export function taskPercent(task) {
  const size = Number(task.file_size) || 0
  const sent = Number(task.sent) || 0
  if (task.status === 'success') return 100
  if (['verifying', 'installing', 'confirming'].includes(task.status)) return 100
  if (!size) return 0
  return Math.min(100, Math.floor((sent * 100) / size))
}

// 返回新的截止时间（毫秒）；用户取消时返回 0
export async function askExtendDeadline() {
  let hours
  try {
    const r = await ElMessageBox.prompt('新的截止时间 = 现在 + N 小时（最多 24）', '延长截止时间', {
      inputValue: '24',
      inputPattern: /^([1-9]|1[0-9]|2[0-4])$/,
      inputErrorMessage: '请输入 1-24',
      confirmButtonText: '延长',
      cancelButtonText: '取消',
    })
    hours = Number(r.value)
  } catch (e) {
    return 0
  }
  return Date.now() + hours * 3600 * 1000
}
