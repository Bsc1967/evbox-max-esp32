# EVBox MAX ESP32 v0.1

Eerste werkende laadversie.

Deze snapshot is bevroren op 2026-08-26 nadat laden aantoonbaar werkte:

- `cmd22` autorisatieflow werkt voor autostartkaart `000000AS`
- `cmd6A -> cmd6B -> cmd23 -> cmd26` startflow werkt
- `cmd26 status 0x48 CHARGING` is gezien
- EVBox contactor/lock is gezien met `lock=1`
- EVBox meterwaarden via `cmd26` zijn gezien
- 1-fase laden werkte met L1 stroom rond 5.7 A bij 6 A limiet

De ESPHome YAML in deze snapshot verwijst naar component commit:

`ff2367d1928fb2f341b4e45537b39301bc463f90`

Gebruik deze map als rollback-basis voordat nieuwe wijzigingen voor PV-surplus,
load balancing of verdere kWh-meterconfiguratie worden aangebracht.
