// Prints a message's channel tones, as poe_p4/main/tx_scheduler.cpp encodes them: ftx_tones FT8|FT4 "<message>".
// Build: see README.md beside this.
#include <stdio.h>
#include <string.h>
#include "ft8/message.h"
#include "ft8/encode.h"
#include "ft8/constants.h"
int main(int argc, char** argv)
{
    if (argc != 3) { fprintf(stderr, "usage: %s FT8|FT4 \"message\"\n", argv[0]); return 2; }
    const int ft4 = strcmp(argv[1], "FT4") == 0;
    ftx_message_t msg;
    if (ftx_message_encode(&msg, NULL, argv[2]) != FTX_MESSAGE_RC_OK) { fprintf(stderr, "encode failed\n"); return 1; }
    uint8_t tones[FT8_NN > FT4_NN ? FT8_NN : FT4_NN];
    const int n = ft4 ? FT4_NN : FT8_NN;
    if (ft4) ft4_encode(msg.payload, tones); else ft8_encode(msg.payload, tones);
    for (int i = 0; i < n; ++i) printf("%d%s", tones[i], i + 1 < n ? " " : "\n");
    return 0;
}
