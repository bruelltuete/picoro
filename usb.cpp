#include "usb.h"
#include "hardware/pll.h"
#include "hardware/clocks.h"
#include "hardware/structs/usb.h"
#include "hardware/structs/syscfg.h"


void powerdownusb()
{
    hw_clear_bits(&usb_hw->main_ctrl, USB_MAIN_CTRL_CONTROLLER_EN_BITS);
    hw_clear_bits(&usb_hw->muxing, USB_USB_MUXING_TO_PHY_BITS | USB_USB_MUXING_SOFTCON_BITS);

    // saves about 1mA
    hw_clear_bits(&clocks_hw->wake_en1,  CLOCKS_WAKE_EN1_CLK_SYS_USBCTRL_BITS  | CLOCKS_WAKE_EN1_CLK_USB_USBCTRL_BITS);
    hw_clear_bits(&clocks_hw->sleep_en1, CLOCKS_SLEEP_EN1_CLK_SYS_USBCTRL_BITS | CLOCKS_SLEEP_EN1_CLK_USB_USBCTRL_BITS);
    hw_set_bits(&syscfg_hw->mempowerdown, SYSCFG_MEMPOWERDOWN_USB_BITS);

    // saves about 0.5mA
    clock_stop(clk_usb);
    pll_deinit(pll_usb);

    hw_clear_bits(&clocks_hw->wake_en0,  CLOCKS_WAKE_EN0_CLK_SYS_PLL_USB_BITS);
    hw_clear_bits(&clocks_hw->sleep_en0, CLOCKS_SLEEP_EN0_CLK_SYS_PLL_USB_BITS);
}
