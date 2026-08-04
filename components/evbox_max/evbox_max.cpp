#include "evbox_max.h"
#include "esphome/core/log.h"

namespace esphome {
namespace evbox_max {

static const char *const TAG = "evbox_max";

void EvboxMaxComponent::setup() {
  if (this->rs485_de_pin_ != nullptr) {
    this->rs485_de_pin_->setup();
    // MAX3485 is half-duplex. Keep the driver disabled by default so the
    // ChargeBox can talk and the ESP only drives the bus while transmitting.
    this->rs485_de_pin_->digital_write(false);
  }
  this->transition_(WAIT_REGISTRATION);
  this->last_rx_ms_ = millis();
  this->last_heartbeat_ms_ = millis();
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
  if (now - this->last_heartbeat_ms_ >= this->heartbeat_interval_ms_) {
    // TX path: heartbeat proves the controller is alive; current setpoint is
    // recalculated locally from the selected control mode and Janitza inputs.
    this->send_heartbeat_();
    this->send_current_setpoint_(this->controller_.calculate_current(this->inputs_));
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
}

void EvboxMaxComponent::set_failsafe_mode(FailsafeMode mode) {
  this->controller_.set_failsafe_mode(mode);
}

void EvboxMaxComponent::set_failsafe_current(float current) {
  this->controller_.set_failsafe_current(current);
}

void EvboxMaxComponent::update_janitza(float import_w, float export_w, bool online) {
  // The Janitza component only provides measurements. Charging policy remains
  // in ChargeController so meter communication and control logic do not blend.
  this->inputs_.grid_import_w = import_w;
  this->inputs_.grid_export_w = export_w;
  this->inputs_.janitza_online = online;
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
  switch (frame.type) {
    case FrameType::REGISTRATION:
      // First contact from the ChargeBox. If it has no address yet, assign the
      // first local address and immediately continue with information probing.
      this->chargebox_address_ = frame.address == 0 ? 1 : frame.address;
      this->transition_(ASSIGN_ADDRESS);
      this->send_frame_(FrameType::ADDRESS_ASSIGNMENT, {this->chargebox_address_});
      this->transition_(READ_INFO);
      this->send_frame_(FrameType::INFO_REQUEST);
      break;
    case FrameType::INFO_RESPONSE:
      // Hardware/model data has been read. Next step is configuration so the
      // controller knows what limits and capabilities the ChargeBox reports.
      this->transition_(READ_CONFIG);
      this->send_frame_(FrameType::CONFIG_REQUEST);
      break;
    case FrameType::CONFIG_RESPONSE:
      // Registration and startup reads are done; the controller may now wait
      // for local authorisation/start commands.
      this->transition_(IDLE);
      break;
    case FrameType::SESSION_STATUS:
      if (!frame.payload.empty()) {
        this->active_current_ = static_cast<float>(frame.payload[0]) / 10.0f;
      }
      // When a local session is active and the ChargeBox reports current, move
      // to CHARGING. More detailed pilot/contact states can be added here once
      // real G2 traces define the payload fields precisely.
      if (this->session_active_ && this->active_current_ > 0.0f) {
        this->transition_(CHARGING);
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

void EvboxMaxComponent::send_frame_(FrameType type, const std::vector<uint8_t> &payload) {
  const auto bytes = encode_frame(Frame{this->chargebox_address_, type, payload});
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

void EvboxMaxComponent::send_heartbeat_() {
  this->send_frame_(FrameType::HEARTBEAT, {static_cast<uint8_t>(this->session_active_ ? 1 : 0)});
}

void EvboxMaxComponent::send_current_setpoint_(float amps) {
  this->active_current_ = amps;
  // Payload is currently encoded as tenths of an ampere. Validate the exact
  // field size/order against captured EVBox MAX traffic before live charging.
  const auto tenths = static_cast<uint16_t>(amps * 10.0f);
  this->send_frame_(FrameType::CURRENT_SETPOINT, {
    static_cast<uint8_t>((tenths >> 8) & 0xFF),
    static_cast<uint8_t>(tenths & 0xFF),
  });
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
