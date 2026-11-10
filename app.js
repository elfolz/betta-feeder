// BettaFeeder Web Bluetooth client — native JavaScript, no framework.
const BLE = {
	service: '7f510001-1b15-4f0b-9a2e-5b8f6a100001',
	rx: '7f510002-1b15-4f0b-9a2e-5b8f6a100001',
	tx: '7f510003-1b15-4f0b-9a2e-5b8f6a100001'
}

const $ = (id) => document.getElementById(id)
const encoder = new TextEncoder()
const decoder = new TextDecoder()
const BLE_COMMAND_TIMEOUT_MS = 15000

const firmwareFieldIds = [
	'ssid', 'feed1', 'feed2', 'firebaseProjectId', 'firebaseClientEmail',
	'wifiStatus', 'clockStatus', 'portionsStatus', 'temperatureStatus',
	'batchStatus', 'pushState', 'firebaseConfigState'
]
function setFirmwareFieldsLoading(loading) {
	firmwareFieldIds.forEach(id => $(id).classList.toggle('firmware-loading', loading))
}

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
let firebaseConfigFieldsEdited = false
let commandWriteQueue = Promise.resolve()
let rxFragments = ''
let statusRefreshTimer = null
let feedReferenceReady = false
let feedTimesReceived = false
let reloadWaitingForRefill = false
let motorTestSupported = false
let motorTestActive = false
let motorTestShots = 0
let motorTestNextEpoch = 0
let receivingFeedHistory = false
let feedHistoryEpochs = []
const validFeedHistoryReasons = new Set([
	'success', 'test_mode', 'motor_busy', 'position_unknown', 'batch_empty',
	'position_mismatch', 'movement_interrupted', 'schedule_too_late', 'time_unavailable'
])
let firebaseSetupAlertShown = false
const bleResponseWaiters = []
const displayedFirmwareLogKeys = new Set()
let activityLogSequence = 0

function isConnected() {
	return Boolean(device?.gatt?.connected || serialConnected)
}

function hasCommandTransport() {
	return Boolean(rxCharacteristic || serialWriter)
}

function updateFeedButtonAvailability() {
	const button = $('testFeedButton')
	const remaining = Number($('portionsStatus').textContent)
	button.disabled = !isConnected() || motorTestActive || !feedReferenceReady || remaining <= 0 || button.classList.contains('is-loading')
}

function updateMotorTestControls() {
	const toggle = $('motorTestToggle')
	if (!toggle) return
	toggle.checked = motorTestActive
	const testPortionsAvailable = Number($('portionsStatus').textContent) >= 9
	toggle.disabled = !isConnected() || !motorTestSupported || (!motorTestActive && (!feedReferenceReady || reloadWaitingForRefill || !testPortionsAvailable))
	const scheduleDisabled = !isConnected() || !feedTimesReceived || motorTestActive
	$('feed1').disabled = scheduleDisabled
	$('feed2').disabled = scheduleDisabled
	$('saveTimesButton').disabled = scheduleDisabled
	updateFeedButtonAvailability()
	$('homeButton').disabled = !isConnected() || motorTestActive
	$('motorLeftButton').disabled = !isConnected() || motorTestActive
	$('motorRightButton').disabled = !isConnected() || motorTestActive
	$('reloadButton').disabled = motorTestActive || !(isConnected() && feedReferenceReady && reloadWaitingForRefill)
	$('motorTestBanner').hidden = !motorTestActive
	if (motorTestActive) {
		$('motorTestBanner').textContent = `MODO TESTE ATIVO · Proximo disparo ${motorTestNextEpoch ? new Date(motorTestNextEpoch * 1000).toLocaleTimeString('pt-BR', { hour: '2-digit', minute: '2-digit', timeZone: 'America/Sao_Paulo' }) : 'em calculo'} · ${motorTestShots}/9 nesta ativacao. O historico desde a recarga tambem pode incluir alimentacoes anteriores a esta ativacao. Cada disparo de teste conta como porcao.`
	}
	const nextTime = motorTestNextEpoch
		? new Date(motorTestNextEpoch * 1000).toLocaleTimeString('pt-BR', { hour: '2-digit', minute: '2-digit', timeZone: 'America/Sao_Paulo' })
		: ''
	if (!isConnected()) $('motorTestStatus').textContent = 'Conecte ao BettaFeeder para controlar o modo teste.'
	else if (!motorTestSupported) $('motorTestStatus').textContent = 'O firmware conectado nao oferece modo teste.'
	else if (motorTestActive) $('motorTestStatus').textContent = `Ativo · disparo ${Math.min(motorTestShots + 1, 9)}/9 nesta ativacao${nextTime ? ` · proximo: ${nextTime}` : ''}`
	else if (motorTestShots >= 9) $('motorTestStatus').textContent = 'Concluido. Antes de abastecer racao real, alinhe o rotor e use SET_HOME para zerar a contagem.'
	else if (motorTestShots > 0) $('motorTestStatus').textContent = 'Cancelado; disparos contam como porcoes. Antes de abastecer, alinhe o rotor e use SET_HOME.'
	else $('motorTestStatus').textContent = testPortionsAvailable && feedReferenceReady && !reloadWaitingForRefill ? 'Desligado. O primeiro disparo sera no proximo quarto de hora.' : 'Passo 1: alinhe a cavidade vazia com a saída e clique em “Definir posição inicial”. Passo 2: confirme a recarga no PWA para liberar 9 porções lógicas.'
}

function log(message, kind = 'info', occurredAt) {
	const logBox = $('log')
	const empty = logBox.querySelector('.log-empty')
	if (empty) empty.remove()
	const row = document.createElement('div')
	row.className = 'log-entry'
	if (kind === 'test') row.classList.add('motor-test-log-entry')
	const eventTimestamp = occurredAt === undefined
		? Date.now()
		: occurredAt instanceof Date ? occurredAt.getTime() : null
	row.dataset.eventTimestamp = eventTimestamp === null ? '' : String(eventTimestamp)
	row.dataset.sequence = String(++activityLogSequence)
	const time = occurredAt === undefined
		? new Date().toLocaleTimeString('pt-BR', { hour: '2-digit', minute: '2-digit', second: '2-digit' })
		: occurredAt instanceof Date
			? occurredAt.toLocaleTimeString('pt-BR', { hour: '2-digit', minute: '2-digit', second: '2-digit' })
			: '—'
	const stamp = document.createElement('span')
	stamp.className = 'time'
	stamp.textContent = time
	const content = document.createElement('span')
	content.textContent = message
	if (kind === 'error') content.style.color = '#ff9299'
	if (kind === 'success') content.style.color = '#00ffb9'
	row.append(stamp, content)
	const rows = [...logBox.querySelectorAll('.log-entry'), row].sort((a, b) => {
		const timeA = Number(a.dataset.eventTimestamp) || 0
		const timeB = Number(b.dataset.eventTimestamp) || 0
		if (timeA !== timeB) return timeB - timeA
		return Number(b.dataset.sequence) - Number(a.dataset.sequence)
	})
	rows.forEach(entry => logBox.append(entry))
	while (logBox.children.length > 100) logBox.lastElementChild.remove()
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

function alertIfFirebaseSetupIncomplete(deviceFirebaseState) {
	if (!pushActive || firebaseSetupAlertShown) return
	const missing = []
	if (!firebaseWebConfigured()) missing.push('Firebase Web do PWA')
	if (deviceFirebaseState !== 'configurado') missing.push('credenciais FCM do ESP32')
	if (missing.length === 0) return

	firebaseSetupAlertShown = true
	$('firebaseSetupMessage').textContent =
		'As notificações estão ativadas neste dispositivo, mas falta configurar: ' +
		missing.join(' e ') + '. Revise a seção de notificações e salve as credenciais necessárias.'
	$('firebaseSetupDialog').showModal()
}

$('firebaseSetupDismiss').addEventListener('click', () => $('firebaseSetupDialog').close())

function waitForBleResponse(predicate, timeoutMs = 5000, commandLabel = '') {
	return new Promise((resolve, reject) => {
		const waiter = { predicate, resolve, reject, timer: null }
		waiter.timer = setTimeout(() => {
			const index = bleResponseWaiters.indexOf(waiter)
			if (index >= 0) bleResponseWaiters.splice(index, 1)
			reject(new Error(`Tempo esgotado aguardando resposta do ESP32${commandLabel ? ` ao processar ${commandLabel}` : ''}.`))
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

async function sendCommandAndWait(command, predicate, timeoutMs = 5000) {
	const commandLabel = command.includes('=') ? command.slice(0, command.indexOf('=')) : command
	const responsePromise = waitForBleResponse(predicate, timeoutMs, commandLabel)
	await sendCommand(command)
	const response = await responsePromise
	if (response.startsWith('ERRO:')) throw new Error(`ESP32: ${response}`)
	return response
}

function errorMessage(error, fallback = 'Ocorreu um erro ao conectar ao Bluetooth.') {
	if (error?.name === 'NotFoundError') return 'Nenhum dispositivo BettaFeeder foi selecionado.'
	if (error?.name === 'SecurityError') return 'O dispositivo bloqueou o acesso ao Bluetooth.'
	if (/connection already in progress/i.test(error?.message || '')) {
		const stage = error?.connectionStage ? ` na etapa "${error.connectionStage}"` : ''
		return `O navegador informou "Connection already in progress"${stage}.`
	}
	return error?.message || fallback
}

async function connectGatt(device, retries = 2) {
	for (let attempt = 0; ; attempt++) {
		try {
			return await device.gatt.connect()
		} catch (error) {
			if (!/connection already in progress/i.test(error?.message || '') || attempt >= retries) throw error
			await new Promise(resolve => setTimeout(resolve, 700 * (attempt + 1)))
		}
	}
}

function setConnected(connected, name = 'BettaFeeder') {
	if (!connected) {
		feedTimesReceived = false
		feedReferenceReady = false
		reloadWaitingForRefill = false
		motorTestSupported = false
	}
	setFirmwareFieldsLoading(connected)
	if (connected) showFeedHistoryLoading()
	else if ($('feedingHistoryList').getAttribute('aria-busy') === 'true') {
		$('feedingHistoryList').removeAttribute('aria-busy')
		$('feedingHistory').hidden = true
	}
	const badge = $('connectionBadge')
	badge.classList.toggle('online', connected)
	badge.classList.toggle('offline', !connected)
	badge.innerHTML = `<i></i> ${connected ? 'Conectado' : 'Desconectado'}`
	$('deviceName').textContent = name
	$('deviceDescription').textContent = connected
		? (connectionMode === 'serial' ? 'Conectado via USB Serial' : 'Conectado via Bluetooth Low Energy')
		: 'Nenhum dispositivo conectado';
	$('saveTimesButton').disabled = !connected || !feedTimesReceived || motorTestActive
	$('saveWifiButton').disabled = !connected
	$('statusButton').disabled = !connected
	updateFeedButtonAvailability()
	$('homeButton').disabled = !connected
	$('motorLeftButton').disabled = !connected
	$('motorRightButton').disabled = !connected
	$('reloadButton').disabled = true // enabled only after STATUS reports aguardando_recarga
	$('feed1').disabled = !connected || !feedTimesReceived || motorTestActive
	$('feed2').disabled = !connected || !feedTimesReceived || motorTestActive
	$('ssid').disabled = !connected
	$('password').disabled = !connected
	$('togglePassword').disabled = !connected
	$('pushTest').disabled = !connected
	$('firebaseProjectId').disabled = !connected
	$('firebaseClientEmail').disabled = !connected
	$('firebasePrivateKey').disabled = !connected
	$('saveFirebaseConfigButton').disabled = !connected || $('saveFirebaseConfigButton').classList.contains('is-loading')
	const bluetoothButton = $('connectButton')
	const serialButton = $('serialConnectButton')
	const serialActive = connected && connectionMode === 'serial'
	bluetoothButton.querySelector('.connect-button-label').textContent = connected && !serialActive ? 'Desconectar' : 'Conectar via Bluetooth'
	bluetoothButton.disabled = serialActive || connectionInProgress
	serialButton.disabled = (connected && Boolean(device?.gatt?.connected)) || serialButton.classList.contains('is-loading')
	updateMotorTestControls()
	if (!serialButton.classList.contains('is-loading')) {
		serialButton.querySelector('.serial-button-label').textContent = connectionMode === 'serial' && connected
			? 'Desconectar USB Serial'
			: 'Conectar via USB Serial'
	}
	bluetoothButton.classList.toggle('button-danger', connected && connectionMode !== 'serial')
	serialButton.classList.toggle('button-danger', connected && connectionMode === 'serial')
	updatePushControl()
	clearInterval(statusRefreshTimer)
	statusRefreshTimer = connected ? setInterval(() => {
		if (!connectionInProgress && isConnected() && hasCommandTransport()) {
			sendCommand('STATUS', true).catch(error => console.warn('Falha ao atualizar status:', error))
		}
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
			? (serialConnected ? 'Desconectar USB Serial' : 'Conectar via USB Serial')
			: (device?.gatt?.connected ? 'Desconectar' : 'Conectar via Bluetooth'))
	button.classList.toggle('button-danger', !loading && (serial ? serialConnected : Boolean(device?.gatt?.connected)))
}

function showFeedHistoryLoading() {
	const panel = $('feedingHistory')
	const list = $('feedingHistoryList')
	panel.hidden = false
	list.setAttribute('aria-busy', 'true')
	list.replaceChildren()
	$('feedingHistoryEmpty').hidden = true
	for (let i = 0; i < 3; i++) {
		const placeholder = document.createElement('li')
		placeholder.className = 'feed-history-skeleton'
		placeholder.setAttribute('aria-hidden', 'true')
		list.appendChild(placeholder)
	}
}

function renderFeedHistory() {
	const list = $('feedingHistoryList')
	list.removeAttribute('aria-busy')
	list.replaceChildren()
	const reasonLabels = {
		motor_busy: 'Motor ocupado',
		position_unknown: 'Posição inicial não definida',
		batch_empty: 'Lote vazio; recarga necessária',
		position_mismatch: 'Posição do rotor inconsistente',
		movement_interrupted: 'Movimento interrompido; verifique possível travamento ou perda de energia',
		schedule_too_late: 'Horário perdido: atraso acima de 30 minutos'
	}
	// Legacy firmware recorded each reboot without NTP as a failed portion.
	// Keep those scheduler events out of the feeding history.
	const ordered = [...feedHistoryEpochs].reverse().filter(record => record.reason !== 'time_unavailable')
	$('feedingHistoryEmpty').hidden = ordered.length > 0
	if (ordered.length === 0) $('feedingHistoryEmpty').textContent = 'Nenhuma alimentação registrada neste lote.'
	ordered.forEach(record => {
		const item = document.createElement('li')
		const when = record.epoch
			? new Date(record.epoch * 1000).toLocaleString('pt-BR', { dateStyle: 'short', timeStyle: 'medium' })
			: 'Data e hora indisponíveis'
		const portion = record.portion ? `Porção ${record.portion} · ` : ''
		if (record.success) {
			item.classList.add('feed-success')
			const testModePrefix = record.reason === 'test_mode' ? 'MODO TESTE · ' : ''
			item.textContent = `${testModePrefix}${portion}Sequência de passos enviada ao motor · ${when}`
		} else {
			item.classList.add('feed-failure')
			item.textContent = `${portion}Não realizada: ${reasonLabels[record.reason] || 'motivo não informado'} · ${when}`
		}
		list.appendChild(item)
	})
	$('feedingHistory').hidden = false
}

async function requestFeedingHistory(showHistory = true) {
	if (!isConnected() || !hasCommandTransport()) return
	if (!showHistory && $('feedingHistory').hidden) return
	showFeedHistoryLoading()
	$('feedingHistoryEmpty').hidden = true
	$('feedingHistoryEmpty').textContent = 'Carregando alimentações...'
	try {
		await sendCommandAndWait('FEED_HISTORY', message => message === 'FEEDS_END', 15000)
	} catch (error) {
		const list = $('feedingHistoryList')
		list.removeAttribute('aria-busy')
		list.replaceChildren()
		$('feedingHistoryEmpty').hidden = false
		$('feedingHistoryEmpty').textContent = 'Falha ao carregar o hist\u00f3rico. Tente atualizar.'
		throw error
	}
}

function parseDeviceMessage(message) {
	// Push diagnostics are neither PWA activity events nor device history.
	if (message.startsWith('PUSH:')) return
	const motorTestState = message.match(/^MOTOR_TEST_STATE:(ON|OFF):(\d+)$/)
	if (motorTestState) {
		motorTestSupported = true
		motorTestActive = motorTestState[1] === 'ON'
		motorTestShots = Number(motorTestState[2])
		if (!motorTestActive) {
			motorTestNextEpoch = 0
			sendCommand('STATUS', true).catch(() => {})
		}
		updateMotorTestControls()
		updateFeedButtonAvailability()
		log(`MODO TESTE ${motorTestActive ? 'ativado' : 'desativado'} (${motorTestShots}/9).`, motorTestActive ? 'test' : 'info')
		return
	}
	const motorTestShot = message.match(/^MOTOR_TEST_SHOT:(\d+)\/9:(\d+)$/)
	if (motorTestShot) {
		motorTestSupported = true
		motorTestShots = Number(motorTestShot[1])
		motorTestNextEpoch = Number(motorTestShot[2])
		updateMotorTestControls()
		log(`MODO TESTE · disparo ${motorTestShots}/9 executado.`, 'test')
		return
	}
	const motorTestNext = message.match(/^MOTOR_TEST_NEXT:(\d+)$/)
	if (motorTestNext) {
		motorTestNextEpoch = Number(motorTestNext[1])
		updateMotorTestControls()
		return
	}
	if (message === 'FEEDS_END') {
		receivingFeedHistory = false
		renderFeedHistory()
	} else if (message.startsWith('FEEDS_BEGIN:')) {
		receivingFeedHistory = true
		feedHistoryEpochs = []
	} else if (message.startsWith('FEED_EVENT:') && receivingFeedHistory) {
		const [epochText, status, portionText, reason] = message.slice('FEED_EVENT:'.length).split(':')
		const epoch = Number(epochText)
		const portion = Number(portionText)
		if (!Number.isInteger(epoch) || epoch < 0 || !Number.isInteger(portion) || portion < 0 || portion > 14 || !['OK', 'FAIL'].includes(status) || !validFeedHistoryReasons.has(reason)) return
		feedHistoryEpochs.push({
			epoch,
			success: status === 'OK',
			portion,
			reason
		})
	}
	if (message.startsWith('LOG:')) {
		const match = message.match(/^LOG:(\d+):(.*)$/)
		if (match) {
			const key = `${match[1]}:${match[2]}`
			if (displayedFirmwareLogKeys.has(key)) return
			displayedFirmwareLogKeys.add(key)
			if (displayedFirmwareLogKeys.size > 256) {
				displayedFirmwareLogKeys.delete(displayedFirmwareLogKeys.values().next().value)
			}
			const epoch = Number(match[1])
			const kind = /teste do motor|teste motor/i.test(match[2]) ? 'test' : 'info'
			log(match[2], kind, epoch ? new Date(epoch * 1000) : null)
		}
		return
	}
	if (message === 'LOGS_BEGIN' || message === 'LOGS_DONE') {
		if (message === 'LOGS_BEGIN') log('Histórico salvo no ESP32:')
		else {
			log('Fim do histórico salvo no ESP32.', 'success')
			resolveBleResponse(message)
		}
		return
	}
	log(`ESP32: ${message}`, 'success')
	resolveBleResponse(message)
	if (message.startsWith('ALIMENTACAO:')) {
		const batchMatch = message.match(/porcoes_no_lote=(\d+)\/14/)
		if (batchMatch) {
			const dispensed = Number(batchMatch[1])
			$('portionsStatus').textContent = String(Math.max(0, 14 - dispensed))
			updateFeedButtonAvailability()
		}
		void requestFeedingHistory(false).catch(error => log(`Falha ao atualizar as alimentações: ${error?.message || error}`, 'error'))
		return
	}
	if (!message.startsWith('STATUS;')) return
	const fields = Object.fromEntries(message.split(';').slice(1).map(part => {
		const i = part.indexOf('=')
		return i < 0 ? [part, ''] : [part.slice(0, i), part.slice(i + 1)]
	}))
	const motorTestMatch = fields.motor_test?.match(/^(ON|OFF),(\d+),(\d+)$/)
	motorTestSupported = Boolean(motorTestMatch)
	if (motorTestMatch) {
		motorTestActive = motorTestMatch[1] === 'ON'
		motorTestShots = Number(motorTestMatch[2])
		motorTestNextEpoch = Number(motorTestMatch[3])
	}
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
	reloadWaitingForRefill = waitingForReload
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
	if (!firebaseConfigFieldsEdited && Object.hasOwn(fields, 'fb_project')) {
		$('firebaseProjectId').value = fields.fb_project
	}
	if (!firebaseConfigFieldsEdited && Object.hasOwn(fields, 'fb_email')) {
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
	$('feed1').disabled = !isConnected() || !feedTimesReceived || motorTestActive
	$('feed2').disabled = !isConnected() || !feedTimesReceived || motorTestActive
	$('saveTimesButton').disabled = !isConnected() || !feedTimesReceived || motorTestActive
	setFirmwareFieldsLoading(false)
	updateMotorTestControls()
	alertIfFirebaseSetupIncomplete(fields.firebase)
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
	let connectionStage = 'seleção do dispositivo'
	server = null
	rxCharacteristic = null
	txCharacteristic = null

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
		connectionStage = 'conexão GATT'
		server = await connectGatt(device)
		connectionStage = 'descoberta do serviço BLE'
		const service = await server.getPrimaryService(BLE.service)
		connectionStage = 'característica de escrita'
		rxCharacteristic = await service.getCharacteristic(BLE.rx)
		connectionStage = 'característica de notificações'
		txCharacteristic = await service.getCharacteristic(BLE.tx)
		connectionStage = 'ativação das notificações'
		txCharacteristic.addEventListener('characteristicvaluechanged', onNotification)
		await txCharacteristic.startNotifications()
		setConnected(true, device.name || 'BettaFeeder')
		log('Bluetooth conectado e notificações ativadas.', 'success')
		toast('Conectado ao BettaFeeder')
		connectionStage = 'envio do STATUS'
		await sendCommandAndWait('STATUS', message => message.startsWith('STATUS;'), 5000)
		await sendCommandAndWait('LOGS', message => message === 'LOGS_DONE', 15000)
		await requestFeedingHistory()
		void syncPushAuthorization()
	} catch (error) {
		const shouldDisconnect = Boolean(server?.connected)
		error.connectionStage = connectionStage
		rxCharacteristic = null
		txCharacteristic = null
		server = null
		if (shouldDisconnect) device?.gatt?.disconnect()
		setConnected(false)
		throw error
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
	$('serialConnectButton').querySelector('.serial-button-label').textContent = 'Desconectar USB Serial'
	log('Serial conectada; Bluetooth permanece ativo.', 'success')
	void readSerialLoop()
	if (verifyDevice) {
		await sendCommandAndWait('STATUS', message => message.startsWith('STATUS;'), 5000)
	} else {
		await sendCommandAndWait('STATUS', message => message.startsWith('STATUS;'), 5000)
	}
	await sendCommandAndWait('LOGS', message => message === 'LOGS_DONE', 15000)
	await requestFeedingHistory()
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
		const portToForget = serialPort
		serialConnected = false
		try { await serialReader?.cancel() } catch (_) { }
		try { serialWriter?.releaseLock() } catch (_) { }
		serialWriter = null
		try {
			if (typeof portToForget?.forget === 'function') await portToForget.forget()
		} catch (error) {
			console.warn('Não foi possível revogar a autorização da porta serial:', error)
		}
		try { await serialPort?.close() } catch (_) { }
		onSerialDisconnected()
		return
	}
	onDisconnected()
}

async function sendCommand(command, quiet = false) {
	const write = async () => {
		if (rxCharacteristic) {
			// Firmware parses one command per GATT write. Write with response for reliability.
			await rxCharacteristic.writeValueWithResponse(encoder.encode(command))
		} else if (serialConnected && serialPort?.writable) {
			if (!serialWriter) serialWriter = serialPort.writable.getWriter()
			await serialWriter.write(encoder.encode(`${command}\n`))
		} else {
			throw new Error('Conecte ao BettaFeeder primeiro.')
		}
	}
	const pendingWrite = commandWriteQueue.then(write, write)
	commandWriteQueue = pendingWrite.catch(() => { })
	await pendingWrite
	if (!quiet && !command.startsWith('FCM_ADD=') && !command.startsWith('FCM_REMOVE_ADD=') && !command.startsWith('PK_ADD=')) log(`Enviado: ${command}`)
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
	await requestFeedingHistory()
	toast('Horários enviados ao ESP32')
}))

$('motorTestToggle').addEventListener('change', () => guarded(async () => {
	const toggle = $('motorTestToggle')
	const requestedActive = toggle.checked
	toggle.disabled = true
	try {
		const response = await sendCommandAndWait(
			`TEST_MODE=${requestedActive ? 'ON' : 'OFF'}`,
			message => message.startsWith('OK: MOTOR_TEST=') || message.startsWith('ERRO:'),
			BLE_COMMAND_TIMEOUT_MS
		)
		if (response.startsWith('ERRO:')) throw new Error(response)
		const result = response.match(/^OK: MOTOR_TEST=(ON|OFF):(\d+):(\d+)$/)
		if (!result) throw new Error('Resposta inesperada do firmware ao alterar o modo teste.')
		motorTestSupported = true
		motorTestActive = result[1] === 'ON'
		motorTestShots = Number(result[2])
		motorTestNextEpoch = Number(result[3])
		updateMotorTestControls()
		updateFeedButtonAvailability()
		if (!motorTestActive) sendCommand('STATUS', true).catch(error => log(`Falha ao atualizar o estado: ${error?.message || error}`, 'error'))
		toast(motorTestActive ? 'Modo teste iniciado' : 'Modo teste interrompido')
	} catch (error) {
		motorTestActive = !requestedActive
		updateMotorTestControls()
		throw error
	}
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

$('firebasePrivateKey').addEventListener('paste', event => {
	const pastedText = event.clipboardData?.getData('text')
	if (!pastedText?.includes('\\n')) return
	event.preventDefault()
	firebaseConfigFieldsEdited = true
	const textarea = event.currentTarget
	textarea.setRangeText(
		pastedText.replace(/\\n/g, '\n'),
		textarea.selectionStart,
		textarea.selectionEnd,
		'end'
	)
})

for (const id of ['firebaseProjectId', 'firebaseClientEmail', 'firebasePrivateKey']) {
	$(id).addEventListener('input', () => { firebaseConfigFieldsEdited = true })
}

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
	const buttonLabel = $('saveFirebaseConfigButtonLabel')
	button.disabled = true
	button.classList.add('is-loading')
	button.setAttribute('aria-busy', 'true')
	buttonLabel.textContent = 'Enviando credenciais...'
	$('firebaseConfigState').textContent = 'Enviando credenciais ao ESP32...'
	try {
		await sendCommandAndWait('FB_BEGIN', message => message === 'OK: FB_BEGIN' || message.startsWith('ERRO:'), BLE_COMMAND_TIMEOUT_MS)
		const sendFragmentedField = async (begin, add, save, value) => {
			await sendCommandAndWait(`${begin}=${value.length}`, message => message === `OK: ${begin}` || message.startsWith('ERRO:'), BLE_COMMAND_TIMEOUT_MS)
			for (let i = 0; i < value.length; i += 12) {
				await sendCommandAndWait(
					`${add}=${value.slice(i, i + 12)}`,
					message => message === `OK: ${add}` || message.startsWith('ERRO:'),
					BLE_COMMAND_TIMEOUT_MS
				)
			}
			await sendCommandAndWait(save, message => message === `OK: ${save}` || message.startsWith('ERRO:'), BLE_COMMAND_TIMEOUT_MS)
		}
		await sendFragmentedField('PRJ_BEGIN', 'PRJ_ADD', 'PRJ_SAVE', projectId)
		await sendFragmentedField('EML_BEGIN', 'EML_ADD', 'EML_SAVE', clientEmail)
		if (privateKey) {
			await sendCommandAndWait(`PK_BEGIN=${privateKey.length}`, message => message === 'OK: PK_BEGIN' || message.startsWith('ERRO:'), BLE_COMMAND_TIMEOUT_MS)
			for (let i = 0; i < privateKey.length; i += 12) {
				await sendCommandAndWait(
					`PK_ADD=${privateKey.slice(i, i + 12)}`,
					message => message === 'OK: PK_ADD' || message.startsWith('ERRO:'),
					BLE_COMMAND_TIMEOUT_MS
				)
			}
			await sendCommandAndWait('PK_SAVE', message => message === 'OK: PK_SAVE' || message.startsWith('ERRO:'), BLE_COMMAND_TIMEOUT_MS)
		}
		await sendCommandAndWait('FB_SAVE', message => message === 'OK: credenciais Firebase salvas.' || message.startsWith('ERRO:'), BLE_COMMAND_TIMEOUT_MS)
		firebaseConfigFieldsEdited = false
		$('firebasePrivateKey').value = ''
		await sendCommandAndWait('STATUS', message => message.startsWith('STATUS;'), 5000)
		$('firebaseConfigState').textContent = 'Credenciais FCM salvas no ESP32.'
		toast('Credenciais Firebase enviadas')
	} finally {
		button.classList.remove('is-loading')
		button.setAttribute('aria-busy', 'false')
		buttonLabel.textContent = 'Salvar credenciais no ESP32'
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
for (const [buttonId, command] of [['motorLeftButton', 'JOG_LEFT'], ['motorRightButton', 'JOG_RIGHT']]) {
	$(buttonId).addEventListener('click', () => guarded(async () => {
		const buttons = [$('motorLeftButton'), $('motorRightButton'), $('homeButton')]
		buttons.forEach(button => { button.disabled = true })
		try {
			const response = await sendCommandAndWait(command, message => message === 'OK: ' + command || message.startsWith('ERRO:') || message.startsWith('BLOQUEADO:'), 15000)
			if (response !== 'OK: ' + command) throw new Error(`ESP32: ${response}`)
			feedReferenceReady = false
			updateFeedButtonAvailability()
		} finally {
			buttons.forEach(button => { button.disabled = !isConnected() })
		}
	}))
}
$('homeButton').addEventListener('click', () => guarded(async () => {
	if (!await confirmAction('Definir posição inicial', 'Confirma que a cavidade vazia está alinhada com a saída? Isso reinicia a contagem e o histórico do lote; será necessário reabastecer e confirmar a recarga.')) return
	const response = await sendCommandAndWait('SET_HOME', message => message.startsWith('OK:') || message.startsWith('ERRO:'), 15000)
	if (!response.startsWith('OK:')) throw new Error(`ESP32: ${response}`)
	await sendCommandAndWait('STATUS', message => message.startsWith('STATUS;'), 5000)
	await requestFeedingHistory(false)
}))
$('reloadButton').addEventListener('click', () => guarded(async () => {
	if (!await confirmAction('Confirmar recarga', 'Confirma que a recarga foi concluída? O ESP32 vai estacionar o rotor e liberar as próximas alimentações.')) return
	const button = $('reloadButton')
	button.disabled = true
	button.classList.add('is-loading')
	button.querySelector('.reload-button-label').textContent = 'Confirmando recarga...'
	try {
		await sendCommandAndWait(
			'RELOAD',
			message => message === 'OK: recarga confirmada pelo PWA; alimentador pronto.' || message.startsWith('ERRO:'),
			15000
		)
		await sendCommandAndWait('STATUS', message => message.startsWith('STATUS;'), 5000)
		await requestFeedingHistory(false)
		toast('Recarga confirmada no ESP32')
	} finally {
		button.classList.remove('is-loading')
		button.querySelector('.reload-button-label').textContent = 'Confirmar recarga'
		if (isConnected() && $('batchStatus').textContent.includes('Aguardando recarga')) button.disabled = false
	}
}))
$('clearLogButton').addEventListener('click', () => {
	$('log').innerHTML = '<div class="log-empty">Os eventos de conexão e configuração aparecerão aqui.</div>'
})

if ('serviceWorker' in navigator && window.isSecureContext) {
	window.addEventListener('load', () => {
		let refreshing = false
		navigator.serviceWorker.addEventListener('controllerchange', () => {
			if (refreshing) return
			refreshing = true
			window.location.reload()
		})

		navigator.serviceWorker.register('./sw.js').then(registration => {
			const checkForUpdates = () => registration.update().catch(err => {
				console.warn('Falha ao verificar atualizaÃ§Ãµes do PWA:', err)
			})

			void checkForUpdates()
			window.setInterval(checkForUpdates, 60 * 60 * 1000)
			document.addEventListener('visibilitychange', () => {
				if (document.visibilityState === 'visible') void checkForUpdates()
			})
		}).catch(err => {
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
const pairedFcmTokenKey = 'bettafeeder-paired-fcm-token'
const fcmRemovalPendingKey = 'bettafeeder-fcm-removal-pending'
const pushDisabledKey = 'bettafeeder-push-disabled'
let fbMessaging = null
let messageListenerRegistered = false
let pushActive = 'Notification' in window && Notification.permission === 'granted' && (() => {
	try {
		return Boolean(localStorage.getItem(pairedFcmTokenKey)) && localStorage.getItem(pushDisabledKey) !== 'true'
	} catch (_) { return false }
})()
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
		throw new Error('Permissão de notificação negada.')
	}
	try { localStorage.removeItem(pushDisabledKey) } catch (_) { }
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
	let previousToken = ''
	try { previousToken = localStorage.getItem(pairedFcmTokenKey) || '' } catch (_) { }
	if (isConnected() && previousToken && previousToken !== token) {
		await removePairedFcmDevice(previousToken)
	}
	currentFcmToken = token
	try { localStorage.setItem(pairedFcmTokenKey, token) } catch (_) { }
	updatePushControl()
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
	pushActive = true
	$('pushState').textContent = isConnected()
		? 'Push ativo neste dispositivo e registrado no ESP32.'
		: 'Push ativo neste dispositivo. O registro no ESP32 ocorre ao conectar.'
	updatePushControl()
}

async function showNativeNotification(title, body) {
	if (!('Notification' in window) || Notification.permission !== 'granted') return
	try {
		const registration = await navigator.serviceWorker.ready
		await registration.showNotification(title, {
			body,
			icon: new URL('./icons/push-icon.png', document.baseURI).href,
			badge: new URL('./icons/push-icon.png', document.baseURI).href,
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
	$('clearFcmDevicesButton').disabled = pushTransitioning || !isConnected()
	$('pushToggleLabel').textContent = enabled ? 'Ativas neste dispositivo' : 'Desativadas neste dispositivo'
	$('notificationDetails').hidden = !enabled
	$('pushTest').disabled = pushTransitioning || !isConnected() || !enabled || $('pushTest').classList.contains('is-loading')
}

async function disablePushNotifications() {
	pushActive = false
	updatePushControl()
	try { localStorage.setItem(pushDisabledKey, 'true') } catch (_) { }
	try { localStorage.setItem(fcmRemovalPendingKey, 'true') } catch (_) { }
	if (isConnected() && hasCommandTransport()) await removePairedFcmDevice()
	if (fbMessaging && currentFcmToken) await fbMessaging.deleteToken()
	currentFcmToken = ''
	$('pushState').textContent = 'Notificações desativadas neste dispositivo. A permissão do navegador continua concedida.'
	updatePushControl()
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

$('clearFcmDevicesButton').addEventListener('click', () => guarded(async () => {
	if (!await confirmAction(
		'Remover todos os dispositivos FCM',
		'Isso apagará do ESP32 todos os destinos de notificação salvos, inclusive dispositivos de teste. As credenciais Firebase serão mantidas.'
	)) return
	const button = $('clearFcmDevicesButton')
	button.classList.add('is-loading')
	button.setAttribute('aria-busy', 'true')
	button.querySelector('.clear-fcm-button-label').textContent = 'Removendo dispositivos...'
	pushTransitioning = true
	updatePushControl()
	try {
		const response = await sendCommandAndWait(
			'FCM_CLEAR',
			message => message === 'OK: dispositivos FCM removidos' || message.startsWith('ERRO:'),
			BLE_COMMAND_TIMEOUT_MS
		)
		if (response.startsWith('ERRO:')) throw new Error(response)
		try { localStorage.removeItem(pairedFcmTokenKey) } catch (_) { }
		$('pushState').textContent = 'Destinos FCM removidos do ESP32. As credenciais Firebase foram mantidas.'
		log(response, 'success')
	} finally {
		button.classList.remove('is-loading')
		button.setAttribute('aria-busy', 'false')
		button.querySelector('.clear-fcm-button-label').textContent = 'Remover todos os dispositivos FCM salvos'
		pushTransitioning = false
		updatePushControl()
	}
}))

if (typeof navigator.permissions?.query === 'function') {
	navigator.permissions.query({ name: 'notifications' }).then(status => {
		status.addEventListener('change', () => { void syncPushAuthorization() })
	}).catch(() => { })
}

let fcmPairPromise = null

async function removePairedFcmDevice(tokenOverride = '') {
	if (!isConnected() || !hasCommandTransport()) throw new Error('Conecte ao firmware para remover este dispositivo.')
	let token = tokenOverride || currentFcmToken
	if (!token) {
		try { token = localStorage.getItem(pairedFcmTokenKey) || '' } catch (_) { }
	}
	if (!token) throw new Error('Token deste dispositivo não encontrado no navegador.')
	if (token.length > 512) throw new Error('Token FCM acima do limite de 512 caracteres.')
	const beginAck = await sendCommandAndWait(
		`FCM_REMOVE_BEGIN=${token.length}`,
		message => message === 'OK: FCM_REMOVE_BEGIN' || message.startsWith('ERRO:')
	)
	if (beginAck.startsWith('ERRO:')) throw new Error(beginAck)
	for (let i = 0; i < token.length; i += 12) {
			await sendCommandAndWait(
				`FCM_REMOVE_ADD=${token.slice(i, i + 12)}`,
				message => message === 'OK: FCM_ADD' || message.startsWith('ERRO:'),
				BLE_COMMAND_TIMEOUT_MS
			)
	}
	const removeAck = await sendCommandAndWait(
		'FCM_REMOVE',
		message => message === 'OK: dispositivo FCM removido' || message.startsWith('ERRO:')
	)
	if (removeAck.startsWith('ERRO:')) throw new Error(removeAck)
	if (currentFcmToken === token) currentFcmToken = ''
	try {
		if (localStorage.getItem(pairedFcmTokenKey) === token) localStorage.removeItem(pairedFcmTokenKey)
		localStorage.removeItem(fcmRemovalPendingKey)
	} catch (_) { }
}

function pairFcmToken() {
	if (fcmPairPromise) return fcmPairPromise
	fcmPairPromise = (async () => {
		if (!currentFcmToken || !hasCommandTransport()) return
		if (currentFcmToken.length > 512) throw new Error('Token FCM acima do limite de 512 caracteres.')
		const token = currentFcmToken
		const beginAck = await sendCommandAndWait(
			`FCM_BEGIN=${token.length}`,
			message => message === 'OK: FCM_BEGIN' || message.startsWith('ERRO:'),
			BLE_COMMAND_TIMEOUT_MS
		)
		if (beginAck.startsWith('ERRO:')) throw new Error(beginAck)
		for (let i = 0; i < token.length; i += 12) {
			await sendCommandAndWait(
				`FCM_ADD=${token.slice(i, i + 12)}`,
				message => message === 'OK: FCM_ADD' || message.startsWith('ERRO:'),
				BLE_COMMAND_TIMEOUT_MS
			)
		}
		const saveAck = await sendCommandAndWait(
			'FCM_SAVE',
			message => message === 'OK: destino FCM salvo' || message.startsWith('ERRO:'),
			BLE_COMMAND_TIMEOUT_MS
		)
		if (saveAck.startsWith('ERRO:')) throw new Error(saveAck)
		updatePushControl()
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

async function syncPushAuthorization() {
	if (!isConnected() || pushTransitioning) return
	let removalPending = false
	let hasPairedToken = false
	let userDisabled = false
	try {
		removalPending = localStorage.getItem(fcmRemovalPendingKey) === 'true'
		hasPairedToken = Boolean(localStorage.getItem(pairedFcmTokenKey))
		userDisabled = localStorage.getItem(pushDisabledKey) === 'true'
	} catch (_) { }
	if (!('Notification' in window) || Notification.permission !== 'granted') {
		pushActive = false
		updatePushControl()
		if (hasPairedToken && !removalPending) {
			try { localStorage.setItem(fcmRemovalPendingKey, 'true') } catch (_) { }
			removalPending = true
		}
		if (removalPending) {
			pushTransitioning = true
			updatePushControl()
			try { await removePairedFcmDevice() }
			catch (error) { log(`Falha ao remover este dispositivo do firmware: ${error?.message || error}`, 'error') }
			finally { pushTransitioning = false; updatePushControl() }
		}
		return
	}
	if (removalPending) {
		pushTransitioning = true
		updatePushControl()
		try { await removePairedFcmDevice() }
		catch (error) {
			log(`Falha ao remover este dispositivo do firmware: ${error?.message || error}`, 'error')
			pushTransitioning = false
			updatePushControl()
			return
		}
		pushTransitioning = false
		updatePushControl()
	}
	if (userDisabled) {
		pushActive = false
		updatePushControl()
		return
	}
	pushTransitioning = true
	updatePushControl()
	try {
		await enablePushNotifications()
	} catch (error) {
		log(`Falha ao atualizar o push: ${error?.message || error}`, 'error')
	} finally {
		pushTransitioning = false
		updatePushControl()
	}
}


$('pushTest').addEventListener('click', () => guarded(async () => {
	if (!isConnected() || !hasCommandTransport()) throw new Error('Conecte ao ESP32 por Bluetooth ou USB Serial.')
	const button = $('pushTest')
	button.disabled = true
	button.classList.add('is-loading')
	button.setAttribute('aria-busy', 'true')
	button.querySelector('.push-test-button-label').textContent = 'Enviando notificação...'
	try {
		// Atualiza o token e garante FCM_BEGIN/FCM_ADD/FCM_SAVE antes do teste.
		await enablePushNotifications()
		await sendCommand('FCM_TEST')
		$('pushState').textContent = 'Teste de push solicitado. Aguarde a notificação.'
		toast('Notificação de teste solicitada')
	} finally {
		button.classList.remove('is-loading')
		button.setAttribute('aria-busy', 'false')
		button.querySelector('.push-test-button-label').textContent = 'Enviar notificação de teste'
		updatePushControl()
	}
}))
