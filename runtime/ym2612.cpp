// C interface to ymfm's YM2612 (third_party/ymfm, BSD-3).
#include "ym2612.h"

// ymfm's OPN chips compiled here, in this file, rather than on their own:
// generate_mask below drives the FM engine's templates directly, and gcc
// keeps no out-of-line copies of them in a separately compiled ymfm_opn.cpp
#include "ymfm_opn.cpp"

namespace {

// Timers count down in input clocks; the game never reads the timer flags,
// but they are kept so the status register behaves like the real chip.
class Interface : public ymfm::ymfm_interface {
public:
    void ymfm_set_timer(uint32_t tnum, int32_t duration_in_clocks) override {
        timer[tnum] = duration_in_clocks;
    }
    void tick(int32_t clocks) {
        for (uint32_t t = 0; t < 2; t++) {
            if (timer[t] < 0) continue;
            timer[t] -= clocks;
            if (timer[t] <= 0) {
                timer[t] = -1;
                m_engine->engine_timer_expired(t);   // re-arms through ymfm_set_timer
            }
        }
    }
private:
    int32_t timer[2] = {-1, -1};
};

// ymfm's own YM2612 mix (ym2612::generate), with channels left out of the
// sum; the engine is clocked the same either way
class Chip : public ymfm::ym2612 {
public:
    using ymfm::ym2612::ym2612;
    void generate_mask(output_data *out, unsigned mask) {
        m_fm.clock(fm_engine::ALL_CHANNELS);
        out->clear();
        output_data temp;
        int const last_fm_channel = m_dac_enable ? 5 : 6;
        for (int chan = 0; chan < last_fm_channel; chan++) {
            m_fm.output(temp.clear(), 5, 256, 1 << chan);
            if (!(mask >> chan & 1)) temp.clear();
            out->data[0] += dac_discontinuity(temp.data[0]);
            out->data[1] += dac_discontinuity(temp.data[1]);
        }
        if (m_dac_enable) {
            int32_t dacval = dac_discontinuity((mask >> 5 & 1) ? int16_t(m_dac_data << 7) >> 7 : 0);
            out->data[0] += m_fm.regs().ch_output_0(0x102) ? dacval : dac_discontinuity(0);
            out->data[1] += m_fm.regs().ch_output_1(0x102) ? dacval : dac_discontinuity(0);
        }
        out->data[0] = (out->data[0] * 128) * 64 / (6 * 65);
        out->data[1] = (out->data[1] * 128) * 64 / (6 * 65);
    }
};

Interface intf;
Chip *chip;

}  // namespace

extern "C" void ym_init(void) {
    if (!chip) chip = new Chip(intf);
    chip->reset();
}

extern "C" void ym_write(int port, uint8_t data) { chip->write((uint32_t)port & 3, data); }

extern "C" uint8_t ym_read_status(void) { return chip->read_status(); }

extern "C" void ym_generate(int32_t *left, int32_t *right) {
    ymfm::ym2612::output_data out;
    chip->generate(&out);
    *left = out.data[0];
    *right = out.data[1];
    intf.tick(YM_CLOCKS_PER_SAMPLE);
}

extern "C" void ym_generate_mask(int32_t *left, int32_t *right, unsigned mask) {
    ymfm::ym2612::output_data out;
    chip->generate_mask(&out, mask);
    *left = out.data[0];
    *right = out.data[1];
    intf.tick(YM_CLOCKS_PER_SAMPLE);
}
