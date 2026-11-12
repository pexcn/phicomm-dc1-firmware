'use strict';

const $ = (selector, root = document) => root.querySelector(selector);
const $$ = (selector, root = document) => [...root.querySelectorAll(selector)];
const routes = {
  overview: ['设备概览', '查看插座状态与整机用电情况。'],
  settings: ['插座设置', '设置上电行为、指示灯与过载保护。'],
  network: ['网络连接', '管理 Wi-Fi、MQTT 与 Home Assistant 连接。'],
  logs: ['运行日志', '查看设备运行与连接事件。'],
  system: ['系统维护', '查看设备信息、管理存储与升级固件。']
};
let config = null;
let loggedIn = false;
let setupRequired = false;
let authBusy = false;
let activeView = 'overview';
let online = false;
let timer = 0;
let polling = false;
let logIndex = 0;
let revision = 0;
let relayBusy = false;
let haveRelayState = false;
let maintenanceBusy = false;
let restarting = false;
let lastUpdate = '';
let toastTimer = 0;
let drawerFocus = null;
const mobile = matchMedia('(max-width: 1024px)');
const systemTheme = matchMedia('(prefers-color-scheme: dark)');
const initialHash = location.hash;

function updateTheme() {
  const mode = document.documentElement.dataset.themeMode;
  document.documentElement.dataset.theme =
    mode === 'system' ? (systemTheme.matches ? 'dark' : 'light') : mode;
  const label = { system: '跟随系统', light: '浅色模式', dark: '深色模式' }[mode];
  $$('[data-theme-toggle]').forEach((button) => {
    $('use', button).setAttribute(
      'href',
      '#i-' + { system: 'system', light: 'sun', dark: 'moon' }[mode]
    );
    button.title = label;
    button.setAttribute('aria-label', '切换主题，当前' + label);
  });
}
$$('[data-theme-toggle]').forEach((button) =>
  button.addEventListener('click', () => {
    const modes = ['system', 'light', 'dark'];
    const mode =
      modes[(modes.indexOf(document.documentElement.dataset.themeMode) + 1) % modes.length];
    document.documentElement.dataset.themeMode = mode;
    try {
      localStorage.setItem('dc1-theme', mode);
    } catch (_) {}
    updateTheme();
  })
);
systemTheme.addEventListener('change', updateTheme);
updateTheme();

function setDrawer(open, restoreFocus = true) {
  open = open && mobile.matches;
  if (open) drawerFocus = document.activeElement;
  document.body.classList.toggle('drawer-open', open);
  $('#backdrop').hidden = !open;
  $('#menu-toggle').setAttribute('aria-expanded', String(open));
  $('#main').inert = open;
  $('#topbar').inert = open;
  $('#sidebar').inert = mobile.matches && !open;
  if (open) $('#drawer-close').focus();
  else if (restoreFocus && drawerFocus) {
    drawerFocus.focus();
    drawerFocus = null;
  }
}
$('#menu-toggle').addEventListener('click', () => setDrawer(true));
$('#drawer-close').addEventListener('click', () => setDrawer(false));
$('#backdrop').addEventListener('click', () => setDrawer(false));
mobile.addEventListener('change', () => setDrawer(false));
document.addEventListener('keydown', (event) => {
  if (!document.body.classList.contains('drawer-open')) return;
  if (event.key === 'Escape') {
    event.preventDefault();
    setDrawer(false);
  }
  if (event.key === 'Tab') {
    const elements = $$('a[href], button', $('#sidebar')).filter(
      (el) => !el.hidden && el.getClientRects().length
    );
    const first = elements[0],
      last = elements[elements.length - 1];
    if (event.shiftKey && document.activeElement === first) {
      event.preventDefault();
      last.focus();
    } else if (!event.shiftKey && document.activeElement === last) {
      event.preventDefault();
      first.focus();
    }
  }
});
setDrawer(false);

function focusView(scrollToTop = false) {
  const target = $(
    location.hash === '#system/storage' && config?.features.module
      ? '#runtime-storage'
      : '#page-title'
  );
  target.focus({ preventScroll: true });
  if (target.id === 'runtime-storage') target.scrollIntoView({ block: 'start' });
  else if (scrollToTop) window.scrollTo(0, 0);
}

function navigate(focus = false) {
  let route = location.hash.slice(1).split('/')[0] || 'overview';
  if (
    !routes[route] ||
    (config && route === 'settings' && !config.features.module) ||
    (config && route === 'logs' && !config.features.logs)
  )
    route = 'overview';
  const storage =
    route === 'system' &&
    location.hash === '#system/storage' &&
    (!config || config.features.module);
  const hash = '#' + route + (storage ? '/storage' : '');
  if (location.hash !== hash) history.replaceState(null, '', hash);
  activeView = route;
  document.documentElement.dataset.page = route;
  $$('.view').forEach((view) => {
    view.hidden = view.id !== 'view-' + route;
  });
  $$('[data-route]').forEach((link) => {
    if (link.dataset.route === route) link.setAttribute('aria-current', 'page');
    else link.removeAttribute('aria-current');
  });
  $('#page-title').textContent = routes[route][0];
  $('#page-description').textContent = routes[route][1];
  setDrawer(false, false);
  if (focus) focusView(true);
  schedulePoll(0);
}
window.addEventListener('hashchange', () => navigate(true));
$$('[data-route], a[href="#system/storage"]').forEach((link) =>
  link.addEventListener('click', () => {
    if (location.hash === link.getAttribute('href')) {
      setDrawer(false, false);
      focusView(true);
    }
  })
);
navigate();

function toast(message, error = false) {
  clearTimeout(toastTimer);
  $('#toast').textContent = message;
  $('#toast').classList.toggle('error', error);
  $('#toast').hidden = false;
  toastTimer = setTimeout(
    () => {
      $('#toast').hidden = true;
    },
    error ? 10000 : 5000
  );
}

async function request(path, data = {}, timeout = 12000) {
  const controller = new AbortController();
  const deadline = setTimeout(() => controller.abort(), timeout);
  try {
    const response = await fetch(path, {
      method: 'POST',
      body: data instanceof URLSearchParams ? data : new URLSearchParams(data),
      cache: 'no-store',
      credentials: 'same-origin',
      headers: { 'X-DC1-Request': '1' },
      signal: controller.signal
    });
    if (response.status === 401 && path !== '/login' && path !== '/password') {
      showLogin(false, '登录已失效，请重新登录。');
    }
    let result;
    try {
      result = await response.json();
    } catch (error) {
      if (error.name === 'AbortError') throw error;
      throw Error(
        response.status === 401
          ? '登录已失效，请刷新页面重新登录。'
          : '设备返回了无效响应，请稍后重试。'
      );
    }
    if (!response.ok || Number(result.code) !== 1)
      throw Error(result.msg || '操作失败，请稍后重试。');
    return result;
  } catch (error) {
    if (error.name === 'AbortError') throw Error('请求超时，请检查设备连接后重试。');
    if (error instanceof TypeError) throw Error('无法连接设备，请检查网络后重试。');
    throw error;
  } finally {
    clearTimeout(deadline);
  }
}

function setValues(data) {
  $$('[data-value]').forEach((element) => {
    const value = data[element.dataset.value];
    if (value !== undefined)
      element.textContent =
        value === null || value === 'null' || value === '' ? '--' : String(value);
  });
}

function setOnline(value) {
  online = value;
  $('#connection').classList.toggle('online', value);
  $('#connection-label').textContent = value ? '设备在线' : restarting ? '等待重启' : '连接中断';
  document.body.classList.toggle('stale', !value);
  $$('[data-relay]').forEach((button) => {
    button.disabled = !value || !haveRelayState || relayBusy || maintenanceBusy;
  });
}

function renderStatus(data) {
  setValues(data);
  const uptime = /^(\d+)T(\d{2}):(\d{2}):(\d{2})$/.exec(data.uptime);
  if (uptime) {
    const [, days, hours, minutes, seconds] = uptime.map(Number);
    $('#uptime').textContent = days
      ? `${days} 天 ${hours} 小时`
      : hours
        ? `${hours} 小时 ${minutes} 分`
        : `${minutes} 分 ${seconds} 秒`;
    $('#uptime').title = `${days} 天 ${hours} 小时 ${minutes} 分 ${seconds} 秒`;
  }
  if ([1, 2, 3, 4].some((channel) => Object.hasOwn(data, 'power' + channel))) {
    haveRelayState = [1, 2, 3, 4].every((channel) => [0, 1].includes(data['power' + channel]));
  }
  $$('[data-relay]').forEach((button) => {
    const channel = button.dataset.relay;
    if (!Object.hasOwn(data, 'power' + channel)) return;
    const on = Number(data['power' + channel]) === 1;
    button.setAttribute('aria-checked', String(on));
    $$('[data-relay-state="' + channel + '"]').forEach((label) => {
      label.textContent = on ? '已开启' : '已关闭';
      label.classList.toggle('on', on);
    });
  });
  if (Object.hasOwn(data, 'discovery')) {
    $('#discovery-status').textContent = data.discovery ? '已启用' : '未启用';
    $('#discovery-button').textContent = data.discovery ? '关闭自动发现' : '启用自动发现';
  }
  if (Object.hasOwn(data, 'runtime_ready')) {
    const ready = Boolean(data.runtime_ready);
    const badRelay = Boolean(data.relay_needs_recovery);
    const badEnergy = Boolean(data.energy_needs_recovery);
    $('#initialize-storage').hidden = ready;
    $('#recover-relay').hidden = !ready || !badRelay;
    $('#recover-energy').hidden = !ready || !badEnergy;
    const relay = $('#relay-summary'),
      energy = $('#energy-summary');
    relay.textContent = !ready ? '存储不可用' : badRelay ? '需恢复' : data.relay_record || '--';
    energy.textContent = !ready
      ? '存储不可用'
      : badEnergy
        ? '记录损坏'
        : data.energy_record || '--';
    relay.className = ready && !badRelay ? 'ok' : 'warning-text';
    energy.className = ready && !badEnergy ? 'ok' : 'warning-text';
    $('#storage-warning').hidden = ready && !badRelay && !badEnergy;
    $('#storage-warning-text').textContent = !ready
      ? '运行数据存储不可用，开关记忆与电量增量可能无法保存。'
      : [
          badRelay ? '开关记忆无效，记忆与反转上电会保持关闭。' : '',
          badEnergy ? '电量记录损坏：当前仅显示临时统计，已暂停保存。' : ''
        ]
          .filter(Boolean)
          .join(' ');
  }
  if (Object.hasOwn(data, 'log')) {
    const output = $('#log-output');
    const atBottom = output.scrollHeight - output.scrollTop - output.clientHeight < 50;
    if (data.log)
      output.textContent = (output.textContent + (output.textContent ? '\n' : '') + data.log).slice(
        -40000
      );
    if (atBottom) output.scrollTop = output.scrollHeight;
    logIndex = data.logindex || 0;
    $('#log-status').textContent = '已更新 · ' + new Date().toLocaleTimeString();
  }
}

function schedulePoll(delay = 3000) {
  clearTimeout(timer);
  if (!config || document.hidden || maintenanceBusy || restarting) return;
  if (activeView === 'settings' || (activeView === 'network' && !config.device.portal)) return;
  if (activeView === 'logs' && !$('#log-auto').checked) return;
  timer = setTimeout(() => pollStatus(), delay);
}

async function pollStatus(manual = false) {
  if (polling || !config || document.hidden || maintenanceBusy || restarting) return;
  polling = true;
  const currentRevision = revision;
  const withLogs = activeView === 'logs';
  try {
    const result = await request('/get_status', withLogs ? { i: logIndex } : {});
    if (revision !== currentRevision) return;
    renderStatus(result.data || {});
    setOnline(true);
    lastUpdate = new Date().toLocaleTimeString();
    $('#updated-at').textContent = '数据更新于 ' + lastUpdate;
    $('#status-warning').hidden = true;
    if (result.data && result.data.ip && config.device.portal) {
      // The provisioning AP can stay reachable while the station connects.
      config.device.portal = false;
      toast('Wi-Fi 已连接，局域网地址：' + result.data.ip);
      setValues({ local_ip: result.data.ip });
    }
  } catch (error) {
    if (revision !== currentRevision) return;
    setOnline(false);
    $('#status-warning').hidden = false;
    $('#status-warning').textContent =
      error.message +
      (lastUpdate ? ' 当前显示的是 ' + lastUpdate + ' 的数据。' : ' 暂无有效状态。');
    if (withLogs) $('#log-status').textContent = '读取失败，等待重试';
    if (manual) toast(error.message, true);
  } finally {
    polling = false;
    schedulePoll(activeView === 'logs' ? 1000 : 3000);
  }
}
document.addEventListener('visibilitychange', async () => {
  if (document.hidden || !loggedIn) return;
  try {
    const { data } = await request('/auth');
    if (!data.authenticated) showLogin(Boolean(data.setup_required), '登录已失效，请重新登录。');
    else schedulePoll(0);
  } catch {
    schedulePoll(0);
  }
});
window.addEventListener('pageshow', (event) => {
  if (event.persisted) {
    showLogin();
    checkLogin();
  }
});
$('#log-auto').addEventListener('change', () => schedulePoll(0));
$('#log-refresh').addEventListener('click', () => pollStatus(true));
$('#log-clear').addEventListener('click', () => {
  $('#log-output').textContent = '';
});

function formData(form) {
  return new URLSearchParams(new FormData(form));
}
function dirtyStatus(form) {
  const status = $('.save-status', form);
  if (status)
    status.textContent =
      form.dataset.baseline === formData(form).toString() ? '无未保存的更改' : '有未保存的更改';
}

function configureFeatures(features) {
  $$('[data-feature]').forEach((element) => {
    const enabled = Boolean(features[element.dataset.feature]);
    element.hidden = !enabled;
    $$('input, button, select', element).forEach((input) => {
      input.disabled = !enabled;
    });
  });
  $$('[data-module]').forEach((element) => {
    element.hidden = !features.module;
  });
}

async function loadConfig() {
  $('#retry-config').disabled = true;
  try {
    const currentRevision = revision;
    const result = await request('/get_config');
    if (!loggedIn || revision !== currentRevision) return;
    const data = result.data;
    if (!data || !data.features || !data.device || !data.fields || !data.module) {
      throw Error('设备返回的设置不完整，请刷新后重试。');
    }
    config = data;
    setValues(config.device);
    const fields = { ...config.fields, ...config.module };
    $$('form[data-settings]').forEach((form) => {
      $$('[name]', form).forEach((input) => {
        if (!Object.hasOwn(fields, input.name)) return;
        if (input.type === 'checkbox') input.checked = Boolean(Number(fields[input.name]));
        else input.value = fields[input.name];
      });
    });
    $$('#main form > fieldset').forEach((fieldset) => {
      fieldset.disabled = false;
    });
    configureFeatures(config.features);
    $$('form[data-settings]').forEach((form) => {
      form.dataset.baseline = formData(form).toString();
      dirtyStatus(form);
    });
    ['#restart', '#factory-reset', '#clear-energy'].forEach((selector) => {
      $(selector).disabled = false;
    });
    $('#load-error').hidden = true;
    document.title = config.features.module ? 'DC1 智能插排' : '设备修复模式';
    if (!config.features.module) toast('设备处于修复模式，可在系统维护中检查与升级固件。');
    updateDhcp();
    if (config.device.portal && (!initialHash || initialHash === '#overview'))
      history.replaceState(null, '', '#network');
    navigate();
    // Read once even when the initial route is a form, then use view-aware polling.
    await pollStatus();
  } catch (error) {
    if (!loggedIn) return;
    $('#load-error-text').textContent = '无法读取设备设置：' + error.message;
    $('#load-error').hidden = false;
    setOnline(false);
  } finally {
    $('#retry-config').disabled = false;
  }
}
$('#retry-config').addEventListener('click', loadConfig);

function confirmAction(title, description, label = '确认') {
  const dialog = $('#confirm-dialog');
  $('#confirm-title').textContent = title;
  $('#confirm-description').textContent = description;
  $('#confirm-ok').textContent = label;
  return new Promise((resolve) => {
    dialog.returnValue = '';
    dialog.addEventListener('close', () => resolve(dialog.returnValue === 'confirm'), {
      once: true
    });
    dialog.showModal();
  });
}
$('#confirm-cancel').addEventListener('click', () => $('#confirm-dialog').close('cancel'));
$('#confirm-ok').addEventListener('click', () => $('#confirm-dialog').close('confirm'));

function markRestarting() {
  restarting = true;
  clearTimeout(timer);
  setOnline(false);
  $('#status-warning').hidden = false;
  $('#status-warning').textContent = '设备正在重启，待网络恢复后请刷新页面。';
  lockControls();
}

function lockControls() {
  const controls = $$(
    '#main form > fieldset, .danger-section button, #initialize-storage, #recover-relay, #recover-energy, #logout'
  );
  const previous = controls.map((control) => control.disabled);
  controls.forEach((control) => {
    control.disabled = true;
  });
  return () =>
    controls.forEach((control, index) => {
      control.disabled = restarting || previous[index];
    });
}

$$('form[data-settings]').forEach((form) => {
  form.addEventListener('input', () => dirtyStatus(form));
  form.addEventListener('change', () => dirtyStatus(form));
  form.addEventListener('submit', async (event) => {
    event.preventDefault();
    if (!config || maintenanceBusy || restarting || form.dataset.busy) return;
    if (form.id === 'module-form' && form.elements.uid.value !== config.fields.uid) {
      if (
        !(await confirmAction(
          '保存并重启设备？',
          '主机名已更改。设备会先保存数据，再重启应用新设置。',
          '保存并重启'
        ))
      )
        return;
    }
    const data = formData(form);
    const submitted = data.toString();
    const currentRevision = revision;
    form.dataset.busy = '1';
    const button = $('button[type="submit"]', form);
    button.disabled = true;
    try {
      const result = await request(form.getAttribute('action'), data);
      if (!loggedIn || revision !== currentRevision) return;
      revision++;
      if (result.data) renderStatus(result.data);
      form.dataset.baseline = submitted;
      dirtyStatus(form);
      toast(result.msg || '设置已保存。');
      if (form.id === 'module-form' && data.get('uid') !== config.fields.uid) markRestarting();
      else schedulePoll(0);
    } catch (error) {
      toast(error.message, true);
    } finally {
      delete form.dataset.busy;
      button.disabled = false;
    }
  });
});

function updateDhcp() {
  const isStatic = $('#dhcp').value === '2';
  $('#static-fields').hidden = !isStatic;
  $$('input', $('#static-fields')).forEach((input) => {
    input.required = isStatic;
  });
}
$('#dhcp').addEventListener('change', updateDhcp);

$('#scan-wifi').addEventListener('click', async () => {
  const button = $('#scan-wifi');
  button.disabled = true;
  $('#scan-status').textContent = '正在搜索附近的网络…';
  try {
    const result = await request('/scan_wifi', {}, 30000);
    const networks = result.data.list || [];
    const list = $('#wifi-list');
    list.replaceChildren();
    networks.forEach((network) => {
      const item = document.createElement('button');
      item.type = 'button';
      const name = document.createElement('span'),
        signal = document.createElement('small');
      name.textContent = network.name || '隐藏网络';
      signal.textContent = network.rssi + ' dBm';
      item.append(name, signal);
      item.addEventListener('click', () => {
        $('#wifi-ssid').value = network.name;
        dirtyStatus($('#wifi-form'));
        $('#wifi-form').elements.wifi_password.focus();
      });
      list.append(item);
    });
    list.hidden = !networks.length;
    $('#scan-status').textContent = networks.length
      ? '点击网络名称进行选择。'
      : '未发现网络，可手动输入或重新搜索。';
  } catch (error) {
    $('#scan-status').textContent = error.message;
  } finally {
    button.disabled = false;
  }
});

$$('[data-relay]').forEach((button) =>
  button.addEventListener('click', async () => {
    if (!online || relayBusy || maintenanceBusy || restarting) return;
    relayBusy = true;
    revision++;
    setOnline(online);
    try {
      const result = await request('/dc1_do', {
        c: button.dataset.relay,
        do: button.getAttribute('aria-checked') === 'true' ? 'off' : 'on'
      });
      revision++;
      renderStatus(result.data || {});
    } catch (error) {
      toast(error.message, true);
      setOnline(false);
    } finally {
      relayBusy = false;
      setOnline(online);
      schedulePoll(0);
    }
  })
);

const actions = [
  [
    '#initialize-storage',
    '初始化运行数据存储？',
    '仅在首次使用或文件系统无法挂载时使用。\n可能清除历史电量和开关记忆。',
    '/dc1_storage',
    { initialize: 1 }
  ],
  [
    '#recover-relay',
    '恢复开关记忆？',
    '使用当前实际开关状态重新建立记忆，替换损坏或缺失的旧记录。',
    '/dc1_storage',
    { recover: 'relay' }
  ],
  [
    '#recover-energy',
    '重新开始电量统计？',
    '损坏的电量记录将被替换，今日、昨日与累计用电量从零重新统计。',
    '/dc1_storage',
    { recover: 'energy' }
  ],
  [
    '#clear-energy',
    '重置全部用电统计？',
    '今日、昨日与累计用电量将全部清零，此操作无法撤销。',
    '/dc1_setting',
    { c: 1 }
  ],
  [
    '#restart',
    '重启设备？',
    '先保存尚未写入的数据，再重启设备。输出会短暂中断，上电后按当前策略恢复。',
    '/operate',
    { d: 1 }
  ],
  [
    '#factory-reset',
    '恢复出厂设置？',
    '清除网络及设备配置、全部用电统计与开关记忆，然后重启。此操作无法撤销。',
    '/operate',
    { d: 2 }
  ]
];
actions.forEach(([selector, title, description, path, data]) => {
  $(selector).addEventListener('click', async () => {
    if (!config || maintenanceBusy || restarting || !(await confirmAction(title, description)))
      return;
    maintenanceBusy = true;
    revision++;
    setOnline(online);
    const unlock = lockControls();
    try {
      const result = await request(path, data);
      if (result.data) renderStatus(result.data);
      toast(result.msg || '操作已完成。');
      if (path === '/operate') markRestarting();
    } catch (error) {
      toast(error.message, true);
    } finally {
      maintenanceBusy = false;
      unlock();
      setOnline(online);
      schedulePoll(0);
    }
  });
});

$('#upload-form').addEventListener('submit', async (event) => {
  event.preventDefault();
  if (!config || maintenanceBusy || restarting) return;
  const form = event.currentTarget;
  const file = form.elements.update.files[0];
  if (!file || !/\.bin$/i.test(file.name) || !file.size) {
    toast('请选择非空的 .bin 应用固件。', true);
    return;
  }
  try {
    const trailer = await file.slice(-68).arrayBuffer();
    const identity = new TextDecoder().decode(trailer.slice(0, 32)).replace(/\0+$/, '');
    if (
      trailer.byteLength !== 68 ||
      identity !== config.device.firmware_id ||
      new DataView(trailer).getUint32(32, true) !== file.size - 68
    ) {
      toast('固件不匹配或缺少校验信息，请选择本项目新版 Release 的 .bin 文件。', true);
      return;
    }
  } catch {
    toast('无法读取固件文件，请重新选择。', true);
    return;
  }
  if (
    !(await confirmAction(
      '上传并升级固件？',
      '文件：' + file.name + '\n设备会先保存数据。上传和升级期间请保持供电，完成后自动重启。',
      '开始升级'
    ))
  )
    return;
  const payload = new FormData(form);
  const unlock = lockControls();
  maintenanceBusy = true;
  revision++;
  clearTimeout(timer);
  setOnline(online);
  $('#upload-button').disabled = true;
  $('#upload-progress').hidden = false;
  $('#upload-progress').value = 0;
  $('#upload-status').textContent = '正在上传固件…';
  const xhr = new XMLHttpRequest();
  xhr.open('POST', '/update');
  xhr.setRequestHeader('X-DC1-Request', '1');
  xhr.responseType = 'json';
  xhr.timeout = 180000;
  xhr.upload.onprogress = (event) => {
    if (!event.lengthComputable) return;
    const percent = Math.floor((event.loaded * 100) / event.total);
    $('#upload-progress').value = percent;
    $('#upload-status').textContent =
      percent === 100
        ? '上传完成，正在等待设备校验与升级结果…'
        : '已上传 ' + percent + '%，请保持供电。';
  };
  const finish = (error, result) => {
    maintenanceBusy = false;
    $('#upload-button').disabled = false;
    if (error) {
      $('#upload-status').textContent = error;
      toast(error, true);
      setOnline(false);
      schedulePoll(0);
    } else {
      $('#upload-status').textContent = result.msg || '升级成功，设备正在重启。';
      toast($('#upload-status').textContent);
      markRestarting();
    }
    unlock();
  };
  xhr.onload = () => {
    if (xhr.status === 401) showLogin(false, '登录已失效，请重新登录。');
    const result = xhr.response;
    if (xhr.status >= 200 && xhr.status < 300 && result && Number(result.code) === 1)
      finish(null, result);
    else finish(result && result.msg ? result.msg : '升级失败或结果未知，请检查设备后重试。');
  };
  xhr.onerror = () => finish('上传连接中断，升级结果未知，请检查设备状态。');
  xhr.ontimeout = () => finish('上传超时，升级结果未知，请检查设备状态。');
  xhr.onabort = () => finish('上传已中断，请检查设备状态。');
  xhr.send(payload);
});

function updateFirmwareName() {
  $('#firmware-name').textContent = $('#firmware-file').files[0]?.name || '未选择文件';
}
$('#firmware-file').addEventListener('change', updateFirmwareName);

function showLogin(setup = false, message = '') {
  loggedIn = false;
  setupRequired = setup;
  config = null;
  revision++;
  clearTimeout(timer);
  haveRelayState = false;
  setOnline(false);
  setDrawer(false, false);
  if ($('#confirm-dialog').open) $('#confirm-dialog').close('cancel');
  document.body.classList.add('auth-screen');
  $('#login-screen').hidden = false;
  document.title = '登录 · DC1 智能插排';
  // Discard private data and unsaved fields when a session ends.
  $$('#main form').forEach((form) => {
    form.reset();
    delete form.dataset.baseline;
  });
  updateFirmwareName();
  $$('#main form > fieldset').forEach((fieldset) => {
    fieldset.disabled = true;
  });
  $$('[data-value]').forEach((element) => {
    element.textContent = '--';
  });
  $('#log-output').textContent = '';
  $('#uptime').textContent = '--';
  logIndex = 0;
  lastUpdate = '';
  $('#updated-at').textContent = '';
  $('#toast').hidden = true;
  $('#password-status').textContent = '';
  $('#login-form').reset();
  $('#login-password').type = 'password';
  $('#login-password').minLength = setup ? 8 : 0;
  $('#login-password').autocomplete = setup ? 'new-password' : 'current-password';
  $('#login-hint').hidden = !setup;
  $('#login-submit').textContent = setup ? '设置密码并登录 →' : '登录 →';
  $('#login-status').textContent = '';
  $('#login-error').textContent = message;
  $('#login-error').hidden = !message;
  $('#login-retry').hidden = true;
  $('#login-form fieldset').disabled = false;
  const toggle = $('[data-password-toggle="login-password"]');
  toggle.setAttribute('aria-label', '显示密码');
  toggle.setAttribute('aria-pressed', 'false');
  $('use', toggle).setAttribute('href', '#i-eye');
  $('#login-password').focus({ preventScroll: true });
}

async function enterApp() {
  loggedIn = true;
  $('#login-form').reset();
  $('#login-screen').hidden = true;
  document.body.classList.remove('auth-screen');
  await loadConfig();
  if (loggedIn) focusView();
}

async function checkLogin() {
  $('#login-retry').hidden = true;
  $('#login-form fieldset').disabled = true;
  $('#login-status').textContent = '正在连接设备…';
  try {
    const { data } = await request('/auth');
    if (!data || !Object.hasOwn(data, 'authenticated')) throw Error('设备返回了无效响应。');
    if (data.authenticated) await enterApp();
    else showLogin(Boolean(data.setup_required));
  } catch (error) {
    $('#login-status').textContent = error.message;
    $('#login-retry').hidden = false;
  }
}
$('#login-retry').addEventListener('click', checkLogin);

$$('[data-password-toggle]').forEach((button) =>
  button.addEventListener('click', () => {
    const input = $('#' + button.dataset.passwordToggle);
    const visible = input.type === 'password';
    input.type = visible ? 'text' : 'password';
    button.setAttribute('aria-label', visible ? '隐藏密码' : '显示密码');
    button.setAttribute('aria-pressed', String(visible));
    $('use', button).setAttribute('href', visible ? '#i-eye-off' : '#i-eye');
  })
);

$('#login-form').addEventListener('submit', async (event) => {
  event.preventDefault();
  if (authBusy) return;
  const form = event.currentTarget;
  const data = formData(form);
  authBusy = true;
  $('fieldset', form).disabled = true;
  $('#login-error').hidden = true;
  $('#login-status').textContent = setupRequired ? '正在设置密码…' : '正在登录…';
  try {
    await request('/login', data);
    await enterApp();
  } catch (error) {
    $('#login-error').textContent = error.message;
    $('#login-error').hidden = false;
    $('#login-password').select();
  } finally {
    authBusy = false;
    $('fieldset', form).disabled = false;
    $('#login-status').textContent = '';
  }
});

$('#logout').addEventListener('click', async () => {
  if (!loggedIn || maintenanceBusy || restarting || authBusy) return;
  authBusy = true;
  $('#logout').disabled = true;
  try {
    await request('/logout');
    showLogin();
  } catch (error) {
    if (loggedIn) toast(error.message, true);
  } finally {
    authBusy = false;
    $('#logout').disabled = false;
  }
});

$('#password-form').addEventListener('submit', async (event) => {
  event.preventDefault();
  if (!loggedIn || maintenanceBusy || restarting || authBusy) return;
  const form = event.currentTarget;
  if (form.elements.new_password.value !== form.elements.confirm_password.value) {
    $('#password-status').textContent = '两次输入的新密码不一致。';
    form.elements.confirm_password.focus();
    return;
  }
  const data = formData(form);
  authBusy = true;
  $('fieldset', form).disabled = true;
  $('#password-status').textContent = '正在保存…';
  try {
    const result = await request('/password', data);
    form.reset();
    $('#password-status').textContent = result.msg;
  } catch (error) {
    $('#password-status').textContent = error.message;
  } finally {
    authBusy = false;
    $('fieldset', form).disabled = !loggedIn;
  }
});

// A restored session can open the dashboard; otherwise only the login shell runs.
checkLogin();
