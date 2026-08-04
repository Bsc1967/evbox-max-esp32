#include "janitza_umg604.h"
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
  float value = 0.0f;
  bool ok = true;

  ok &= this->read_float_register_(this->reg_l1_current_, &value);
  if (ok && this->l1_current_sensor_ != nullptr) this->l1_current_sensor_->publish_state(value);
  ok &= this->read_float_register_(this->reg_l2_current_, &value);
  if (ok && this->l2_current_sensor_ != nullptr) this->l2_current_sensor_->publish_state(value);
  ok &= this->read_float_register_(this->reg_l3_current_, &value);
  if (ok && this->l3_current_sensor_ != nullptr) this->l3_current_sensor_->publish_state(value);
  ok &= this->read_float_register_(this->reg_l1_voltage_, &value);
  if (ok && this->l1_voltage_sensor_ != nullptr) this->l1_voltage_sensor_->publish_state(value);
  ok &= this->read_float_register_(this->reg_l2_voltage_, &value);
  if (ok && this->l2_voltage_sensor_ != nullptr) this->l2_voltage_sensor_->publish_state(value);
  ok &= this->read_float_register_(this->reg_l3_voltage_, &value);
  if (ok && this->l3_voltage_sensor_ != nullptr) this->l3_voltage_sensor_->publish_state(value);
  ok &= this->read_float_register_(this->reg_total_power_, &value);
  if (ok && this->total_power_sensor_ != nullptr) this->total_power_sensor_->publish_state(value);
  ok &= this->read_float_register_(this->reg_import_power_, &this->import_power_w_);
  if (ok && this->import_power_sensor_ != nullptr) this->import_power_sensor_->publish_state(this->import_power_w_);
  ok &= this->read_float_register_(this->reg_export_power_, &this->export_power_w_);
  if (ok && this->export_power_sensor_ != nullptr) this->export_power_sensor_->publish_state(this->export_power_w_);

  this->online_ = ok;
  this->publish_status_();
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

  if (response[7] != 0x03 || response[8] != 4) {
    ESP_LOGW(TAG, "Unexpected Modbus response for register %u", address);
    return false;
  }

  uint8_t ordered[4] = {response[9], response[10], response[11], response[12]};
  std::memcpy(value, ordered, sizeof(float));
  return true;
}

bool JanitzaUmg604Component::modbus_request_(uint16_t address, uint16_t words, uint8_t *response, size_t response_len) {
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

  const int received = lwip_recv(fd, response, response_len, 0);
  lwip_close(fd);
  return received >= 9;
}

uint16_t JanitzaUmg604Component::transaction_id_() {
  return this->next_transaction_id_++;
}

void JanitzaUmg604Component::publish_status_() {
  if (this->communication_text_sensor_ != nullptr) {
    this->communication_text_sensor_->publish_state(this->online_ ? "Janitza online" : "Janitza offline");
  }
}

}  // namespace janitza_umg604
}  // namespace esphome
