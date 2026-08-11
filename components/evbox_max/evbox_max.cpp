#include "evbox_max.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#include <algorithm>
#include <cstdlib>

namespace esphome {
namespace evbox_max {

static const char *const TAG = "evbox_max";
static constexpr uint8_t ADDR_CP = 0x80;
static constexpr uint8_t ADDR_BROADCAST = 0xBC;
static constexpr uint16_t ACK = 0xAA00;
static constexpr uint32_t SETTINGS_MAGIC = 0x45564258UL;
static constexpr uint16_t SETTINGS_VERSION = 1;

void EvboxMaxComponent::setup() {
  this->settings_pref_ = global_preferences->make_preference<StoredSettings>(fnv1_hash("evbox_max_settings"));
  this->load_settings_();
  if (this->rs485_de_pin_ != nullptr) {
    this->rs485_de_pin_->setup();
    // MAX3485 is half-duplex. Keep the driver disabled by default so the
    // ChargeBox can talk and the ESP only drives the bus while transmitting.
    this->rs485_de_pin_->digital_write(false);
  }
  this->setup_output_pin_(this->relay_evbox_known_pin_);
  this->setup_output_pin_(this->relay_janitza_ok_pin_);
  this->setup_output_pin_(this->relay_charging_active_pin_);
  this->setup_output_pin_(this->relay_failsafe_pin_);
  this->transition_(WAIT_REGISTRATION);
  this->last_rx_ms_ = millis();
  this->last_heartbeat_ms_ = millis();
  this->send_restart_registration_();
}

void EvboxMaxComponent::loop() {
  // RX path: consume every available byte and let the protocol parser decide
  // when a full frame has arrived. Protocol handling stays out of the UART
  // byte loop, which keeps transport and state-machine logic separated.
  while (this->available()) {
    uint8_t byte;
    this->read_byte(&byte);
    Frame frame;
    if (this->parser_.push(byte, &frame)) {
      this->last_rx_ms_ = millis();
      this->evbox_online_ = true;
      this->handle_frame_(frame);
    }
  }

  const uint32_t now = millis();
  if (this->chargebox_address_ == 0 && now - this->last_heartbeat_ms_ >= this->heartbeat_interval_ms_) {
    this->send_restart_registration_();
    this->last_heartbeat_ms_ = now;
  } else if (this->chargebox_address_ != 0 && now - this->last_heartbeat_ms_ >= this->heartbeat_interval_ms_) {
    // TX path: heartbeat proves the controller is alive; current setpoint is
    // recalculated locally from the selected control mode and Janitza inputs.
    this->send_heartbeat_();
    if (!this->stop_requested_ &&
        (this->session_active_ || this->state_ == CHARGING || this->state_ == SESSION_STARTING ||
         this->state_ == STARTING || this->state_ == AUTHORIZED)) {
      this->desired_current_ = this->controller_.calculate_current(this->inputs_);
      this->send_current_setpoint_(this->desired_current_);
    }
    this->last_heartbeat_ms_ = now;
  }

  this->watchdog_();
  this->run_startup_sequence_();
  this->update_relays_();

  if (now - this->last_publish_ms_ >= 1000) {
    this->publish_();
    this->last_publish_ms_ = now;
  }
}

void EvboxMaxComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "EVBox MAX controller");
  ESP_LOGCONFIG(TAG, "  Heartbeat interval: %u ms", this->heartbeat_interval_ms_);
  ESP_LOGCONFIG(TAG, "  Watchdog timeout: %u ms", this->watchdog_timeout_ms_);
  ESP_LOGCONFIG(TAG, "  ChargeBox address: 0x%02X", this->chargebox_address_);
  ESP_LOGCONFIG(TAG, "  RS485 driver enable pin: %s", this->rs485_de_pin_ != nullptr ? "configured" : "not configured");
}

void EvboxMaxComponent::set_mode(ChargingMode mode) {
  this->controller_.set_mode(mode);
  this->save_settings_();
}

void EvboxMaxComponent::set_failsafe_mode(FailsafeMode mode) {
  this->controller_.set_failsafe_mode(mode);
  this->save_settings_();
}

void EvboxMaxComponent::set_max_current(float current) {
  this->inputs_.max_current = current;
  this->save_settings_();
}

void EvboxMaxComponent::set_charger_breaker_current(float current) {
  this->inputs_.charger_breaker_current = current;
  this->save_settings_();
}

void EvboxMaxComponent::set_main_fuse_current(float current) {
  this->inputs_.main_fuse_current = current;
  this->save_settings_();
}

void EvboxMaxComponent::set_manual_current(float current) {
  this->inputs_.manual_current = current;
  this->save_settings_();
}

void EvboxMaxComponent::set_failsafe_current(float current) {
  this->controller_.set_failsafe_current(current);
  this->save_settings_();
}

void EvboxMaxComponent::set_pv_enabled(bool enabled) {
  this->inputs_.pv_enabled = enabled;
  this->save_settings_();
}

void EvboxMaxComponent::update_janitza(float import_w, float export_w, float l1_current, float l2_current,
                                      float l3_current, float l1_voltage, float l2_voltage, float l3_voltage,
                                      float l1_power_w, float l2_power_w, float l3_power_w, bool online) {
  // The Janitza component only provides measurements. Charging policy remains
  // in ChargeController so meter communication and control logic do not blend.
  this->inputs_.grid_import_w = import_w;
  this->inputs_.grid_export_w = export_w;
  this->inputs_.grid_total_power_w = import_w > 0.0f ? import_w : -export_w;
  this->inputs_.l1_current = l1_current;
  this->inputs_.l2_current = l2_current;
  this->inputs_.l3_current = l3_current;
  this->inputs_.l1_power_w = l1_power_w;
  this->inputs_.l2_power_w = l2_power_w;
  this->inputs_.l3_power_w = l3_power_w;
  this->inputs_.janitza_online = online;
  this->janitza_online_ = online;
  (void) l1_voltage;
  (void) l2_voltage;
  (void) l3_voltage;
  this->update_ev_measurements_();

  const float next_current = this->controller_.calculate_current(this->inputs_);
  this->desired_current_ = next_current;
  const bool charge_flow_active = this->session_active_ || this->state_ == AUTHORIZED ||
                                  this->state_ == STARTING || this->state_ == SESSION_STARTING ||
                                  this->state_ == CHARGING;
  if (!this->stop_requested_ && charge_flow_active && next_current < this->active_current_) {
    // Overload response path: do not wait for the next heartbeat tick when the
    // meter says current must go down. A lower setpoint is sent immediately.
    this->send_current_setpoint_(next_current);
    this->last_heartbeat_ms_ = millis();
  }
}

void EvboxMaxComponent::start_session() {
  this->stop_requested_ = false;
  this->session_active_ = true;
  this->transition_(AUTHORIZED);
}

void EvboxMaxComponent::stop_session() {
  this->stop_requested_ = true;
  this->session_active_ = false;
  this->send_current_setpoint_(0.0f);
  this->transition_(FINISHING);
}

void EvboxMaxComponent::handle_frame_(const Frame &frame) {
  // This is the EVBox communication state machine. Frame identifiers are kept
  // in protocol.h; this method decides how a valid protocol frame changes the
  // controller state and which follow-up command is sent.
  ESP_LOGD(TAG, "RX EVBox dst=0x%02X src=0x%02X cmd=0x%02X data=%s", frame.dst, frame.src, frame.cmd,
           frame.data.c_str());
  switch (frame.type) {
    case FrameType::REGISTRATION:
      if (frame.dst != ADDR_CP || frame.data.size() < 7) {
        ESP_LOGD(TAG, "Ignoring registration-like frame dst=0x%02X data_len=%u", frame.dst,
                 static_cast<unsigned>(frame.data.size()));
        break;
      }
      this->chargebox_serial_ = frame.data.substr(0, 7);
      if (frame.data.size() >= 11) {
        this->chargebox_firmware_ = static_cast<uint16_t>(std::strtoul(frame.data.substr(7, 4).c_str(), nullptr, 10));
      }
      this->chargebox_address_ = frame.src == 0 ? 1 : frame.src;
      ESP_LOGI(TAG, "CB registration serial=%s assign=0x%02X firmware=%u", this->chargebox_serial_.c_str(),
               this->chargebox_address_, this->chargebox_firmware_);
      this->transition_(ASSIGN_ADDRESS);
      this->send_packet_(ADDR_BROADCAST, 0x11, this->chargebox_serial_ + hex_byte(this->chargebox_address_) + "03");
      this->transition_(READ_INFO);
      this->schedule_startup_step_(1, 300);
      break;
    case FrameType::INFO_RESPONSE:
      // Hardware/model data has been read. Next step is configuration so the
      // controller knows what limits and capabilities the ChargeBox reports.
      this->transition_(READ_CONFIG);
      this->schedule_startup_step_(5, 300);
      break;
    case FrameType::CONFIG_RESPONSE:
      ESP_LOGI(TAG, "CB config received; automatic config write disabled to preserve working meter settings");
      this->transition_(IDLE);
      break;
    case FrameType::CONFIG_SET_RESPONSE:
      if (frame.data == hex_word(ACK)) {
        ESP_LOGI(TAG, "CB config accepted: meter set to Modbus/serial address 1");
      } else {
        ESP_LOGW(TAG, "CB config response data=%s", frame.data.c_str());
      }
      // Registration and startup config are done; the controller may now wait
      // for local authorisation/start commands.
      this->transition_(IDLE);
      break;
    case FrameType::AUTHENTICATE_CARD:
      if (frame.dst == ADDR_CP && frame.src >= 1 && frame.src <= 20) {
        this->chargebox_address_ = frame.src;
        this->evbox_online_ = true;
        const uint8_t auth_state = parse_hex_byte(frame.data, 0);
        const uint8_t card_len = parse_hex_byte(frame.data, 2);
        const size_t available = frame.data.size() > 4 ? frame.data.size() - 4 : 0;
        const size_t safe_len = std::min<size_t>(card_len, available);
        const std::string card = frame.data.substr(4, safe_len);
        const bool charge_flow_allowed = this->state_ == AUTHORIZED || this->state_ == STARTING ||
                                         this->state_ == SESSION_STARTING || this->state_ == CHARGING;
        const bool access_granted = !this->stop_requested_ && (card == "000000AS" || charge_flow_allowed);
        ESP_LOGI(TAG, "CB authenticate request state=0x%02X card=%s access=%s", auth_state, card.c_str(),
                 access_granted ? "granted" : "denied");
        const std::string padded_card = (card + std::string(22, '0')).substr(0, 22);
        this->send_packet_(frame.src, 0x22,
                           hex_byte(access_granted ? 0x01 : 0x12) + hex_byte(card_len) + padded_card + "FFFF");
        if (access_granted && this->state_ == IDLE) {
          this->transition_(AUTHORIZED);
        }
      }
      break;
    case FrameType::CURRENT_REQUEST:
      if (frame.dst == ADDR_CP && frame.src >= 1 && frame.src <= 20) {
        this->chargebox_address_ = frame.src;
        this->evbox_online_ = true;
        const uint8_t request_code = parse_hex_byte(frame.data, 0);
        this->send_packet_(frame.src, 0x6A, hex_word(ACK));
        const bool charge_flow_allowed = this->session_active_ || this->state_ == AUTHORIZED ||
                                         this->state_ == STARTING || this->state_ == SESSION_STARTING ||
                                         this->state_ == CHARGING;
        if (!this->stop_requested_ && charge_flow_allowed) {
          if (request_code == 0x81) {
            this->session_active_ = true;
            this->transition_(CHARGING);
          } else if (this->state_ != CHARGING && this->state_ != SESSION_STARTING) {
            this->transition_(STARTING);
          }
          this->desired_current_ = this->controller_.calculate_current(this->inputs_);
          this->send_current_setpoint_(this->desired_current_);
        } else {
          ESP_LOGI(TAG, "CB current request acknowledged; no current limit sent while state=%s", this->state_name_());
        }
      }
      break;
    case FrameType::CURRENT_SETPOINT:
      if (frame.dst == ADDR_CP && frame.src >= 1 && frame.src <= 20) {
        this->chargebox_address_ = frame.src;
        this->evbox_online_ = true;
        ESP_LOGD(TAG, "CB acknowledged current setpoint");
        if (!this->stop_requested_ && (this->state_ == STARTING || this->state_ == AUTHORIZED)) {
          this->transition_(SESSION_STARTING);
        }
      }
      break;
    case FrameType::STATE_UPDATE:
      if (frame.dst == ADDR_CP && frame.src >= 1 && frame.src <= 20) {
        this->chargebox_address_ = frame.src;
        this->evbox_online_ = true;
        const uint8_t code = parse_hex_byte(frame.data, 0);
        const uint8_t is_charging = parse_hex_byte(frame.data, 6);
        const uint8_t led_colour = parse_hex_byte(frame.data, 8);
        const uint8_t lock_state = parse_hex_byte(frame.data, 10);
        const uint8_t cable_current = parse_hex_byte(frame.data, 12);
        ESP_LOGI(TAG, "CB state cmd26 status=0x%02X is_charging=%u led=0x%02X lock=%u cable=%u data_len=%u",
                 code, is_charging, led_colour, lock_state, cable_current, static_cast<unsigned>(frame.data.size()));
        if (code == 0x02) {
          this->stop_requested_ = false;
          this->session_active_ = false;
          this->transition_(IDLE);
        } else if (code == 0x17) {
          if (this->stop_requested_) {
            this->session_active_ = false;
            this->transition_(FINISHING);
          } else {
            this->session_active_ = false;
            this->transition_(AUTHORIZED);
          }
        }
        else if (code == 0x47 || code == 0x4A) {
          if (this->stop_requested_) {
            this->session_active_ = false;
            this->transition_(FINISHING);
          } else if (this->state_ == SESSION_STARTING) {
            this->transition_(SESSION_STARTING);
          } else {
            this->transition_(STARTING);
          }
        }
        else if (code == 0x48) {
          this->stop_requested_ = false;
          this->session_active_ = true;
          this->transition_(CHARGING);
        } else if (code == 0x4B) {
          this->session_active_ = false;
          this->transition_(FINISHING);
        }
        else if (code == 0x0A) this->transition_(FAULT);
        if (frame.data.size() >= 128) {
          const uint32_t raw_limit = parse_hex_uint(frame.data, 124, 4);
          if (raw_limit > 0 && raw_limit <= 320) {
            this->returned_current_limit_ = static_cast<float>(raw_limit) / 10.0f;
            this->current_limit_returned_ = true;
            this->update_ev_measurements_();
            ESP_LOGI(TAG, "CB returned active current limit %.1f A", this->returned_current_limit_);
          } else if (raw_limit > 0) {
            ESP_LOGD(TAG, "Ignoring non-current cmd26 field at current-limit offset: raw=%u", raw_limit);
          }
        }
        this->update_meter_from_state_(frame.data);
        const std::string ack_data = this->chargebox_firmware_ > 100 ? hex_dword(this->session_) + hex_dword(this->seconds_since_2000_())
                                                                     : hex_dword(this->session_);
        this->send_packet_(frame.src, 0x26, ack_data);
      }
      break;
    case FrameType::METERING_START:
      if (frame.dst == ADDR_CP && frame.src >= 1 && frame.src <= 20) {
        this->session_active_ = !this->stop_requested_;
        if (!this->stop_requested_) {
          this->session_start_meter_kwh_ = this->meter_value_kwh_;
          this->have_session_start_meter_ = this->meter_value_kwh_ > 0.0f;
          this->transition_(SESSION_STARTING);
        }
        const std::string data = this->chargebox_firmware_ > 100 ? std::string("01") + hex_dword(this->session_) + hex_dword(this->seconds_since_2000_())
                                                                 : std::string("01") + hex_dword(this->session_);
        this->send_packet_(frame.src, 0x23, data);
      }
      break;
    case FrameType::METERING_END:
      if (frame.dst == ADDR_CP && frame.src >= 1 && frame.src <= 20) {
        this->session_active_ = false;
        this->send_packet_(frame.src, 0x24, "01");
      }
      break;
    case FrameType::METER_PUSH:
      if (frame.dst == ADDR_CP && frame.src >= 1 && frame.src <= 20) {
        ESP_LOGD(TAG, "CB meter push data=%s", frame.data.c_str());
        this->update_meter_from_push_(frame.data);
        this->send_packet_(frame.src, 0x66, "");
      }
      break;
    case FrameType::FAULT:
      this->transition_(FAULT);
      break;
    default:
      ESP_LOGD(TAG, "Unhandled frame type 0x%02X", static_cast<uint8_t>(frame.type));
      break;
  }
}

void EvboxMaxComponent::transition_(EvboxState state) {
  if (this->state_ == state) {
    return;
  }
  // All state changes go through this helper so logging, future guards, and HA
  // publishing can be attached in one place.
  const char *previous_name = this->state_name_();
  this->state_ = state;
  ESP_LOGI(TAG, "State %s -> %s", previous_name, this->state_name_());
}

void EvboxMaxComponent::load_settings_() {
  StoredSettings settings{};
  if (this->settings_pref_.load(&settings) && settings.magic == SETTINGS_MAGIC && settings.version == SETTINGS_VERSION) {
    this->apply_settings_(settings);
    ESP_LOGI(TAG, "Restored HA settings from NVS");
  } else {
    ESP_LOGI(TAG, "No stored HA settings found; using YAML defaults");
  }
  this->settings_restored_ = true;
}

void EvboxMaxComponent::save_settings_() {
  if (!this->settings_restored_) return;
  const auto settings = this->current_settings_();
  this->settings_pref_.save(&settings);
}

EvboxMaxComponent::StoredSettings EvboxMaxComponent::current_settings_() const {
  StoredSettings settings{};
  settings.magic = SETTINGS_MAGIC;
  settings.version = SETTINGS_VERSION;
  settings.mode = static_cast<uint8_t>(this->controller_.mode());
  settings.failsafe_mode = static_cast<uint8_t>(this->controller_.failsafe_mode());
  settings.pv_enabled = this->inputs_.pv_enabled;
  settings.manual_current = this->inputs_.manual_current;
  settings.max_current = this->inputs_.max_current;
  settings.charger_breaker_current = this->inputs_.charger_breaker_current;
  settings.main_fuse_current = this->inputs_.main_fuse_current;
  settings.failsafe_current = this->controller_.failsafe_current();
  return settings;
}

void EvboxMaxComponent::apply_settings_(const StoredSettings &settings) {
  this->controller_.set_mode(static_cast<ChargingMode>(settings.mode));
  this->controller_.set_failsafe_mode(static_cast<FailsafeMode>(settings.failsafe_mode));
  this->controller_.set_failsafe_current(settings.failsafe_current);
  this->inputs_.pv_enabled = settings.pv_enabled;
  this->inputs_.manual_current = settings.manual_current;
  this->inputs_.max_current = settings.max_current;
  this->inputs_.charger_breaker_current = settings.charger_breaker_current;
  this->inputs_.main_fuse_current = settings.main_fuse_current;
}

void EvboxMaxComponent::update_meter_from_state_(const std::string &data) {
  if (data.size() < 26) return;

  if (data.size() >= 56) {
    this->temperature_c_ = static_cast<float>(parse_hex_uint(data, 52, 4)) / 10.0f;
  }

  if (data.size() >= 132) {
    const uint32_t measurement_block = parse_hex_uint(data, 68, 8) | parse_hex_uint(data, 76, 8) |
                                       parse_hex_uint(data, 84, 8) | parse_hex_uint(data, 92, 8) |
                                       parse_hex_uint(data, 100, 8);
    if (measurement_block != 0) {
      this->ev_l1_voltage_v_ = static_cast<float>(parse_hex_uint(data, 68, 4));
      this->ev_l2_voltage_v_ = static_cast<float>(parse_hex_uint(data, 72, 4));
      this->ev_l3_voltage_v_ = static_cast<float>(parse_hex_uint(data, 76, 4));
      this->ev_l1_current_a_ = static_cast<float>(parse_hex_uint(data, 80, 4)) / 100.0f;
      this->ev_l2_current_a_ = static_cast<float>(parse_hex_uint(data, 84, 4)) / 100.0f;
      this->ev_l3_current_a_ = static_cast<float>(parse_hex_uint(data, 88, 4)) / 100.0f;
      this->ev_l1_power_factor_ = static_cast<float>(parse_hex_uint(data, 96, 4)) / 1000.0f;
      this->ev_l2_power_factor_ = static_cast<float>(parse_hex_uint(data, 100, 4)) / 1000.0f;
      this->ev_l3_power_factor_ = static_cast<float>(parse_hex_uint(data, 104, 4)) / 1000.0f;
      const float socket_temperature_c = static_cast<float>(parse_hex_uint(data, 92, 4));
      ESP_LOGD(TAG, "CB meter state V %.0f/%.0f/%.0f I %.2f/%.2f/%.2f chassis %.1fC socket %.1fC PF %.3f/%.3f/%.3f",
               this->ev_l1_voltage_v_, this->ev_l2_voltage_v_, this->ev_l3_voltage_v_, this->ev_l1_current_a_,
               this->ev_l2_current_a_, this->ev_l3_current_a_, this->temperature_c_, socket_temperature_c,
               this->ev_l1_power_factor_, this->ev_l2_power_factor_, this->ev_l3_power_factor_);
    } else {
      ESP_LOGD(TAG, "CB cmd26 extended meter block is zero; keeping configured phase mask 0x%02X",
               this->evbox_active_phase_mask_);
    }
    this->update_phase_detection_();
  }

  const uint32_t raw_meter = parse_hex_uint(data, 18, 8);
  if (raw_meter == 0) {
    ESP_LOGD(TAG, "CB cmd26 meter counter is zero; keeping previous kWh value %.3f", this->meter_value_kwh_);
    return;
  }

  // EVBox cmd26 carries the ChargeBox meter counter in Wh at this offset in
  // captures from the working local test app. Publish as kWh for Home Assistant.
  const float meter_kwh = static_cast<float>(raw_meter) / 1000.0f;
  if (meter_kwh <= 0.0f || meter_kwh > 1000000.0f) return;

  this->meter_value_kwh_ = meter_kwh;
  if (this->session_active_ && this->have_session_start_meter_ && meter_kwh >= this->session_start_meter_kwh_) {
    this->session_energy_kwh_ = meter_kwh - this->session_start_meter_kwh_;
  }
  ESP_LOGD(TAG, "CB meter %.3f kWh session %.3f kWh", this->meter_value_kwh_, this->session_energy_kwh_);
}

void EvboxMaxComponent::update_meter_from_push_(const std::string &data) {
  if (data.size() < 44) return;

  this->ev_l1_voltage_v_ = static_cast<float>(parse_hex_uint(data, 0, 4));
  this->ev_l2_voltage_v_ = static_cast<float>(parse_hex_uint(data, 4, 4));
  this->ev_l3_voltage_v_ = static_cast<float>(parse_hex_uint(data, 8, 4));
  this->ev_l1_current_a_ = static_cast<float>(parse_hex_uint(data, 12, 4)) / 100.0f;
  this->ev_l2_current_a_ = static_cast<float>(parse_hex_uint(data, 16, 4)) / 100.0f;
  this->ev_l3_current_a_ = static_cast<float>(parse_hex_uint(data, 20, 4)) / 100.0f;
  this->ev_l1_power_factor_ = static_cast<float>(parse_hex_uint(data, 24, 4)) / 1000.0f;
  this->ev_l2_power_factor_ = static_cast<float>(parse_hex_uint(data, 28, 4)) / 1000.0f;
  this->ev_l3_power_factor_ = static_cast<float>(parse_hex_uint(data, 32, 4)) / 1000.0f;

  const uint32_t raw_meter = parse_hex_uint(data, 36, 8);
  if (raw_meter > 0) {
    const float meter_kwh = static_cast<float>(raw_meter) / 1000.0f;
    if (meter_kwh > 0.0f && meter_kwh < 1000000.0f) {
      this->meter_value_kwh_ = meter_kwh;
      if (this->session_active_ && this->have_session_start_meter_ && meter_kwh >= this->session_start_meter_kwh_) {
        this->session_energy_kwh_ = meter_kwh - this->session_start_meter_kwh_;
      }
    }
  }
  this->update_phase_detection_();
  ESP_LOGD(TAG, "CB meter push V %.0f/%.0f/%.0f I %.2f/%.2f/%.2f PF %.3f/%.3f/%.3f kWh %.3f",
           this->ev_l1_voltage_v_, this->ev_l2_voltage_v_, this->ev_l3_voltage_v_, this->ev_l1_current_a_,
           this->ev_l2_current_a_, this->ev_l3_current_a_, this->ev_l1_power_factor_, this->ev_l2_power_factor_,
           this->ev_l3_power_factor_, this->meter_value_kwh_);
}

void EvboxMaxComponent::update_phase_detection_() {
  uint8_t current_mask = 0;
  if (std::fabs(this->ev_l1_current_a_) >= 0.5f) current_mask |= 0x01;
  if (std::fabs(this->ev_l2_current_a_) >= 0.5f) current_mask |= 0x02;
  if (std::fabs(this->ev_l3_current_a_) >= 0.5f) current_mask |= 0x04;

  uint8_t voltage_mask = 0;
  if (!std::isnan(this->ev_l1_voltage_v_) && this->ev_l1_voltage_v_ >= 180.0f) voltage_mask |= 0x01;
  if (!std::isnan(this->ev_l2_voltage_v_) && this->ev_l2_voltage_v_ >= 180.0f) voltage_mask |= 0x02;
  if (!std::isnan(this->ev_l3_voltage_v_) && this->ev_l3_voltage_v_ >= 180.0f) voltage_mask |= 0x04;

  const uint8_t phase_mask = current_mask != 0 ? current_mask : (voltage_mask != 0 ? voltage_mask : this->evbox_active_phase_mask_);
  this->evbox_active_phase_mask_ = phase_mask & 0x07;
  this->evbox_detected_charge_phases_ = this->count_phases_(this->evbox_active_phase_mask_);
  this->inputs_.active_phase_mask = this->evbox_active_phase_mask_;
  this->inputs_.charge_phases = this->evbox_detected_charge_phases_ > 0 ? this->evbox_detected_charge_phases_ : 1;
  this->inputs_.ev_l1_current = this->ev_l1_current_a_;
  this->inputs_.ev_l2_current = this->ev_l2_current_a_;
  this->inputs_.ev_l3_current = this->ev_l3_current_a_;
  this->inputs_.ev_current = std::max(this->ev_l1_current_a_, std::max(this->ev_l2_current_a_, this->ev_l3_current_a_));
  this->update_ev_measurements_();
}

uint8_t EvboxMaxComponent::count_phases_(uint8_t phase_mask) const {
  uint8_t count = 0;
  if ((phase_mask & 0x01) != 0) count++;
  if ((phase_mask & 0x02) != 0) count++;
  if ((phase_mask & 0x04) != 0) count++;
  return count;
}

void EvboxMaxComponent::update_ev_measurements_() {
  const float pf1 = std::isnan(this->ev_l1_power_factor_) ? 1.0f : this->ev_l1_power_factor_;
  const float pf2 = std::isnan(this->ev_l2_power_factor_) ? 1.0f : this->ev_l2_power_factor_;
  const float pf3 = std::isnan(this->ev_l3_power_factor_) ? 1.0f : this->ev_l3_power_factor_;
  const float v1 = std::isnan(this->ev_l1_voltage_v_) ? 0.0f : this->ev_l1_voltage_v_;
  const float v2 = std::isnan(this->ev_l2_voltage_v_) ? 0.0f : this->ev_l2_voltage_v_;
  const float v3 = std::isnan(this->ev_l3_voltage_v_) ? 0.0f : this->ev_l3_voltage_v_;
  this->ev_power_w_ = this->ev_l1_current_a_ * v1 * pf1 + this->ev_l2_current_a_ * v2 * pf2 +
                      this->ev_l3_current_a_ * v3 * pf3;
}

void EvboxMaxComponent::setup_output_pin_(GPIOPin *pin) {
  if (pin == nullptr) return;
  pin->setup();
  pin->digital_write(false);
}

void EvboxMaxComponent::update_relays_() {
  const bool charging_active = !this->stop_requested_ &&
                               (this->session_active_ || this->state_ == CHARGING ||
                                this->state_ == SESSION_STARTING || this->state_ == STARTING);
  const bool failsafe = !this->janitza_online_ &&
                        (this->controller_.mode() == CHARGING_MODE_LOAD_BALANCING ||
                         this->controller_.mode() == CHARGING_MODE_PV_SURPLUS);
  if (this->relay_evbox_known_pin_ != nullptr) this->relay_evbox_known_pin_->digital_write(this->chargebox_address_ != 0);
  if (this->relay_janitza_ok_pin_ != nullptr) this->relay_janitza_ok_pin_->digital_write(this->janitza_online_);
  if (this->relay_charging_active_pin_ != nullptr) this->relay_charging_active_pin_->digital_write(charging_active);
  if (this->relay_failsafe_pin_ != nullptr) this->relay_failsafe_pin_->digital_write(failsafe);
}

void EvboxMaxComponent::schedule_startup_step_(uint8_t step, uint32_t delay_ms) {
  this->startup_step_ = step;
  this->startup_step_due_ms_ = millis() + delay_ms;
}

void EvboxMaxComponent::run_startup_sequence_() {
  if (this->startup_step_ == 0 || this->chargebox_address_ == 0) return;
  if (millis() - this->startup_step_due_ms_ > 0x80000000UL) return;

  const uint8_t step = this->startup_step_;
  this->startup_step_ = 0;
  switch (step) {
    case 1:
      ESP_LOGI(TAG, "Startup step 1: connection state");
      this->send_connection_state_();
      this->schedule_startup_step_(2, 400);
      break;
    case 2:
      ESP_LOGI(TAG, "Startup step 2: meter update interval");
      this->send_meter_update_interval_();
      this->schedule_startup_step_(3, 300);
      break;
    case 3:
      ESP_LOGI(TAG, "Startup step 3: meter info request");
      this->send_packet_(this->chargebox_address_, 0x13, "");
      this->schedule_startup_step_(4, 300);
      break;
    case 4:
      ESP_LOGI(TAG, "Startup step 4: status update request");
      this->send_status_update_request_();
      this->schedule_startup_step_(5, 400);
      break;
    case 5:
      ESP_LOGI(TAG, "Startup step 5: CB config request");
      this->transition_(READ_CONFIG);
      this->send_packet_(this->chargebox_address_, 0x33, "");
      break;
    case 6:
      ESP_LOGI(TAG, "Startup step 6: CB config write");
      this->send_packet_(this->chargebox_address_, 0x34, this->pending_config_34_);
      this->pending_config_34_.clear();
      break;
    default:
      break;
  }
}

void EvboxMaxComponent::send_packet_(uint8_t dst, uint8_t cmd, const std::string &data) {
  const auto bytes = encode_frame(ADDR_CP, dst, cmd, data);
  ESP_LOGD(TAG, "TX EVBox dst=0x%02X src=0x%02X cmd=0x%02X data=%s", dst, ADDR_CP, cmd, data.c_str());
  if (this->rs485_de_pin_ != nullptr) {
    // Enable RS485 transmit before writing and disable it after flush() has
    // drained the UART buffer, otherwise the last byte can be clipped.
    this->rs485_de_pin_->digital_write(true);
  }
  this->write_array(bytes);
  this->flush();
  if (this->rs485_de_pin_ != nullptr) {
    this->rs485_de_pin_->digital_write(false);
  }
}

void EvboxMaxComponent::send_restart_registration_() {
  ESP_LOGI(TAG, "Requesting EVBox registration restart");
  this->send_packet_(ADDR_BROADCAST, 0x1E, "");
}

void EvboxMaxComponent::send_connection_state_() {
  if (this->chargebox_address_ == 0) return;
  this->send_packet_(this->chargebox_address_, 0x1B, "0000038400");
}

void EvboxMaxComponent::send_status_update_request_() {
  if (this->chargebox_address_ == 0) return;
  this->send_packet_(this->chargebox_address_, 0x18, "02");
}

void EvboxMaxComponent::send_meter_update_interval_() {
  if (this->chargebox_address_ == 0) return;
  this->send_packet_(this->chargebox_address_, 0x65, "000F");
}

void EvboxMaxComponent::send_meter_modbus_config_(const std::string &config) {
  if (this->chargebox_address_ == 0) return;
  if (config.size() < 60) {
    ESP_LOGW(TAG, "CB config too short for meter mode patch: len=%u", static_cast<unsigned>(config.size()));
    this->transition_(IDLE);
    return;
  }

  const uint32_t connection_timeout = parse_hex_uint(config, 0, 8, 3600);
  const uint32_t heartbeat_interval = parse_hex_uint(config, 8, 8, 900);
  const uint32_t meter_update_interval = parse_hex_uint(config, 16, 8, 30);
  const uint8_t relay = parse_hex_byte(config, 24, 0x03);
  const uint8_t phase = parse_hex_byte(config, 32, 0x01);
  const uint8_t tethered = parse_hex_byte(config, 34, 0x00);
  const uint8_t led_brightness = parse_hex_byte(config, 36, 0x30);
  const uint8_t auto_start = parse_hex_byte(config, 54, 0x01);
  const uint8_t enable_cmd2a = parse_hex_byte(config, 56, 0x00);
  const uint8_t auto_stop = parse_hex_byte(config, 58, 0x01);

  std::string patched = "FFFFFFFF";
  patched += hex_byte(led_brightness);
  patched += hex_byte(relay);
  patched += "0000";
  patched += "01";
  patched += hex_byte(phase);
  patched += hex_byte(tethered);
  patched += "0100000000000000";
  patched += hex_byte(auto_start);
  patched += hex_byte(enable_cmd2a);
  patched += hex_dword(connection_timeout);
  patched += hex_dword(heartbeat_interval);
  patched += hex_dword(meter_update_interval);
  patched += hex_byte(auto_stop);
  if (this->chargebox_firmware_ > 100) {
    patched += "0000000003E8010000";
  }
  this->pending_config_34_ = patched;
  ESP_LOGI(TAG, "Setting CB meter config to Modbus/serial address 1 after startup delay");
  this->schedule_startup_step_(6, 800);
}

void EvboxMaxComponent::send_heartbeat_() {
  if (this->chargebox_address_ == 0) return;
  this->send_status_update_request_();
}

void EvboxMaxComponent::send_current_setpoint_(float amps) {
  if (this->chargebox_address_ == 0) return;
  this->commanded_current_ = amps;
  this->active_current_ = amps;
  this->current_limit_returned_ = false;
  this->update_ev_measurements_();
  const auto tenths = static_cast<uint16_t>(std::max(0.0f, std::min(32.0f, amps)) * 10.0f);
  const std::string value = hex_word(tenths);
  ESP_LOGI(TAG, "Sending commanded current limit %.1f A to CB", static_cast<float>(tenths) / 10.0f);
  this->send_packet_(this->chargebox_address_, 0x6B, std::string("01") + hex_word(60) + value + value + value);
}

uint32_t EvboxMaxComponent::seconds_since_2000_() const {
  return millis() / 1000UL + 840000000UL;
}

void EvboxMaxComponent::watchdog_() {
  if (millis() - this->last_rx_ms_ > this->watchdog_timeout_ms_) {
    // EVBox communication loss is treated as a hard fault. The ChargeBox must
    // not keep receiving stale current commands when the bus is silent.
    this->evbox_online_ = false;
    this->transition_(FAULT);
  }
}

void EvboxMaxComponent::publish_() {
  if (this->status_text_sensor_ != nullptr) {
    this->status_text_sensor_->publish_state(this->session_active_ ? "Session active" : "Ready");
  }
  if (this->state_text_sensor_ != nullptr) {
    this->state_text_sensor_->publish_state(this->state_name_());
  }
  if (this->communication_text_sensor_ != nullptr) {
    this->communication_text_sensor_->publish_state(this->communication_name_());
  }
  if (this->ev_current_sensor_ != nullptr) {
    this->ev_current_sensor_->publish_state(this->inputs_.ev_current);
  }
  if (this->current_limit_sensor_ != nullptr) {
    this->current_limit_sensor_->publish_state(this->commanded_current_);
  }
  if (this->desired_current_sensor_ != nullptr) {
    this->desired_current_sensor_->publish_state(this->desired_current_);
  }
  if (this->commanded_current_sensor_ != nullptr) {
    this->commanded_current_sensor_->publish_state(this->commanded_current_);
  }
  if (this->returned_current_limit_sensor_ != nullptr && !std::isnan(this->returned_current_limit_)) {
    this->returned_current_limit_sensor_->publish_state(this->returned_current_limit_);
  }
  if (this->l1_current_sensor_ != nullptr) {
    this->l1_current_sensor_->publish_state(this->ev_l1_current_a_);
  }
  if (this->l2_current_sensor_ != nullptr) {
    this->l2_current_sensor_->publish_state(this->ev_l2_current_a_);
  }
  if (this->l3_current_sensor_ != nullptr) {
    this->l3_current_sensor_->publish_state(this->ev_l3_current_a_);
  }
  if (this->l1_voltage_sensor_ != nullptr && !std::isnan(this->ev_l1_voltage_v_)) {
    this->l1_voltage_sensor_->publish_state(this->ev_l1_voltage_v_);
  }
  if (this->l2_voltage_sensor_ != nullptr && !std::isnan(this->ev_l2_voltage_v_)) {
    this->l2_voltage_sensor_->publish_state(this->ev_l2_voltage_v_);
  }
  if (this->l3_voltage_sensor_ != nullptr && !std::isnan(this->ev_l3_voltage_v_)) {
    this->l3_voltage_sensor_->publish_state(this->ev_l3_voltage_v_);
  }
  if (this->l1_power_factor_sensor_ != nullptr && !std::isnan(this->ev_l1_power_factor_)) {
    this->l1_power_factor_sensor_->publish_state(this->ev_l1_power_factor_);
  }
  if (this->l2_power_factor_sensor_ != nullptr && !std::isnan(this->ev_l2_power_factor_)) {
    this->l2_power_factor_sensor_->publish_state(this->ev_l2_power_factor_);
  }
  if (this->l3_power_factor_sensor_ != nullptr && !std::isnan(this->ev_l3_power_factor_)) {
    this->l3_power_factor_sensor_->publish_state(this->ev_l3_power_factor_);
  }
  if (this->detected_charge_phases_sensor_ != nullptr) {
    this->detected_charge_phases_sensor_->publish_state(this->evbox_detected_charge_phases_);
  }
  if (this->active_phase_mask_sensor_ != nullptr) {
    this->active_phase_mask_sensor_->publish_state(this->evbox_active_phase_mask_);
  }
  if (this->power_sensor_ != nullptr) {
    this->power_sensor_->publish_state(this->ev_power_w_);
  }
  if (this->session_energy_sensor_ != nullptr) {
    this->session_energy_sensor_->publish_state(this->session_energy_kwh_);
  }
  if (this->meter_value_sensor_ != nullptr) {
    this->meter_value_sensor_->publish_state(this->meter_value_kwh_);
  }
  if (this->temperature_sensor_ != nullptr && !std::isnan(this->temperature_c_)) {
    this->temperature_sensor_->publish_state(this->temperature_c_);
  }
}

const char *EvboxMaxComponent::state_name_() const {
  switch (this->state_) {
    case BOOT: return "BOOT";
    case WAIT_REGISTRATION: return "WAIT_REGISTRATION";
    case ASSIGN_ADDRESS: return "ASSIGN_ADDRESS";
    case READ_INFO: return "READ_INFO";
    case READ_CONFIG: return "READ_CONFIG";
    case IDLE: return "IDLE";
    case AUTHORIZED: return "AUTHORIZED";
    case STARTING: return "STARTING";
    case SESSION_STARTING: return "SESSION_STARTING";
    case CHARGING: return "CHARGING";
    case PAUSED: return "PAUSED";
    case FINISHING: return "FINISHING";
    case FAULT: return "FAULT";
    default: return "UNKNOWN";
  }
}

const char *EvboxMaxComponent::communication_name_() const {
  return this->evbox_online_ ? "EVBox online" : "EVBox offline";
}

}  // namespace evbox_max
}  // namespace esphome

