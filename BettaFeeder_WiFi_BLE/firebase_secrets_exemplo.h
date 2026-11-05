#pragma once
// Arquivo LOCAL. NAO publique no GitHub; NUNCA coloque na PWA.
// Dados do JSON da conta de servico Firebase:
static const char FIREBASE_PROJECT_ID[] = "SEU_PROJECT_ID_AQUI";
static const char FIREBASE_CLIENT_EMAIL[] = "SEU_CLIENT_EMAIL_AQUI";
static const char FIREBASE_PRIVATE_KEY[] = R"KEY(-----BEGIN PRIVATE KEY-----
SEU_CERTIFICADO_AQUI
)KEY";
// Certificado raiz GTS R1 do bundle CA do sistema, usado no TLS dos endpoints Google:
static const char GOOGLE_ROOT_CA[] = R"CA(-----BEGIN CERTIFICATE-----
SEU_CERTIFICADO_AQUI
-----END CERTIFICATE-----
)CA";
