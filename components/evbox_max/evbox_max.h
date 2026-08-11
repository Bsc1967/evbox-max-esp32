#pragma once

#include "controller.h"
#include "protocol.h"
#include <cmath>
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/components/uart/uart.h"
#include "esphome/core/component.h"
#include "esphome/core/gpio.h"
#include "esphome/core/preferences.h"

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
  void set_max_current(float current);
  void set_charger_breaker_current(float current);
  void set_main_fuse_current(float current);
  void set_manual_current(float current);
  void set_failsafe_current(float current);
  ChargingMode get_mode() const { return this->controller_.mode(); }
  FailsafeMode get_failsafe_mode() const { return this->controller_.failsafe_mode(); }
  float get_max_current() const { return this->inputs_.max_current; }
  float get_charger_breaker_current() const { return this->inputs_.charger_breaker_current; }
  float get_main_fuse_current() const { return this->inputs_.main_fuse_current; }
  float get_manual_current() const { return this->inputs_.manual_current; }
  float get_failsafe_current() const { return this->controller_.failsafe_current(); }
  bool get_pv_enabled() const { return this->inputs_.pv_enabled; }
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
  void set_pv_enabled(bool enabled);
  void start_session();
  void stop_session();

 protected:
  struct StoredSettings {
    uint32_t magic{0};
    uint16_t version{0};
    uint8_t mode{CHARGING_MODE_MANUAL};
    uint8_t failsafe_mode{FAILSAFE_MODE_LIMIT_6A};
    bool pv_enabled{false};
    float manual_current{6.0f};
    float max_current{16.0f};
    float charger_breaker_current{16.0f};
    float main_fuse_current{25.0f};
    float failsafe_current{6.0f};
  };

  void handle_frame_(const Frame &frame);
  void transition_(EvboxState state);
  void load_settings_();
  void save_settings_();
  StoredSettings current_settings_() const;
  void apply_settings_(const StoredSettings &settings);
  void update_meter_from_state_(const std::string &data);
  void send_packet_(uint8_t dst, uint8_t cmd, const std::string &data = {});
  void send_restart_registration_();
  void send_connection_state_();
  void send_status_update_request_();
  void send_heartbeat_();
  void send_current_setpoint_(float amps);
  uint32_t seconds_since_2000_() const;
  void watchdog_();
  void publish_();

  const char *state_name_() const;
  const char *communication_name_() const;

  FrameParser parser_{};
  ChargeController controller_{};
  ControlInputs inputs_{};
  EvboxState state_{BOOT};
  uint8_t chargebox_address_{0x00};
  std::string chargebox_serial_{};
  uint16_t chargebox_firmware_{0};
  uint32_t session_{1};
  bool evbox_online_{false};
  bool session_active_{false};
  bool settings_restored_{false};
  bool have_session_start_meter_{false};
  float active_current_{0.0f};
  float session_energy_kwh_{0.0f};
  float meter_value_kwh_{0.0f};
  float session_start_meter_kwh_{0.0f};
  float temperature_c_{NAN};
  uint32_t heartbeat_interval_ms_{5000};
  uint32_t watchdog_timeout_ms_{30000};
  uint32_t last_rx_ms_{0};
  uint32_t last_heartbeat_ms_{0};
  uint32_t last_publish_ms_{0};
  ESPPreferenceObject settings_pref_{};

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
