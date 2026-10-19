/*
 * ==============================================================================
 *  PROYEK SISTEM SCADA IoT — MONITORING INTENSITAS CAHAYA UV (ESP8266 + SENSOR LDR)
 *  
 *  Fitur:
 *   1. Akuisisi Data Intensitas Radiasi Cahaya UV via Sensor LDR (Pin Analog A0)
 *   2. Klasifikasi Kondisi UV Multi-Level (Berdasarkan Referensi Jurnal)
 *   3. Deteksi Alarm / Peringatan Otomatis saat Intensitas UV KURANG
 *   4. Pengiriman Telemetri ke GitHub API (Repository Dispatch)
 *   5. Notifikasi Otomatis ke Telegram via GitHub Actions Workflow
 *   6. Edge Detection Anti-Spam (Hanya kirim Telegram saat status masuk ke BAHAYA)
 * ==============================================================================
 */

#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecure.h>
#include <time.h>

// ==============================================================================
// 1. KONFIGURASI JARINGAN WiFi
// ==============================================================================
const char* WIFI_SSID       = "Byy";       // Ganti dengan SSID WiFi Anda
const char* WIFI_PASSWORD   = "buyyyyyy";  // Ganti dengan Password WiFi Anda

// ==============================================================================
// 2. KONFIGURASI REPOSITORI GITHUB & TELEMETRI SCADA
// ==============================================================================
const bool  ENABLE_GITHUB_LOG = true;
const char* GITHUB_OWNER      = "byyaja";  // Username Akun GitHub
const char* GITHUB_REPO       = "unyil";   // Nama Repositori GitHub

/*
 * PENTING KEAMANAN GITHUB:
 * Masukkan token GitHub Anda di bawah ini hanya di komputer lokal saat hendak flash!
 * JANGAN commit / push file yang berisi token asli ke GitHub publik,
 * karena GitHub Secret Scanning akan otomatis mematikan (revoke) token tersebut!
 */
const char* GITHUB_TOKEN      = "ghp_MASUKKAN_TOKEN_BARU_ANDA_DISINI"; 

// ==============================================================================
// 3. PARAMETER SENSOR LDR & AMBANG BATAS UV (REFERENSI JURNAL)
// ==============================================================================
#define LDR_PIN         A0        // Pin analog ESP8266 (A0, rentang ADC 0 - 1023)
#define LED_INDICATOR   2         // LED onboard ESP8266 (GPIO2, Active LOW)
const char* SENSOR_LOCATION = "Ruang Sterilisasi UV"; // Nama Node / Lokasi SCADA

/*
 * CATATAN PENGATURAN REFERENSI JURNAL:
 * Nilai analog dibaca dari A0 dan di-scale ke resolusi 12-bit (0 - 4095).
 * Pada rangkaian voltage divider standar (LDR ke GND, Resistor ke VCC):
 *   - Cahaya/UV Terang  -> Resistansi LDR Kecil -> Nilai ADC RENDAH
 *   - Cahaya/UV Kurang  -> Resistansi LDR Besar -> Nilai ADC TINGGI
 * 
 * Silakan sesuaikan angka ambang batas berikut sesuai tabel jurnal yang Anda temukan:
 */
const int BATAS_UV_SANGAT_KURANG = 3700; // ADC >= 3700 : UV Sangat Kurang (BAHAYA)
const int BATAS_UV_KURANG        = 2500; // ADC >= 2500 : UV Kurang (PERINGATAN)
const int BATAS_UV_CUKUP         = 1200; // ADC >= 1200 : UV Cukup (AMAN)
                                         // ADC <  1200 : UV Optimal / Sangat Baik (AMAN)

// ==============================================================================
// 4. TIMING PENGUKURAN & INTERVAL SCADA
// ==============================================================================
// Contoh: Interval pembacaan 10 menit = 600.000 ms. (Untuk tes cepat bisa diganti 10.000 ms)
const unsigned long INTERVAL_BACA = 10000;   // 10 detik untuk uji coba (ganti 600000 untuk 10 menit)
const unsigned long DURASI_TOTAL  = 43200000; // 12 Jam (43.200.000 ms)

unsigned long lastReadTime  = 0;
unsigned long startTime     = 0;
bool firstRead              = true;

// Edge Detection: Mencegah spam pesan Telegram
bool statusBahayaActive     = false;

// Counter Telemetri SCADA
int totalAkuisisi  = 0;
int totalAman      = 0;
int totalPeringatan= 0;

// ==============================================================================
// 5. SINKRONISASI WAKTU NTP
// ==============================================================================
const char* ntpServer      = "pool.ntp.org";
const long  gmtOffset      = 28800; // GMT+8 WITA (atau sesuaikan: 25200 untuk GMT+7 WIB)
const int   daylightOffset = 0;

// ==============================================================================
// DEKLARASI FUNGSI PROTOTYPE
// ==============================================================================
void connectWiFi();
int readSensorLDR();
String klasifikasiKondisiUV(int nilaiLDR);
bool cekKondisiBahaya(int nilaiLDR);
bool kirimTelemetriGitHub(String status, String kondisi, int nilaiLDR, bool triggerTelegram);
String getWaktuFormatted();

// ==============================================================================
// SETUP SISTEM SCADA
// ==============================================================================
void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println("\n╔════════════════════════════════════════════════════════════╗");
  Serial.println("║        SISTEM SCADA IoT — MONITOR INTENSITAS UV            ║");
  Serial.println("║               ESP8266 + SENSOR CAHAYA LDR                  ║");
  Serial.println("╚════════════════════════════════════════════════════════════╝");

  // Inisialisasi LED Indikator
  pinMode(LED_INDICATOR, OUTPUT);
  digitalWrite(LED_INDICATOR, HIGH); // LED mati (Active LOW)

  // Hubungkan ke WiFi
  connectWiFi();

  // Sinkronisasi Waktu NTP
  configTime(gmtOffset, daylightOffset, ntpServer);
  Serial.println("⏰ Melakukan sinkronisasi waktu NTP...");
  delay(2000);

  // Uji pembacaan sensor awal
  int nilaiAwal = readSensorLDR();
  String kondisiAwal = klasifikasiKondisiUV(nilaiAwal);
  Serial.printf("📊 Pembacaan Awal Sensor: %d | Kondisi: %s\n", nilaiAwal, kondisiAwal.c_str());

  startTime = millis();
  Serial.println("✅ Sistem SCADA Siap beroperasi! Mengirim telemetri berkala...\n");
}

// ==============================================================================
// LOOP UTAMA SCADA
// ==============================================================================
void loop() {
  // Cek apakah durasi monitoring 12 jam telah selesai
  if (millis() - startTime >= DURASI_TOTAL) {
    Serial.println("\n⏱️ [SCADA] Sesi monitoring 12 jam telah selesai.");
    Serial.printf("  Total Pembacaan : %d\n", totalAkuisisi);
    Serial.printf("  Kondisi Aman    : %d\n", totalAman);
    Serial.printf("  Kondisi Kurang  : %d\n", totalPeringatan);
    digitalWrite(LED_INDICATOR, HIGH);

    while (true) {
      delay(10000);
      yield();
    }
  }

  // Jaga koneksi WiFi tetap terhubung
  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
  }

  // Eksekusi non-blocking akuisisi data sensor
  unsigned long currentTime = millis();
  if (firstRead || (currentTime - lastReadTime >= INTERVAL_BACA)) {
    firstRead = false;
    lastReadTime = currentTime;

    // 1. Baca nilai intensitas LDR (skala 0 - 4095)
    int nilaiLDR = readSensorLDR();
    totalAkuisisi++;

    // 2. Evaluasi kondisi UV berdasarkan ambang batas jurnal
    String kondisi = klasifikasiKondisiUV(nilaiLDR);
    bool bahaya = cekKondisiBahaya(nilaiLDR);
    String statusLabel = bahaya ? "BAHAYA" : "AMAN";

    if (bahaya) {
      totalPeringatan++;
      digitalWrite(LED_INDICATOR, LOW); // Nyalakan LED peringatan
    } else {
      totalAman++;
      digitalWrite(LED_INDICATOR, HIGH); // Matikan LED
    }

    // 3. Logika Notifikasi Telegram (Hanya dikirim saat transisi ke UV Kurang/Bahaya)
    bool triggerTelegram = false;
    if (bahaya && !statusBahayaActive) {
      statusBahayaActive = true;
      triggerTelegram = true;
      Serial.println("🚨 [ALARM SCADA] INTENSITAS CAHAYA UV KURANG DARI AMBANG BATAS!");
      Serial.println("📱 Mengirim sinyal darurat notifikasi Telegram...");
    } else if (!bahaya && statusBahayaActive) {
      statusBahayaActive = false;
      Serial.println("✅ [SCADA] Intensitas cahaya UV telah kembali ke batas aman.");
    }

    // 4. Tampilkan status telemetri di Serial Monitor
    Serial.println("────────────────────────────────────────────────────────────");
    Serial.printf("📡 [SCADA Telemetri #%d] %s\n", totalAkuisisi, getWaktuFormatted().c_str());
    Serial.printf("💡 Intensitas ADC : %d\n", nilaiLDR);
    Serial.printf("📊 Kondisi UV     : %s\n", kondisi.c_str());
    Serial.printf("🛡️ Status Sistem  : %s\n", statusLabel.c_str());
    Serial.printf("📈 Aman: %d | Kurang/Bahaya: %d\n", totalAman, totalPeringatan);

    // 5. Kirim telemetri ke repositori GitHub
    if (ENABLE_GITHUB_LOG) {
      bool sent = kirimTelemetriGitHub(statusLabel, kondisi, nilaiLDR, triggerTelegram);
      Serial.println(sent ? "✅ [GitHub SCADA] Telemetri tersimpan." : "❌ [GitHub SCADA] Gagal simpan telemetri.");
    }
    Serial.println("────────────────────────────────────────────────────────────\n");
  }

  yield(); // Menjaga stabilitas Watchdog ESP8266
}

// ==============================================================================
// FUNGSI AKUISISI & KALIBRASI SENSOR LDR
// ==============================================================================
int readSensorLDR() {
  int rawADC = analogRead(LDR_PIN); // Rentang native ESP8266: 0 - 1023
  // Konversi / mapping ke skala 12-bit (0 - 4095) untuk keselarasan dengan parameter SCADA
  int scaledADC = map(rawADC, 0, 1023, 0, 4095);
  return scaledADC;
}

// ==============================================================================
// KLASIFIKASI KONDISI UV BERDASARKAN AMBANG BATAS JURNAL
// ==============================================================================
String klasifikasiKondisiUV(int nilaiLDR) {
  if (nilaiLDR >= BATAS_UV_SANGAT_KURANG) {
    return "UV Sangat Kurang";
  } else if (nilaiLDR >= BATAS_UV_KURANG) {
    return "UV Kurang (Peringatan)";
  } else if (nilaiLDR >= BATAS_UV_CUKUP) {
    return "UV Cukup";
  } else {
    return "UV Optimal";
  }
}

// ==============================================================================
// EVALUASI KONDISI ALARM / BAHAYA
// ==============================================================================
bool cekKondisiBahaya(int nilaiLDR) {
  // Peringatan aktif jika intensitas UV kurang dari ambang batas aman
  return (nilaiLDR >= BATAS_UV_KURANG);
}

// ==============================================================================
// PENGIRIMAN DATA TELEMETRI KE GITHUB API (REPOSITORY DISPATCH)
// ==============================================================================
bool kirimTelemetriGitHub(String status, String kondisi, int nilaiLDR, bool triggerTelegram) {
  WiFiClientSecure client;
  client.setInsecure(); // Bypass verifikasi sertifikat SSL

  HTTPClient http;
  String url = "https://api.github.com/repos/" + String(GITHUB_OWNER) + "/" + String(GITHUB_REPO) + "/dispatches";

  http.begin(client, url);
  http.addHeader("Authorization", String("token ") + GITHUB_TOKEN);
  http.addHeader("Accept", "application/vnd.github.v3+json");
  http.addHeader("Content-Type", "application/json");
  http.addHeader("User-Agent", "ESP8266-SCADA-UV-Client");

  String timestamp = getWaktuFormatted();

  // Payload data terstruktur untuk SCADA Log & Telegram Dispatch
  String payload = "{";
  payload += "\"event_type\":\"sensor_update\",";
  payload += "\"client_payload\":{";
  payload += "\"status\":\"" + status + "\",";
  payload += "\"kondisi\":\"" + kondisi + "\",";
  payload += "\"location\":\"" + String(SENSOR_LOCATION) + "\",";
  payload += "\"timestamp\":\"" + timestamp + "\",";
  payload += "\"ldr_value\":\"" + String(nilaiLDR) + "\",";
  payload += "\"trigger_telegram\":\"" + String(triggerTelegram ? "true" : "false") + "\"";
  payload += "}}";

  int httpCode = http.POST(payload);
  http.end();

  return (httpCode == 204); // GitHub API mengembalikan 204 jika event sukses dibuat
}

// ==============================================================================
// KONEKSI KE JARINGAN WiFi
// ==============================================================================
void connectWiFi() {
  Serial.printf("📶 Menghubungkan ke WiFi: %s ", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  int attempt = 0;
  while (WiFi.status() != WL_CONNECTED && attempt < 30) {
    delay(500);
    Serial.print(".");
    attempt++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println(" [TERHUBUNG]");
    Serial.print("📍 IP Address Node: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println(" [GAGAL]");
    Serial.println("⚠️ Mengulang dalam 5 detik...");
    delay(5000);
    ESP.restart();
  }
}

// ==============================================================================
// FORMATTING TIMESTAMP WAKTU
// ==============================================================================
String getWaktuFormatted() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    return "Waktu tidak sinkron";
  }
  char buffer[35];
  strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S WITA", &timeinfo);
  return String(buffer);
}
