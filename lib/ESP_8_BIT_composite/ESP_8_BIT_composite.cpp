/* Copyright (c) 2020, Peter Barrett
**
** Permission to use, copy, modify, and/or distribute this software for
** any purpose with or without fee is hereby granted, provided that the
** above copyright notice and this permission notice appear in all copies.
**
** THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
** WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED
** WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR
** BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES
** OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS,
** WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION,
** ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS
** SOFTWARE.
*/

/*
** Extracted from Peter Barrett's ESP_8_BIT project and adapted to Arduino
** library by Roger Cheng
*/

#include "ESP_8_BIT_composite.h"

static const char *TAG = "ESP_8_BIT";

static ESP_8_BIT_composite* _instance_ = NULL;
static int _pal_ = 0;

// Changed from the original library: the NTSC palette is computed at start-up with standard
// colour strength and hue, lines can be the standard 227.5 colour cycles, and the output can
// be standard 525-line interlaced video (each field shows the whole 240-line framebuffer).
static float _saturation_ = 1.0f;
static float _hue_deg_ = 0.0f;
static bool _standard_line_ = true;
static bool _interlace_ = true;
static bool _flip_ = false;     // this line's subcarrier is inverted (every other line at 227.5)
static uint8_t _parity_ = 0;    // toggles every line, across frames too
static int _eq_width, _broad_width;

// A rectangle drawn with its own 256-colour palette (the cover art), switched with the buffers.
static bool _reg_on = false;
static int _reg_x0, _reg_y0, _reg_x1, _reg_y1;
static const uint32_t* _reg_pal;
static volatile bool _regp_ready = false;
static bool _regp_on;
static int _regp_x0, _regp_y0, _regp_x1, _regp_y1;
static const uint32_t* _regp_pal;
static int _blit_y;  // framebuffer row being blitted

// Rows drawn with a palette of their own (the visualizer's glow) instead of RGB332; the region
// above still overrides it inside its rectangle. Switched with the buffers too.
static bool _band_on = false;
static int _band_y0, _band_y1;
static const uint32_t* _band_pal;
static volatile bool _bandp_ready = false;
static bool _bandp_on;
static int _bandp_y0, _bandp_y1;
static const uint32_t* _bandp_pal;

static float _hue_cos = 1, _hue_sin = 0;

//====================================================================================================
//====================================================================================================
//
// low level HW setup of DAC/DMA/APLL/PWM
//

lldesc_t _dma_desc[4] = {0};
intr_handle_t _isr_handle;

extern "C"
void IRAM_ATTR video_isr(volatile void* buf);

// simple isr
void IRAM_ATTR i2s_intr_handler_video(void *arg)
{
    if (I2S0.int_st.out_eof)
        video_isr(((lldesc_t*)I2S0.out_eof_des_addr)->buf); // get the next line of video
    I2S0.int_clr.val = I2S0.int_st.val;                     // reset the interrupt
}

static esp_err_t start_dma(int line_width,int samples_per_cc, int ch = 1)
{
    periph_module_enable(PERIPH_I2S0_MODULE);

    // setup interrupt
    if (esp_intr_alloc(ETS_I2S0_INTR_SOURCE, ESP_INTR_FLAG_LEVEL1 | ESP_INTR_FLAG_IRAM,
        i2s_intr_handler_video, 0, &_isr_handle) != ESP_OK)
        return -1;

    // reset conf
    I2S0.conf.val = 1;
    I2S0.conf.val = 0;
    I2S0.conf.tx_right_first = 1;
    I2S0.conf.tx_mono = (ch == 2 ? 0 : 1);

    I2S0.conf2.lcd_en = 1;
    I2S0.fifo_conf.tx_fifo_mod_force_en = 1;
    I2S0.sample_rate_conf.tx_bits_mod = 16;
    I2S0.conf_chan.tx_chan_mod = (ch == 2) ? 0 : 1;

    // Create TX DMA buffers
    for (int i = 0; i < 2; i++) {
        int n = line_width*2*ch;
        if (n >= 4092) {
            printf("DMA chunk too big:%d\n",n);
            return -1;
        }
        _dma_desc[i].buf = (uint8_t*)heap_caps_calloc(1, n, MALLOC_CAP_DMA);
        if (!_dma_desc[i].buf)
            return -1;

        _dma_desc[i].owner = 1;
        _dma_desc[i].eof = 1;
        _dma_desc[i].length = n;
        _dma_desc[i].size = n;
        _dma_desc[i].empty = (uint32_t)(i == 1 ? _dma_desc : _dma_desc+1);
    }
    I2S0.out_link.addr = (uint32_t)_dma_desc;

    //  Setup up the apll: See ref 3.2.7 Audio PLL
    //  f_xtal = (int)rtc_clk_xtal_freq_get() * 1000000;
    //  f_out = xtal_freq * (4 + sdm2 + sdm1/256 + sdm0/65536); // 250 < f_out < 500
    //  apll_freq = f_out/((o_div + 2) * 2)
    //  operating range of the f_out is 250 MHz ~ 500 MHz
    //  operating range of the apll_freq is 16 ~ 128 MHz.
    //  select sdm0,sdm1,sdm2 to produce nice multiples of colorburst frequencies

    //  see calc_freq() for math: (4+a)*10/((2 + b)*2) mhz
    //  up to 20mhz seems to work ok:
    //  rtc_clk_apll_enable(1,0x00,0x00,0x4,0);   // 20mhz for fancy DDS

    if (!_pal_) {
        switch (samples_per_cc) {
            case 3: rtc_clk_apll_enable(1,0x46,0x97,0x4,2);   break;    // 10.7386363636 3x NTSC (10.7386398315mhz)
            case 4: rtc_clk_apll_enable(1,0x46,0x97,0x4,1);   break;    // 14.3181818182 4x NTSC (14.3181864421mhz)
        }
    } else {
        rtc_clk_apll_enable(1,0x04,0xA4,0x6,1);     // 17.734476mhz ~4x PAL
    }

    I2S0.clkm_conf.clkm_div_num = 1;            // I2S clock divider’s integral value.
    I2S0.clkm_conf.clkm_div_b = 0;              // Fractional clock divider’s numerator value.
    I2S0.clkm_conf.clkm_div_a = 1;              // Fractional clock divider’s denominator value
    I2S0.sample_rate_conf.tx_bck_div_num = 1;
    I2S0.clkm_conf.clka_en = 1;                 // Set this bit to enable clk_apll.
    I2S0.fifo_conf.tx_fifo_mod = (ch == 2) ? 0 : 1; // 32-bit dual or 16-bit single channel data

    dac_output_enable(DAC_CHANNEL_1);           // DAC, video on GPIO25
    dac_i2s_enable();                           // start DAC!

    I2S0.conf.tx_start = 1;                     // start DMA!
    I2S0.int_clr.val = 0xFFFFFFFF;
    I2S0.int_ena.out_eof = 1;
    I2S0.out_link.start = 1;
    return esp_intr_enable(_isr_handle);        // start interruprs!
}

void video_init_hw(int line_width, int samples_per_cc)
{
    // setup apll 4x NTSC or PAL colorburst rate
    start_dma(line_width,samples_per_cc,1);

    // Now ideally we would like to use the decoupled left DAC channel to produce audio
    // But when using the APLL there appears to be some clock domain conflict that causes
    // nasty digitial spikes and dropouts.
}

//====================================================================================================
//====================================================================================================


// ntsc phase representation of a rrrgggbb pixel
// must be in RAM so VBL works
static uint32_t ntsc_RGB332[256];  // filled by make_ntsc_palette()

// PAL yuyv palette, must be in RAM
// Kept in flash; copied to RAM (which the interrupt needs) only when PAL is used.
static const uint32_t pal_yuyv[] = {
    0x18181818,0x1A16191E,0x1E121A26,0x21101A2C,0x1E1D1A1B,0x211B1A20,0x25171B29,0x27151C2E,
    0x25231B1E,0x27201C23,0x2B1D1D2B,0x2E1A1E31,0x2B281D20,0x2E261E26,0x31221F2E,0x34202034,
    0x322D1F23,0x342B2029,0x38282131,0x3A252137,0x38332126,0x3A30212B,0x3E2D2234,0x412A2339,
    0x3E382229,0x4136232E,0x44322436,0x4730253C,0x453E242C,0x483C2531,0x4B382639,0x4E36273F,
    0x171B1D19,0x1A181E1F,0x1D151F27,0x20121F2D,0x1E201F1C,0x201E1F22,0x241A202A,0x26182130,
    0x2425201F,0x27232124,0x2A20222D,0x2D1D2332,0x2A2B2222,0x2D282327,0x3125242F,0x33222435,
    0x31302424,0x332E242A,0x372A2632,0x3A282638,0x37362627,0x3A33262D,0x3D302735,0x402D283B,
    0x3E3B272A,0x4039282F,0x44352938,0x46332A3D,0x4441292D,0x473E2A32,0x4B3B2B3B,0x4D382C40,
    0x171D221B,0x191B2220,0x1D182329,0x1F15242E,0x1D23231E,0x1F202423,0x231D252B,0x261A2631,
    0x23282520,0x26262626,0x2A22272E,0x2C202834,0x2A2E2723,0x2C2B2829,0x30282931,0x33252937,
    0x30332926,0x3331292B,0x362D2A34,0x392B2B39,0x36382A29,0x39362B2E,0x3D322C36,0x3F302D3C,
    0x3D3E2C2B,0x3F3B2D31,0x43382E39,0x46352F3F,0x44432E2E,0x46412F34,0x4A3E303C,0x4D3B3042,
    0x1620271C,0x181E2722,0x1C1A282A,0x1F182930,0x1C26281F,0x1F232924,0x22202A2D,0x251D2B32,
    0x232B2A22,0x25292B27,0x29252C30,0x2B232C35,0x29302C24,0x2C2E2C2A,0x2F2A2D32,0x32282E38,
    0x2F362D27,0x32332E2D,0x36302F35,0x382D303B,0x363B2F2A,0x38393030,0x3C353138,0x3F33323E,
    0x3C40312D,0x3F3E3232,0x423A333B,0x45383340,0x43463330,0x46443435,0x4940353E,0x4C3E3543,
    0x15232B1E,0x18212C23,0x1B1D2D2B,0x1E1B2E31,0x1C282D20,0x1E262E26,0x22222F2E,0x24202F34,
    0x222E2F23,0x242B3029,0x28283131,0x2B253137,0x28333126,0x2B31312B,0x2F2D3234,0x312B3339,
    0x2F383229,0x3136332E,0x35323436,0x3730353C,0x353E342B,0x383B3531,0x3B383639,0x3E35363F,
    0x3B43362E,0x3E413634,0x423D373C,0x443B3842,0x42493831,0x45473837,0x4943393F,0x4B413A45,
    0x1526301F,0x17233125,0x1B20322D,0x1D1D3333,0x1B2B3222,0x1D293327,0x21253430,0x24233435,
    0x21303425,0x242E342A,0x272A3532,0x2A283638,0x28363527,0x2A33362D,0x2E303735,0x302D383B,
    0x2E3B372A,0x30393830,0x34353938,0x37333A3E,0x3440392D,0x373E3A32,0x3B3B3B3B,0x3D383B40,
    0x3B463B30,0x3D433B35,0x41403C3D,0x443D3D43,0x424C3D33,0x44493D38,0x48463E40,0x4A433F46,
    0x14283520,0x16263626,0x1A23372E,0x1D203734,0x1A2E3723,0x1D2B3729,0x20283831,0x23253937,
    0x21333826,0x2331392B,0x272D3A34,0x292B3B39,0x27383A29,0x29363B2E,0x2D333C36,0x30303D3C,
    0x2D3E3C2B,0x303B3D31,0x34383E39,0x36363E3F,0x34433E2E,0x36413E34,0x3A3D3F3C,0x3C3B4042,
    0x3A493F31,0x3D464036,0x4043413F,0x43404244,0x414E4134,0x434C4239,0x47484342,0x4A464447,
    0x132B3A22,0x16293B27,0x19253C30,0x1C233D35,0x19313C25,0x1C2E3D2A,0x202B3E32,0x22283E38,
    0x20363E27,0x22343E2D,0x26303F35,0x292E403B,0x263B3F2A,0x29394030,0x2C364138,0x2F33423E,
    0x2D41412D,0x2F3E4232,0x333B433B,0x35384440,0x33464330,0x35444435,0x3940453E,0x3C3E4543,
    0x394C4533,0x3C494538,0x40464640,0x42434746,0x40514735,0x434F473B,0x464B4843,0x49494949,
    //odd
    0x18181818,0x19161A1E,0x1A121E26,0x1A10212C,0x1A1D1E1B,0x1A1B2120,0x1B172529,0x1C15272E,
    0x1B23251E,0x1C202723,0x1D1D2B2B,0x1E1A2E31,0x1D282B20,0x1E262E26,0x1F22312E,0x20203434,
    0x1F2D3223,0x202B3429,0x21283831,0x21253A37,0x21333826,0x21303A2B,0x222D3E34,0x232A4139,
    0x22383E29,0x2336412E,0x24324436,0x2530473C,0x243E452C,0x253C4831,0x26384B39,0x27364E3F,
    0x1D1B1719,0x1E181A1F,0x1F151D27,0x1F12202D,0x1F201E1C,0x1F1E2022,0x201A242A,0x21182630,
    0x2025241F,0x21232724,0x22202A2D,0x231D2D32,0x222B2A22,0x23282D27,0x2425312F,0x24223335,
    0x24303124,0x242E332A,0x262A3732,0x26283A38,0x26363727,0x26333A2D,0x27303D35,0x282D403B,
    0x273B3E2A,0x2839402F,0x29354438,0x2A33463D,0x2941442D,0x2A3E4732,0x2B3B4B3B,0x2C384D40,
    0x221D171B,0x221B1920,0x23181D29,0x24151F2E,0x23231D1E,0x24201F23,0x251D232B,0x261A2631,
    0x25282320,0x26262626,0x27222A2E,0x28202C34,0x272E2A23,0x282B2C29,0x29283031,0x29253337,
    0x29333026,0x2931332B,0x2A2D3634,0x2B2B3939,0x2A383629,0x2B36392E,0x2C323D36,0x2D303F3C,
    0x2C3E3D2B,0x2D3B3F31,0x2E384339,0x2F35463F,0x2E43442E,0x2F414634,0x303E4A3C,0x303B4D42,
    0x2720161C,0x271E1822,0x281A1C2A,0x29181F30,0x28261C1F,0x29231F24,0x2A20222D,0x2B1D2532,
    0x2A2B2322,0x2B292527,0x2C252930,0x2C232B35,0x2C302924,0x2C2E2C2A,0x2D2A2F32,0x2E283238,
    0x2D362F27,0x2E33322D,0x2F303635,0x302D383B,0x2F3B362A,0x30393830,0x31353C38,0x32333F3E,
    0x31403C2D,0x323E3F32,0x333A423B,0x33384540,0x33464330,0x34444635,0x3540493E,0x353E4C43,
    0x2B23151E,0x2C211823,0x2D1D1B2B,0x2E1B1E31,0x2D281C20,0x2E261E26,0x2F22222E,0x2F202434,
    0x2F2E2223,0x302B2429,0x31282831,0x31252B37,0x31332826,0x31312B2B,0x322D2F34,0x332B3139,
    0x32382F29,0x3336312E,0x34323536,0x3530373C,0x343E352B,0x353B3831,0x36383B39,0x36353E3F,
    0x36433B2E,0x36413E34,0x373D423C,0x383B4442,0x38494231,0x38474537,0x3943493F,0x3A414B45,
    0x3026151F,0x31231725,0x32201B2D,0x331D1D33,0x322B1B22,0x33291D27,0x34252130,0x34232435,
    0x34302125,0x342E242A,0x352A2732,0x36282A38,0x35362827,0x36332A2D,0x37302E35,0x382D303B,
    0x373B2E2A,0x38393030,0x39353438,0x3A33373E,0x3940342D,0x3A3E3732,0x3B3B3B3B,0x3B383D40,
    0x3B463B30,0x3B433D35,0x3C40413D,0x3D3D4443,0x3D4C4233,0x3D494438,0x3E464840,0x3F434A46,
    0x35281420,0x36261626,0x37231A2E,0x37201D34,0x372E1A23,0x372B1D29,0x38282031,0x39252337,
    0x38332126,0x3931232B,0x3A2D2734,0x3B2B2939,0x3A382729,0x3B36292E,0x3C332D36,0x3D30303C,
    0x3C3E2D2B,0x3D3B3031,0x3E383439,0x3E36363F,0x3E43342E,0x3E413634,0x3F3D3A3C,0x403B3C42,
    0x3F493A31,0x40463D36,0x4143403F,0x42404344,0x414E4134,0x424C4339,0x43484742,0x44464A47,
    0x3A2B1322,0x3B291627,0x3C251930,0x3D231C35,0x3C311925,0x3D2E1C2A,0x3E2B2032,0x3E282238,
    0x3E362027,0x3E34222D,0x3F302635,0x402E293B,0x3F3B262A,0x40392930,0x41362C38,0x42332F3E,
    0x41412D2D,0x423E2F32,0x433B333B,0x44383540,0x43463330,0x44443535,0x4540393E,0x453E3C43,
    0x454C3933,0x45493C38,0x46464040,0x47434246,0x47514035,0x474F433B,0x484B4643,0x49494949,
};

//====================================================================================================
//====================================================================================================

uint32_t cpu_ticks()
{
  return xthal_get_ccount();
}

uint32_t us() {
    return cpu_ticks()/240;
}

// Color clock frequency is 315/88 (3.57954545455)
// DAC_MHZ is 315/11 or 8x color clock
// 455/2 color clocks per line, round up to maintain phase
// HSYNCH period is 44/315*455 or 63.55555..us
// Field period is 262*44/315*455 or 16651.5555us

#define IRE(_x)          ((uint32_t)(((_x)+40)*255/3.3/147.5) << 8)   // 3.3V DAC
#define SYNC_LEVEL       IRE(-40)
#define BLANKING_LEVEL   IRE(0)
#define BLACK_LEVEL      IRE(7.5)
#define GRAY_LEVEL       IRE(50)
#define WHITE_LEVEL      IRE(100)


#define P0 (color >> 16)
#define P1 (color >> 8)
#define P2 (color)
#define P3 (color << 8)

// Double-buffering: _bufferA and _bufferB will be swapped back and forth.
static uint8_t** _bufferA;
static uint8_t** _bufferB;

// _lines may point to either _bufferA or _bufferB, depending on which is being displayed
// _backBuffer points to whichever one _lines is not pointing to
static uint8_t** _lines; // Front buffer currently on display
static uint8_t** _backBuffer; // Back buffer waiting to be swapped to front

// TRUE when _backBuffer is ready to go.
static bool _swapReady;

// Notification handle once front and back buffers have been swapped.
static TaskHandle_t _swapCompleteNotify;

// Number of swaps completed
static uint32_t _swap_counter = 0;

volatile int _line_counter = 0;
volatile uint32_t _frame_counter = 0;

int _active_lines;
int _line_count;

int _line_width;
int _samples_per_cc;
const uint32_t* _palette;

float _sample_rate;

int _hsync;
int _hsync_long;
int _hsync_short;
int _burst_start;
int _burst_width;
int _active_start;

int16_t* _burst0 = 0; // pal bursts
int16_t* _burst1 = 0;

static int usec(float us)
{
    uint32_t r = (uint32_t)(us*_sample_rate);
    return ((r + _samples_per_cc)/(_samples_per_cc << 1))*(_samples_per_cc << 1);  // multiple of color clock, word align
}

#define NTSC_COLOR_CLOCKS_PER_SCANLINE 228       // really 227.5 for NTSC but want to avoid half phase fiddling for now
#define NTSC_FREQUENCY (315000000.0/88)
#define NTSC_LINES 262

#define PAL_COLOR_CLOCKS_PER_SCANLINE 284        // really 283.75 ?
#define PAL_FREQUENCY 4433618.75
#define PAL_LINES 312

void pal_init();

// The four DAC samples (0, 90, 180 and 270 degrees of subcarrier, most significant byte first)
// for a colour, with the burst at 180 degrees as NTSC specifies. Chroma is trimmed where it
// would swing below -15 IRE or above 115 IRE.
uint32_t ESP_8_BIT_composite::encodeColor(uint8_t r8, uint8_t g8, uint8_t b8, int level)
{
    const float dacPerIre = 255 / 3.3f / 147.5f;
    const float k = level / (255.0f * 256.0f);
    float r = r8 * k, g = g8 * k, b = b8 * k;
    float y = 0.299f * r + 0.587f * g + 0.114f * b;
    float u = 0.492f * (b - y), v = 0.877f * (r - y);
    float yIre = 7.5f + 92.5f * y;
    float cu = _saturation_ * 92.5f * (u * _hue_cos - v * _hue_sin);
    float cv = _saturation_ * 92.5f * (u * _hue_sin + v * _hue_cos);
    float c = sqrtf(cu * cu + cv * cv);
    float limit = fminf(yIre + 15, 115 - yIre);
    if (c > limit && c > 0) {
        float f = (limit > 0 ? limit : 0) / c;
        cu *= f, cv *= f;
    }
    const float s[4] = {yIre - cv, yIre - cu, yIre + cv, yIre + cu};
    uint32_t e = 0;
    for (int i = 0; i < 4; i++) {
        int d = (int)((s[i] + 40) * dacPerIre);  // truncated, like the IRE() levels
        e = (e << 8) | (uint32_t)(d < 0 ? 0 : (d > 255 ? 255 : d));
    }
    return e;
}

static void make_ntsc_palette()
{
    _hue_cos = cosf(_hue_deg_ * (float)M_PI / 180);
    _hue_sin = sinf(_hue_deg_ * (float)M_PI / 180);
    for (int i = 0; i < 256; i++)
        ntsc_RGB332[i] = ESP_8_BIT_composite::encodeColor((i >> 5) * 255 / 7, ((i >> 2) & 7) * 255 / 7, (i & 3) * 85);
}

void video_init(int samples_per_cc, int ntsc)
{
    _samples_per_cc = samples_per_cc;

    if (ntsc) {
        _sample_rate = 315.0/88 * samples_per_cc;   // DAC rate
        _line_width = NTSC_COLOR_CLOCKS_PER_SCANLINE*samples_per_cc;
        if (_standard_line_ && samples_per_cc == 4)
            _line_width = 910;  // 227.5 colour cycles: alternate lines are inverted below
        make_ntsc_palette();
        _line_count = NTSC_LINES;
        _hsync_long = usec(63.555-4.7);
        _active_start = usec(samples_per_cc == 4 ? 10 : 10.5);
        _hsync = usec(4.7);
        if (_line_width == 910) {
            // 768 samples of picture must end before the line does: start a little earlier.
            _active_start = usec(9);
            if (_interlace_) _line_count = 525;
        } else {
            _interlace_ = false;
        }
        if (samples_per_cc == 4 && _interlace_) {
            // SMPTE 170M: 4.75 us sync, burst from 5.3 us, 2.37 us equalizing pulses. Even
            // sample counts (samples go out in swapped pairs) keep the edges clean, and the burst
            // stays on a 4-sample boundary so it keeps its phase against the picture.
            _hsync = 68;
            _burst_start = 76;
        } else {
            _burst_start = _hsync;
        }
        _eq_width = 34;
        _broad_width = _line_width/2 - _hsync;
        _palette = ntsc_RGB332;
        _pal_ = 0;
    } else {
        _interlace_ = false;
        pal_init();
        uint32_t* table = (uint32_t*)heap_caps_malloc(sizeof(pal_yuyv), MALLOC_CAP_INTERNAL | MALLOC_CAP_32BIT);
        if (!table) abort();
        memcpy(table, pal_yuyv, sizeof(pal_yuyv));
        _palette = table;
        _pal_ = 1;
    }

    _active_lines = 240;
    video_init_hw(_line_width,_samples_per_cc);    // init the hardware
}

//===================================================================================================
//===================================================================================================
// PAL

void pal_init()
{
    int cc_width = 4;
    _sample_rate = PAL_FREQUENCY*cc_width/1000000.0;       // DAC rate in mhz
    _line_width = PAL_COLOR_CLOCKS_PER_SCANLINE*cc_width;
    _line_count = PAL_LINES;
    _hsync_short = usec(2);
    _hsync_long = usec(30);
    _hsync = usec(4.7);
    _burst_start = usec(5.6);
    _burst_width = (int)(10*cc_width + 4) & 0xFFFE;
    _active_start = usec(10.4);

    // make colorburst tables for even and odd lines
    _burst0 = new int16_t[_burst_width];
    _burst1 = new int16_t[_burst_width];
    float phase = 2*M_PI/2;
    for (int i = 0; i < _burst_width; i++)
    {
        _burst0[i] = BLANKING_LEVEL + sin(phase + 3*M_PI/4) * BLANKING_LEVEL/1.5;
        _burst1[i] = BLANKING_LEVEL + sin(phase - 3*M_PI/4) * BLANKING_LEVEL/1.5;
        phase += 2*M_PI/cc_width;
    }
}

void IRAM_ATTR blit_pal(uint8_t* src, uint16_t* dst)
{
    uint32_t c,color;
    bool even = _line_counter & 1;
    const uint32_t* p = even ? _palette : _palette + 256;
    int left = 0;
    int right = 256;
    uint8_t mask = 0xFF;

    // 192 of 288 color clocks wide: roughly correct aspect ratio
    dst += 88;

    // 4 pixels over 3 color clocks, 12 samples
    // do the blitting
    for (int i = left; i < right; i += 4) {
        c = *((uint32_t*)(src+i));
        color = p[c & mask];
        dst[0^1] = P0;
        dst[1^1] = P1;
        dst[2^1] = P2;
        color = p[(c >> 8) & mask];
        dst[3^1] = P3;
        dst[4^1] = P0;
        dst[5^1] = P1;
        color = p[(c >> 16) & mask];
        dst[6^1] = P2;
        dst[7^1] = P3;
        dst[8^1] = P0;
        color = p[(c >> 24) & mask];
        dst[9^1] = P1;
        dst[10^1] = P2;
        dst[11^1] = P3;
        dst += 12;
    }
}

void IRAM_ATTR burst_pal(uint16_t* line)
{
    line += _burst_start;
    int16_t* b = (_line_counter & 1) ? _burst0 : _burst1;
    for (int i = 0; i < _burst_width; i += 2) {
        line[i^1] = b[i];
        line[(i+1)^1] = b[i+1];
    }
}

//===================================================================================================
//===================================================================================================
// ntsc tables
// AA AA                // 2 pixels, 1 color clock - atari
// AA AB BB             // 3 pixels, 2 color clocks - nes
// AAA ABB BBC CCC      // 4 pixels, 3 color clocks - sms

// cc == 3 gives 684 samples per line, 3 samples per cc, 3 pixels for 2 cc
// cc == 4 gives 912 samples per line, 4 samples per cc, 2 pixels per cc

#ifdef PERF
#define BEGIN_TIMING()  uint32_t t = cpu_ticks()
#define END_TIMING() t = cpu_ticks() - t; _blit_ticks_min = min(_blit_ticks_min,t); _blit_ticks_max = max(_blit_ticks_max,t);
#define ISR_BEGIN() uint32_t t = cpu_ticks()
#define ISR_END() t = cpu_ticks() - t;_isr_us += (t+120)/240;
uint32_t _blit_ticks_min = 0;
uint32_t _blit_ticks_max = 0;
uint32_t _isr_us = 0;
#else
#define BEGIN_TIMING()
#define END_TIMING()
#define ISR_BEGIN()
#define ISR_END()
#endif

// The palette for framebuffer row y outside the region.
static inline __attribute__((always_inline)) const uint32_t* row_palette(int y)
{
    return _band_on && y >= _band_y0 && y < _band_y1 ? _band_pal : _palette;
}

// A line crossing the region: each pixel picks its palette.
static void IRAM_ATTR blit_region(const uint8_t* src, uint16_t* dst)
{
    const uint32_t* p = row_palette(_blit_y);
    const bool flip = _flip_;
    const int x0 = _reg_x0, x1 = _reg_x1;
    uint32_t color, c[4];
    for (int i = 0; i < 256; i += 4) {
        for (int k = 0; k < 4; k++) {
            const int x = i + k;
            uint32_t v = (x >= x0 && x < x1 ? _reg_pal : p)[src[x]];
            c[k] = flip ? (v << 16) | (v >> 16) : v;
        }
        color = c[0];
        dst[0^1] = P0;
        dst[1^1] = P1;
        dst[2^1] = P2;
        color = c[1];
        dst[3^1] = P3;
        dst[4^1] = P0;
        dst[5^1] = P1;
        color = c[2];
        dst[6^1] = P2;
        dst[7^1] = P3;
        dst[8^1] = P0;
        color = c[3];
        dst[9^1] = P1;
        dst[10^1] = P2;
        dst[11^1] = P3;
        dst += 12;
    }
}

// 480i from a 240-line framebuffer: each row covers two picture lines, and field 2 sits half
// a line below field 1. Each output line is resampled where it really is, a quarter row from
// its row's centre: field 1 is 3/4 of its row and 1/4 of the one above, field 2 3/4 of its row
// and 1/4 of the one below. Both fields then carry the same picture with the same softness,
// so the monitor's deinterlacer has nothing to flicker between. The encoded samples are
// linear in the colour, so blending them blends the colours.
static inline __attribute__((always_inline)) uint32_t mix_samples(uint32_t u, uint32_t v, bool flip)
{
    uint32_t m = u - ((u >> 2) & 0x3F3F3F3Fu) + ((v >> 2) & 0x3F3F3F3Fu);  // per byte about (3u + v) / 4
    return flip ? (m << 16) | (m >> 16) : m;
}

static inline __attribute__((always_inline)) uint32_t plain_sample(uint32_t u, bool flip)
{
    return flip ? (u << 16) | (u >> 16) : u;
}

// Twelve samples (four pixels) as six 32-bit stores. Samples go out in swapped pairs and the
// DAC takes each one's high byte, so a word carries sample 2m in bits 31-24 and sample 2m + 1
// in bits 15-8 (the other bytes are ignored).
static inline __attribute__((always_inline)) void store_group(uint32_t* w, uint32_t c0, uint32_t c1, uint32_t c2, uint32_t c3)
{
    w[0] = (c0 & 0xFF000000u) | (c0 >> 8);
    w[1] = (c0 << 16) | ((c1 << 8) & 0xFF00u);
    w[2] = (c1 & 0xFF000000u) | (c1 >> 8);
    w[3] = (c2 << 16) | ((c2 << 8) & 0xFF00u);
    w[4] = (c2 & 0xFF000000u) | (c3 >> 8);
    w[5] = (c3 << 16) | ((c3 << 8) & 0xFF00u);
}

// Pixels [from, to) (multiples of 4) of row a mixed with row b, palettes qa and qb.
template <bool Flip>
static inline __attribute__((always_inline)) void resample_span(const uint8_t* a, const uint8_t* b, const uint32_t* qa,
                                                                const uint32_t* qb, int from, int to, uint16_t* dst)
{
    uint32_t* w = (uint32_t*)(dst + from * 3);
    for (int i = from; i < to; i += 4, w += 6) {
        const uint32_t wa = *(const uint32_t*)(a + i), wb = *(const uint32_t*)(b + i);
        if (wa == wb && qa == qb) {  // the same above and below: nothing to blend
            store_group(w, plain_sample(qa[wa & 0xFF], Flip), plain_sample(qa[(wa >> 8) & 0xFF], Flip),
                        plain_sample(qa[(wa >> 16) & 0xFF], Flip), plain_sample(qa[wa >> 24], Flip));
        } else {
            store_group(w, mix_samples(qa[wa & 0xFF], qb[wb & 0xFF], Flip),
                        mix_samples(qa[(wa >> 8) & 0xFF], qb[(wb >> 8) & 0xFF], Flip),
                        mix_samples(qa[(wa >> 16) & 0xFF], qb[(wb >> 16) & 0xFF], Flip),
                        mix_samples(qa[wa >> 24], qb[wb >> 24], Flip));
        }
    }
}

// The four pixels at i, where the region's edge runs through: each picks its palettes.
template <bool Flip>
static inline __attribute__((always_inline)) void resample_edge(const uint8_t* a, const uint8_t* b, const uint32_t* pa,
                                                                const uint32_t* pb, const uint32_t* ra, const uint32_t* rb,
                                                                int x0, int x1, int i, uint16_t* dst)
{
    uint32_t c[4];
    for (int k = 0; k < 4; k++) {
        const bool in = i + k >= x0 && i + k < x1;
        c[k] = mix_samples((in ? ra : pa)[a[i + k]], (in ? rb : pb)[b[i + k]], Flip);
    }
    store_group((uint32_t*)(dst + i * 3), c[0], c[1], c[2], c[3]);
}

// Everything the interrupt runs must be in IRAM (flash is unreadable while it's being
// written), and templates lose IRAM_ATTR: these are inlined into blit_resampled instead.
template <bool Flip>
static inline __attribute__((always_inline)) void resample(int ya, int yb, uint16_t* dst)
{
    const uint8_t* a = _lines[ya];
    const uint8_t* b = _lines[yb];
    const uint32_t* pa = row_palette(ya);
    const uint32_t* pb = row_palette(yb);
    const bool regA = _reg_on && ya >= _reg_y0 && ya < _reg_y1, regB = _reg_on && yb >= _reg_y0 && yb < _reg_y1;
    if (!regA && !regB) {
        resample_span<Flip>(a, b, pa, pb, 0, 256, dst);
        return;
    }
    const uint32_t* ra = regA ? _reg_pal : pa;
    const uint32_t* rb = regB ? _reg_pal : pb;
    const int x0 = _reg_x0 < 0 ? 0 : _reg_x0, x1 = _reg_x1 > 256 ? 256 : _reg_x1;
    const int left = x0 & ~3, in0 = (x0 + 3) & ~3, in1 = x1 & ~3, right = (x1 + 3) & ~3;
    resample_span<Flip>(a, b, pa, pb, 0, left, dst);
    if (left < in0) resample_edge<Flip>(a, b, pa, pb, ra, rb, x0, x1, left, dst);
    resample_span<Flip>(a, b, ra, rb, in0, in1, dst);
    if (in1 < right) resample_edge<Flip>(a, b, pa, pb, ra, rb, x0, x1, in1, dst);
    resample_span<Flip>(a, b, pa, pb, right, 256, dst);
}

// Line ya of a field, resampled toward row yb (the row above in field 1, below in field 2).
static void IRAM_ATTR blit_resampled(int ya, int yb, uint16_t* dst)
{
    if (_flip_) resample<true>(ya, yb, dst);
    else resample<false>(ya, yb, dst);
}

// draw a line of game in NTSC
void IRAM_ATTR blit(uint8_t* src, uint16_t* dst)
{
    const uint32_t* p = row_palette(_blit_y);
    uint32_t color,c;
    uint32_t mask = 0xFF;
    int i;

    BEGIN_TIMING();
    if (_pal_) {
        blit_pal(src,dst);
        END_TIMING();
        return;
    }
    if (_reg_on && _blit_y >= _reg_y0 && _blit_y < _reg_y1) {
        blit_region(src,dst);
        END_TIMING();
        return;
    }

    // AAA ABB BBC CCC
    // 4 pixels, 3 color clocks, 4 samples per cc
    // each pixel gets 3 samples, 192 color clocks wide
    const bool flip = _flip_;
    for (i = 0; i < 256; i += 4) {
        c = *((uint32_t*)(src+i));
        color = p[c & mask]; if (flip) color = (color << 16) | (color >> 16);
        dst[0^1] = P0;
        dst[1^1] = P1;
        dst[2^1] = P2;
        color = p[(c >> 8) & mask]; if (flip) color = (color << 16) | (color >> 16);
        dst[3^1] = P3;
        dst[4^1] = P0;
        dst[5^1] = P1;
        color = p[(c >> 16) & mask]; if (flip) color = (color << 16) | (color >> 16);
        dst[6^1] = P2;
        dst[7^1] = P3;
        dst[8^1] = P0;
        color = p[(c >> 24) & mask]; if (flip) color = (color << 16) | (color >> 16);
        dst[9^1] = P1;
        dst[10^1] = P2;
        dst[11^1] = P3;
        dst += 12;
    }

    END_TIMING();
}

void IRAM_ATTR burst(uint16_t* line)
{
    if (_pal_) {
        burst_pal(line);
        return;
    }

    int i,phase;
    switch (_samples_per_cc) {
        case 4:
            // 4 samples per color clock
            for (i = _burst_start; i < _burst_start + (4*10); i += 4) {
                line[i+1] = BLANKING_LEVEL;
                line[i+0] = BLANKING_LEVEL + (_flip_ ? -BLANKING_LEVEL/2 : BLANKING_LEVEL/2);
                line[i+3] = BLANKING_LEVEL;
                line[i+2] = BLANKING_LEVEL + (_flip_ ? BLANKING_LEVEL/2 : -BLANKING_LEVEL/2);
            }
            break;
        case 3:
            // 3 samples per color clock
            phase = 0.866025*BLANKING_LEVEL/2;
            for (i = _hsync; i < _hsync + (3*10); i += 6) {
                line[i+1] = BLANKING_LEVEL;
                line[i+0] = BLANKING_LEVEL + phase;
                line[i+3] = BLANKING_LEVEL - phase;
                line[i+2] = BLANKING_LEVEL;
                line[i+5] = BLANKING_LEVEL + phase;
                line[i+4] = BLANKING_LEVEL - phase;
            }
            break;
    }
}

void IRAM_ATTR sync(uint16_t* line, int syncwidth)
{
    for (int i = 0; i < syncwidth; i++)
        line[i] = SYNC_LEVEL;
}

void IRAM_ATTR blanking(uint16_t* line, bool vbl)
{
    int syncwidth = vbl ? _hsync_long : _hsync;
    sync(line,syncwidth);
    for (int i = syncwidth; i < _line_width; i++)
        line[i] = BLANKING_LEVEL;
    if (!vbl)
        burst(line);    // no burst during vbl
}

// Fancy pal non-interlace
// http://martin.hinner.info/vga/pal.html
void IRAM_ATTR pal_sync2(uint16_t* line, int width, int swidth)
{
    swidth = swidth ? _hsync_long : _hsync_short;
    int i;
    for (i = 0; i < swidth; i++)
        line[i] = SYNC_LEVEL;
    for (; i < width; i++)
        line[i] = BLANKING_LEVEL;
}

uint8_t DRAM_ATTR _sync_type[8] = {0,0,0,3,3,2,0,0};
void IRAM_ATTR pal_sync(uint16_t* line, int i)
{
    uint8_t t = _sync_type[i-304];
    pal_sync2(line,_line_width/2, t & 2);
    pal_sync2(line+_line_width/2,_line_width/2, t & 1);
}

// Interlaced NTSC vertical blanking, one half line at a time.
enum { HALF_BLANK, HALF_EQ, HALF_BROAD, HALF_HSYNC };
static void IRAM_ATTR half_line(uint16_t* line, int width, int type)
{
    int s = type == HALF_EQ ? _eq_width : type == HALF_BROAD ? _broad_width : type == HALF_HSYNC ? _hsync : 0;
    int i = 0;
    for (; i < s; i++) line[i] = SYNC_LEVEL;
    for (; i < width; i++) line[i] = BLANKING_LEVEL;
}

// Standard line numbers (1-525): field 1 has its picture on 22-261, field 2 on 285-524.
static void IRAM_ATTR ntsc_vbi(uint16_t* line, int n)
{
    int a = HALF_HSYNC, b = HALF_BLANK;
    if (n <= 3 || (n >= 7 && n <= 9) || n == 264 || n == 265 || n == 270 || n == 271) a = b = HALF_EQ;
    else if (n <= 6 || n == 267 || n == 268) a = b = HALF_BROAD;
    else if (n == 263) b = HALF_EQ;
    else if (n == 266) a = HALF_EQ, b = HALF_BROAD;
    else if (n == 269) a = HALF_BROAD, b = HALF_EQ;
    else if (n == 272) a = HALF_EQ;
    int half = _line_width/2;
    half_line(line, half, a);
    half_line(line + half, _line_width - half, b);
    if (a == HALF_HSYNC)
        burst(line);
}

static void IRAM_ATTR field_done()
{
    _frame_counter++;

    // Is the back buffer ready to go?
    if (_swapReady) {
      // Swap front and back buffers
      if (_lines == _bufferA) {
        _lines = _bufferB;
        _backBuffer = _bufferA;
      } else {
        _lines = _bufferA;
        _backBuffer = _bufferB;
      }
      _swapReady = false;
      _swap_counter++;
      if (_regp_ready) {
        _reg_on = _regp_on;
        _reg_x0 = _regp_x0, _reg_y0 = _regp_y0, _reg_x1 = _regp_x1, _reg_y1 = _regp_y1;
        _reg_pal = _regp_pal;
        _regp_ready = false;
      }
      if (_bandp_ready) {
        _band_on = _bandp_on;
        _band_y0 = _bandp_y0, _band_y1 = _bandp_y1;
        _band_pal = _bandp_pal;
        _bandp_ready = false;
      }

      // Signal video_sync() swap has completed
      vTaskNotifyGiveFromISR(
          _swapCompleteNotify,
          NULL);
    }
}

// Wait for front and back buffers to swap before starting drawing
void video_sync()
{
  if (!_lines)
    return;
  ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
}

// Workhorse ISR handles audio and video updates
extern "C"
void IRAM_ATTR video_isr(volatile void* vbuf)
{
    if (!_lines)
        return;

    ISR_BEGIN();

    int i = _line_counter++;
    uint16_t* buf = (uint16_t*)vbuf;
    _parity_ ^= 1;
    _flip_ = !_pal_ && _line_width == 910 && _parity_;
    if (_interlace_) {
        int n = i + 1;
        if (n >= 22 && n < 22 + _active_lines) {
            sync(buf,_hsync);
            burst(buf);
            const int ya = n - 22;
            blit_resampled(ya, ya > 0 ? ya - 1 : ya, buf + _active_start);
        } else if (n >= 285 && n < 285 + _active_lines) {
            sync(buf,_hsync);
            burst(buf);
            const int ya = n - 285;
            blit_resampled(ya, ya + 1 < _active_lines ? ya + 1 : ya, buf + _active_start);
        } else {
            ntsc_vbi(buf, n);
        }
        if (n == 263)
            field_done();
        if (_line_counter == _line_count) {
            _line_counter = 0;
            field_done();
        }
        ISR_END();
        return;
    }
    if (_pal_) {
        // pal
        if (i < 32) {
            blanking(buf,false);                // pre render/black 0-32
        } else if (i < _active_lines + 32) {    // active video 32-272
            sync(buf,_hsync);
            burst(buf);
            blit(_lines[i-32],buf + _active_start);
        } else if (i < 304) {                   // post render/black 272-304
            blanking(buf,false);
        } else {
            pal_sync(buf,i);                    // 8 lines of sync 304-312
        }
    } else {
        // ntsc
        if (i < _active_lines) {                // active video
            sync(buf,_hsync);
            burst(buf);
            _blit_y = i;
            blit(_lines[i],buf + _active_start);

        } else if (i < (_active_lines + 5)) {   // post render/black
            blanking(buf,false);

        } else if (i < (_active_lines + 8)) {   // vsync
            blanking(buf,true);

        } else {                                // pre render/black
            blanking(buf,false);
        }
    }

    if (_line_counter == _line_count) {
        _line_counter = 0;                      // frame is done
        field_done();
    }

    ISR_END();
}

//===================================================================================================
//===================================================================================================
// Wrapper class

/*
 * @brief Constructor for ESP_8_BIT composite video wrapper class
 * @param ntsc True (or nonzero) for NTSC mode, False (or zero) for PAL mode
 */
void ESP_8_BIT_composite::setColor(float saturation, float hueDegrees)
{
  _saturation_ = saturation;
  _hue_deg_ = hueDegrees;
}

void ESP_8_BIT_composite::setStandardLine(bool on)
{
  _standard_line_ = on;
}

void ESP_8_BIT_composite::setInterlace(bool on)
{
  _interlace_ = on;
}

void ESP_8_BIT_composite::setBand(int y0, int y1, const uint32_t* palette)
{
  _bandp_ready = false;
  _bandp_on = palette != nullptr;
  _bandp_y0 = y0, _bandp_y1 = y1;
  _bandp_pal = palette;
  _bandp_ready = true;
}

void ESP_8_BIT_composite::pause()
{
  esp_intr_disable(_isr_handle);
  I2S0.int_ena.out_eof = 0;
  I2S0.out_link.stop = 1;
  I2S0.conf.tx_start = 0;
  I2S0.int_clr.val = 0xFFFFFFFF;
}

void ESP_8_BIT_composite::resume()
{
  // From the first of the two line buffers again, as start_dma() left it.
  I2S0.lc_conf.out_rst = 1;
  I2S0.lc_conf.out_rst = 0;
  I2S0.conf.tx_reset = 1;
  I2S0.conf.tx_reset = 0;
  I2S0.conf.tx_fifo_reset = 1;
  I2S0.conf.tx_fifo_reset = 0;
  I2S0.out_link.addr = (uint32_t)_dma_desc;
  I2S0.conf.tx_start = 1;
  I2S0.int_clr.val = 0xFFFFFFFF;
  I2S0.int_ena.out_eof = 1;
  I2S0.out_link.start = 1;
  esp_intr_enable(_isr_handle);
}

uint8_t** ESP_8_BIT_composite::getDisplayedFrameBufferLines()
{
  return _lines;
}

void ESP_8_BIT_composite::setRegion(int x0, int y0, int x1, int y1, const uint32_t* palette)
{
  _regp_ready = false;
  _regp_on = palette != nullptr;
  _regp_x0 = x0, _regp_y0 = y0, _regp_x1 = x1, _regp_y1 = y1;
  _regp_pal = palette;
  _regp_ready = true;
}

ESP_8_BIT_composite::ESP_8_BIT_composite(int ntsc)
{
  _pal_ = !ntsc;
  if (NULL == _instance_)
  {
    _instance_ = this;
  }
  _started = false;
  _bufferA = NULL;
  _bufferB = NULL;
}

/*
 * @brief Destructor for ESP_8_BIT composite video wrapper class.
 */
ESP_8_BIT_composite::~ESP_8_BIT_composite()
{
  if(_bufferA)
  {
    frameBufferFree(_bufferA);
    _bufferA= NULL;
  }
  if(_bufferB)
  {
    frameBufferFree(_bufferB);
    _bufferB= NULL;
  }
  if (_started)
  {
    // Free resources by mirroring everything allocated in start_dma()
    esp_intr_disable(_isr_handle);
    dac_i2s_disable();
    dac_output_disable(DAC_CHANNEL_1);
    if (!_pal_) {
        rtc_clk_apll_enable(false,0x46,0x97,0x4,1);
    } else {
        rtc_clk_apll_enable(false,0x04,0xA4,0x6,1);
    }
    for (int i = 0; i < 2; i++) {
      heap_caps_free((void*)(_dma_desc[i].buf));
      _dma_desc[i].buf = NULL;
    }
    // Missing: There doesn't seem to be a esp_intr_free() to go with esp_intr_alloc()?
    periph_module_disable(PERIPH_I2S0_MODULE);
    _started = false;
  }
  _lines = NULL;
  _backBuffer = NULL;
  _instance_ = NULL;
}

/*
 * @brief Check to ensure this instance is the first and only allowed instance
 */
void ESP_8_BIT_composite::instance_check()
{
  if (_instance_ != this)
  {
    ESP_LOGE(TAG, "Only one instance of ESP_8_BIT_composite class is allowed.");
    ESP_ERROR_CHECK(ESP_FAIL);
  }
}

/*
 * @brief Video subsystem setup: allocate frame buffer and start engine
 */
void ESP_8_BIT_composite::begin()
{
  instance_check();

  if (_started)
  {
    ESP_LOGE(TAG, "begin() is only allowed to be called once.");
    ESP_ERROR_CHECK(ESP_FAIL);
  }
  _started = true;

  _bufferA = frameBufferAlloc();
  _bufferB = frameBufferAlloc();

  _lines = _bufferA;
  _backBuffer = _bufferB;

  // Initialize double-buffering infrastructure
  _swapReady = false;
  _swapCompleteNotify = xTaskGetCurrentTaskHandle();

  // Start video signal generator
  video_init(4, !_pal_);
}

/////////////////////////////////////////////////////////////////////////////
//
//  Frame buffer memory allocation notes
//
// Architecture can tolerate each _line[i] being a separate chunk of memory
// but allocating in tiny 256 byte chunks is inefficient. (16 bytes of
// overhead per allocation.) On the opposite end, allocating the entire
// buffer at once (256*240 = 60kB) demands a large contiguous chunk of
// memory which might not exist if memory space is fragmented.
//
// Compromise: Allocate frame buffer in 4kB chunks. This means each
// frame buffer is made of 15 4kB chunks instead of a single 60kB chunk.
//
// 14 extra allocations * 16 byte overhead = 224 extra bytes, worth it.

const uint16_t linesPerFrame = 240;
const uint16_t bytesPerLine = 256;
const uint16_t linesPerChunk = 16;
const uint16_t chunkSize = bytesPerLine*linesPerChunk;
const uint16_t chunksPerFrame = 15;

/*
 * @brief Allocate memory for frame buffer
 */
uint8_t** ESP_8_BIT_composite::frameBufferAlloc()
{
  uint8_t** lineArray = NULL;
  uint8_t*  lineChunk = NULL;
  uint8_t*  lineStep  = NULL;

  lineArray = new uint8_t*[linesPerFrame];
  if ( NULL == lineArray )
  {
    ESP_LOGE(TAG, "Frame lines array allocation fail");
    ESP_ERROR_CHECK(ESP_FAIL);
  }

  for (uint8_t chunk = 0; chunk < chunksPerFrame; chunk++)
  {
    lineChunk = new uint8_t[chunkSize];
    if ( NULL == lineChunk )
    {
      ESP_LOGE(TAG, "Frame buffer chunk allocation fail");
      ESP_ERROR_CHECK(ESP_FAIL);
    }
    lineStep = lineChunk;
    for (uint8_t lineIndex = 0; lineIndex < linesPerChunk; lineIndex++)
    {
      lineArray[(chunk*linesPerChunk)+lineIndex] = lineStep;
      lineStep += bytesPerLine;
    }
  }

  return lineArray;
}

/*
 * @brief Free memory allocated by frameBufferAlloc();
 */
void ESP_8_BIT_composite::frameBufferFree(uint8_t** lineArray)
{
  for (uint8_t chunk = 0; chunk < chunksPerFrame; chunk++)
  {
    free(lineArray[chunk*linesPerChunk]);
  }
  free(lineArray);
}

/*
 * @brief Wait for current frame to finish rendering
 */
void ESP_8_BIT_composite::waitForFrame()
{
  instance_check();

  _swapReady = true;

  video_sync();
}

/*
 * @brief Retrieve pointer to frame buffer lines array
 */
uint8_t** ESP_8_BIT_composite::getFrameBufferLines()
{
  instance_check();

  return _backBuffer;
}

/*
 * @brief Number of frames sent to screen
 */
uint32_t ESP_8_BIT_composite::getRenderedFrameCount()
{
  return _frame_counter;
}

/*
 * @brief Number of buffer swaps performed
 */
uint32_t ESP_8_BIT_composite::getBufferSwapCount()
{
  return _swap_counter;
}
