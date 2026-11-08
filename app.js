// BettaFeeder Web Bluetooth client — native JavaScript, no framework.
const BLE = {
	service: '7f510001-1b15-4f0b-9a2e-5b8f6a100001',
	rx: '7f510002-1b15-4f0b-9a2e-5b8f6a100001',
	tx: '7f510003-1b15-4f0b-9a2e-5b8f6a100001'
}

const $ = (id) => document.getElementById(id)
const encoder = new TextEncoder()
const decoder = new TextDecoder()
try { localStorage.clear() } catch (_) { }

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
let statusRefreshTimer = null
let feedReferenceReady = false
let feedTimesReceived = false
const bleResponseWaiters = []

function isConnected() {
	return Boolean(device?.gatt?.connected || serialConnected)
}

function hasCommandTransport() {
	return Boolean(rxCharacteristic || serialWriter)
}

function updateFeedButtonAvailability() {
	const button = $('testFeedButton')
	const remaining = Number($('portionsStatus').textContent)
	button.disabled = !isConnected() || !feedReferenceReady || remaining <= 0 || button.classList.contains('is-loading')
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

function confirmAction(title, message) {
	const dialog = $('confirmDialog')
	$('confirmTitle').textContent = title
	$('confirmMessage').textContent = message
	dialog.showModal()
	return new Promise(resolve => {
		dialog.addEventListener('close', () => resolve(dialog.returnValue === 'confirm'), { once: true })
	})
}

function waitForBleResponse(predicate, timeoutMs = 3000) {
	return new Promise((resolve, reject) => {
		const waiter = { predicate, resolve, reject, timer: null }
		waiter.timer = setTimeout(() => {
			const index = bleResponseWaiters.indexOf(waiter)
			if (index >= 0) bleResponseWaiters.splice(index, 1)
			reject(new Error('Tempo esgotado aguardando resposta do ESP32.'))
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

async function sendCommandAndWait(command, predicate, timeoutMs = 3000) {
	const responsePromise = waitForBleResponse(predicate, timeoutMs)
	await sendCommand(command)
	const response = await responsePromise
	if (response.startsWith('ERRO:')) throw new Error(`ESP32: ${response}`)
	return response
}

function errorMessage(error, fallback = 'Ocorreu um erro ao conectar ao Bluetooth.') {
	if (error?.name === 'NotFoundError') return 'Nenhum dispositivo BettaFeeder foi selecionado.'
	if (error?.name === 'SecurityError') return 'O dispositivo bloqueou o acesso ao Bluetooth.'
	return error?.message || fallback
}

function setConnected(connected, name = 'BettaFeeder') {
	if (!connected) feedTimesReceived = false
	const badge = $('connectionBadge')
	badge.classList.toggle('online', connected)
	badge.classList.toggle('offline', !connected)
	badge.innerHTML = `<i></i> ${connected ? 'Conectado' : 'Desconectado'}`
	$('deviceName').textContent = name
	$('deviceDescription').textContent = connected
		? (connectionMode === 'serial' ? 'Conectado via USB Serial' : 'Conectado via Bluetooth Low Energy')
		: 'Nenhum dispositivo conectado';
	$('saveTimesButton').disabled = !connected || !feedTimesReceived
	$('saveWifiButton').disabled = !connected
	$('statusButton').disabled = !connected
	updateFeedButtonAvailability()
	$('homeButton').disabled = !connected
	$('reloadButton').disabled = true // enabled only after STATUS reports aguardando_recarga
	$('feed1').disabled = !connected || !feedTimesReceived
	$('feed2').disabled = !connected || !feedTimesReceived
	$('ssid').disabled = !connected
	$('password').disabled = !connected
	$('togglePassword').disabled = !connected
	$('pushTest').disabled = !connected
	$('firebaseProjectId').disabled = !connected
	$('firebaseClientEmail').disabled = !connected
	$('firebasePrivateKey').disabled = !connected
	$('saveFirebaseConfigButton').disabled = !connected
	const bluetoothButton = $('connectButton')
	const serialButton = $('serialConnectButton')
	const serialActive = connected && connectionMode === 'serial'
	bluetoothButton.querySelector('.connect-button-label').textContent = connected && !serialActive ? 'Desconectar' : 'Conectar via Bluetooth'
	bluetoothButton.disabled = serialActive || connectionInProgress
	serialButton.disabled = (connected && Boolean(device?.gatt?.connected)) || serialButton.classList.contains('is-loading')
	if (!serialButton.classList.contains('is-loading')) {
		serialButton.querySelector('.serial-button-label').textContent = connectionMode === 'serial' && connected
			? 'Desconectar Serial'
			: 'Conectar via USB Serial'
	}
	bluetoothButton.classList.toggle('button-danger', connected && connectionMode !== 'serial')
	serialButton.classList.toggle('button-danger', connected && connectionMode === 'serial')
	updatePushControl()
	clearInterval(statusRefreshTimer)
	statusRefreshTimer = connected ? setInterval(() => {
		if (isConnected() && hasCommandTransport()) sendCommand('STATUS', true).catch(error => console.warn('Falha ao atualizar status:', error))
	}, 10000) : null
}

function setConnectionLoading(loading, mode = 'bluetooth') {
	const serial = mode === 'serial'
	const button = $(serial ? 'serialConnectButton' : 'connectButton')
	button.classList.toggle('is-loading', loading)
	button.disabled = loading || (serial
		? Boolean(device?.gatt?.connected)
		: (serialConnected && connectionMode === 'serial'))
	button.setAttribute('aria-busy', String(loading))
	button.querySelector(serial ? '.serial-button-label' : '.connect-button-label').textContent = loading
		? 'Conectando...'
		: (serial
			? (serialConnected ? 'Desconectar Serial' : 'Conectar via USB Serial')
			: (device?.gatt?.connected ? 'Desconectar' : 'Conectar via Bluetooth'))
	button.classList.toggle('button-danger', !loading && (serial ? serialConnected : Boolean(device?.gatt?.connected)))
}

function parseDeviceMessage(message) {
	log(`ESP32: ${message}`, 'success')
	resolveBleResponse(message)
	if (message.startsWith('ALIMENTACAO:')) {
		const batchMatch = message.match(/porcoes_no_lote=(\d+)\/14/)
		if (batchMatch) {
			const dispensed = Number(batchMatch[1])
			$('portionsStatus').textContent = String(Math.max(0, 14 - dispensed))
			updateFeedButtonAvailability()
		}
		return
	}
	if (!message.startsWith('STATUS;')) return
	const fields = Object.fromEntries(message.split(';').slice(1).map(part => {
		const i = part.indexOf('=')
		return i < 0 ? [part, ''] : [part.slice(0, i), part.slice(i + 1)]
	}))
	const wifiLabels = { conectado: 'Conectado', desconectado: 'Desconectado' }
	$('wifiStatus').textContent = wifiLabels[fields.wifi] || '—'
	const dateMatch = fields.data?.match(/^(\d{4})-(\d{2})-(\d{2})$/)
	const arduinoDate = dateMatch
		? `${dateMatch[3]}/${dateMatch[2]}/${dateMatch[1].slice(-2)}`
		: fields.data
	const timeMatch = fields.hora?.match(/^(\d{2}:\d{2})(?::(\d{1,2}))?$/)
	const arduinoTime = timeMatch
		? `${timeMatch[1]}:${(timeMatch[2] || '00').padStart(2, '0')}`
		: (fields.hora === 'nao_sincronizada' ? 'Não sincronizado' : fields.hora)
	$('clockStatus').textContent = arduinoTime && arduinoDate
		? `${arduinoTime} ${arduinoDate}`
		: (arduinoTime || '—')
	const batchMatch = fields.porcoes_lote?.match(/^(\d+)\/14$/)
	const dispensed = batchMatch ? Number(batchMatch[1]) : null
	const waitingForReload = fields.lote === 'aguardando_recarga'
	$('portionsStatus').textContent = fields.porcoes_restantes ||
		(dispensed !== null ? String(waitingForReload ? 0 : Math.max(0, 14 - dispensed)) : '—')
	const temperature = fields.temperatura
	$('temperatureStatus').textContent = temperature && temperature !== 'indisponivel'
		? `${temperature.replace('.', ',')} °C`
		: 'Indisponível'
	const measuredTemperature = temperature !== undefined && temperature !== 'indisponivel' ? Number(temperature) : null
	$('temperatureStatus').classList.toggle('temperature-low', measuredTemperature !== null &&
		(measuredTemperature < 24 || measuredTemperature > 28))
	const referenceLabels = { confirmada: 'Confirmada', verificar: 'Precisa ser definida' }
	const batchLabels = { pronto: 'Pronto', aguardando_recarga: 'Aguardando recarga' }
	$('batchStatus').textContent = `Referência: ${referenceLabels[fields.referencia] || '—'} · Lote: ${batchLabels[fields.lote] || '—'}`
	if (fields.push) {
		const pushLabels = { configurado: 'Configurado', sem_destino: 'Sem destino configurado' }
		$('pushState').textContent = 'Destino no ESP32: ' + (pushLabels[fields.push] || 'Estado desconhecido')
	}
	if (fields.firebase) {
		$('firebaseConfigState').textContent = fields.firebase === 'configurado'
			? 'Credenciais FCM configuradas no ESP32.'
			: 'Credenciais FCM ainda não configuradas no ESP32.'
	}
	if (Object.hasOwn(fields, 'fb_project')) {
		$('firebaseProjectId').value = fields.fb_project
	}
	if (Object.hasOwn(fields, 'fb_email')) {
		$('firebaseClientEmail').value = fields.fb_email
	}
	if (Object.hasOwn(fields, 'ssid')) $('ssid').value = fields.ssid
	const ready = fields.referencia === 'confirmada'
	feedReferenceReady = ready
	updateFeedButtonAvailability()
	$('reloadButton').disabled = !(isConnected() && ready && waitingForReload)
	$('feed1').value = fields.feed1 || ''
	$('feed2').value = fields.feed2 || ''
	feedTimesReceived = Boolean(fields.feed1 && fields.feed2)
	$('feed1').disabled = !isConnected() || !feedTimesReceived
	$('feed2').disabled = !isConnected() || !feedTimesReceived
	$('saveTimesButton').disabled = !isConnected() || !feedTimesReceived
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
	setConnectionLoading(true, 'serial')
	try {
		if (!('serial' in navigator)) throw new Error('Web Serial não está disponível. Use Chrome ou Edge em HTTPS/localhost.')
		log('Selecione a porta USB Serial do ESP32...')
		await attachSerialPort(await navigator.serial.requestPort())
	} finally {
		connectionInProgress = false
		setConnectionLoading(false, 'serial')
	}
}

async function attachSerialPort(port, verifyDevice = false) {
	serialPort = port
	await serialPort.open({ baudRate: 115200 })
	serialConnected = true
	connectionMode = 'serial'
	setConnected(true, 'BettaFeeder')
	$('serialConnectButton').querySelector('.serial-button-label').textContent = 'Desconectar Serial'
	log('Serial conectada; Bluetooth será desativado pelo primeiro comando.', 'success')
	void readSerialLoop()
	if (verifyDevice) {
		await sendCommandAndWait('STATUS', message => message.startsWith('STATUS;'), 5000)
	} else {
		await sendCommand('STATUS')
	}
	void syncPushAuthorization()
}

async function reconnectAuthorizedSerial() {
	if (connectionInProgress || isConnected() || !window.isSecureContext || !('serial' in navigator)) return
	let ports
	try { ports = await navigator.serial.getPorts() } catch (_) { return }
	if (!ports.length || connectionInProgress || isConnected()) return

	connectionInProgress = true
	setConnectionLoading(true, 'serial')
	try {
		for (const port of ports) {
			try {
				await attachSerialPort(port, true)
				log('Porta USB Serial autorizada reconectada automaticamente.', 'success')
				return
			} catch (error) {
				serialConnected = false
				try { await serialReader?.cancel() } catch (_) { }
				try { serialWriter?.releaseLock() } catch (_) { }
				serialReader = null
				serialWriter = null
				try { await port.close() } catch (_) { }
				serialPort = null
				connectionMode = null
				setConnected(false)
				console.warn('Porta serial autorizada não respondeu como BettaFeeder:', error)
			}
		}
	} finally {
		connectionInProgress = false
		setConnectionLoading(false, 'serial')
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
		// BLE_ON clears the firmware's persistent serial mode and restarts BLE.
		try {
			log('Solicitando reativação do Bluetooth no ESP32...')
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
		onSerialDisconnected()
		return
	}
	onDisconnected()
}

async function sendCommand(command, quiet = false) {
	if (rxCharacteristic) {
		// Firmware parses one command per GATT write. Write with response for reliability.
		await rxCharacteristic.writeValueWithResponse(encoder.encode(command))
	} else if (serialConnected && serialPort?.writable) {
		if (!serialWriter) serialWriter = serialPort.writable.getWriter()
		await serialWriter.write(encoder.encode(`${command}\n`))
	} else {
		throw new Error('Conecte ao BettaFeeder primeiro.')
	}
	if (!quiet && !command.startsWith('FCM_ADD=') && !command.startsWith('PK_ADD=')) log(`Enviado: ${command}`)
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
	const validTime = value => /^(?:[01]\d|2[0-3]):[0-5]\d$/.test(value)
	if (!validTime(first) || !validTime(second)) throw new Error('Informe horários válidos no formato HH:MM.')
	if (first === second) throw new Error('Os horários precisam ser diferentes.')
	await sendCommandAndWait(
		`TIMES=${first},${second}`,
		message => message.startsWith('OK: horarios salvos=') || message.startsWith('ERRO:')
	)
	toast('Horários enviados ao ESP32')
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

$('firebaseConfigForm').addEventListener('submit', event => guarded(async () => {
	event.preventDefault()
	const projectId = $('firebaseProjectId').value.trim()
	const clientEmail = $('firebaseClientEmail').value.trim()
	const privateKeyInput = $('firebasePrivateKey').value.trim()
	if (!/^[a-z][a-z0-9-]{4,28}[a-z0-9]$/.test(projectId)) {
		throw new Error('Informe um ID de projeto Firebase válido.')
	}
	if (!clientEmail.endsWith('.iam.gserviceaccount.com')) {
		throw new Error('Informe o e-mail de uma conta de serviço Firebase.')
	}
	let privateKey = ''
	if (privateKeyInput) {
		privateKey = privateKeyInput
			.replace(/^['"]|['"]$/g, '')
			.replace(/\r\n?/g, '\n')
			.replace(/\\n/g, '\n')
			.trim()
			.replace(/\n/g, '\\n')
		if (!privateKey.includes('-----BEGIN PRIVATE KEY-----') || !privateKey.includes('-----END PRIVATE KEY-----')) {
			throw new Error('A chave deve estar no formato PEM da conta de serviço.')
		}
		if (privateKey.length < 300 || privateKey.length > 1900) {
			throw new Error('O tamanho da chave privada PEM é inválido.')
		}
	}
	const button = $('saveFirebaseConfigButton')
	button.disabled = true
	button.textContent = 'Enviando credenciais...'
	$('firebaseConfigState').textContent = 'Enviando credenciais ao ESP32...'
	try {
		await sendCommandAndWait('FB_BEGIN', message => message === 'OK: FB_BEGIN' || message.startsWith('ERRO:'))
		const sendFragmentedField = async (begin, add, save, value) => {
			await sendCommandAndWait(`${begin}=${value.length}`, message => message === `OK: ${begin}` || message.startsWith('ERRO:'))
			for (let i = 0; i < value.length; i += 12) {
				await sendCommand(`${add}=${value.slice(i, i + 12)}`, true)
				await new Promise(resolve => setTimeout(resolve, 100))
			}
			await sendCommandAndWait(save, message => message === `OK: ${save}` || message.startsWith('ERRO:'))
		}
		await sendFragmentedField('PRJ_BEGIN', 'PRJ_ADD', 'PRJ_SAVE', projectId)
		await sendFragmentedField('EML_BEGIN', 'EML_ADD', 'EML_SAVE', clientEmail)
		if (privateKey) {
			await sendCommandAndWait(`PK_BEGIN=${privateKey.length}`, message => message === 'OK: PK_BEGIN' || message.startsWith('ERRO:'))
			for (let i = 0; i < privateKey.length; i += 12) {
				await sendCommand(`PK_ADD=${privateKey.slice(i, i + 12)}`, true)
				await new Promise(resolve => setTimeout(resolve, 100))
			}
			await sendCommandAndWait('PK_SAVE', message => message === 'OK: PK_SAVE' || message.startsWith('ERRO:'))
		}
		await sendCommandAndWait('FB_SAVE', message => message === 'OK: credenciais Firebase salvas.' || message.startsWith('ERRO:'))
		$('firebasePrivateKey').value = ''
		await sendCommand('STATUS', true)
		$('firebaseConfigState').textContent = 'Credenciais FCM salvas no ESP32.'
		toast('Credenciais Firebase enviadas')
	} finally {
		button.textContent = 'Salvar credenciais no ESP32'
		button.disabled = !isConnected()
	}
}))

$('togglePassword').addEventListener('click', () => {
	const input = $('password')
	const visible = input.type === 'password'
	input.type = visible ? 'text' : 'password'
	$('togglePassword').textContent = visible ? 'Ocultar' : 'Mostrar'
})

$('statusButton').addEventListener('click', () => guarded(() => sendCommand('STATUS')))
$('testFeedButton').addEventListener('click', () => guarded(async () => {
	if (!await confirmAction('Confirmar alimentação', 'Alimentar agora? O motor vai liberar uma porção imediatamente.')) return
	const button = $('testFeedButton')
	button.classList.add('is-loading')
	button.disabled = true
	button.setAttribute('aria-busy', 'true')
	button.querySelector('.feed-button-label').textContent = 'Alimentando...'
	try {
		const response = await sendCommandAndWait(
			'FEED',
			message => message.startsWith('ALIMENTACAO:') || message.startsWith('BLOQUEADO:') ||
				message.startsWith('FALHA:') || message.startsWith('LOTE_VAZIO:'),
			60000
		)
		if (!response.startsWith('ALIMENTACAO:')) throw new Error(`ESP32: ${response}`)
	} finally {
		button.classList.remove('is-loading')
		button.removeAttribute('aria-busy')
		button.querySelector('.feed-button-label').textContent = 'Alimentar agora'
		updateFeedButtonAvailability()
	}
}))
$('homeButton').addEventListener('click', () => guarded(async () => {
	if (!await confirmAction('Definir posição inicial', 'Confirma que a cavidade vazia está fisicamente alinhada com a saída?')) return
	await sendCommand('SET_HOME')
}))
$('reloadButton').addEventListener('click', () => guarded(async () => {
	if (!await confirmAction('Confirmar recarga', 'Confirma que a recarga foi concluída? O ESP32 vai estacionar o rotor e liberar as próximas alimentações.')) return
	await sendCommand('RELOAD')
	await sendCommand('STATUS', true)
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

window.addEventListener('load', () => {
	if ('serial' in navigator) {
		void reconnectAuthorizedSerial()
		navigator.serial.addEventListener('connect', () => { void reconnectAuthorizedSerial() })
		navigator.serial.addEventListener('disconnect', event => {
			if (event.target === serialPort) onSerialDisconnected()
		})
	}
})

// FCM Web: envia o token em blocos pequenos, compatíveis com BLE MTU 23.
let currentFcmToken = ''
let fbMessaging = null
let messageListenerRegistered = false
let pushActive = 'Notification' in window && Notification.permission === 'granted'
let pushTransitioning = false

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
	updatePushControl()
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
	pushActive = true
	$('pushState').textContent = 'Push ativo neste dispositivo. O registro no ESP32 ocorre automaticamente quando conectado.'
	$('pushTest').disabled = !isConnected()
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
	updatePushControl()
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

function updatePushControl() {
	const toggle = $('pushToggle')
	const enabled = pushTransitioning ? toggle.checked : pushActive
	if (!pushTransitioning) toggle.checked = pushActive
	toggle.disabled = pushTransitioning || !isConnected()
	$('pushToggleLabel').textContent = enabled ? 'Ativas neste dispositivo' : 'Desativadas neste dispositivo'
	$('notificationDetails').hidden = enabled
	$('pushTest').disabled = !isConnected() || enabled
}

async function disablePushNotifications() {
	pushActive = false
	updatePushControl()
	if (fbMessaging && currentFcmToken) await fbMessaging.deleteToken()
	currentFcmToken = ''
	await clearPairedFcmToken()
	$('pushState').textContent = 'Notificações desativadas neste dispositivo. A permissão do navegador continua concedida.'
	$('pushTest').disabled = !isConnected()
	updatePushControl()
}

$('pushToggle').addEventListener('change', async event => {
	const requestedEnabled = event.currentTarget.checked
	if (!requestedEnabled && pushActive && !await confirmAction('Desativar notificações', 'Deseja desativar as notificações push neste dispositivo? A permissão do navegador continuará concedida.')) {
		updatePushControl()
		return
	}
	pushTransitioning = true
	updatePushControl()
	try {
		await guarded(async () => {
			if (!requestedEnabled) await disablePushNotifications()
			else await enablePushNotifications()
		})
	} finally {
		pushTransitioning = false
		updatePushControl()
	}
})

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
		$('pushState').textContent = 'Destino enviado ao ESP32. Use Notificação de teste.'
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
	$('pushTest').disabled = !isConnected()
	$('pushState').textContent = 'Destino de notificação removido do ESP32.'
}

async function syncPushAuthorization() {
	if (!isConnected()) return
	if (!('Notification' in window) || Notification.permission !== 'granted') {
		pushActive = false
		updatePushControl()
		try { await clearPairedFcmToken() }
		catch (error) { log(`Falha ao remover o destino FCM: ${error?.message || error}`, 'error') }
		return
	}
	if (!pushActive) {
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
	if (!isConnected() || !hasCommandTransport()) throw new Error('Conecte ao ESP32 por Bluetooth ou USB Serial.')
	const button = $('pushTest')
	button.disabled = true
	try {
		// Atualiza o token e garante FCM_BEGIN/FCM_ADD/FCM_SAVE antes do teste.
		await enablePushNotifications()
		await sendCommand('FCM_TEST')
		$('pushState').textContent = 'Teste de push solicitado. Aguarde a notificação.'
		toast('Notificação de teste solicitada')
	} finally {
		button.disabled = !isConnected()
	}
}))
