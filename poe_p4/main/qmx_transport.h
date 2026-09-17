#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Installs the USB Host Library and CDC-ACM driver, and polls for the QMX
// (VID:0x0483 PID:0xA34C) on the Host port. Once open, radio_control's own
// cat_cdc_ready()/cat_cdc_send() (declared in radio_transport.h, implemented
// in qmx_transport.cpp) start working -- this call just gets that machinery
// running in the background.
void qmx_transport_start(void);

#ifdef __cplusplus
}
#endif
