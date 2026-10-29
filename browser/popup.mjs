import {SETTINGS_KEY, normalizeSettings} from './settings.mjs';
import {IGNORE_KEY} from './worker.mjs';
const form = document.querySelector('#settings');
const enabled = document.querySelector('#enabled');
const origins = document.querySelector('#origins');
const status = document.querySelector('#status');
const ignored = document.querySelector('#ignored');
const host = document.querySelector('#host');
form.querySelector('button').disabled = true;

// Ask the application itself before blaming anything else: "not running" and
// "this site is not allowed" are different problems with different answers.
async function checkHost() {
  let port;
  try {
    port = chrome.runtime.connectNative('org.native_downloader.bridge');
  } catch {
    host.textContent = '本机下载器：未连接（无法启动本机组件，检查注册项）。';
    return;
  }
  const answer = await new Promise(resolve => {
    const timer = setTimeout(() => resolve({error: 'timeout'}), 3000);
    port.onMessage.addListener(message => { clearTimeout(timer); resolve(message); });
    port.onDisconnect.addListener(() => {
      clearTimeout(timer);
      const reason = chrome.runtime.lastError?.message ?? '';
      resolve({error: reason || 'disconnected'});
    });
    port.postMessage({version: 1, id: 'popup-ping', method: 'ping'});
  });
  try { port.disconnect(); } catch {}
  if (answer?.ok && answer.result?.state === 'ok') {
    host.textContent = '本机下载器：已连接。';
  } else if (answer && answer.ok === false) {
    // The component answered, so it is running; a refusal means the application
    // is an older build or does not accept this request.
    host.textContent =
      `本机下载器：已连接，但应用拒绝了这个请求（${answer.error ?? '未知原因'}），请更新到同一版本。`;
  } else {
    host.textContent = '本机下载器：未运行（请先启动 LiteFetch），接管不会发生。';
  }
}
await checkHost();

// The reasons the worker records are identifiers; the popup turns them into
// something a person can act on.
const explain = reason => ({
  capture_disabled: '接管已关闭，下载仍由浏览器处理。',
  origin_not_enabled: '这个站点不在允许列表里，下载仍由浏览器处理。',
  request_not_observed: '没有看到对应的网络请求（可能是页面缓存或直接由脚本触发），下载仍由浏览器处理。',
  request_not_replayable: '这个请求带有无法安全重放的信息（登录态、来源页等），下载仍由浏览器处理。',
  ambiguous_request: '同一地址有多个请求，无法确定该接管哪一个，下载仍由浏览器处理。',
  request_already_claimed: '这个请求已经交给本机了。',
  browser_not_eligible: '浏览器报告的下载状态不允许接管（例如已经开始写入或已完成）。',
  download_context_missing: '下载缺少可用的地址或时间信息，无法接管。',
  observation_overflow: '短时间内请求过多，扩展主动暂停了接管。',
}[reason] ?? `下载仍由浏览器处理（原因：${reason}）。`);

const phaseText = phase => ({
  prepared: '已在本机建好任务，等待提交',
  committing: '正在提交',
  committed: '已接管，任务已交给本机',
  released: '已放回浏览器',
  cancelled: '已取消',
  attention: '需要处理',
}[phase] ?? phase);

const failureText = reason => ({
  browser_cancel_not_confirmed: '浏览器没有确认取消原来的下载，任务留在本机未提交',
  browser_resume_failed: '取消原下载失败，且无法恢复浏览器的下载',
  native_task_not_prepared: '本机没有准备好任务',
  native_lease_invalid: '本机的准备已过期',
  native_status_unavailable: '连不上本机下载器',
  native_task_mismatch: '本机任务与这条接管记录不一致',
}[reason] ?? reason ?? '');

try {
  const values = await chrome.storage.local.get([SETTINGS_KEY, IGNORE_KEY]);
  const settings = normalizeSettings(values[SETTINGS_KEY]);
  enabled.checked = settings.enabled;
  origins.value = settings.origins.join('\n');
  form.querySelector('button').disabled = false;
  const last = values[IGNORE_KEY];
  const all = await chrome.storage.local.get(null);
  // The journal says what actually happened to each takeover, which is the
  // difference between "the browser kept the file" and "we paused it and then
  // failed to finish".
  const records = Object.entries(all)
    .filter(([key]) => key.startsWith('handoff.journal.'))
    .map(([, record]) => record)
    .filter(record => record && typeof record === 'object')
    .sort((left, right) => (right.at ?? right.downloadId ?? 0) - (left.at ?? left.downloadId ?? 0))
    .slice(0, 3)
    .map(record => `${phaseText(record.phase)}${record.reason ? `：${failureText(record.reason)}` : ''}`);
  const lines = [];
  if (last && last.reason) {
    const at = last.at ? new Date(last.at).toLocaleTimeString() : '';
    lines.push(`上次未接管：${explain(last.reason)}${at ? `（${at}）` : ''}`);
  }
  if (records.length) lines.push(`最近接管：${records.join('；')}`);
  ignored.textContent = lines.join('\n');
} catch { status.textContent = '设置读取失败，请重新打开。'; }
form.addEventListener('submit', async event => {
  event.preventDefault();
  const lines = origins.value.split(/\r?\n/).map(line => line.trim()).filter(Boolean);
  const settings = normalizeSettings({enabled: enabled.checked, origins: lines});
  try {
    await chrome.storage.local.set({[SETTINGS_KEY]: settings});
    status.textContent = settings.origins.length
      ? `已保存：只在 ${settings.origins.length} 个站点接管。`
      : '已保存：所有站点的下载都会交给 LiteFetch。';
  } catch { status.textContent = '保存失败，原设置保留。'; }
});
