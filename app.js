// BettaFeeder Web Bluetooth client — native JavaScript, no framework.
const BLE = {
  service: '7f510001-1b15-4f0b-9a2e-5b8f6a100001',
  rx:      '7f510002-1b15-4f0b-9a2e-5b8f6a100001',
  tx:      '7f510003-1b15-4f0b-9a2e-5b8f6a100001'
};

const $ = (id) => document.getElementById(id);
const encoder = new TextEncoder();
const decoder = new TextDecoder();
let device = null;
let server = null;
let rxCharacteristic = null;
let txCharacteristic = null;
let toastTimer = null;

function log(message, kind = 'info') {
  const logBox = $('log');
  const empty = logBox.querySelector('.log-empty');
  if (empty) empty.remove();
  const row = document.createElement('div');
  row.className = 'log-entry';
  const time = new Date().toLocaleTimeString('pt-BR', {hour:'2-digit', minute:'2-digit', second:'2-digit'});
  const stamp = document.createElement('span');
  stamp.className = 'time';
  stamp.textContent = time;
  const content = document.createElement('span');
  content.textContent = message;
  if (kind === 'error') content.style.color = '#ff9299';
  if (kind === 'success') content.style.color = '#00ffb9';
  row.append(stamp, content);
  logBox.prepend(row);
  while (logBox.children.length > 40) logBox.lastElementChild.remove();
}

function toast(message) {
  const el = $('toast');
  el.textContent = message;
  el.classList.add('show');
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => el.classList.remove('show'), 2600);
}

function setConnected(connected, name = 'BettaFeeder') {
  const badge = $('connectionBadge');
  badge.classList.toggle('online', connected);
  badge.classList.toggle('offline', !connected);
  badge.innerHTML = `<i></i> ${connected ? 'Conectado' : 'Desconectado'}`;
  $('deviceName').textContent = name;
  $('deviceDescription').textContent = connected ? 'Conectado via Bluetooth Low Energy' : 'Nenhum dispositivo conectado';
  ['saveTimesButton','saveWifiButton','statusButton','testFeedButton','homeButton'].forEach(id => $(id).disabled = !connected);
  $('connectButton').textContent = connected ? 'Desconectar' : 'Conectar';
}

function parseDeviceMessage(message) {
  log(`ESP32: ${message}`, 'success');
  if (!message.startsWith('STATUS;')) return;
  const fields = Object.fromEntries(message.split(';').slice(1).map(part => {
    const i = part.indexOf('=');
    return i < 0 ? [part, ''] : [part.slice(0, i), part.slice(i + 1)];
  }));
  $('wifiStatus').textContent = fields.wifi || '—';
  $('clockStatus').textContent = fields.hora && fields.data ? `${fields.hora} · ${fields.data}` : (fields.hora || '—');
  $('positionStatus').textContent = fields.posicao || '—';
  $('portionsStatus').textContent = fields.porcoes_lote || '—';
  if (fields.feed1) $('feed1').value = fields.feed1;
  if (fields.feed2) $('feed2').value = fields.feed2;
}

function onNotification(event) {
  const value = event.target.value;
  const message = decoder.decode(value);
  message.split(/\r?\n/).filter(Boolean).forEach(parseDeviceMessage);
}

async function connect() {
  if (!('bluetooth' in navigator)) {
    $('bleWarning').classList.remove('hidden');
    throw new Error('Web Bluetooth não está disponível neste navegador.');
  }
  if (!window.isSecureContext) throw new Error('O Bluetooth exige HTTPS ou localhost.');
  log('Procurando BettaFeeder por Bluetooth…');
  device = await navigator.bluetooth.requestDevice({
    filters: [{name: 'BettaFeeder'}],
    optionalServices: [BLE.service]
  });
  device.addEventListener('gattserverdisconnected', onDisconnected);
  server = await device.gatt.connect();
  const service = await server.getPrimaryService(BLE.service);
  rxCharacteristic = await service.getCharacteristic(BLE.rx);
  txCharacteristic = await service.getCharacteristic(BLE.tx);
  await txCharacteristic.startNotifications();
  txCharacteristic.addEventListener('characteristicvaluechanged', onNotification);
  setConnected(true, device.name || 'BettaFeeder');
  log('Bluetooth conectado e notificações ativadas.', 'success');
  toast('Conectado ao BettaFeeder');
  await sendCommand('STATUS');
}

function onDisconnected() {
  rxCharacteristic = null;
  txCharacteristic = null;
  server = null;
  setConnected(false);
  log('Dispositivo Bluetooth desconectado.', 'error');
  toast('Conexão encerrada');
}

async function disconnect() {
  if (device?.gatt?.connected) device.gatt.disconnect();
  else onDisconnected();
}

async function sendCommand(command) {
  if (!rxCharacteristic) throw new Error('Conecte ao BettaFeeder primeiro.');
  // Firmware parses one command per GATT write. Write with response for reliability.
  await rxCharacteristic.writeValueWithResponse(encoder.encode(command));
  log(`Enviado: ${command}`);
}

async function guarded(action) {
  try { await action(); }
  catch (error) {
    console.error(error);
    log(error?.message || String(error), 'error');
    toast(error?.message || 'Ocorreu um erro');
  }
}

$('connectButton').addEventListener('click', () => guarded(async () => {
  if (device?.gatt?.connected) return disconnect();
  await connect();
}));

$('saveTimesButton').addEventListener('click', () => guarded(async () => {
  const first = $('feed1').value;
  const second = $('feed2').value;
  if (!first || !second) throw new Error('Informe os dois horários.');
  if (first === second) throw new Error('Os horários precisam ser diferentes.');
  await sendCommand(`FEED1=${first}`);
  await new Promise(resolve => setTimeout(resolve, 180));
  await sendCommand(`FEED2=${second}`);
  toast('Horários enviados ao ESP32');
}));

$('wifiForm').addEventListener('submit', event => guarded(async () => {
  event.preventDefault();
  const ssid = $('ssid').value.trim();
  const password = $('password').value;
  if (!ssid) throw new Error('Informe o nome da rede Wi-Fi.');
  if (!password) throw new Error('Informe a senha do Wi-Fi.');
  // The SSID and password are sent as two distinct BLE writes.
  await sendCommand(`SSID=${ssid}`);
  await new Promise(resolve => setTimeout(resolve, 250));
  await sendCommand(`PASS=${password}`);
  $('password').value = '';
  toast('Dados do Wi-Fi enviados');
}));

$('togglePassword').addEventListener('click', () => {
  const input = $('password');
  const visible = input.type === 'password';
  input.type = visible ? 'text' : 'password';
  $('togglePassword').textContent = visible ? 'Ocultar' : 'Mostrar';
});

$('statusButton').addEventListener('click', () => guarded(() => sendCommand('STATUS')));
$('testFeedButton').addEventListener('click', () => guarded(async () => {
  if (!confirm('Acionar o motor e liberar uma porção agora?')) return;
  await sendCommand('FEED');
}));
$('homeButton').addEventListener('click', () => guarded(async () => {
  if (!confirm('Confirma que a cavidade vazia está fisicamente alinhada com a saída?')) return;
  await sendCommand('SET_HOME');
}));
$('clearLogButton').addEventListener('click', () => {
  $('log').innerHTML = '<div class="log-empty">Os eventos de conexão e configuração aparecerão aqui.</div>';
});

if (!('bluetooth' in navigator) || !window.isSecureContext) {
  $('bleWarning').classList.remove('hidden');
}

if ('serviceWorker' in navigator && window.isSecureContext) {
  window.addEventListener('load', () => {
    navigator.serviceWorker.register('./sw.js').catch(err => {
      console.warn('Falha ao registrar service worker:', err);
    });
  });
}
