# EVBox MAX ESP32 state machines en Home Assistant variabelen

Versie: v0.3.0 pauze/herstart diagnose  
Component pin in ESPHome YAML: `3cb7b873fe0ae274df45fba5aed1c7f430c5e370`  
Device in Home Assistant: `auto_evbox`

Dit document koppelt de software-state, de EVBox ChargeBox codes en de Home Assistant entiteiten. Gebruik dit als diagnoseblad wanneer laden, pauzeren, kWh-meter of load balancing blijft hangen.

Belangrijkste HA-entiteiten:

| Wat | HA entiteit |
| --- | --- |
| Onze ESP/controller state machine | `sensor.auto_evbox_evbox_state` |
| Menselijke samenvatting | `sensor.auto_evbox_evbox_status` |
| EVBox ChargeBox `cmd26` status | `sensor.auto_evbox_evbox_cb_status_detail` |
| EVBox ChargeBox `cmd6A` request | `sensor.auto_evbox_evbox_current_request_state` |
| Kabelstatus | `sensor.auto_evbox_evbox_cable_status` |
| Kabel maximale stroom | `sensor.auto_evbox_evbox_cable_max_current` |
| Lock status tekst | `sensor.auto_evbox_evbox_lock_status` |
| Lock status ruwe waarde | `sensor.auto_evbox_evbox_cb_lock_state` |
| CB zegt dat hij laadt | `sensor.auto_evbox_evbox_cb_is_charging` |
| Door ESP naar CB gestuurd | `sensor.auto_evbox_evbox_commanded_current` |
| Door CB teruggemeld | `sensor.auto_evbox_evbox_returned_current_limit` |
| Werkelijke EV stroom | `sensor.auto_evbox_evbox_current` |
| Start laden | `switch.auto_evbox_start_charging` |
| Pauze laden zonder sessie-einde | `switch.auto_evbox_pause_charging` |
| Stop laden / sessie beeindigen | `switch.auto_evbox_stop_charging` |
| PV-regelstatus | `sensor.auto_evbox_evbox_pv_status` |
| Begrenzingsreden | `sensor.auto_evbox_evbox_limit_reason` |
| Meterstatus | `sensor.auto_evbox_evbox_meter_status` |
| Meterstand | `sensor.auto_evbox_evbox_meter_value` |

<div style="page-break-after: always;"></div>

## Pagina 1 - Hoofdstate machine van de ESP/controller

Deze state machine is de softwarematige toestand van de ESPHome component. Dit zie je direct in:

`sensor.auto_evbox_evbox_state`

```mermaid
stateDiagram-v2
    [*] --> BOOT
    BOOT --> WAIT_REGISTRATION: setup() / registratie opnieuw starten
    WAIT_REGISTRATION --> ASSIGN_ADDRESS: CB meldt zich op bus
    ASSIGN_ADDRESS --> READ_INFO: CP kent CB-adres
    READ_INFO --> READ_CONFIG: cmd13 info gelezen
    READ_CONFIG --> IDLE: geen kabel / CB beschikbaar
    READ_CONFIG --> PREPARING: kabel aanwezig

    IDLE --> PREPARING: cmd26 0x47 / kabel aanwezig
    PREPARING --> AUTHORIZED: Start laden / cmd22 autostart gestuurd
    PREPARING --> STARTING: cmd6A 0x37 autostart gezien
    AUTHORIZED --> STARTING: wachten op cmd6A / cmd26
    STARTING --> SESSION_STARTING: cmd6B stroomvrijgave gestuurd
    SESSION_STARTING --> CHARGING: cmd23 of cmd26 0x48
    STARTING --> CHARGING: cmd6A 0x81 of cmd26 0x48

    CHARGING --> PAUSED: Pause Charging of PV pause / cmd6B 0A, geen cmd32
    PAUSED --> STARTING: Start Charging of PV resume / cmd6B >= 6A, geen cmd32
    CHARGING --> FINISHING: Stop laden / cmd6B 0A + cmd32
    PAUSED --> FINISHING: Stop laden / cmd32
    FINISHING --> PREPARING: kabel blijft aangesloten / CB terug voorbereiden
    FINISHING --> IDLE: kabel los / beschikbaar
    FINISHING --> STARTING: start opnieuw na FINISHED/0x30

    WAIT_REGISTRATION --> FAULT: watchdog timeout
    ASSIGN_ADDRESS --> FAULT: watchdog timeout
    READ_INFO --> FAULT: watchdog timeout
    READ_CONFIG --> FAULT: watchdog timeout
    PREPARING --> FAULT: CB foutcode
    STARTING --> PREPARING: start timeout zonder sessie
    FAULT --> WAIT_REGISTRATION: herstel / nieuwe communicatie
```

Statewaarden:

| ESP state | Betekenis | Verwachte HA-checks |
| --- | --- | --- |
| `BOOT` | Component start op | kort zichtbaar |
| `WAIT_REGISTRATION` | Wachten tot CB zich meldt | `sensor.auto_evbox_evbox_communication` |
| `ASSIGN_ADDRESS` | ESP wijst CB-adres toe | `sensor.auto_evbox_evbox_cb_serial` |
| `READ_INFO` | Leest CB/meter info | `sensor.auto_evbox_evbox_meter_model`, `sensor.auto_evbox_evbox_meter_serial` |
| `READ_CONFIG` | Leest CB-configuratie | logregels `cmd33`, `cmd34` |
| `IDLE` | Klaar, geen actieve kabel/start | `sensor.auto_evbox_evbox_status` = Ready |
| `PREPARING` | Kabel aanwezig, wacht op start/autorisatie | `sensor.auto_evbox_evbox_cable_status` |
| `AUTHORIZED` | `cmd22` autostart is gestuurd of geaccepteerd | logregel `cmd22 autostart authorization` |
| `STARTING` | Start loopt, wacht op lock/stroom/sessie | `sensor.auto_evbox_evbox_current_request_state` |
| `SESSION_STARTING` | Stroom is vrijgegeven, sessie nog niet bewezen actief | `sensor.auto_evbox_evbox_commanded_current` |
| `CHARGING` | CB meldt laden | `sensor.auto_evbox_evbox_cb_is_charging` = 1 |
| `PAUSED` | Zachte pauze: stroomvrijgave 0A, geen `cmd32`; stekker en sessie blijven herstelbaar | `switch.auto_evbox_pause_charging`, `sensor.auto_evbox_evbox_pv_status` |
| `FINISHING` | Harde stop: sessie afbouwen met `cmd32` | `switch.auto_evbox_stop_charging` |
| `FAULT` | Fout of watchdog | `sensor.auto_evbox_evbox_status` = Fault |

<div style="page-break-after: always;"></div>

## Pagina 2 - EVBox ChargeBox `cmd26` status machine

Dit is de toestand die de EVBox ChargeBox zelf meldt via `cmd26`. Dit zie je in:

`sensor.auto_evbox_evbox_cb_status_detail`

```mermaid
stateDiagram-v2
    [*] --> AVAILABLE_02
    AVAILABLE_02 --> PREPARING_47: kabel ingestoken
    PREPARING_47 --> READY_4A: autorisatie/start voorbereid
    PREPARING_47 --> CHARGING_48: contactor actief / sessie loopt
    READY_4A --> CHARGING_48: EV vraagt stroom
    CHARGING_48 --> PAUSED_49: cmd6B 0A zonder cmd32 / tijdelijk pauzeren
    PAUSED_49 --> CHARGING_48: cmd6B >= 6A / hervatten
    CHARGING_48 --> FINISHED_4B: sessie stopt, kabel blijft zitten
    PAUSED_49 --> FINISHED_4B: Stop Charging / cmd32
    FINISHED_4B --> PREPARING_47: opnieuw starten met kabel erin
    FINISHED_4B --> AVAILABLE_02: kabel los
    PREPARING_47 --> ERROR_0A: CB meldt fout
    CHARGING_48 --> ERROR_0A: CB meldt fout
    ERROR_0A --> PREPARING_47: fout weg, kabel aanwezig
    ERROR_0A --> AVAILABLE_02: fout weg, kabel los
```

Bekende codes in de huidige software:

| `cmd26` code | Naam in HA | Betekenis voor diagnose |
| --- | --- | --- |
| `0x02` | `AVAILABLE 0x02` | CB beschikbaar, normaal geen kabel/start |
| `0x0A` | `ERROR_OR_FAULT 0x0A` | Fout of speciale G3-status; context nodig |
| `0x17` | `IN_USE_OR_PLUGGED 0x17` | In gebruik of ingeplugd, afhankelijk van fase |
| `0x47` | `PREPARING 0x47` | Kabel aanwezig, nog niet ladend |
| `0x48` | `CHARGING 0x48` | Laden actief, lock en contactor meestal actief |
| `0x49` | `PAUSED 0x49` | Laden gepauzeerd |
| `0x4A` | `READY 0x4A` | Klaar voor laadfase |
| `0x4B` | `FINISHED_PLUGGED_IN 0x4B` | Sessie klaar, kabel zit nog in de EVBox |
| onbekend | `UNKNOWN` | Ruwe log nodig |

Bij jouw bekende hangpunt stond dit zo:

| Signaal | Waarde |
| --- | --- |
| `sensor.auto_evbox_evbox_cb_status_detail` | `PREPARING 0x47` |
| `sensor.auto_evbox_evbox_current_request_state` | `OBSERVED_PRESTART_20 0x20` |
| `sensor.auto_evbox_evbox_lock_status` | `UNLOCKED` |
| `sensor.auto_evbox_evbox_commanded_current` | `0.0 A` |

Dat betekent: kabel gezien, maar start/lock/stroomvrijgave nog niet rond.

<div style="page-break-after: always;"></div>

## Pagina 3 - EVBox `cmd6A` current request en start-handshake

Dit is de belangrijkste startdiagnose. De CB vraagt of meldt via `cmd6A`; de ESP antwoordt eerst met ACK en stuurt pas daarna op het juiste moment `cmd6B`.

HA-entiteit:

`sensor.auto_evbox_evbox_current_request_state`

```mermaid
stateDiagram-v2
    [*] --> NO_REQUEST
    NO_REQUEST --> PRESTART_20: CB cmd6A 0x20
    NO_REQUEST --> CONNECTED_30: CB cmd6A 0x30
    PRESTART_20 --> AUTH_SENT: Local start / CP stuurt cmd22 kaart 000000AS
    CONNECTED_30 --> AUTH_SENT: Local start / CP stuurt cmd22 kaart 000000AS
    AUTH_SENT --> AUTH_READY_07: CB meldt cmd6A 0x07
    AUTH_SENT --> AUTH_LOCK_37: CB meldt cmd6A 0x37
    AUTH_SENT --> FALLBACK_6B: CB blijft 0x20/0x30 na korte wachttijd
    AUTH_READY_07 --> CURRENT_RELEASE: delay / cmd6B >= 6A
    AUTH_LOCK_37 --> CURRENT_RELEASE: delay / cmd6B >= 6A
    FALLBACK_6B --> CURRENT_RELEASE: guarded cmd6B fallback
    PAUSED_49 --> CURRENT_RELEASE: Start Charging vanuit pauze / cmd6B >= 6A
    CURRENT_RELEASE --> METERING_23: CB cmd23 sessiestart
    METERING_23 --> CHARGING_81: CB cmd6A 0x81 of cmd26 0x48
    CHARGING_81 --> [*]
```

Belangrijke `cmd6A` codes:

| `cmd6A` code | Naam in HA | Actie van ESP |
| --- | --- | --- |
| geen | `NO_REQUEST` | Nog geen actuele CB request gezien |
| `0x00` | `WAITING_FOR_CMD26` | ACK, status afwachten |
| `0x01` | `CHARGING_ACTIVE_G2` | Sessie actief behandelen |
| `0x07` | `AUTHORIZED_READY_G2` | Mag startvrijgave voorbereiden |
| `0x20` | `OBSERVED_PRESTART_20` | ACK; bij start eerst `cmd22`, daarna fallback mogelijk |
| `0x28` | `OBSERVED_PRESTART_28` | ACK; diagnostisch pre-start signaal |
| `0x2F` | `OBSERVED_PRESTART_2F` | ACK; diagnostisch pre-start signaal |
| `0x30` | `CONNECTED_WAITING` | Kabel/startwachtstand |
| `0x37` | `AUTHORIZED_WAIT_LOCK` | Startvrijgave plannen |
| `0x77` | `OBSERVED_READY_77` | Niet automatisch vrijgeven |
| `0x80` | `UNPLUGGED` | Start resetten |
| `0x81` | `CHARGING` | Laden actief |
| `0xA0` | `AVAILABLE` | Terug naar beschikbaar |
| `0xA7` | `READY` | Klaar/ready, G3-context nodig |
| `0xC1` | `FINISHED` | Sessie klaar |
| `0xE7` | `FAILED` | Fout |

Startvolgorde die we nu willen zien in de log:

1. `cmd26 PREPARING 0x47`, kabel > 0A.
2. Handmatige start of PV-start zet `sensor.auto_evbox_evbox_state` op `AUTHORIZED` of `STARTING`.
3. ESP stuurt `cmd22` met kaart `000000AS`.
4. Ideaal: CB stuurt `cmd6A 0x07` of `0x37`.
5. Daarna ESP stuurt `cmd6B`.
6. Als stap 4 niet komt: ESP probeert `guarded cmd6B fallback`.
7. Succes is bewezen door `cmd23` of `cmd26 0x48`.

<div style="page-break-after: always;"></div>

## Pagina 4 - Handmatig starten, stoppen en pauzeren

Bediening in HA:

| Actie | HA entiteit |
| --- | --- |
| Modus kiezen | `select.auto_evbox_charge_mode` |
| Start laden | `switch.auto_evbox_start_charging` |
| Pauze laden zonder sessie-einde | `switch.auto_evbox_pause_charging` |
| Stop laden | `switch.auto_evbox_stop_charging` |
| Handmatige laadstroom | `number.auto_evbox_manual_current` |
| Maximale laadstroom | `number.auto_evbox_max_current` |
| EVBox groepzekering | `number.auto_evbox_evbox_group_breaker_current` |
| Hoofdzekering | `number.auto_evbox_main_fuse_current` |
| Failsafe mode | `select.auto_evbox_failsafe_mode` |
| Failsafe stroom | `number.auto_evbox_failsafe_current` |

```mermaid
stateDiagram-v2
    [*] --> READY
    READY --> START_REQUESTED: switch Start Charging
    START_REQUESTED --> AUTHORIZING: cmd22 autostart
    AUTHORIZING --> CURRENT_RELEASE: cmd6A 0x07/0x37 of fallback
    CURRENT_RELEASE --> CHARGING: cmd6B >= 6A en CB bevestigt laden
    CHARGING --> PAUSED: switch Pause Charging / cmd6B 0A, geen cmd32
    PAUSED --> CURRENT_RELEASE: switch Start Charging / cmd6B >= 6A, geen cmd32
    CHARGING --> STOPPING: switch Stop Charging / cmd6B 0A + cmd32
    PAUSED --> STOPPING: switch Stop Charging / cmd32
    STOPPING --> FINISHED: CB 0x49/0x4B/0x47, sessie beeindigd
    FINISHED --> READY: kabel los of nieuwe autorisatie/reset
```

Regels:

| Situatie | Wat doet de ESP |
| --- | --- |
| Start in `MANUAL` | Vraagt `manual_current`, begrensd door `max_current`, groepzekering, hoofdzekering en Janitza |
| Pauze tijdens laden | Stuurt alleen `cmd6B 0A`; geen `cmd32`; dit is de normale tijdelijke onderbreking |
| Hervatten na pauze | Stuurt opnieuw `cmd6B >= 6A`; geen nieuwe harde stop |
| Stop tijdens laden | Stuurt eerst `cmd6B 0A`, daarna `cmd32`; dit beeindigt de sessie |
| Pauze zonder stekker eruit | Gebruik `switch.auto_evbox_pause_charging`, niet `switch.auto_evbox_stop_charging` |
| Lager dan 6A gevraagd | Niet als laadstroom sturen; 0A betekent pauze |
| Tussen 0A en 6A | Wordt als 0A/suspend behandeld, omdat AC laden minimaal 6A vraagt |

Diagnose in HA:

| Vraag | Kijk naar |
| --- | --- |
| Heeft de knop gewerkt? | `sensor.auto_evbox_evbox_status` |
| Is de startflow actief? | `sensor.auto_evbox_evbox_state` |
| Heeft de ESP stroom gestuurd? | `sensor.auto_evbox_evbox_commanded_current` |
| Heeft de CB die stroom terug gemeld? | `sensor.auto_evbox_evbox_returned_current_limit` |
| Loopt er echt stroom? | `sensor.auto_evbox_evbox_current` |
| Is de kabel gelocked? | `sensor.auto_evbox_evbox_lock_status` |

<div style="page-break-after: always;"></div>

## Pagina 5 - PV-surplus state machine

PV-surplus gebruikt totaal actief vermogen van de Janitza. Negatief totaalvermogen betekent netto teruglevering. Daarbovenop blijft de load balancing per fase actief.

Hoofd-HA-entiteiten:

| Wat | HA entiteit |
| --- | --- |
| PV mode aan/uit | `switch.auto_evbox_enable_pv_mode` |
| Modus | `select.auto_evbox_charge_mode` |
| PV status | `sensor.auto_evbox_evbox_pv_status` |
| Begrenzingsreden | `sensor.auto_evbox_evbox_limit_reason` |
| PV beschikbare stroom | `sensor.auto_evbox_evbox_pv_available_current` |
| PV starttimer | `sensor.auto_evbox_evbox_pv_start_timer` |
| PV pauzetimer | `sensor.auto_evbox_evbox_pv_pause_timer` |
| Janitza totaalvermogen | `sensor.auto_evbox_janitza_total_power` |
| Janitza import/export | `sensor.auto_evbox_janitza_import_power`, `sensor.auto_evbox_janitza_export_power` |

```mermaid
stateDiagram-v2
    [*] --> PV_DISABLED
    PV_DISABLED --> PV_WAIT_SURPLUS: mode PV_SURPLUS en PV switch aan
    PV_WAIT_SURPLUS --> PV_WAIT_START: netto export genoeg voor >= 6A
    PV_WAIT_START --> PV_START_ALLOWED: starttimer klaar
    PV_START_ALLOWED --> STARTING: startflow starten of hervatten
    STARTING --> PV_CHARGING: CB meldt laden
    PV_CHARGING --> PV_HOLD: export zakt kort weg
    PV_HOLD --> PV_CHARGING: export herstelt binnen pauzetijd
    PV_HOLD --> PV_PAUSED: pauzetimer klaar / cmd6B 0A, geen cmd32
    PV_PAUSED --> PV_CHARGING: export weer genoeg / cmd6B >= 6A
    PV_PAUSED --> PV_WAIT_START: export weer genoeg maar starttimer actief
    PV_PAUSED --> PV_DISABLED: PV switch uit of mode veranderd
```

PV-statuswaarden die je kunt tegenkomen:

| PV status | Betekenis |
| --- | --- |
| `PV_DISABLED` | PV-regeling staat uit |
| `PV_WAIT_SURPLUS` | Wacht op voldoende negatief totaalvermogen |
| `PV_WAIT_START` | Er is genoeg surplus, starttimer loopt |
| `PV_START_ALLOWED` | PV mag laden starten/hervatten |
| `PV_CHARGING` | PV-regeling laat laden toe |
| `PV_PAUSED` | PV-regeling heeft laden tijdelijk op 0A gezet |
| `IDLE` | Geen actieve PV-beslissing |

Belangrijke regel: PV-pauze gebruikt dezelfde zachte pauze als de knop `Pause Charging`. De software hoort dus `cmd6B 0A` te sturen zonder `cmd32`. `Stop Charging` is voor bewust sessie-einde, niet voor PV-surplus regelen.

Begrenzingsredenen:

| Limit reason | Betekenis |
| --- | --- |
| `PV_BELOW_6A` | PV-stroom onder minimale AC-laadgrens |
| `PV_BELOW_6A_PAUSED` | Gepauzeerd omdat PV onder 6A bleef |
| `PV_START_DELAY` | Starttimer loopt |
| `PV_SURPLUS` | PV-surplus is voldoende |
| `PV_SWITCH_OFF` | PV schakelaar staat uit |
| `SAFETY_LIMIT` | Begrensd door zekering/load balancing |
| `UNKNOWN` | Nog geen geldige regelbeslissing |

<div style="page-break-after: always;"></div>

## Pagina 6 - Load balancing per fase

Load balancing gebruikt Janitza stromen plus getekend vermogen per fase. De EVBox-fasen kunnen in HA worden gemapt naar de fysieke Janitza-fasen.

Fase mapping in jouw huidige YAML:

| EVBox fase | Janitza fase |
| --- | --- |
| EVBox L1 | Janitza L3 |
| EVBox L2 | Janitza L1 |
| EVBox L3 | Janitza L2 |

HA-entiteiten:

| Wat | HA entiteit |
| --- | --- |
| Mapping samenvatting | `sensor.auto_evbox_evbox_grid_phase_mapping` |
| EVBox L1 mapping | `select.auto_evbox_evbox_l1_grid_phase` |
| EVBox L2 mapping | `select.auto_evbox_evbox_l2_grid_phase` |
| EVBox L3 mapping | `select.auto_evbox_evbox_l3_grid_phase` |
| Hoofdzekering | `number.auto_evbox_main_fuse_current` |
| EVBox groepzekering | `number.auto_evbox_evbox_group_breaker_current` |
| Max laadstroom | `number.auto_evbox_max_current` |
| Toegestaan na zekeringen | `sensor.auto_evbox_evbox_allowed_current` |
| Gevraagd voor beveiliging | `sensor.auto_evbox_evbox_requested_current` |

```mermaid
stateDiagram-v2
    [*] --> READ_JANITZA
    READ_JANITZA --> MAP_PHASES: mapping EVBox L1/L2/L3 naar Janitza L1/L2/L3
    MAP_PHASES --> CALC_SIGNED_CURRENT: vermogen per fase bepaalt import/export teken
    CALC_SIGNED_CURRENT --> APPLY_MAIN_FUSE: hoofdzekering per fase bewaken
    APPLY_MAIN_FUSE --> APPLY_GROUP_BREAKER: EVBox groepzekering bewaken
    APPLY_GROUP_BREAKER --> APPLY_MAX_CURRENT: max laadstroom bewaken
    APPLY_MAX_CURRENT --> OUTPUT_ALLOWED_CURRENT: laagste grens wint
    OUTPUT_ALLOWED_CURRENT --> SEND_CMD6B: als laden actief of start vrijgegeven
```

Rekenlogica:

| Stap | Regel |
| --- | --- |
| Janitza stroom | Heeft geen teken |
| Janitza vermogen per fase | Positief = import, negatief = export |
| Signed current | Bij negatief vermogen wordt fase-stroom als exportruimte behandeld |
| Hoofdzekering | Elke actieve EVBox-fase mag de hoofdzekering niet overschrijden |
| EVBox groepzekering | Laadstroom mag niet boven `EVBox Group Breaker Current` |
| Max current | Laadstroom mag niet boven `Max Current` |
| 1-fase laden | Alleen actieve EVBox-fase telt |
| 3-fase laden | Laagste beschikbare ruimte van actieve fasen bepaalt limiet |

Janitza HA-entiteiten:

| Janitza signaal | HA entiteit |
| --- | --- |
| L1 stroom | `sensor.auto_evbox_janitza_l1_current` |
| L2 stroom | `sensor.auto_evbox_janitza_l2_current` |
| L3 stroom | `sensor.auto_evbox_janitza_l3_current` |
| L1 vermogen | `sensor.auto_evbox_janitza_l1_power` |
| L2 vermogen | `sensor.auto_evbox_janitza_l2_power` |
| L3 vermogen | `sensor.auto_evbox_janitza_l3_power` |
| Totaalvermogen | `sensor.auto_evbox_janitza_total_power` |
| Communicatie | `sensor.auto_evbox_janitza_communication` |

<div style="page-break-after: always;"></div>

## Pagina 7 - kWh-meter en EVBox metingen

De EVBox-kWh-meter loopt via de CB. De software kan waarden uit `cmd13`, `cmd23` en `cmd26` halen. In de laatste werkende logs kwamen meterstand, spanning, stroom en powerfactor live uit `cmd26`.

HA-entiteiten:

| Wat | HA entiteit |
| --- | --- |
| Meterstatus | `sensor.auto_evbox_evbox_meter_status` |
| Metermodel | `sensor.auto_evbox_evbox_meter_model` |
| Meterserial | `sensor.auto_evbox_evbox_meter_serial` |
| Meterstand kWh | `sensor.auto_evbox_evbox_meter_value` |
| Ruwe meterstand Wh | `sensor.auto_evbox_evbox_raw_meter_wh` |
| Sessie energie | `sensor.auto_evbox_evbox_session_energy` |
| EVBox vermogen | `sensor.auto_evbox_evbox_power` |
| EVBox totaalstroom | `sensor.auto_evbox_evbox_current` |
| EVBox temperatuur | `sensor.auto_evbox_evbox_temperature` |

```mermaid
stateDiagram-v2
    [*] --> METER_UNKNOWN
    METER_UNKNOWN --> CONFIG_CHECK: cmd33 config gelezen
    CONFIG_CHECK --> CONFIG_RESTORE: meter type niet serial/Modbus
    CONFIG_RESTORE --> CONFIG_CHECK: cmd34 geschreven, opnieuw lezen
    CONFIG_CHECK --> METER_PRESENT: meter type serial/Modbus
    METER_PRESENT --> LIVE_FROM_CMD26: cmd26 bevat meterblok
    LIVE_FROM_CMD26 --> SESSION_COUNTING: meterstand stijgt tijdens laden
    SESSION_COUNTING --> LIVE_FROM_CMD26: laden gepauzeerd of gestopt
    METER_PRESENT --> METER_UNKNOWN: cmd13 ontbreekt maar cmd26 kan nog live zijn
```

Meterdiagnose:

| Situatie | Waarde in HA |
| --- | --- |
| Meter werkt volledig | `meter_status` = `PRESENT` of `LIVE_FROM_CMD26` |
| Meterstand loopt | `meter_value` en `raw_meter_wh` stijgen |
| Sessie loopt | `session_energy` stijgt vanaf sessiestart |
| Alleen live blok beschikbaar | model/serial kunnen `UNKNOWN` blijven, maar kWh en spanning kunnen toch goed zijn |
| Meter config fout | log toont `meter_config=0x00`; dan moet CB via `cmd34` naar serial/Modbus |

EVBox fase-metingen:

| EVBox signaal | HA entiteit |
| --- | --- |
| L1 spanning | `sensor.auto_evbox_evbox_l1_voltage` |
| L2 spanning | `sensor.auto_evbox_evbox_l2_voltage` |
| L3 spanning | `sensor.auto_evbox_evbox_l3_voltage` |
| L1 stroom | `sensor.auto_evbox_evbox_l1_current` |
| L2 stroom | `sensor.auto_evbox_evbox_l2_current` |
| L3 stroom | `sensor.auto_evbox_evbox_l3_current` |
| L1 powerfactor | `sensor.auto_evbox_evbox_l1_power_factor` |
| L2 powerfactor | `sensor.auto_evbox_evbox_l2_power_factor` |
| L3 powerfactor | `sensor.auto_evbox_evbox_l3_power_factor` |
| Gedetecteerde laadfasen | `sensor.auto_evbox_evbox_detected_charge_phases` |
| Actieve fase mask | `sensor.auto_evbox_evbox_active_phase_mask` |

<div style="page-break-after: always;"></div>

## Pagina 8 - Multi-EVBox busdiagnose v0.3

Deze pagina is bedoeld voor de v0.3.0-richting met meerdere EVBoxen op dezelfde bus. De huidige primaire regeling blijft op de eerste/actieve ChargeBox gericht; de extra slots zijn vooral diagnose.

```mermaid
stateDiagram-v2
    [*] --> BUS_LISTEN
    BUS_LISTEN --> CB1_SEEN: frame van adres 1
    BUS_LISTEN --> CB2_SEEN: frame van adres 2
    BUS_LISTEN --> CB3_SEEN: frame van adres 3
    CB1_SEEN --> PRIMARY_CONTROL: primaire box krijgt start/stop/current
    CB2_SEEN --> SECONDARY_DIAG: secundaire box ACK/diagnose
    CB3_SEEN --> SECONDARY_DIAG: secundaire box ACK/diagnose
    SECONDARY_DIAG --> BUS_LISTEN: status gepubliceerd in HA
    PRIMARY_CONTROL --> BUS_LISTEN: status gepubliceerd in HA
```

HA-entiteiten per box:

| Slot | Samenvatting | Status | Kabel | Lock | Meter |
| --- | --- | --- | --- | --- | --- |
| EVBox 1 | `sensor.auto_evbox_evbox_1_summary` | `sensor.auto_evbox_evbox_1_status` | `sensor.auto_evbox_evbox_1_cable_status` | `sensor.auto_evbox_evbox_1_lock_status` | `sensor.auto_evbox_evbox_1_meter_value` |
| EVBox 2 | `sensor.auto_evbox_evbox_2_summary` | `sensor.auto_evbox_evbox_2_status` | `sensor.auto_evbox_evbox_2_cable_status` | `sensor.auto_evbox_evbox_2_lock_status` | `sensor.auto_evbox_evbox_2_meter_value` |
| EVBox 3 | `sensor.auto_evbox_evbox_3_summary` | `sensor.auto_evbox_evbox_3_status` | `sensor.auto_evbox_evbox_3_cable_status` | `sensor.auto_evbox_evbox_3_lock_status` | `sensor.auto_evbox_evbox_3_meter_value` |

Extra per slot:

| Slot | Serial | L1 spanning | L1 stroom |
| --- | --- | --- | --- |
| EVBox 1 | `sensor.auto_evbox_evbox_1_serial` | `sensor.auto_evbox_evbox_1_l1_voltage` | `sensor.auto_evbox_evbox_1_l1_current` |
| EVBox 2 | `sensor.auto_evbox_evbox_2_serial` | `sensor.auto_evbox_evbox_2_l1_voltage` | `sensor.auto_evbox_evbox_2_l1_current` |
| EVBox 3 | `sensor.auto_evbox_evbox_3_serial` | `sensor.auto_evbox_evbox_3_l1_voltage` | `sensor.auto_evbox_evbox_3_l1_current` |

Let op: voor echt actief regelen van drie laders is nog een verdeelstrategie nodig. Dit document beschrijft de huidige diagnose- en primaire regelstate.

<div style="page-break-after: always;"></div>

## Pagina 9 - Snelle diagnose: waar blijf ik hangen?

```mermaid
flowchart TD
    A[Startprobleem] --> B{EVBox online?}
    B -- nee --> B1[Check RS485, voeding, A/B, baud 38400 8N1]
    B -- ja --> C{Kabel connected?}
    C -- nee --> C1[Check sensor.auto_evbox_evbox_cable_status en cable max current]
    C -- ja --> D{CB status PREPARING?}
    D -- nee --> D1[Check cmd26 code in CB Status Detail]
    D -- ja --> E{cmd22 gestuurd?}
    E -- nee --> E1[Startknop of PV-start bereikt start_session niet]
    E -- ja --> F{cmd6A 0x07/0x37 of fallback?}
    F -- nee --> F1[Hangt in autorisatie/current request]
    F -- ja --> G{cmd6B gestuurd?}
    G -- nee --> G1[PV of safety limit blokkeert stroomvrijgave]
    G -- ja --> H{CB lock=1?}
    H -- nee --> H1[CB accepteert stroom maar lockt/contacteert niet]
    H -- ja --> I{cmd26 CHARGING?}
    I -- nee --> I1[Wachten op cmd23/cmd26 of EV vraagt geen stroom]
    I -- ja --> J[Laden actief]
    J --> K{Tijdelijk pauzeren?}
    K -- ja --> K1[Gebruik Pause Charging: cmd6B 0A zonder cmd32]
    K -- nee --> K2[Gebruik Stop Charging alleen voor sessie-einde]
```

Checklist met HA-entiteiten:

| Check | Goed teken | HA entiteit |
| --- | --- | --- |
| Communicatie | `EVBox online` | `sensor.auto_evbox_evbox_communication` |
| Kabel | `CONNECTED` | `sensor.auto_evbox_evbox_cable_status` |
| Kabel amp | groter dan 0, bij jou vaak 32A | `sensor.auto_evbox_evbox_cable_max_current` |
| CB status | `PREPARING`, daarna `CHARGING` | `sensor.auto_evbox_evbox_cb_status_detail` |
| Current request | `0x20/0x30`, daarna `0x07/0x37/0x81` | `sensor.auto_evbox_evbox_current_request_state` |
| Autorisatie | logregel `cmd22 autostart authorization` | ESPHome log |
| Vrijgave | groter dan 0A of `guarded cmd6B fallback` | `sensor.auto_evbox_evbox_commanded_current` |
| CB terugmelding | meestal 6A of hoger | `sensor.auto_evbox_evbox_returned_current_limit` |
| Lock | `LOCKED` of ruwe waarde 1 | `sensor.auto_evbox_evbox_lock_status` |
| Echt laden | `CHARGING` en stroom > 0A | `sensor.auto_evbox_evbox_current` |
| Zacht pauzeren | `PAUSED`, geen `cmd32` in log | `switch.auto_evbox_pause_charging` |
| Hard stoppen | `cmd32` in log, sessie kan eindigen | `switch.auto_evbox_stop_charging` |
