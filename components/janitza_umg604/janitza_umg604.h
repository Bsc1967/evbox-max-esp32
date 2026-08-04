#pragma once

#include <cstdint>
#include <vector>
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/core/component.h"

namespace esphome {
namespace evbox_max {
class EvboxMaxComponent;
}
namespace janitza_umg604 {

class JanitzaUmg604Component : public PollingComponent {
 public:
  void setup() override;
  void update() override;
  void dump_config() override;
  uint16_t get_warn_if_blocking_over() const override { return 500; }

  void set_host(const std::string &host) { this->host_ = host; }
  void set_port(uint16_t port) { this->port_ = port; }
  void set_unit_id(uint8_t unit_id) { this->unit_id_ = unit_id; }
  void set_poll_interval_ms(uint32_t interval) { this->set_update_interval(interval); }

  void set_register_l1_current(uint16_t value) { this->reg_l1_current_ = value; }
  void set_register_l2_current(uint16_t value) { this->reg_l2_current_ = value; }
  void set_register_l3_current(uint16_t value) { this->reg_l3_current_ = value; }
  void set_register_l1_voltage(uint16_t value) { this->reg_l1_voltage_ = value; }
  void set_register_l2_voltage(uint16_t value) { this->reg_l2_voltage_ = value; }
  void set_register_l3_voltage(uint16_t value) { this->reg_l3_voltage_ = value; }
  void set_register_total_power(uint16_t value) { this->reg_total_power_ = value; }
  void set_register_import_power(uint16_t value) { this->reg_import_power_ = value; }
  void set_register_export_power(uint16_t value) { this->reg_export_power_ = value; }

  void set_l1_current_sensor(sensor::Sensor *sensor) { this->l1_current_sensor_ = sensor; }
  void set_l2_current_sensor(sensor::Sensor *sensor) { this->l2_current_sensor_ = sensor; }
  void set_l3_current_sensor(sensor::Sensor *sensor) { this->l3_current_sensor_ = sensor; }
  void set_l1_voltage_sensor(sensor::Sensor *sensor) { this->l1_voltage_sensor_ = sensor; }
  void set_l2_voltage_sensor(sensor::Sensor *sensor) { this->l2_voltage_sensor_ = sensor; }
  void set_l3_voltage_sensor(sensor::Sensor *sensor) { this->l3_voltage_sensor_ = sensor; }
  void set_total_power_sensor(sensor::Sensor *sensor) { this->total_power_sensor_ = sensor; }
  void set_import_power_sensor(sensor::Sensor *sensor) { this->import_power_sensor_ = sensor; }
  void set_export_power_sensor(sensor::Sensor *sensor) { this->export_power_sensor_ = sensor; }
  void set_communication_text_sensor(text_sensor::TextSensor *sensor) { this->communication_text_sensor_ = sensor; }
  void set_evbox_parent(evbox_max::EvboxMaxComponent *parent) { this->evbox_parent_ = parent; }

  bool online() const { return this->online_; }
  float import_power_w() const { return this->import_power_w_; }
  float export_power_w() const { return this->export_power_w_; }

 protected:
  bool read_float_register_(uint16_t address, float *value);
  bool read_live_registers_();
  bool read_holding_registers_(uint16_t address, uint16_t words, std::vector<uint16_t> *registers);
  bool decode_float_(const std::vector<uint16_t> &registers, uint16_t start_address, uint16_t address, float *value) const;
  bool modbus_request_(uint16_t address, uint16_t words, uint8_t *response, size_t response_len);
  uint16_t transaction_id_();
  void publish_status_();

  std::string host_{};
  uint16_t port_{502};
  uint8_t unit_id_{1};
  bool online_{false};
  uint16_t next_transaction_id_{1};

  uint16_t reg_l1_current_{1325};
  uint16_t reg_l2_current_{1327};
  uint16_t reg_l3_current_{1329};
  uint16_t reg_l1_voltage_{1317};
  uint16_t reg_l2_voltage_{1319};
  uint16_t reg_l3_voltage_{1321};
  uint16_t reg_total_power_{1369};
  uint16_t reg_import_power_{1369};
  uint16_t reg_export_power_{1369};

  float import_power_w_{0.0f};
  float export_power_w_{0.0f};

  sensor::Sensor *l1_current_sensor_{nullptr};
  sensor::Sensor *l2_current_sensor_{nullptr};
  sensor::Sensor *l3_current_sensor_{nullptr};
  sensor::Sensor *l1_voltage_sensor_{nullptr};
  sensor::Sensor *l2_voltage_sensor_{nullptr};
  sensor::Sensor *l3_voltage_sensor_{nullptr};
  sensor::Sensor *total_power_sensor_{nullptr};
  sensor::Sensor *import_power_sensor_{nullptr};
  sensor::Sensor *export_power_sensor_{nullptr};
  text_sensor::TextSensor *communication_text_sensor_{nullptr};
  evbox_max::EvboxMaxComponent *evbox_parent_{nullptr};
};

}  // namespace janitza_umg604
}  // namespace esphome
