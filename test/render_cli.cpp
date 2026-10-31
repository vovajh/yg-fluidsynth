#include "../src/renderer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc, char** argv) {
    if (argc < 4) { fprintf(stderr, "usage: %s midi sf2 out.wav [bits] [mono]\n", argv[0]); return 2; }
    static fsr::Params p; static fsr::Job job;
    memset(&p, 0, sizeof(p));
    snprintf(p.midi_path, sizeof(p.midi_path), "%s", argv[1]);
    snprintf(p.sf_paths[0], sizeof(p.sf_paths[0]), "%s", argv[2]); p.sf_count = 1;
    snprintf(p.out_path, sizeof(p.out_path), "%s", argv[3]);
    p.sample_rate = 44100; p.gain = 1.0; p.speed = 1.0; p.reverb = p.chorus = p.limiter = true;
    p.bits = argc > 4 ? atoi(argv[4]) : 16; p.mono = argc > 5; p.interp = 4; p.polyphony = 256;
    p.tail_max_s = 4; p.max_seconds = 900; strcpy(p.reverb_engine, "dat"); strcpy(p.bank_select, "gs");
    if (const char* o = getenv("OVR")) { int c, b, pr; if (sscanf(o, "%d %d %d", &c, &b, &pr) == 3) { p.ovr[0] = {c, b, pr}; p.ovr_count = 1; } }
    fsr::initLogging();
    int rc = fsr::render(p, job);
    printf("rc=%d state=%d sec=%.2f wall=%.2f peak=%.3f bytes=%zu err='%s' warn='%s'\n", rc, job.state, job.seconds, job.wall, job.peak, job.bytes, job.error, job.warnings);
    return rc;
}
