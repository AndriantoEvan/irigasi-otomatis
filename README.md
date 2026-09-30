# 🪴 Sistem Irigasi Otomatis IoT Solar-Powered

Sistem penyiraman tanaman otomatis berbasis **Internet of Things (IoT)** yang hemat energi dengan mekanisme *Modified Deep Sleep*, bertenaga mandiri dari panel surya (*Solar-Powered*), dilengkapi **Web Dashboard** interaktif *self-hosted*, serta notifikasi *push real-time* via **ntfy**.

---

## 📌 Fitur Utama

- **Solar Powered & BMS:** Sistem daya mandiri dari Solar Panel 5V/6V, baterai Li-ion 18650, modul *charger* TP4056 (dengan proteksi BMS over-charge/over-discharge), dan Step-Up MT3608.
- **Manajemen Daya Hemat Energi (Modified Deep Sleep):** NodeMCU tidur secara presisi berdasarkan waktu penyiraman berikutnya dan otomatis bangun via koneksi pin `D0` ke `RST`.
- **Tombol Bangun Manual (Wake Button):** Tombol fisik untuk mereset/membangunkan sistem secara manual kapan saja tanpa menunggu siklus *deep sleep* selesai.
- **Web Dashboard Local (AP+STA Mode):** Dashboard kontrol dan konfigurasi jadwal di-host langsung dari ESP8266 (`ESP8266WebServer`) via hotspot Access Point internal (`WIFI_AP_STA`), tanpa membutuhkan server/cloud eksternal.
- **Penyimpanan Konfigurasi Persisten:** Jadwal dan konfigurasi sistem disimpan di *flash memory* internal ESP8266 menggunakan **LittleFS/EEPROM**.
- **Sinkronisasi Waktu Akurat:** Sinkronisasi jam otomatis via **NTP Client**.
- **Notifikasi Push via ntfy:** Mengirimkan laporan status penyiraman langsung ke aplikasi Android via HTTP POST tanpa perantara *backend*.
- **Keamanan & Proteksi Hardware:** Fitur *Dry-Run Protection* untuk membatasi durasi nyala pompa demi mencegah kerusakan saat air habis.

---

## 🛠️ Komponen Utama

| No | Komponen | Deskripsi / Fungsi |
|---|---|---|
| 1 | **NodeMCU ESP8266 v3** | Mikrokontroler utama berbasis Wi-Fi |
| 2 | **Solar Panel Mini 5V/6V** | Sumber pengisian daya utama dari sinar matahari |
| 3 | **Modul TP4056 BMS** | Charger baterai Li-ion dengan proteksi *over-charge* & *over-discharge* |
| 4 | **Baterai Li-ion 18650** | Penyimpan daya sistem |
| 5 | **Modul Step-Up MT3608** | Regulating & step-up tegangan keluaran ke 5.0V stabil |
| 6 | **Modul Relay 1 Channel 5V** | Sakelar elektronik untuk kontrol daya pompa air |
| 7 | **Mini Submersible Pump 5V** | Aktuator penyiram tanaman |
| 8 | **Push-Button** | Tombol pemicu bangun manual (*Wake Button*) |
| 9 | **Casing Waterproof (Box IP65)** | Pelindung rangkaian elektronik dari cuaca outdoor |

---

## 💻 Kebutuhan Software & Library

- **Arduino IDE**
- **ESP8266 Board Package**
- **Driver CH340**
- **Library Arduino:**
  - `WiFiManager` (by tzapu)
  - `NTPClient` (by Fabrice Weinberg)
  - `ArduinoJson` (by Benoit Blanchon)
  - `ESP8266WiFi`, `ESP8266WebServer`, `ESP8266HTTPClient`, `LittleFS` *(Bawaan Board Package)*

---

## 🚀 Panduan Memulai (Getting Started)

1. **Clone Repositori:**
   ```bash
   git clone [https://github.com/AndriantoEvan/irigasi-otomatis.git](https://github.com/AndriantoEvan/irigasi-otomatis.git)
   cd irigasi-otomatis
