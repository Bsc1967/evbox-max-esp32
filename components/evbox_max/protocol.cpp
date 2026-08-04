#include "protocol.h"

namespace esphome {
namespace evbox_max {

uint8_t checksum(const uint8_t *data, size_t length) {
  uint8_t sum = 0;
  for (size_t i = 0; i < length; i++) {
    sum = static_cast<uint8_t>(sum + data[i]);
  }
  return static_cast<uint8_t>(0U - sum);
}

std::vector<uint8_t> encode_frame(const Frame &frame) {
  std::vector<uint8_t> out;
  const auto payload_length = static_cast<uint8_t>(frame.payload.size());

  out.reserve(5 + payload_length);
  out.push_back(EVBOX_MAX_SOF);
  out.push_back(frame.address);
  out.push_back(static_cast<uint8_t>(frame.type));
  out.push_back(payload_length);
  out.insert(out.end(), frame.payload.begin(), frame.payload.end());

  const uint8_t cs = checksum(out.data() + 1, out.size() - 1);
  out.push_back(cs);
  return out;
}

bool FrameParser::push(uint8_t byte, Frame *frame) {
  switch (this->state_) {
    case ParseState::WAIT_SOF:
      if (byte == EVBOX_MAX_SOF) {
        this->current_ = Frame{};
        this->checksum_buffer_.clear();
        this->state_ = ParseState::ADDRESS;
      }
      break;
    case ParseState::ADDRESS:
      this->current_.address = byte;
      this->checksum_buffer_.push_back(byte);
      this->state_ = ParseState::TYPE;
      break;
    case ParseState::TYPE:
      this->current_.type = static_cast<FrameType>(byte);
      this->checksum_buffer_.push_back(byte);
      this->state_ = ParseState::LENGTH;
      break;
    case ParseState::LENGTH:
      this->expected_length_ = byte;
      this->checksum_buffer_.push_back(byte);
      this->current_.payload.clear();
      if (this->expected_length_ > EVBOX_MAX_MAX_PAYLOAD) {
        this->reset();
      } else if (this->expected_length_ == 0) {
        this->state_ = ParseState::CHECKSUM;
      } else {
        this->state_ = ParseState::PAYLOAD;
      }
      break;
    case ParseState::PAYLOAD:
      this->current_.payload.push_back(byte);
      this->checksum_buffer_.push_back(byte);
      if (this->current_.payload.size() >= this->expected_length_) {
        this->state_ = ParseState::CHECKSUM;
      }
      break;
    case ParseState::CHECKSUM: {
      const uint8_t expected = checksum(this->checksum_buffer_.data(), this->checksum_buffer_.size());
      if (byte == expected) {
        *frame = this->current_;
        this->reset();
        return true;
      }
      this->reset();
      break;
    }
  }

  return false;
}

void FrameParser::reset() {
  this->state_ = ParseState::WAIT_SOF;
  this->expected_length_ = 0;
  this->current_ = Frame{};
  this->checksum_buffer_.clear();
}

}  // namespace evbox_max
}  // namespace esphome
