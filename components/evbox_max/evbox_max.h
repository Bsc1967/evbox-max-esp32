#pragma once

#include "controller.h"
#include "protocol.h"
#include <cmath>
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/components/uart/uart.h"
#include "esphome/core/component.h"
#include "esphome/core/gpio.h"

namespace esphome {
namespace evbox_max {

enum EvboxState : uint8_t {
  BOOT = 0,
  WAIT_REGISTRATION,
  ASSIGN_ADDRESS,
  READ_INFO,
  READ_CONFIG,
  IDLE,
  AUTHORIZED,
  STARTING,
  CHARGING,
  PAUSED,
  FINISHING,
  FAULT,
};

class EvboxMaxComponent : public Component, public uart::UARTDevice {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;

  void set_mode(ChargingMode mode);
  void set_failsafe_mode(FailsafeMode mode);
  void set_max_current(float current) { this->inputs_.max_current = current; }
  void set_charger_breaker_current(float current) { this->inputs_.charger_breaker_current = current; }
  void set_main_fuse_current(float current) { this->inputs_.main_fuse_current = current; }
  void set_manual_current(float current) { this->inputs_.manual_current = current; }
  void set_failsafe_current(float current);
  void set_charge_phases(uint8_t phases) {
    const uint8_t clamped = phases < 1 ? 1 : (phases > 3 ? 3 : phases);
    this->inputs_.charge_phases = clamped;
    this->inputs_.active_phase_mask = clamped == 1 ? 0x01 : (clamped == 2 ? 0x03 : 0x07);
  }
  void set_active_phase_mask(uint8_t mask) { this->inputs_.active_phase_mask = mask & 0x07; }
  void set_heartbeat_interval(uint32_t interval) { this->heartbeat_interval_ms_ = interval; }
  void set_watchdog_timeout(uint32_t timeout) { this->watchdog_timeout_ms_ = timeout; }
  void set_rs485_de_pin(GPIOPin *pin) { this->rs485_de_pin_ = pin; }

  void set_status_text_sensor(text_sensor::TextSensor *sensor) { this->status_text_sensor_ = sensor; }
  void set_state_text_sensor(text_sensor::TextSensor *sensor) { this->state_text_sensor_ = sensor; }
  void set_communication_text_sensor(text_sensor::TextSensor *sensor) { this->communication_text_sensor_ = sensor; }
  void set_ev_current_sensor(sensor::Sensor *sensor) { this->ev_current_sensor_ = sensor; }
  void set_session_energy_sensor(sensor::Sensor *sensor) { this->session_energy_sensor_ = sensor; }
  void set_meter_value_sensor(sensor::Sensor *sensor) { this->meter_value_sensor_ = sensor; }
  void set_temperature_sensor(sensor::Sensor *sensor) { this->temperature_sensor_ = sensor; }

  void update_janitza(float import_w, float export_w, float l1_current, float l2_current, float l3_current, bool online);
  void set_pv_enabled(bool enabled) { this->inputs_.pv_enabled = enabled; }
  void start_session();
  void stop_session();

 protected:
  void handle_frame_(const Frame &frame);
  void transition_(EvboxState state);
  void send_frame_(FrameType type, const std::vector<uint8_t> &payload = {});
  void send_heartbeat_();
  void send_current_setpoint_(float amps);
  void watchdog_();
  void publish_();

  const char *state_name_() const;
  const char *communication_name_() const;

  FrameParser parser_{};
  ChargeController controller_{};
  ControlInputs inputs_{};
  EvboxState state_{BOOT};
  uint8_t chargebox_address_{0x00};
  bool evbox_online_{false};
  bool session_active_{false};
  float active_current_{0.0f};
  float session_energy_kwh_{0.0f};
  float meter_value_kwh_{0.0f};
  float temperature_c_{NAN};
  uint32_t heartbeat_interval_ms_{5000};
  uint32_t watchdog_timeout_ms_{30000};
  uint32_t last_rx_ms_{0};
  uint32_t last_heartbeat_ms_{0};
  uint32_t last_publish_ms_{0};

  text_sensor::TextSensor *status_text_sensor_{nullptr};
  text_sensor::TextSensor *state_text_sensor_{nullptr};
  text_sensor::TextSensor *communication_text_sensor_{nullptr};
  sensor::Sensor *ev_current_sensor_{nullptr};
  sensor::Sensor *session_energy_sensor_{nullptr};
  sensor::Sensor *meter_value_sensor_{nullptr};
  sensor::Sensor *temperature_sensor_{nullptr};
  GPIOPin *rs485_de_pin_{nullptr};
};

}  // namespace evbox_max
}  // namespace esphome
