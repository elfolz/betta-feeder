# BettaFeeder

Alimentador automático de ração seca para aquários, com dosagem por rotor impresso em 3D. O ESP32 executa os horários programados de forma independente do painel; um PWA (aplicativo web instalável) permite configurar e acompanhar o equipamento pelo celular.

## Recursos

- Até dois horários de alimentação por dia, sincronizados por Wi-Fi/NTP.
- Acionamento manual, ajuste da posição inicial e confirmação de recarga pelo painel.
- Rotor com 15 cavidades: 14 porções por carga e uma cavidade vazia de segurança alinhada à saída.
- Conexão local por Bluetooth Low Energy (BLE) ou USB Serial.
- Monitoramento opcional da temperatura da água com DS18B20 e alertas abaixo de 24 °C ou acima de 28 °C.
- Notificações push via Firebase Cloud Messaging (FCM), quando configuradas, para recarga, temperatura e eventos.
- Histórico local de eventos e estado persistente após reinicialização.
- Modelos STL da estrutura, mecanismo, fixação, funil e duas versões de caixa eletrônica.

> O motor não tem sensor de posição. A posição é estimada pelo firmware; após uma interrupção de energia durante o movimento, confira o rotor e defina novamente a posição inicial. O histórico registra o acionamento do motor, não confirma a queda física da ração.

## Arquivos do projeto

- `index.html`, `app.js`, CSS e `sw.js`: painel PWA.
- `BettaFeeder_WiFi_BLE/BettaFeeder_WiFi_BLE.ino`: firmware do ESP32.
- `models/`: arquivos STL para impressão.

## Eletrônica e materiais

Necessário para o conjunto principal:

- Placa ESP32-WROOM-DA (ou placa ESP32 compatível com o firmware).
- Motor de passo 28BYJ-48 de 5 V e placa driver ULN2003 compatível.
- Fonte regulada de 5 V dimensionada para o motor e a placa; evite alimentar o motor pelo pino 3V3. Una o GND da fonte ao GND do ESP32.
- Cabos/jumpers e, conforme a montagem, parafusos M3 para fixação.

Opcional:

- Sensor de temperatura DS18B20 à prova d'água.
- Resistor de 4,7 kΩ entre DATA do DS18B20 e 3,3 V.
- Acesso a Wi-Fi 2,4 GHz para sincronização do relógio e serviços de rede.

### Ligações do firmware

| Componente | ESP32 |
| --- | --- |
| ULN2003 IN1 | GPIO 16 |
| ULN2003 IN2 | GPIO 17 |
| ULN2003 IN3 | GPIO 18 |
| ULN2003 IN4 | GPIO 19 |
| DS18B20 DATA (opcional) | GPIO 4 |
| DS18B20 VDD | 3,3 V |
| DS18B20 GND | GND |

O motor conecta-se ao conector do driver ULN2003, e a alimentação de 5 V do motor vai ao driver. Confira a pinagem da sua placa antes de energizar. Não é necessário módulo RTC: o horário vem de NTP quando há Wi-Fi.

## Impressão 3D

Os STL estão em milímetros. Abra cada arquivo no fatiador, confira a escala e a orientação da pré-visualização e use suportes somente onde a geometria exigir. Como ponto de partida geral: bico de 0,4 mm, camada de 0,2 mm, 3 paredes e 20–30% de preenchimento. PLA pode servir para protótipos; para uso perto do aquário, PETG costuma tolerar melhor umidade e impacto. Mantenha peças impressas e eletrônica protegidas de respingos. Ajuste tolerâncias e suportes à sua impressora; as recomendações abaixo não substituem a inspeção do fatiador.

| Arquivo | Função e orientação sugerida | Infill sugerido | Observações |
| --- | --- | --- | --- |
| `01_base.stl` | Base estrutural; coloque a face inferior plana sobre a mesa. | 25–30% | Use brim se houver pouca área de contato. Confira se a base ficou plana antes de montar. |
| `02_rotor_15_porcoes.stl` | Rotor de dosagem; imprima deitado pela face plana, com as cavidades voltadas para cima. | 20–25% | Evite suportes dentro das cavidades; remova rebarbas e confira o giro livre na base. |
| `03_esqueleto_motor.stl` | Suporte do motor; apoie a face mais ampla e plana na mesa. | 30–40% | Inspecione os encaixes e furos após a impressão; não force o motor no suporte. |
| `04_tampa_superior_removivel.stl` | Tampa superior; oriente a maior face plana para baixo. | 15–20% | Verifique o encaixe com a base sem pressionar excessivamente. |
| `05_presilha_central.stl` | Presilha de fixação ao aquário; apoie a face mais plana. | 40–50% | Peça de fixação sob carga: prefira mais paredes (4–5). Se a geometria formar balanços, use suportes adequados ao seu fatiador. |
| `06_parafuso_presilha.stl` | Parafuso da presilha; oriente o eixo vertical e a cabeça na mesa, se o fatiador mostrar rosca imprimível nessa posição. | 50–100% | Peça pequena e sujeita a aperto; prefira 5 paredes. A rosca impressa pode exigir ajuste fino. Faça um teste de encaixe sem apertar contra o vidro. |
| `07_01_caixa_ESP32_Super_Mini_ULN2003.stl` | Caixa compacta para a variante Super Mini; base apoiada no leito. | 15–20% | Compatível com a tampa Super Mini correspondente. Confira a placa e o driver reais antes de imprimir. |
| `07_02_caixa_ESP32_Full_Size.stl` | Caixa para a variante Full Size; base apoiada no leito. | 15–20% | Compatível com a tampa Full Size correspondente. |
| `08_01_tampa_caixa_deslizante_Super_Mini.stl` | Tampa deslizante da caixa Super Mini; deite a face maior. | 15–20% | Use junto à caixa `07_01`; teste o deslizamento e ajuste a folga no fatiador se necessário. |
| `08_02_tampa_caixa_deslizante_Full_Size.stl` | Tampa deslizante da caixa Full Size; deite a face maior. | 15–20% | Use junto à caixa `07_02`; teste o deslizamento e ajuste a folga no fatiador se necessário. |
| `09_funil.stl` | Funil para direcionar a ração ao rotor; apoie a borda/base mais larga na mesa. | 10–15% | Se houver balanços na parede interna, use suportes removíveis. Evite suportes presos no canal. |
| `10_porca_m3.stl` | Porca impressa para montagem M3; apoie uma face plana. | 50–100% | Peça pequena: use brim ou impressão com outras peças para melhorar a aderência. Verifique se o parafuso M3 entra sem danificar a rosca. |
| `11_rampa.stl` | Rampa de alimentação; apoie a face inferior mais plana. | 15–20% | Confira a orientação no fatiador para evitar suportes no caminho da ração. |

Imprima apenas uma das combinações de caixa e tampa (`07_01` + `08_01` ou `07_02` + `08_02`). O tamanho físico e os encaixes devem ser conferidos no fatiador e com os componentes da montagem antes de imprimir o conjunto todo.

## Firmware

1. Instale a Arduino IDE e o pacote de placas **esp32 by Espressif Systems**.
2. Abra `BettaFeeder_WiFi_BLE/BettaFeeder_WiFi_BLE.ino`.
3. Instale as bibliotecas **NimBLE-Arduino**, **OneWire** e **DallasTemperature** pelo gerenciador de bibliotecas. `WiFi`, `Preferences` e demais componentes ESP32 vêm com o pacote da placa.
4. Selecione sua placa ESP32. Para placas com flash de 4 MB, o firmware recomenda o esquema de partição **No FS (2MB APP x2)**, pois a partição padrão de aplicativo pode ser pequena.
5. Conecte por USB, selecione a porta serial e grave o sketch.
6. Use o painel hospedado ou sirva os arquivos web localmente. Conecte por BLE ou USB Serial, configure o Wi-Fi/horários e alinhe manualmente a cavidade vazia à saída antes de usar `SET_HOME`.

O BLE anuncia o dispositivo como `BettaFeeder`. O firmware inicia com duas refeições configuradas para 08:00 e 20:00; altere os horários pelo painel. Sem Wi-Fi/NTP, os horários automáticos aguardam sincronização. Para push é necessário configurar o Firebase/FCM no painel, além de permitir notificações no navegador.

## Uso inicial

1. Monte o conjunto mecânico e confira se o rotor gira sem atrito.
2. Com o motor parado, alinhe manualmente a cavidade vazia com a abertura de saída.
3. Conecte ao ESP32 por BLE ou USB e envie **Definir posição inicial** (`SET_HOME`).
4. Confirme a recarga no painel depois de abastecer o rotor.
5. Configure Wi-Fi e até dois horários; faça um acionamento de teste sem ração antes de deixar o equipamento em operação.

## Estrutura

```text
BettaFeeder_WiFi_BLE/   Firmware ESP32
models/                 Peças imprimíveis STL
images/                 Imagens do site e prévias dos modelos
index.html              Painel PWA
sobre.html              Página de apresentação
```
