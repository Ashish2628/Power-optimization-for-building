#include <ESP8266WiFi.h>
#include <espnow.h>
#include <FS.h>
#include <Updater.h>

#define SENSOR_ID_PIR 1001
#define FIRMWARE_FILE "/firmware.bin"
#define CHUNK_SIZE 240  // ESP-NOW max payload size

int pirPin = D1;

uint8_t receiverAddress[] ={0x24, 0xDC, 0xC3, 0xAE, 0x8B, 0x2C};  // Receiver MAC address

typedef struct {
    char sensorType[10];  // Sensor type (e.g., LDR, DHT)
    int numValues;        // Number of values sent
    float values[2];      // Sensor data values
    char sensorID[20];    // Unique sensor ID
} SensorData;

SensorData pirData;

size_t firmwareSize = 0; 
size_t totalPackets = 0;
size_t receivedPackets = 0;
bool updateReady = false;
File firmwareFile;
char FIRMWARE_VERSION[10] = "1.0.0";  // Fixed 10-byte array
uint8_t metadata[12];
uint8_t ackMessage = 1;  // Simple ACK response

void OnDataSent(uint8_t *mac_addr, uint8_t sendStatus) {
    Serial.println(sendStatus == 0 ? "Packet sent successfully" : "Failed to send packet");
}

void OnDataRecv(uint8_t *mac, uint8_t *incomingData, uint8_t len) {
    if (len < 4) return;
    if (memcmp(mac, receiverAddress, 6) != 0) return;

    // 🔹 If master requests firmware version
    if (strncmp((char*)incomingData, "checkversion", len) == 0) {
        Serial.printf("Sending firmware version: '%s' (size: %d bytes)\n", FIRMWARE_VERSION, sizeof(FIRMWARE_VERSION));
        esp_now_send(mac, (uint8_t*)FIRMWARE_VERSION, sizeof(FIRMWARE_VERSION));  
        return;
    }

    // 🔹 If receiving metadata (firmware size + total packets)
    if (len == sizeof(metadata)) {  
        uint32_t receivedMagic;
        memcpy(&receivedMagic, incomingData, 4);

        if (receivedMagic == 0xABCD1234) {  // Validate magic header
            memcpy(&firmwareSize, incomingData + 4, 4);
            memcpy(&totalPackets, incomingData + 8, 4);
            receivedPackets = 0;  // Reset received packets counter

            Serial.println("🚀 START PACKET RECEIVED!");
            Serial.printf("Firmware Size: %u bytes\n", firmwareSize);
            Serial.printf("Total Packets: %u\n", totalPackets); 

            // Open firmware file for writing (overwrite any existing one)
            firmwareFile = SPIFFS.open(FIRMWARE_FILE, "w");
            if (!firmwareFile) {
                Serial.println("❌ ERROR: Cannot open firmware file for writing!");
                return;
            }
            Serial.println("✅ Firmware file opened for writing.");

            esp_now_send(mac, &ackMessage, sizeof(ackMessage));  // Send ACK
            return;
        }
    }

    // 🔹 Read packet index
    uint32_t packetIndex;
    memcpy(&packetIndex, incomingData, 4);
    size_t dataSize = len - 4;

    // 🔹 If end signal is received
    if (packetIndex == 0xFFFFFFFF) {
        Serial.println("✅ Firmware transfer complete. Ready for update.");
        updateReady = true;
        
        if (firmwareFile) {
            firmwareFile.close();
            Serial.println("📁 Firmware file closed.");
        }

        esp_now_send(mac, &ackMessage, sizeof(ackMessage));  // Send ACK
        return;
    }

    // 🔹 If wrong packet sequence, ignore it
    if (packetIndex != receivedPackets) {
        Serial.printf("⚠️ Unexpected packet %d, expected %d. Ignoring...\n", packetIndex, receivedPackets);
        return;
    }

    // 🔹 Write firmware data to SPIFFS
    firmwareFile.write(incomingData + 4, dataSize);
    receivedPackets++;

    Serial.printf("✅ Received packet %d, size: %d\n", packetIndex, dataSize);
    esp_now_send(mac, &ackMessage, sizeof(ackMessage));  // Send ACK
}

bool applyFirmwareUpdate() {
    // 🔹 Check if firmware file exists
    if (!SPIFFS.exists(FIRMWARE_FILE)) {
        Serial.println("❌ ERROR: Firmware file does not exist!");
        return false;
    }

    // 🔹 Open firmware file for reading
    File firmwareFile = SPIFFS.open(FIRMWARE_FILE, "r");
    if (!firmwareFile) {
        Serial.println("❌ ERROR: Failed to open firmware file!");
        return false;
    }

    size_t firmwareSize = firmwareFile.size();
    Serial.printf("🔄 Applying update, size: %d bytes\n", firmwareSize);

    // 🔹 Begin update process
    if (!Update.begin(firmwareSize)) {
        Serial.println("❌ ERROR: Update failed to start!");
        return false;
    }

    size_t written = Update.writeStream(firmwareFile);
    firmwareFile.close();

    // 🔹 Validate firmware write
    if (written != firmwareSize) {
        Serial.println("❌ ERROR: Firmware write failed!");
        return false;
    }

    if (!Update.end()) {
        Serial.println("❌ ERROR: Update error!");
        return false;
    }

    Serial.println("✅ Update successful! Restarting...");
    ESP.restart();
    return true;
}

void setup() {
  Serial.begin(115200);
  WiFi.mode(WIFI_STA);
  pinMode(pirPin, INPUT);

  if (!SPIFFS.begin()) {
        Serial.println("❌ SPIFFS mount failed! Formatting...");
        SPIFFS.format();
        if (!SPIFFS.begin()) {
            Serial.println("❌ SPIFFS mount failed again! Aborting.");
            return;
        }
    }

  if (esp_now_init() != 0) {
    Serial.println("Error initializing ESP-NOW");
    return;
  }

  esp_now_set_self_role(ESP_NOW_ROLE_CONTROLLER);
  esp_now_register_send_cb(OnDataSent);
  esp_now_register_recv_cb(OnDataRecv);
  esp_now_add_peer(receiverAddress, ESP_NOW_ROLE_SLAVE, 1, NULL, 0);
}

void loop() {
  int value = digitalRead(pirPin);
  pirData.values[0] = value;
  strcpy(pirData.sensorType, "PIR");
  strcpy(pirData.sensorID, "PIR100112202428");
  pirData.numValues = 1;

  // Corrected esp_now_send function call
   esp_now_send(receiverAddress, (uint8_t *) &pirData, sizeof(pirData));


  Serial.print("Motion: ");
  Serial.println(value == HIGH ? "Detected" : "Not Detected");

  if (updateReady) {
        applyFirmwareUpdate();
    }

  delay(15000);
}
