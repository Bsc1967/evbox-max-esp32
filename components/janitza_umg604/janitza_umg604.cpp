#include "janitza_umg604.h"
#include "../evbox_max/evbox_max.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

namespace esphome {
namespace janitza_umg604 {

static const char *const TAG = "janitza_umg604";

void JanitzaUmg604Component::setup() {
  this->online_ = false;
}

void JanitzaUmg604Component::update() {
  const bool ok = this->read_live_registers_();
  this->online_ = ok;

  if (ok) {
    const uint8_t phase_mask = this->detect_charge_phase_mask_();
    const uint8_t detected = this->count_charge_phases_(phase_mask);
    if (detected >= 1 && detected <= 3) {
      this->detected_charge_phase_mask_ = phase_mask;
      this->detected_charge_phases_ = detected;
      if (this->evbox_parent_ != nullptr) {
        this->evbox_parent_->set_charge_phases(detected);
        this->evbox_parent_->set_active_phase_mask(phase_mask);
      }
      if (this->detected_charge_phases_sensor_ != nullptr) {
        this->detected_charge_phases_sensor_->publish_state(detected);
      }
    }
  }

  if (this->evbox_parent_ != nullptr) {
    this->evbox_parent_->update_janitza(this->import_power_w_, this->export_power_w_, this->l1_current_a_,
                                        this->l2_current_a_, this->l3_current_a_, ok);
  }
  this->publish_status_();
}

bool JanitzaUmg604Component::read_live_registers_() {
  float value = 0.0f;

  const uint16_t start = std::min({
    this->reg_l1_voltage_, this->reg_l2_voltage_, this->reg_l3_voltage_,
    this->reg_l1_current_, this->reg_l2_current_, this->reg_l3_current_,
    this->reg_total_power_,
  });
  const uint16_t end = std::max({
    this->reg_l1_voltage_, this->reg_l2_voltage_, this->reg_l3_voltage_,
    this->reg_l1_current_, this->reg_l2_current_, this->reg_l3_current_,
    this->reg_total_power_,
  });
  const uint16_t words = end - start + 2;

  // Fast path: the proven UMG604 live registers are close together
  // (1317..1369). One Modbus read gives all values, cutting normal response
  // time from several TCP round-trips to one.
  std::vector<uint16_t> registers;
  if (words <= 124 && this->read_holding_registers_(start, words, &registers)) {
    if (this->decode_float_(registers, start, this->reg_l1_current_, &value)) {
      this->l1_current_a_ = value;
      if (this->l1_current_sensor_ != nullptr) {
        this->l1_current_sensor_->publish_state(value);
      }
    }
    if (this->decode_float_(registers, start, this->reg_l2_current_, &value)) {
      this->l2_current_a_ = value;
      if (this->l2_current_sensor_ != nullptr) {
        this->l2_current_sensor_->publish_state(value);
      }
    }
    if (this->decode_float_(registers, start, this->reg_l3_current_, &value)) {
      this->l3_current_a_ = value;
      if (this->l3_current_sensor_ != nullptr) {
        this->l3_current_sensor_->publish_state(value);
      }
    }
    if (this->decode_float_(registers, start, this->reg_l1_voltage_, &value) && this->l1_voltage_sensor_ != nullptr)
      this->l1_voltage_sensor_->publish_state(value);
    if (this->decode_float_(registers, start, this->reg_l2_voltage_, &value) && this->l2_voltage_sensor_ != nullptr)
      this->l2_voltage_sensor_->publish_state(value);
    if (this->decode_float_(registers, start, this->reg_l3_voltage_, &value) && this->l3_voltage_sensor_ != nullptr)
      this->l3_voltage_sensor_->publish_state(value);
    if (!this->decode_float_(registers, start, this->reg_total_power_, &value)) {
      return false;
    }
  } else {
    // Fallback for unusual custom register maps that are too far apart for a
    // compact read. This keeps configurability, but the normal path should be
    // the single-request read above.
    if (!this->read_float_register_(this->reg_l1_current_, &value)) return false;
    this->l1_current_a_ = value;
    if (this->l1_current_sensor_ != nullptr) this->l1_current_sensor_->publish_state(value);
    if (!this->read_float_register_(this->reg_l2_current_, &value)) return false;
    this->l2_current_a_ = value;
    if (this->l2_current_sensor_ != nullptr) this->l2_current_sensor_->publish_state(value);
    if (!this->read_float_register_(this->reg_l3_current_, &value)) return false;
    this->l3_current_a_ = value;
    if (this->l3_current_sensor_ != nullptr) this->l3_current_sensor_->publish_state(value);
    if (!this->read_float_register_(this->reg_l1_voltage_, &value)) return false;
    if (this->l1_voltage_sensor_ != nullptr) this->l1_voltage_sensor_->publish_state(value);
    if (!this->read_float_register_(this->reg_l2_voltage_, &value)) return false;
    if (this->l2_voltage_sensor_ != nullptr) this->l2_voltage_sensor_->publish_state(value);
    if (!this->read_float_register_(this->reg_l3_voltage_, &value)) return false;
    if (this->l3_voltage_sensor_ != nullptr) this->l3_voltage_sensor_->publish_state(value);
    if (!this->read_float_register_(this->reg_total_power_, &value)) return false;
  }

  if (this->total_power_sensor_ != nullptr) this->total_power_sensor_->publish_state(value);
  this->import_power_w_ = value > 0.0f ? value : 0.0f;
  this->export_power_w_ = value < 0.0f ? -value : 0.0f;
  if (this->import_power_sensor_ != nullptr) this->import_power_sensor_->publish_state(this->import_power_w_);
  if (this->export_power_sensor_ != nullptr) this->export_power_sensor_->publish_state(this->export_power_w_);
  return true;
}

void JanitzaUmg604Component::dump_config() {
  ESP_LOGCONFIG(TAG, "Janitza UMG604 Modbus TCP");
  ESP_LOGCONFIG(TAG, "  Host: %s:%u", this->host_.c_str(), this->port_);
  ESP_LOGCONFIG(TAG, "  Unit ID: %u", this->unit_id_);
}

bool JanitzaUmg604Component::read_float_register_(uint16_t address, float *value) {
  uint8_t response[13]{};
  if (!this->modbus_request_(address, 2, response, sizeof(response))) {
    return false;
  }

  // Function 0x03 returns holding registers. For a 32-bit float we expect four
  // data bytes after the MBAP header, unit id, function code, and byte count.
  if (response[7] != 0x03 || response[8] != 4) {
    ESP_LOGW(TAG, "Unexpected Modbus response for register %u", address);
    return false;
  }

  // Janitza/Modbus sends register bytes in network order. ESP32 stores floats
  // little-endian, so first build the 32-bit IEEE-754 bit pattern as an integer
  // value and then copy that bit pattern into the float.
  //
  // Example: 230.0 V is 0x43660000 on the wire as 43 66 00 00. Building the
  // integer 0x43660000 and memcpy'ing the integer value yields the correct
  // float on the ESP32.
  const uint32_t raw = (static_cast<uint32_t>(response[9]) << 24) |
                       (static_cast<uint32_t>(response[10]) << 16) |
                       (static_cast<uint32_t>(response[11]) << 8) |
                       static_cast<uint32_t>(response[12]);
  std::memcpy(value, &raw, sizeof(float));
  return true;
}

bool JanitzaUmg604Component::read_holding_registers_(uint16_t address, uint16_t words, std::vector<uint16_t> *registers) {
  std::vector<uint8_t> response(9 + words * 2);
  if (!this->modbus_request_(address, words, response.data(), response.size())) {
    return false;
  }

  if (response[7] != 0x03 || response[8] != words * 2) {
    ESP_LOGW(TAG, "Unexpected Modbus response for registers %u..%u", address, address + words - 1);
    return false;
  }

  registers->clear();
  registers->reserve(words);
  for (uint16_t i = 0; i < words; i++) {
    const size_t offset = 9 + i * 2;
    registers->push_back(static_cast<uint16_t>((response[offset] << 8) | response[offset + 1]));
  }
  return true;
}

bool JanitzaUmg604Component::decode_float_(const std::vector<uint16_t> &registers, uint16_t start_address, uint16_t address, float *value) const {
  const uint16_t index = address - start_address;
  if (index + 1 >= registers.size()) {
    return false;
  }

  const uint32_t raw = (static_cast<uint32_t>(registers[index]) << 16) | registers[index + 1];
  std::memcpy(value, &raw, sizeof(float));
  return true;
}

bool JanitzaUmg604Component::modbus_request_(uint16_t address, uint16_t words, uint8_t *response, size_t response_len) {
  // ESPHome does not need a full Modbus hub here: the UMG604 is read over a
  // plain TCP socket using one request/response per register group.
  int fd = lwip_socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
  if (fd < 0) {
    ESP_LOGW(TAG, "Unable to create socket");
    return false;
  }

  timeval timeout{};
  timeout.tv_sec = 2;
  lwip_setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  lwip_setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(this->port_);
  addr.sin_addr.s_addr = inet_addr(this->host_.c_str());

  if (lwip_connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
    lwip_close(fd);
    ESP_LOGW(TAG, "Unable to connect to %s:%u", this->host_.c_str(), this->port_);
    return false;
  }

  // MBAP header + Modbus PDU:
  // transaction, protocol=0, length=6, unit id, function=0x03,
  // start register, register count.
  const uint16_t tx = this->transaction_id_();
  uint8_t request[12] = {
    static_cast<uint8_t>((tx >> 8) & 0xFF), static_cast<uint8_t>(tx & 0xFF),
    0x00, 0x00,
    0x00, 0x06,
    this->unit_id_,
    0x03,
    static_cast<uint8_t>((address >> 8) & 0xFF), static_cast<uint8_t>(address & 0xFF),
    static_cast<uint8_t>((words >> 8) & 0xFF), static_cast<uint8_t>(words & 0xFF),
  };

  const int sent = lwip_send(fd, request, sizeof(request), 0);
  if (sent != sizeof(request)) {
    lwip_close(fd);
    return false;
  }

  size_t received_total = 0;
  while (received_total < response_len) {
    const int received = lwip_recv(fd, response + received_total, response_len - received_total, 0);
    if (received <= 0) {
      break;
    }
    received_total += static_cast<size_t>(received);
  }
  lwip_close(fd);
  return received_total == response_len;
}

uint16_t JanitzaUmg604Component::transaction_id_() {
  return this->next_transaction_id_++;
}

uint8_t JanitzaUmg604Component::detect_charge_phase_mask_() const {
  uint8_t mask = 0;
  if (std::fabs(this->l1_current_a_) >= this->phase_detect_current_) mask |= 0x01;
  if (std::fabs(this->l2_current_a_) >= this->phase_detect_current_) mask |= 0x02;
  if (std::fabs(this->l3_current_a_) >= this->phase_detect_current_) mask |= 0x04;

  // No active phase usually means no charging or meter noise. Keep the last
  // known mask so the controller does not bounce to a meaningless zero-phase
  // state between sessions.
  return mask != 0 ? mask : this->detected_charge_phase_mask_;
}

uint8_t JanitzaUmg604Component::count_charge_phases_(uint8_t phase_mask) const {
  uint8_t count = 0;
  if ((phase_mask & 0x01) != 0) count++;
  if ((phase_mask & 0x02) != 0) count++;
  if ((phase_mask & 0x04) != 0) count++;
  return count;
}

void JanitzaUmg604Component::publish_status_() {
  if (this->communication_text_sensor_ != nullptr) {
    this->communication_text_sensor_->publish_state(this->online_ ? "Janitza online" : "Janitza offline");
  }
}

}  // namespace janitza_umg604
}  // namespace esphome
