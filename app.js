// BettaFeeder Web Bluetooth client — native JavaScript, no framework.
const BLE = {
	service: '7f510001-1b15-4f0b-9a2e-5b8f6a100001',
	rx: '7f510002-1b15-4f0b-9a2e-5b8f6a100001',
	tx: '7f510003-1b15-4f0b-9a2e-5b8f6a100001'
}

const $ = (id) => document.getElementById(id)
const encoder = new TextEncoder()
const decoder = new TextDecoder()
let device = null
let server = null
let rxCharacteristic = null
let txCharacteristic = null
let serialPort = null
let serialReader = null
let serialWriter = null
let serialConnected = false
let connectionMode = null
let serialReadBuffer = ''
let toastTimer = null
let connectionInProgress = false
let rxFragments = ''
const bleResponseWaiters = []

function isConnected() {
	return Boolean(device?.gatt?.connected || serialConnected)
}

function hasCommandTransport() {
	return Boolean(rxCharacteristic || serialWriter)
}

function log(message, kind = 'info') {
	const logBox = $('log')
	const empty = logBox.querySelector('.log-empty')
	if (empty) empty.remove()
	const row = document.createElement('div')
	row.className = 'log-entry'
	const time = new Date().toLocaleTimeString('pt-BR', { hour: '2-digit', minute: '2-digit', second: '2-digit' })
	const stamp = document.createElement('span')
	stamp.className = 'time'
	stamp.textContent = time
	const content = document.createElement('span')
	content.textContent = message
	if (kind === 'error') content.style.color = '#ff9299'
	if (kind === 'success') content.style.color = '#00ffb9'
	row.append(stamp, content)
	logBox.prepend(row)
	while (logBox.children.length > 40) logBox.lastElementChild.remove()
}

function toast(message) {
	const el = $('toast')
	el.textContent = message
	el.classList.add('show')
	clearTimeout(toastTimer)
	toastTimer = setTimeout(() => el.classList.remove('show'), 2600)
}

function waitForBleResponse(predicate, timeoutMs = 3000) {
	return new Promise((resolve, reject) => {
		const waiter = { predicate, resolve, reject, timer: null }
		waiter.timer = setTimeout(() => {
			const index = bleResponseWaiters.indexOf(waiter)
			if (index >= 0) bleResponseWaiters.splice(index, 1)
			reject(new Error('Tempo esgotado aguardando resposta do Arduino.'))
		}, timeoutMs)
		bleResponseWaiters.push(waiter)
	})
}

function resolveBleResponse(message) {
	for (let i = 0; i < bleResponseWaiters.length; i++) {
		const waiter = bleResponseWaiters[i]
		if (!waiter.predicate(message)) continue
		bleResponseWaiters.splice(i, 1)
		clearTimeout(waiter.timer)
		waiter.resolve(message)
		return
	}
}

async function sendCommandAndWait(command, predicate) {
	const responsePromise = waitForBleResponse(predicate)
	await sendCommand(command)
	const response = await responsePromise
	if (response.startsWith('ERRO:')) throw new Error(`Arduino: ${response}`)
	return response
}

function errorMessage(error, fallback = 'Ocorreu um erro ao conectar ao Bluetooth.') {
	if (error?.name === 'NotFoundError') return 'Nenhum dispositivo BettaFeeder foi selecionado.'
	if (error?.name === 'SecurityError') return 'O dispositivo bloqueou o acesso ao Bluetooth.'
	return error?.message || fallback
}

function setConnected(connected, name = 'BettaFeeder') {
	const badge = $('connectionBadge')
	badge.classList.toggle('online', connected)
	badge.classList.toggle('offline', !connected)
	badge.innerHTML = `<i></i> ${connected ? 'Conectado' : 'Desconectado'}`
	$('deviceName').textContent = name
	$('deviceDescription').textContent = connected
		? (connectionMode === 'serial' ? 'Conectado via USB Serial' : 'Conectado via Bluetooth Low Energy')
		: 'Nenhum dispositivo conectado';
	['saveTimesButton', 'saveWifiButton', 'statusButton', 'testFeedButton', 'homeButton', 'reloadButton'].forEach(id => $(id).disabled = !connected)
	$('pushEnable').disabled = !connected
	$('pushTest').disabled = !(connected && Boolean(currentFcmToken))
	$('connectButton').querySelector('.connect-button-label').textContent = connected ? 'Desconectar' : 'Conectar'
	$('serialConnectButton').querySelector('.serial-button-label').textContent = connectionMode === 'serial' && connected
		? 'Desconectar Serial'
		: 'Conectar via USB Serial'
	updatePushPermissionBanner()
}

function setConnectionLoading(loading) {
	const button = $('connectButton')
	button.classList.toggle('is-loading', loading)
	button.disabled = loading
	button.setAttribute('aria-busy', String(loading))
	button.querySelector('.connect-button-label').textContent = loading
		? 'Conectando...'
		: (device?.gatt?.connected ? 'Desconectar' : 'Conectar')
}

function parseDeviceMessage(message) {
	log(`Arduino: ${message}`, 'success')
	resolveBleResponse(message)
	if (!message.startsWith('STATUS;')) return
	const fields = Object.fromEntries(message.split(';').slice(1).map(part => {
		const i = part.indexOf('=')
		return i < 0 ? [part, ''] : [part.slice(0, i), part.slice(i + 1)]
	}))
	$('wifiStatus').textContent = fields.wifi || '—'
	$('clockStatus').textContent = fields.hora && fields.data ? `${fields.hora} · ${fields.data}` : (fields.hora || '—')
	$('positionStatus').textContent = fields.posicao || '—'
	$('portionsStatus').textContent = fields.porcoes_lote || '—'
	$('batchStatus').textContent = `${fields.lote || '—'} · referência: ${fields.referencia || '—'}`
	if (fields.push) $('pushState').textContent = 'Destino no Arduino: ' + fields.push
	const ready = fields.lote === 'pronto' && fields.referencia === 'confirmada'
	$('testFeedButton').disabled = !(isConnected() && ready)
	if (fields.feed1) $('feed1').value = fields.feed1
	if (fields.feed2) $('feed2').value = fields.feed2
}

function onNotification(event) {
	const value = event.target.value
	const message = decoder.decode(value)
	if (message.startsWith('+')) {
		rxFragments += message.slice(1)
		return
	}
	if (message.startsWith('=')) {
		rxFragments += message.slice(1)
		parseDeviceMessage(rxFragments)
		rxFragments = ''
		return
	}
	rxFragments = ''
	message.split(/\r?\n/).filter(Boolean).forEach(parseDeviceMessage)
}

async function connect() {
	if (connectionInProgress) return
	connectionInProgress = true
	setConnectionLoading(true)

	try {
		if (!('bluetooth' in navigator)) {
			throw new Error('Web Bluetooth não está disponível neste dispositivo.')
		}
		if (!window.isSecureContext) throw new Error('O Bluetooth exige HTTPS ou localhost.')
		log('Procurando BettaFeeder por Bluetooth...')
		device = await navigator.bluetooth.requestDevice({
			filters: [{ services: [BLE.service] }]
		})
		device.addEventListener('gattserverdisconnected', onDisconnected)
		server = await device.gatt.connect()
		const service = await server.getPrimaryService(BLE.service)
		rxCharacteristic = await service.getCharacteristic(BLE.rx)
		txCharacteristic = await service.getCharacteristic(BLE.tx)
		await txCharacteristic.startNotifications()
		txCharacteristic.addEventListener('characteristicvaluechanged', onNotification)
		setConnected(true, device.name || 'BettaFeeder')
		log('Bluetooth conectado e notificações ativadas.', 'success')
		toast('Conectado ao BettaFeeder')
		await sendCommand('STATUS')
		void syncPushAuthorization()
	} finally {
		connectionInProgress = false
		setConnectionLoading(false)
	}
}

async function connectSerial() {
	if (connectionInProgress || isConnected()) return
	connectionInProgress = true
	setConnectionLoading(true)
	try {
		if (!('serial' in navigator)) throw new Error('Web Serial não está disponível. Use Chrome ou Edge em HTTPS/localhost.')
		log('Selecione a porta USB Serial do Arduino...')
		await attachSerialPort(await navigator.serial.requestPort(), false)
	} finally {
		connectionInProgress = false
		setConnectionLoading(false)
	}
}

async function attachSerialPort(port, automatic) {
	serialPort = port
	await serialPort.open({ baudRate: 115200 })
	serialConnected = true
	connectionMode = 'serial'
	const info = serialPort.getInfo()
	localStorage.setItem('bettafeeder.serialPort', JSON.stringify({
		usbVendorId: info.usbVendorId || null,
		usbProductId: info.usbProductId || null
	}))
	setConnected(true, 'BettaFeeder')
	$('serialConnectButton').querySelector('.serial-button-label').textContent = 'Desconectar Serial'
	log(automatic
		? 'Serial reconectada automaticamente. Bluetooth será desativado pelo primeiro comando.'
		: 'Serial conectada. O primeiro comando desativará o Bluetooth no Arduino.', 'success')
	void readSerialLoop()
	await sendCommand('STATUS')
	void syncPushAuthorization()
}

async function reconnectSavedSerial() {
	if (connectionInProgress || isConnected() || !window.isSecureContext || !('serial' in navigator)) return
	let savedInfo = null
	try { savedInfo = JSON.parse(localStorage.getItem('bettafeeder.serialPort') || 'null') } catch (_) { }
	if (!savedInfo) return
	const ports = await navigator.serial.getPorts()
	if (!ports.length) return
	const matchingPort = ports.find(port => {
		const info = port.getInfo()
		return info.usbVendorId === savedInfo.usbVendorId && info.usbProductId === savedInfo.usbProductId
	})
	if (!matchingPort) return
	connectionInProgress = true
	try {
		await attachSerialPort(matchingPort, true)
	} catch (error) {
		serialConnected = false
		serialPort = null
		connectionMode = null
		console.warn('Reconexão serial automática indisponível:', error)
	} finally {
		connectionInProgress = false
	}
}

async function readSerialLoop() {
	while (serialConnected && serialPort?.readable) {
		const reader = serialPort.readable.getReader()
		serialReader = reader
		try {
			while (serialConnected) {
				const { value, done } = await reader.read()
				if (done) break
				if (!value) continue
				serialReadBuffer += decoder.decode(value, { stream: true })
				const lines = serialReadBuffer.split(/\r?\n/)
				serialReadBuffer = lines.pop() || ''
				lines.filter(Boolean).forEach(parseDeviceMessage)
			}
		} catch (error) {
			if (serialConnected) log(`Serial: ${error?.message || error}`, 'error')
		} finally {
			reader.releaseLock()
			serialReader = null
		}
	}
	if (serialConnected) onSerialDisconnected()
}

async function reconnectSavedDevice() {
	return
	const savedDevice = null
	if (!savedDevice) return

	if (connectionInProgress) return
	connectionInProgress = true
	setConnectionLoading(true)

	try {
		if (!('bluetooth' in navigator) || !window.isSecureContext) {
			throw new Error('A reconexão Bluetooth exige um dispositivo compatível e HTTPS (ou localhost).')
		}

		if (typeof navigator.bluetooth.getDevices !== 'function') {
			throw new Error('Este dispositivo não permite reconectar automaticamente. Toque em Conectar para selecionar o BettaFeeder.')
		}

		const permittedDevices = await navigator.bluetooth.getDevices()
		const rememberedDevice = permittedDevices.find(item => item.id === savedDevice.id)
		if (!rememberedDevice) {
			throw new Error('O dispositivo salvo não está disponível para reconexão. Clique em Conectar para selecioná-lo novamente.')
		}
		await connect(rememberedDevice, true)
	} catch (error) {
		const message = errorMessage(error, 'Não foi possível reconectar ao dispositivo salvo.')
		console.error(error)
		log(`Falha na reconexão automática: ${message}`, 'error')
	} finally {
		connectionInProgress = false
		setConnectionLoading(false)
	}
}

function onDisconnected() {
	rxCharacteristic = null
	txCharacteristic = null
	server = null
	connectionMode = null
	setConnected(false)
	log('Dispositivo Bluetooth desconectado.', 'error')
	toast('Conexão encerrada')
}

function onSerialDisconnected() {
	serialConnected = false
	serialWriter = null
	serialPort = null
	connectionMode = null
	$('serialConnectButton').querySelector('.serial-button-label').textContent = 'Conectar via USB Serial'
	setConnected(false)
	log('Porta USB Serial desconectada.', 'error')
}

async function disconnect() {
	if (device?.gatt?.connected) return device.gatt.disconnect()
	if (serialConnected) {
		// O firmware mantém o modo serial persistente e só reativa o BLE após
		// receber BLE_ON. Envie o comando antes de fechar a porta.
		try {
			log('Solicitando reativação do Bluetooth no Arduino...')
			await sendCommand('BLE_ON')
			await new Promise(resolve => setTimeout(resolve, 350))
		} catch (error) {
			console.warn('Não foi possível enviar BLE_ON antes de desconectar:', error)
		}
		serialConnected = false
		try { await serialReader?.cancel() } catch (_) { }
		try { serialWriter?.releaseLock() } catch (_) { }
		serialWriter = null
		try { await serialPort?.close() } catch (_) { }
		localStorage.removeItem('bettafeeder.serialPort')
		onSerialDisconnected()
		return
	}
	onDisconnected()
}

async function sendCommand(command) {
	if (rxCharacteristic) {
		// Firmware parses one command per GATT write. Write with response for reliability.
		await rxCharacteristic.writeValueWithResponse(encoder.encode(command))
	} else if (serialConnected && serialPort?.writable) {
		if (!serialWriter) serialWriter = serialPort.writable.getWriter()
		await serialWriter.write(encoder.encode(`${command}\n`))
	} else {
		throw new Error('Conecte ao BettaFeeder primeiro.')
	}
	if (!command.startsWith('FCM_ADD=')) log(`Enviado: ${command}`)
}

async function guarded(action) {
	try { await action() }
	catch (error) {
		console.error(error)
		const message = errorMessage(error)
		log(message, 'error')
		toast(message)
	}
}

$('connectButton').addEventListener('click', () => guarded(async () => {
	if (device?.gatt?.connected) return disconnect()
	await connect()
}))
$('serialConnectButton').addEventListener('click', () => guarded(async () => {
	if (serialConnected) return disconnect()
	await connectSerial()
}))

$('saveTimesButton').addEventListener('click', () => guarded(async () => {
	const first = $('feed1').value
	const second = $('feed2').value
	if (!first || !second) throw new Error('Informe os dois horários.')
	if (first === second) throw new Error('Os horários precisam ser diferentes.')
	await sendCommand(`FEED1=${first}`)
	await new Promise(resolve => setTimeout(resolve, 180))
	await sendCommand(`FEED2=${second}`)
	toast('Horários enviados ao Arduino')
}))

$('wifiForm').addEventListener('submit', event => guarded(async () => {
	event.preventDefault()
	const ssid = $('ssid').value.trim()
	const password = $('password').value
	if (!ssid) throw new Error('Informe o nome da rede Wi-Fi.')
	if (!password) throw new Error('Informe a senha do Wi-Fi.')
	// The SSID and password are sent as two distinct BLE writes.
	await sendCommand(`SSID=${ssid}`)
	await new Promise(resolve => setTimeout(resolve, 250))
	await sendCommand(`PASS=${password}`)
	$('password').value = ''
	toast('Dados do Wi-Fi enviados')
}))

$('togglePassword').addEventListener('click', () => {
	const input = $('password')
	const visible = input.type === 'password'
	input.type = visible ? 'text' : 'password'
	$('togglePassword').textContent = visible ? 'Ocultar' : 'Mostrar'
})

$('statusButton').addEventListener('click', () => guarded(() => sendCommand('STATUS')))
$('testFeedButton').addEventListener('click', () => guarded(async () => {
	if (!confirm('Acionar o motor e liberar uma porção agora?')) return
	await sendCommand('FEED')
}))
$('homeButton').addEventListener('click', () => guarded(async () => {
	if (!confirm('Confirma que a cavidade vazia está fisicamente alinhada com a saída?')) return
	await sendCommand('SET_HOME')
}))
$('reloadButton').addEventListener('click', () => guarded(async () => {
	if (!confirm('Confirma que o rotor está na posição vazia 0 e que você colocou manualmente uma porção em CADA UMA das outras 14 cavidades?')) return
	await sendCommand('RELOAD')
	await new Promise(resolve => setTimeout(resolve, 300))
	await sendCommand('STATUS')
}))
$('clearLogButton').addEventListener('click', () => {
	$('log').innerHTML = '<div class="log-empty">Os eventos de conexão e configuração aparecerão aqui.</div>'
})

if ('serviceWorker' in navigator && window.isSecureContext) {
	window.addEventListener('load', () => {
		navigator.serviceWorker.register('./sw.js').catch(err => {
			console.warn('Falha ao registrar service worker:', err)
		})
	})
}

// Web Serial só permite reconectar automaticamente portas que o usuário já
// autorizou. Não abre seletor e não tenta reconectar Bluetooth.
window.addEventListener('load', () => {
	void reconnectSavedSerial()
	if ('serial' in navigator) {
		navigator.serial.addEventListener('connect', () => { void reconnectSavedSerial() })
	}
})

// FCM Web: envia o token em blocos pequenos, compatíveis com BLE MTU 23.
let currentFcmToken = ''
let fbMessaging = null
let messageListenerRegistered = false

function firebaseWebConfigured() {
	const config = window.BETTA_FIREBASE_CONFIG
	return config && config.apiKey && config.apiKey !== 'COLOQUE_WEB_API_KEY' &&
		config.vapidKey && config.vapidKey !== 'COLOQUE_VAPID_PUBLICA' &&
		config.messagingSenderId && config.messagingSenderId !== 'COLOQUE_MESSAGING_SENDER_ID'
}

async function enablePushNotifications() {
	if (!firebaseWebConfigured()) throw new Error('Preencha firebase-config.js antes.')
	if (!('serviceWorker' in navigator) || !('Notification' in window)) {
		throw new Error('Este dispositivo não oferece Web Push.')
	}
	const permission = Notification.permission === 'granted'
		? 'granted'
		: await Notification.requestPermission()
	updatePushPermissionBanner()
	if (permission !== 'granted') {
		await clearPairedFcmToken()
		throw new Error('Permissão de notificação negada.')
	}
	if (!window.firebase?.messaging?.isSupported) throw new Error('Firebase SDK não carregado.')
	if (!await firebase.messaging.isSupported()) throw new Error('FCM não suportado neste dispositivo.')
	if (!firebase.apps.length) firebase.initializeApp(window.BETTA_FIREBASE_CONFIG)
	fbMessaging = firebase.messaging()
	const registration = await navigator.serviceWorker.register('./sw.js')
	await registration.update()
	const token = await fbMessaging.getToken({
		vapidKey: window.BETTA_FIREBASE_CONFIG.vapidKey,
		serviceWorkerRegistration: registration
	})
	if (!token) throw new Error('Firebase não forneceu token de registro.')
	currentFcmToken = token
	$('pushState').textContent = 'Push ativo neste dispositivo. O registro no Arduino ocorre automaticamente quando conectado.'
	$('pushTest').disabled = true
	if (!messageListenerRegistered) {
		messageListenerRegistered = true
		fbMessaging.onMessage(async payload => {
			const title = payload.notification?.title || payload.data?.title || 'BettaFeeder'
			const message = payload.notification?.body || payload.data?.body || 'Nova notificação'
			log(`${title}: ${message}`, 'success')
			toast(message)
			await showNativeNotification(title, message)
		})
	}
	if (isConnected()) await pairFcmToken()
}

async function showNativeNotification(title, body) {
	if (!('Notification' in window) || Notification.permission !== 'granted') return
	try {
		const registration = await navigator.serviceWorker.ready
		await registration.showNotification(title, {
			body,
			icon: './icons/icon-192.png',
			badge: './icons/icon-192.png',
			tag: 'bettafeeder-push',
			renotify: true
		})
	} catch (error) {
		console.warn('Não foi possível exibir a notificação nativa:', error)
	}
}

const pushTestMode = new URLSearchParams(window.location.search).get('modoTeste') === 'true'
$('pushTest').classList.toggle('hidden', !pushTestMode)

function updatePushControl() {
	const permissionGranted = 'Notification' in window && Notification.permission === 'granted'
	const enableButton = $('pushEnable')
	enableButton.textContent = permissionGranted ? 'Desativar notificações e remover a permissão' : 'Ativar notificações'
	enableButton.classList.toggle('button-danger', permissionGranted)
	enableButton.disabled = !isConnected()
	$('pushTest').classList.toggle('hidden', !pushTestMode)
}

async function disablePushNotifications() {
	if (fbMessaging && currentFcmToken) await fbMessaging.deleteToken()
	currentFcmToken = ''
	await clearPairedFcmToken()
	$('pushState').textContent = 'Notificações desativadas neste dispositivo. A permissão do navegador continua concedida.'
	$('pushTest').disabled = true
	updatePushControl()
}

$('pushEnable').addEventListener('click', () => guarded(async () => {
	if ('Notification' in window && Notification.permission === 'granted') await disablePushNotifications()
	else await enablePushNotifications()
	updatePushControl()
}))

function updatePushPermissionBanner() {
	const banner = $('pushPermissionBanner')
	const message = $('pushPermissionMessage')
	if (!banner || !message) return
	const permission = 'Notification' in window ? Notification.permission : 'unsupported'
	const connected = isConnected()
	banner.classList.toggle('hidden', !connected || permission === 'granted')
	message.textContent = permission === 'denied'
		? 'As notificações estão bloqueadas. Libere a permissão nas configurações do dispositivo.'
		: 'Permita o envio de notificações para receber alertas do alimentador.'
	updatePushControl()
}

updatePushPermissionBanner()
updatePushControl()
if (typeof navigator.permissions?.query === 'function') {
	navigator.permissions.query({ name: 'notifications' }).then(status => {
		status.addEventListener('change', () => { void syncPushAuthorization() })
	}).catch(() => { })
}

let fcmPairPromise = null

function pairFcmToken() {
	if (fcmPairPromise) return fcmPairPromise
	fcmPairPromise = (async () => {
		if (!currentFcmToken || !hasCommandTransport()) return
		if (currentFcmToken.length > 512) throw new Error('Token FCM acima do limite de 512 caracteres.')
		const token = currentFcmToken
		await sendCommandAndWait(
			`FCM_BEGIN=${token.length}`,
			message => message === 'OK: FCM_BEGIN' || message.startsWith('ERRO:')
		)
		for (let i = 0; i < token.length; i += 12) {
			await sendCommand(`FCM_ADD=${token.slice(i, i + 12)}`)
			await new Promise(resolve => setTimeout(resolve, 100))
		}
		await sendCommandAndWait(
			'FCM_SAVE',
			message => message === 'OK: destino FCM salvo' || message.startsWith('ERRO:')
		)
		$('pushTest').disabled = false
		$('pushState').textContent = 'Destino enviado ao Arduino. Use Notificação de teste.'
		await new Promise(resolve => setTimeout(resolve, 250))
		await sendCommand('STATUS')
	})()
	fcmPairPromise.then(
		() => { fcmPairPromise = null },
		() => { fcmPairPromise = null }
	)
	return fcmPairPromise
}

async function clearPairedFcmToken() {
	if (!isConnected() || !hasCommandTransport()) return
	await sendCommandAndWait(
		'FCM_CLEAR',
		message => message === 'OK: destino FCM removido' || message.startsWith('ERRO:')
	)
	currentFcmToken = ''
	$('pushTest').disabled = true
	$('pushState').textContent = 'Destino de notificação removido do Arduino.'
}

async function syncPushAuthorization() {
	if (!isConnected()) return
	if (!('Notification' in window) || Notification.permission !== 'granted') {
		updatePushPermissionBanner()
		try { await clearPairedFcmToken() }
		catch (error) { log(`Falha ao remover o destino FCM: ${error?.message || error}`, 'error') }
		return
	}
	try {
		await enablePushNotifications()
	} catch (error) {
		log(`Falha ao atualizar o push: ${error?.message || error}`, 'error')
	}
}


$('pushTest').addEventListener('click', () => guarded(async () => {
	if (!pushTestMode) return
	if (!isConnected() || !hasCommandTransport()) throw new Error('Conecte ao Arduino por Bluetooth ou USB Serial.')
	const button = $('pushTest')
	button.disabled = true
	try {
		// Atualiza o token e garante FCM_BEGIN/FCM_ADD/FCM_SAVE antes do teste.
		await enablePushNotifications()
		await sendCommand('FCM_TEST')
		$('pushState').textContent = 'Teste de push solicitado. Aguarde a notificação.'
		toast('Notificação de teste solicitada')
	} finally {
		button.disabled = !(isConnected() && Boolean(currentFcmToken))
	}
}))
