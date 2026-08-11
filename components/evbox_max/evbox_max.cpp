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
    if (this->session_active_ || this->state_ == CHARGING || this->state_ == STARTING || this->state_ == AUTHORIZED) {
      this->send_current_setpoint_(this->controller_.calculate_current(this->inputs_));
    }
    this->last_heartbeat_ms_ = now;
  }

  this->watchdog_();

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
                                      float l3_current, bool online) {
  // The Janitza component only provides measurements. Charging policy remains
  // in ChargeController so meter communication and control logic do not blend.
  this->inputs_.grid_import_w = import_w;
  this->inputs_.grid_export_w = export_w;
  this->inputs_.l1_current = l1_current;
  this->inputs_.l2_current = l2_current;
  this->inputs_.l3_current = l3_current;
  this->inputs_.ev_current = this->active_current_;
  this->inputs_.janitza_online = online;

  const float next_current = this->controller_.calculate_current(this->inputs_);
  if (next_current < this->active_current_) {
    // Overload response path: do not wait for the next heartbeat tick when the
    // meter says current must go down. A lower setpoint is sent immediately.
    this->send_current_setpoint_(next_current);
    this->last_heartbeat_ms_ = millis();
  }
}

void EvboxMaxComponent::start_session() {
  this->session_active_ = true;
  this->transition_(AUTHORIZED);
}

void EvboxMaxComponent::stop_session() {
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
      this->send_connection_state_();
      this->send_packet_(this->chargebox_address_, 0x13, "");
      this->send_status_update_request_();
      break;
    case FrameType::INFO_RESPONSE:
      // Hardware/model data has been read. Next step is configuration so the
      // controller knows what limits and capabilities the ChargeBox reports.
      this->transition_(READ_CONFIG);
      this->send_packet_(this->chargebox_address_, 0x33, "");
      break;
    case FrameType::CONFIG_RESPONSE:
      // Registration and startup reads are done; the controller may now wait
      // for local authorisation/start commands.
      this->transition_(IDLE);
      break;
    case FrameType::CURRENT_REQUEST:
      if (frame.dst == ADDR_CP && frame.src >= 1 && frame.src <= 20) {
        this->chargebox_address_ = frame.src;
        this->evbox_online_ = true;
        this->send_packet_(frame.src, 0x6A, hex_word(ACK));
        if (this->state_ != CHARGING) this->transition_(STARTING);
        this->send_current_setpoint_(this->controller_.calculate_current(this->inputs_));
      }
      break;
    case FrameType::CURRENT_SETPOINT:
      if (frame.dst == ADDR_CP && frame.src >= 1 && frame.src <= 20) {
        this->chargebox_address_ = frame.src;
        this->evbox_online_ = true;
        ESP_LOGD(TAG, "CB acknowledged current setpoint");
      }
      break;
    case FrameType::STATE_UPDATE:
      if (frame.dst == ADDR_CP && frame.src >= 1 && frame.src <= 20) {
        this->chargebox_address_ = frame.src;
        this->evbox_online_ = true;
        const uint8_t code = parse_hex_byte(frame.data, 0);
        ESP_LOGI(TAG, "CB state cmd26 status=0x%02X data_len=%u", code, static_cast<unsigned>(frame.data.size()));
        if (code == 0x02) {
          this->session_active_ = false;
          this->transition_(IDLE);
        } else if (code == 0x17) {
          this->session_active_ = false;
          this->transition_(AUTHORIZED);
        }
        else if (code == 0x47 || code == 0x4A) this->transition_(STARTING);
        else if (code == 0x48) {
          this->session_active_ = true;
          this->transition_(CHARGING);
        } else if (code == 0x4B) {
          this->session_active_ = false;
          this->transition_(FINISHING);
        }
        else if (code == 0x0A) this->transition_(FAULT);
        if (frame.data.size() >= 128) {
          const uint32_t raw_limit = parse_hex_uint(frame.data, 124, 4);
          if (raw_limit > 0 && raw_limit < 1000) {
            this->active_current_ = static_cast<float>(raw_limit) / 10.0f;
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
        this->session_active_ = true;
        this->session_start_meter_kwh_ = this->meter_value_kwh_;
        this->have_session_start_meter_ = this->meter_value_kwh_ > 0.0f;
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
  ESP_LOGI(TAG, "State %s -> %u", this->state_name_(), state);
  this->state_ = state;
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

  const uint32_t raw_meter = parse_hex_uint(data, 18, 8);
  if (raw_meter == 0) return;

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

void EvboxMaxComponent::send_heartbeat_() {
  if (this->chargebox_address_ == 0) return;
  this->send_status_update_request_();
}

void EvboxMaxComponent::send_current_setpoint_(float amps) {
  if (this->chargebox_address_ == 0) return;
  this->active_current_ = amps;
  const auto tenths = static_cast<uint16_t>(std::max(0.0f, std::min(32.0f, amps)) * 10.0f);
  const std::string value = hex_word(tenths);
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
    this->ev_current_sensor_->publish_state(this->active_current_);
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
