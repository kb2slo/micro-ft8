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

    // A send that fails waits before trying again, and gives up after a few tries.
    QmxIqLink failing;
    expect(qmx_iq_link_step(failing, true, false, true, -1, 0) == QmxIqLinkAction::Send, "first try");
    expect(!qmx_iq_link_send_failed(failing, 0), "one failure is not the last");
    expect(qmx_iq_link_step(failing, true, false, true, -1, kQmxIqLinkRetryMs - 1) == QmxIqLinkAction::None,
           "no retry before the pause is over");
    expect(qmx_iq_link_step(failing, true, false, true, -1, kQmxIqLinkRetryMs) == QmxIqLinkAction::Send,
           "retried after the pause");
    expect(!qmx_iq_link_send_failed(failing, kQmxIqLinkRetryMs), "two failures are not the last");
    expect(qmx_iq_link_step(failing, true, false, true, -1, 2 * kQmxIqLinkRetryMs) == QmxIqLinkAction::Send,
           "third try");
    expect(qmx_iq_link_send_failed(failing, 2 * kQmxIqLinkRetryMs), "the third failure gives up");
    expect(qmx_iq_link_step(failing, true, false, true, -1, 9 * kQmxIqLinkRetryMs) == QmxIqLinkAction::None,
           "and sends no more on this link");
    expect(qmx_iq_link_step(failing, false, false, true, -1, 10 * kQmxIqLinkRetryMs) == QmxIqLinkAction::None,
           "link drops");
    expect(qmx_iq_link_step(failing, true, false, true, -1, 10 * kQmxIqLinkRetryMs + 1) == QmxIqLinkAction::Send,
           "a new link starts clean");

    // Busy: no command goes out, and a waiting one is asked again afterwards.
    QmxIqLink busy;
    expect(qmx_iq_link_step(busy, true, true, true, -1, 0) == QmxIqLinkAction::None, "not sent while busy");
    expect(qmx_iq_link_step(busy, true, false, true, -1, 10) == QmxIqLinkAction::Send, "sent once idle");
    expect(qmx_iq_link_step(busy, true, true, true, -1, 20) == QmxIqLinkAction::None, "busy while waiting");
    expect(qmx_iq_link_step(busy, true, false, true, -1, 30) == QmxIqLinkAction::Send, "asked again after");

    // The want changes on a live link (qmx_rx_mode.h): the command goes out again, with its own check.
    QmxIqLink sw;
    qmx_iq_link_step(sw, true, false, false, -1, 0);
    expect(qmx_iq_link_step(sw, true, false, false, 0, 10) == QmxIqLinkAction::Matched, "audio at link-up");
    expect(qmx_iq_link_step(sw, true, false, false, 0, 20) == QmxIqLinkAction::None, "quiet while the want holds");
    expect(qmx_iq_link_step(sw, true, true, true, 0, 30) == QmxIqLinkAction::None, "a new want waits out a busy radio");
    expect(qmx_iq_link_step(sw, true, false, true, 0, 40) == QmxIqLinkAction::Send, "and is sent when it is not");
    expect(qmx_iq_link_step(sw, true, false, true, 1, 50) == QmxIqLinkAction::Matched, "audio to IQ, IQ read back");
    expect(qmx_iq_link_step(sw, true, false, false, 1, 60) == QmxIqLinkAction::Send, "and back");
    expect(qmx_iq_link_step(sw, true, false, false, 1, 70) == QmxIqLinkAction::Mismatch,
           "a radio that did not switch is a mismatch");
    expect(qmx_iq_link_step(sw, true, false, false, 1, 80) == QmxIqLinkAction::None, "said once");

    // The want flips while a reply is awaited: the reply to the old command is not judged by the new.
    QmxIqLink flip;
    qmx_iq_link_step(flip, true, false, true, -1, 0);
    expect(qmx_iq_link_step(flip, true, false, false, 1, 10) == QmxIqLinkAction::None,
           "no verdict on a stale reply");
    expect(qmx_iq_link_step(flip, true, false, false, -1, 20) == QmxIqLinkAction::Send, "the new want is sent");
    expect(qmx_iq_link_step(flip, true, false, false, 0, 30) == QmxIqLinkAction::Matched, "and checked");

    // A switch whose write gives up is not retried every pass: it waits for the want to change again.
    QmxIqLink dead;
    qmx_iq_link_step(dead, true, false, false, -1, 0);
    qmx_iq_link_step(dead, true, false, false, 0, 10);
    for (int i = 0; i < kQmxIqLinkSendTries; ++i) {
        const int64_t t = 100 + i * kQmxIqLinkRetryMs;
        expect(qmx_iq_link_step(dead, true, false, true, -1, t) == QmxIqLinkAction::Send, "switch tried");
        qmx_iq_link_send_failed(dead, t);
    }
    expect(qmx_iq_link_step(dead, true, false, true, -1, 100 * kQmxIqLinkRetryMs) == QmxIqLinkAction::None,
           "gave up on this want");
    expect(qmx_iq_link_step(dead, true, false, false, -1, 101 * kQmxIqLinkRetryMs) == QmxIqLinkAction::Send,
           "and tries the next one");

    if (g_fails) {
        fprintf(stderr, "%d failure(s)\n", g_fails);
        return 1;
    }
    printf("qmx_iq_link: ok\n");
    return 0;
}
