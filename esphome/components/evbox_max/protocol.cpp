#include "protocol.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>

namespace esphome {
namespace evbox_max {

uint8_t evbox_checksum(const std::string &payload) {
  uint8_t sum = 0;
  for (char c : payload) sum = static_cast<uint8_t>(sum + static_cast<uint8_t>(c));
  return sum;
}

uint8_t evbox_parity(const std::string &payload) {
  uint8_t value = 0;
  for (char c : payload) value ^= static_cast<uint8_t>(c);
  return value;
}

std::string hex_byte(uint8_t value) {
  char out[3];
  std::snprintf(out, sizeof(out), "%02X", value);
  return out;
}

std::string hex_word(uint16_t value) {
  char out[5];
  std::snprintf(out, sizeof(out), "%04X", value);
  return out;
}

std::string hex_dword(uint32_t value) {
  char out[9];
  std::snprintf(out, sizeof(out), "%08lX", static_cast<unsigned long>(value));
  return out;
}

uint8_t parse_hex_byte(const std::string &text, size_t offset, uint8_t fallback) {
  if (offset + 2 > text.size()) return fallback;
  const std::string slice = text.substr(offset, 2);
  char *end = nullptr;
  errno = 0;
  const auto value = std::strtoul(slice.c_str(), &end, 16);
  return errno != 0 || end == slice.c_str() || *end != '\0' ? fallback : static_cast<uint8_t>(value & 0xFF);
}

uint32_t parse_hex_uint(const std::string &text, size_t offset, size_t length, uint32_t fallback) {
  if (offset + length > text.size()) return fallback;
  const std::string slice = text.substr(offset, length);
  char *end = nullptr;
  errno = 0;
  const auto value = std::strtoul(slice.c_str(), &end, 16);
  return errno != 0 || end == slice.c_str() || *end != '\0' ? fallback : static_cast<uint32_t>(value);
}

FrameType frame_type_for_cmd(uint8_t cmd) {
  switch (cmd) {
    case 0x11: return FrameType::REGISTRATION;
    case 0x13: return FrameType::INFO_RESPONSE;
    case 0x21: return FrameType::HEARTBEAT;
    case 0x22: return FrameType::AUTHENTICATE_CARD;
    case 0x23: return FrameType::METERING_START;
    case 0x24: return FrameType::METERING_END;
    case 0x26: return FrameType::STATE_UPDATE;
    case 0x31: return FrameType::REMOTE_START;
    case 0x32: return FrameType::REMOTE_STOP;
    case 0x33: return FrameType::CONFIG_RESPONSE;
    case 0x34: return FrameType::CONFIG_SET_RESPONSE;
    case 0x65: return FrameType::METER_UPDATE_INTERVAL;
    case 0x66: return FrameType::METER_PUSH;
    case 0x6A: return FrameType::CURRENT_REQUEST;
    case 0x6B: return FrameType::CURRENT_SETPOINT;
    default: return FrameType::UNKNOWN;
  }
}

std::vector<uint8_t> encode_frame(uint8_t src, uint8_t dst, uint8_t cmd, const std::string &data) {
  const std::string payload = hex_byte(dst) + hex_byte(src) + hex_byte(cmd) + data;
  const std::string protected_tail = payload + hex_byte(evbox_checksum(payload)) + hex_byte(evbox_parity(payload));
  std::vector<uint8_t> out;
  out.reserve(protected_tail.size() + 2);
  out.push_back(EVBOX_MAX_SOF);
  out.insert(out.end(), protected_tail.begin(), protected_tail.end());
  out.push_back(EVBOX_MAX_EOF);
  out.push_back(EVBOX_MAX_TRAILER);
  return out;
}

bool FrameParser::push(uint8_t byte, Frame *frame) {
  if (this->pending_trailer_) {
    this->pending_trailer_ = false;
    if (byte == EVBOX_MAX_TRAILER) return false;
  }

  if (byte == EVBOX_MAX_SOF) {
    this->buffer_.clear();
    this->in_frame_ = true;
    return false;
  }
  if (!this->in_frame_) return false;

  this->buffer_.push_back(byte);
  if (this->buffer_.size() > EVBOX_MAX_MAX_FRAME) {
    this->reset();
    return false;
  }
  if (byte != EVBOX_MAX_EOF) return false;

  this->in_frame_ = false;
  this->pending_trailer_ = true;
  return this->parse_buffer_(frame);
}

bool FrameParser::parse_buffer_(Frame *frame) {
  if (this->buffer_.size() < 11 || this->buffer_.back() != EVBOX_MAX_EOF) return false;

  std::string text;
  text.reserve(this->buffer_.size());
  for (size_t i = 0; i + 1 < this->buffer_.size(); i++) {
    const uint8_t byte = this->buffer_[i];
    if (!((byte >= '0' && byte <= '9') || (byte >= 'A' && byte <= 'F') || (byte >= 'a' && byte <= 'f'))) return false;
    text.push_back(static_cast<char>(byte >= 'a' && byte <= 'f' ? byte - 32 : byte));
  }
  if (text.size() < 10) return false;

  const std::string payload = text.substr(0, text.size() - 4);
  const uint8_t got_checksum = parse_hex_byte(text, text.size() - 4);
  const uint8_t got_parity = parse_hex_byte(text, text.size() - 2);
  if (got_checksum != evbox_checksum(payload) || got_parity != evbox_parity(payload)) return false;
  if (payload.size() < 6) return false;

  frame->dst = parse_hex_byte(payload, 0);
  frame->src = parse_hex_byte(payload, 2);
  frame->cmd = parse_hex_byte(payload, 4);
  frame->address = frame->src;
  frame->type = frame_type_for_cmd(frame->cmd);
  frame->data = payload.substr(6);
  return true;
}

void FrameParser::reset() {
  this->in_frame_ = false;
  this->pending_trailer_ = false;
  this->buffer_.clear();
}

}  // namespace evbox_max
}  // namespace esphome
