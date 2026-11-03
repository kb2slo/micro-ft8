// qmx_iq_link: the Q9 command sent at each CAT link-up and its read-back check
// (components/radio_control/include/qmx_iq_link.h).

#include <cstdio>

#include "qmx_iq_link.h"

static int g_fails = 0;

static void expect(bool ok, const char* msg)
{
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", msg);
        ++g_fails;
    }
}

int main()
{
    // Not ready: nothing, and a link that dropped starts over.
    QmxIqLink link;
    expect(qmx_iq_link_step(link, false, false, true, -1, 0) == QmxIqLinkAction::None, "nothing before CAT is up");
    expect(qmx_iq_link_step(link, true, false, true, -1, 100) == QmxIqLinkAction::Send, "sends at link-up");
    expect(qmx_iq_link_step(link, true, false, true, -1, 110) == QmxIqLinkAction::None, "and waits");
    expect(qmx_iq_link_step(link, true, false, true, 1, 120) == QmxIqLinkAction::Matched,
           "IQ wanted, IQ read back");
    expect(qmx_iq_link_step(link, true, false, true, 1, 130) == QmxIqLinkAction::None, "done until the link drops");
    expect(qmx_iq_link_step(link, true, false, true, 0, 5000) == QmxIqLinkAction::None, "a later reply is not checked");
    expect(qmx_iq_link_step(link, false, false, true, -1, 6000) == QmxIqLinkAction::None, "link drops");
    expect(qmx_iq_link_step(link, true, false, true, -1, 7000) == QmxIqLinkAction::Send, "and is sent again on the next");

    // Audio wanted (the default, key off): the same check the other way round.
    QmxIqLink audio;
    expect(qmx_iq_link_step(audio, true, false, false, -1, 0) == QmxIqLinkAction::Send, "audio: sends");
    expect(qmx_iq_link_step(audio, true, false, false, 0, 10) == QmxIqLinkAction::Matched, "audio wanted, audio read back");

    // A radio that stays in the other mode.
    QmxIqLink wrong;
    qmx_iq_link_step(wrong, true, false, false, -1, 0);
    expect(qmx_iq_link_step(wrong, true, false, false, 1, 10) == QmxIqLinkAction::Mismatch,
           "audio wanted, radio reads IQ: mismatch");
    QmxIqLink wrong_iq;
    qmx_iq_link_step(wrong_iq, true, false, true, -1, 0);
    expect(qmx_iq_link_step(wrong_iq, true, false, true, 0, 10) == QmxIqLinkAction::Mismatch,
           "IQ wanted, radio reads audio: mismatch");

    // No reply in time: said once, then quiet.
    QmxIqLink silent;
    qmx_iq_link_step(silent, true, false, true, -1, 0);
    expect(qmx_iq_link_step(silent, true, false, true, -1, kQmxIqLinkReplyMs - 1) == QmxIqLinkAction::None,
           "still waiting just before the deadline");
    expect(qmx_iq_link_step(silent, true, false, true, -1, kQmxIqLinkReplyMs) == QmxIqLinkAction::NoReply,
           "no reply at the deadline");
    expect(qmx_iq_link_step(silent, true, false, true, -1, kQmxIqLinkReplyMs + 10) == QmxIqLinkAction::None,
           "and not again");

    // Busy: no command goes out, and a waiting one is asked again afterwards.
    QmxIqLink busy;
    expect(qmx_iq_link_step(busy, true, true, true, -1, 0) == QmxIqLinkAction::None, "not sent while busy");
    expect(qmx_iq_link_step(busy, true, false, true, -1, 10) == QmxIqLinkAction::Send, "sent once idle");
    expect(qmx_iq_link_step(busy, true, true, true, -1, 20) == QmxIqLinkAction::None, "busy while waiting");
    expect(qmx_iq_link_step(busy, true, false, true, -1, 30) == QmxIqLinkAction::Send, "asked again after");

    if (g_fails) {
        fprintf(stderr, "%d failure(s)\n", g_fails);
        return 1;
    }
    printf("qmx_iq_link: ok\n");
    return 0;
}
