// C interface to ymfm's YM2612 (third_party/ymfm, BSD-3).
#include "ym2612.h"

#include "ymfm_opn.h"

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

Interface intf;
ymfm::ym2612 *chip;

}  // namespace

extern "C" void ym_init(void) {
    if (!chip) chip = new ymfm::ym2612(intf);
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
