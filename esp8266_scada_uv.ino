/*
 * ==============================================================================
 *  PROYEK SISTEM SCADA IoT — MONITORING INTENSITAS CAHAYA UV (ESP8266 + LDR)
 *  
 *  Konfigurasi ADC 10-Bit ESP8266 (0 - 1023):
 *   - ADC >= 750        : BAHAYA     (UV Sangat Kurang / Kritis)
 *   - 650 <= ADC < 750  : PERINGATAN (UV Kurang dari Standar)
 *   - ADC < 650         : AMAN       (UV Optimal / Sesuai Acuan)
 * ==============================================================================
 */

#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecure.h>
#include <time.h>

// ==============================================================================
// 1. KONFIGURASI JARINGAN WiFi
// ==============================================================================
const char* WIFI_SSID       = "py";        // Ganti dengan SSID WiFi Anda
const char* WIFI_PASSWORD   = "11111111";  // Ganti dengan Password WiFi Anda

// ==============================================================================
// 2. KONFIGURASI REPOSITORI GITHUB & TELEMETRI SCADA
// ==============================================================================
const bool  ENABLE_GITHUB_LOG = true;
const char* GITHUB_OWNER      = "byyaja";  // Username Akun GitHub
const char* GITHUB_REPO       = "SCADA";   // Nama Repositori GitHub Baru (SCADA)

/*
 * PENTING KEAMANAN GITHUB:
 * Masukkan token GitHub Anda di bawah ini hanya di komputer lokal saat hendak flash!
 * JANGAN commit / push file yang berisi token asli ke GitHub publik.
 */
const char* GITHUB_TOKEN      = ""; 

// ==============================================================================
// 3. PARAMETER SENSOR LDR (RENTANG 10-BIT ADC: 0 - 1023)
// ==============================================================================
#define LDR_PIN         A0        // Pin analog ESP8266 (A0, rentang native 0 - 1023)
#define LED_INDICATOR   2         // LED onboard ESP8266 (GPIO2, Active LOW)
const char* SENSOR_LOCATION = "Ruang Sterilisasi UV"; // Nama Node / Lokasi SCADA

// Ambang Batas Acuan SCADA (ADC 10-Bit Native):
const int BATAS_BAHAYA     = 750; // ADC >= 750        : BAHAYA (UV Sangat Kurang)
const int BATAS_PERINGATAN = 650; // 650 <= ADC < 750  : PERINGATAN (UV Kurang)
                                  // ADC < 650         : AMAN (UV Optimal)

// ==============================================================================
// 4. TIMING PENGUKURAN & INTERVAL SCADA (MONITORING KONTINU TANPA BATAS WAKTU)
// ==============================================================================
const unsigned long INTERVAL_BACA = 15000;   // Interval pembacaan (15 detik)
unsigned long lastReadTime  = 0;
bool firstRead              = true;

// Edge Detection: Mencegah spam pesan Telegram
bool statusAlarmActive      = false;

// Counter Telemetri SCADA
int totalAkuisisi  = 0;
int totalAman      = 0;
int totalPeringatan= 0;
int totalBahaya    = 0;

// ==============================================================================
// 5. SINKRONISASI WAKTU NTP
// ==============================================================================
const char* ntpServer      = "pool.ntp.org";
const long  gmtOffset      = 28800; // GMT+8 WITA (25200 untuk GMT+7 WIB)
const int   daylightOffset = 0;

// ==============================================================================
// DEKLARASI FUNGSI PROTOTYPE
// ==============================================================================
void connectWiFi();
int readSensorLDR();
String klasifikasiKondisiUV(int nilaiLDR);
String getStatusLabel(int nilaiLDR);
bool cekKondisiAlarm(int nilaiLDR);
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
  Serial.println("║       ESP8266 ADC 10-BIT (0-1023) + SENSOR CAHAYA LDR      ║");
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
  String statusAwal = getStatusLabel(nilaiAwal);
  Serial.printf("📊 Pembacaan Awal Sensor ADC (10-bit): %d | Status: %s | Kondisi: %s\n", 
    nilaiAwal, statusAwal.c_str(), kondisiAwal.c_str());

  Serial.println("✅ Sistem SCADA Siap beroperasi! Mengirim telemetri secara kontinu...\n");
}

// ==============================================================================
// LOOP UTAMA SCADA (KONTINU / REAL-TIME MONITORING TANPA BATAS WAKTU)
// ==============================================================================
void loop() {
  // Jaga koneksi WiFi tetap terhubung
  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
  }

  // Eksekusi non-blocking akuisisi data sensor
  unsigned long currentTime = millis();
  if (firstRead || (currentTime - lastReadTime >= INTERVAL_BACA)) {
    firstRead = false;
    lastReadTime = currentTime;

    // 1. Baca nilai intensitas LDR murni 10-bit (rentang 0 - 1023)
    int nilaiLDR = readSensorLDR();
    totalAkuisisi++;

    // 2. Evaluasi status & kondisi UV berdasarkan batas 650 & 750
    String kondisi = klasifikasiKondisiUV(nilaiLDR);
    String statusLabel = getStatusLabel(nilaiLDR);
    bool isAlarm = cekKondisiAlarm(nilaiLDR);

    // Update Counter & Kontrol LED Indikator
    if (nilaiLDR >= BATAS_BAHAYA) {
      totalBahaya++;
      digitalWrite(LED_INDICATOR, LOW); // LED ON penuh
    } else if (nilaiLDR >= BATAS_PERINGATAN) {
      totalPeringatan++;
      digitalWrite(LED_INDICATOR, LOW); // LED ON
    } else {
      totalAman++;
      digitalWrite(LED_INDICATOR, HIGH); // LED OFF
    }

    // 3. Logika Notifikasi Telegram (Hanya dikirim saat transisi masuk ke Peringatan/Bahaya)
    bool triggerTelegram = false;
    if (isAlarm && !statusAlarmActive) {
      statusAlarmActive = true;
      triggerTelegram = true;
      Serial.printf("🚨 [ALARM SCADA] INTENSITAS UV KURANG! Status: %s (ADC: %d)\n", statusLabel.c_str(), nilaiLDR);
      Serial.println("📱 Mengirim sinyal notifikasi Telegram...");
    } else if (!isAlarm && statusAlarmActive) {
      statusAlarmActive = false;
      Serial.println("✅ [SCADA] Intensitas cahaya UV telah kembali ke ambang AMAN / OPTIMAL.");
    }

    // 4. Tampilkan status telemetri di Serial Monitor
    Serial.println("────────────────────────────────────────────────────────────");
    Serial.printf("📡 [SCADA Telemetri #%d] %s\n", totalAkuisisi, getWaktuFormatted().c_str());
    Serial.printf("💡 Intensitas ADC (10-Bit) : %d (Batas: Peringatan >= 650, Bahaya >= 750)\n", nilaiLDR);
    Serial.printf("📊 Kondisi Radiasi UV       : %s\n", kondisi.c_str());
    Serial.printf("🛡️ Status SCADA             : %s\n", statusLabel.c_str());
    Serial.printf("📈 Aman: %d | Peringatan: %d | Bahaya: %d\n", totalAman, totalPeringatan, totalBahaya);

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
// FUNGSI BACA SENSOR LDR (NATIVE 10-BIT ADC: 0 - 1023)
// ==============================================================================
int readSensorLDR() {
  // ESP8266 memiliki 1 pin ADC (A0) dengan resolusi native 10-bit (0 - 1023)
  int rawADC = analogRead(LDR_PIN);
  return rawADC;
}

// ==============================================================================
// KLASIFIKASI KONDISI UV BERDASARKAN AMBANG BATAS
// ==============================================================================
String klasifikasiKondisiUV(int nilaiLDR) {
  if (nilaiLDR >= BATAS_BAHAYA) {
    return "UV Sangat Kurang";
  } else if (nilaiLDR >= BATAS_PERINGATAN) {
    return "UV Kurang";
  } else {
    return "UV Optimal";
  }
}

// ==============================================================================
// LABEL STATUS SCADA (BAHAYA, PERINGATAN, AMAN)
// ==============================================================================
String getStatusLabel(int nilaiLDR) {
  if (nilaiLDR >= BATAS_BAHAYA) {
    return "BAHAYA";
  } else if (nilaiLDR >= BATAS_PERINGATAN) {
    return "PERINGATAN";
  } else {
    return "AMAN";
  }
}

// ==============================================================================
// EVALUASI KONDISI ALARM (TRUE JIKA >= 650)
// ==============================================================================
bool cekKondisiAlarm(int nilaiLDR) {
  // Peringatan / Alarm aktif jika intensitas UV di bawah batas acuan (ADC >= 650)
  return (nilaiLDR >= BATAS_PERINGATAN);
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
