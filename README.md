# 🪴 Sistem Irigasi Otomatis IoT Solar-Powered

Proyek sistem penyiraman tanaman otomatis berbasis **Internet of Things (IoT)** yang hemat energi dengan mekanisme *Modified Deep Sleep*, bertenaga mandiri dari panel surya (*Solar-Powered*), dilengkapi **Web Dashboard** interaktif yang di-host langsung (*self-hosted*) pada mikrokontroler, serta notifikasi *push real-time* via **ntfy**.

---

## 📌 Fitur Utama

- **Solar Powered & BMS:** Sistem catu daya mandiri menggunakan Solar Panel 5V/6V, baterai Li-ion 18650, modul *charger* TP4056 dengan *Battery Management System* (BMS), dan Step-Up MT3608.
- **Manajemen Daya Hemat Energi (Modified Deep Sleep):** NodeMCU tidur secara presisi berdasarkan jadwal penyiraman berikutnya dan otomatis bangun via koneksi pin `D0` ke `RST`.
- **Tombol Bangun Manual (Wake Button):** Tombol *push-button* fisik untuk mereset/membangunkan sistem secara manual kapan saja tanpa menunggu siklus *deep sleep* selesai.
- **Web Dashboard Local (AP+STA Mode):** Dashboard kontrol dan konfigurasi jadwal di-host langsung dari ESP8266 (`ESP8266WebServer`) via hotspot Access Point internal (`WIFI_AP_STA`), tanpa membutuhkan server/cloud eksternal.
- **Penyimpanan Konfigurasi Persisten:** Jadwal dan konfigurasi sistem disimpan di *flash memory* internal ESP8266 menggunakan **LittleFS/EEPROM**.
- **Sinkronisasi Waktu Akurat:** Sinkronisasi jam otomatis via **NTP Client**.
- **Notifikasi Push via ntfy:** Mengirimkan laporan status penyiraman langsung ke aplikasi Android via HTTP POST tanpa perantara *backend*.
- **Keamanan & Proteksi Hardware:** Fitur *Dry-Run Protection* untuk mencegah kerusakan pompa submersible saat kekurangan air.

---

## 🛠️ Modul & Komponen Hardware

| No | Komponen | Deskripsi / Spesifikasi |
|---|---|---|
| 1 | **NodeMCU ESP8266 v3** | Mikrokontroler utama berbasis Wi-Fi |
| 2 | **Solar Panel Mini 5V/6V** | Sumber pengisian daya utama dari sinar matahari |
| 3 | **Modul TP4056 BMS** | Charger baterai Li-ion dengan proteksi *over-charge* & *over-discharge* (IC DW01+FS8205) |
| 4 | **Baterai Li-ion 18650** | Penyimpan daya sistem |
| 5 | **Modul Step-Up MT3608** | Penaik tegangan baterai ke 5.0V stabil |
| 6 | **Modul Relay 1 Channel 5V** | Sakelar elektronik sakelar daya pompa (Active LOW) |
| 7 | **Mini Submersible Pump 5V** | Aktuator penyiram air |
| 8 | **Push-Button** | Tombol pemicu bangun manual (Manual Wake Button) |
| 9 | **Casing Waterproof (Box IP65)** | Pelindung rangkaian elektronik dari panas dan hujan |

---

## 🔌 Panduan Skematik & Sambungan Kabel (Wiring Map)

> ⚠️ **PENTING SEBELUM DIBERI DAYA:** Putar potensio pada modul MT3608 dan ukur menggunakan multimeter hingga tegangan output **VOUT+ / VOUT-** terbaca tepat **5.0V** menggunakan sumber USB 5V sebelum menyambungkan ke NodeMCU!

### 1. Sistem Daya (Solar Panel → Baterai → Regulated Output)
* `Solar Panel (+)` ➔ `TP4056 IN(+)`
* `Solar Panel (-)` ➔ `TP4056 IN(-)`
* `TP4056 BAT(+)` ➔ `Baterai 18650 (+)` **DAN** `MT3608 VIN(+)`
* `TP4056 BAT(-)` ➔ `Baterai 18650 (-)` **DAN** `MT3608 VIN(-)`

### 2. Sistem Kontrol & Aktuator
* `MT3608 VOUT(+)` (5.0V) ➔ NodeMCU `VIN`, Relay `VCC`, dan Relay `COM`
* `MT3608 VOUT(-)` (GND) ➔ NodeMCU `GND`, Relay `GND`, dan Kabel (-) Pompa Air
* `NodeMCU Pin D7` ➔ Relay Pin `IN`
* `Relay Pin NO` ➔ Kabel (+) Pompa Air

### 3. Jalur Deep Sleep & Tombol Manual
* `NodeMCU Pin D0` ➔ `NodeMCU Pin RST` *(Sambungan pendek pada board)*
* `Tombol Kaki 1` ➔ `NodeMCU Pin RST`
* `Tombol Kaki 2` ➔ `GND`

---

## 💻 Kebutuhan Software & Library

- **Arduino IDE** (Versi terbaru)
- **ESP8266 Board Package** (`http://arduino.esp8266.com/stable/package_esp8266com_index.json`)
- **Driver CH340** (Untuk komunikasi USB NodeMCU)
- **Library Arduino (Wajib):**
  - `WiFiManager` (by tzapu)
  - `NTPClient` (by Fabrice Weinberg)
  - `ArduinoJson` (by Benoit Blanchon)
  - `ESP8266WiFi`, `ESP8266WebServer`, `ESP8266HTTPClient`, `LittleFS` *(Bawaan board package)*

---

## 🚀 Panduan Memulai (Getting Started)

1. **Clone Repositori:**
   ```bash
   git clone [https://github.com/AndriantoEvan/irigasi-otomatis.git](https://github.com/AndriantoEvan/irigasi-otomatis.git)
   cd irigasi-otomatis
