#define BLYNK_TEMPLATE_ID "TMPL6ficqYq0G"
#define BLYNK_TEMPLATE_NAME "Iot"
#define BLYNK_AUTH_TOKEN "cw1H9OAdNKqtGUc7qn1yj-17ZB1NwFFT"
#define BLYNK_PRINT Serial
#include <ESP8266WiFi.h>
#include <BlynkSimpleEsp8266.h>
#include <HX711.h>
#include <Servo.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
// WiFi
char ssid[] = "An";
char pass[] = "12345678";
// Loadcell
#define DOUT D3
#define CLK  D4
HX711 scale;
float weight;
float targetWeight = 150.0;
const float WEIGHT_THRESHOLD = 50.0;
// Servo
Servo myServo;
#define SERVO_PIN D6
bool servoIsRunning = false;
bool systemEnabled = false;  // true: hoạt động bình thường, false: tắt toàn bộ xử lý tự động
bool servoLocked = false;
unsigned long servoLockStart = 0;
const unsigned long SERVO_LOCK_DURATION = 120000UL;
// Relay & cảm biến nước
const int relayPin = D5;
const int waterSensorAnalogPin = A0;
int analogValue;
float waterLevelPercent;
const unsigned long MAX_PUMP_RUN_TIME = 30000UL;
const float MIN_WATER_CHANGE = 1.0;
float minWaterThreshold = 225.0;
float maxWaterThreshold = 400.0;
//Nút điều khiển on/off
BLYNK_WRITE(V30) {
  systemEnabled = param.asInt();
  if (systemEnabled) {
    Serial.println("Hệ thống đã BẬT trở lại");
  } else {
    Serial.println("Hệ thống đã TẮT – bỏ qua toàn bộ xử lý tự động");
    digitalWrite(relayPin, LOW);     // Tắt bơm
    myServo.write(0);                // Đóng servo
  }
}
// Siêu âm
#define TRIG_FOOD D2
#define ECHO_FOOD D1
#define TRIG_WATER D7
#define ECHO_WATER D8
float distanceFood = -1, distanceWater = -1;
unsigned long foodCheckStartTime = 0, waterCheckStartTime = 0;
bool foodDistanceTooFar = false, waterDistanceTooFar = false;
const float DISTANCE_THRESHOLD = 6.0;
const unsigned long DISTANCE_TIMEOUT = 60000L;
// Lock bơm khi nước cạn
bool pumpLocked = false;
unsigned long pumpLockStart = 0;
const unsigned long PUMP_LOCK_DURATION = 120000UL; // 1 giờ
BlynkTimer timer;
// ====== NTP + Hẹn giờ ======
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 7 * 3600, 60000); // UTC+7
struct TimeSlot {
  int hour;
  int minute;
  bool enabled;
  bool eventTriggered;
};
TimeSlot timeList[5] = {{0,0,false,false},{0,0,false,false},{0,0,false,false},{0,0,false,false},{0,0,false,false}};
// ====== BLYNK INPUT ======
BLYNK_WRITE(V1) {
  targetWeight = param.asFloat();
  Serial.print("New TARGET_WEIGHT: ");
  Serial.println(targetWeight);
}
BLYNK_WRITE(V2) {
  minWaterThreshold = param.asFloat();
  Serial.print("Min Water Threshold: ");
  Serial.println(minWaterThreshold);
}
BLYNK_WRITE(V3) {
  maxWaterThreshold = param.asFloat();
  Serial.print("Max Water Threshold: ");
  Serial.println(maxWaterThreshold);
}
BLYNK_WRITE(V10) { long t = param[0].asLong(); timeList[0].hour = t / 3600; timeList[0].minute = (t % 3600) / 60; }
BLYNK_WRITE(V11) { long t = param[0].asLong(); timeList[1].hour = t / 3600; timeList[1].minute = (t % 3600) / 60; }
BLYNK_WRITE(V12) { long t = param[0].asLong(); timeList[2].hour = t / 3600; timeList[2].minute = (t % 3600) / 60; }
BLYNK_WRITE(V13) { long t = param[0].asLong(); timeList[3].hour = t / 3600; timeList[3].minute = (t % 3600) / 60; }
BLYNK_WRITE(V14) { long t = param[0].asLong(); timeList[4].hour = t / 3600; timeList[4].minute = (t % 3600) / 60; }
BLYNK_WRITE(V15) { timeList[0].enabled = param.asInt(); }
BLYNK_WRITE(V16) { timeList[1].enabled = param.asInt(); }
BLYNK_WRITE(V17) { timeList[2].enabled = param.asInt(); }
BLYNK_WRITE(V18) { timeList[3].enabled = param.asInt(); }
BLYNK_WRITE(V19) { timeList[4].enabled = param.asInt(); }
// ====== ĐỌC SIÊU ÂM ======
float readDistance(int trigPin, int echoPin) {
  digitalWrite(trigPin, LOW);
  delayMicroseconds(2);
  digitalWrite(trigPin, HIGH);
  delayMicroseconds(10);
  digitalWrite(trigPin, LOW);
  long duration = pulseIn(echoPin, HIGH, 30000);
  if (duration == 0) return -1;
  return duration * 0.034 / 2;
}
// ====== THỨC ĂN ======
void handleFoodLogic(unsigned long now) {
  if (!systemEnabled) return;
  weight = scale.get_units(5);
  distanceFood = readDistance(TRIG_FOOD, ECHO_FOOD);
  Serial.print("Weight: "); Serial.print(weight); Serial.println(" g");
  Blynk.virtualWrite(V4, weight);  // Gửi trọng lượng lên Blynk
  // Kiểm tra siêu âm (kho chứa thức ăn trống)
  if (distanceFood > 0) {
    Serial.print("Food Distance: "); Serial.print(distanceFood); Serial.println(" cm");
    if (distanceFood > DISTANCE_THRESHOLD) {
      if (foodCheckStartTime == 0) {
        foodCheckStartTime = now;
      } else if (now - foodCheckStartTime >= DISTANCE_TIMEOUT && !foodDistanceTooFar) {
        foodDistanceTooFar = true;
        servoLocked = true;
        servoLockStart = now;
        // QUAY VỀ VỊ TRÍ ĐÓNG KHI BỊ KHÓA
        myServo.write(0);
        servoIsRunning = false;
        Blynk.logEvent("food_empty", "Kho chứa thức ăn trống (siêu âm >10s)");
        Serial.println("ALERT: Hết thức ăn (siêu âm) – khóa servo");
      }
    } else {
      foodCheckStartTime = 0;
      foodDistanceTooFar = false;
    }
  }
  // Mở khóa servo sau 30 phút nếu bị khóa
 if (servoLocked && now - servoLockStart >= SERVO_LOCK_DURATION) {
    servoLocked = false;
    foodCheckStartTime = 0;  // Reset lại bộ đếm
    foodDistanceTooFar = false;
    Serial.println(" Servo được mở khóa sau 30 phút");
}
  // ======= MỞ SERVO CẤP THỨC ĂN DƯỚI NGƯỠNG WEIGHT_THRESHOLD =======
  if (!servoLocked && weight < WEIGHT_THRESHOLD) {
    if (!servoIsRunning) {
      myServo.write(120);  // Mở servo
      servoIsRunning = true;
      Serial.println("Đang mở servo để cấp thức ăn (vì nhỏ hơn WEIGHT_THRESHOLD)");
    }
  }
  else if (!servoLocked && weight >= targetWeight && servoIsRunning) {
    myServo.write(0);  // Đóng servo
    servoIsRunning = false;
    Serial.printf("Đã đạt trọng lượng mục tiêu: %.2fg – servo dừng\n", weight);
  }
  else if (!servoLocked && weight >= WEIGHT_THRESHOLD && !servoIsRunning) {
    Serial.printf("Không cấp thức ăn: trọng lượng hiện tại %.2fg ≥ WEIGHT_THRESHOLD %.2fg\n", weight, WEIGHT_THRESHOLD);
  }
  Blynk.virtualWrite(V5, distanceFood);  // Gửi khoảng cách đồ ăn lên Blynk
}
// ====== NƯỚC UỐNG ======
void handleWaterLogic(unsigned long now) {
  if (!systemEnabled) {
    digitalWrite(relayPin, LOW); // đảm bảo bơm tắt
    return;
  }

  analogValue = analogRead(waterSensorAnalogPin);
  Blynk.virtualWrite(V0, analogValue); // gửi giá trị analog lên Blynk

  Serial.print("Water Analog Raw: ");
  Serial.println(analogValue);

  // Đọc khoảng cách bằng siêu âm
  distanceWater = readDistance(TRIG_WATER, ECHO_WATER);
  if (distanceWater > 0) {
    Serial.print("Water Distance: "); Serial.print(distanceWater); Serial.println(" cm");

    if (distanceWater > DISTANCE_THRESHOLD) {
      if (waterCheckStartTime == 0) {
        waterCheckStartTime = now;
      } else if (now - waterCheckStartTime >= DISTANCE_TIMEOUT && !waterDistanceTooFar) {
        waterDistanceTooFar = true;
        pumpLocked = true;
        pumpLockStart = now;
        Blynk.logEvent("low_water1", "Bình nước trống (siêu âm >10s)");
        Serial.println("ALERT: Bình nước trống (siêu âm) – Bơm sẽ bị khóa 1 giờ");
      }
    } else {
      waterCheckStartTime = 0;
      waterDistanceTooFar = false;
    }
  }

  // Mở khóa sau 1 giờ nếu bị khóa
  if (pumpLocked && now - pumpLockStart >= PUMP_LOCK_DURATION) {
    pumpLocked = false;
    waterCheckStartTime = 0;
    waterDistanceTooFar = false;
    Serial.println("Bơm đã được mở khóa sau 1 giờ");
  }

  static bool shouldPump = false;

  if (pumpLocked) {
    digitalWrite(relayPin, LOW);
    Serial.println("Bơm bị khóa – không hoạt động");
    return;
  }

  // Sử dụng ngưỡng analog thay vì phần trăm
  if (analogValue < minWaterThreshold) {
    shouldPump = true;
    Serial.printf("Analog %d < minThreshold %.0f – bắt đầu bơm\n", analogValue, minWaterThreshold);
  }

  if (shouldPump) {
    if (analogValue < maxWaterThreshold) {
      digitalWrite(relayPin, HIGH);
      Serial.printf("Đang bơm... Analog %d < %.0f (max)\n", analogValue, maxWaterThreshold);
    } else {
      digitalWrite(relayPin, LOW);
      shouldPump = false;
      Serial.printf("Đã đạt ngưỡng nước: Analog %d ≥ %.0f – dừng bơm\n", analogValue, maxWaterThreshold);
    }
  } else {
    digitalWrite(relayPin, LOW);
    Serial.println("Mực nước ổn định – không cần bơm");
  }

  Blynk.virtualWrite(V6, distanceWater);  // Gửi khoảng cách nước
}


// ====== HẸN GIỜ ======
void handleTimedFeedingAndWatering() {
  if (!systemEnabled) return;
  timeClient.update();
  int currentHour = timeClient.getHours();
  int currentMinute = timeClient.getMinutes();
  for (int i = 0; i < 5; i++) {
    if (timeList[i].enabled &&
        timeList[i].hour == currentHour &&
        timeList[i].minute == currentMinute &&
        !timeList[i].eventTriggered) {
      Serial.printf("Hẹn giờ kích hoạt: %02d:%02d\n", currentHour, currentMinute);
      // ======= CẤP THỨC ĂN ĐẾN KHI ĐỦ =======
      weight = scale.get_units(5);
     if (!servoLocked && weight < targetWeight) {
        Serial.printf("Đang có %.2fg < %.2fg – bắt đầu quay servo\n", weight, targetWeight);
        const unsigned long maxFeedTime = 20000; // cấp thức ăn tối đa 20 giây
        unsigned long feedStart = millis();
        while (weight < targetWeight && millis() - feedStart < maxFeedTime) {
          myServo.write(100);
          delay(8000);
          myServo.write(0);
          delay(1000);
          weight = scale.get_units(5);
          Serial.printf("Đang cấp thức ăn... %.2fg\n", weight);
        }
        Serial.printf("Kết thúc cấp thức ăn: %.2fg\n", weight);
      } else {
        Blynk.logEvent("food_full","Thức ăn đã đầy không thể cấp thêm");
        Serial.printf("Thức ăn đã đủ: %.2fg ≥ %.2fg\n", weight, targetWeight);
      }
      // ======= BƠM NƯỚC ĐẾN KHI ĐỦ maxWaterThreshold =======
analogValue = analogRead(waterSensorAnalogPin);
if (!pumpLocked && analogValue < maxWaterThreshold) {
  Serial.printf("Bắt đầu bơm: %.1f%% < %.1f%% (mục tiêu)\n", waterLevelPercent, maxWaterThreshold);
  // Bật relay (bơm nước)
  digitalWrite(relayPin, HIGH);
  // Chờ đến khi mức nước đạt ngưỡng hoặc hết thời gian bơm tối đa
  unsigned long pumpStart = millis();
  const unsigned long maxPumpTime = 20000; // bơm tối đa 20 giây
  while (analogValue < maxWaterThreshold && millis() - pumpStart < maxPumpTime) {
    delay(1000); // Đợi 1 giây giữa các lần kiểm tra
    analogValue = analogRead(waterSensorAnalogPin);
    Serial.printf("Đang bơm... %.1f%%\n", analogValue);
  }
  // Tắt relay (bơm nước)
  digitalWrite(relayPin, LOW);
  Serial.printf("Mực nước sau khi bơm: %.1f%%\n", waterLevelPercent);
} else if (pumpLocked) {
  Serial.println("Bơm bị khóa – không hoạt động");
} else {
  Blynk.logEvent("water_full","Nước đã đầy không thể cấp thêm");
  Serial.printf("Mực nước đã đủ (%.1f%% ≥ %.1f%%) – không bơm\n", waterLevelPercent, maxWaterThreshold);
}

      // Đánh dấu đã xử lý slot thời gian
      timeList[i].eventTriggered = true;

    } else if (timeList[i].hour != currentHour || timeList[i].minute != currentMinute) {
      timeList[i].eventTriggered = false;
    }
  }
}
// ====== ĐỌC DỮ LIỆU ĐỊNH KỲ ======
void sendSensorData() {
  unsigned long now = millis();
  handleFoodLogic(now);
  handleWaterLogic(now);
  handleTimedFeedingAndWatering();
}
void setup() {
  Serial.begin(9600);
  Blynk.begin(BLYNK_AUTH_TOKEN, ssid, pass);
  timeClient.begin();
  pinMode(relayPin, OUTPUT); digitalWrite(relayPin, LOW);
  pinMode(TRIG_FOOD, OUTPUT); pinMode(ECHO_FOOD, INPUT);
  pinMode(TRIG_WATER, OUTPUT); pinMode(ECHO_WATER, INPUT);

  scale.begin(DOUT, CLK); scale.set_scale(1320.75); scale.tare();
  myServo.attach(SERVO_PIN); myServo.write(0);
   Blynk.syncVirtual(V30);
  timer.setInterval(1000L, sendSensorData);
}
void loop() {
  Blynk.run();
  timer.run();
}
