# EVLoadBalance WaveShare

ESP32 firmware voor de Waveshare ESP32-S3-Relay-6CH als lokale EVBox MAX/HomeLine load balancer.

Er zijn drie varianten:

- `esphome/evloadbalance.yaml`: aanbevolen voor Home Assistant. Geeft ESPHome entiteiten voor datalogging, modus-keuze en setpoints.
- `esphome/evbox-g2-waveshare-ricardo.yaml`: vrijwel directe kopie van Ricardo's EVBox G2 ESPHome YAML, alleen aangepast naar Waveshare ESP32-S3 RS485 GPIO17/GPIO18. Gebruik deze om puur te testen of de EVBox gaat laden.
- `esphome/evbox-g2-waveshare.yaml`: eigen Waveshare ESP32-S3 variant op basis van de EVBox G2 flow uit `ricardoschluter/evbox-g2-esphome-controller`, maar met de lokale `evbox_max` en `janitza_umg604` componenten.
- `src/main.cpp` met `platformio.ini`: standalone Arduino/PlatformIO testfirmware met eigen webpagina.

De gevraagde upstream-bron staat lokaal onder `upstream/evbox-g2-esphome-controller-main`.

## Hardware

- Board: Waveshare ESP32-S3-Relay-6CH
- EVBox RS485: onboard RS485, GPIO17 TX / GPIO18 RX, 38400 8N1
- Janitza: Modbus TCP via WiFi, standaard `192.168.1.30:502`, unit id `1`
- Relais:
  - CH1: EVBox ChargeBox bekend
  - CH2: Janitza data vers
  - CH3: laden actief
  - CH4: fout/stale fallback actief

Volgens de Waveshare wiki gebruikt dit board relais GPIO1, GPIO2, GPIO41, GPIO42, GPIO45 en GPIO46; RGB zit op GPIO38 en buzzer op GPIO21.

## Eerste start

### ESPHome / Home Assistant

1. Kopieer de inhoud van de map `esphome` naar je ESPHome configuratiemap. Belangrijk: `evloadbalance.yaml` en de map `components` moeten naast elkaar staan.
2. Zet in `secrets.yaml`:
   ```yaml
   wifi_ssid: "jouw-wifi"
   wifi_password: "jouw-wachtwoord"
   ```
3. Pas bovenin `evloadbalance.yaml` eventueel `janitza_ip` aan.
4. Installeer/flash via de ESPHome add-on.

Voor de pure Ricardo-laadtest kies je `evbox-g2-waveshare-ricardo.yaml`. Voor de eigen load-balancer variant kies je `evbox-g2-waveshare.yaml` en controleer je bovenin dat `main_fuse_current`, `charger_breaker_current`, `max_charge_current`, `charge_phases` en `janitza_ip` overeenkomen met je installatie.

Home Assistant krijgt o.a.:

- Select `Laadmodus`: `disabled`, `manual`, `load`, `pv`
- Switches: `Regeling uit`, `Manueel laden`, `Load balancing`, `PV surplus`
- Numbers: handmatige laadstroom, maximale laadstroom, hoofdzekering, Janitza timeout, fallback laadstroom
- Sensors: berekende laadstroom, laatst gestuurde limiet, Janitza import/export, Janitza L1/L2/L3, Janitza leeftijd
- Binary sensors: Janitza data vers, EVBox bekend
- Text sensors: EVBox status, laadregel reden

Deze entiteiten worden via de ESPHome native API aan Home Assistant aangeboden, zodat HA ze kan loggen in de recorder/history.

### Standalone PlatformIO

1. Flash met PlatformIO:
   ```powershell
   pio run -t upload
   ```
2. Open de serial monitor:
   ```powershell
   pio device monitor
   ```
3. Verbind met WiFi AP `EVLoadBalance-setup`.
4. Open `http://192.168.4.1`.
5. Vul WiFi, Janitza en laadregeling in en sla op.

## Load balancing

Modi:

- `disabled`: stuurt 0 A.
- `manual`: vaste handmatige stroom.
- `load`: begrenst op hoofdzekering/fasebelasting en gebruikt PV-export als extra ruimte.
- `pv`: laadt alleen op PV-overschot. Bij korte dip houdt hij 30 s minimaal 6 A vast, daarna 0 A.

Als Janitza-data ontbreekt of ouder is dan de timeout, valt `load`/`pv` terug op de ingestelde fallback, standaard 6 A.

## EVBox-protocol uit testapp

Gebruikt dezelfde framevorm als de lokale PC-testapp:

- `0x02 + ASCII payload + ASCII checksum + ASCII XOR parity + 0x03`
- CP adres standaard `0x80`
- Nieuwe CB meldt zich met `src 00 dst 80 cmd 11`
- CP wijst adres toe met `cmd 11` response naar broadcast
- Current limit via `cmd 6B`: `01 + min*10 + L1*10 + L2*10 + L3*10`
- 16 A voorbeeld: `01003C00A000A000A0`
- 7 A voorbeeld: `01003C004600460046`
