/**
 * iPhone 15 USB-C 改定位尾插固件 (ESP32-C3 版)
 * 适配原理图：IO6 -> 3998K RXD, IO7 -> 3998K TXD
 */

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <HardwareSerial.h>

// ================= 硬件引脚定义 (根据您的原理图) =================
// ESP32 IO6 (Pin 20) -> 连接到 3998K Pin 5 (RXD) -> 这是发送脚
#define PIN_MFI_TX 6 

// ESP32 IO7 (Pin 21) -> 连接到 3998K Pin 4 (TXD) -> 这是接收脚
#define PIN_MFI_RX 7  

// 3998K 芯片波特率
// 注意：绝大多数此类芯片默认是 9600。
// 如果烧录后地图不动，请尝试修改为 19200 或 115200 重新烧录
#define MFI_BAUD_RATE 9600 

// ================= 蓝牙配置 =================
#define DEVICE_NAME         "IOS_Location_Pro"
#define SERVICE_UUID        "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CHARACTERISTIC_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a8"

// ================= 全局变量 =================
// 📍 修改默认坐标：北京朝阳区望京 SOHO
double currentLat = 40.0003;
double currentLon = 116.4814;

// 定义第二个硬件串口，用于和 3998K 通信
HardwareSerial MfiSerial(1);

bool deviceConnected = false;
unsigned long lastTime = 0;

// ================= 工具函数算法 =================

// 1. 计算 NMEA 异或校验和
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

// 2. 将十进制坐标转换为 NMEA 度分格式 (DDMM.MMMM)
// 例如: 39.9088 -> 3954.5280
String convertToNMEA(double val) {
  val = abs(val);
  int deg = (int)val;
  double min = (val - deg) * 60.0;
  char buf[20];
  // 格式化为: 2位度数 + 2位整数分 + . + 4位小数分
  sprintf(buf, "%02d%07.4f", deg, min);
  return String(buf);
}

// ================= 蓝牙回调逻辑 =================
class MyServerCallbacks: public BLEServerCallbacks {
    void onConnect(BLEServer* pServer) {
      deviceConnected = true;
      Serial.println(">>> 蓝牙已连接");
    };

    void onDisconnect(BLEServer* pServer) {
      deviceConnected = false;
      Serial.println(">>> 蓝牙断开，重启广播...");
      BLEDevice::startAdvertising(); // 断开后必须重启广播，否则搜不到
    }
};

class MyCallbacks: public BLECharacteristicCallbacks {
    // 当手机 App 写入新坐标时触发
    void onWrite(BLECharacteristic *pCharacteristic) {
      std::string value = pCharacteristic->getValue();
      
      if (value.length() > 0) {
        String rxValue = String(value.c_str());
        Serial.printf("收到指令: %s\n", rxValue.c_str());

        // 解析格式 "lat,lon" (例如: 40.7580,-73.9855)
        int commaIndex = rxValue.indexOf(',');
        if (commaIndex > 0) {
          String latStr = rxValue.substring(0, commaIndex);
          String lonStr = rxValue.substring(commaIndex + 1);
          
          currentLat = latStr.toDouble();
          currentLon = lonStr.toDouble();
          
          Serial.println(">>> 坐标更新成功！");
        }
      }
    }
};

// ================= 初始化 =================
void setup() {
  // 1. 初始化调试串口 (通过烧录排针连接电脑)
  Serial.begin(115200);
  Serial.println("\n\n=== iPhone 改定位尾插系统启动 ===");

  // 2. 初始化 3998K 通信串口
  // 参数: 波特率, 数据位, RX引脚(IO7), TX引脚(IO6)
  MfiSerial.begin(MFI_BAUD_RATE, SERIAL_8N1, PIN_MFI_RX, PIN_MFI_TX);
  Serial.printf("3998K 串口已初始化: TX=IO%d, RX=IO%d, Baud=%d\n", PIN_MFI_TX, PIN_MFI_RX, MFI_BAUD_RATE);

  // 3. 初始化 BLE
  BLEDevice::init(DEVICE_NAME);
  BLEServer *pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());

  BLEService *pService = pServer->createService(SERVICE_UUID);

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
  pAdvertising->setMinPreferred(0x06); 
  BLEDevice::startAdvertising();
  
  Serial.println("BLE 广播中，请使用 LightBlue 连接...");
}

// ================= 主循环 =================
void loop() {
  // 按照 1Hz (每秒一次) 的频率向 iPhone 注入数据
  if (millis() - lastTime > 1000) {
    lastTime = millis();
    
    // 1. 准备数据
    String latN = convertToNMEA(currentLat);
    String lonN = convertToNMEA(currentLon);
    String ns = (currentLat >= 0) ? "N" : "S";
    String ew = (currentLon >= 0) ? "E" : "W";
    
    // 2. 生成 $GPGGA (定位数据)
    // 格式: $GPGGA,时间,纬度,N,经度,E,质量(1),卫星(8),精度(0.9),海拔(10.0M)...
    String rawGGA = "GPGGA,120000.00," + latN + "," + ns + "," + lonN + "," + ew + ",1,08,0.9,10.0,M,0.0,M,,";
    String packetGGA = "$" + rawGGA + "*" + calculateChecksum(rawGGA) + "\r\n";
    
    // 3. 生成 $GPRMC (推荐最小数据)
    String rawRMC = "GPRMC,120000.00,A," + latN + "," + ns + "," + lonN + "," + ew + ",0.00,0.00,010124,,,A";
    String packetRMC = "$" + rawRMC + "*" + calculateChecksum(rawRMC) + "\r\n";

    // 4. 发送给 3998K (注入 iPhone)
    MfiSerial.print(packetGGA);
    MfiSerial.print(packetRMC);
    
    // 5. 调试打印 (在电脑串口监视器查看)
    Serial.print("[Sent to iPhone]: ");
    Serial.print(packetGGA); // 只打印一行意思一下
  }
}