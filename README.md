# 🌾 Sistem Irigasi Otomatis (Smart Automatic Irrigation System)

![License](https://img.shields.io/github/license/AndriantoEvan/irigasi-otomatis?style=flat-square)
![Forks](https://img.shields.io/github/forks/AndriantoEvan/irigasi-otomatis?style=flat-square)
![Stars](https://img.shields.io/github/stars/AndriantoEvan/irigasi-otomatis?style=flat-square)
![Issues](https://img.shields.io/github/issues/AndriantoEvan/irigasi-otomatis?style=flat-square)

Sistem Irigasi Otomatis berbasis **Internet of Things (IoT)** dan **Mikrokontroler** yang dirancang untuk mengoptimalkan penggunaan air dan otomatisasi penyiraman tanaman berdasarkan kondisi kelembapan tanah real-time.

---

## 📌 Fitur Utama

- 🌡️ **Monitoring Real-time**: Memantau tingkat kelembapan tanah, suhu, dan kondisi lingkungan secara akurat.
- 🚰 **Penyiraman Otomatis**: Pompa air akan aktif/nonaktif secara otomatis berdasarkan ambang batas (*threshold*) kelembapan tanah.
- 📱 **Integrasi IoT / Dashboard**: Pemantauan dan kontrol manual jarak jauh melalui antarmuka web atau aplikasi mobile.
- ⚡ **Efisiensi Energi & Air**: Mengurangi pemborosan air dan konsumsi daya dengan sistem kendali terukur.

---

## 🛠️ Komponen & Perangkat Keras

Berikut adalah perangkat keras yang umum digunakan dalam proyek ini:

| Komponen | Deskripsi |
| :--- | :--- |
| **Mikrokontroler** | ESP32 / ESP8266 / Arduino Uno |
| **Sensor** | Soil Moisture Sensor (Capacitive/Resistive), DHT11/DHT22 |
| **Actuator** | Relay Module (1-Channel / 2-Channel) |
| **Output** | Mini Water Pump DC (5V/12V), Solenoid Valve |
| **Catu Daya** | Adapter 5V/12V DC atau Panel Surya |

---

## 🏗️ Arsitektur & Cara Kerja

1. **Pembacaan Data Sensor**: Sensor membaca kelembapan tanah dan mengirimkan nilai analog/digital ke mikrokontroler.
2. **Pengolahan Data**: Mikrokontroler memproses data berdasarkan kriteria logika yang telah ditentukan:
   - Jika `Kelembapan Tanah < Threshold Minimum` ➡️ **Pompa ON**
   - Jika `Kelembapan Tanah >= Threshold Maksimum` ➡️ **Pompa OFF**
3. **Konektivitas Cloud / IoT**: Data dikirimkan ke cloud platform (seperti Blynk, ThingsBoard, atau MQTT Broker) untuk pemantauan jarak jauh.

---

## 🚀 Panduan Memulai (*Getting Started*)

### 1. Persyaratan Sistem
- [Arduino IDE](https://www.arduino.cc/en/software) (versi terbaru disarankan)
- Driver USB Serial sesuai mikrokontroler (CH340 / CP2102)
- Board Manager & Library terkait (misal: `ESP8266WiFi`, `Blynk`, `DHT sensor library`)

### 2. Langkah Instalasi

1. **Clone Repositori**
   ```bash
   git clone https://github.com/AndriantoEvan/irigasi-otomatis.git
   cd irigasi-otomatis
   ```

2. **Buka Kode Program**
   Buka file `.ino` utama menggunakan Arduino IDE.

3. **Pengaturan Konfigurasi**
   Kustomisasi kredensial Wi-Fi, token API/IoT, dan batas kelembapan (*threshold*) pada kode program:
   ```cpp
   const char* ssid = "NAMA_WIFI_ANDA";
   const char* password = "PASSWORD_WIFI_ANDA";
   int thresholdDry = 300; // Sesuaikan dengan hasil kalibrasi sensor
   ```

4. **Upload Program**
   Sambungkan board ke PC/Laptop, pilih Board dan Port yang sesuai, lalu klik **Upload**.

---

## 📸 Skema Rangkaian (*Wiring Diagram*)

*(Anda dapat menambahkan gambar skema pinout/sirkuit di sini)*

```text
[ Soil Moisture Sensor ] ---> Analog Pin (A0)
[ Relay Module ]         ---> Digital Pin (D2) ---> [ Water Pump ]
[ DHT11 Sensor ]         ---> Digital Pin (D4)
```

---

## 🤝 Kontribusi

Kontribusi selalu terbuka! Jika Anda memiliki saran, perbaikan bug, atau penambahan fitur baru:

1. Fork repositori ini.
2. Buat branch fitur baru (`git checkout -b feature/FiturBaru`).
3. Commit perubahan Anda (`git commit -m 'Menambahkan FiturBaru'`).
4. Push ke branch tersebut (`git push origin feature/FiturBaru`).
5. Buat **Pull Request**.

---

## 📄 Lisensi

Proyek ini dilindungi di bawah lisensi [MIT](LICENSE) - lihat file LICENSE untuk detail lebih lanjut.

---

### 👨‍💻 Pengembang
Dibuat oleh [Andrianto Evan](https://github.com/AndriantoEvan).
