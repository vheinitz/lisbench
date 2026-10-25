// LisDevice: the instrument side of CLSI LIS01-A2 (formerly ASTM E1381) for
// microcontrollers. Part of lisbench (MIT licence).
//
// Written in C++11 without heap or standard library so that it runs on an
// Arduino Uno (2 KB RAM). The same header is tested on the PC against the
// lisbench link implementation.
//
// The device sends one message at a time (records fetched through a
// callback, framed on the fly) and accepts transfers from the LIS (frames
// checked and acknowledged, text handed to a callback). It never blocks:
// feed received bytes with on_byte() and call poll() often.

#ifndef LISBENCH_LIS_DEVICE_H
#define LISBENCH_LIS_DEVICE_H

#include <stddef.h>
#include <stdint.h>

#ifndef LISDEV_RECORD_MAX
#define LISDEV_RECORD_MAX 256  // longest record this device sends, without CR
#endif

namespace lisdev {

namespace ctl {
static const uint8_t stx = 0x02;
static const uint8_t etx = 0x03;
static const uint8_t eot = 0x04;
static const uint8_t enq = 0x05;
static const uint8_t ack = 0x06;
static const uint8_t lf = 0x0A;
static const uint8_t cr = 0x0D;
static const uint8_t nak = 0x15;
static const uint8_t etb = 0x17;
}  // namespace ctl

static const size_t max_text = 240;
static const size_t frame_overhead = 7;

enum Outcome { delivered, rejected, timeout };
enum State { idle, waiting_enq_reply, waiting_frame_reply, busy_wait, contention_wait, receiving };

struct Timing {
    uint32_t reply_timeout_ms;
    uint32_t receive_timeout_ms;
    uint32_t busy_retry_ms;
    uint32_t contention_ms;
    uint8_t max_frame_attempts;
    Timing()
        : reply_timeout_ms(15000), receive_timeout_ms(30000), busy_retry_ms(10000), contention_ms(1000),
          max_frame_attempts(6) {}
};

struct Callbacks {
    void* ctx;
    void (*write)(void* ctx, const uint8_t* data, size_t n);
    size_t (*record_count)(void* ctx);
    // Copies at most cap bytes of record `index` (without CR) into buf and
    // returns its full length, which may be larger than cap.
    size_t (*record_get)(void* ctx, size_t index, char* buf, size_t cap);
    // Text of a received frame; record_end when the frame ends a record. May be null.
    void (*received)(void* ctx, const char* text, size_t n, bool record_end);
    // Result of start(). May be null.
    void (*finished)(void* ctx, Outcome outcome);
};

class Device {
public:
    explicit Device(const Callbacks& cb, const Timing& timing = Timing())
        : cb_(cb), t_(timing), state_(idle), since_(0), record_(0), record_len_(0), offset_(0), chunk_(0),
          fn_(1), attempts_(0), rx_len_(0), rx_terminator_(0), rx_expected_(1), rx_in_frame_(false), pending_(false) {}

    State state() const { return state_; }

    // Starts sending the message. False while a transfer is running, for an
    // empty message, or when a record is longer than LISDEV_RECORD_MAX
    // (nothing is ever cut off).
    bool start(uint32_t now) {
        const size_t count = cb_.record_count(cb_.ctx);
        if (state_ != idle || count == 0) {
            return false;
        }
        for (size_t i = 0; i < count; ++i) {
            if (cb_.record_get(cb_.ctx, i, record_buf_, LISDEV_RECORD_MAX) > LISDEV_RECORD_MAX) {
                return false;
            }
        }
        send_enq(now);
        return true;
    }

    void on_byte(uint8_t b, uint32_t now) {
        switch (state_) {
            case receiving:
                on_receive_byte(b, now);
                return;
            case idle:
            case busy_wait:
            case contention_wait:
                if (b == ctl::enq) {
                    // A waiting retry gives way and starts again after the
                    // LIS has finished its transfer.
                    pending_ = state_ != idle;
                    put(ctl::ack);
                    state_ = receiving;
                    rx_expected_ = 1;
                    rx_in_frame_ = false;
                    since_ = now;
                }
                return;
            case waiting_enq_reply:
                if (b == ctl::ack) {
                    record_ = 0;
                    load_record();
                    attempts_ = 0;
                    send_frame(now);
                } else if (b == ctl::nak) {
                    state_ = busy_wait;
                    since_ = now;
                } else if (b == ctl::enq) {
                    // Contention: the instrument has priority and tries again.
                    state_ = contention_wait;
                    since_ = now;
                }
                return;
            case waiting_frame_reply:
                if (b == ctl::ack || b == ctl::eot) {  // EOT: interrupt request, may be ignored
                    next_frame(now);
                } else if (b == ctl::nak) {
                    if (attempts_ >= t_.max_frame_attempts) {
                        put(ctl::eot);
                        finish(rejected);
                    } else {
                        send_frame(now);
                    }
                }
                return;
        }
    }

    void poll(uint32_t now) {
        const uint32_t elapsed = now - since_;  // wraps correctly after 49 days
        switch (state_) {
            case waiting_enq_reply:
            case waiting_frame_reply:
                if (elapsed >= t_.reply_timeout_ms) {
                    put(ctl::eot);
                    finish(timeout);
                }
                return;
            case busy_wait:
                if (elapsed >= t_.busy_retry_ms) {
                    send_enq(now);
                }
                return;
            case contention_wait:
                if (elapsed >= t_.contention_ms) {
                    send_enq(now);
                }
                return;
            case receiving:
                if (elapsed >= t_.receive_timeout_ms) {
                    end_receive(now);
                }
                return;
            case idle:
                return;
        }
    }

private:
    void put(uint8_t b) { cb_.write(cb_.ctx, &b, 1); }

    void end_receive(uint32_t now) {
        state_ = idle;
        if (pending_) {
            pending_ = false;
            send_enq(now);
        }
    }

    void finish(Outcome o) {
        state_ = idle;
        if (cb_.finished != 0) {
            cb_.finished(cb_.ctx, o);
        }
    }

    void send_enq(uint32_t now) {
        put(ctl::enq);
        state_ = waiting_enq_reply;
        fn_ = 1;
        since_ = now;
    }

    void load_record() {
        // Lengths were checked in start().
        record_len_ = cb_.record_get(cb_.ctx, record_, record_buf_, LISDEV_RECORD_MAX);
        offset_ = 0;
    }

    // Byte i of the record text including its terminating CR.
    uint8_t text_at(size_t i) const {
        return i < record_len_ ? static_cast<uint8_t>(record_buf_[i]) : ctl::cr;
    }

    // Frames the part of the record at offset_; the frame is written in
    // pieces and its checksum computed on the way, so no frame buffer is needed.
    void send_frame(uint32_t now) {
        const size_t total = record_len_ + 1;
        const size_t rest = total - offset_;
        chunk_ = rest > max_text ? max_text : rest;
        const bool last = offset_ + chunk_ == total;
        uint8_t sum = 0;
        uint8_t head[2] = {ctl::stx, static_cast<uint8_t>('0' + fn_)};
        cb_.write(cb_.ctx, head, 2);
        sum = static_cast<uint8_t>(sum + head[1]);
        uint8_t piece[16];
        size_t n = 0;
        for (size_t i = 0; i < chunk_; ++i) {
            const uint8_t c = text_at(offset_ + i);
            sum = static_cast<uint8_t>(sum + c);
            piece[n++] = c;
            if (n == sizeof(piece)) {
                cb_.write(cb_.ctx, piece, n);
                n = 0;
            }
        }
        if (n > 0) {
            cb_.write(cb_.ctx, piece, n);
        }
        const uint8_t terminator = last ? ctl::etx : ctl::etb;
        sum = static_cast<uint8_t>(sum + terminator);
        static const char hex[] = "0123456789ABCDEF";
        uint8_t tail[5] = {terminator, static_cast<uint8_t>(hex[sum >> 4]), static_cast<uint8_t>(hex[sum & 0x0F]),
                           ctl::cr, ctl::lf};
        cb_.write(cb_.ctx, tail, 5);
        ++attempts_;
        state_ = waiting_frame_reply;
        since_ = now;
    }

    void next_frame(uint32_t now) {
        offset_ += chunk_;
        fn_ = static_cast<uint8_t>((fn_ + 1) % 8);
        attempts_ = 0;
        if (offset_ >= record_len_ + 1) {
            ++record_;
            if (record_ >= cb_.record_count(cb_.ctx)) {
                put(ctl::eot);
                finish(delivered);
                return;
            }
            load_record();
        }
        send_frame(now);
    }

    static int hex_value(uint8_t c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return -1;
    }

    void on_receive_byte(uint8_t b, uint32_t now) {
        if (!rx_in_frame_) {
            if (b == ctl::stx) {
                rx_in_frame_ = true;
                rx_len_ = 0;
                rx_terminator_ = 0;
            } else if (b == ctl::eot) {
                end_receive(now);
            }
            return;
        }
        if (rx_len_ >= sizeof(rx_)) {  // overlong frame
            rx_in_frame_ = false;
            put(ctl::nak);
            since_ = now;
            return;
        }
        rx_[rx_len_++] = b;
        if (rx_terminator_ == 0 && (b == ctl::etx || b == ctl::etb)) {
            rx_terminator_ = rx_len_;  // position after the terminator
        }
        if (rx_terminator_ != 0 && rx_len_ == rx_terminator_ + 4) {
            rx_in_frame_ = false;
            since_ = now;
            check_frame();
        }
    }

    // LIS01-A2 does not allow SOH..ACK, LF, DLE..ETB in frame text.
    bool has_restricted() const {
        for (size_t i = 1; i + 1 < rx_terminator_; ++i) {
            const uint8_t c = rx_[i];
            if ((c >= 0x01 && c <= 0x06) || c == 0x0A || (c >= 0x10 && c <= 0x17)) {
                return true;
            }
        }
        return false;
    }

    // rx_ holds FN text terminator C1 C2 CR LF (STX not stored).
    void check_frame() {
        uint8_t sum = 0;
        for (size_t i = 0; i < rx_terminator_; ++i) {
            sum = static_cast<uint8_t>(sum + rx_[i]);
        }
        const int hi = hex_value(rx_[rx_terminator_]);
        const int lo = hex_value(rx_[rx_terminator_ + 1]);
        const uint8_t fn_char = rx_[0];
        const bool well_formed = hi >= 0 && lo >= 0 && rx_[rx_terminator_ + 2] == ctl::cr &&
                                 rx_[rx_terminator_ + 3] == ctl::lf && fn_char >= '0' && fn_char <= '7';
        if (!well_formed || static_cast<uint8_t>((hi << 4) | lo) != sum || has_restricted()) {
            put(ctl::nak);
            return;
        }
        const uint8_t fn = static_cast<uint8_t>(fn_char - '0');
        if (fn == static_cast<uint8_t>((rx_expected_ + 7) % 8)) {
            put(ctl::ack);  // repetition after a lost ACK
            return;
        }
        if (fn != rx_expected_) {
            put(ctl::nak);
            return;
        }
        put(ctl::ack);
        rx_expected_ = static_cast<uint8_t>((rx_expected_ + 1) % 8);
        const bool record_end = rx_[rx_terminator_ - 1] == ctl::etx;
        size_t text_len = rx_terminator_ - 2;  // without FN and terminator
        if (record_end && text_len > 0 && rx_[text_len] == ctl::cr) {
            --text_len;  // record CR
        }
        if (cb_.received != 0) {
            cb_.received(cb_.ctx, reinterpret_cast<const char*>(rx_ + 1), text_len, record_end);
        }
    }

    Callbacks cb_;
    Timing t_;
    State state_;
    uint32_t since_;

    size_t record_;
    size_t record_len_;
    size_t offset_;
    size_t chunk_;
    uint8_t fn_;
    uint8_t attempts_;
    char record_buf_[LISDEV_RECORD_MAX];

    uint8_t rx_[max_text + frame_overhead];
    size_t rx_len_;
    size_t rx_terminator_;
    uint8_t rx_expected_;
    bool rx_in_frame_;
    bool pending_;  // own message waits until the LIS's transfer is over
};

}  // namespace lisdev

#endif
