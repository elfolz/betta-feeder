// Service Worker PWA + FCM. Payloads de notificacao sao exibidos pelo SDK em segundo plano.
importScripts('./firebase-config.js')
try {
	importScripts('https://www.gstatic.com/firebasejs/10.14.1/firebase-app-compat.js')
	importScripts('https://www.gstatic.com/firebasejs/10.14.1/firebase-messaging-compat.js')
	if (self.BETTA_FIREBASE_CONFIG?.apiKey !== 'COLOQUE_WEB_API_KEY') {
		firebase.initializeApp(self.BETTA_FIREBASE_CONFIG)
		const messaging = firebase.messaging()
		messaging.onBackgroundMessage(payload => {
			const title = payload.notification?.title || payload.data?.title || 'BettaFeeder'
			const body = payload.notification?.body || payload.data?.body || 'Nova notificação'
			return self.registration.showNotification(title, {
				body,
				icon: './icons/icon-192.png',
				badge: './icons/icon-192.png',
				tag: 'bettafeeder-push',
				renotify: true
			})
		})
	}
} catch (error) {
	console.warn('SDK de push indisponivel:', error)
}
const CACHE_NAME = 'bettafeeder-pwa-v7'
const ASSETS = ['./', './index.html', './styles.css', './app.js', './firebase-config.js', './manifest.webmanifest', './icons/icon-192.png', './icons/icon-512.png']
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
