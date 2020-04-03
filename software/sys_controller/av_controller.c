//
// Copyright (C) 2016-2020  Markus Hiienkari <mhiienka@niksula.hut.fi>
//
// This file is part of CPS2 Digital AV Interface project.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//

#include <stdio.h>
#include <unistd.h>
#include "system.h"
#include "string.h"
#include "altera_avalon_pio_regs.h"
#include "i2c_opencores.h"
#include "sysconfig.h"
#include "adv7513.h"
#include "si5351.h"
#include "sc_config_regs.h"
#include "video_modes.h"
#include "avconfig.h"

#define PB_MASK             (3<<30)
#define PB0_BIT             (1<<30)
#define PB1_BIT             (1<<31)

#define ADV7513_MAIN_BASE 0x72
#define ADV7513_EDID_BASE 0x7e
#define ADV7513_PKTMEM_BASE 0x70
#define ADV7513_CEC_BASE 0x78

#define SI5351_BASE (0xC0>>1)

typedef struct {
    uint32_t si_clkin_freq;
    si5351_ms_config_t si_mclk_conf;
} clk_config_t;

si5351_dev si_dev = {.i2cm_base = I2C_OPENCORES_0_BASE,
                     .i2c_addr = SI5351_BASE,
                     .xtal_freq = 0LU};

adv7513_dev advtx_dev = {.i2cm_base = I2C_OPENCORES_0_BASE,
                         .main_base = ADV7513_MAIN_BASE,
                         .edid_base = ADV7513_EDID_BASE,
                         .pktmem_base = ADV7513_PKTMEM_BASE,
                         .cec_base = ADV7513_CEC_BASE};

volatile sc_regs *sc = (volatile sc_regs*)SC_CONFIG_0_BASE;

input_mode_t input_mode;

clk_config_t clk_configs[] = { \
    /* CPS1/2 */
    { 16000000UL, {6565, 111, 125, 36, 0, 0, 0, 0, 0} },  \
    /* CPS3 */
    { 42954500UL, {4760, 72584, 85909, 36, 0, 0, 1, 0, 0} } \
};


void update_sc_config(mode_data_t *vm_in, mode_data_t *vm_out, vm_mult_config_t *vm_conf)
{
    misc_config_reg misc_config = {.data=0x00000000};
    sl_config_reg sl_config = {.data=0x00000000};
    sl_config2_reg sl_config2 = {.data=0x00000000};
    hv_config_reg hv_out_config = {.data=0x00000000};
    hv_config2_reg hv_out_config2 = {.data=0x00000000};
    hv_config3_reg hv_out_config3 = {.data=0x00000000};
    xy_config_reg xy_out_config = {.data=0x00000000};
    xy_config2_reg xy_out_config2 = {.data=0x00000000};

    // Set output params
    hv_out_config.h_total = vm_out->timings.h_total;
    hv_out_config.h_active = vm_out->timings.h_active;
    hv_out_config.h_backporch = vm_out->timings.h_backporch;
    hv_out_config2.h_synclen = vm_out->timings.h_synclen;
    hv_out_config2.v_total = vm_out->timings.v_total;
    hv_out_config2.v_active = vm_out->timings.v_active;
    hv_out_config3.v_backporch = vm_out->timings.v_backporch;
    hv_out_config3.v_synclen = vm_out->timings.v_synclen;
    hv_out_config3.v_startline = vm_conf->framesync_line;

    xy_out_config.x_size = vm_conf->x_size;
    xy_out_config.y_size = vm_conf->y_size;
    xy_out_config.x_offset = vm_conf->x_offset;
    xy_out_config2.y_offset = vm_conf->y_offset;
    xy_out_config2.x_start_lb = vm_conf->x_start_lb;
    xy_out_config2.y_start_lb = vm_conf->linebuf_startline;
    xy_out_config2.x_rpt = vm_conf->x_rpt;
    xy_out_config2.y_rpt = vm_conf->y_rpt;
    xy_out_config2.x_skip = vm_conf->x_skip;

    sc->misc_config = misc_config;
    sc->sl_config = sl_config;
    sc->sl_config2 = sl_config2;
    sc->hv_out_config = hv_out_config;
    sc->hv_out_config2 = hv_out_config2;
    sc->hv_out_config3 = hv_out_config3;
    sc->xy_out_config = xy_out_config;
    sc->xy_out_config2 = xy_out_config2;
}

// Initialize hardware
int init_hw()
{
    I2C_init(I2C_OPENCORES_0_BASE,ALT_CPU_FREQ, 400000);

    // init HDMI TX
    adv7513_init(&advtx_dev, 1);

    // Init Si5351C
    si5351_init(&si_dev);

    set_default_avconfig(1);

    return 0;
}

int check_input_mode_change() {
    int mode_changed = 0;

    input_mode_t target_mode = {0};

    target_mode.h_active = sc->fe_status.h_active;
    target_mode.v_active = sc->fe_status.v_active;
    target_mode.h_total = sc->fe_status.h_total;
    target_mode.v_total = sc->fe_status2.v_total;

    if (memcmp(&input_mode, &target_mode, sizeof(input_mode_t)))
        mode_changed = 1;

    memcpy(&input_mode, &target_mode, sizeof(input_mode_t));

    return mode_changed;
}

int main()
{
    int ret, input_mode_change, output_mode_id;
    status_t status;
    mode_data_t vmode_in, vmode_out;
    vm_mult_config_t vm_conf;
    avconfig_t *cur_avconfig;

    uint32_t btn_vec, btn_vec_prev=0;

    ret = init_hw();

    if (ret == 0) {
        printf("### cps2_digiAV INIT OK ###\n\n");
    } else {
        printf("Init error  %d", ret);
        while (1) {}
    }

    // configure audio MCLK
    si5351_set_frac_mult(&si_dev, SI_PLLB, SI_CLK6, SI_CLKIN, &clk_configs[sc->fe_status2.mclk_cfg_id].si_mclk_conf);

    cur_avconfig = get_current_avconfig();

    while(1) {

        btn_vec = ~IORD_ALTERA_AVALON_PIO_DATA(PIO_0_BASE) & PB_MASK;

        if ((btn_vec_prev == 0) && btn_vec) {
            if (btn_vec & PB0_BIT) {
                step_ad_mode(1);
            }
            if (btn_vec & PB1_BIT) {
                step_ad_mode(0);
            }
        }

        status = update_avconfig();

        input_mode_change = check_input_mode_change();

        if ((status == MODE_CHANGE) || input_mode_change) {
            output_mode_id = get_output_mode(&input_mode, cur_avconfig->ad_mode_id, &vm_conf, &vmode_in, &vmode_out);

            if (output_mode_id != -1) {
                printf("Output mode %d (%s) selected\n", output_mode_id, vmode_out.name);

                if (vmode_out.si_pclk_mult != 0)
                    si5351_set_integer_mult(&si_dev, SI_PLLA, SI_CLK1, SI_CLKIN, clk_configs[sc->fe_status2.mclk_cfg_id].si_clkin_freq, vmode_out.si_pclk_mult, vmode_out.si_ms_conf.outdiv);
                else
                    si5351_set_frac_mult(&si_dev, SI_PLLA, SI_CLK1, SI_CLKIN, &vmode_out.si_ms_conf);

                update_sc_config(&vmode_in, &vmode_out, &vm_conf);
                adv7513_set_pixelrep_vic(&advtx_dev, vmode_out.tx_pixelrep, vmode_out.hdmitx_pixr_ifr, vmode_out.vic);
            }
        }

        adv7513_check_hpd_power(&advtx_dev);
        adv7513_update_config(&advtx_dev, &cur_avconfig->adv7513_cfg);

        btn_vec_prev = btn_vec;

        usleep(WAITLOOP_SLEEP_US);
    }

    return 0;
}
