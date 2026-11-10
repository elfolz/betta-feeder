// Service Worker PWA + Firebase Cloud Messaging.
// O clique deve ser registrado antes do SDK para preservar o comportamento.
self.addEventListener('notificationclick', event => {
	event.notification.close()
	event.waitUntil(self.clients.matchAll({ type: 'window', includeUncontrolled: true }).then(clientList => {
		for (const client of clientList) {
			if ('focus' in client) return client.focus()
		}
		if (self.clients.openWindow) return self.clients.openWindow('./')
		return undefined
	}))
})

importScripts('./firebase-config.js')
try {
	importScripts('https://www.gstatic.com/firebasejs/10.14.1/firebase-app-compat.js')
	importScripts('https://www.gstatic.com/firebasejs/10.14.1/firebase-messaging-compat.js')
	if (self.BETTA_FIREBASE_CONFIG?.apiKey !== 'COLOQUE_WEB_API_KEY') {
		firebase.initializeApp(self.BETTA_FIREBASE_CONFIG)
		const messaging = firebase.messaging()
		messaging.onBackgroundMessage(payload => {
			const source = payload.notification || payload.data || {}
			return self.registration.showNotification(source.title || 'BettaFeeder', {
				body: source.body || 'Nova notificação',
				icon: new URL('./icons/push-icon.png', self.registration.scope).href,
				badge: new URL('./icons/push-icon.png', self.registration.scope).href,
				tag: 'bettafeeder-push',
				renotify: true
			})
		})
	}
} catch (error) {
	console.warn('SDK de push indisponível:', error)
}

const CACHE_NAME = 'bettafeeder-pwa-v89'
const ASSETS = ['./', './index.html', './sobre.html', './styles.css', './landing-page.css', './app.js?v=69', './firebase-config.js', './manifest.webmanifest', './icons/icon-192.png', './icons/icon-512.png', './icons/push-icon.png', './images/models/rotor.png', './images/models/base.png', './images/models/tampa.png', './images/models/presilha.png', './images/models/parafuso-presilha.png', './images/models/caixa-full-size.png', './images/models/tampa-caixa.png', './images/models/funil.png', './images/og/controle.png', './images/og/projeto.png']
self.addEventListener('install', event => {
	event.waitUntil(caches.open(CACHE_NAME).then(cache => cache.addAll(ASSETS)))
	self.skipWaiting()
})
self.addEventListener('activate', event => {
	event.waitUntil(caches.keys().then(keys => Promise.all(keys.filter(key => key !== CACHE_NAME).map(key => caches.delete(key)))))
	self.clients.claim()
})
self.addEventListener('fetch', event => {
	if (event.request.method !== 'GET') return
	if (new URL(event.request.url).origin !== self.location.origin) return
	event.respondWith(fetch(event.request).then(response => {
		const copy = response.clone()
		caches.open(CACHE_NAME).then(cache => cache.put(event.request, copy))
		return response
	}).catch(() => caches.match(event.request).then(cached => cached || caches.match('./index.html'))))
})
