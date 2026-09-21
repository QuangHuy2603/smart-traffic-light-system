#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <TM1637Display.h>

// ============================================================
// SMART TRAFFIC LIGHT - ESP32 DEVKIT V1
// ============================================================


// ============================================================
// 1. WIFI
// ============================================================

const bool ENABLE_WIFI = true;

const char* WIFI_SSID = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";


// ============================================================
// 2. GPIO - TRAFFIC LIGHT
// ============================================================

// NORTH + SOUTH
const uint8_t NS_RED_PIN    = 13;
const uint8_t NS_YELLOW_PIN = 14;
const uint8_t NS_GREEN_PIN  = 16;

// EAST + WEST
const uint8_t EW_RED_PIN    = 17;
const uint8_t EW_YELLOW_PIN = 18;
const uint8_t EW_GREEN_PIN  = 19;


// ============================================================
// 3. TM1637
// ============================================================

const uint8_t TM1637_CLK = 22;
const uint8_t TM1637_DIO = 21;

TM1637Display display(TM1637_CLK, TM1637_DIO);


// ============================================================
// 4. BUTTONS
// ============================================================

// Button:
// GPIO ---- BUTTON ---- GND
//
// INPUT_PULLUP:
// Không nhấn = HIGH
// Nhấn       = LOW

const uint8_t PEDESTRIAN_BUTTON_PIN = 25;
const uint8_t PRIORITY_BUTTON_PIN   = 26;


// ============================================================
// 5. IR SENSORS
// ============================================================

const uint8_t IR_NS1_PIN = 32;
const uint8_t IR_NS2_PIN = 33;

const uint8_t IR_EW1_PIN = 34;
const uint8_t IR_EW2_PIN = 35;

// Hầu hết module IR LM393:
// LOW = phát hiện xe
// HIGH = không có xe

const bool IR_ACTIVE_LOW = true;


// ============================================================
// 6. TIME SETTINGS
// ============================================================

const uint32_t GREEN_NORMAL_TIME = 10000;  // 10 giây
const uint32_t GREEN_LONG_TIME   = 12000;  // 12 giây
const uint32_t GREEN_SHORT_TIME  = 7000;   // 7 giây

const uint32_t YELLOW_TIME = 3000;         // 3 giây

// Không sử dụng ALL RED

const uint32_t PEDESTRIAN_GREEN_TIME = 12000; // 12 giây
const uint32_t PRIORITY_GREEN_TIME   = 15000; // 15 giây

// Không chuyển pha quá nhanh khi vừa bật xanh
const uint32_t MIN_GREEN_BEFORE_FORCED_SWITCH = 3000;


// ============================================================
// 7. AXIS
// ============================================================

enum Axis
{
  AXIS_NS,
  AXIS_EW
};

// ============================================================
// HƯỚNG CHO PEDESTRIAN
// ============================================================
//
// Hiện tại:
// Pedestrian -> NS
//
// Nếu nút pedestrian của bạn cần EW,
// đổi AXIS_NS thành AXIS_EW.

const Axis PEDESTRIAN_AXIS = AXIS_NS;


// ============================================================
// HƯỚNG CHO PRIORITY
// ============================================================
//
// Hiện tại:
// Priority -> NS
//
// Nếu xe ưu tiên chạy hướng EW,
// đổi AXIS_NS thành AXIS_EW.

const Axis PRIORITY_AXIS = AXIS_NS;


// ============================================================
// 8. TRAFFIC STATE
// ============================================================

enum TrafficState
{
  NS_GREEN,
  NS_YELLOW,

  EW_GREEN,
  EW_YELLOW
};

TrafficState currentState = NS_GREEN;

uint32_t stateStartedMs  = 0;
uint32_t stateDurationMs = GREEN_NORMAL_TIME;


// ============================================================
// 9. 1ms TIMER
// ============================================================

volatile uint32_t systemMs = 0;

hw_timer_t* timer = nullptr;

void ARDUINO_ISR_ATTR onTimer()
{
  systemMs++;
}

uint32_t nowMs()
{
  return systemMs;
}


// ============================================================
// 10. SENSOR STATE
// ============================================================

bool irNS1 = false;
bool irNS2 = false;

bool irEW1 = false;
bool irEW2 = false;

uint32_t lastSensorUpdate = 0;


// ============================================================
// 11. REQUEST STATE
// ============================================================

bool pedestrianRequest = false;
bool pedestrianServing = false;

bool priorityRequest = false;
bool priorityServing = false;


// ============================================================
// 12. BUTTON DEBOUNCE
// ============================================================

const uint32_t BUTTON_DEBOUNCE_MS = 35;


// ---------------- PEDESTRIAN ----------------

bool pedLastRaw = HIGH;
bool pedStableState = HIGH;

uint32_t pedLastChange = 0;


// ---------------- PRIORITY ----------------

bool priLastRaw = HIGH;
bool priStableState = HIGH;

uint32_t priLastChange = 0;


// ============================================================
// 13. TM1637
// ============================================================

uint32_t lastDisplayUpdate = 0;


// ============================================================
// 14. WEB SERVER
// ============================================================

WebServer server(80);

bool webStarted = false;
bool wifiConfigured = false;

uint32_t lastWifiReconnect = 0;


// ============================================================
// 15. DEBUG
// ============================================================

uint32_t lastDebugPrint = 0;


// ============================================================
// FUNCTION DECLARATIONS
// ============================================================

bool readIR(uint8_t pin);

void sampleSensors();
void updateSensors();

int getNSDensity();
int getEWDensity();

String densityName(int density);

uint32_t getAdaptiveGreenTime(Axis axis);

void applyTrafficLights();

void enterState(TrafficState newState);

bool handleRequestsDuringGreen();

void updateFSM();

bool pedestrianButtonPressed();
bool priorityButtonPressed();

void updateButtons();

uint32_t getStateRemainingSeconds();

void getAxisCountdowns(
  uint16_t& nsCountdown,
  uint16_t& ewCountdown
);

void updateTM1637();

String stateName();

String nsLightName();
String ewLightName();

void addCorsHeaders();

void handleOptions();
void handleStatus();

void startWebServer();

void startWiFi();
void updateWiFi();

void printDebug();


// ============================================================
// 16. IR FUNCTIONS
// ============================================================

bool readIR(uint8_t pin)
{
  int value = digitalRead(pin);

  if (IR_ACTIVE_LOW)
  {
    return value == LOW;
  }

  return value == HIGH;
}


void sampleSensors()
{
  irNS1 = readIR(IR_NS1_PIN);
  irNS2 = readIR(IR_NS2_PIN);

  irEW1 = readIR(IR_EW1_PIN);
  irEW2 = readIR(IR_EW2_PIN);
}


void updateSensors()
{
  uint32_t now = nowMs();

  if (now - lastSensorUpdate < 50)
  {
    return;
  }

  lastSensorUpdate = now;

  sampleSensors();
}


// ============================================================
// 17. TRAFFIC DENSITY
// ============================================================

int getNSDensity()
{
  int count = 0;

  if (irNS1) count++;
  if (irNS2) count++;

  return count;
}


int getEWDensity()
{
  int count = 0;

  if (irEW1) count++;
  if (irEW2) count++;

  return count;
}


String densityName(int density)
{
  if (density == 0)
  {
    return "EMPTY";
  }

  if (density == 1)
  {
    return "LOW";
  }

  return "HIGH";
}


// ============================================================
// 18. ADAPTIVE GREEN TIME
// ============================================================

uint32_t getAdaptiveGreenTime(Axis axis)
{
  int nsDensity = getNSDensity();
  int ewDensity = getEWDensity();

  // Hai hướng bằng nhau
  if (nsDensity == ewDensity)
  {
    return GREEN_NORMAL_TIME;
  }


  // NS đông hơn EW
  if (nsDensity > ewDensity)
  {
    if (axis == AXIS_NS)
    {
      return GREEN_LONG_TIME;
    }

    return GREEN_SHORT_TIME;
  }


  // EW đông hơn NS
  if (axis == AXIS_EW)
  {
    return GREEN_LONG_TIME;
  }

  return GREEN_SHORT_TIME;
}


// ============================================================
// 19. TRAFFIC LIGHT OUTPUT
// ============================================================

void applyTrafficLights()
{
  // Tắt tất cả trước
  digitalWrite(NS_RED_PIN, LOW);
  digitalWrite(NS_YELLOW_PIN, LOW);
  digitalWrite(NS_GREEN_PIN, LOW);

  digitalWrite(EW_RED_PIN, LOW);
  digitalWrite(EW_YELLOW_PIN, LOW);
  digitalWrite(EW_GREEN_PIN, LOW);


  switch (currentState)
  {

    // ========================================================
    // NS GREEN
    // ========================================================

    case NS_GREEN:

      digitalWrite(NS_GREEN_PIN, HIGH);

      digitalWrite(EW_RED_PIN, HIGH);

      break;


    // ========================================================
    // NS YELLOW
    // ========================================================

    case NS_YELLOW:

      digitalWrite(NS_YELLOW_PIN, HIGH);

      digitalWrite(EW_RED_PIN, HIGH);

      break;


    // ========================================================
    // EW GREEN
    // ========================================================

    case EW_GREEN:

      digitalWrite(EW_GREEN_PIN, HIGH);

      digitalWrite(NS_RED_PIN, HIGH);

      break;


    // ========================================================
    // EW YELLOW
    // ========================================================

    case EW_YELLOW:

      digitalWrite(EW_YELLOW_PIN, HIGH);

      digitalWrite(NS_RED_PIN, HIGH);

      break;
  }
}


// ============================================================
// 20. ENTER NEW STATE
// ============================================================

void enterState(TrafficState newState)
{
  currentState = newState;

  stateStartedMs = nowMs();


  // ==========================================================
  // NS GREEN
  // ==========================================================

  if (newState == NS_GREEN)
  {

    // Priority
    if (
      priorityRequest &&
      PRIORITY_AXIS == AXIS_NS
    )
    {
      priorityServing = true;

      stateDurationMs =
        PRIORITY_GREEN_TIME;


      // Nếu pedestrian cùng hướng
      // thì xem như được phục vụ luôn

      if (
        pedestrianRequest &&
        PEDESTRIAN_AXIS == AXIS_NS
      )
      {
        pedestrianRequest = false;
        pedestrianServing = false;
      }
    }


    // Pedestrian
    else if (
      pedestrianRequest &&
      PEDESTRIAN_AXIS == AXIS_NS
    )
    {
      pedestrianServing = true;

      stateDurationMs =
        PEDESTRIAN_GREEN_TIME;
    }


    // Bình thường
    else
    {
      stateDurationMs =
        getAdaptiveGreenTime(AXIS_NS);
    }
  }


  // ==========================================================
  // EW GREEN
  // ==========================================================

  else if (newState == EW_GREEN)
  {

    // Priority
    if (
      priorityRequest &&
      PRIORITY_AXIS == AXIS_EW
    )
    {
      priorityServing = true;

      stateDurationMs =
        PRIORITY_GREEN_TIME;


      if (
        pedestrianRequest &&
        PEDESTRIAN_AXIS == AXIS_EW
      )
      {
        pedestrianRequest = false;
        pedestrianServing = false;
      }
    }


    // Pedestrian
    else if (
      pedestrianRequest &&
      PEDESTRIAN_AXIS == AXIS_EW
    )
    {
      pedestrianServing = true;

      stateDurationMs =
        PEDESTRIAN_GREEN_TIME;
    }


    // Bình thường
    else
    {
      stateDurationMs =
        getAdaptiveGreenTime(AXIS_EW);
    }
  }


  // ==========================================================
  // YELLOW
  // ==========================================================

  else
  {
    stateDurationMs =
      YELLOW_TIME;
  }


  applyTrafficLights();


  Serial.print("STATE -> ");
  Serial.println(stateName());
}


// ============================================================
// 21. BUTTON FUNCTIONS
// ============================================================

bool pedestrianButtonPressed()
{
  bool raw =
    digitalRead(PEDESTRIAN_BUTTON_PIN);

  uint32_t now =
    nowMs();


  // Có thay đổi trạng thái nút
  if (raw != pedLastRaw)
  {
    pedLastRaw = raw;

    pedLastChange = now;
  }


  // Chờ debounce
  if (
    now - pedLastChange >=
    BUTTON_DEBOUNCE_MS
  )
  {
    if (pedStableState != raw)
    {
      pedStableState = raw;


      // LOW = nhấn
      if (pedStableState == LOW)
      {
        return true;
      }
    }
  }


  return false;
}


bool priorityButtonPressed()
{
  bool raw =
    digitalRead(PRIORITY_BUTTON_PIN);

  uint32_t now =
    nowMs();


  if (raw != priLastRaw)
  {
    priLastRaw = raw;

    priLastChange = now;
  }


  if (
    now - priLastChange >=
    BUTTON_DEBOUNCE_MS
  )
  {
    if (priStableState != raw)
    {
      priStableState = raw;


      if (priStableState == LOW)
      {
        return true;
      }
    }
  }


  return false;
}


// ============================================================
// 22. UPDATE BUTTONS
// ============================================================

void updateButtons()
{
  // ==========================================================
  // PEDESTRIAN
  // ==========================================================

  if (pedestrianButtonPressed())
  {
    if (!pedestrianRequest)
    {
      pedestrianRequest = true;

      pedestrianServing = false;


      Serial.println(
        "[BUTTON] PEDESTRIAN REQUEST"
      );
    }
  }


  // ==========================================================
  // PRIORITY
  // ==========================================================

  if (priorityButtonPressed())
  {
    if (!priorityRequest)
    {
      priorityRequest = true;

      priorityServing = false;


      Serial.println(
        "[BUTTON] PRIORITY REQUEST"
      );
    }
  }
}


// ============================================================
// 23. HANDLE BUTTON REQUEST
// ============================================================

bool handleRequestsDuringGreen()
{
  bool currentIsGreen =
    currentState == NS_GREEN ||
    currentState == EW_GREEN;


  if (!currentIsGreen)
  {
    return false;
  }


  Axis currentAxis;


  if (currentState == NS_GREEN)
  {
    currentAxis = AXIS_NS;
  }
  else
  {
    currentAxis = AXIS_EW;
  }


  uint32_t elapsed =
    nowMs() - stateStartedMs;


  // ==========================================================
  // PRIORITY - ƯU TIÊN CAO NHẤT
  // ==========================================================

  if (priorityRequest)
  {

    // Xe ưu tiên đang ở hướng đang xanh
    if (currentAxis == PRIORITY_AXIS)
    {

      if (!priorityServing)
      {
        priorityServing = true;


        uint32_t requiredEnd =
          elapsed +
          PRIORITY_GREEN_TIME;


        if (stateDurationMs < requiredEnd)
        {
          stateDurationMs =
            requiredEnd;
        }


        if (
          pedestrianRequest &&
          PEDESTRIAN_AXIS ==
          PRIORITY_AXIS
        )
        {
          pedestrianRequest = false;
          pedestrianServing = false;
        }
      }


      return false;
    }


    // Xe ưu tiên ở hướng đang đỏ
    // Chuyển hướng sau tối thiểu 3 giây xanh

    if (
      elapsed >=
      MIN_GREEN_BEFORE_FORCED_SWITCH
    )
    {

      pedestrianServing = false;


      if (currentAxis == AXIS_NS)
      {
        enterState(NS_YELLOW);
      }
      else
      {
        enterState(EW_YELLOW);
      }


      return true;
    }


    return false;
  }


  // ==========================================================
  // PEDESTRIAN
  // ==========================================================

  if (pedestrianRequest)
  {

    // Đúng hướng đang xanh
    if (currentAxis == PEDESTRIAN_AXIS)
    {

      if (!pedestrianServing)
      {
        pedestrianServing = true;


        uint32_t requiredEnd =
          elapsed +
          PEDESTRIAN_GREEN_TIME;


        if (stateDurationMs < requiredEnd)
        {
          stateDurationMs =
            requiredEnd;
        }
      }


      return false;
    }


    // Hướng cần pedestrian đang đỏ
    // chuyển qua yellow trước

    if (
      elapsed >=
      MIN_GREEN_BEFORE_FORCED_SWITCH
    )
    {

      if (currentAxis == AXIS_NS)
      {
        enterState(NS_YELLOW);
      }
      else
      {
        enterState(EW_YELLOW);
      }


      return true;
    }
  }


  return false;
}


// ============================================================
// 24. FSM
// ============================================================

void updateFSM()
{
  // Xử lý nút trước
  if (handleRequestsDuringGreen())
  {
    return;
  }


  uint32_t elapsed =
    nowMs() - stateStartedMs;


  if (elapsed < stateDurationMs)
  {
    return;
  }


  // ==========================================================
  // NS GREEN -> NS YELLOW
  // ==========================================================

  if (currentState == NS_GREEN)
  {

    if (priorityServing)
    {
      priorityServing = false;
      priorityRequest = false;
    }


    if (pedestrianServing)
    {
      pedestrianServing = false;
      pedestrianRequest = false;
    }


    enterState(NS_YELLOW);

    return;
  }


  // ==========================================================
  // NS YELLOW
  // ==========================================================

  if (currentState == NS_YELLOW)
  {

    // PRIORITY
    if (priorityRequest)
    {

      if (PRIORITY_AXIS == AXIS_NS)
      {
        enterState(NS_GREEN);
      }
      else
      {
        enterState(EW_GREEN);
      }


      return;
    }


    // PEDESTRIAN
    if (pedestrianRequest)
    {

      if (PEDESTRIAN_AXIS == AXIS_NS)
      {
        enterState(NS_GREEN);
      }
      else
      {
        enterState(EW_GREEN);
      }


      return;
    }


    // NORMAL
    enterState(EW_GREEN);

    return;
  }


  // ==========================================================
  // EW GREEN -> EW YELLOW
  // ==========================================================

  if (currentState == EW_GREEN)
  {

    if (priorityServing)
    {
      priorityServing = false;
      priorityRequest = false;
    }


    if (pedestrianServing)
    {
      pedestrianServing = false;
      pedestrianRequest = false;
    }


    enterState(EW_YELLOW);

    return;
  }


  // ==========================================================
  // EW YELLOW
  // ==========================================================

  if (currentState == EW_YELLOW)
  {

    // PRIORITY
    if (priorityRequest)
    {

      if (PRIORITY_AXIS == AXIS_NS)
      {
        enterState(NS_GREEN);
      }
      else
      {
        enterState(EW_GREEN);
      }


      return;
    }


    // PEDESTRIAN
    if (pedestrianRequest)
    {

      if (PEDESTRIAN_AXIS == AXIS_NS)
      {
        enterState(NS_GREEN);
      }
      else
      {
        enterState(EW_GREEN);
      }


      return;
    }


    // NORMAL
    enterState(NS_GREEN);

    return;
  }
}


// ============================================================
// 25. COUNTDOWN
// ============================================================

uint32_t getStateRemainingSeconds()
{
  uint32_t elapsed =
    nowMs() - stateStartedMs;


  if (elapsed >= stateDurationMs)
  {
    return 0;
  }


  uint32_t remaining =
    stateDurationMs - elapsed;


  // Làm tròn lên
  return (remaining + 999) / 1000;
}


// ============================================================
// 26. NS / EW COUNTDOWN
// ============================================================

void getAxisCountdowns(
  uint16_t& nsCountdown,
  uint16_t& ewCountdown
)
{
  uint16_t remaining =
    getStateRemainingSeconds();


  uint16_t yellowSeconds =
    YELLOW_TIME / 1000;


  switch (currentState)
  {

    // ========================================================
    case NS_GREEN:

      nsCountdown =
        remaining;

      ewCountdown =
        remaining +
        yellowSeconds;

      break;


    // ========================================================
    case NS_YELLOW:

      nsCountdown =
        remaining;

      ewCountdown =
        remaining;

      break;


    // ========================================================
    case EW_GREEN:

      ewCountdown =
        remaining;

      nsCountdown =
        remaining +
        yellowSeconds;

      break;


    // ========================================================
    case EW_YELLOW:

      ewCountdown =
        remaining;

      nsCountdown =
        remaining;

      break;
  }


  if (nsCountdown > 99)
  {
    nsCountdown = 99;
  }


  if (ewCountdown > 99)
  {
    ewCountdown = 99;
  }
}


// ============================================================
// 27. TM1637 DISPLAY
// ============================================================
//
// Hiển thị:
//
// NS:EW
//
// Ví dụ:
// 08:05
//
// ============================================================

void updateTM1637()
{
  uint32_t now =
    nowMs();


  if (
    now -
    lastDisplayUpdate <
    100
  )
  {
    return;
  }


  lastDisplayUpdate = now;


  uint16_t ns;
  uint16_t ew;


  getAxisCountdowns(
    ns,
    ew
  );


  int displayValue =
    (ns * 100) + ew;


  display.showNumberDecEx(
    displayValue,
    0b01000000,
    true,
    4,
    0
  );
}


// ============================================================
// 28. STATE NAME
// ============================================================

String stateName()
{
  switch (currentState)
  {
    case NS_GREEN:
      return "NS_GREEN";


    case NS_YELLOW:
      return "NS_YELLOW";


    case EW_GREEN:
      return "EW_GREEN";


    case EW_YELLOW:
      return "EW_YELLOW";
  }


  return "UNKNOWN";
}


// ============================================================
// 29. LIGHT NAME
// ============================================================

String nsLightName()
{
  if (currentState == NS_GREEN)
  {
    return "GREEN";
  }


  if (currentState == NS_YELLOW)
  {
    return "YELLOW";
  }


  return "RED";
}


String ewLightName()
{
  if (currentState == EW_GREEN)
  {
    return "GREEN";
  }


  if (currentState == EW_YELLOW)
  {
    return "YELLOW";
  }


  return "RED";
}


// ============================================================
// 30. CORS
// ============================================================

void addCorsHeaders()
{
  server.sendHeader(
    "Access-Control-Allow-Origin",
    "*"
  );


  server.sendHeader(
    "Access-Control-Allow-Methods",
    "GET, OPTIONS"
  );


  server.sendHeader(
    "Access-Control-Allow-Headers",
    "Content-Type"
  );
}


void handleOptions()
{
  addCorsHeaders();


  server.send(
    204,
    "text/plain",
    ""
  );
}


// ============================================================
// 31. WEB API
// ============================================================

void handleStatus()
{
  uint16_t nsCountdown;
  uint16_t ewCountdown;


  getAxisCountdowns(
    nsCountdown,
    ewCountdown
  );


  int nsDensity =
    getNSDensity();


  int ewDensity =
    getEWDensity();


  bool vehicleNS =
    nsDensity > 0;


  bool vehicleEW =
    ewDensity > 0;


  bool priorityActive =
    priorityRequest ||
    priorityServing;


  bool pedestrianActive =
    pedestrianRequest ||
    pedestrianServing;


  String mode =
    "NORMAL";


  if (priorityActive)
  {
    mode =
      "PRIORITY";
  }

  else if (pedestrianActive)
  {
    mode =
      "PEDESTRIAN";
  }


  String json;

  json.reserve(800);


  json += "{";


  // ==========================================================
  // STATE
  // ==========================================================

  json += "\"state\":\"";
  json += stateName();
  json += "\",";


  json += "\"mode\":\"";
  json += mode;
  json += "\",";


  // ==========================================================
  // LIGHT
  // ==========================================================

  json += "\"nsLight\":\"";
  json += nsLightName();
  json += "\",";


  json += "\"ewLight\":\"";
  json += ewLightName();
  json += "\",";


  // ==========================================================
  // COUNTDOWN
  // ==========================================================

  json += "\"nsCountdown\":";
  json += nsCountdown;
  json += ",";


  json += "\"ewCountdown\":";
  json += ewCountdown;
  json += ",";


  json += "\"currentCountdown\":";
  json += getStateRemainingSeconds();
  json += ",";


  // ==========================================================
  // IR
  // ==========================================================

  json += "\"irNS1\":";
  json += irNS1 ? "true" : "false";
  json += ",";


  json += "\"irNS2\":";
  json += irNS2 ? "true" : "false";
  json += ",";


  json += "\"irEW1\":";
  json += irEW1 ? "true" : "false";
  json += ",";


  json += "\"irEW2\":";
  json += irEW2 ? "true" : "false";
  json += ",";


  // ==========================================================
  // COMPATIBILITY
  // ==========================================================

  json += "\"vehicleNS\":";
  json += vehicleNS ? "true" : "false";
  json += ",";


  json += "\"vehicleEW\":";
  json += vehicleEW ? "true" : "false";
  json += ",";


  // ==========================================================
  // DENSITY
  // ==========================================================

  json += "\"densityNS\":\"";
  json += densityName(nsDensity);
  json += "\",";


  json += "\"densityEW\":\"";
  json += densityName(ewDensity);
  json += "\",";


  // ==========================================================
  // PEDESTRIAN
  // ==========================================================

  json += "\"pedestrianRequest\":";

  json +=
    pedestrianActive
    ? "true"
    : "false";

  json += ",";


  // ==========================================================
  // PRIORITY
  // ==========================================================

  json += "\"priorityActive\":";

  json +=
    priorityActive
    ? "true"
    : "false";

  json += ",";


  json +=
    "\"priorityDirection\":\"";


  if (priorityActive)
  {
    if (PRIORITY_AXIS == AXIS_NS)
    {
      json += "NS";
    }
    else
    {
      json += "EW";
    }
  }
  else
  {
    json += "NONE";
  }


  json += "\",";


  // ==========================================================
  // UPTIME
  // ==========================================================

  json += "\"uptime\":";

  json +=
    nowMs() / 1000;


  json += "}";


  addCorsHeaders();


  server.send(
    200,
    "application/json",
    json
  );
}


// ============================================================
// 32. WEB SERVER
// ============================================================

void startWebServer()
{
  if (webStarted)
  {
    return;
  }


  server.on(
    "/",
    HTTP_GET,
    []()
    {
      addCorsHeaders();


      String text =
        "SMART TRAFFIC LIGHT ESP32\n\n";


      text +=
        "Status API:\n";


      text +=
        "/api/status\n";


      server.send(
        200,
        "text/plain",
        text
      );
    }
  );


  server.on(
    "/api/status",
    HTTP_GET,
    handleStatus
  );


  server.on(
    "/api/status",
    HTTP_OPTIONS,
    handleOptions
  );


  server.onNotFound(
    []()
    {

      if (
        server.method() ==
        HTTP_OPTIONS
      )
      {
        handleOptions();

        return;
      }


      addCorsHeaders();


      server.send(
        404,
        "text/plain",
        "Not found"
      );
    }
  );


  server.begin();


  webStarted = true;


  Serial.println();
  Serial.println(
    "WEB SERVER STARTED"
  );


  Serial.print(
    "ESP32: http://"
  );

  Serial.println(
    WiFi.localIP()
  );


  Serial.print(
    "API  : http://"
  );

  Serial.print(
    WiFi.localIP()
  );

  Serial.println(
    "/api/status"
  );
}


// ============================================================
// 33. WIFI START
// ============================================================

void startWiFi()
{
  if (!ENABLE_WIFI)
  {
    return;
  }


  if (
    strlen(WIFI_SSID) == 0 ||
    strcmp(
      WIFI_SSID,
      "TEN_WIFI_CUA_BAN"
    ) == 0
  )
  {
    Serial.println();
    Serial.println(
      "WiFi chua duoc cau hinh."
    );

    return;
  }


  wifiConfigured = true;


  WiFi.mode(
    WIFI_STA
  );


  // Giúp WiFi ổn định hơn
  WiFi.setSleep(false);


  WiFi.begin(
    WIFI_SSID,
    WIFI_PASSWORD
  );


  Serial.println();
  Serial.print(
    "Connecting WiFi"
  );


  lastWifiReconnect =
    nowMs();
}


// ============================================================
// 34. WIFI UPDATE
// ============================================================

void updateWiFi()
{
  if (
    !ENABLE_WIFI ||
    !wifiConfigured
  )
  {
    return;
  }


  static bool wasConnected =
    false;


  // ==========================================================
  // CONNECTED
  // ==========================================================

  if (
    WiFi.status() ==
    WL_CONNECTED
  )
  {

    if (!wasConnected)
    {
      Serial.println();
      Serial.println(
        "WiFi connected!"
      );


      Serial.print(
        "ESP32 IP: "
      );


      Serial.println(
        WiFi.localIP()
      );


      wasConnected =
        true;
    }


    if (!webStarted)
    {
      startWebServer();
    }


    server.handleClient();


    return;
  }


  // ==========================================================
  // DISCONNECTED
  // ==========================================================

  if (wasConnected)
  {
    Serial.println();
    Serial.println(
      "WiFi disconnected!"
    );


    wasConnected =
      false;
  }


  // Reconnect mỗi 5 giây
  if (
    nowMs() -
    lastWifiReconnect >=
    5000
  )
  {
    lastWifiReconnect =
      nowMs();


    Serial.println(
      "Reconnecting WiFi..."
    );


    WiFi.reconnect();
  }
}


// ============================================================
// 35. DEBUG
// ============================================================

void printDebug()
{
  if (
    nowMs() -
    lastDebugPrint <
    1000
  )
  {
    return;
  }


  lastDebugPrint =
    nowMs();


  Serial.print(
    "STATE="
  );

  Serial.print(
    stateName()
  );


  Serial.print(
    " | NS1="
  );

  Serial.print(
    irNS1
  );


  Serial.print(
    " NS2="
  );

  Serial.print(
    irNS2
  );


  Serial.print(
    " | EW1="
  );

  Serial.print(
    irEW1
  );


  Serial.print(
    " EW2="
  );

  Serial.print(
    irEW2
  );


  Serial.print(
    " | Density NS="
  );

  Serial.print(
    getNSDensity()
  );


  Serial.print(
    " EW="
  );

  Serial.print(
    getEWDensity()
  );


  Serial.print(
    " | PED="
  );

  Serial.print(
    pedestrianRequest ||
    pedestrianServing
  );


  Serial.print(
    " | PRI="
  );

  Serial.print(
    priorityRequest ||
    priorityServing
  );


  uint16_t ns;
  uint16_t ew;


  getAxisCountdowns(
    ns,
    ew
  );


  Serial.print(
    " | NS_Count="
  );

  Serial.print(
    ns
  );


  Serial.print(
    " EW_Count="
  );

  Serial.println(
    ew
  );
}


// ============================================================
// 36. SETUP
// ============================================================

void setup()
{
  Serial.begin(
    115200
  );


  // ==========================================================
  // TRAFFIC LED OUTPUT
  // ==========================================================

  pinMode(
    NS_RED_PIN,
    OUTPUT
  );

  pinMode(
    NS_YELLOW_PIN,
    OUTPUT
  );

  pinMode(
    NS_GREEN_PIN,
    OUTPUT
  );


  pinMode(
    EW_RED_PIN,
    OUTPUT
  );

  pinMode(
    EW_YELLOW_PIN,
    OUTPUT
  );

  pinMode(
    EW_GREEN_PIN,
    OUTPUT
  );


  // Tắt tất cả LED ban đầu

  digitalWrite(
    NS_RED_PIN,
    LOW
  );

  digitalWrite(
    NS_YELLOW_PIN,
    LOW
  );

  digitalWrite(
    NS_GREEN_PIN,
    LOW
  );


  digitalWrite(
    EW_RED_PIN,
    LOW
  );

  digitalWrite(
    EW_YELLOW_PIN,
    LOW
  );

  digitalWrite(
    EW_GREEN_PIN,
    LOW
  );


  // ==========================================================
  // IR INPUT
  // ==========================================================

  pinMode(
    IR_NS1_PIN,
    INPUT
  );

  pinMode(
    IR_NS2_PIN,
    INPUT
  );

  pinMode(
    IR_EW1_PIN,
    INPUT
  );

  pinMode(
    IR_EW2_PIN,
    INPUT
  );


  sampleSensors();


  // ==========================================================
  // BUTTON INPUT
  // ==========================================================

  pinMode(
    PEDESTRIAN_BUTTON_PIN,
    INPUT_PULLUP
  );


  pinMode(
    PRIORITY_BUTTON_PIN,
    INPUT_PULLUP
  );


  // ==========================================================
  // BUTTON INITIAL STATE
  // ==========================================================

  pedLastRaw =
    digitalRead(
      PEDESTRIAN_BUTTON_PIN
    );


  pedStableState =
    pedLastRaw;


  priLastRaw =
    digitalRead(
      PRIORITY_BUTTON_PIN
    );


  priStableState =
    priLastRaw;


  // ==========================================================
  // TM1637
  // ==========================================================

  display.setBrightness(
    7
  );


  display.clear();


  // ==========================================================
  // ESP32 HARDWARE TIMER
  // Arduino ESP32 Core 3.x
  // ==========================================================

  timer =
    timerBegin(
      1000000
    );


  timerAttachInterrupt(
    timer,
    &onTimer
  );


  timerAlarm(
    timer,
    1000,
    true,
    0
  );


  pedLastChange =
    nowMs();


  priLastChange =
    nowMs();


  // ==========================================================
  // INITIAL TRAFFIC STATE
  // ==========================================================

  enterState(
    NS_GREEN
  );


  // ==========================================================
  // WIFI
  // ==========================================================

  startWiFi();


  // ==========================================================
  // START MESSAGE
  // ==========================================================

  Serial.println();
  Serial.println(
    "================================"
  );

  Serial.println(
    "SMART TRAFFIC LIGHT STARTED"
  );

  Serial.println(
    "================================"
  );


  Serial.println(
    "FSM:"
  );


  Serial.println(
    "NS_GREEN"
  );

  Serial.println(
    " -> NS_YELLOW"
  );

  Serial.println(
    " -> EW_GREEN"
  );

  Serial.println(
    " -> EW_YELLOW"
  );


  Serial.println();
  Serial.println(
    "NO ALL-RED PHASE"
  );

  Serial.println();
}


// ============================================================
// 37. LOOP
// ============================================================

void loop()
{
  // Đọc cảm biến
  updateSensors();


  // Đọc nút
  updateButtons();


  // Điều khiển đèn
  updateFSM();


  // Countdown
  updateTM1637();


  // WiFi + Web
  updateWiFi();


  // Serial monitor
  printDebug();


  // Không sử dụng delay()
}