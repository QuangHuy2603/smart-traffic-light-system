"use strict";

// ======================================================
// ESP32 CONNECTION CONFIGURATION
// ======================================================

const DEFAULT_ESP32_IP = "172.20.10.2";
const STORAGE_KEY_IP = "smart_traffic_esp32_ip";

function isValidIpv4(ip) {
  if (typeof ip !== "string") return false;
  const parts = ip.trim().split(".");
  if (parts.length !== 4) return false;
  return parts.every((part) => {
    if (!/^\d+$/.test(part)) return false;
    const num = Number(part);
    return num >= 0 && num <= 255 && String(num) === part;
  });
}

function loadSavedIp() {
  try {
    const saved = localStorage.getItem(STORAGE_KEY_IP);
    if (saved && isValidIpv4(saved.trim())) {
      return saved.trim();
    }
  } catch (e) {
    console.warn("Cannot access localStorage:", e);
  }
  return DEFAULT_ESP32_IP;
}

function saveIp(ip) {
  try {
    localStorage.setItem(STORAGE_KEY_IP, ip);
  } catch (e) {
    console.warn("Cannot save to localStorage:", e);
  }
}

// IP của ESP32 (lấy từ localStorage hoặc mặc định)
let ESP32_IP = loadSavedIp();

// REST API trạng thái hệ thống
let API_URL = `http://${ESP32_IP}/api/status`;

function setEsp32Ip(newIp) {
  ESP32_IP = newIp;
  API_URL = `http://${ESP32_IP}/api/status`;
  if (dom.esp32IpLine) {
    dom.esp32IpLine.textContent = ESP32_IP;
  }
}

// false = lấy dữ liệu thật từ ESP32
// true  = chạy dữ liệu giả lập để test giao diện
const MOCK_MODE = false;

const POLL_INTERVAL_MS = 500;
const REQUEST_TIMEOUT_MS = 1200;
const OFFLINE_AFTER_FAILURES = 5;
const MAX_EVENTS = 5;


// ======================================================
// VALID VALUES SETS (FSM MỚI - 4 STATES, KHÔNG CÓ ALL-RED)
// ======================================================

const VALID_STATES = new Set([
  "NS_GREEN",
  "NS_YELLOW",
  "EW_GREEN",
  "EW_YELLOW"
]);

const VALID_MODES = new Set([
  "NORMAL",
  "PEDESTRIAN",
  "PRIORITY"
]);

const VALID_LIGHTS = new Set([
  "RED",
  "YELLOW",
  "GREEN"
]);

const VALID_DENSITIES = new Set([
  "EMPTY",
  "LOW",
  "HIGH"
]);

const VALID_PRIORITY_DIRECTIONS = new Set([
  "NONE",
  "NS",
  "EW"
]);


// ======================================================
// MÔ TẢ TRẠNG THÁI FSM TIẾNG VIỆT
// ======================================================

const STATE_DESCRIPTIONS = {
  NS_GREEN: "Đèn xanh hướng Bắc – Nam",
  NS_YELLOW: "Đèn vàng hướng Bắc – Nam, chuẩn bị dừng",
  EW_GREEN: "Đèn xanh hướng Đông – Tây",
  EW_YELLOW: "Đèn vàng hướng Đông – Tây, chuẩn bị dừng"
};


// ======================================================
// MÔ TẢ CHẾ ĐỘ & MẬT ĐỘ
// ======================================================

const MODE_LABELS = {
  NORMAL: "Bình thường",
  PEDESTRIAN: "Người đi bộ",
  PRIORITY: "Ưu tiên khẩn cấp"
};

const LIGHT_LABELS = {
  RED: "ĐÈN ĐỎ",
  YELLOW: "ĐÈN VÀNG",
  GREEN: "ĐÈN XANH"
};

const DENSITY_LABELS = {
  EMPTY: "Trống",
  LOW: "Thấp",
  HIGH: "Cao"
};


// ======================================================
// BIẾN TOÀN CỤC & DOM CACHE
// ======================================================

const dom = {};
let previousData = null;
let consecutiveFailures = 0;
let connectionState = "connecting";
let pollingTimer = null;
let mockTick = 0;
let requestInProgress = false;
let lastServerUpdateTime = 0;


// ======================================================
// KHỞI CHẠY ỨNG DỤNG
// ======================================================

document.addEventListener("DOMContentLoaded", () => {
  cacheDomElements();
  initConnectionControls();
  updateConnectionStatus("connecting");
  fetchTrafficData();

  pollingTimer = window.setInterval(fetchTrafficData, POLL_INTERVAL_MS);
  window.setInterval(updateLocalCountdown, 200);
});

window.addEventListener("beforeunload", () => {
  if (pollingTimer) {
    window.clearInterval(pollingTimer);
  }
});


// ======================================================
// KHỞI TẠO ĐIỀU KHIỂN KẾT NỐI IP ESP32
// ======================================================

function initConnectionControls() {
  if (dom.ipInput) {
    dom.ipInput.value = ESP32_IP;

    dom.ipInput.addEventListener("input", () => {
      if (dom.ipErrorMsg) {
        dom.ipErrorMsg.hidden = true;
      }
      dom.ipInput.classList.remove("is-invalid");
    });

    dom.ipInput.addEventListener("keydown", (event) => {
      if (event.key === "Enter") {
        event.preventDefault();
        handleConnect();
      }
    });
  }

  if (dom.btnConnect) {
    dom.btnConnect.addEventListener("click", handleConnect);
  }

  if (dom.esp32IpLine) {
    dom.esp32IpLine.textContent = ESP32_IP;
  }
}

function handleConnect() {
  if (!dom.ipInput) return;
  const inputVal = dom.ipInput.value.trim();

  if (!isValidIpv4(inputVal)) {
    if (dom.ipErrorMsg) {
      dom.ipErrorMsg.hidden = false;
    }
    dom.ipInput.classList.add("is-invalid");
    return;
  }

  if (dom.ipErrorMsg) {
    dom.ipErrorMsg.hidden = true;
  }
  dom.ipInput.classList.remove("is-invalid");

  saveIp(inputVal);
  setEsp32Ip(inputVal);

  consecutiveFailures = 0;
  updateConnectionStatus("connecting");
  fetchTrafficData();
}


// ======================================================
// CACHE DOM ELEMENTS
// ======================================================

function cacheDomElements() {
  const ids = [
    // Điều khiển IP ESP32
    "ipInput",
    "btnConnect",
    "ipErrorMsg",

    // Trạng thái kết nối Header
    "connectionBadge",
    "connectionDot",
    "connectionText",
    "lastUpdate",
    "connectionAlert",
    "priorityAlert",
    "priorityBannerDirection",

    // Cột Bắc – Nam
    "nsRed",
    "nsYellow",
    "nsGreen",
    "nsLightValue",
    "nsCountdown",
    "irNS1Status",
    "irNS2Status",
    "nsSensorCount",
    "nsAxisDensity",

    // Cột Đông – Tây
    "ewRed",
    "ewYellow",
    "ewGreen",
    "ewLightValue",
    "ewCountdown",
    "irEW1Status",
    "irEW2Status",
    "ewSensorCount",
    "ewAxisDensity",

    // Trạng thái hệ thống
    "summaryMode",
    "stateTitle",
    "stateDescription",
    "pedestrianStatusLine",
    "priorityStatusLine",
    "uptimeStatusLine",
    "esp32IpLine",

    // Nhật ký sự kiện
    "eventLogBody",
    "emptyEventRow"
  ];

  ids.forEach((id) => {
    dom[id] = document.getElementById(id);
  });
}


// ======================================================
// FETCH DATA (POLLING TỪ ESP32)
// ======================================================

async function fetchTrafficData() {
  if (requestInProgress) {
    return;
  }

  requestInProgress = true;

  try {
    const rawData = MOCK_MODE
      ? getMockData()
      : await requestStatus();

    const adaptedData = MOCK_MODE
      ? rawData
      : adaptEsp32Data(rawData);

    const currentData = normalizeData(adaptedData, previousData);

    if (connectionState !== "online") {
      addEvent("Kết nối ESP32 thành công", "KẾT NỐI");
    }

    consecutiveFailures = 0;
    updateConnectionStatus("online");

    detectEvents(previousData, currentData);
    updateDashboard(currentData);

    previousData = currentData;
    lastServerUpdateTime = Date.now();
  } catch (error) {
    consecutiveFailures += 1;

    if (consecutiveFailures >= OFFLINE_AFTER_FAILURES) {
      if (connectionState !== "offline") {
        addEvent("Mất kết nối với ESP32", "CẢNH BÁO");
      }
      updateConnectionStatus("offline");
    }

    console.warn("ESP32 status request failed:", error.message);
  } finally {
    requestInProgress = false;
  }
}


// ======================================================
// REQUEST ESP32 HTTP API
// ======================================================

async function requestStatus() {
  const controller = new AbortController();
  const timeoutId = window.setTimeout(() => controller.abort(), REQUEST_TIMEOUT_MS);

  try {
    const response = await fetch(API_URL, {
      method: "GET",
      cache: "no-store",
      signal: controller.signal,
      headers: {
        Accept: "application/json"
      }
    });

    if (!response.ok) {
      throw new Error(`HTTP ${response.status}`);
    }

    const data = await response.json();

    if (!data || typeof data !== "object" || Array.isArray(data)) {
      throw new Error("Invalid JSON response");
    }

    return data;
  } catch (error) {
    if (error.name === "AbortError") {
      throw new Error(`Request timed out after ${REQUEST_TIMEOUT_MS} ms`);
    }
    throw error;
  } finally {
    window.clearTimeout(timeoutId);
  }
}


// ======================================================
// ĐỌC TRỰC TIẾP JSON TỪ FIRMWARE MỚI
// ======================================================

function adaptEsp32Data(raw) {
  return {
    state: raw.state,
    mode: raw.mode,
    nsLight: raw.nsLight,
    ewLight: raw.ewLight,
    nsCountdown: raw.nsCountdown,
    ewCountdown: raw.ewCountdown,
    currentCountdown: raw.currentCountdown,

    // 4 Cảm biến IR thực tế
    irNS1: raw.irNS1,
    irNS2: raw.irNS2,
    irEW1: raw.irEW1,
    irEW2: raw.irEW2,

    vehicleNS: raw.vehicleNS,
    vehicleEW: raw.vehicleEW,

    // Mật độ từ firmware
    densityNS: raw.densityNS,
    densityEW: raw.densityEW,

    // Nút người đi bộ thật
    pedestrianRequest: raw.pedestrianRequest,

    // Nút ưu tiên khẩn cấp thật
    priorityActive: raw.priorityActive,
    priorityDirection: raw.priorityDirection,

    // Uptime thực tế (đơn vị: giây)
    uptime: raw.uptime
  };
}


// ======================================================
// CHUẨN HÓA DỮ LIỆU (NORMALIZE)
// ======================================================

function normalizeData(raw, previous) {
  const fallback = previous || {};

  const booleanOrFallback = (value, oldValue) =>
    typeof value === "boolean" ? value : oldValue;

  const enumOrFallback = (value, allowed, oldValue) =>
    allowed.has(value) ? value : oldValue;

  const numberOrFallback = (value, oldValue) =>
    Number.isFinite(Number(value)) && Number(value) >= 0
      ? Number(value)
      : oldValue;

  const normalized = {
    state: enumOrFallback(raw.state, VALID_STATES, fallback.state || "NS_GREEN"),
    mode: enumOrFallback(raw.mode, VALID_MODES, fallback.mode || "NORMAL"),
    nsLight: enumOrFallback(raw.nsLight, VALID_LIGHTS, fallback.nsLight || "GREEN"),
    ewLight: enumOrFallback(raw.ewLight, VALID_LIGHTS, fallback.ewLight || "RED"),
    nsCountdown: numberOrFallback(raw.nsCountdown, fallback.nsCountdown ?? null),
    ewCountdown: numberOrFallback(raw.ewCountdown, fallback.ewCountdown ?? null),
    currentCountdown: numberOrFallback(raw.currentCountdown, fallback.currentCountdown ?? null),
    irNS1: booleanOrFallback(raw.irNS1, fallback.irNS1 ?? false),
    irNS2: booleanOrFallback(raw.irNS2, fallback.irNS2 ?? false),
    irEW1: booleanOrFallback(raw.irEW1, fallback.irEW1 ?? false),
    irEW2: booleanOrFallback(raw.irEW2, fallback.irEW2 ?? false),
    densityNS: enumOrFallback(raw.densityNS, VALID_DENSITIES, fallback.densityNS || "EMPTY"),
    densityEW: enumOrFallback(raw.densityEW, VALID_DENSITIES, fallback.densityEW || "EMPTY"),
    pedestrianRequest: booleanOrFallback(raw.pedestrianRequest, fallback.pedestrianRequest ?? false),
    priorityActive: booleanOrFallback(raw.priorityActive, fallback.priorityActive ?? false),
    priorityDirection: enumOrFallback(raw.priorityDirection, VALID_PRIORITY_DIRECTIONS, fallback.priorityDirection || "NONE"),
    uptime: numberOrFallback(raw.uptime, fallback.uptime ?? null)
  };

  // Chỉ tính mật độ dự phòng nếu firmware không gửi hoặc gửi không hợp lệ
  if (!VALID_DENSITIES.has(raw.densityNS) && typeof normalized.irNS1 === "boolean" && typeof normalized.irNS2 === "boolean") {
    normalized.densityNS = densityFromSensors(normalized.irNS1, normalized.irNS2);
  }

  if (!VALID_DENSITIES.has(raw.densityEW) && typeof normalized.irEW1 === "boolean" && typeof normalized.irEW2 === "boolean") {
    normalized.densityEW = densityFromSensors(normalized.irEW1, normalized.irEW2);
  }

  return normalized;
}


// ======================================================
// CẬP NHẬT TOÀN BỘ DASHBOARD
// ======================================================

function updateDashboard(data) {
  updateTrafficLights(data);
  updateCountdown(data);
  updateSensors(data);
  updateDensity(data);
  updateSystemState(data);
  updatePriority(data);

  if (dom.lastUpdate) {
    dom.lastUpdate.textContent = formatClockTime(new Date());
  }
}


// ======================================================
// CẬP NHẬT CỤM ĐÈN GIAO THÔNG (DỰA TRỰC TIẾP nsLight, ewLight)
// ======================================================

function updateTrafficLights(data) {
  const safeLights = getSafeLightState(data);

  // Đèn Bắc – Nam
  setDirectionLights("ns", safeLights.ns);
  if (dom.nsLightValue) {
    dom.nsLightValue.textContent = LIGHT_LABELS[safeLights.ns] || safeLights.ns;
    setLightTextClass(dom.nsLightValue, safeLights.ns);
  }

  // Đèn Đông – Tây
  setDirectionLights("ew", safeLights.ew);
  if (dom.ewLightValue) {
    dom.ewLightValue.textContent = LIGHT_LABELS[safeLights.ew] || safeLights.ew;
    setLightTextClass(dom.ewLightValue, safeLights.ew);
  }
}

function setDirectionLights(prefix, activeColor) {
  ["Red", "Yellow", "Green"].forEach((color) => {
    const el = dom[`${prefix}${color}`];
    if (!el) return;

    const isActive = color.toUpperCase() === activeColor;
    el.classList.toggle("is-active", isActive);
  });
}

function setLightTextClass(element, color) {
  element.classList.remove("state-red", "state-yellow", "state-green");
  if (color === "RED") element.classList.add("state-red");
  else if (color === "YELLOW") element.classList.add("state-yellow");
  else if (color === "GREEN") element.classList.add("state-green");
}

function getSafeLightState(data) {
  // Tránh xung đột nếu cả hai hướng cùng báo GREEN
  if (data.nsLight === "GREEN" && data.ewLight === "GREEN") {
    return { ns: "RED", ew: "RED" };
  }

  return {
    ns: VALID_LIGHTS.has(data.nsLight) ? data.nsLight : "UNKNOWN",
    ew: VALID_LIGHTS.has(data.ewLight) ? data.ewLight : "UNKNOWN"
  };
}


// ======================================================
// COUNTDOWN: ĐỒNG BỘ TỪ SERVER & LOCAL SMOOTHING
// ======================================================

function updateCountdown(data) {
  if (dom.nsCountdown) {
    dom.nsCountdown.textContent = formatCountdown(data.nsCountdown);
  }

  if (dom.ewCountdown) {
    dom.ewCountdown.textContent = formatCountdown(data.ewCountdown);
  }
}

function updateLocalCountdown() {
  if (!previousData || !lastServerUpdateTime) {
    return;
  }

  const elapsedSeconds = Math.floor(
    (Date.now() - lastServerUpdateTime) / 1000
  );

  const ns = Math.max(
    0,
    previousData.nsCountdown - elapsedSeconds
  );

  const ew = Math.max(
    0,
    previousData.ewCountdown - elapsedSeconds
  );

  if (dom.nsCountdown) {
    dom.nsCountdown.textContent = formatCountdown(ns);
  }

  if (dom.ewCountdown) {
    dom.ewCountdown.textContent = formatCountdown(ew);
  }
}


// ======================================================
// CẬP NHẬT 4 CẢM BIẾN IR & SỐ CẢM BIẾN PHÁT HIỆN
// ======================================================

function updateSensors(data) {
  // Hướng Bắc – Nam: IR_NS1 & IR_NS2
  updateSensorBadge(dom.irNS1Status, data.irNS1);
  updateSensorBadge(dom.irNS2Status, data.irNS2);

  const nsCount = countDetected(data.irNS1, data.irNS2);
  if (dom.nsSensorCount) {
    dom.nsSensorCount.textContent = `${nsCount}/2 cảm biến`;
  }

  // Hướng Đông – Tây: IR_EW1 & IR_EW2
  updateSensorBadge(dom.irEW1Status, data.irEW1);
  updateSensorBadge(dom.irEW2Status, data.irEW2);

  const ewCount = countDetected(data.irEW1, data.irEW2);
  if (dom.ewSensorCount) {
    dom.ewSensorCount.textContent = `${ewCount}/2 cảm biến`;
  }
}

function updateSensorBadge(element, isDetected) {
  if (!element) return;

  element.classList.remove("is-detected", "is-clear");

  if (isDetected === true) {
    element.textContent = "Có phương tiện";
    element.classList.add("is-detected");
  } else if (isDetected === false) {
    element.textContent = "Không có phương tiện";
    element.classList.add("is-clear");
  } else {
    element.textContent = "--";
  }
}


// ======================================================
// CẬP NHẬT MẬT ĐỘ PHƯƠNG TIỆN (EMPTY, LOW, HIGH)
// ======================================================

function updateDensity(data) {
  // Hướng Bắc – Nam
  if (dom.nsAxisDensity) {
    const text = DENSITY_LABELS[data.densityNS] || "--";
    dom.nsAxisDensity.textContent = text;
    dom.nsAxisDensity.classList.remove("density-empty", "density-low", "density-high");
    if (data.densityNS === "EMPTY") dom.nsAxisDensity.classList.add("density-empty");
    else if (data.densityNS === "LOW") dom.nsAxisDensity.classList.add("density-low");
    else if (data.densityNS === "HIGH") dom.nsAxisDensity.classList.add("density-high");
  }

  // Hướng Đông – Tây
  if (dom.ewAxisDensity) {
    const text = DENSITY_LABELS[data.densityEW] || "--";
    dom.ewAxisDensity.textContent = text;
    dom.ewAxisDensity.classList.remove("density-empty", "density-low", "density-high");
    if (data.densityEW === "EMPTY") dom.ewAxisDensity.classList.add("density-empty");
    else if (data.densityEW === "LOW") dom.ewAxisDensity.classList.add("density-low");
    else if (data.densityEW === "HIGH") dom.ewAxisDensity.classList.add("density-high");
  }
}


// ======================================================
// CẬP NHẬT TRẠNG THÁI HỆ THỐNG
// ======================================================

function updateSystemState(data) {
  // 1. Chế độ
  if (dom.summaryMode) {
    dom.summaryMode.textContent = MODE_LABELS[data.mode] || data.mode;
  }

  // 2. FSM hiện tại
  if (dom.stateTitle) {
    dom.stateTitle.textContent = data.state;
  }

  if (dom.stateDescription) {
    dom.stateDescription.textContent = STATE_DESCRIPTIONS[data.state] || "Đang xử lý...";
  }

  // 3. Người đi bộ
  if (dom.pedestrianStatusLine) {
    if (data.mode === "PEDESTRIAN") {
      dom.pedestrianStatusLine.textContent = "Đang ưu tiên qua đường";
      dom.pedestrianStatusLine.className = "system-value state-cyan";
    } else if (data.pedestrianRequest) {
      dom.pedestrianStatusLine.textContent = "Có yêu cầu qua đường";
      dom.pedestrianStatusLine.className = "system-value state-yellow";
    } else {
      dom.pedestrianStatusLine.textContent = "Không có yêu cầu";
      dom.pedestrianStatusLine.className = "system-value";
    }
  }

  // 4. Ưu tiên khẩn cấp
  if (dom.priorityStatusLine) {
    const isPriority = data.priorityActive || data.mode === "PRIORITY";
    if (isPriority) {
      const dirText = data.priorityDirection === "NS" ? "Bắc – Nam" : data.priorityDirection === "EW" ? "Đông – Tây" : "";
      dom.priorityStatusLine.textContent = `Đang kích hoạt (${dirText})`;
      dom.priorityStatusLine.className = "system-value state-red";
    } else {
      dom.priorityStatusLine.textContent = "Không hoạt động";
      dom.priorityStatusLine.className = "system-value";
    }
  }

  // 5. Thời gian hoạt động (uptime - đơn vị: giây)
  if (dom.uptimeStatusLine) {
    dom.uptimeStatusLine.textContent = formatUptime(data.uptime);
  }

  // 6. IP ESP32
  if (dom.esp32IpLine) {
    dom.esp32IpLine.textContent = ESP32_IP;
  }
}


// ======================================================
// CẬP NHẬT BANNER ƯU TIÊN KHẨN CẤP
// ======================================================

function updatePriority(data) {
  const isActive = data.priorityActive || data.mode === "PRIORITY";

  if (dom.priorityAlert) {
    dom.priorityAlert.hidden = !isActive;
  }

  if (dom.priorityBannerDirection) {
    const dir = data.priorityDirection === "NS" ? "Bắc – Nam" : data.priorityDirection === "EW" ? "Đông – Tây" : "Không xác định";
    dom.priorityBannerDirection.textContent = dir;
  }
}


// ======================================================
// TRẠNG THÁI KẾT NỐI ESP32
// ======================================================

function updateConnectionStatus(status) {
  connectionState = status;

  if (dom.connectionBadge) {
    dom.connectionBadge.classList.remove("is-connecting", "is-online", "is-offline");
    dom.connectionBadge.classList.add(`is-${status}`);
  }

  const labels = {
    connecting: "Connecting...",
    online: "Online",
    offline: "Offline"
  };

  if (dom.connectionText) {
    dom.connectionText.textContent = labels[status] || status;
  }

  if (dom.connectionAlert) {
    dom.connectionAlert.hidden = status !== "offline";
  }
}


// ======================================================
// PHÁT HIỆN SỰ KIỆN ĐỂ GHI NHẬT KÝ (THEO DÕI CẢ 4 IR)
// ======================================================

function detectEvents(previous, current) {
  if (!previous) {
    addEvent(`${current.state}: ${STATE_DESCRIPTIONS[current.state] || "Nhận trạng thái FSM"}`, "TRẠNG THÁI");
    return;
  }

  // FSM thay đổi
  if (previous.state !== current.state) {
    addEvent(`${current.state}: ${STATE_DESCRIPTIONS[current.state] || "Chuyển FSM"}`, "TRẠNG THÁI");
  }

  // Mode thay đổi
  if (previous.mode !== current.mode) {
    addEvent(`Chế độ hoạt động: ${MODE_LABELS[current.mode] || current.mode}`, "CHẾ ĐỘ");
  }

  // 4 Cảm biến IR thay đổi
  const sensorConfigs = [
    { key: "irNS1", name: "IR_NS1" },
    { key: "irNS2", name: "IR_NS2" },
    { key: "irEW1", name: "IR_EW1" },
    { key: "irEW2", name: "IR_EW2" }
  ];

  sensorConfigs.forEach(({ key, name }) => {
    if (previous[key] !== current[key] && typeof current[key] === "boolean") {
      addEvent(
        current[key] ? `${name} phát hiện phương tiện` : `${name} hết phát hiện phương tiện`,
        "CẢM BIẾN"
      );
    }
  });

  // Người đi bộ
  if (!previous.pedestrianRequest && current.pedestrianRequest) {
    addEvent("Nhận yêu cầu người đi bộ qua đường", "YÊU CẦU");
  } else if (previous.pedestrianRequest && !current.pedestrianRequest) {
    addEvent("Hết yêu cầu người đi bộ qua đường", "YÊU CẦU");
  }

  // Ưu tiên khẩn cấp
  if (!previous.priorityActive && current.priorityActive) {
    const dir = current.priorityDirection === "NS" ? "Bắc – Nam" : current.priorityDirection === "EW" ? "Đông – Tây" : "Khẩn cấp";
    addEvent(`Kích hoạt ưu tiên khẩn cấp: Hướng ${dir}`, "CẢNH BÁO");
  } else if (previous.priorityActive && !current.priorityActive) {
    addEvent("Hủy chế độ ưu tiên khẩn cấp", "CHẾ ĐỘ");
  } else if (current.priorityActive && previous.priorityDirection !== current.priorityDirection) {
    const dir = current.priorityDirection === "NS" ? "Bắc – Nam" : "Đông – Tây";
    addEvent(`Đổi hướng ưu tiên khẩn cấp: Hướng ${dir}`, "CẢNH BÁO");
  }
}


// ======================================================
// THÊM SỰ KIỆN VÀO NHẬT KÝ (GIỚI HẠN 5 DÒNG)
// ======================================================

function addEvent(message, type = "HỆ THỐNG") {
  if (!dom.eventLogBody) return;

  const emptyRow = document.getElementById("emptyEventRow");
  if (emptyRow) {
    emptyRow.remove();
  }

  const row = document.createElement("tr");
  const normalizedType = String(type).toUpperCase();

  if (normalizedType.includes("CẢNH BÁO") || normalizedType.includes("ALERT")) {
    row.className = "event-alert";
  } else if (normalizedType.includes("CẢM BIẾN") || normalizedType.includes("SENSOR")) {
    row.className = "event-sensor";
  } else if (normalizedType.includes("CHẾ ĐỘ") || normalizedType.includes("TRẠNG THÁI") || normalizedType.includes("KẾT NỐI")) {
    row.className = "event-mode";
  }

  [formatClockTime(new Date()), normalizedType, message].forEach((value) => {
    const cell = document.createElement("td");
    cell.textContent = value;
    row.appendChild(cell);
  });

  dom.eventLogBody.prepend(row);

  while (dom.eventLogBody.children.length > MAX_EVENTS) {
    dom.eventLogBody.lastElementChild.remove();
  }
}


// ======================================================
// CÁC HÀM TIỆN ÍCH (HELPERS)
// ======================================================

function densityFromSensors(first, second) {
  const count = Number(Boolean(first)) + Number(Boolean(second));
  return count === 0 ? "EMPTY" : count === 1 ? "LOW" : "HIGH";
}

function countDetected(first, second) {
  return Number(Boolean(first)) + Number(Boolean(second));
}

function formatCountdown(value) {
  if (!Number.isFinite(value)) return "--";
  return String(Math.max(0, Math.floor(value))).padStart(2, "0");
}

function formatUptime(totalSeconds) {
  if (!Number.isFinite(totalSeconds) || totalSeconds < 0) {
    return "--:--:--";
  }
  const s = Math.floor(totalSeconds);
  const days = Math.floor(s / 86400);
  const hours = Math.floor((s % 86400) / 3600);
  const minutes = Math.floor((s % 3600) / 60);
  const seconds = s % 60;

  const timeStr = `${String(hours).padStart(2, "0")}:${String(minutes).padStart(2, "0")}:${String(seconds).padStart(2, "0")}`;
  if (days > 0) {
    return `${days} ngày ${timeStr}`;
  }
  return timeStr;
}

function formatClockTime(date) {
  return date.toLocaleTimeString("en-GB", {
    hour: "2-digit",
    minute: "2-digit",
    second: "2-digit",
    hour12: false
  });
}


// ======================================================
// DỮ LIỆU GIẢ LẬP ĐỂ TEST GIAO DIỆN (CHỈ DÙNG KHI MOCK_MODE = TRUE)
// KHÔNG CÓ ALL-RED, ĐẦY ĐỦ 4 IR, UPTIME GIÂY
// ======================================================

function getMockData() {
  const phases = [
    { state: "NS_GREEN", nsLight: "GREEN", ewLight: "RED" },
    { state: "NS_YELLOW", nsLight: "YELLOW", ewLight: "RED" },
    { state: "EW_GREEN", nsLight: "RED", ewLight: "GREEN" },
    { state: "EW_YELLOW", nsLight: "RED", ewLight: "YELLOW" }
  ];

  const phaseIndex = Math.floor(mockTick / 16) % phases.length;
  const phase = phases[phaseIndex];
  const phaseCountdown = 8 - Math.floor((mockTick % 16) / 2);

  mockTick += 1;

  const irNS1 = (mockTick % 10) < 6;
  const irNS2 = (mockTick % 10) < 3;
  const irEW1 = (mockTick % 8) < 4;
  const irEW2 = (mockTick % 8) < 2;

  const densityNS = densityFromSensors(irNS1, irNS2);
  const densityEW = densityFromSensors(irEW1, irEW2);

  return {
    ...phase,
    mode: "NORMAL",
    nsCountdown: phaseCountdown,
    ewCountdown: phaseCountdown,
    currentCountdown: phaseCountdown,

    irNS1,
    irNS2,
    irEW1,
    irEW2,

    vehicleNS: irNS1 || irNS2,
    vehicleEW: irEW1 || irEW2,

    densityNS,
    densityEW,

    pedestrianRequest: false,
    priorityActive: false,
    priorityDirection: "NONE",

    uptime: Math.floor(Date.now() / 1000) % 250000
  };
}