#include "qmx_iq_channel.h"

double qmx_iq_channel_center_hz(int32_t dial_offset_hz)
{
    return kQmxIqFt8CenterHz + (double)dial_offset_hz;
}

const char* qmx_iq_channel_refusal(int32_t dial_offset_hz)
{
    const double center = qmx_iq_channel_center_hz(dial_offset_hz);
    if (center + kQmxIqChannelHalfHz > kQmxIqWindowEdgeHz || center - kQmxIqChannelHalfHz < -kQmxIqWindowEdgeHz) {
        return "outside the QMX's receive window on this band";
    }
    // The LO's own leakage sits at 0 Hz: a channel over it would decode through that spur.
    if (center - kQmxIqChannelHalfHz < 0.0 && center + kQmxIqChannelHalfHz > 0.0) {
        return "on the QMX's LO on this band";
    }
    return nullptr;
}
