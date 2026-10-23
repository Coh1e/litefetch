import {SETTINGS_KEY, normalizeSettings} from './settings.mjs';
const form = document.querySelector('#settings');
const enabled = document.querySelector('#enabled');
const origins = document.querySelector('#origins');
const status = document.querySelector('#status');
form.querySelector('button').disabled = true;
try {
  const values = await chrome.storage.local.get(SETTINGS_KEY);
  const settings = normalizeSettings(values[SETTINGS_KEY]);
  enabled.checked = settings.enabled;
  origins.value = settings.origins.join('\n');
  form.querySelector('button').disabled = false;
} catch { status.textContent = '设置读取失败，请重新打开。'; }
form.addEventListener('submit', async event => {
  event.preventDefault();
  const lines = origins.value.split(/\r?\n/).map(line => line.trim()).filter(Boolean);
  const settings = normalizeSettings({enabled: enabled.checked, origins: lines});
  if ((lines.length && !settings.origins.length) || (enabled.checked && !settings.origins.length)) {
    status.textContent = '请输入有效的网站地址，不包含路径或账号。'; return;
  }
  try {
    await chrome.storage.local.set({[SETTINGS_KEY]: settings});
    status.textContent = '已保存。';
  } catch { status.textContent = '保存失败，原设置保留。'; }
});
