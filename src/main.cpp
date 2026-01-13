#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <HardwareSerial.h>

// ================= 硬件定义 =================
// 请确保 PCB 上的 ESP32 引脚与这里一致
#define PIN_MFI_TX 21  // 连到 3998K 的 Pin 5 (RXD)
#define PIN_MFI_RX 20  // 连到 3998K 的 Pin 4 (TXD)

// 3998K 的通信波特率 (通常是 9600，如果地图不动，尝试 19200 或 115200)
#define MFI_BAUD_RATE 9600 

// ================= 蓝牙定义 =================
#define DEVICE_NAME         "IOS_Loc_Spoofer" // 蓝牙搜到的名字
#define SERVICE_UUID        "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CHARACTERISTIC_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a8"

// ================= 全局变量 =================
// 默认坐标：北京天安门 (防止一上电没数据)
double currentLat = 39.9088;
double currentLon = 116.3975;

HardwareSerial MfiSerial(1); // 使用 UART1 与 3998K 通信
unsigned long lastTime = 0;
bool deviceConnected = false;

// ================= 工具函数 =================

// 1. NMEA 异或校验和计算
String calculateChecksum(String sentence) {
  int checksum = 0;
  for (unsigned int i = 0; i < sentence.length(); i++) {
    checksum ^= sentence[i];
  }
  String hexC = String(checksum, HEX);
  if (hexC.length() < 2) hexC = "0" + hexC;
  hexC.toUpperCase();
  return hexC;
}

// 2. 将十进制坐标转为 NMEA 度分格式 (DDMM.MMMM)
String convertToNMEA(double val) {
  val = abs(val);
  int deg = (int)val;
  double min = (val - deg) * 60.0;
  char buf[20];
  sprintf(buf, "%02d%07.4f", deg, min);
  return String(buf);
}

// ================= 蓝牙回调类 =================
class MyServerCallbacks: public BLEServerCallbacks {
    void onConnect(BLEServer* pServer) {
      deviceConnected = true;
      Serial.println(">>> iPhone 已连接 BLE");
    };

    void onDisconnect(BLEServer* pServer) {
      deviceConnected = false;
      Serial.println(">>> iPhone 已断开，重新开始广播");
      BLEDevice::startAdvertising(); // 掉线后自动重连
    }
};

class MyCallbacks: public BLECharacteristicCallbacks {
    // 当手机 App 写入数据时触发
    void onWrite(BLECharacteristic *pCharacteristic) {
      std::string value = pCharacteristic->getValue();
      
      if (value.length() > 0) {
        String rxValue = String(value.c_str());
        Serial.print("收到坐标指令: ");
        Serial.println(rxValue);

        // 解析格式: "lat,lon" (例如: 40.75,-73.98)
        int commaIndex = rxValue.indexOf(',');
        if (commaIndex > 0) {
          String latStr = rxValue.substring(0, commaIndex);
          String lonStr = rxValue.substring(commaIndex + 1);
          
          currentLat = latStr.toDouble();
          currentLon = lonStr.toDouble();
          
          Serial.println(">>> 坐标已更新，即将注入 iPhone");
        }
      }
    }
};

// ================= 主程序 =================
void setup() {
  // 1. 初始化 USB 调试串口 (电脑看日志用)
  Serial.begin(115200);
  Serial.println("系统启动...");

  // 2. 初始化与 3998K 的通信串口
  // 注意：ESP32-C3 可以任意映射引脚
  MfiSerial.begin(MFI_BAUD_RATE, SERIAL_8N1, PIN_MFI_RX, PIN_MFI_TX);

  // 3. 初始化 BLE
  BLEDevice::init(DEVICE_NAME);
  BLEServer *pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());

  BLEService *pService = pServer->createService(SERVICE_UUID);

  // 创建特征值：允许读 (Read) 和 写 (Write)
  BLECharacteristic *pCharacteristic = pService->createCharacteristic(
                                         CHARACTERISTIC_UUID,
                                         BLECharacteristic::PROPERTY_READ |
                                         BLECharacteristic::PROPERTY_WRITE
                                       );

  pCharacteristic->setCallbacks(new MyCallbacks());
  pCharacteristic->addDescriptor(new BLE2902());

  pService->start();

  // 4. 开始广播
  BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setScanResponse(true);
  pAdvertising->setMinPreferred(0x06);  // 兼容 iPhone 连接参数
  BLEDevice::startAdvertising();
  
  Serial.println("BLE 广播已启动，等待连接...");
}

void loop() {
  // 按照 1Hz (每秒一次) 的频率向 3998K 发送数据
  // 无论蓝牙是否连接，都要持续发送，保证 GPS 信号不断
  if (millis() - lastTime > 1000) {
    lastTime = millis();
    
    // 1. 准备数据
    String latN = convertToNMEA(currentLat);
    String lonN = convertToNMEA(currentLon);
    String ns = (currentLat >= 0) ? "N" : "S";
    String ew = (currentLon >= 0) ? "E" : "W";
    
    // 2. 生成 $GPGGA (定位信息)
    String rawGGA = "GPGGA,120001.00," + latN + "," + ns + "," + lonN + "," + ew + ",1,08,0.9,10.0,M,0.0,M,,";
    String nmeaGGA = "$" + rawGGA + "*" + calculateChecksum(rawGGA) + "\r\n";
    
    // 3. 生成 $GPRMC (推荐最小数据)
    String rawRMC = "GPRMC,120001.00,A," + latN + "," + ns + "," + lonN + "," + ew + ",0.00,0.00,010124,,,A";
    String nmeaRMC = "$" + rawRMC + "*" + calculateChecksum(rawRMC) + "\r\n";

    // 4. 发送给 3998K (注入 iPhone)
    MfiSerial.print(nmeaGGA);
    MfiSerial.print(nmeaRMC);
    
    // 5. 调试输出
    // Serial.print("Sent: "); Serial.print(nmeaGGA); // 如果觉得串口太吵可以注释掉
  }
}