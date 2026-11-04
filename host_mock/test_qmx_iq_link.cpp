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
    expect(qmx_iq_link_step(link, false, false, -1, 0) == QmxIqLinkAction::None, "nothing before CAT is up");
    expect(qmx_iq_link_step(link, true, false, -1, 100) == QmxIqLinkAction::Send, "sends at link-up");
    expect(qmx_iq_link_step(link, true, false, -1, 110) == QmxIqLinkAction::None, "and waits");
    expect(qmx_iq_link_step(link, true, false, 1, 120) == QmxIqLinkAction::Matched,
           "radio reads IQ: matched");
    expect(qmx_iq_link_step(link, true, false, 1, 130) == QmxIqLinkAction::None, "done until the link drops");
    expect(qmx_iq_link_step(link, true, false, 0, 5000) == QmxIqLinkAction::None, "a later reply is not checked");
    expect(qmx_iq_link_step(link, false, false, -1, 6000) == QmxIqLinkAction::None, "link drops");
    expect(qmx_iq_link_step(link, true, false, -1, 7000) == QmxIqLinkAction::Send, "and is sent again on the next");

    // A radio that stays in audio mode: said once, then quiet.
    QmxIqLink refused;
    qmx_iq_link_step(refused, true, false, -1, 0);
    expect(qmx_iq_link_step(refused, true, false, 0, 10) == QmxIqLinkAction::Mismatch, "radio reads audio: mismatch");
    expect(qmx_iq_link_step(refused, true, false, 0, 20) == QmxIqLinkAction::None, "said once");

    // No reply in time: said once, then quiet.
    QmxIqLink silent;
    qmx_iq_link_step(silent, true, false, -1, 0);
    expect(qmx_iq_link_step(silent, true, false, -1, kQmxIqLinkReplyMs - 1) == QmxIqLinkAction::None,
           "still waiting just before the deadline");
    expect(qmx_iq_link_step(silent, true, false, -1, kQmxIqLinkReplyMs) == QmxIqLinkAction::NoReply,
           "no reply at the deadline");
    expect(qmx_iq_link_step(silent, true, false, -1, kQmxIqLinkReplyMs + 10) == QmxIqLinkAction::None,
           "and not again");

    // A send that fails waits before trying again, and gives up after a few tries.
    QmxIqLink failing;
    expect(qmx_iq_link_step(failing, true, false, -1, 0) == QmxIqLinkAction::Send, "first try");
    expect(!qmx_iq_link_send_failed(failing, 0), "one failure is not the last");
    expect(qmx_iq_link_step(failing, true, false, -1, kQmxIqLinkRetryMs - 1) == QmxIqLinkAction::None,
           "no retry before the pause is over");
    expect(qmx_iq_link_step(failing, true, false, -1, kQmxIqLinkRetryMs) == QmxIqLinkAction::Send,
           "retried after the pause");
    expect(!qmx_iq_link_send_failed(failing, kQmxIqLinkRetryMs), "two failures are not the last");
    expect(qmx_iq_link_step(failing, true, false, -1, 2 * kQmxIqLinkRetryMs) == QmxIqLinkAction::Send,
           "third try");
    expect(qmx_iq_link_send_failed(failing, 2 * kQmxIqLinkRetryMs), "the third failure gives up");
    expect(qmx_iq_link_step(failing, true, false, -1, 9 * kQmxIqLinkRetryMs) == QmxIqLinkAction::None,
           "and sends no more on this link");
    expect(qmx_iq_link_step(failing, false, false, -1, 10 * kQmxIqLinkRetryMs) == QmxIqLinkAction::None,
           "link drops");
    expect(qmx_iq_link_step(failing, true, false, -1, 10 * kQmxIqLinkRetryMs + 1) == QmxIqLinkAction::Send,
           "a new link starts clean");

    // Busy: no command goes out, and a waiting one is asked again afterwards.
    QmxIqLink busy;
    expect(qmx_iq_link_step(busy, true, true, -1, 0) == QmxIqLinkAction::None, "not sent while busy");
    expect(qmx_iq_link_step(busy, true, false, -1, 10) == QmxIqLinkAction::Send, "sent once idle");
    expect(qmx_iq_link_step(busy, true, true, -1, 20) == QmxIqLinkAction::None, "busy while waiting");
    expect(qmx_iq_link_step(busy, true, false, -1, 30) == QmxIqLinkAction::Send, "asked again after");

    if (g_fails) {
        fprintf(stderr, "%d failure(s)\n", g_fails);
        return 1;
    }
    printf("qmx_iq_link: ok\n");
    return 0;
}
