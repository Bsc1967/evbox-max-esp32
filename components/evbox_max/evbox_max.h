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
  PREPARING,
  AUTHORIZED,
  STARTING,
  SESSION_STARTING,
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
  void set_commissioning_mode(bool enabled) { this->commissioning_mode_ = enabled; }
  void set_charge_phases(uint8_t phases) {
    const uint8_t clamped = phases < 1 ? 1 : (phases > 3 ? 3 : phases);
    this->inputs_.charge_phases = clamped;
    this->inputs_.active_phase_mask = clamped == 1 ? 0x01 : (clamped == 2 ? 0x03 : 0x07);
    this->evbox_detected_charge_phases_ = clamped;
    this->evbox_active_phase_mask_ = this->inputs_.active_phase_mask;
    this->update_ev_measurements_();
  }
  void set_active_phase_mask(uint8_t mask) {
    this->inputs_.active_phase_mask = mask & 0x07;
    this->evbox_active_phase_mask_ = this->inputs_.active_phase_mask;
    this->evbox_detected_charge_phases_ = this->count_phases_(this->inputs_.active_phase_mask);
    this->update_ev_measurements_();
  }
  void set_heartbeat_interval(uint32_t interval) { this->heartbeat_interval_ms_ = interval; }
  void set_watchdog_timeout(uint32_t timeout) { this->watchdog_timeout_ms_ = timeout; }
  void set_rs485_de_pin(GPIOPin *pin) { this->rs485_de_pin_ = pin; }
  void set_relay_evbox_known_pin(GPIOPin *pin) { this->relay_evbox_known_pin_ = pin; }
  void set_relay_janitza_ok_pin(GPIOPin *pin) { this->relay_janitza_ok_pin_ = pin; }
  void set_relay_charging_active_pin(GPIOPin *pin) { this->relay_charging_active_pin_ = pin; }
  void set_relay_failsafe_pin(GPIOPin *pin) { this->relay_failsafe_pin_ = pin; }

  void set_status_text_sensor(text_sensor::TextSensor *sensor) { this->status_text_sensor_ = sensor; }
  void set_state_text_sensor(text_sensor::TextSensor *sensor) { this->state_text_sensor_ = sensor; }
  void set_communication_text_sensor(text_sensor::TextSensor *sensor) { this->communication_text_sensor_ = sensor; }
  void set_protocol_profile_text_sensor(text_sensor::TextSensor *sensor) { this->protocol_profile_text_sensor_ = sensor; }
  void set_cb_serial_text_sensor(text_sensor::TextSensor *sensor) { this->cb_serial_text_sensor_ = sensor; }
  void set_cb_status_detail_text_sensor(text_sensor::TextSensor *sensor) { this->cb_status_detail_text_sensor_ = sensor; }
  void set_current_request_state_text_sensor(text_sensor::TextSensor *sensor) {
    this->current_request_state_text_sensor_ = sensor;
  }
  void set_ev_current_sensor(sensor::Sensor *sensor) { this->ev_current_sensor_ = sensor; }
  void set_current_limit_sensor(sensor::Sensor *sensor) { this->current_limit_sensor_ = sensor; }
  void set_desired_current_sensor(sensor::Sensor *sensor) { this->desired_current_sensor_ = sensor; }
  void set_commanded_current_sensor(sensor::Sensor *sensor) { this->commanded_current_sensor_ = sensor; }
  void set_returned_current_limit_sensor(sensor::Sensor *sensor) { this->returned_current_limit_sensor_ = sensor; }
  void set_l1_current_sensor(sensor::Sensor *sensor) { this->l1_current_sensor_ = sensor; }
  void set_l2_current_sensor(sensor::Sensor *sensor) { this->l2_current_sensor_ = sensor; }
  void set_l3_current_sensor(sensor::Sensor *sensor) { this->l3_current_sensor_ = sensor; }
  void set_l1_voltage_sensor(sensor::Sensor *sensor) { this->l1_voltage_sensor_ = sensor; }
  void set_l2_voltage_sensor(sensor::Sensor *sensor) { this->l2_voltage_sensor_ = sensor; }
  void set_l3_voltage_sensor(sensor::Sensor *sensor) { this->l3_voltage_sensor_ = sensor; }
  void set_l1_power_factor_sensor(sensor::Sensor *sensor) { this->l1_power_factor_sensor_ = sensor; }
  void set_l2_power_factor_sensor(sensor::Sensor *sensor) { this->l2_power_factor_sensor_ = sensor; }
  void set_l3_power_factor_sensor(sensor::Sensor *sensor) { this->l3_power_factor_sensor_ = sensor; }
  void set_detected_charge_phases_sensor(sensor::Sensor *sensor) { this->detected_charge_phases_sensor_ = sensor; }
  void set_active_phase_mask_sensor(sensor::Sensor *sensor) { this->active_phase_mask_sensor_ = sensor; }
  void set_power_sensor(sensor::Sensor *sensor) { this->power_sensor_ = sensor; }
  void set_session_energy_sensor(sensor::Sensor *sensor) { this->session_energy_sensor_ = sensor; }
  void set_meter_value_sensor(sensor::Sensor *sensor) { this->meter_value_sensor_ = sensor; }
  void set_temperature_sensor(sensor::Sensor *sensor) { this->temperature_sensor_ = sensor; }
  void set_cb_firmware_sensor(sensor::Sensor *sensor) { this->cb_firmware_sensor_ = sensor; }
  void set_cb_hardware_generation_sensor(sensor::Sensor *sensor) { this->cb_hardware_generation_sensor_ = sensor; }

  void update_janitza(float import_w, float export_w, float l1_current, float l2_current, float l3_current,
                      float l1_voltage, float l2_voltage, float l3_voltage, float l1_power_w, float l2_power_w,
                      float l3_power_w, bool online);
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
  void update_meter_from_push_(const std::string &data);
  void update_phase_detection_();
  uint8_t count_phases_(uint8_t phase_mask) const;
  void update_ev_measurements_();
  void setup_output_pin_(GPIOPin *pin);
  void update_relays_();
  void note_chargebox_seen_(uint8_t address);
  void run_startup_sequence_();
  void schedule_startup_step_(uint8_t step, uint32_t delay_ms);
  void send_packet_(uint8_t dst, uint8_t cmd, const std::string &data = {});
  void send_restart_registration_();
  void send_connection_state_();
  void send_status_update_request_();
  void send_periodic_cmd18_();
  void send_meter_update_interval_();
  void send_remote_start_();
  void send_remote_stop_();
  void log_autostart_config_(const std::string &config);
  void schedule_current_release_(uint32_t delay_ms);
  void run_delayed_current_release_();
  void send_current_setpoint_(float amps);
  bool charge_flow_requested_() const;
  bool current_setpoint_allowed_() const;
  bool is_supported_current_request_(uint8_t code) const;
  const char *protocol_profile_name_() const;
  const char *cb_status_name_(uint8_t code) const;
  const char *current_request_name_(uint8_t code) const;
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
  uint8_t chargebox_hardware_generation_{0};
  uint32_t session_{1};
  bool evbox_online_{false};
  bool session_active_{false};
  bool stop_requested_{false};
  bool start_requested_{false};
  bool current_start_released_{false};
  bool remote_start_blocked_{false};
  bool commissioning_mode_{true};
  bool settings_restored_{false};
  bool have_session_start_meter_{false};
  float active_current_{0.0f};
  float desired_current_{0.0f};
  float commanded_current_{0.0f};
  float returned_current_limit_{NAN};
  bool current_limit_returned_{false};
  float ev_l1_current_a_{0.0f};
  float ev_l2_current_a_{0.0f};
  float ev_l3_current_a_{0.0f};
  float ev_l1_voltage_v_{NAN};
  float ev_l2_voltage_v_{NAN};
  float ev_l3_voltage_v_{NAN};
  float ev_l1_power_factor_{NAN};
  float ev_l2_power_factor_{NAN};
  float ev_l3_power_factor_{NAN};
  float ev_power_w_{0.0f};
  uint8_t evbox_detected_charge_phases_{1};
  uint8_t evbox_active_phase_mask_{0x01};
  float session_energy_kwh_{0.0f};
  float meter_value_kwh_{NAN};
  float session_start_meter_kwh_{0.0f};
  float temperature_c_{NAN};
  uint32_t heartbeat_interval_ms_{5000};
  uint32_t watchdog_timeout_ms_{30000};
  uint32_t last_rx_ms_{0};
  uint32_t last_periodic_cmd18_ms_{0};
  uint32_t start_requested_ms_{0};
  uint8_t last_cb_status_code_{0};
  bool have_last_cb_status_code_{false};
  uint8_t last_current_request_code_{0};
  bool have_last_current_request_code_{false};
  uint32_t last_current_request_ms_{0};
  uint8_t pending_current_request_code_{0};
  bool pending_current_request_after_config_{false};
  bool delayed_current_release_pending_{false};
  uint32_t delayed_current_release_due_ms_{0};
  uint32_t last_heartbeat_rx_ms_{0};
  uint32_t last_heartbeat_tx_ms_{0};
  uint32_t last_publish_ms_{0};
  uint32_t startup_step_due_ms_{0};
  uint8_t startup_step_{0};
  bool startup_config_received_{false};
  uint32_t last_startup_sync_request_ms_{0};
  std::string pending_config_34_ {};
  ESPPreferenceObject settings_pref_{};
  bool janitza_online_{false};

  text_sensor::TextSensor *status_text_sensor_{nullptr};
  text_sensor::TextSensor *state_text_sensor_{nullptr};
  text_sensor::TextSensor *communication_text_sensor_{nullptr};
  text_sensor::TextSensor *protocol_profile_text_sensor_{nullptr};
  text_sensor::TextSensor *cb_serial_text_sensor_{nullptr};
  text_sensor::TextSensor *cb_status_detail_text_sensor_{nullptr};
  text_sensor::TextSensor *current_request_state_text_sensor_{nullptr};
  sensor::Sensor *ev_current_sensor_{nullptr};
  sensor::Sensor *current_limit_sensor_{nullptr};
  sensor::Sensor *desired_current_sensor_{nullptr};
  sensor::Sensor *commanded_current_sensor_{nullptr};
  sensor::Sensor *returned_current_limit_sensor_{nullptr};
  sensor::Sensor *l1_current_sensor_{nullptr};
  sensor::Sensor *l2_current_sensor_{nullptr};
  sensor::Sensor *l3_current_sensor_{nullptr};
  sensor::Sensor *l1_voltage_sensor_{nullptr};
  sensor::Sensor *l2_voltage_sensor_{nullptr};
  sensor::Sensor *l3_voltage_sensor_{nullptr};
  sensor::Sensor *l1_power_factor_sensor_{nullptr};
  sensor::Sensor *l2_power_factor_sensor_{nullptr};
  sensor::Sensor *l3_power_factor_sensor_{nullptr};
  sensor::Sensor *detected_charge_phases_sensor_{nullptr};
  sensor::Sensor *active_phase_mask_sensor_{nullptr};
  sensor::Sensor *power_sensor_{nullptr};
  sensor::Sensor *session_energy_sensor_{nullptr};
  sensor::Sensor *meter_value_sensor_{nullptr};
  sensor::Sensor *temperature_sensor_{nullptr};
  sensor::Sensor *cb_firmware_sensor_{nullptr};
  sensor::Sensor *cb_hardware_generation_sensor_{nullptr};
  GPIOPin *rs485_de_pin_{nullptr};
  GPIOPin *relay_evbox_known_pin_{nullptr};
  GPIOPin *relay_janitza_ok_pin_{nullptr};
  GPIOPin *relay_charging_active_pin_{nullptr};
  GPIOPin *relay_failsafe_pin_{nullptr};
};

}  // namespace evbox_max
}  // namespace esphome
