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
static const char *const KNOWN_GOOD_METER_CONFIG =
    "00000E10000003840000001E03000001010030FF000000000000000100010000000003E8010000000100";

void EvboxMaxComponent::setup() {
  this->settings_pref_ = global_preferences->make_preference<StoredSettings>(fnv1_hash("evbox_max_settings"));
  this->load_settings_();
  this->session_active_ = false;
  this->stop_requested_ = false;
  this->start_requested_ = false;
  this->current_start_released_ = false;
  this->finished_reset_pending_ = false;
  this->remote_start_blocked_ = false;
  this->automatic_remote_start_attempted_ = false;
  this->remote_start_pending_ = false;
  this->delayed_start_trigger_pending_ = false;
  this->active_current_ = 0.0f;
  this->desired_current_ = 0.0f;
  this->commanded_current_ = 0.0f;
  this->last_current_request_code_ = 0;
  this->have_last_current_request_code_ = false;
  this->last_current_request_ms_ = 0;
  this->pending_current_request_code_ = 0;
  this->pending_current_request_after_config_ = false;
  this->delayed_current_release_pending_ = false;
  this->delayed_start_trigger_due_ms_ = 0;
  this->delayed_current_release_due_ms_ = 0;
  this->start_requested_ms_ = 0;
  this->last_cb_status_code_ = 0;
  this->have_last_cb_status_code_ = false;
  this->startup_config_received_ = false;
  this->known_good_meter_config_restore_attempted_ = false;
  this->known_good_meter_config_verified_ = false;
  this->last_meter_config_restore_ms_ = 0;
  this->meter_config_restore_attempts_ = 0;
  this->remote_start_config_write_attempted_ = false;
  this->remote_start_config_verified_ = false;
  this->last_startup_sync_request_ms_ = 0;
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
  this->last_periodic_cmd18_ms_ = millis();
  this->send_restart_registration_();
  if (this->chargebox_address_ != 0) {
    ESP_LOGI(TAG, "ChargeBox address retained after ESP restart; forcing startup sync for CB 0x%02X",
             this->chargebox_address_);
    this->last_startup_sync_request_ms_ = millis();
    this->schedule_startup_step_(1, 800);
  }
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
  if (this->state_ == FAULT && now - this->last_periodic_cmd18_ms_ >= this->heartbeat_interval_ms_) {
    if (now - this->last_rx_ms_ > this->watchdog_timeout_ms_) {
      ESP_LOGI(TAG, "EVBox bus silent in FAULT; requesting registration restart");
      this->send_restart_registration_();
    } else {
      ESP_LOGW(TAG, "EVBox reports FAULT but bus is alive; polling status without sending start/current commands");
      this->send_status_update_request_();
    }
    this->last_periodic_cmd18_ms_ = now;
  } else if (this->chargebox_address_ == 0 && now - this->last_periodic_cmd18_ms_ >= this->heartbeat_interval_ms_) {
    this->send_restart_registration_();
    this->last_periodic_cmd18_ms_ = now;
  } else if (this->chargebox_address_ != 0 && now - this->last_periodic_cmd18_ms_ >= this->heartbeat_interval_ms_) {
    // Periodic cmd18 is a status poll used by the current captures. It is kept
    // separate from the real MAX heartbeat cmd21, which is only sent as ACK.
    this->send_periodic_cmd18_();
    if (!this->startup_config_received_ && this->startup_step_ == 0 &&
        (this->last_startup_sync_request_ms_ == 0 || now - this->last_startup_sync_request_ms_ >= 10000UL)) {
      ESP_LOGI(TAG, "Startup sync not complete; retrying config/status sync for CB 0x%02X", this->chargebox_address_);
      this->last_startup_sync_request_ms_ = now;
      this->schedule_startup_step_(1, 100);
    }
    if (!this->stop_requested_ && this->charge_flow_requested_() && this->current_setpoint_allowed_()) {
      this->desired_current_ = this->controller_.calculate_current(this->inputs_);
      float delta = this->desired_current_ - this->active_current_;
      if (delta < 0.0f) delta = -delta;
      if (this->active_current_ <= 0.0f || delta >= 0.5f) {
        this->send_current_setpoint_(this->desired_current_);
      }
    }
    if (this->start_requested_ && !this->session_active_ && this->start_requested_ms_ != 0 &&
        now - this->start_requested_ms_ >= 60000UL) {
      ESP_LOGW(TAG, "Start request timed out; last cmd26=%s last cmd6A=%s",
               this->have_last_cb_status_code_ ? this->cb_status_name_(this->last_cb_status_code_) : "UNKNOWN",
               this->have_last_current_request_code_ ? this->current_request_name_(this->last_current_request_code_) : "UNKNOWN");
      this->start_requested_ = false;
      this->current_start_released_ = false;
      this->delayed_current_release_pending_ = false;
      this->finished_reset_pending_ = false;
      this->delayed_start_trigger_pending_ = false;
      this->remote_start_pending_ = false;
      if (this->have_last_cb_status_code_ && this->last_cb_status_code_ == 0x47) {
        this->remote_start_blocked_ = true;
      }
      this->start_requested_ms_ = 0;
      if (!this->stop_requested_ && this->state_ != CHARGING) {
        this->transition_(this->have_last_cb_status_code_ && this->last_cb_status_code_ == 0x47 ? PREPARING
                                                                                                  : this->state_);
      }
    }
    this->last_periodic_cmd18_ms_ = now;
  }

  this->watchdog_();
  this->run_startup_sequence_();
  this->run_delayed_start_trigger_();
  this->run_delayed_current_release_();
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
  ESP_LOGCONFIG(TAG, "  ChargeBox serial: %s", this->chargebox_serial_.c_str());
  ESP_LOGCONFIG(TAG, "  ChargeBox firmware: %u", this->chargebox_firmware_);
  ESP_LOGCONFIG(TAG, "  ChargeBox hardware generation: %u", this->chargebox_hardware_generation_);
  ESP_LOGCONFIG(TAG, "  Protocol profile: %s", this->protocol_profile_name_());
  ESP_LOGCONFIG(TAG, "  Commissioning mode: %s", this->commissioning_mode_ ? "YES" : "NO");
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
  const bool charge_flow_active = this->session_active_ || this->state_ == STARTING || this->state_ == SESSION_STARTING ||
                                  this->state_ == CHARGING;
  if (!this->stop_requested_ && charge_flow_active && next_current < this->active_current_) {
    if (!this->current_setpoint_allowed_()) {
      ESP_LOGD(TAG, "Calculated lower current %.1f A, but cmd6B is not released yet", next_current);
      return;
    }
    // Overload response path: do not wait for the next heartbeat tick when the
    // meter says current must go down. A lower setpoint is sent immediately.
    this->send_current_setpoint_(next_current);
    this->last_periodic_cmd18_ms_ = millis();
  }
}

void EvboxMaxComponent::start_session() {
  if (!this->authorization_allowed_()) {
    ESP_LOGW(TAG, "Local start blocked; charge mode is disabled/off");
    this->transition_(this->have_last_cb_status_code_ && this->last_cb_status_code_ == 0x47 ? PREPARING : IDLE);
    return;
  }

  this->remote_start_blocked_ = false;
  this->automatic_remote_start_attempted_ = false;
  this->stop_requested_ = false;
  this->session_active_ = false;
  this->current_start_released_ = false;
  this->finished_reset_pending_ = false;
  this->delayed_start_trigger_pending_ = false;
  this->delayed_current_release_pending_ = false;
  this->remote_start_pending_ = false;
  this->start_requested_ = true;
  this->start_requested_ms_ = millis();
  ESP_LOGI(TAG, "Local start requested; sending cmd31 remote start and waiting for CB cmd6A flow");
  if (this->send_remote_start_()) {
    this->remote_start_pending_ = true;
    this->automatic_remote_start_attempted_ = true;
    this->transition_(STARTING);
    return;
  }

  const bool connected_waiting_state =
      this->have_last_cb_status_code_ && this->cb_cable_max_current_ > 0 &&
      (this->last_cb_status_code_ == 0x0A || this->last_cb_status_code_ == 0x4B) &&
      this->have_last_current_request_code_ && this->last_current_request_code_ == 0x30;
  if (connected_waiting_state) {
    ESP_LOGI(TAG, "Local start in %s/connected waiting state; cmd31 could not be sent, waiting for CB ready request",
             this->cb_status_name_(this->last_cb_status_code_));
    this->transition_(STARTING);
  } else if (this->have_last_cb_status_code_ && this->last_cb_status_code_ == 0x4B && this->cb_cable_max_current_ > 0) {
    this->finished_reset_pending_ = true;
    this->remote_start_blocked_ = false;
    this->automatic_remote_start_attempted_ = false;
    ESP_LOGI(TAG, "Start requested while CB is FINISHED_PLUGGED_IN without cmd6A=0x30; sending cmd32 reset before restart");
    this->send_remote_stop_();
    this->transition_(PREPARING);
    return;
  } else if (this->have_last_cb_status_code_ && this->last_cb_status_code_ == 0x47) {
    ESP_LOGI(TAG, "Local start while CB is PREPARING_G3; cmd31 could not be sent, waiting for CB cmd22/cmd6A");
    this->transition_(STARTING);
  } else {
    ESP_LOGI(TAG, "Start request queued until CB reports PREPARING_G3/cmd22/cmd23");
    this->transition_(this->cb_cable_max_current_ > 0 ? PREPARING : IDLE);
  }
}

void EvboxMaxComponent::stop_session() {
  this->stop_requested_ = true;
  this->start_requested_ = false;
  this->finished_reset_pending_ = false;
  this->delayed_start_trigger_pending_ = false;
  this->delayed_current_release_pending_ = false;
  this->remote_start_pending_ = false;
  this->start_requested_ms_ = 0;
  this->send_remote_stop_();
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
      if (frame.data.size() >= 15) {
        this->chargebox_hardware_generation_ = static_cast<uint8_t>(std::strtoul(frame.data.substr(11, 4).c_str(), nullptr, 10));
      }
      this->chargebox_address_ = frame.src == 0 ? 1 : frame.src;
      this->startup_config_received_ = false;
      this->known_good_meter_config_restore_attempted_ = false;
      this->known_good_meter_config_verified_ = false;
      this->last_meter_config_restore_ms_ = 0;
      this->meter_config_restore_attempts_ = 0;
      this->remote_start_config_write_attempted_ = false;
      this->remote_start_config_verified_ = false;
      this->last_startup_sync_request_ms_ = millis();
      ESP_LOGI(TAG, "CB registration serial=%s assign=0x%02X firmware=%u hw_gen=%u profile=%s",
               this->chargebox_serial_.c_str(), this->chargebox_address_, this->chargebox_firmware_,
               this->chargebox_hardware_generation_, this->protocol_profile_name_());
      this->transition_(ASSIGN_ADDRESS);
      this->send_packet_(ADDR_BROADCAST, 0x11, this->chargebox_serial_ + hex_byte(this->chargebox_address_) + "03");
      this->transition_(READ_INFO);
      this->schedule_startup_step_(1, 300);
      break;
    case FrameType::INFO_RESPONSE:
      this->update_meter_info_(frame.data);
      // Hardware/model data has been read. Next step is configuration so the
      // controller knows what limits and capabilities the ChargeBox reports.
      this->transition_(READ_CONFIG);
      this->schedule_startup_step_(5, 300);
      break;
    case FrameType::CONFIG_RESPONSE:
      ESP_LOGI(TAG, "CB config received; preserving meter settings while checking remote start flag");
      this->startup_config_received_ = true;
      this->log_autostart_config_(frame.data);
      if (this->commissioning_mode_ && frame.data.size() >= 68) {
        const uint8_t allow_remote_start = parse_hex_byte(frame.data, 66, 0xFF);
        const uint8_t meter_config = parse_hex_byte(frame.data, 30, 0xFF);
        const bool needs_known_good_meter_restore = meter_config != 0x01;
        const bool needs_remote_start_restore = allow_remote_start == 0x00;
        const bool needs_serial_meter_restore = meter_config != 0x01;
        if (needs_known_good_meter_restore) {
          this->known_good_meter_config_verified_ = false;
          const uint32_t now = millis();
          const uint32_t elapsed = now - this->last_meter_config_restore_ms_;
          const bool first_attempt = this->meter_config_restore_attempts_ == 0;
          const bool retry_due = first_attempt || elapsed >= 30000UL;
          if (retry_due && this->meter_config_restore_attempts_ < 5) {
            if (this->send_known_good_meter_config_restore_(frame.data)) {
              this->known_good_meter_config_restore_attempted_ = true;
              this->last_meter_config_restore_ms_ = now;
              this->meter_config_restore_attempts_++;
              ESP_LOGW(TAG, "Serial meter config restore attempt %u sent; waiting for cmd33 read-back",
                       this->meter_config_restore_attempts_);
              break;
            }
          } else {
            const uint32_t retry_in_ms = retry_due ? 0UL : 30000UL - elapsed;
            ESP_LOGW(TAG, "Serial meter config still not restored: meter_config=0x%02X attempts=%u next_retry_in=%us",
                     meter_config, this->meter_config_restore_attempts_,
                     static_cast<unsigned>(retry_in_ms / 1000UL));
          }
        } else {
          this->known_good_meter_config_verified_ = true;
          ESP_LOGI(TAG, "CB config verified: meter type is serial/Modbus");
        }
        if (needs_serial_meter_restore && !needs_known_good_meter_restore) {
          ESP_LOGW(TAG, "CB meter type decode is still provisional although full config matches known-good snapshot");
        }
        if (needs_remote_start_restore) {
          if (!this->remote_start_config_write_attempted_) {
            if (this->send_remote_start_config_enable_(frame.data)) {
              this->remote_start_config_write_attempted_ = true;
            }
          } else if (!this->remote_start_config_verified_) {
            ESP_LOGW(TAG, "CB remote start flag still not restored after accepted cmd34; remote_start=0x%02X",
                     allow_remote_start);
          }
        } else {
          this->remote_start_config_verified_ = true;
          ESP_LOGI(TAG, "CB remote start config verified enabled");
        }
      }
      if (this->pending_current_request_after_config_ && !this->stop_requested_ &&
          this->is_supported_current_request_(this->pending_current_request_code_)) {
        ESP_LOGI(TAG, "Deferred CB current request %s acknowledged after config sync; waiting for explicit start",
                 this->current_request_name_(this->pending_current_request_code_));
        this->pending_current_request_after_config_ = false;
        this->transition_(this->have_last_cb_status_code_ && this->last_cb_status_code_ == 0x47 ? PREPARING : IDLE);
      } else if (this->have_last_cb_status_code_ && this->last_cb_status_code_ == 0x47) {
        this->transition_(this->start_requested_ ? STARTING : PREPARING);
      } else if (this->have_last_cb_status_code_ && this->last_cb_status_code_ == 0x02) {
        this->transition_(IDLE);
      } else {
        this->transition_(IDLE);
      }
      break;
    case FrameType::CONFIG_SET_RESPONSE:
      if (frame.data == hex_word(ACK)) {
        ESP_LOGI(TAG, "CB config accepted; requesting cmd13 meter info before cmd33 read-back");
        this->remote_start_config_verified_ = false;
        this->schedule_startup_step_(4, 800);
      } else {
        ESP_LOGW(TAG, "CB config response data=%s", frame.data.c_str());
      }
      // Registration and startup config are done; the controller may now wait
      // for local authorisation/start commands.
      if (this->have_last_cb_status_code_ && this->last_cb_status_code_ == 0x47) {
        this->transition_(this->start_requested_ ? STARTING : PREPARING);
      } else {
        this->transition_(IDLE);
      }
      break;
    case FrameType::HEARTBEAT:
      if (frame.dst == ADDR_CP && frame.src >= 1 && frame.src <= 20) {
        this->note_chargebox_seen_(frame.src);
        this->evbox_online_ = true;
        this->last_heartbeat_rx_ms_ = millis();
        ESP_LOGI(TAG, "CB heartbeat cmd21 received; sending ACK");
        this->send_packet_(frame.src, 0x21, "");
        this->last_heartbeat_tx_ms_ = millis();
      }
      break;
    case FrameType::AUTHENTICATE_CARD:
      if (frame.dst == ADDR_CP && frame.src >= 1 && frame.src <= 20) {
        this->note_chargebox_seen_(frame.src);
        this->evbox_online_ = true;
        const uint8_t auth_state = parse_hex_byte(frame.data, 0);
        const uint8_t card_len = parse_hex_byte(frame.data, 2);
        const size_t available = frame.data.size() > 4 ? frame.data.size() - 4 : 0;
        const size_t safe_len = std::min<size_t>(card_len, available);
        const std::string card = frame.data.substr(4, safe_len);
        const bool autostart_card = card == "000000AS";
        const bool access_granted = !this->stop_requested_ && this->authorization_allowed_();
        ESP_LOGI(TAG, "CB authenticate request state=0x%02X card=%s access=%s", auth_state, card.c_str(),
                 access_granted ? "granted" : "denied");
        const std::string padded_card = (card + std::string(22, '0')).substr(0, 22);
        this->send_packet_(frame.src, 0x22,
                           hex_byte(access_granted ? 0x01 : 0x12) + hex_byte(card_len) + padded_card + "FFFF");
        if (access_granted && autostart_card && !this->charge_flow_requested_()) {
          ESP_LOGI(TAG, "CB autostart card accepted; enabling local charge tracking");
          this->start_requested_ = true;
          if (this->start_requested_ms_ == 0) this->start_requested_ms_ = millis();
        }
        if (access_granted && (this->state_ == IDLE || this->state_ == PREPARING)) {
          this->transition_(AUTHORIZED);
        }
      }
      break;
    case FrameType::CURRENT_REQUEST:
      if (frame.dst == ADDR_CP && frame.src >= 1 && frame.src <= 20) {
        this->note_chargebox_seen_(frame.src);
        this->evbox_online_ = true;
        const uint8_t request_code = parse_hex_byte(frame.data, 0);
        this->last_current_request_code_ = request_code;
        this->have_last_current_request_code_ = true;
        this->last_current_request_ms_ = millis();
        this->send_packet_(frame.src, 0x6A, hex_word(ACK));
        ESP_LOGI(TAG, "CB cmd6A raw data=%s len=%u", frame.data.c_str(), static_cast<unsigned>(frame.data.size()));
        if (frame.data.size() >= 4) {
          const uint8_t request_value = parse_hex_byte(frame.data, 2);
          ESP_LOGI(TAG, "CB cmd6A decode: byte0 state=0x%02X %s byte1 value=0x%02X", request_code,
                   this->current_request_name_(request_code), request_value);
        } else {
          ESP_LOGI(TAG, "CB cmd6A decode: byte0 state=0x%02X %s", request_code,
                   this->current_request_name_(request_code));
        }
        if (!this->startup_config_received_ && this->is_supported_current_request_(request_code) &&
            request_code != 0x81 && request_code != 0x01 && !this->charge_flow_requested_()) {
          ESP_LOGI(TAG, "Deferring CB current request %s until config sync is complete",
                   this->current_request_name_(request_code));
          this->pending_current_request_code_ = request_code;
          this->pending_current_request_after_config_ = true;
          break;
        }
        if (this->stop_requested_) {
          ESP_LOGI(TAG, "CB current request acknowledged; no current limit sent while stop is requested");
          break;
        }

        if (request_code == 0x81 || request_code == 0x01) {
          this->session_active_ = true;
          this->current_start_released_ = true;
          this->start_requested_ = false;
          this->start_requested_ms_ = 0;
          this->delayed_current_release_pending_ = false;
          this->remote_start_pending_ = false;
          this->transition_(CHARGING);
          break;
        }

        if (request_code == 0x80 || request_code == 0xA0 || request_code == 0xC1 || request_code == 0xE7) {
          this->session_active_ = false;
          this->start_requested_ = false;
          this->current_start_released_ = false;
          this->delayed_current_release_pending_ = false;
          this->remote_start_pending_ = false;
          this->start_requested_ms_ = 0;
          this->transition_(request_code == 0xE7 ? FAULT : IDLE);
          break;
        }

        if (request_code == 0x37) {
          if (this->start_requested_) {
            ESP_LOGI(TAG, "CB reports AUTHORIZED_WAIT_LOCK; waiting for CONNECTED_WAITING before cmd6B");
            this->transition_(STARTING);
          } else {
            ESP_LOGI(TAG, "CB reports AUTHORIZED_WAIT_LOCK without active start request; ACK only");
          }
          break;
        }

        if (request_code == 0x30) {
          if (this->start_requested_ && !this->current_start_released_ && !this->delayed_current_release_pending_) {
            this->desired_current_ = this->controller_.calculate_current(this->inputs_);
            ESP_LOGI(TAG, "CB current request CONNECTED_WAITING with active start; scheduling cmd6B current release %.1f A",
                     this->desired_current_);
            this->schedule_current_release_(100);
            this->transition_(STARTING);
          } else {
            ESP_LOGI(TAG, "CB current request WAITING_FOR_CMD26 acknowledged; no cmd6B release needed");
          }
          if (!this->charge_flow_requested_()) {
            this->transition_(this->have_last_cb_status_code_ && this->last_cb_status_code_ == 0x47 ? PREPARING : IDLE);
          }
          break;
        }

        if (!this->is_supported_current_request_(request_code)) {
          ESP_LOGW(TAG, "CB current request state 0x%02X is not a start-release event; ACK only", request_code);
          break;
        }

        if (this->current_request_allows_start_(request_code) && !this->current_start_released_) {
          this->desired_current_ = this->controller_.calculate_current(this->inputs_);
          this->start_requested_ = true;
          this->remote_start_pending_ = false;
          if (this->state_ != CHARGING && this->state_ != STARTING) {
            this->transition_(STARTING);
          }
          ESP_LOGI(TAG, "Scheduling delayed cmd6B current release after allowed CB current request %s",
                   this->current_request_name_(request_code));
          this->schedule_current_release_(800);
        } else {
          ESP_LOGI(TAG, "CB current request %s acknowledged; start guard not satisfied",
                   this->current_request_name_(request_code));
        }
      }
      break;
    case FrameType::CURRENT_SETPOINT:
      if (frame.dst == ADDR_CP && frame.src >= 1 && frame.src <= 20) {
        this->note_chargebox_seen_(frame.src);
        this->evbox_online_ = true;
        ESP_LOGD(TAG, "CB acknowledged current setpoint");
        // A cmd6B ACK only confirms that the current setpoint was accepted on
        // the MAX bus. The CB session starts only when cmd23 or CHARGING state
        // follows, so keep the controller in STARTING until the CB proves it.
      }
      break;
    case FrameType::STATE_UPDATE:
      if (frame.dst == ADDR_CP && frame.src >= 1 && frame.src <= 20) {
        this->note_chargebox_seen_(frame.src);
        this->evbox_online_ = true;
        const uint8_t code = parse_hex_byte(frame.data, 0);
        const uint8_t is_charging = parse_hex_byte(frame.data, 6);
        const uint8_t led_colour = parse_hex_byte(frame.data, 8);
        const uint8_t lock_state = parse_hex_byte(frame.data, 10);
        const uint8_t cable_current = parse_hex_byte(frame.data, 12);
        ESP_LOGI(TAG, "CB cmd26 raw data=%s len=%u", frame.data.c_str(), static_cast<unsigned>(frame.data.size()));
        ESP_LOGI(TAG,
                 "CB cmd26 decode: byte0 status=0x%02X %s byte3 is_charging=%u byte4 led=0x%02X byte5 lock=%u byte6 cable=%uA",
                 code, this->cb_status_name_(code), is_charging, led_colour, lock_state, cable_current);
        this->cb_is_charging_ = is_charging;
        this->cb_led_colour_ = led_colour;
        this->cb_lock_state_ = lock_state;
        this->cb_cable_max_current_ = cable_current;
        this->cable_status_ = cable_current > 0 ? "CONNECTED" : "UNPLUGGED";
        this->lock_status_ = lock_state != 0 ? "LOCKED" : "UNLOCKED";
        this->last_cb_status_code_ = code;
        this->have_last_cb_status_code_ = true;
        ESP_LOGI(TAG, "CB state cmd26 status=0x%02X %s is_charging=%u led=0x%02X lock=%u cable=%u data_len=%u",
                 code, this->cb_status_name_(code), is_charging, led_colour, lock_state, cable_current,
                 static_cast<unsigned>(frame.data.size()));
        if (code == 0x02) {
          this->stop_requested_ = false;
          this->session_active_ = false;
          this->start_requested_ = false;
          this->current_start_released_ = false;
          this->delayed_current_release_pending_ = false;
          this->remote_start_blocked_ = false;
          this->automatic_remote_start_attempted_ = false;
          this->remote_start_pending_ = false;
          this->start_requested_ms_ = 0;
          this->transition_(IDLE);
        } else if (code == 0x17) {
          if (this->stop_requested_) {
            this->session_active_ = false;
            this->transition_(FINISHING);
          } else {
            this->session_active_ = false;
            this->transition_(PREPARING);
          }
        }
        else if (code == 0x47) {
          if (this->stop_requested_) {
            this->session_active_ = false;
            this->transition_(FINISHING);
          } else if (this->start_requested_) {
            this->session_active_ = false;
            this->desired_current_ = this->controller_.calculate_current(this->inputs_);
            if (this->finished_reset_pending_) {
              this->finished_reset_pending_ = false;
              ESP_LOGI(TAG, "CB returned PREPARING_G3 after 4B reset; sending cmd31 remote start");
              if (this->send_remote_start_()) {
                this->remote_start_pending_ = true;
                this->automatic_remote_start_attempted_ = true;
              }
            } else if (!this->current_start_released_ && !this->delayed_current_release_pending_) {
              ESP_LOGI(TAG, "CB is PREPARING_G3 with queued start request; waiting for cmd31 response or CB cmd22/cmd6A");
            }
            this->transition_(STARTING);
          } else {
            this->desired_current_ = this->controller_.calculate_current(this->inputs_);
            if (this->remote_start_blocked_) {
              ESP_LOGI(TAG, "CB is PREPARING_G3, but automatic remote start is blocked after last cmd31 failure");
              this->session_active_ = false;
              this->transition_(PREPARING);
            } else {
              ESP_LOGI(TAG, "CB is PREPARING_G3 with cable present; waiting for explicit local start");
              this->session_active_ = false;
              this->transition_(PREPARING);
            }
          }
        }
        else if (code == 0x4A) {
          if (this->stop_requested_) {
            this->session_active_ = false;
            this->transition_(FINISHING);
          } else if (this->start_requested_) {
            this->transition_(AUTHORIZED);
          } else {
            this->session_active_ = false;
            this->transition_(IDLE);
          }
        }
        else if (code == 0x48) {
          this->stop_requested_ = false;
          this->session_active_ = true;
          this->start_requested_ = false;
          this->current_start_released_ = true;
          this->delayed_current_release_pending_ = false;
          this->finished_reset_pending_ = false;
          this->delayed_start_trigger_pending_ = false;
          this->remote_start_blocked_ = false;
          this->automatic_remote_start_attempted_ = false;
          this->remote_start_pending_ = false;
          this->start_requested_ms_ = 0;
          this->transition_(CHARGING);
        } else if (code == 0x4B) {
          this->stop_requested_ = false;
          this->session_active_ = false;
          this->remote_start_pending_ = false;
          if (!this->start_requested_) {
            this->current_start_released_ = false;
            this->delayed_current_release_pending_ = false;
            this->remote_start_blocked_ = false;
            this->automatic_remote_start_attempted_ = false;
            this->finished_reset_pending_ = false;
            this->delayed_start_trigger_pending_ = false;
            this->start_requested_ms_ = 0;
          } else if (this->remote_start_blocked_) {
            ESP_LOGW(TAG, "CB reports FINISHED_PLUGGED_IN/CONNECTED_WAITING but remote start is blocked; not sending cmd6B");
          } else if (this->have_last_current_request_code_ && this->last_current_request_code_ == 0x30) {
            this->finished_reset_pending_ = false;
            this->delayed_start_trigger_pending_ = false;
            if (!this->current_start_released_ && !this->delayed_current_release_pending_) {
              this->desired_current_ = this->controller_.calculate_current(this->inputs_);
              ESP_LOGI(TAG, "CB reports FINISHED_PLUGGED_IN with CONNECTED_WAITING; scheduling cmd6B current release %.1f A",
                       this->desired_current_);
              this->schedule_current_release_(750);
            }
          } else if (!this->finished_reset_pending_) {
            this->current_start_released_ = false;
            this->delayed_current_release_pending_ = false;
            this->finished_reset_pending_ = true;
            this->remote_start_blocked_ = false;
            this->automatic_remote_start_attempted_ = false;
            this->delayed_start_trigger_pending_ = false;
            ESP_LOGI(TAG, "CB reports FINISHED_PLUGGED_IN with queued start; sending cmd32 reset");
            this->send_remote_stop_();
          } else {
            ESP_LOGI(TAG, "CB reports FINISHED_PLUGGED_IN; reset already pending, waiting for PREPARING/auth flow");
          }
          this->transition_(this->start_requested_ ? STARTING : (cable_current > 0 ? PREPARING : IDLE));
        }
        else if (code == 0x0A) {
          const bool connected_waiting = cable_current > 0 && this->have_last_current_request_code_ &&
                                         this->last_current_request_code_ == 0x30;
          if (this->startup_config_received_ && connected_waiting) {
            ESP_LOGW(TAG, "CB reports 0x0A with cable=%uA and cmd6A=0x30; treating as connected/preparing",
                     cable_current);
            this->session_active_ = false;
            this->remote_start_pending_ = false;
            if (this->start_requested_) {
              if (!this->current_start_released_ && !this->delayed_current_release_pending_) {
                this->desired_current_ = this->controller_.calculate_current(this->inputs_);
                ESP_LOGI(TAG, "Queued start request in 0x0A/connected state; scheduling cmd6B current release %.1f A",
                         this->desired_current_);
                this->schedule_current_release_(750);
              }
              this->transition_(STARTING);
            } else {
              this->current_start_released_ = false;
              this->transition_(PREPARING);
            }
          } else {
          this->start_requested_ = false;
          this->session_active_ = false;
          this->current_start_released_ = false;
          this->delayed_current_release_pending_ = false;
          this->finished_reset_pending_ = false;
          this->delayed_start_trigger_pending_ = false;
          this->remote_start_blocked_ = false;
          this->automatic_remote_start_attempted_ = false;
          this->remote_start_pending_ = false;
          this->start_requested_ms_ = 0;
          if (!this->startup_config_received_) {
            ESP_LOGW(TAG, "CB reports ERROR 0x0A during startup; keeping startup sync alive before declaring FAULT");
            this->transition_(READ_CONFIG);
            if (this->startup_step_ == 0) {
              this->last_startup_sync_request_ms_ = millis();
              this->schedule_startup_step_(1, 100);
            }
          } else if (cable_current > 0 && this->have_last_current_request_code_ &&
                     this->is_supported_current_request_(this->last_current_request_code_)) {
            ESP_LOGW(TAG, "CB reports ERROR 0x0A but cmd6A=%s and cable=%uA are present; treating as PREPARING for manual diagnostics",
                     this->current_request_name_(this->last_current_request_code_), cable_current);
            this->transition_(PREPARING);
          } else {
            this->transition_(FAULT);
          }
          }
        }
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
        const bool g3_like_state = frame.data.size() >= 128;
        const std::string ack_data = g3_like_state ? hex_dword(this->session_) + hex_dword(this->seconds_since_2000_())
                                                   : hex_dword(this->session_);
        this->send_packet_(frame.src, 0x26, ack_data);
      }
      break;
    case FrameType::REMOTE_START:
      if (frame.dst == ADDR_CP && frame.src >= 1 && frame.src <= 20) {
        const uint8_t result = parse_hex_byte(frame.data, 0);
        ESP_LOGI(TAG, "CB remote start response=0x%02X %s", result, result == 0x01 ? "success" : "failed");
        this->remote_start_pending_ = false;
        if (result == 0x01 && !this->stop_requested_) {
          this->start_requested_ = true;
          this->remote_start_blocked_ = false;
          if (this->state_ != CHARGING && this->state_ != SESSION_STARTING) {
            this->transition_(STARTING);
          }
          if (this->have_last_current_request_code_ && this->last_current_request_code_ == 0x30 &&
              this->cb_cable_max_current_ > 0 && !this->current_start_released_ &&
              !this->delayed_current_release_pending_) {
            this->desired_current_ = this->controller_.calculate_current(this->inputs_);
            ESP_LOGI(TAG, "Remote start accepted while CB is already CONNECTED_WAITING; scheduling cmd6B %.1f A",
                     this->desired_current_);
            this->schedule_current_release_(800);
          }
        } else if (!this->stop_requested_ && this->start_requested_) {
          ESP_LOGW(TAG, "Remote start failed; waiting for CB autostart cmd22/cmd6A flow");
          this->start_requested_ = false;
          this->start_requested_ms_ = 0;
          this->delayed_current_release_pending_ = false;
          this->delayed_start_trigger_pending_ = false;
          this->current_start_released_ = false;
          this->finished_reset_pending_ = false;
          this->remote_start_pending_ = false;
          this->remote_start_blocked_ = true;
          this->transition_(this->cb_cable_max_current_ > 0 ? PREPARING : IDLE);
        }
      }
      break;
    case FrameType::REMOTE_STOP:
      if (frame.dst == ADDR_CP && frame.src >= 1 && frame.src <= 20) {
        const uint8_t result = parse_hex_byte(frame.data, 0);
        ESP_LOGI(TAG, "CB remote stop response=0x%02X %s", result, result == 0x01 ? "success" : "failed");
        if (result == 0x01) {
          if (this->finished_reset_pending_ && this->start_requested_) {
            ESP_LOGI(TAG, "CB accepted 4B reset; keeping start request and waiting for PREPARING");
            this->stop_requested_ = false;
            this->session_active_ = false;
            this->current_start_released_ = false;
            this->delayed_current_release_pending_ = false;
            this->remote_start_pending_ = false;
            this->transition_(PREPARING);
          } else {
            this->start_requested_ = false;
            this->session_active_ = false;
            this->current_start_released_ = false;
            this->finished_reset_pending_ = false;
            this->delayed_start_trigger_pending_ = false;
            this->delayed_current_release_pending_ = false;
            this->remote_start_pending_ = false;
            this->start_requested_ms_ = 0;
            this->transition_(FINISHING);
          }
        } else if (this->finished_reset_pending_ && this->start_requested_ && this->have_last_current_request_code_ &&
                   this->last_current_request_code_ == 0x30) {
          this->finished_reset_pending_ = false;
          this->desired_current_ = this->controller_.calculate_current(this->inputs_);
          ESP_LOGW(TAG, "CB rejected cmd32 reset while CONNECTED_WAITING is present; falling back to cmd6B current release %.1f A",
                   this->desired_current_);
          this->schedule_current_release_(750);
          this->transition_(STARTING);
        }
      }
      break;
    case FrameType::METERING_START:
      if (frame.dst == ADDR_CP && frame.src >= 1 && frame.src <= 20) {
        const bool accept_session = !this->stop_requested_;
        this->session_active_ = accept_session;
        if (accept_session) {
          this->session_start_meter_kwh_ = this->meter_value_kwh_;
          this->have_session_start_meter_ = !std::isnan(this->meter_value_kwh_) && this->meter_value_kwh_ > 0.0f;
          this->current_start_released_ = true;
          this->start_requested_ = false;
          this->delayed_current_release_pending_ = false;
          this->remote_start_pending_ = false;
          this->start_requested_ms_ = 0;
          this->transition_(CHARGING);
        } else {
          ESP_LOGI(TAG, "CB metering start acknowledged without activating local session; stop is requested");
        }
        const std::string data = "010000000000000000";
        this->send_packet_(frame.src, 0x23, data);
      }
      break;
    case FrameType::METERING_END:
      if (frame.dst == ADDR_CP && frame.src >= 1 && frame.src <= 20) {
        this->session_active_ = false;
        this->start_requested_ = false;
        this->current_start_released_ = false;
        this->delayed_current_release_pending_ = false;
        this->remote_start_pending_ = false;
        this->start_requested_ms_ = 0;
        this->send_packet_(frame.src, 0x24, "01");
        this->transition_(FINISHING);
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
    const uint32_t raw_temperature = parse_hex_uint(data, 52, 4);
    this->temperature_c_ = static_cast<float>(raw_temperature) / 10.0f;
    ESP_LOGD(TAG, "CB cmd26 raw temperature offset52=%04X -> %.1fC", static_cast<unsigned>(raw_temperature), this->temperature_c_);
  }

  if (data.size() >= 132) {
    const uint32_t raw_l1_voltage = parse_hex_uint(data, 68, 4);
    const uint32_t raw_l2_voltage = parse_hex_uint(data, 72, 4);
    const uint32_t raw_l3_voltage = parse_hex_uint(data, 76, 4);
    const uint32_t raw_l1_current = parse_hex_uint(data, 80, 4);
    const uint32_t raw_l2_current = parse_hex_uint(data, 84, 4);
    const uint32_t raw_l3_current = parse_hex_uint(data, 88, 4);
    const uint32_t raw_socket_temperature = parse_hex_uint(data, 92, 4);
    const uint32_t raw_pf1 = parse_hex_uint(data, 96, 4);
    const uint32_t raw_pf2 = parse_hex_uint(data, 100, 4);
    const uint32_t raw_pf3 = parse_hex_uint(data, 104, 4);
    const uint32_t measurement_block = raw_l1_voltage | raw_l2_voltage | raw_l3_voltage | raw_l1_current |
                                       raw_l2_current | raw_l3_current | raw_socket_temperature | raw_pf1 | raw_pf2 |
                                       raw_pf3;
    ESP_LOGD(TAG,
             "CB cmd26 meter raw offsets: V68/72/76=%04X/%04X/%04X I80/84/88=%04X/%04X/%04X socket92=%04X PF96/100/104=%04X/%04X/%04X",
             static_cast<unsigned>(raw_l1_voltage), static_cast<unsigned>(raw_l2_voltage), static_cast<unsigned>(raw_l3_voltage), static_cast<unsigned>(raw_l1_current), static_cast<unsigned>(raw_l2_current), static_cast<unsigned>(raw_l3_current),
             static_cast<unsigned>(raw_socket_temperature), static_cast<unsigned>(raw_pf1), static_cast<unsigned>(raw_pf2), static_cast<unsigned>(raw_pf3));
    if (measurement_block != 0) {
      this->ev_l1_voltage_v_ = static_cast<float>(raw_l1_voltage);
      this->ev_l2_voltage_v_ = static_cast<float>(raw_l2_voltage);
      this->ev_l3_voltage_v_ = static_cast<float>(raw_l3_voltage);
      this->ev_l1_current_a_ = static_cast<float>(raw_l1_current) / 100.0f;
      this->ev_l2_current_a_ = static_cast<float>(raw_l2_current) / 100.0f;
      this->ev_l3_current_a_ = static_cast<float>(raw_l3_current) / 100.0f;
      this->ev_l1_power_factor_ = static_cast<float>(raw_pf1) / 1000.0f;
      this->ev_l2_power_factor_ = static_cast<float>(raw_pf2) / 1000.0f;
      this->ev_l3_power_factor_ = static_cast<float>(raw_pf3) / 1000.0f;
      const float socket_temperature_c = static_cast<float>(raw_socket_temperature);
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
  this->raw_meter_wh_ = static_cast<float>(raw_meter);
  ESP_LOGD(TAG, "CB cmd26 meter counter raw offset18=%08X -> %u Wh", static_cast<unsigned>(raw_meter), static_cast<unsigned>(raw_meter));
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

  const uint32_t raw_l1_voltage = parse_hex_uint(data, 0, 4);
  const uint32_t raw_l2_voltage = parse_hex_uint(data, 4, 4);
  const uint32_t raw_l3_voltage = parse_hex_uint(data, 8, 4);
  const uint32_t raw_l1_current = parse_hex_uint(data, 12, 4);
  const uint32_t raw_l2_current = parse_hex_uint(data, 16, 4);
  const uint32_t raw_l3_current = parse_hex_uint(data, 20, 4);
  const uint32_t raw_pf1 = parse_hex_uint(data, 24, 4);
  const uint32_t raw_pf2 = parse_hex_uint(data, 28, 4);
  const uint32_t raw_pf3 = parse_hex_uint(data, 32, 4);
  const uint32_t raw_meter = parse_hex_uint(data, 36, 8);
  this->raw_meter_wh_ = static_cast<float>(raw_meter);
  const bool all_zero_block = raw_l1_voltage == 0 && raw_l2_voltage == 0 && raw_l3_voltage == 0 &&
                              raw_l1_current == 0 && raw_l2_current == 0 && raw_l3_current == 0 &&
                              raw_pf1 == 0 && raw_pf2 == 0 && raw_pf3 == 0 && raw_meter == 0;
  if (all_zero_block) {
    ESP_LOGD(TAG, "CB meter push is all zero; keeping previous EVBox meter measurements");
    return;
  }

  this->ev_l1_voltage_v_ = static_cast<float>(raw_l1_voltage);
  this->ev_l2_voltage_v_ = static_cast<float>(raw_l2_voltage);
  this->ev_l3_voltage_v_ = static_cast<float>(raw_l3_voltage);
  this->ev_l1_current_a_ = static_cast<float>(raw_l1_current) / 100.0f;
  this->ev_l2_current_a_ = static_cast<float>(raw_l2_current) / 100.0f;
  this->ev_l3_current_a_ = static_cast<float>(raw_l3_current) / 100.0f;
  this->ev_l1_power_factor_ = static_cast<float>(raw_pf1) / 1000.0f;
  this->ev_l2_power_factor_ = static_cast<float>(raw_pf2) / 1000.0f;
  this->ev_l3_power_factor_ = static_cast<float>(raw_pf3) / 1000.0f;
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

void EvboxMaxComponent::update_meter_info_(const std::string &data) {
  if (data.size() < 4) return;

  const uint16_t result = parse_hex_uint(data, 0, 4);
  if (result != ACK) {
    this->meter_status_ = result == 0x0055 ? "NACK" : "ERROR 0x" + hex_word(result);
    ESP_LOGW(TAG, "CB meter info not available: %s", this->meter_status_.c_str());
    return;
  }

  this->meter_status_ = "PRESENT";
  if (data.size() >= 40) {
    const uint8_t model_len = parse_hex_byte(data, 22, 0);
    const size_t safe_model_len = std::min<size_t>(model_len, 16);
    this->meter_model_ = data.substr(24, safe_model_len);
    while (!this->meter_model_.empty() && this->meter_model_.back() == '0') this->meter_model_.pop_back();
    if (this->meter_model_.empty()) this->meter_model_ = "UNKNOWN";
  }
  if (data.size() >= 56) {
    this->meter_serial_ = data.substr(40, 16);
    while (!this->meter_serial_.empty() && this->meter_serial_.front() == '0') this->meter_serial_.erase(0, 1);
    if (this->meter_serial_.empty()) this->meter_serial_ = "UNKNOWN";
  }
  ESP_LOGI(TAG, "CB meter info status=%s model=%s serial=%s", this->meter_status_.c_str(),
           this->meter_model_.c_str(), this->meter_serial_.c_str());
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

void EvboxMaxComponent::note_chargebox_seen_(uint8_t address) {
  if (address == 0 || address > 20) return;
  const bool first_seen_after_boot = this->chargebox_address_ == 0;
  this->chargebox_address_ = address;
  if (!this->startup_config_received_ && this->startup_step_ == 0 &&
      (this->last_startup_sync_request_ms_ == 0 || millis() - this->last_startup_sync_request_ms_ >= 10000UL)) {
    ESP_LOGI(TAG, "%s at 0x%02X; running startup sync",
             first_seen_after_boot ? "ChargeBox already active" : "ChargeBox live frame seen", address);
    this->last_startup_sync_request_ms_ = millis();
    this->schedule_startup_step_(1, 100);
  }
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
      this->schedule_startup_step_(2, 500);
      break;
    case 2:
      ESP_LOGI(TAG, "Startup step 2: LED enable");
      this->send_led_enable_();
      this->schedule_startup_step_(3, 500);
      break;
    case 3:
      ESP_LOGI(TAG, "Startup step 3: meter update interval");
      this->send_meter_update_interval_();
      this->schedule_startup_step_(4, 500);
      break;
    case 4:
      ESP_LOGI(TAG, "Startup step 4: meter info request");
      this->send_packet_(this->chargebox_address_, 0x13, "");
      this->schedule_startup_step_(5, 500);
      break;
    case 5:
      ESP_LOGI(TAG, "Startup step 5: status update request");
      this->send_status_update_request_();
      this->schedule_startup_step_(6, 1500);
      break;
    case 6:
      ESP_LOGI(TAG, "Startup step 6: CB config request");
      this->transition_(READ_CONFIG);
      this->send_config_request_();
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
  if (this->chargebox_address_ != 0) {
    this->send_packet_(this->chargebox_address_, 0x1E, "");
  }
}

void EvboxMaxComponent::send_connection_state_() {
  if (this->chargebox_address_ == 0) return;
  this->send_packet_(ADDR_BROADCAST, 0x1B, "0000038400");
  this->send_packet_(this->chargebox_address_, 0x1B, "0000038400");
}

void EvboxMaxComponent::send_led_enable_() {
  if (this->chargebox_address_ == 0) return;
  this->send_packet_(this->chargebox_address_, 0x1C, "01");
}

void EvboxMaxComponent::send_status_update_request_() {
  if (this->chargebox_address_ == 0) return;
  this->send_packet_(this->chargebox_address_, 0x18, "02");
}

void EvboxMaxComponent::send_meter_update_interval_() {
  if (this->chargebox_address_ == 0) return;
  this->send_packet_(this->chargebox_address_, 0x65, "000F");
}

void EvboxMaxComponent::send_config_request_() {
  if (this->chargebox_address_ == 0) return;
  ESP_LOGI(TAG, "Requesting CB config cmd33");
  this->send_packet_(this->chargebox_address_, 0x33, "");
}

bool EvboxMaxComponent::send_known_good_meter_config_restore_(const std::string &config) {
  if (this->chargebox_address_ == 0) return false;
  if (config.size() < 60) {
    ESP_LOGW(TAG, "Refusing serial meter config restore: cmd33 len=%u too short",
             static_cast<unsigned>(config.size()));
    return false;
  }

  const std::string led_brightness = config.size() >= 38 ? config.substr(36, 2) : "30";
  const std::string relay = config.size() >= 26 ? config.substr(24, 2) : "03";
  const std::string phase = config.size() >= 34 ? config.substr(32, 2) : "01";
  const std::string tethered = config.size() >= 36 ? config.substr(34, 2) : "00";
  const std::string auto_start = config.size() >= 56 ? config.substr(54, 2) : "01";
  const std::string enable_cmd_2a = config.size() >= 58 ? config.substr(56, 2) : "00";
  const std::string auto_stop = config.size() >= 60 ? config.substr(58, 2) : "01";
  const std::string connection_timeout = config.size() >= 8 ? config.substr(0, 8) : "00000E10";
  const std::string heartbeat_interval = config.size() >= 16 ? config.substr(8, 8) : "00000384";
  const std::string meter_update_interval = config.size() >= 24 ? config.substr(16, 8) : "0000000F";
  const std::string request = std::string("FFFFFFFF") + led_brightness + relay + "0000" + "01" + phase + tethered +
                              "0100000000000000" + auto_start + enable_cmd_2a + connection_timeout +
                              heartbeat_interval + meter_update_interval + auto_stop;

  ESP_LOGW(TAG, "Commissioning mode: setting CB meter config to serial Modbus via cmd34");
  ESP_LOGW(TAG, "Current cmd33=%s", config.c_str());
  ESP_LOGW(TAG, "Patched cmd34=%s", request.c_str());
  this->send_packet_(this->chargebox_address_, 0x34, request);
  return true;
}

bool EvboxMaxComponent::send_remote_start_config_enable_(const std::string &config) {
  if (this->chargebox_address_ == 0) return false;
  if (config.size() < 68) {
    ESP_LOGW(TAG, "Cannot enable CB remote start: cmd33 config is too short len=%u",
             static_cast<unsigned>(config.size()));
    return false;
  }

  // cmd34 is not a raw write-back of cmd33. It starts with a field mask and
  // uses a shifted layout. Only the remote-start flag is written here; meter
  // type restore is deliberately not attempted until the mapping is proven.
  std::string request(94, '0');
  const auto copy_field = [&](size_t dst, size_t src, size_t len) {
    if (dst + len <= request.size() && src + len <= config.size()) {
      request.replace(dst, len, config.substr(src, len));
    }
  };

  request.replace(0, 8, "03A3F781");
  copy_field(8, 36, 2);    // LED brightness
  copy_field(10, 24, 2);   // meter/current mode group
  copy_field(16, 30, 2);   // meter type
  copy_field(18, 32, 2);   // meter configuration
  copy_field(22, 38, 2);   // secondary autostart-related flag
  copy_field(48, 6, 2);    // interval/limit field from known captures
  copy_field(54, 12, 4);   // nominal voltage/current field
  copy_field(62, 20, 4);   // meter update interval
  copy_field(76, 68, 6);   // breaker/current calibration block
  if (config.size() >= 72) copy_field(82, 74, 2);
  if (config.size() >= 74) copy_field(84, 72, 2);

  copy_field(16, 30, 2);         // preserve meter type
  request.replace(38, 2, "01");  // auto start card authentication
  request.replace(74, 2, "01");  // allow remote start in cmd34 layout

  ESP_LOGW(TAG, "Commissioning mode: sending mapped cmd34 to enable remote start only; meter config is preserved");
  this->send_packet_(this->chargebox_address_, 0x34, request);
  return true;
}

bool EvboxMaxComponent::send_remote_start_() {
  if (this->chargebox_address_ == 0) {
    ESP_LOGW(TAG, "Remote start requested before ChargeBox address is known");
    return false;
  }
  const std::string card = this->remote_start_card_.empty() ? std::string("000000AS") : this->remote_start_card_;
  const uint8_t card_len = static_cast<uint8_t>(std::min<size_t>(card.size(), 22));
  const std::string card_data = (card.substr(0, 22) + std::string(22, '0')).substr(0, 22);
  ESP_LOGI(TAG, "Sending remote start cmd31 to CB card_len=%u card=%s", card_len, card_data.c_str());
  this->send_packet_(this->chargebox_address_, 0x31, hex_byte(card_len) + card_data);
  return true;
}

bool EvboxMaxComponent::send_unsolicited_authorize_card_() {
  if (this->chargebox_address_ == 0) {
    ESP_LOGW(TAG, "Unsolicited cmd22 authorize requested before ChargeBox address is known");
    return false;
  }
  const std::string card = this->remote_start_card_.empty() ? std::string("000000AS") : this->remote_start_card_;
  const uint8_t card_len = static_cast<uint8_t>(std::min<size_t>(card.size(), 22));
  const std::string card_data = (card.substr(0, 22) + std::string(22, '0')).substr(0, 22);
  const std::string payload = hex_byte(0x01) + hex_byte(card_len) + card_data + "FFFF";
  ESP_LOGW(TAG, "Sending unsolicited cmd22 authorize card_len=%u card=%s payload=%s",
           static_cast<unsigned>(card_len), card_data.c_str(), payload.c_str());
  this->send_packet_(this->chargebox_address_, 0x22, payload);
  return true;
}

void EvboxMaxComponent::send_remote_stop_() {
  if (this->chargebox_address_ == 0) return;
  ESP_LOGI(TAG, "Sending remote stop cmd32 to CB with zero session payload");
  this->send_packet_(this->chargebox_address_, 0x32, "00000000");
}

void EvboxMaxComponent::log_autostart_config_(const std::string &config) {
  ESP_LOGI(TAG, "CB config raw len=%u", static_cast<unsigned>(config.size()));
  for (size_t offset = 0; offset < config.size(); offset += 64) {
    ESP_LOGI(TAG, "CB config raw[%u..%u]=%s", static_cast<unsigned>(offset),
             static_cast<unsigned>(std::min(offset + 64, config.size())), config.substr(offset, 64).c_str());
  }

  if (config.size() < 58) {
    ESP_LOGW(TAG, "CB config too short to read suspected autostart byte: len=%u",
             static_cast<unsigned>(config.size()));
    return;
  }

  const uint8_t byte_26 = parse_hex_byte(config, 52, 0xFF);
  const uint8_t byte_27 = parse_hex_byte(config, 54, 0xFF);
  const uint8_t byte_28 = parse_hex_byte(config, 56, 0xFF);
  ESP_LOGI(TAG, "CB config suspected autostart window byte26=0x%02X byte27=0x%02X byte28=0x%02X; config unchanged",
           byte_26, byte_27, byte_28);

  if (config.size() >= 68) {
    const uint8_t meter_config = parse_hex_byte(config, 30, 0xFF);
    const uint8_t phase_config = parse_hex_byte(config, 32, 0xFF);
    const uint8_t allow_remote_start = parse_hex_byte(config, 66, 0xFF);
    ESP_LOGI(TAG, "CB config decoded: meter_config(offset30)=0x%02X phase_config(offset32)=0x%02X auto_start(offset54)=0x%02X allow_remote_start(offset66)=0x%02X",
             meter_config, phase_config, byte_27, allow_remote_start);
    if (allow_remote_start == 0x00) {
      ESP_LOGW(TAG, "CB config appears to have remote start disabled; cmd31 is expected to return 0x23 failed");
    }
    if (meter_config != 0x01) {
      ESP_LOGW(TAG, "CB config appears to have non-serial meter config; kWh meter values are expected to stay zero");
    }
  }
}

void EvboxMaxComponent::schedule_start_trigger_(uint32_t delay_ms) {
  if (this->chargebox_address_ == 0 || this->stop_requested_) return;
  this->delayed_start_trigger_pending_ = true;
  this->delayed_start_trigger_due_ms_ = millis() + delay_ms;
  ESP_LOGI(TAG, "Scheduled cmd31 start trigger in %u ms", delay_ms);
}

void EvboxMaxComponent::run_delayed_start_trigger_() {
  if (!this->delayed_start_trigger_pending_) return;
  const uint32_t now = millis();
  if (static_cast<int32_t>(now - this->delayed_start_trigger_due_ms_) < 0) return;

  this->delayed_start_trigger_pending_ = false;
  if (this->stop_requested_ || !this->start_requested_ || this->session_active_) {
    ESP_LOGI(TAG, "Skipping delayed cmd31 start trigger; start flow no longer requested");
    return;
  }

  ESP_LOGI(TAG, "Sending delayed cmd31 start trigger");
  if (this->send_remote_start_()) {
    this->remote_start_pending_ = true;
    this->automatic_remote_start_attempted_ = true;
  }
}

void EvboxMaxComponent::schedule_current_release_(uint32_t delay_ms) {
  if (this->chargebox_address_ == 0 || this->stop_requested_) return;
  this->delayed_current_release_pending_ = true;
  this->delayed_current_release_due_ms_ = millis() + delay_ms;
  ESP_LOGI(TAG, "Scheduled cmd6B current release in %u ms", delay_ms);
}

void EvboxMaxComponent::run_delayed_current_release_() {
  if (!this->delayed_current_release_pending_) return;
  const uint32_t now = millis();
  if (static_cast<int32_t>(now - this->delayed_current_release_due_ms_) < 0) return;

  this->delayed_current_release_pending_ = false;
  if (this->stop_requested_ || !this->charge_flow_requested_()) {
    ESP_LOGI(TAG, "Skipping delayed cmd6B current release; charge flow no longer requested");
    return;
  }

  this->current_start_released_ = true;
  this->desired_current_ = this->controller_.calculate_current(this->inputs_);
  ESP_LOGI(TAG, "Sending delayed cmd6B current release %.1f A", this->desired_current_);
  this->send_current_setpoint_(this->desired_current_);
}

void EvboxMaxComponent::send_periodic_cmd18_() {
  if (this->chargebox_address_ == 0) return;
  ESP_LOGD(TAG, "Sending periodic cmd18 status poll; function still marked TE_TESTEN");
  this->send_status_update_request_();
}

bool EvboxMaxComponent::charge_flow_requested_() const {
  return this->start_requested_ || this->session_active_ || this->current_start_released_ ||
         this->state_ == SESSION_STARTING || this->state_ == CHARGING;
}

bool EvboxMaxComponent::current_setpoint_allowed_() const {
  return this->current_start_released_ || this->session_active_ || this->state_ == SESSION_STARTING ||
         this->state_ == CHARGING;
}

bool EvboxMaxComponent::authorization_allowed_() const {
  return this->controller_.mode() != CHARGING_MODE_DISABLED;
}

bool EvboxMaxComponent::automatic_start_allowed_() const {
  return this->controller_.mode() == CHARGING_MODE_LOAD_BALANCING ||
         this->controller_.mode() == CHARGING_MODE_PV_SURPLUS;
}

bool EvboxMaxComponent::current_request_allows_start_(uint8_t code) const {
  if (!this->authorization_allowed_()) return false;
  if (code == 0x07) {
    return this->start_requested_ || this->automatic_start_allowed_();
  }
  if (code == 0xA7) {
    return this->automatic_start_allowed_();
  }
  return false;
}

bool EvboxMaxComponent::is_supported_current_request_(uint8_t code) const {
  if (this->chargebox_hardware_generation_ != 0 && this->chargebox_hardware_generation_ != 3) return false;
  switch (code) {
    case 0x07:
    case 0xA7:
    case 0x37:
    case 0x01:
    case 0x81:
      return true;
    default:
      return false;
  }
}

const char *EvboxMaxComponent::protocol_profile_name_() const {
  switch (this->chargebox_hardware_generation_) {
    case 2: return "MAX_PROFILE_G2";
    case 3: return "MAX_PROFILE_G3";
    case 4: return "MAX_PROFILE_G4";
    case 0: return "MAX_PROFILE_UNKNOWN";
    default: return "MAX_PROFILE_UNSUPPORTED";
  }
}

const char *EvboxMaxComponent::cb_status_name_(uint8_t code) const {
  switch (code) {
    case 0x02: return "AVAILABLE";
    case 0x0A: return "ERROR_OR_FAULT";
    case 0x17: return "IN_USE_OR_PLUGGED";
    case 0x47: return "PREPARING";
    case 0x48: return "CHARGING";
    case 0x4A: return "READY";
    case 0x4B: return "FINISHED_PLUGGED_IN";
    default: return "UNKNOWN";
  }
}

const char *EvboxMaxComponent::current_request_name_(uint8_t code) const {
  switch (code) {
    case 0x00: return "WAITING_FOR_CMD26";
    case 0x01: return "CHARGING_ACTIVE_G2";
    case 0x07: return "AUTHORIZED_READY_G2";
    case 0x20: return "OBSERVED_PRESTART_20";
    case 0x28: return "OBSERVED_PRESTART_28";
    case 0x2F: return "OBSERVED_PRESTART_2F";
    case 0x30: return "CONNECTED_WAITING";
    case 0x37: return "AUTHORIZED_WAIT_LOCK";
    case 0x80: return "UNPLUGGED";
    case 0x81: return "CHARGING";
    case 0xA0: return "AVAILABLE";
    case 0xA7: return "READY";
    case 0xC1: return "FINISHED";
    case 0xE7: return "FAILED";
    default: return "UNKNOWN";
  }
}

void EvboxMaxComponent::send_current_setpoint_(float amps) {
  if (this->chargebox_address_ == 0) return;
  this->commanded_current_ = amps;
  this->active_current_ = amps;
  this->current_limit_returned_ = false;
  this->update_ev_measurements_();
  const auto tenths = static_cast<uint16_t>(std::max(0.0f, std::min(32.0f, amps)) * 10.0f);
  const uint8_t phase_mask = this->evbox_active_phase_mask_ != 0 ? this->evbox_active_phase_mask_ : 0x01;
  // Geekabit/Ricardo captures send all three cmd6B phase fields. Some G3
  // firmware stays in CONNECTED_WAITING when inactive phase fields are zero.
  const uint16_t l1_tenths = tenths;
  const uint16_t l2_tenths = tenths;
  const uint16_t l3_tenths = tenths;
  ESP_LOGI(TAG, "Sending commanded current limit %.1f A to CB all cmd6B phase fields; detected mask=0x%02X L1=%.1fA L2=%.1fA L3=%.1fA",
           static_cast<float>(tenths) / 10.0f, phase_mask, static_cast<float>(l1_tenths) / 10.0f,
           static_cast<float>(l2_tenths) / 10.0f, static_cast<float>(l3_tenths) / 10.0f);
  this->send_packet_(this->chargebox_address_, 0x6B,
                     std::string("01") + hex_word(60) + hex_word(l1_tenths) + hex_word(l2_tenths) +
                         hex_word(l3_tenths));
}

uint32_t EvboxMaxComponent::seconds_since_2000_() const {
  return millis() / 1000UL + 840000000UL;
}

void EvboxMaxComponent::watchdog_() {
  if (millis() - this->last_rx_ms_ > this->watchdog_timeout_ms_) {
    this->evbox_online_ = false;
    if (this->charge_flow_requested_() || this->active_current_ > 0.0f) {
      // During an active or requested charge flow, bus silence is a hard fault:
      // the ChargeBox must not keep receiving stale current commands.
      this->transition_(FAULT);
    }
  }
}

void EvboxMaxComponent::publish_() {
  if (this->status_text_sensor_ != nullptr) {
    if (this->state_ == FAULT) {
      this->status_text_sensor_->publish_state("Fault");
    } else if (this->session_active_) {
      this->status_text_sensor_->publish_state("Session active");
    } else if (this->start_requested_) {
      this->status_text_sensor_->publish_state("Start requested");
    } else if (this->state_ == PREPARING) {
      this->status_text_sensor_->publish_state("Cable connected");
    } else if (this->state_ == FINISHING) {
      this->status_text_sensor_->publish_state("Finishing");
    } else {
      this->status_text_sensor_->publish_state("Ready");
    }
  }
  if (this->state_text_sensor_ != nullptr) {
    this->state_text_sensor_->publish_state(this->state_name_());
  }
  if (this->communication_text_sensor_ != nullptr) {
    this->communication_text_sensor_->publish_state(this->communication_name_());
  }
  if (this->protocol_profile_text_sensor_ != nullptr) {
    this->protocol_profile_text_sensor_->publish_state(this->protocol_profile_name_());
  }
  if (this->cb_serial_text_sensor_ != nullptr) {
    this->cb_serial_text_sensor_->publish_state(this->chargebox_serial_);
  }
  if (this->cb_status_detail_text_sensor_ != nullptr) {
    if (this->have_last_cb_status_code_) {
      this->cb_status_detail_text_sensor_->publish_state(std::string(this->cb_status_name_(this->last_cb_status_code_)) +
                                                         " 0x" + hex_byte(this->last_cb_status_code_));
    } else {
      this->cb_status_detail_text_sensor_->publish_state("UNKNOWN");
    }
  }
  if (this->current_request_state_text_sensor_ != nullptr) {
    if (this->have_last_current_request_code_ && this->last_current_request_ms_ != 0) {
      this->current_request_state_text_sensor_->publish_state(
          std::string(this->current_request_name_(this->last_current_request_code_)) + " 0x" +
          hex_byte(this->last_current_request_code_));
    } else {
      this->current_request_state_text_sensor_->publish_state("NO_REQUEST");
    }
  }
  if (this->cable_status_text_sensor_ != nullptr) {
    this->cable_status_text_sensor_->publish_state(this->cable_status_);
  }
  if (this->lock_status_text_sensor_ != nullptr) {
    this->lock_status_text_sensor_->publish_state(this->lock_status_);
  }
  if (this->meter_status_text_sensor_ != nullptr) {
    this->meter_status_text_sensor_->publish_state(this->meter_status_);
  }
  if (this->meter_model_text_sensor_ != nullptr) {
    this->meter_model_text_sensor_->publish_state(this->meter_model_);
  }
  if (this->meter_serial_text_sensor_ != nullptr) {
    this->meter_serial_text_sensor_->publish_state(this->meter_serial_);
  }
  if (this->cb_firmware_sensor_ != nullptr) {
    this->cb_firmware_sensor_->publish_state(this->chargebox_firmware_);
  }
  if (this->cb_hardware_generation_sensor_ != nullptr) {
    this->cb_hardware_generation_sensor_->publish_state(this->chargebox_hardware_generation_);
  }
  if (this->cb_is_charging_sensor_ != nullptr) {
    this->cb_is_charging_sensor_->publish_state(this->cb_is_charging_);
  }
  if (this->cb_led_colour_sensor_ != nullptr) {
    this->cb_led_colour_sensor_->publish_state(this->cb_led_colour_);
  }
  if (this->cb_lock_state_sensor_ != nullptr) {
    this->cb_lock_state_sensor_->publish_state(this->cb_lock_state_);
  }
  if (this->cable_max_current_sensor_ != nullptr) {
    this->cable_max_current_sensor_->publish_state(this->cb_cable_max_current_);
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
  if (this->meter_value_sensor_ != nullptr && !std::isnan(this->meter_value_kwh_)) {
    this->meter_value_sensor_->publish_state(this->meter_value_kwh_);
  }
  if (this->raw_meter_wh_sensor_ != nullptr && !std::isnan(this->raw_meter_wh_)) {
    this->raw_meter_wh_sensor_->publish_state(this->raw_meter_wh_);
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
    case PREPARING: return "PREPARING";
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
