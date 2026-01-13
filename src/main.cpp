#include <Arduino.h>
#include <BluetoothSerial.h> // 必须用这个库

// 蓝牙名字
#define DEVICE_NAME "ESP32_GPS_Classic" 

BluetoothSerial SerialBT;

// 默认坐标：北京天安门
double currentLat = 39.9088; 
double currentLon = 116.3975;
unsigned long lastTime = 0;

// 校验和计算
String calculateChecksum(String sentence) {
  int checksum = 0;
  for (int i = 0; i < sentence.length(); i++) checksum ^= sentence[i];
  String hexC = String(checksum, HEX);
  if (hexC.length() < 2) hexC = "0" + hexC;
  hexC.toUpperCase();
  return hexC;
}

// 坐标转换
String convertToNMEA(double val) {
  val = abs(val);
  int deg = (int)val;
  double min = (val - deg) * 60.0;
  char buf[20];
  sprintf(buf, "%02d%07.4f", deg, min);
  return String(buf);
}

void setup() {
  Serial.begin(115200);
  
  // 启动经典蓝牙模式
  // 这种模式下，你必须先在手机系统设置里配对！
  if(!SerialBT.begin(DEVICE_NAME)) {
    Serial.println("启动失败！如果你的芯片是 C3/S3，不支持此模式。");
    while(1);
  }
  
  Serial.println("经典蓝牙已启动！");
  Serial.println("请现在去手机【设置 -> 蓝牙】里配对: " DEVICE_NAME);
}

void loop() {
  // 1. 接收电脑串口指令修改坐标 (格式: lat,lon)
  if (Serial.available()) {
    String input = Serial.readStringUntil('\n');
    int commaIndex = input.indexOf(',');
    if (commaIndex > 0) {
      currentLat = input.substring(0, commaIndex).toDouble();
      currentLon = input.substring(commaIndex + 1).toDouble();
      Serial.print("坐标更新为: ");
      Serial.print(currentLat);
      Serial.print(",");
      Serial.println(currentLon);
    }
  }

  // 2. 每秒发送 NMEA 数据给手机
  if (millis() - lastTime > 1000) {
    lastTime = millis();

    String latN = convertToNMEA(currentLat);
    String lonN = convertToNMEA(currentLon);
    String ns = (currentLat >= 0) ? "N" : "S";
    String ew = (currentLon >= 0) ? "E" : "W";

    // 必须加 \r\n，否则手机不识别
    String rawGGA = "GPGGA,120005.00," + latN + "," + ns + "," + lonN + "," + ew + ",1,08,0.9,10.0,M,0.0,M,,";
    String nmeaGGA = "$" + rawGGA + "*" + calculateChecksum(rawGGA) + "\r\n";

    String rawRMC = "GPRMC,120005.00,A," + latN + "," + ns + "," + lonN + "," + ew + ",0.00,0.00,010124,,,A";
    String nmeaRMC = "$" + rawRMC + "*" + calculateChecksum(rawRMC) + "\r\n";

    // 通过蓝牙发给手机
    SerialBT.print(nmeaGGA);
    SerialBT.print(nmeaRMC);
    
    // 串口同步打印，方便你看
    Serial.print(nmeaGGA);
  }
}