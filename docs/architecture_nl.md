# Architectuur en Communicatie

Dit project vervangt de originele EVBox ChargePoint-controller door een ESP32
met ESPHome. De originele EVBox ChargeBox blijft het vermogensdeel doen. De ESP
stuurt lokaal via RS485 en leest de Janitza UMG604 via Modbus TCP.

## Hoofdrollen

- `evbox_max`: praat met de EVBox ChargeBox via EVBox MAX over RS485.
- `janitza_umg604`: leest netmetingen via Modbus TCP.
- `ChargeController`: bepaalt lokaal de gewenste laadstroom.
- Home Assistant: toont sensoren en geeft bediencommando's, maar beslist niet
  zelfstandig over veiligheid of laadstroom.

## EVBox RS485 Communicatie

De EVBox-kant bestaat uit drie lagen:

1. UART transport
   - 38400 baud
   - 8 databits
   - geen parity
   - 1 stopbit
   - half-duplex RS485 via MAX3485

2. Protocol
   - startbyte `0x7E`
   - adres
   - frametype
   - payloadlengte
   - payload
   - checksum

3. State machine
   - beslist wat een geldig frame betekent
   - stuurt vervolgframes zoals address assignment, info request, heartbeat en
     current setpoint

De MAX3485 gebruikt een driver-enable pin. Die staat normaal uit zodat de
ChargeBox de bus kan gebruiken. Alleen tijdens zenden zet de ESP deze pin aan.
Na `flush()` gaat hij weer uit, zodat de laatste byte niet wordt afgekapt.

## Frame Parser

De parser werkt streaming. Dat betekent dat hij byte voor byte wordt gevoed
vanuit `loop()`.

Parserstappen:

```text
WAIT_SOF
ADDRESS
TYPE
LENGTH
PAYLOAD
CHECKSUM
```

Pas wanneer checksum en lengte kloppen, levert de parser een compleet `Frame`
op. Slechte of te lange frames worden weggegooid en de parser wacht opnieuw op
de startbyte.

## Checksum

De checksum is een simpele two's-complement additieve checksum. Alle bytes na
de startbyte worden opgeteld. De checksum wordt zo gekozen dat:

```text
adres + type + lengte + payload + checksum == 0 modulo 256
```

Dit is goedkoop voor de ESP en vangt veel simpele communicatieproblemen af.

## EVBox State Machine

De gewenste hoofdflow is:

```text
BOOT
WAIT_REGISTRATION
ASSIGN_ADDRESS
READ_INFO
READ_CONFIG
IDLE
AUTHORIZED
STARTING
CHARGING
PAUSED
FINISHING
FAULT
```

Betekenis:

- `BOOT`: component start op.
- `WAIT_REGISTRATION`: wacht op eerste contact van de ChargeBox.
- `ASSIGN_ADDRESS`: kent een lokaal adres toe als de ChargeBox nog geen adres
  heeft.
- `READ_INFO`: vraagt hardware/modelinformatie op.
- `READ_CONFIG`: vraagt configuratie en limieten op.
- `IDLE`: systeem is klaar, maar er loopt geen laadsessie.
- `AUTHORIZED`: lokaal startcommando is gegeven.
- `STARTING`: laadstart wordt voorbereid.
- `CHARGING`: er wordt geladen.
- `PAUSED`: laden is tijdelijk gepauzeerd.
- `FINISHING`: sessie wordt netjes afgebouwd.
- `FAULT`: communicatie- of protocolfout.

Alle overgangen lopen door `transition_()`. Daardoor kunnen logging,
Home Assistant publicatie en toekomstige veiligheidschecks op één plek worden
toegevoegd.

## Heartbeat en Watchdog

De ESP stuurt periodiek:

- heartbeat
- gewenste laadstroom

Als er te lang geen geldig EVBox-frame binnenkomt, gaat de controller naar
`FAULT`. Dat voorkomt dat oude stroomcommando's actief blijven wanneer de bus
stilvalt.

## Janitza Modbus TCP

De Janitza-component opent per poll een TCP-verbinding naar:

```text
192.168.1.30:502
```

Hij leest holding registers met Modbus functie `0x03`. De huidige implementatie
verwacht 32-bit IEEE-754 floats over twee registers in big-endian
Modbus-volgorde. Als waarden extreem groot, negatief of bijna nul zijn terwijl
het display van de Janitza normaal is, klopt meestal de registermap of
byte/word-volgorde niet.

Gelezen waarden:

- stroom L1/L2/L3
- spanning L1/L2/L3
- totaal vermogen
- importvermogen
- exportvermogen
- communicatiestatus

De registermap is overgenomen uit het werkende project
`Janitza UMG604-Pro HA`:

```text
1317 voltage_l1
1319 voltage_l2
1321 voltage_l3
1325 current_l1
1327 current_l2
1329 current_l3
1369 active_power_total
```

Import/export wordt afgeleid uit `active_power_total`: positief is import,
negatief is export.

Als één registerpoll faalt, wordt de meter voor die cyclus offline gezet. De
EVBox-regelalgoritmes kunnen dan direct naar failsafe.

De ESPHome waarschuwing voor lang blokkerende componenten staat voor deze
component op 500 ms. Modbus TCP over netwerk mag dus normaal rond 100-300 ms
duren zonder dat dit als probleem wordt gelogd.

Voor snelle terugregeling leest de ESP de live Janitza-registers in één
Modbus-request in plaats van losse requests per sensor. Met `evbox_max_id`
gekoppeld aan de EVBox-component worden import/exportwaarden direct doorgegeven.
Als de berekende laadstroom lager wordt dan de huidige setpoint, stuurt
`evbox_max` meteen een lagere current setpoint en wacht hij niet op de volgende
heartbeat.

## Regelalgoritmes

De laadstroom wordt bepaald in `ChargeController`, niet in het protocol.

Modes:

- `MANUAL`: vaste handmatige stroom, begrensd door `max_current`.
- `LOAD_BALANCING`: verlaagt laadstroom op basis van importvermogen.
- `PV_SURPLUS`: gebruikt exportvermogen als indicatie voor beschikbaar
  PV-overschot.
- `DISABLED`: zet laadstroom op 0 A.

Failsafe bij Janitza-verlies:

- `LIMIT_6A`: terug naar maximaal 6 A.
- `STOP`: laadstroom naar 0 A.

## Belangrijke Validatie

De projectstructuur en componentgrenzen staan klaar, maar EVBox MAX
frametypes en payloadvelden moeten nog gevalideerd worden met echte EVBox G2
buscaptures of protocolinformatie voordat live laden veilig is.

Start daarom met:

1. RS485 receive-only logging.
2. Checksum en frameparser valideren.
3. Registration/address assignment controleren.
4. Heartbeat bevestigen.
5. Current setpoint testen zonder contactor.
6. Pas daarna gecontroleerd laden.
