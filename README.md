# Tractor Tilt Monitor — Kubota EK1-261

Bezpečnostný monitor náklonu do kabíny traktora. Meria bočný (roll) a pozdĺžny
(pitch) náklon, zobrazuje na OLED a pri prekročení prahu píska.

> ⚠️ **Toto je pomocný varovný systém, nie náhrada za ROPS/pás ani za bezpečnú
> jazdu.** Prahy sú konzervatívny odhad — over a dolaď ich na stroji na
> kontrolovanom svahu.

## Zapojenie

### I2C zbernica (OLED + MPU-6050 zdieľajú)
| Modul | VCC | GND | SDA | SCL |
|---|---|---|---|---|
| GY-521 (MPU-6050) | 3V3 | GND | GPIO21 | GPIO22 |
| OLED SSD1306 | 3V3 | GND | GPIO21 | GPIO22 |

MPU je na adrese `0x68`, OLED na `0x3C` — na jednej zbernici nekolidujú.

### Bzučák cez BC337 (NPN, low-side spínač)
```
GPIO25 ──[ 1k ]── B (baza BC337)
                  C ── jeden vyvod bzucaka
                  E ── GND
   +5V ───────────── druhy vyvod bzucaka
```
- BC337: pri pohľade na plochú stranu s vývodmi dole je poradie **C – B – E**
  (over v datasheete, DIOTEC 171553).
- Bázový odpor ~1 kΩ (máš breadboard, netreba spájkovať hneď).
- Chceš hlasnejšie? Kolektor/bzučák priveď na +12 V namiesto +5 V (PK-20A38WQ
  to znesie), emitor stále na GND — logika ostáva rovnaká.

### Napájanie z traktora (12 V)
```
12V traktor ──[ poistka 1A ]──[ dioda proti prepolovaniu ]── LM2596 IN+
                                                              LM2596 IN-  ── GND
LM2596 OUT+ (nastav na 5.0V!) ── ESP32 5V/VIN
LM2596 OUT- ─────────────────── ESP32 GND
```
1. **Pred pripojením ESP32** nastav LM2596 trimrom presne na **5,0 V** (meraj
   multimetrom naprázdno).
2. Poistka + dióda (napr. 1N5819/1N4007) chránia pred skratom a prepólovaním.
3. Automobilová sieť má napäťové špičky — pridaj elektrolyt (~470 µF) na vstup
   LM2596. LM2596 znesie vstup do 40 V.
4. Napájaj z okruhu, ktorý ide cez **zapaľovanie** (ne priamo z batérie), inak
   ti to bude vybíjať batériu pri státí.

## Kalibrácia a nastavenie

1. **Montáž:** krabičku S-BOX pevne priskrutkuj do kabíny (nie voľne položenú —
   inak meriaš hádzanie krabičky, nie traktora). Nemusí byť dokonalo rovno.
2. **Vynulovanie:** postav traktor na **rovnú plochu**, zapni a stlač **BOOT
   tlačidlo (GPIO0)** na ESP32. Tým sa aktuálna poloha uloží ako „0°" a
   vykompenzuje sa šikmá montáž.
3. **Prahy** v `.ino` (`TILT_WARN`, `TILT_DANGER`) doladi na stroji.

## Prahy náklonu — dôležité

Predvolené: `WARN = 12°`, `DANGER = 20°` (celkový náklon od zvislice).

Malé kompaktné traktory ako EK1-261 majú vysoké ťažisko a prevrátia sa **skôr,
než väčšina ľudí čaká**, hlavne do boku a pri jazde po vrstevnici svahu. Statický
uhol preklopenia býva ~30–40°, ale za jazdy (zotrvačnosť, náklad, terén) sa
bezpečná hranica výrazne znižuje. Preto:

- Varovanie nastav **výrazne pod** reálny uhol preklopenia.
- Doladenie rob **opatrne, na miernom svahu, s možnosťou úniku**, ideálne bez
  vodiča (diaľkovo) alebo s pásom a ROPS.
- Zvlášť sleduj **roll (bočný náklon)** — je nebezpečnejší než pitch.

## Knižnice (Arduino IDE)
- ESP32 core (Espressif ~2.0.x) — kód používa `ledcSetup`/`ledcAttachPin`
- **Adafruit SSD1306** + **Adafruit GFX**
- MPU-6050 sa číta priamo cez `Wire` (bez knižnice)

Board: **ESP32 Dev Module**. Serial Monitor **115200**.
